#include "zivid_handeye.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <stdexcept>

#include <Zivid/Calibration/Detector.h>
#include <Zivid/Calibration/MarkerDictionary.h>
#include <Zivid/Calibration/Pose.h>
#include <viam/sdk/components/camera.hpp>
#include <viam/sdk/log/logging.hpp>

#include "pose_utils.hpp"
#include "zivid_camera.hpp"

namespace viam_zivid {

namespace {

// ---------------------------------------------------------------------------
// JSON helpers (hand-rolled, no external library)
// ---------------------------------------------------------------------------

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

std::string iso8601_now() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return std::string(buf);
}

std::string timestamp_for_filename() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", std::gmtime(&t));
    return std::string(buf);
}

std::string matrix4x4_to_json_array(const Zivid::Matrix4x4& mat) {
    std::ostringstream ss;
    ss << "[";
    for (size_t row = 0; row < 4; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            if (row > 0 || col > 0) ss << ",";
            ss << static_cast<double>(mat(row, col));
        }
    }
    ss << "]";
    return ss.str();
}

// ---------------------------------------------------------------------------
// Residual stats helpers
// ---------------------------------------------------------------------------

struct ResidualStats {
    double rotation_mean, rotation_max, rotation_std;
    double translation_mean, translation_max, translation_std;
};

ResidualStats compute_residual_stats(
    const std::vector<Zivid::Calibration::HandEyeResidual>& residuals) {
    const size_t n = residuals.size();

    std::vector<double> rots(n), trans(n);
    for (size_t i = 0; i < n; ++i) {
        rots[i] = static_cast<double>(residuals[i].rotation());
        trans[i] = static_cast<double>(residuals[i].translation());
    }

    auto mean_of = [](const std::vector<double>& v) {
        return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    };
    auto max_of = [](const std::vector<double>& v) {
        return *std::max_element(v.begin(), v.end());
    };
    auto std_of = [&mean_of](const std::vector<double>& v) {
        const double m = mean_of(v);
        double sq_sum = 0.0;
        for (double x : v) sq_sum += (x - m) * (x - m);
        return std::sqrt(sq_sum / static_cast<double>(v.size()));
    };

    return {mean_of(rots), max_of(rots), std_of(rots),
            mean_of(trans), max_of(trans), std_of(trans)};
}

std::string quality_label(const ResidualStats& s) {
    if (s.rotation_mean < 1.0 && s.translation_mean < 1.0) return "good";
    if (s.rotation_mean < 2.0 && s.translation_mean < 3.0) return "acceptable";
    return "poor";
}

}  // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ZividHandEyeCalibration::ZividHandEyeCalibration(viam::sdk::Dependencies deps,
                                                  const viam::sdk::ResourceConfig& cfg)
    : viam::sdk::GenericService(cfg.name()) {
    const auto& attrs = cfg.attributes();

    auto require_str = [&](const std::string& key) -> std::string {
        auto it = attrs.find(key);
        if (it == attrs.end())
            throw std::invalid_argument("ZividHandEyeCalibration: '" + key + "' attribute required");
        return it->second.get_unchecked<std::string>();
    };

    arm_name_ = require_str("arm");
    const auto camera_name = require_str("camera");

    auto dir_it = attrs.find("save_dir");
    save_dir_ = (dir_it != attrs.end()) ? dir_it->second.get_unchecked<std::string>() : "/var/lib/viam";

    auto dict_it = attrs.find("marker_dictionary");
    marker_dictionary_name_ =
        (dict_it != attrs.end()) ? dict_it->second.get_unchecked<std::string>() : "aruco4x4_50";

    // Wire up arm dependency.
    auto arm_resource = deps.at(viam::sdk::Name{viam::sdk::API::get<viam::sdk::Arm>(), "", arm_name_});
    arm_ = std::dynamic_pointer_cast<viam::sdk::Arm>(arm_resource);
    if (!arm_) {
        throw std::runtime_error("ZividHandEyeCalibration: dependency '" + arm_name_ +
                                 "' is not an Arm component");
    }

    // Wire up camera dependency.
    // Viam passes gRPC proxy objects as dependencies, not the real C++ instances, so
    // dynamic_pointer_cast to ZividCamera would always fail.  Use the static registry
    // that ZividCamera populates on construction instead.
    zivid_camera_ = ZividCamera::find(camera_name);
    if (!zivid_camera_) {
        throw std::runtime_error("ZividHandEyeCalibration: no ZividCamera named '" +
                                 camera_name + "' found in registry — ensure it is listed "
                                 "in depends_on and has finished initialising");
    }
    camera_serial_ = zivid_camera_->serial_number();
}

