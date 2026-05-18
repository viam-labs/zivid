#include "zivid_camera.hpp"

#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <Zivid/CameraInfo.h>
#include <Zivid/Experimental/SettingsInfo.h>
#include <Zivid/NetworkConfiguration.h>
#include <Zivid/CameraIntrinsics.h>
#include <Zivid/Experimental/Calibration.h>
#include <Zivid/Image.h>
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
    uint32_t rgb = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
                   static_cast<uint32_t>(b);
    float out{};
    std::memcpy(&out, &rgb, sizeof(float));
    return out;
}

// Encode an RGBA image as a JPEG byte vector using stb_image_write.
std::vector<unsigned char> encode_jpeg(const uint8_t* rgba_data, int width, int height,
                                       int quality = 85) {
    std::vector<unsigned char> buf;
    auto callback = [](void* ctx, void* data, int size) {
        auto* vec = static_cast<std::vector<unsigned char>*>(ctx);
        const auto* bytes = static_cast<unsigned char*>(data);
        vec->insert(vec->end(), bytes, bytes + size);
    };
    int rc = stbi_write_jpg_to_func(callback, &buf, width, height, 4,
                                    rgba_data, quality);
    if (rc == 0) {
        throw std::runtime_error("JPEG compression failed (stb_image_write)");
    }
    return buf;
}

