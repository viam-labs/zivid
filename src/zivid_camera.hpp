#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <Zivid/Application.h>
#include <Zivid/Camera.h>
#include <Zivid/Frame.h>
#include <Zivid/Settings.h>
#include <Zivid/Settings2D.h>

#include <viam/sdk/components/camera.hpp>
#include <viam/sdk/config/resource.hpp>
#include <viam/sdk/resource/resource.hpp>

namespace viam_zivid {

struct AcquisitionConfig {
    std::optional<double> aperture;
    std::optional<double> brightness;
    std::optional<double> exposure_time_us;
    std::optional<double> gain;
};

struct Point3D {
    double x, y, z;
};

struct BoxRoiConfig {
    Point3D point_o;
    Point3D point_a;
    Point3D point_b;
    double extents_min;
    double extents_max;
};

struct DepthRoiConfig {
    double min;
    double max;
};

struct Config {
    std::optional<std::string> serial_number;
    std::vector<AcquisitionConfig> acquisitions;
    std::optional<std::string> engine;
    std::optional<BoxRoiConfig> box_roi;
    std::optional<DepthRoiConfig> depth_roi;
};

Config parse_config(const viam::sdk::ResourceConfig& cfg);

class ZividCamera : public viam::sdk::Camera {
   public:
    ZividCamera(std::shared_ptr<Zivid::Application> app,
                viam::sdk::Dependencies deps,
                const viam::sdk::ResourceConfig& cfg);

    ~ZividCamera() override;

    viam::sdk::Camera::image_collection get_images(std::vector<std::string> filter_source_names,
                                                   const viam::sdk::ProtoStruct& extra) override;

    viam::sdk::Camera::point_cloud get_point_cloud(std::string mime_type,
                                                   const viam::sdk::ProtoStruct& extra) override;

    viam::sdk::Camera::properties get_properties() override;

    std::vector<viam::sdk::GeometryConfig> get_geometries(
        const viam::sdk::ProtoStruct& extra) override;

    viam::sdk::ProtoStruct do_command(const viam::sdk::ProtoStruct& command) override;

    viam::sdk::ProtoStruct get_status() override;

   private:
    // Returns the cached frame if it is younger than kFrameCacheTtl, otherwise
    // triggers a new capture and updates the cache.
    Zivid::Frame get_or_capture();

    static constexpr std::chrono::milliseconds kFrameCacheTtl{500};

    std::shared_ptr<Zivid::Application> app_;
    Zivid::Camera camera_;
    Zivid::Settings settings_;

    std::mutex capture_mutex_;
    std::condition_variable capture_cv_;
    bool capturing_{false};
    std::optional<Zivid::Frame> cached_frame_;
    std::chrono::steady_clock::time_point cached_frame_time_;
};

}  // namespace viam_zivid