// ---------------------------------------------------------------------------
// do_command dispatcher
// ---------------------------------------------------------------------------

viam::sdk::ProtoStruct ZividHandEyeCalibration::do_command(
    const viam::sdk::ProtoStruct& command) {
    auto it = command.find("command");
    if (it == command.end()) return {};

    const auto& cmd = it->second.get_unchecked<std::string>();

    if (cmd == "capture_and_detect") return cmd_capture_and_detect(command);
    if (cmd == "calibrate_eye_in_hand") return cmd_calibrate_eye_in_hand();
    if (cmd == "reset_calibration") return cmd_reset_calibration();

    throw std::invalid_argument("ZividHandEyeCalibration: unknown command '" + cmd +
                                "'. Valid: capture_and_detect, calibrate_eye_in_hand, "
                                "reset_calibration");
}

// ---------------------------------------------------------------------------
// capture_and_detect
// ---------------------------------------------------------------------------

viam::sdk::ProtoStruct ZividHandEyeCalibration::cmd_capture_and_detect(
    const viam::sdk::ProtoStruct& args) {
    // Detection mode.
    std::string detection_mode = "calibration_board";
    auto mode_it = args.find("detection_mode");
    if (mode_it != args.end()) {
        detection_mode = mode_it->second.get_unchecked<std::string>();
    }
    if (detection_mode != "calibration_board" && detection_mode != "markers") {
        throw std::invalid_argument(
            "detection_mode must be 'calibration_board' or 'markers'");
    }

    // Read arm end-effector pose and convert to Zivid Matrix4x4.
    const auto viam_pose = arm_->get_end_position();
    const Zivid::Matrix4x4 robot_pose = viam_pose_to_zivid(viam_pose);

    // Log arm pose and resulting rotation matrix column for debugging.
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4);
        oss << "arm pose: x=" << viam_pose.coordinates.x
            << " y=" << viam_pose.coordinates.y
            << " z=" << viam_pose.coordinates.z
            << " ox=" << viam_pose.orientation.o_x
            << " oy=" << viam_pose.orientation.o_y
            << " oz=" << viam_pose.orientation.o_z
            << " theta=" << viam_pose.theta
            << " | R col2: [" << robot_pose(0,2) << ", " << robot_pose(1,2) << ", " << robot_pose(2,2) << "]"
            << " t: [" << robot_pose(0,3) << ", " << robot_pose(1,3) << ", " << robot_pose(2,3) << "]";
        VIAM_RESOURCE_LOG(info) << oss.str();
    }

    // Capture a fresh frame.
    Zivid::Frame frame = zivid_camera_->capture_for_calibration();

    viam::sdk::ProtoStruct result;

    if (detection_mode == "calibration_board") {
        const auto detection = Zivid::Calibration::detectCalibrationBoard(frame);

        result["detected"] = detection.valid();
        result["status_description"] = detection.statusDescription();

        if (!detection.valid()) {
            result["accumulated_count"] = static_cast<double>(session_inputs_.size());
            return result;
        }

        const auto centroid = detection.centroid();
        viam::sdk::ProtoStruct centroid_struct;
        centroid_struct["x"] = static_cast<double>(centroid.x);
        centroid_struct["y"] = static_cast<double>(centroid.y);
        centroid_struct["z"] = static_cast<double>(centroid.z);
        result["centroid"] = viam::sdk::ProtoValue{std::move(centroid_struct)};

        SessionInput si;
        si.robot_pose = robot_pose;
        si.input.emplace(Zivid::Calibration::Pose{robot_pose}, detection);
        si.detection_mode = detection_mode;
        si.centroid = centroid;

        std::lock_guard<std::mutex> lock(session_mutex_);
        last_detection_mode_ = detection_mode;
        session_inputs_.push_back(std::move(si));
        result["accumulated_count"] = static_cast<double>(session_inputs_.size());

    } else {
        // Markers mode.
        std::vector<int> marker_ids;
        auto ids_it = args.find("marker_ids");
        if (ids_it != args.end()) {
            for (const auto& v : ids_it->second.get_unchecked<viam::sdk::ProtoList>()) {
                marker_ids.push_back(static_cast<int>(v.get_unchecked<double>()));
            }
        }
        if (marker_ids.empty()) {
            throw std::invalid_argument(
                "capture_and_detect with detection_mode='markers' requires 'marker_ids' list");
        }

        const auto dict = Zivid::Calibration::MarkerDictionary::fromString(marker_dictionary_name_);
        const auto detection = Zivid::Calibration::detectMarkers(frame, marker_ids, dict);

        result["detected"] = detection.valid();

        if (!detection.valid()) {
            result["accumulated_count"] = static_cast<double>(session_inputs_.size());
            result["status_description"] = std::string("No markers detected");
            return result;
        }

        viam::sdk::ProtoList detected_ids_list;
        std::vector<int> detected_ids;
        for (const auto& marker : detection.detectedMarkers()) {
            detected_ids.push_back(marker.id());
            detected_ids_list.push_back(static_cast<double>(marker.id()));
        }
        result["detected_marker_ids"] = viam::sdk::ProtoValue{std::move(detected_ids_list)};

        SessionInput si;
        si.robot_pose = robot_pose;
        si.input.emplace(Zivid::Calibration::Pose{robot_pose}, detection);
        si.detection_mode = detection_mode;
        si.detected_marker_ids = detected_ids;

        std::lock_guard<std::mutex> lock(session_mutex_);
        last_detection_mode_ = detection_mode;
        session_inputs_.push_back(std::move(si));
        result["accumulated_count"] = static_cast<double>(session_inputs_.size());
    }

    return result;
}

