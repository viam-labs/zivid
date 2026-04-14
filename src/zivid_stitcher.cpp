#include "zivid_stitcher.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <Zivid/Experimental/LocalPointCloudRegistrationParameters.h>
#include <Zivid/Experimental/PointCloudExport.h>
#include <Zivid/Experimental/Toolbox/PointCloudRegistration.h>
#include <viam/sdk/log/logging.hpp>

#include "pose_utils.hpp"
#include "zivid_camera.hpp"

namespace viam_zivid {

namespace {

// ---------------------------------------------------------------------------
// Hand-eye JSON loading
// ---------------------------------------------------------------------------

// Scale matrix that converts millimetres to metres (applied before export).
Zivid::Matrix4x4 mm_to_m_matrix() {
    // clang-format off
    const std::array<float, 16> elems{
        0.001f, 0.f,    0.f,    0.f,
        0.f,    0.001f, 0.f,    0.f,
        0.f,    0.f,    0.001f, 0.f,
        0.f,    0.f,    0.f,    1.f};
    // clang-format on
    return Zivid::Matrix4x4{elems.begin(), elems.end()};
}

// Multiply two 4×4 Zivid matrices (Zivid::Matrix4x4 has no operator*).
Zivid::Matrix4x4 mat4_multiply(const Zivid::Matrix4x4& A, const Zivid::Matrix4x4& B) {
    std::array<float, 16> out{};
    for (size_t r = 0; r < 4; ++r) {
        for (size_t c = 0; c < 4; ++c) {
            float v = 0.f;
            for (size_t k = 0; k < 4; ++k) v += A(r, k) * B(k, c);
            out[r * 4 + c] = v;
        }
    }
    return Zivid::Matrix4x4{out.begin(), out.end()};
}

// Minimal extractor: find the first occurrence of `"transform": [` in the file
// and parse 16 comma-separated floating-point numbers that follow.
Zivid::Matrix4x4 load_hand_eye_transform(const std::string& json_path) {
    std::ifstream f(json_path);
    if (!f.is_open()) {
        throw std::runtime_error("ZividStitcher: cannot open hand_eye_json: " + json_path);
    }
    const std::string content((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());

    const std::string key = "\"transform\":";
    const auto key_pos = content.find(key);
    if (key_pos == std::string::npos) {
        throw std::runtime_error("ZividStitcher: 'transform' key not found in " + json_path);
    }

    // Find the opening '[' after the key.
    auto bracket_pos = content.find('[', key_pos + key.size());
    if (bracket_pos == std::string::npos) {
        throw std::runtime_error("ZividStitcher: malformed transform array in " + json_path);
    }

    // Parse 16 doubles from the array.
    std::array<float, 16> elems{};
    std::istringstream ss(content.substr(bracket_pos + 1));
    char sep;
    for (int i = 0; i < 16; ++i) {
        double v;
        if (!(ss >> v)) {
            throw std::runtime_error("ZividStitcher: could not parse transform element " +
                                     std::to_string(i) + " in " + json_path);
        }
        elems[static_cast<size_t>(i)] = static_cast<float>(v);
        if (i < 15) ss >> sep;  // consume comma
    }
    return Zivid::Matrix4x4{elems.begin(), elems.end()};
}

// ---------------------------------------------------------------------------
// Timestamp helper
// ---------------------------------------------------------------------------

std::string timestamp_for_filename() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", std::gmtime(&t));
    return std::string(buf);
}

}  // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ZividStitcher::ZividStitcher(viam::sdk::Dependencies deps,
                              const viam::sdk::ResourceConfig& cfg)
    : viam::sdk::GenericService(cfg.name()) {
    const auto& attrs = cfg.attributes();

    auto require_str = [&](const std::string& key) -> std::string {
        auto it = attrs.find(key);
        if (it == attrs.end())
            throw std::invalid_argument("ZividStitcher: '" + key + "' attribute required");
        return it->second.get_unchecked<std::string>();
    };

    const auto arm_name = require_str("arm");
    camera_name_ = require_str("camera");
    const auto hand_eye_json = require_str("hand_eye_json");

    auto dir_it = attrs.find("save_dir");
    save_dir_ = (dir_it != attrs.end())
                    ? dir_it->second.get_unchecked<std::string>()
                    : "/var/lib/viam";

    auto voxel_it = attrs.find("voxel_size_mm");
    if (voxel_it != attrs.end())
        voxel_size_mm_ = static_cast<float>(voxel_it->second.get_unchecked<double>());

    auto icp_it = attrs.find("icp_enabled");
    if (icp_it != attrs.end())
        icp_enabled_ = icp_it->second.get_unchecked<bool>();

    auto corr_it = attrs.find("icp_max_correspondence_mm");
    if (corr_it != attrs.end())
        icp_max_correspondence_mm_ =
            static_cast<float>(corr_it->second.get_unchecked<double>());

    auto settle_it = attrs.find("settle_delay_s");
    if (settle_it != attrs.end())
        settle_delay_s_ = settle_it->second.get_unchecked<double>();

    auto per_pose_it = attrs.find("save_per_pose_clouds");
    if (per_pose_it != attrs.end())
        save_per_pose_clouds_ = per_pose_it->second.get_unchecked<bool>();

    // Parse scan_poses list.
    auto poses_it = attrs.find("scan_poses");
    if (poses_it != attrs.end()) {
        for (const auto& item : poses_it->second.get_unchecked<viam::sdk::ProtoList>()) {
            const auto& p = item.get_unchecked<viam::sdk::ProtoStruct>();
            auto dbl = [&](const std::string& k, double def = 0.0) -> double {
                auto it = p.find(k);
                return (it != p.end()) ? it->second.get_unchecked<double>() : def;
            };
            scan_poses_.push_back({dbl("x"), dbl("y"), dbl("z"),
                                   dbl("ox"), dbl("oy"), dbl("oz", -1.0), dbl("theta")});
        }
    }

    // Load hand-eye calibration transform.
    hand_eye_transform_ = load_hand_eye_transform(hand_eye_json);

    // Wire up arm dependency by searching the deps map by short name, avoiding
    // Name{} construction issues with different SDK versions.
    for (const auto& [dep_name, dep_resource] : deps) {
        if (dep_name.name() == arm_name) {
            arm_ = std::dynamic_pointer_cast<viam::sdk::Arm>(dep_resource);
            break;
        }
    }
    if (!arm_) {
        throw std::runtime_error("ZividStitcher: arm dependency '" + arm_name +
                                 "' not found in deps — ensure it is listed in depends_on");
    }

    // Camera is looked up lazily via ZividCamera::find() at capture time;
    // no depends_on entry needed for it.

    VIAM_RESOURCE_LOG(info) << "ZividStitcher ready. hand_eye_json=" << hand_eye_json
                             << " voxel_size_mm=" << voxel_size_mm_
                             << " icp_enabled=" << icp_enabled_;
}