// Apply sRGB gamma encoding to a linear uint8 value to brighten it for viewers
// that treat stored PCD colors as linear (e.g. CloudCompare without sRGB support).
uint8_t gamma_encode(uint8_t v) {
    const float f = v / 255.f;
    const float g = (f <= 0.0031308f) ? 12.92f * f
                                       : 1.055f * std::pow(f, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(g * 255.f + 0.5f);
}

// Build a binary PCD buffer from XYZ + RGBA point cloud data.
// Colors from copyPointsXYZColorsRGBA() are in Zivid's linear-ish space; we apply
// sRGB gamma encoding so they render correctly in viewers (e.g. CloudCompare) that
// treat stored values as linear.
// Only valid (non-NaN) points are included (unorganized layout).
std::vector<unsigned char> encode_pcd(
    const Zivid::Array2D<Zivid::PointXYZColorRGBA>& points) {
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
        valid_points.push_back({p.point.x * 1e-3f, p.point.y * 1e-3f, p.point.z * 1e-3f,
                                 pack_rgb(gamma_encode(p.color.r),
                                          gamma_encode(p.color.g),
                                          gamma_encode(p.color.b))});
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
viam::sdk::Camera::depth_map build_depth_map(const Zivid::Array2D<Zivid::PointXYZ>& points,
                                              size_t width, size_t height) {
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

template <typename T>
std::optional<T> attr(const viam::sdk::ProtoStruct& attrs, const std::string& key) {
    auto it = attrs.find(key);
    if (it == attrs.end()) return std::nullopt;
    return it->second.get_unchecked<T>();
}

Zivid::Settings::Acquisition make_acquisition(const AcquisitionConfig& a) {
    Zivid::Settings::Acquisition acq;
    if (a.aperture)
        acq.set(Zivid::Settings::Acquisition::Aperture{*a.aperture});
    if (a.brightness) {
        const auto max_brightness =
            Zivid::Settings::Acquisition::Brightness::validRange().max();
        const double clamped = std::min(*a.brightness, max_brightness);
        if (clamped != *a.brightness) {
            std::cerr << "[ZividCamera] brightness " << *a.brightness
                      << " exceeds 3D max " << max_brightness
                      << ", clamping to " << clamped << "\n";
        }
        acq.set(Zivid::Settings::Acquisition::Brightness{clamped});
    }
    if (a.exposure_time_us)
        acq.set(Zivid::Settings::Acquisition::ExposureTime{
            std::chrono::microseconds{static_cast<int64_t>(*a.exposure_time_us)}});
    if (a.gain)
        acq.set(Zivid::Settings::Acquisition::Gain{*a.gain});
    return acq;
}

Zivid::Settings2D::Acquisition make_acquisition_2d(const AcquisitionConfig& a) {
    Zivid::Settings2D::Acquisition acq;
    if (a.aperture)
        acq.set(Zivid::Settings2D::Acquisition::Aperture{*a.aperture});
    if (a.brightness) {
        const auto max_brightness =
            Zivid::Settings2D::Acquisition::Brightness::validRange().max();
        const double clamped = std::min(*a.brightness, max_brightness);
        if (clamped != *a.brightness) {
            std::cerr << "[ZividCamera] brightness " << *a.brightness
                      << " exceeds 2D max " << max_brightness
                      << ", clamping to " << clamped << "\n";
        }
        acq.set(Zivid::Settings2D::Acquisition::Brightness{clamped});
    }
    if (a.exposure_time_us)
        acq.set(Zivid::Settings2D::Acquisition::ExposureTime{
            std::chrono::microseconds{static_cast<int64_t>(*a.exposure_time_us)}});
    if (a.gain)
        acq.set(Zivid::Settings2D::Acquisition::Gain{*a.gain});
    return acq;
}

Zivid::Settings make_settings(const Config& config) {
    Zivid::Settings::Acquisitions acquisitions;
    for (const auto& a : config.acquisitions) {
        acquisitions.emplaceBack(make_acquisition(a));
    }

    Zivid::Settings2D::Acquisitions acquisitions_2d;
    for (const auto& a : config.acquisitions_2d) {
        acquisitions_2d.emplaceBack(make_acquisition_2d(a));
    }
    if (acquisitions_2d.isEmpty()) {
        acquisitions_2d.emplaceBack(Zivid::Settings2D::Acquisition{});
    }

    Zivid::Settings settings{
        acquisitions,
        Zivid::Settings::Color{Zivid::Settings2D{acquisitions_2d}}};

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
            throw std::invalid_argument("Unknown Zivid engine '" + e +
                                        "'. Valid values: phase, stripe, omni, sage.");
    }

    if (config.depth_roi) {
        const auto& d = *config.depth_roi;
        settings.set(Zivid::Settings::RegionOfInterest{
            Zivid::Settings::RegionOfInterest::Depth{
                Zivid::Settings::RegionOfInterest::Depth::Enabled{true},
                Zivid::Settings::RegionOfInterest::Depth::Range{d.min, d.max}}});
    }

    if (config.box_roi) {
        const auto& b = *config.box_roi;
        settings.set(Zivid::Settings::RegionOfInterest{
            Zivid::Settings::RegionOfInterest::Box{
                Zivid::Settings::RegionOfInterest::Box::Enabled{true},
                Zivid::Settings::RegionOfInterest::Box::PointO{
                    static_cast<float>(b.point_o.x),
                    static_cast<float>(b.point_o.y),
                    static_cast<float>(b.point_o.z)},
                Zivid::Settings::RegionOfInterest::Box::PointA{
                    static_cast<float>(b.point_a.x),
                    static_cast<float>(b.point_a.y),
                    static_cast<float>(b.point_a.z)},
                Zivid::Settings::RegionOfInterest::Box::PointB{
                    static_cast<float>(b.point_b.x),
                    static_cast<float>(b.point_b.y),
                    static_cast<float>(b.point_b.z)},
                Zivid::Settings::RegionOfInterest::Box::Extents{b.extents_min, b.extents_max}}});
    }

    return settings;
}

}  // namespace

Config parse_config(const viam::sdk::ResourceConfig& cfg) {
    Config result;
    const auto& attrs = cfg.attributes();
    result.serial_number = attr<std::string>(attrs, "serial_number");
    result.engine        = attr<std::string>(attrs, "engine");

    auto parse_acquisitions = [&](const std::string& key) {
        std::vector<AcquisitionConfig> out;
        auto it = attrs.find(key);
        if (it != attrs.end()) {
            for (const auto& item : it->second.get_unchecked<viam::sdk::ProtoList>()) {
                const auto& a = item.get_unchecked<viam::sdk::ProtoStruct>();
                AcquisitionConfig acq;
                acq.aperture         = attr<double>(a, "aperture");
                acq.brightness       = attr<double>(a, "brightness");
                acq.exposure_time_us = attr<double>(a, "exposure_time_us");
                acq.gain             = attr<double>(a, "gain");
                out.push_back(acq);
            }
        }
        return out;
    };

    result.acquisitions    = parse_acquisitions("acquisitions");
    result.acquisitions_2d = parse_acquisitions("acquisitions_2d");
    if (result.acquisitions_2d.empty())
        result.acquisitions_2d = parse_acquisitions("acquisitions_2D");

    // Fall back to a single default 3D acquisition if none specified.
    if (result.acquisitions.empty()) {
        result.acquisitions.push_back(AcquisitionConfig{});
    }

    auto roi_it = attrs.find("roi");
    if (roi_it != attrs.end()) {
        const auto& roi = roi_it->second.get_unchecked<viam::sdk::ProtoStruct>();

        auto depth_it = roi.find("depth");
        if (depth_it != roi.end()) {
            const auto& d = depth_it->second.get_unchecked<viam::sdk::ProtoStruct>();
            result.depth_roi = DepthRoiConfig{
                attr<double>(d, "min").value_or(0.0),
                attr<double>(d, "max").value_or(0.0)};
        }

        auto box_it = roi.find("box");
        if (box_it != roi.end()) {
            const auto& b = box_it->second.get_unchecked<viam::sdk::ProtoStruct>();

            auto parse_point = [&](const std::string& key) -> Point3D {
                const auto& p = b.at(key).get_unchecked<viam::sdk::ProtoStruct>();
                return {attr<double>(p, "x").value_or(0.0),
                        attr<double>(p, "y").value_or(0.0),
                        attr<double>(p, "z").value_or(0.0)};
            };

            const auto& extents = b.at("extents").get_unchecked<viam::sdk::ProtoStruct>();
            result.box_roi = BoxRoiConfig{
                parse_point("point_o"),
                parse_point("point_a"),
                parse_point("point_b"),
                attr<double>(extents, "min").value_or(0.0),
                attr<double>(extents, "max").value_or(0.0)};
        }
    }

    return result;
}

ZividCamera::ZividCamera(std::shared_ptr<Zivid::Application> app,
                         viam::sdk::Dependencies /*deps*/,
                         const viam::sdk::ResourceConfig& cfg)
    : viam::sdk::Camera(cfg.name()), app_(std::move(app)) {
    const auto config = parse_config(cfg);
    settings_ = make_settings(config);

    // On reconfigure, Viam constructs the new instance before destroying the old one.
    // Disconnect any lingering connection to the target camera so connectCamera() succeeds.
    for (auto& cam : app_->cameras()) {
        const bool matches = config.serial_number
            ? cam.info().serialNumber().value() == *config.serial_number
            : cam.state().status().value() ==
                  Zivid::CameraState::Status::ValueType::connected;
        if (matches) {
            try { cam.disconnect(); } catch (...) {}
            break;
        }
    }

    if (config.serial_number) {
        camera_ = app_->connectCamera(
            Zivid::CameraInfo::SerialNumber{*config.serial_number});
    } else {
        camera_ = app_->connectCamera();
    }

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
        return *cached_frame_;
    }

    // This thread owns the capture.
    capturing_ = true;
    lock.unlock();

    VIAM_RESOURCE_LOG(info) << "capture begin";
    const auto capture_start = std::chrono::steady_clock::now();
    Zivid::Frame frame = camera_.capture2D3D(settings_);
    const auto capture_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - capture_start)
                                .count();
    VIAM_RESOURCE_LOG(info) << "capture end (" << capture_ms << " ms)";

    lock.lock();
    cached_frame_ = std::move(frame);
    cached_frame_time_ = std::chrono::steady_clock::now();
    capturing_ = false;
    lock.unlock();

    capture_cv_.notify_all();
    return *cached_frame_;
}

