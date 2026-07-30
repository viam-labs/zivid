#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <Zivid/Calibration/DetectionResultFiducialMarkers.h>
#include <Zivid/Calibration/HandEye.h>
#include <Zivid/Matrix.h>
#include <Zivid/Point.h>

#include <viam/sdk/components/arm.hpp>
#include <viam/sdk/config/resource.hpp>
#include <viam/sdk/resource/resource.hpp>
#include <viam/sdk/services/generic.hpp>

namespace viam_zivid {

class ZividCamera;

class ZividHandEyeCalibration : public viam::sdk::GenericService {
   public:
    ZividHandEyeCalibration(viam::sdk::Dependencies deps, const viam::sdk::ResourceConfig& cfg);

    viam::sdk::ProtoStruct do_command(const viam::sdk::ProtoStruct& command) override;
    viam::sdk::ProtoStruct get_status() override;

   private:
    struct SessionInput {
        Zivid::Matrix4x4 robot_pose;
        std::optional<Zivid::Calibration::HandEyeInput> input;  // always set; optional avoids default-ctor requirement
        std::string detection_mode;                             // "calibration_board" | "markers"
        std::optional<Zivid::PointXYZ> centroid;
        std::optional<std::vector<int>> detected_marker_ids;
    };

    viam::sdk::ProtoStruct cmd_capture_and_detect(const viam::sdk::ProtoStruct& args);
    viam::sdk::ProtoStruct cmd_calibrate_eye_in_hand();
    viam::sdk::ProtoStruct cmd_reset_calibration();

    std::string save_to_file(const Zivid::Calibration::HandEyeOutput& result) const;

    std::shared_ptr<viam::sdk::Arm> arm_;
    ZividCamera* zivid_camera_;  // non-owning; lifetime governed by Viam dependency system
    std::string arm_name_;
    std::string camera_serial_;
    std::string save_dir_;
    std::string marker_dictionary_name_;
    std::string last_detection_mode_;  // tracks mode used across session for save_to_file

    std::mutex session_mutex_;
    std::vector<SessionInput> session_inputs_;
};

}  // namespace viam_zivid