// ---------------------------------------------------------------------------
// do_command dispatcher
// ---------------------------------------------------------------------------

viam::sdk::ProtoStruct ZividStitcher::do_command(const viam::sdk::ProtoStruct& command) {
    auto it = command.find("command");
    if (it == command.end()) return {};

    const auto& cmd = it->second.get_unchecked<std::string>();

    if (cmd == "run_scan") return cmd_run_scan();
    if (cmd == "reset_scan") return cmd_reset_scan();
    if (cmd == "capture_and_accumulate") return cmd_capture_and_accumulate();
    if (cmd == "export") return cmd_export(command);

    throw std::invalid_argument("ZividStitcher: unknown command '" + cmd +
                                "'. Valid: run_scan, reset_scan, capture_and_accumulate, export");
}

viam::sdk::ProtoStruct ZividStitcher::get_status() {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    viam::sdk::ProtoStruct s;
    s["accumulated_count"] = static_cast<double>(accumulated_count_);
    s["point_count"] = static_cast<double>(
        accumulated_cloud_ ? accumulated_cloud_->size() : 0u);
    return s;
}

// ---------------------------------------------------------------------------
// run_scan
// ---------------------------------------------------------------------------

viam::sdk::ProtoStruct ZividStitcher::cmd_run_scan() {
    if (scan_poses_.empty()) {
        throw std::runtime_error(
            "ZividStitcher: no scan_poses configured. "
            "Add a 'scan_poses' list to the service attributes.");
    }

    // Create a timestamped output directory for this scan session.
    const std::string ts = timestamp_for_filename();
    const std::filesystem::path out_dir =
        std::filesystem::path(save_dir_) / ("zivid_scan_" + ts);
    std::filesystem::create_directories(out_dir);
    VIAM_RESOURCE_LOG(info) << "run_scan: output directory " << out_dir.string();

    VIAM_RESOURCE_LOG(info) << "run_scan: starting scan with " << scan_poses_.size()
                             << " poses, settle_delay=" << settle_delay_s_ << "s";

    // Reset any previous session.
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        accumulated_cloud_.reset();
        accumulated_count_ = 0;
    }

    viam::sdk::ProtoList pose_results;
    size_t detected_count = 0;

    for (size_t i = 0; i < scan_poses_.size(); ++i) {
        const auto& sp = scan_poses_[i];

        viam::sdk::pose target;
        target.coordinates.x = sp.x;
        target.coordinates.y = sp.y;
        target.coordinates.z = sp.z;
        target.orientation.o_x = sp.ox;
        target.orientation.o_y = sp.oy;
        target.orientation.o_z = sp.oz;
        target.theta = sp.theta;

        VIAM_RESOURCE_LOG(info) << "run_scan pose " << (i + 1) << "/" << scan_poses_.size()
                                 << ": moving to x=" << sp.x << " y=" << sp.y
                                 << " z=" << sp.z << " ox=" << sp.ox << " oy=" << sp.oy
                                 << " oz=" << sp.oz << " theta=" << sp.theta;

        bool move_ok = true;
        try {
            arm_->move_to_position(target);
        } catch (const std::exception& e) {
            VIAM_RESOURCE_LOG(warn) << "run_scan pose " << (i + 1) << ": move failed: "
                                     << e.what() << " — skipping";
            move_ok = false;
        }

        viam::sdk::ProtoStruct pose_result;
        pose_result["pose_index"] = static_cast<double>(i);
        pose_result["move_ok"] = move_ok;

        if (move_ok) {
            // Settle.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int64_t>(settle_delay_s_ * 1000)));

            try {
                auto cloud = capture_transformed_cloud();

                // Save per-pose cloud as .pcd in world frame.
                if (save_per_pose_clouds_) {
                    const std::string pose_path =
                        (out_dir / ("pose_" + std::to_string(i + 1) + ".pcd")).string();
                    save_cloud_to_pcd(cloud, pose_path);
                    pose_result["pose_file"] = pose_path;
                    VIAM_RESOURCE_LOG(info) << "Saved pose " << (i + 1) << " to " << pose_path;
                }

                // Accumulate.
                std::lock_guard<std::mutex> lock(scan_mutex_);
                if (!accumulated_cloud_) {
                    accumulated_cloud_ = std::move(cloud);
                } else {
                    if (icp_enabled_) {
                        namespace ET = Zivid::Experimental::Toolbox;
                        Zivid::Experimental::LocalPointCloudRegistrationParameters params;
                        params.set(Zivid::Experimental::LocalPointCloudRegistrationParameters::
                                       MaxCorrespondenceDistance{icp_max_correspondence_mm_});
                        const auto reg =
                            ET::localPointCloudRegistration(cloud, *accumulated_cloud_, params);
                        if (reg.converged()) {
                            accumulated_cloud_->transform(reg.transform().toMatrix());
                        } else {
                            VIAM_RESOURCE_LOG(warn) << "ICP did not converge at pose " << (i + 1);
                        }
                    }
                    accumulated_cloud_->extend(cloud);
                }
                ++accumulated_count_;
                pose_result["accumulated_count"] = static_cast<double>(accumulated_count_);
                pose_result["point_count"] = static_cast<double>(accumulated_cloud_->size());
                ++detected_count;
            } catch (const std::exception& e) {
                VIAM_RESOURCE_LOG(warn) << "run_scan pose " << (i + 1)
                                         << ": capture failed: " << e.what();
                pose_result["capture_error"] = std::string(e.what());
            }
        }

        pose_results.push_back(viam::sdk::ProtoValue{std::move(pose_result)});
    }

    VIAM_RESOURCE_LOG(info) << "run_scan: captured " << detected_count << "/"
                             << scan_poses_.size() << " poses.";

    if (detected_count == 0) {
        throw std::runtime_error(
            "ZividStitcher: run_scan captured 0/" +
            std::to_string(scan_poses_.size()) +
            " poses — check logs for per-pose move/capture errors");
    }

    // Export merged cloud into the same output directory (in metres).
    std::lock_guard<std::mutex> lock(scan_mutex_);
    auto export_cloud = accumulated_cloud_->voxelDownsampled(voxel_size_mm_, 1);
    export_cloud.transform(mm_to_m_matrix());
    const std::string merged_path = (out_dir / "merged.ply").string();
    save_cloud_to_ply(export_cloud, merged_path);
    VIAM_RESOURCE_LOG(info) << "Exported " << export_cloud.size() << " points to " << merged_path;

    viam::sdk::ProtoStruct result;
    result["output_dir"]      = out_dir.string();
    result["output_file"]     = merged_path;
    result["point_count"]     = static_cast<double>(export_cloud.size());
    result["poses_attempted"] = static_cast<double>(scan_poses_.size());
    result["poses_captured"]  = static_cast<double>(detected_count);
    result["pose_results"]    = viam::sdk::ProtoValue{std::move(pose_results)};
    return result;
}