viam::sdk::Camera::image_collection ZividCamera::get_images(
    std::vector<std::string> filter_source_names, const viam::sdk::ProtoStruct& /*extra*/) {
    const bool want_color =
        filter_source_names.empty() ||
        std::find(filter_source_names.begin(), filter_source_names.end(), kColorSourceName) !=
            filter_source_names.end();
    const bool want_depth =
        filter_source_names.empty() ||
        std::find(filter_source_names.begin(), filter_source_names.end(), kDepthSourceName) !=
            filter_source_names.end();

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

viam::sdk::Camera::point_cloud ZividCamera::get_point_cloud(std::string /*mime_type*/,
                                                             const viam::sdk::ProtoStruct& /*extra*/) {
    auto frame = get_or_capture();
    const auto pc = frame.pointCloud();
    const auto points = pc.copyPointsXYZColorsRGBA();

    viam::sdk::Camera::point_cloud result;
    result.mime_type = "pointcloud/pcd";
    result.pc = encode_pcd(points);
    return result;
}

viam::sdk::Camera::properties ZividCamera::get_properties() {
    const auto intrinsics = Zivid::Experimental::Calibration::intrinsics(camera_);

    viam::sdk::Camera::properties props{};
    props.supports_pcd = true;

    const auto& cm = intrinsics.cameraMatrix();
    props.intrinsic_parameters.focal_x_px = cm.fx().value();
    props.intrinsic_parameters.focal_y_px = cm.fy().value();
    props.intrinsic_parameters.center_x_px = cm.cx().value();
    props.intrinsic_parameters.center_y_px = cm.cy().value();

    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (cached_frame_) {
            const auto pc = cached_frame_->pointCloud();
            props.intrinsic_parameters.width_px = static_cast<int>(pc.width());
            props.intrinsic_parameters.height_px = static_cast<int>(pc.height());
        }
    }

    const auto& dist = intrinsics.distortion();
    props.distortion_parameters.model = "brown_conrady";
    props.distortion_parameters.parameters = std::vector<double>{
        dist.k1().value(), dist.k2().value(), dist.p1().value(),
        dist.p2().value(), dist.k3().value()};

    props.mime_types = {"image/jpeg", "image/vnd.viam.dep"};
    props.frame_rate = 0.f;  // on-demand capture; no fixed frame rate

    return props;
}

