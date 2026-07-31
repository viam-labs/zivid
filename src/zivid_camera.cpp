#include "zivid_camera.hpp"

#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <Zivid/CameraInfo.h>
#include <Zivid/CameraIntrinsics.h>
#include <Zivid/Experimental/Calibration.h>
#include <Zivid/Experimental/SettingsInfo.h>
#include <Zivid/Image.h>
#include <Zivid/NetworkConfiguration.h>
#include <Zivid/PointCloud.h>
#include <Zivid/PointCloudCompositeTypes.h>
#include <Zivid/Resolution.h>
#include <viam/sdk/common/proto_value.hpp>
#include <viam/sdk/log/logging.hpp>

namespace viam_zivid {

// Static registry definitions.
std::mutex ZividCamera::registry_mutex_;
std::unordered_map<std::string, ZividCamera*> ZividCamera::registry_;

// static
ZividCamera* ZividCamera::find(const std::string& name) {
    std::lock_guard<std::mutex> lk(registry_mutex_);
    auto it = registry_.find(name);
    return (it != registry_.end()) ? it->second : nullptr;
}

namespace {

constexpr const char* kColorSourceName = "color";
constexpr const char* kDepthSourceName = "depth";

// Pack R, G, B into a float for PCD rgb field (PCL convention).
float pack_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t rgb = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    float out{};
    std::memcpy(&out, &rgb, sizeof(float));
    return out;
}

// Encode an RGBA image as a JPEG byte vector using stb_image_write.
std::vector<unsigned char> encode_jpeg(const uint8_t* rgba_data, int width, int height, int quality = 85) {
    std::vector<unsigned char> buf;
    auto callback = [](void* ctx, void* data, int size) {
        auto* vec = static_cast<std::vector<unsigned char>*>(ctx);
        const auto* bytes = static_cast<unsigned char*>(data);
        vec->insert(vec->end(), bytes, bytes + size);
    };
    int rc = stbi_write_jpg_to_func(callback, &buf, width, height, 4, rgba_data, quality);
    if (rc == 0) {
        throw std::runtime_error("JPEG compression failed (stb_image_write)");
    }
    return buf;
}

// Apply sRGB gamma encoding to a linear uint8 value to brighten it for viewers
// that treat stored PCD colors as linear (e.g. CloudCompare without sRGB support).
uint8_t gamma_encode(uint8_t v) {
    const float f = v / 255.f;
    const float g = (f <= 0.0031308f) ? 12.92f * f : 1.055f * std::pow(f, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(g * 255.f + 0.5f);
}

// Build a binary PCD buffer from XYZ + RGBA point cloud data.
// Colors from copyPointsXYZColorsRGBA() are in Zivid's linear-ish space; we apply
// sRGB gamma encoding so they render correctly in viewers (e.g. CloudCompare) that
// treat stored values as linear.
// Only valid (non-NaN) points are included (unorganized layout).
std::vector<unsigned char> encode_pcd(const Zivid::Array2D<Zivid::PointXYZColorRGBA>& points) {
    struct PcdPoint {
        float x, y, z, rgb;
    };

    std::vector<PcdPoint> valid_points;
    valid_points.reserve(points.size());

    for (size_t i = 0; i < points.size(); ++i) {
        const auto& p = points(i);
        if (std::isnan(p.point.x) || std::isnan(p.point.y) || std::isnan(p.point.z)) {
            continue;
        }
        // Zivid XYZ is in millimetres; convert to metres for PCD convention.
        valid_points.push_back({p.point.x * 1e-3f,
                                p.point.y * 1e-3f,
                                p.point.z * 1e-3f,
                                pack_rgb(gamma_encode(p.color.r), gamma_encode(p.color.g), gamma_encode(p.color.b))});
    }

    const size_t n = valid_points.size();

    std::ostringstream header;
    header << "VERSION 0.7\n"
           << "FIELDS x y z rgb\n"
           << "SIZE 4 4 4 4\n"
           << "TYPE F F F F\n"
           << "COUNT 1 1 1 1\n"
           << "WIDTH " << n << "\n"
           << "HEIGHT 1\n"
           << "VIEWPOINT 0 0 0 1 0 0 0\n"
           << "POINTS " << n << "\n"
           << "DATA binary\n";

    const std::string header_str = header.str();
    const size_t binary_size = n * sizeof(PcdPoint);

    std::vector<unsigned char> buf;
    buf.resize(header_str.size() + binary_size);
    std::memcpy(buf.data(), header_str.data(), header_str.size());
    if (n > 0) {
        std::memcpy(buf.data() + header_str.size(), valid_points.data(), binary_size);
    }
    return buf;
}

// Build a Viam depth map from PointCloud Z values (in mm, stored as uint16).
viam::sdk::Camera::depth_map build_depth_map(const Zivid::Array2D<Zivid::PointXYZ>& points, size_t width, size_t height) {
    viam::sdk::Camera::depth_map dm = viam::sdk::Camera::depth_map::from_shape({height, width});
    for (size_t row = 0; row < height; ++row) {
        for (size_t col = 0; col < width; ++col) {
            const auto& p = points(row * width + col);
            if (std::isnan(p.z) || p.z < 0.f || p.z > 65535.f) {
                dm(row, col) = 0;
            } else {
                dm(row, col) = static_cast<uint16_t>(p.z);
            }
        }
    }
    return dm;
}

// Human-readable JSON type name for a config value, so errors can quote the type the operator
// actually wrote rather than an SDK enumerator.
const char* kind_name(viam::sdk::ProtoValue::Kind kind) {
    switch (kind) {
        case viam::sdk::ProtoValue::k_null:
            return "null";
        case viam::sdk::ProtoValue::k_bool:
            return "bool";
        case viam::sdk::ProtoValue::k_double:
            return "number";
        case viam::sdk::ProtoValue::k_string:
            return "string";
        case viam::sdk::ProtoValue::k_list:
            return "list";
        case viam::sdk::ProtoValue::k_struct:
            return "object";
    }
    return "unknown";
}

// JSON type name expected of each type a config value can be read as.
template <typename T>
struct json_type;

template <>
struct json_type<bool> {
    static constexpr const char* name = "bool";
};

template <>
struct json_type<double> {
    static constexpr const char* name = "number";
};

template <>
struct json_type<std::string> {
    static constexpr const char* name = "string";
};

template <>
struct json_type<viam::sdk::ProtoList> {
    static constexpr const char* name = "list";
};

template <>
struct json_type<viam::sdk::ProtoStruct> {
    static constexpr const char* name = "object";
};

// Noun phrase identifying a config attribute by its full dotted path in error messages.
std::string attr_desc(const std::string& path) {
    return "config attribute '" + path + "'";
}

// Reads `value` as T, throwing an error naming `what` (a noun phrase identifying the value,
// e.g. "config attribute 'roi.box.point_o.x'"), the expected type and the type written.
//
// ProtoValue::get<T>() returns nullptr on a type mismatch, whereas get_unchecked<T>()
// reinterprets the stored bytes: reading a string as a double that way is undefined behaviour,
// i.e. a module crash with no diagnostic, which is exactly what a mistyped config must not do.
template <typename T>
const T& typed_value(const viam::sdk::ProtoValue& value, const std::string& what) {
    if (const T* typed = value.get<T>()) {
        return *typed;
    }
    throw std::invalid_argument(what + " must be of type " + json_type<T>::name + ", but is of type " + kind_name(value.kind()) + ".");
}

// A config object together with its dotted path ("" at the top level), so that every accessor
// can report the full attribute path — e.g. "roi.box.point_o.x" — without each call site having
// to restate where it sits in the config tree.
class ConfigObject {
   public:
    ConfigObject(const viam::sdk::ProtoStruct& fields, std::string path) : fields_(&fields), path_(std::move(path)) {}

    // Dotted config path of member `key`.
    std::string child_path(const std::string& key) const {
        return path_.empty() ? key : path_ + "." + key;
    }

    // Member `key` read as T, or nullopt when the key is absent.
    template <typename T>
    std::optional<T> get(const std::string& key) const {
        const auto it = fields_->find(key);
        if (it == fields_->end()) {
            return std::nullopt;
        }
        return typed_value<T>(it->second, attr_desc(child_path(key)));
    }

    // Member `key` as a nested object, or nullopt when the key is absent.
    std::optional<ConfigObject> object(const std::string& key) const {
        const auto it = fields_->find(key);
        if (it == fields_->end()) {
            return std::nullopt;
        }
        return ConfigObject{typed_value<viam::sdk::ProtoStruct>(it->second, attr_desc(child_path(key))), child_path(key)};
    }

    // Member `key` as a nested object, naming this block when the key is absent.
    ConfigObject required_object(const std::string& key) const {
        auto child = object(key);
        if (!child) {
            throw std::invalid_argument((path_.empty() ? std::string{"the module config"} : attr_desc(path_)) +
                                        " is missing required key '" + key + "'.");
        }
        return *child;
    }

    // Member `key` as a list, or nullptr when the key is absent.
    const viam::sdk::ProtoList* list(const std::string& key) const {
        const auto it = fields_->find(key);
        if (it == fields_->end()) {
            return nullptr;
        }
        return &typed_value<viam::sdk::ProtoList>(it->second, attr_desc(child_path(key)));
    }

   private:
    const viam::sdk::ProtoStruct* fields_;
    std::string path_;
};

// Zivid expresses each setting's valid range in that setting's own ValueType; normalize to
// double so one bounds check covers f-numbers, gains and microsecond durations alike.
double range_bound(double value) {
    return value;
}

double range_bound(std::chrono::microseconds value) {
    return static_cast<double>(value.count());
}

// Rejects an out-of-range value, naming the attribute, the value, the accepted range and whose
// range it is (`source`).
void require_in_range(double value, double min, double max, const std::string& path, const std::string& source) {
    if (value >= min && value <= max) {
        return;
    }
    std::ostringstream msg;
    msg << attr_desc(path) << " is " << value << ", outside the range [" << min << ", " << max << "] accepted by " << source
        << ". Use the get_acquisition_ranges DoCommand to discover the ranges this camera accepts.";
    throw std::invalid_argument(msg.str());
}

// Validates a config value against the setting's model-independent range and returns it.
// Zivid's own out-of-range exception does not say which config attribute produced the value, so
// check it here, where the attribute path is still known.
template <typename Setting>
double checked_value(double value, const std::string& path) {
    const auto range = Setting::validRange();
    require_in_range(value, range_bound(range.min()), range_bound(range.max()), path, "the Zivid SDK");
    return value;
}

// checked_value for a duration-valued setting, taking and validating microseconds.
template <typename Setting>
std::chrono::microseconds checked_duration(double value_us, const std::string& path) {
    return std::chrono::microseconds{static_cast<int64_t>(checked_value<Setting>(value_us, path))};
}

// Builds a Zivid acquisition from config. Templated over the acquisition type because the 3D
// (Zivid::Settings::Acquisition) and 2D (Zivid::Settings2D::Acquisition) variants share member
// names but are distinct types with different valid ranges — a difference that has to be
// validated per variant rather than assumed away. `path` is the acquisition's config path,
// e.g. "acquisitions[0]".
template <typename Acquisition>
Acquisition make_acquisition(const AcquisitionConfig& a, const std::string& path) {
    using Aperture = typename Acquisition::Aperture;
    using Brightness = typename Acquisition::Brightness;
    using ExposureTime = typename Acquisition::ExposureTime;
    using Gain = typename Acquisition::Gain;

    Acquisition acq;
    if (a.aperture) {
        acq.set(Aperture{checked_value<Aperture>(*a.aperture, path + ".aperture")});
    }
    if (a.brightness) {
        acq.set(Brightness{checked_value<Brightness>(*a.brightness, path + ".brightness")});
    }
    if (a.exposure_time_us) {
        acq.set(ExposureTime{checked_duration<ExposureTime>(*a.exposure_time_us, path + ".exposure_time_us")});
    }
    if (a.gain) {
        acq.set(Gain{checked_value<Gain>(*a.gain, path + ".gain")});
    }
    return acq;
}

// Validates a config value against the range the *connected* camera reports. The ranges checked
// while building settings are the SDK's model-independent ones — the union over every Zivid
// model — so a value can pass those and still be rejected by this particular camera (e.g. the
// XL250 has a fixed f/3.0 aperture). Checking after connect turns that into a config error
// naming the model, instead of a Zivid exception raised much later at capture time.
template <typename Setting>
void check_against_camera(const std::optional<double>& value,
                          const Zivid::CameraInfo& info,
                          const std::string& path,
                          const std::string& camera) {
    if (!value) {
        return;
    }
    const auto range = Zivid::Experimental::SettingsInfo::validRange<Setting>(info);
    require_in_range(*value, range_bound(range.min()), range_bound(range.max()), path, camera);
}

template <typename Acquisition>
void check_acquisitions_against_camera(const std::vector<AcquisitionConfig>& acquisitions,
                                       const Zivid::CameraInfo& info,
                                       const std::string& key,
                                       const std::string& camera) {
    for (size_t i = 0; i < acquisitions.size(); ++i) {
        const auto& a = acquisitions[i];
        const std::string path = key + "[" + std::to_string(i) + "]";
        check_against_camera<typename Acquisition::Aperture>(a.aperture, info, path + ".aperture", camera);
        check_against_camera<typename Acquisition::Brightness>(a.brightness, info, path + ".brightness", camera);
        check_against_camera<typename Acquisition::ExposureTime>(a.exposure_time_us, info, path + ".exposure_time_us", camera);
        check_against_camera<typename Acquisition::Gain>(a.gain, info, path + ".gain", camera);
    }
}

// Identifies a camera in operator-facing messages, e.g. "Zivid 2+ LR110 (serial 26179B29)".
// Best effort: it only ever decorates another message, so it must not raise one of its own.
std::string describe_camera(const Zivid::Camera& camera) {
    try {
        const auto info = camera.info();
        return info.modelName().value() + " (serial " + info.serialNumber().value() + ")";
    } catch (...) {
        return "the Zivid camera (model and serial unavailable)";
    }
}

// Maps a config string to a Zivid pixel-sampling value. Both the 3D
// (Zivid::Settings::Sampling::Pixel) and 2D (Zivid::Settings2D::Sampling::Pixel)
// enums share the same member names, so this templates over the ValueType.
template <typename ValueType>
ValueType parse_pixel_sampling(const std::string& s, const char* what) {
    if (s == "all")
        return ValueType::all;
    if (s == "by2x2")
        return ValueType::by2x2;
    if (s == "by4x4")
        return ValueType::by4x4;
    if (s == "blueSubsample2x2")
        return ValueType::blueSubsample2x2;
    if (s == "blueSubsample4x4")
        return ValueType::blueSubsample4x4;
    if (s == "redSubsample2x2")
        return ValueType::redSubsample2x2;
    if (s == "redSubsample4x4")
        return ValueType::redSubsample4x4;
    throw std::invalid_argument(std::string(what) + " '" + s +
                                "' is invalid. Valid values: all, by2x2, by4x4, blueSubsample2x2, "
                                "blueSubsample4x4, redSubsample2x2, redSubsample4x4.");
}

// Build the 2D color settings (acquisitions + color pixel sampling). Shared by
// make_settings (for capture) and stored on the camera so get_properties() can
// compute intrinsics that MATCH the returned 2D color image resolution.
Zivid::Settings2D make_settings_2d(const Config& config) {
    Zivid::Settings2D::Acquisitions acquisitions_2d;
    for (size_t i = 0; i < config.acquisitions_2d.size(); ++i) {
        acquisitions_2d.emplaceBack(
            make_acquisition<Zivid::Settings2D::Acquisition>(config.acquisitions_2d[i], "acquisitions_2d[" + std::to_string(i) + "]"));
    }
    if (acquisitions_2d.isEmpty()) {
        acquisitions_2d.emplaceBack(Zivid::Settings2D::Acquisition{});
    }

    Zivid::Settings2D settings_2d{acquisitions_2d};
    if (config.color_pixel_sampling) {
        settings_2d.set(Zivid::Settings2D::Sampling::Pixel{
            parse_pixel_sampling<Zivid::Settings2D::Sampling::Pixel::ValueType>(*config.color_pixel_sampling, "color_pixel_sampling")});
    }
    return settings_2d;
}

Zivid::Settings make_settings(const Config& config) {
    Zivid::Settings::Acquisitions acquisitions;
    for (size_t i = 0; i < config.acquisitions.size(); ++i) {
        acquisitions.emplaceBack(
            make_acquisition<Zivid::Settings::Acquisition>(config.acquisitions[i], "acquisitions[" + std::to_string(i) + "]"));
    }

    Zivid::Settings settings{acquisitions, Zivid::Settings::Color{make_settings_2d(config)}};

    if (config.pixel_sampling) {
        settings.set(Zivid::Settings::Sampling::Pixel{
            parse_pixel_sampling<Zivid::Settings::Sampling::Pixel::ValueType>(*config.pixel_sampling, "pixel_sampling")});
    }

    if (config.engine) {
        const auto& e = *config.engine;
        if (e == "phase")
            settings.set(Zivid::Settings::Engine::phase);
        else if (e == "stripe")
            settings.set(Zivid::Settings::Engine::stripe);
        else if (e == "omni")
            settings.set(Zivid::Settings::Engine::omni);
        else if (e == "sage")
            settings.set(Zivid::Settings::Engine{Zivid::Settings::Engine::ValueType::sage});
        else
            throw std::invalid_argument("Unknown Zivid engine '" + e + "'. Valid values: phase, stripe, omni, sage.");
    }

    if (config.depth_roi) {
        const auto& d = *config.depth_roi;
        settings.set(Zivid::Settings::RegionOfInterest{Zivid::Settings::RegionOfInterest::Depth{
            Zivid::Settings::RegionOfInterest::Depth::Enabled{true}, Zivid::Settings::RegionOfInterest::Depth::Range{d.min, d.max}}});
    }

    if (config.box_roi) {
        const auto& b = *config.box_roi;
        settings.set(Zivid::Settings::RegionOfInterest{Zivid::Settings::RegionOfInterest::Box{
            Zivid::Settings::RegionOfInterest::Box::Enabled{true},
            Zivid::Settings::RegionOfInterest::Box::PointO{
                static_cast<float>(b.point_o.x), static_cast<float>(b.point_o.y), static_cast<float>(b.point_o.z)},
            Zivid::Settings::RegionOfInterest::Box::PointA{
                static_cast<float>(b.point_a.x), static_cast<float>(b.point_a.y), static_cast<float>(b.point_a.z)},
            Zivid::Settings::RegionOfInterest::Box::PointB{
                static_cast<float>(b.point_b.x), static_cast<float>(b.point_b.y), static_cast<float>(b.point_b.z)},
            Zivid::Settings::RegionOfInterest::Box::Extents{b.extents_min, b.extents_max}}});
    }

    if (config.processing && config.processing->noise_removal) {
        const auto& nr = *config.processing->noise_removal;
        using NoiseRemoval = Zivid::Settings::Processing::Filters::Noise::Removal;
        if (nr.enabled)
            settings.set(NoiseRemoval::Enabled{*nr.enabled});
        if (nr.threshold)
            settings.set(
                NoiseRemoval::Threshold{checked_value<NoiseRemoval::Threshold>(*nr.threshold, "processing.noise_removal.threshold")});
    }

    return settings;
}

}  // namespace

Config parse_config(const viam::sdk::ResourceConfig& cfg) {
    Config result;
    const ConfigObject root{cfg.attributes(), ""};

    result.serial_number = root.get<std::string>("serial_number");
    result.engine = root.get<std::string>("engine");
    result.pixel_sampling = root.get<std::string>("pixel_sampling");
    result.color_pixel_sampling = root.get<std::string>("color_pixel_sampling");

    auto parse_acquisitions = [&](const std::string& key) {
        std::vector<AcquisitionConfig> out;
        const viam::sdk::ProtoList* items = root.list(key);
        if (items == nullptr) {
            return out;
        }
        for (size_t i = 0; i < items->size(); ++i) {
            const std::string item_path = key + "[" + std::to_string(i) + "]";
            const ConfigObject a{typed_value<viam::sdk::ProtoStruct>((*items)[i], attr_desc(item_path)), item_path};
            AcquisitionConfig acq;
            acq.aperture = a.get<double>("aperture");
            acq.brightness = a.get<double>("brightness");
            acq.exposure_time_us = a.get<double>("exposure_time_us");
            acq.gain = a.get<double>("gain");
            out.push_back(acq);
        }
        return out;
    };

    result.acquisitions = parse_acquisitions("acquisitions");
    result.acquisitions_2d = parse_acquisitions("acquisitions_2d");
    if (result.acquisitions_2d.empty())
        result.acquisitions_2d = parse_acquisitions("acquisitions_2D");

    // Fall back to a single default 3D acquisition if none specified.
    if (result.acquisitions.empty()) {
        result.acquisitions.push_back(AcquisitionConfig{});
    }

    if (const auto roi = root.object("roi")) {
        if (const auto depth = roi->object("depth")) {
            result.depth_roi = DepthRoiConfig{depth->get<double>("min").value_or(0.0), depth->get<double>("max").value_or(0.0)};
        }

        if (const auto box = roi->object("box")) {
            auto parse_point = [&](const std::string& key) -> Point3D {
                const ConfigObject p = box->required_object(key);
                return {p.get<double>("x").value_or(0.0), p.get<double>("y").value_or(0.0), p.get<double>("z").value_or(0.0)};
            };

            const ConfigObject extents = box->required_object("extents");
            result.box_roi = BoxRoiConfig{parse_point("point_o"),
                                          parse_point("point_a"),
                                          parse_point("point_b"),
                                          extents.get<double>("min").value_or(0.0),
                                          extents.get<double>("max").value_or(0.0)};
        }
    }

    if (const auto proc = root.object("processing")) {
        ProcessingConfig pc;

        if (const auto nr = proc->object("noise_removal")) {
            pc.noise_removal = NoiseRemovalConfig{nr->get<bool>("enabled"), nr->get<double>("threshold")};
        }

        result.processing = pc;
    }

    return result;
}

ZividCamera::ZividCamera(std::shared_ptr<Zivid::Application> app, viam::sdk::Dependencies /*deps*/, const viam::sdk::ResourceConfig& cfg)
    : viam::sdk::Camera(cfg.name()), app_(std::move(app)) {
    const auto config = parse_config(cfg);
    settings_ = make_settings(config);
    settings_2d_ = make_settings_2d(config);

    // On reconfigure, Viam constructs the new instance before destroying the old one.
    // Disconnect any lingering connection to the target camera so connectCamera() succeeds.
    for (auto& cam : app_->cameras()) {
        const bool matches = config.serial_number ? cam.info().serialNumber().value() == *config.serial_number
                                                  : cam.state().status().value() == Zivid::CameraState::Status::ValueType::connected;
        if (matches) {
            try {
                cam.disconnect();
            } catch (...) {
            }
            break;
        }
    }

    const std::string target =
        config.serial_number ? "Zivid camera " + *config.serial_number : std::string{"the first available Zivid camera"};
    try {
        if (config.serial_number) {
            camera_ = app_->connectCamera(Zivid::CameraInfo::SerialNumber{*config.serial_number});
        } else {
            camera_ = app_->connectCamera();
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("failed to connect to " + target + ": " + e.what());
    }

    const std::string camera_desc = describe_camera(camera_);
    VIAM_RESOURCE_LOG(info) << "connected to " << camera_desc;

    const auto cam_info = camera_.info();
    check_acquisitions_against_camera<Zivid::Settings::Acquisition>(config.acquisitions, cam_info, "acquisitions", camera_desc);
    check_acquisitions_against_camera<Zivid::Settings2D::Acquisition>(config.acquisitions_2d, cam_info, "acquisitions_2d", camera_desc);

    std::lock_guard<std::mutex> lk(registry_mutex_);
    registry_[cfg.name()] = this;
}

ZividCamera::~ZividCamera() {
    {
        std::lock_guard<std::mutex> lk(registry_mutex_);
        registry_.erase(name());
    }
    try {
        camera_.disconnect();
    } catch (...) {
    }
}

Zivid::Frame ZividCamera::get_or_capture() {
    std::unique_lock<std::mutex> lock(capture_mutex_);

    // Return cached frame if still fresh.
    const auto now = std::chrono::steady_clock::now();
    if (cached_frame_ && (now - cached_frame_time_) < kFrameCacheTtl) {
        return *cached_frame_;
    }

    // Another thread is already capturing — wait for it to finish and reuse its frame.
    if (capturing_) {
        capture_cv_.wait(lock, [this] { return !capturing_; });
        // The owning thread clears capturing_ on failure too, in which case there is no frame
        // to hand back and dereferencing the empty optional would be undefined behaviour.
        if (!cached_frame_) {
            throw std::runtime_error("capture failed on another thread; see the preceding error");
        }
        return *cached_frame_;
    }

    // This thread owns the capture.
    capturing_ = true;
    lock.unlock();

    VIAM_RESOURCE_LOG(info) << "capture begin";
    const auto capture_start = std::chrono::steady_clock::now();
    std::optional<Zivid::Frame> frame;
    try {
        frame = camera_.capture2D3D(settings_);
    } catch (const std::exception& e) {
        // Leaving capturing_ set would wedge every later capture on the condition variable.
        lock.lock();
        capturing_ = false;
        lock.unlock();
        capture_cv_.notify_all();
        throw std::runtime_error(describe_camera(camera_) + " rejected the configured capture settings: " + e.what());
    }
    const auto capture_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - capture_start).count();
    VIAM_RESOURCE_LOG(info) << "capture end (" << capture_ms << " ms)";

    lock.lock();
    cached_frame_ = std::move(frame);
    cached_frame_time_ = std::chrono::steady_clock::now();
    capturing_ = false;
    lock.unlock();

    capture_cv_.notify_all();
    return *cached_frame_;
}

viam::sdk::Camera::image_collection ZividCamera::get_images(std::vector<std::string> filter_source_names,
                                                            const viam::sdk::ProtoStruct& /*extra*/) {
    const bool want_color =
        filter_source_names.empty() ||
        std::find(filter_source_names.begin(), filter_source_names.end(), kColorSourceName) != filter_source_names.end();
    const bool want_depth =
        filter_source_names.empty() ||
        std::find(filter_source_names.begin(), filter_source_names.end(), kDepthSourceName) != filter_source_names.end();

    auto frame = get_or_capture();

    viam::sdk::Camera::image_collection result;

    if (want_color) {
        auto opt_frame2d = frame.frame2D();
        if (!opt_frame2d) {
            throw std::runtime_error("No 2D frame in capture result");
        }
        const auto color_img = opt_frame2d->imageRGBA_SRGB();
        const int w = static_cast<int>(color_img.width());
        const int h = static_cast<int>(color_img.height());
        auto jpeg = encode_jpeg(reinterpret_cast<const uint8_t*>(color_img.data()), w, h);

        viam::sdk::Camera::raw_image color_raw;
        color_raw.mime_type = "image/jpeg";
        color_raw.bytes = std::move(jpeg);
        color_raw.source_name = kColorSourceName;
        result.images.push_back(std::move(color_raw));
    }

    if (want_depth) {
        const auto pc = frame.pointCloud();
        const auto xyz = pc.copyPointsXYZ();
        auto dm = build_depth_map(xyz, pc.width(), pc.height());
        auto encoded = viam::sdk::Camera::encode_depth_map(dm);

        viam::sdk::Camera::raw_image depth_raw;
        depth_raw.mime_type = "image/vnd.viam.dep";
        depth_raw.bytes = std::move(encoded);
        depth_raw.source_name = kDepthSourceName;
        result.images.push_back(std::move(depth_raw));
    }

    return result;
}

viam::sdk::Camera::point_cloud ZividCamera::get_point_cloud(std::string /*mime_type*/, const viam::sdk::ProtoStruct& /*extra*/) {
    auto frame = get_or_capture();
    const auto pc = frame.pointCloud();
    const auto points = pc.copyPointsXYZColorsRGBA();

    viam::sdk::Camera::point_cloud result;
    result.mime_type = "pointcloud/pcd";
    result.pc = encode_pcd(points);
    return result;
}

viam::sdk::Camera::properties ZividCamera::get_properties() {
    // The intrinsics MUST correspond to the 2D color image that get_images() serves, so they
    // are derived from the same 2D settings the capture uses.
    //
    // This is also the first point at which the config-derived 2D settings are checked against
    // the connected camera model, so a value this model does not support surfaces here rather
    // than while parsing.
    try {
        const auto info = camera_.info();
        const auto base = Zivid::Experimental::Calibration::intrinsics(camera_, settings_2d_);
        const auto base_res = Zivid::Experimental::SettingsInfo::resolution2D(info, settings_2d_);
        const auto color_res = Zivid::Experimental::SettingsInfo::resolution2D(info, settings_);

        const double sx = static_cast<double>(color_res.width()) / base_res.width();
        const double sy = static_cast<double>(color_res.height()) / base_res.height();

        VIAM_RESOURCE_LOG(debug) << "[get_properties] base_res " << base_res.width() << "x" << base_res.height() << ", served color_res "
                                 << color_res.width() << "x" << color_res.height() << ", scale " << sx << "x" << sy;

        viam::sdk::Camera::properties props{};
        props.supports_pcd = true;

        const auto& cm = base.cameraMatrix();
        props.intrinsic_parameters.width_px = static_cast<int>(color_res.width());
        props.intrinsic_parameters.height_px = static_cast<int>(color_res.height());
        props.intrinsic_parameters.focal_x_px = cm.fx().value() * sx;
        props.intrinsic_parameters.focal_y_px = cm.fy().value() * sy;
        props.intrinsic_parameters.center_x_px = cm.cx().value() * sx;
        props.intrinsic_parameters.center_y_px = cm.cy().value() * sy;

        const auto& dist = base.distortion();
        props.distortion_parameters.model = "brown_conrady";
        props.distortion_parameters.parameters =
            std::vector<double>{dist.k1().value(), dist.k2().value(), dist.p1().value(), dist.p2().value(), dist.k3().value()};

        props.mime_types = {"image/jpeg", "image/vnd.viam.dep"};
        props.frame_rate = 0.f;  // on-demand capture; no fixed frame rate

        return props;
    } catch (const std::exception& e) {
        const std::string camera_desc = describe_camera(camera_);
        VIAM_RESOURCE_LOG(error) << "2D color settings rejected by " << camera_desc << ":\n" << settings_2d_.toString();
        throw std::runtime_error(camera_desc +
                                 " rejected the configured 2D color settings while computing intrinsics; check acquisitions_2d and "
                                 "color_pixel_sampling against this camera model: " +
                                 e.what());
    }
}

std::vector<viam::sdk::GeometryConfig> ZividCamera::get_geometries(const viam::sdk::ProtoStruct& /*extra*/) {
    return {};
}

viam::sdk::ProtoStruct ZividCamera::do_command(const viam::sdk::ProtoStruct& command) {
    auto it = command.find("command");
    if (it == command.end()) {
        return {};
    }

    const auto& cmd = typed_value<std::string>(it->second, "DoCommand field 'command'");

    if (cmd == "get_acquisition_ranges") {
        const auto info = camera_.info();
        namespace SI = Zivid::Experimental::SettingsInfo;

        auto make_range_struct = [](double min, double max) {
            viam::sdk::ProtoStruct s;
            s["min"] = min;
            s["max"] = max;
            return viam::sdk::ProtoValue{std::move(s)};
        };

        // 3D acquisition ranges
        viam::sdk::ProtoStruct acq3d;
        {
            auto aperture = SI::validRange<Zivid::Settings::Acquisition::Aperture>(info);
            auto brightness = SI::validRange<Zivid::Settings::Acquisition::Brightness>(info);
            auto exposure = SI::validRange<Zivid::Settings::Acquisition::ExposureTime>(info);
            auto gain = SI::validRange<Zivid::Settings::Acquisition::Gain>(info);
            acq3d["aperture"] = make_range_struct(aperture.min(), aperture.max());
            acq3d["brightness"] = make_range_struct(brightness.min(), brightness.max());
            acq3d["exposure_time_us"] =
                make_range_struct(static_cast<double>(exposure.min().count()), static_cast<double>(exposure.max().count()));
            acq3d["gain"] = make_range_struct(gain.min(), gain.max());
        }

        // 2D acquisition ranges
        viam::sdk::ProtoStruct acq2d;
        {
            auto aperture = SI::validRange<Zivid::Settings2D::Acquisition::Aperture>(info);
            auto brightness = SI::validRange<Zivid::Settings2D::Acquisition::Brightness>(info);
            auto exposure = SI::validRange<Zivid::Settings2D::Acquisition::ExposureTime>(info);
            auto gain = SI::validRange<Zivid::Settings2D::Acquisition::Gain>(info);
            acq2d["aperture"] = make_range_struct(aperture.min(), aperture.max());
            acq2d["brightness"] = make_range_struct(brightness.min(), brightness.max());
            acq2d["exposure_time_us"] =
                make_range_struct(static_cast<double>(exposure.min().count()), static_cast<double>(exposure.max().count()));
            acq2d["gain"] = make_range_struct(gain.min(), gain.max());
        }

        viam::sdk::ProtoStruct result;
        result["acquisitions"] = viam::sdk::ProtoValue{std::move(acq3d)};
        result["acquisitions_2d"] = viam::sdk::ProtoValue{std::move(acq2d)};
        return result;
    }

    if (cmd == "get_network_configuration") {
        const auto net = camera_.networkConfiguration();
        const auto& ipv4 = net.ipv4();

        viam::sdk::ProtoStruct ipv4_struct;
        ipv4_struct["mode"] = ipv4.mode().toString();
        ipv4_struct["address"] = ipv4.address().value();
        ipv4_struct["subnet_mask"] = ipv4.subnetMask().value();

        viam::sdk::ProtoStruct result;
        result["ipv4"] = viam::sdk::ProtoValue{std::move(ipv4_struct)};
        return result;
    }

    if (cmd == "save_zdf") {
        std::string path;
        auto path_it = command.find("path");
        if (path_it != command.end()) {
            path = typed_value<std::string>(path_it->second, "DoCommand field 'path'");
        } else {
            const auto ts =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            path = "/var/lib/viam/zivid_diagnostic_" + camera_.info().serialNumber().value() + "_" + std::to_string(ts) + ".zdf";
        }

        Zivid::Settings diag_settings = settings_;
        diag_settings.set(Zivid::Settings::Diagnostics::Enabled{true});

        std::unique_lock<std::mutex> lock(capture_mutex_);
        capture_cv_.wait(lock, [this] { return !capturing_; });
        capturing_ = true;
        lock.unlock();

        VIAM_RESOURCE_LOG(info) << "diagnostic capture begin (saving to " << path << ")";
        try {
            Zivid::Frame frame = camera_.capture2D3D(diag_settings);
            frame.save(path);
        } catch (...) {
            lock.lock();
            capturing_ = false;
            capture_cv_.notify_all();
            throw;
        }
        VIAM_RESOURCE_LOG(info) << "diagnostic capture saved";

        lock.lock();
        capturing_ = false;
        capture_cv_.notify_all();

        viam::sdk::ProtoStruct result;
        result["path"] = path;
        return result;
    }

    if (cmd == "get_camera_state") {
        const auto state = camera_.state();

        viam::sdk::ProtoStruct temp;
        temp["dmd"] = state.temperature().dmd().value();
        temp["general"] = state.temperature().general().value();
        temp["led"] = state.temperature().led().value();
        temp["lens"] = state.temperature().lens().value();
        temp["pcb"] = state.temperature().pcb().value();

        viam::sdk::ProtoStruct result;
        result["status"] = state.status().toString();
        result["temperature"] = viam::sdk::ProtoValue{std::move(temp)};

        if (state.inaccessibleReason().hasValue()) {
            result["inaccessible_reason"] = state.inaccessibleReason().toString();
        }

        return result;
    }

    return {};
}

Zivid::Frame ZividCamera::capture_for_calibration() {
    std::unique_lock<std::mutex> lock(capture_mutex_);
    capture_cv_.wait(lock, [this] { return !capturing_; });

    capturing_ = true;
    lock.unlock();

    VIAM_RESOURCE_LOG(info) << "calibration capture begin";
    std::optional<Zivid::Frame> frame;
    try {
        frame = camera_.capture2D3D(settings_);
    } catch (const std::exception& e) {
        // Leaving capturing_ set would wedge every later capture on the condition variable.
        lock.lock();
        capturing_ = false;
        lock.unlock();
        capture_cv_.notify_all();
        throw std::runtime_error(describe_camera(camera_) + " rejected the configured capture settings: " + e.what());
    }
    VIAM_RESOURCE_LOG(info) << "calibration capture end";

    lock.lock();
    capturing_ = false;
    lock.unlock();
    capture_cv_.notify_all();

    return std::move(*frame);
}

std::string ZividCamera::serial_number() const {
    return camera_.info().serialNumber().value();
}

viam::sdk::ProtoStruct ZividCamera::get_status() {
    viam::sdk::ProtoStruct status;
    const auto state = camera_.state();
    status["status"] = state.toString();
    return status;
}

}  // namespace viam_zivid