// ---------------------------------------------------------------------------
// reset_scan
// ---------------------------------------------------------------------------

viam::sdk::ProtoStruct ZividStitcher::cmd_reset_scan() {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    const auto count = accumulated_count_;
    accumulated_cloud_.reset();
    accumulated_count_ = 0;
    VIAM_RESOURCE_LOG(info) << "reset_scan: cleared " << count << " captures";
    viam::sdk::ProtoStruct result;
    result["cleared_count"] = static_cast<double>(count);
    return result;
}

// ---------------------------------------------------------------------------
// capture_and_accumulate
// ---------------------------------------------------------------------------

Zivid::UnorganizedPointCloud ZividStitcher::capture_transformed_cloud() {
    // Read arm end-effector pose and compute base_T_cam.
    const auto arm_pose = arm_->get_end_position();
    const Zivid::Matrix4x4 base_T_flange = viam_pose_to_zivid(arm_pose);
    const Zivid::Matrix4x4 base_T_cam = mat4_multiply(base_T_flange, hand_eye_transform_);

    {
        std::ostringstream oss;
        oss << std::fixed;
        oss.precision(2);
        oss << "capture: arm x=" << arm_pose.coordinates.x
            << " y=" << arm_pose.coordinates.y << " z=" << arm_pose.coordinates.z
            << " ox=" << arm_pose.orientation.o_x << " oy=" << arm_pose.orientation.o_y
            << " oz=" << arm_pose.orientation.o_z << " theta=" << arm_pose.theta;
        VIAM_RESOURCE_LOG(info) << oss.str();
    }

    ZividCamera* zivid_camera = ZividCamera::find(camera_name_);
    if (!zivid_camera) {
        throw std::runtime_error("ZividStitcher: ZividCamera '" + camera_name_ +
                                 "' not found — is the camera component running?");
    }

    const auto frame = zivid_camera->capture_for_calibration();
    auto cloud = frame.pointCloud()
                      .toUnorganizedPointCloud()
                      .voxelDownsampled(voxel_size_mm_, 1);
    cloud.transform(base_T_cam);
    return cloud;
}