std::vector<viam::sdk::GeometryConfig> ZividCamera::get_geometries(
    const viam::sdk::ProtoStruct& /*extra*/) {
    return {};
}

viam::sdk::ProtoStruct ZividCamera::do_command(const viam::sdk::ProtoStruct& command) {
    auto it = command.find("command");
    if (it == command.end()) {
        return {};
    }

    const auto& cmd = it->second.get_unchecked<std::string>();

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
            auto aperture   = SI::validRange<Zivid::Settings::Acquisition::Aperture>(info);
            auto brightness = SI::validRange<Zivid::Settings::Acquisition::Brightness>(info);
            auto exposure   = SI::validRange<Zivid::Settings::Acquisition::ExposureTime>(info);
            auto gain       = SI::validRange<Zivid::Settings::Acquisition::Gain>(info);
            acq3d["aperture"]         = make_range_struct(aperture.min(), aperture.max());
            acq3d["brightness"]       = make_range_struct(brightness.min(), brightness.max());
            acq3d["exposure_time_us"] = make_range_struct(
                static_cast<double>(exposure.min().count()),
                static_cast<double>(exposure.max().count()));
            acq3d["gain"] = make_range_struct(gain.min(), gain.max());
        }

        // 2D acquisition ranges
        viam::sdk::ProtoStruct acq2d;
        {
            auto aperture   = SI::validRange<Zivid::Settings2D::Acquisition::Aperture>(info);
            auto brightness = SI::validRange<Zivid::Settings2D::Acquisition::Brightness>(info);
            auto exposure   = SI::validRange<Zivid::Settings2D::Acquisition::ExposureTime>(info);
            auto gain       = SI::validRange<Zivid::Settings2D::Acquisition::Gain>(info);
            acq2d["aperture"]         = make_range_struct(aperture.min(), aperture.max());
            acq2d["brightness"]       = make_range_struct(brightness.min(), brightness.max());
            acq2d["exposure_time_us"] = make_range_struct(
                static_cast<double>(exposure.min().count()),
                static_cast<double>(exposure.max().count()));
            acq2d["gain"] = make_range_struct(gain.min(), gain.max());
        }

        viam::sdk::ProtoStruct result;
        result["acquisitions"]    = viam::sdk::ProtoValue{std::move(acq3d)};
        result["acquisitions_2d"] = viam::sdk::ProtoValue{std::move(acq2d)};
        return result;
    }

    if (cmd == "get_network_configuration") {
        const auto net = camera_.networkConfiguration();
        const auto& ipv4 = net.ipv4();

        viam::sdk::ProtoStruct ipv4_struct;
        ipv4_struct["mode"]        = ipv4.mode().toString();
        ipv4_struct["address"]     = ipv4.address().value();
        ipv4_struct["subnet_mask"] = ipv4.subnetMask().value();

        viam::sdk::ProtoStruct result;
        result["ipv4"] = viam::sdk::ProtoValue{std::move(ipv4_struct)};
        return result;
    }

    if (cmd == "get_camera_state") {
        const auto state = camera_.state();

        viam::sdk::ProtoStruct temp;
        temp["dmd"]     = state.temperature().dmd().value();
        temp["general"] = state.temperature().general().value();
        temp["led"]     = state.temperature().led().value();
        temp["lens"]    = state.temperature().lens().value();
        temp["pcb"]     = state.temperature().pcb().value();

        viam::sdk::ProtoStruct result;
        result["status"]      = state.status().toString();
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
    Zivid::Frame frame = camera_.capture2D3D(settings_);
    VIAM_RESOURCE_LOG(info) << "calibration capture end";

    lock.lock();
    capturing_ = false;
    lock.unlock();
    capture_cv_.notify_all();

    return frame;
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