// ---------------------------------------------------------------------------
// calibrate_eye_in_hand
// ---------------------------------------------------------------------------

viam::sdk::ProtoStruct ZividHandEyeCalibration::cmd_calibrate_eye_in_hand() {
    std::vector<Zivid::Calibration::HandEyeInput> inputs;
    std::vector<SessionInput> snapshot;

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        snapshot = session_inputs_;
    }

    if (snapshot.empty()) {
        throw std::runtime_error(
            "calibrate_eye_in_hand: no captured poses. Run capture_and_detect first.");
    }

    for (const auto& s : snapshot) {
        inputs.push_back(*s.input);
    }

    const auto calibration_result = Zivid::Calibration::calibrateEyeInHand(inputs);

    viam::sdk::ProtoStruct result;
    result["valid"] = calibration_result.valid();
    result["num_inputs"] = static_cast<double>(snapshot.size());
    result["diversity_warning"] = snapshot.size() < 10;

    if (!calibration_result.valid()) {
        return result;
    }

    const auto& transform = calibration_result.transform();
    const auto& residuals = calibration_result.residuals();

    // Raw 4×4 transform as a flat list of 16 doubles (row-major).
    viam::sdk::ProtoList transform_list;
    for (size_t row = 0; row < 4; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            transform_list.push_back(static_cast<double>(transform(row, col)));
        }
    }
    result["transform"] = viam::sdk::ProtoValue{std::move(transform_list)};

    // Per-pose residuals.
    viam::sdk::ProtoList residuals_list;
    for (const auto& r : residuals) {
        viam::sdk::ProtoStruct entry;
        entry["rotation_deg"] = static_cast<double>(r.rotation());
        entry["translation_mm"] = static_cast<double>(r.translation());
        residuals_list.push_back(viam::sdk::ProtoValue{std::move(entry)});
    }
    result["residuals"] = viam::sdk::ProtoValue{std::move(residuals_list)};

    // Summary stats.
    const auto stats = compute_residual_stats(residuals);
    viam::sdk::ProtoStruct summary;
    summary["rotation_mean_deg"] = stats.rotation_mean;
    summary["rotation_max_deg"] = stats.rotation_max;
    summary["rotation_std_deg"] = stats.rotation_std;
    summary["translation_mean_mm"] = stats.translation_mean;
    summary["translation_max_mm"] = stats.translation_max;
    summary["translation_std_mm"] = stats.translation_std;
    result["residual_summary"] = viam::sdk::ProtoValue{std::move(summary)};

    result["quality"] = quality_label(stats);

    // Viam frame-system config (matches viam-labs/opencv output format).
    const auto ov = zivid_to_viam_ov(transform);

    viam::sdk::ProtoStruct translation_struct;
    translation_struct["x"] = ov.tx;
    translation_struct["y"] = ov.ty;
    translation_struct["z"] = ov.tz;

    viam::sdk::ProtoStruct ov_value;
    ov_value["x"] = ov.ox;
    ov_value["y"] = ov.oy;
    ov_value["z"] = ov.oz;
    ov_value["th"] = ov.theta_deg;

    viam::sdk::ProtoStruct orientation_struct;
    orientation_struct["type"] = std::string("ov_degrees");
    orientation_struct["value"] = viam::sdk::ProtoValue{std::move(ov_value)};

    viam::sdk::ProtoStruct frame_struct;
    frame_struct["translation"] = viam::sdk::ProtoValue{std::move(translation_struct)};
    frame_struct["orientation"] = viam::sdk::ProtoValue{std::move(orientation_struct)};
    frame_struct["parent"] = arm_name_;
    result["frame"] = viam::sdk::ProtoValue{std::move(frame_struct)};

    // Persist to disk.
    const auto output_file = save_to_file(calibration_result);
    result["output_file"] = output_file;

    return result;
}