void ZividStitcher::save_cloud_to_ply(const Zivid::UnorganizedPointCloud& cloud,
                                       const std::string& path) const {
    Zivid::Experimental::PointCloudExport::exportUnorganizedPointCloud(
        cloud,
        Zivid::Experimental::PointCloudExport::FileFormat::PLY{
            path,
            Zivid::Experimental::PointCloudExport::FileFormat::PLY::Layout::unordered,
            Zivid::Experimental::PointCloudExport::ColorSpace::sRGB});
}

void ZividStitcher::save_cloud_to_pcd(const Zivid::UnorganizedPointCloud& cloud,
                                       const std::string& path) const {
    // Write a binary PCD in meters (same convention as the camera's encode_pcd).
    // Zivid UnorganizedPointCloud is in mm, so we scale by 1e-3.
    const auto xyz    = cloud.copyPointsXYZ();
    const auto colors = cloud.copyColorsRGBA_SRGB();
    const size_t n    = cloud.size();

    struct PcdPoint { float x, y, z, rgb; };
    std::vector<PcdPoint> pts;
    pts.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& p = xyz(i);
        const auto& c = colors(i);
        uint32_t rgb_packed = (static_cast<uint32_t>(c.r) << 16) |
                              (static_cast<uint32_t>(c.g) <<  8) |
                               static_cast<uint32_t>(c.b);
        float rgb_float{};
        std::memcpy(&rgb_float, &rgb_packed, sizeof(float));
        pts.push_back({p.x * 1e-3f, p.y * 1e-3f, p.z * 1e-3f, rgb_float});
    }

    std::ostringstream hdr;
    hdr << "VERSION 0.7\n"
        << "FIELDS x y z rgb\n"
        << "SIZE 4 4 4 4\n"
        << "TYPE F F F F\n"
        << "COUNT 1 1 1 1\n"
        << "WIDTH " << n << "\n"
        << "HEIGHT 1\n"
        << "VIEWPOINT 0 0 0 1 0 0 0\n"
        << "POINTS " << n << "\n"
        << "DATA binary\n";

    std::ofstream f(path, std::ios::binary);
    if (!f.is_open())
        throw std::runtime_error("ZividStitcher: cannot write PCD to " + path);
    const std::string hdr_str = hdr.str();
    f.write(hdr_str.data(), static_cast<std::streamsize>(hdr_str.size()));
    if (n > 0)
        f.write(reinterpret_cast<const char*>(pts.data()),
                static_cast<std::streamsize>(n * sizeof(PcdPoint)));
}

