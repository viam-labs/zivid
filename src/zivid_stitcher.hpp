#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <Zivid/Matrix.h>
#include <Zivid/UnorganizedPointCloud.h>

#include <viam/sdk/components/arm.hpp>
#include <viam/sdk/config/resource.hpp>
#include <viam/sdk/resource/resource.hpp>
#include <viam/sdk/services/generic.hpp>

namespace viam_zivid {

class ZividCamera;

struct ScanPose {
    double x, y, z;            // mm
    double ox, oy, oz, theta;  // OV orientation (degrees)
};

class ZividStitcher : public viam::sdk::GenericService {
   public:
    ZividStitcher(viam::sdk::Dependencies deps, const viam::sdk::ResourceConfig& cfg);

    viam::sdk::ProtoStruct do_command(const viam::sdk::ProtoStruct& command) override;
    viam::sdk::ProtoStruct get_status() override;

   private:
    viam::sdk::ProtoStruct cmd_run_scan();
    viam::sdk::ProtoStruct cmd_reset_scan();
    viam::sdk::ProtoStruct cmd_capture_and_accumulate();
    viam::sdk::ProtoStruct cmd_export(const viam::sdk::ProtoStruct& args);

    // Captures one frame, transforms to base frame, and returns the cloud.
    Zivid::UnorganizedPointCloud capture_transformed_cloud();
    void save_cloud_to_ply(const Zivid::UnorganizedPointCloud& cloud, const std::string& path) const;
    void save_cloud_to_pcd(const Zivid::UnorganizedPointCloud& cloud, const std::string& path) const;

    std::shared_ptr<viam::sdk::Arm> arm_;
    std::string camera_name_;  // looked up lazily via ZividCamera::find() at capture time

    Zivid::Matrix4x4 hand_eye_transform_;  // flange_T_cam loaded from hand_eye_json
    std::optional<Zivid::UnorganizedPointCloud> accumulated_cloud_;
    size_t accumulated_count_{0};

    std::mutex scan_mutex_;

    std::vector<ScanPose> scan_poses_;

    std::string save_dir_;
    float voxel_size_mm_{1.0f};
    bool icp_enabled_{true};
    float icp_max_correspondence_mm_{2.0f};
    double settle_delay_s_{2.0};
    bool save_per_pose_clouds_{false};
};

}  // namespace viam_zivid