// ---------------------------------------------------------------------------
// reset_calibration
// ---------------------------------------------------------------------------

viam::sdk::ProtoStruct ZividHandEyeCalibration::cmd_reset_calibration() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    const auto count = session_inputs_.size();
    session_inputs_.clear();
    last_detection_mode_.clear();

    viam::sdk::ProtoStruct result;
    result["cleared_count"] = static_cast<double>(count);
    return result;
}

// ---------------------------------------------------------------------------
// get_status
// ---------------------------------------------------------------------------

viam::sdk::ProtoStruct ZividHandEyeCalibration::get_status() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    viam::sdk::ProtoStruct status;
    status["accumulated_count"] = static_cast<double>(session_inputs_.size());
    status["diversity_warning"] = session_inputs_.size() < 10;
    return status;
}

// ---------------------------------------------------------------------------
// save_to_file
// ---------------------------------------------------------------------------

std::string ZividHandEyeCalibration::save_to_file(
    const Zivid::Calibration::HandEyeOutput& calibration_result) const {
    const std::string ts = timestamp_for_filename();
    const std::string path = save_dir_ + "/zivid_handeye_" + ts + ".json";

    const auto& transform = calibration_result.transform();
    const auto& residuals = calibration_result.residuals();
    const auto stats = compute_residual_stats(residuals);

    std::ostringstream json;
    json << "{\n";
    json << "  \"timestamp\": \"" << iso8601_now() << "\",\n";
    json << "  \"camera_serial\": \"" << json_escape(camera_serial_) << "\",\n";
    json << "  \"detection_mode\": \"" << json_escape(last_detection_mode_) << "\",\n";
    json << "  \"marker_dictionary\": \"" << json_escape(marker_dictionary_name_) << "\",\n";
    json << "  \"num_inputs\": " << session_inputs_.size() << ",\n";
    json << "  \"inputs\": [\n";

    for (size_t i = 0; i < session_inputs_.size(); ++i) {
        const auto& s = session_inputs_[i];
        json << "    {\n";
        json << "      \"robot_pose_matrix\": " << matrix4x4_to_json_array(s.robot_pose) << ",\n";
        json << "      \"detection_mode\": \"" << json_escape(s.detection_mode) << "\",\n";
        json << "      \"detected\": true";
        if (s.centroid) {
            json << ",\n      \"centroid\": {\"x\": " << s.centroid->x
                 << ", \"y\": " << s.centroid->y << ", \"z\": " << s.centroid->z << "}";
        }
        if (s.detected_marker_ids) {
            json << ",\n      \"detected_marker_ids\": [";
            for (size_t j = 0; j < s.detected_marker_ids->size(); ++j) {
                if (j > 0) json << ", ";
                json << (*s.detected_marker_ids)[j];
            }
            json << "]";
        }
        json << "\n    }";
        if (i + 1 < session_inputs_.size()) json << ",";
        json << "\n";
    }

    json << "  ],\n";
    json << "  \"result\": {\n";
    json << "    \"valid\": true,\n";
    json << "    \"transform\": " << matrix4x4_to_json_array(transform) << ",\n";
    json << "    \"residuals\": [\n";
    for (size_t i = 0; i < residuals.size(); ++i) {
        json << "      {\"rotation_deg\": " << residuals[i].rotation()
             << ", \"translation_mm\": " << residuals[i].translation() << "}";
        if (i + 1 < residuals.size()) json << ",";
        json << "\n";
    }
    json << "    ],\n";
    json << "    \"residual_summary\": {\n";
    json << "      \"rotation_mean_deg\": " << stats.rotation_mean << ",\n";
    json << "      \"rotation_max_deg\": " << stats.rotation_max << ",\n";
    json << "      \"rotation_std_deg\": " << stats.rotation_std << ",\n";
    json << "      \"translation_mean_mm\": " << stats.translation_mean << ",\n";
    json << "      \"translation_max_mm\": " << stats.translation_max << ",\n";
    json << "      \"translation_std_mm\": " << stats.translation_std << "\n";
    json << "    },\n";
    json << "    \"quality\": \"" << quality_label(stats) << "\",\n";
    json << "    \"diversity_warning\": " << (session_inputs_.size() < 10 ? "true" : "false") << "\n";
    json << "  }\n";
    json << "}\n";

    std::ofstream file(path);
    if (!file) {
        throw std::runtime_error("ZividHandEyeCalibration: failed to open '" + path +
                                 "' for writing");
    }
    file << json.str();

    return path;
}

}  // namespace viam_zivid