viam::sdk::ProtoStruct ZividStitcher::cmd_capture_and_accumulate() {
    auto cloud = capture_transformed_cloud();

    std::lock_guard<std::mutex> lock(scan_mutex_);

    if (!accumulated_cloud_) {
        // First capture — just store it.
        accumulated_cloud_ = std::move(cloud);
    } else {
        if (icp_enabled_) {
            // Refine alignment: align accumulated cloud (source) with new cloud (target).
            // Following the Zivid stitch sample pattern: transform accumulated to match new.
            namespace ET = Zivid::Experimental::Toolbox;
            Zivid::Experimental::LocalPointCloudRegistrationParameters params;
            params.set(Zivid::Experimental::LocalPointCloudRegistrationParameters::
                           MaxCorrespondenceDistance{icp_max_correspondence_mm_});

            const auto reg = ET::localPointCloudRegistration(cloud, *accumulated_cloud_, params);
            if (reg.converged()) {
                accumulated_cloud_->transform(reg.transform().toMatrix());
                VIAM_RESOURCE_LOG(info)
                    << "ICP converged, sourceCoverage=" << reg.sourceCoverage()
                    << " rmse=" << reg.rootMeanSquareError();
            } else {
                VIAM_RESOURCE_LOG(warn) << "ICP did not converge — using pose-only alignment";
            }
        }
        accumulated_cloud_->extend(cloud);
    }

    ++accumulated_count_;
    const auto point_count = accumulated_cloud_->size();

    VIAM_RESOURCE_LOG(info) << "accumulated_count=" << accumulated_count_
                             << " point_count=" << point_count;

    viam::sdk::ProtoStruct result;
    result["accumulated_count"] = static_cast<double>(accumulated_count_);
    result["point_count"] = static_cast<double>(point_count);
    return result;
}

// ---------------------------------------------------------------------------
// export
// ---------------------------------------------------------------------------

viam::sdk::ProtoStruct ZividStitcher::cmd_export(const viam::sdk::ProtoStruct& /*args*/) {
    std::lock_guard<std::mutex> lock(scan_mutex_);

    if (!accumulated_cloud_ || accumulated_cloud_->size() == 0) {
        throw std::runtime_error(
            "ZividStitcher: no captured data to export. Run capture_and_accumulate first.");
    }

    // Final voxel downsample to remove overlap artifacts from merging.
    auto export_cloud = accumulated_cloud_->voxelDownsampled(voxel_size_mm_, 1);

    const std::string output_path =
        save_dir_ + "/zivid_scan_" + timestamp_for_filename() + ".ply";

    save_cloud_to_ply(export_cloud, output_path);

    VIAM_RESOURCE_LOG(info) << "Exported " << export_cloud.size() << " points to "
                             << output_path;

    viam::sdk::ProtoStruct result;
    result["output_file"] = output_path;
    result["point_count"] = static_cast<double>(export_cloud.size());
    return result;
}

}  // namespace viam_zivid
