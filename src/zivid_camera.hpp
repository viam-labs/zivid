#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

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

// Maps to Zivid::Settings::Processing::Filters::Noise::Removal.
// Raising the threshold discards more low-confidence (noisy/floating) points;
// lowering it keeps more data. Zivid SDK default is enabled at threshold ~7.
struct NoiseRemovalConfig {
    std::optional<bool> enabled;
    std::optional<double> threshold;
};

struct ProcessingConfig {
    std::optional<NoiseRemovalConfig> noise_removal;
};

struct Config {
    std::optional<std::string> serial_number;
    std::vector<AcquisitionConfig> acquisitions;     // 3D capture
    std::vector<AcquisitionConfig> acquisitions_2d;  // 2D color capture
    std::optional<std::string> engine;
    std::optional<BoxRoiConfig> box_roi;
    std::optional<DepthRoiConfig> depth_roi;
    std::optional<ProcessingConfig> processing;
    // Maps to Zivid::Settings::Sampling::Pixel — subsamples/bins the 3D sensor
    // readout. Lower resolution means fewer points and faster capture+processing.
    // Valid: all, by2x2, by4x4, blueSubsample2x2, blueSubsample4x4,
    // redSubsample2x2, redSubsample4x4. Defaults to the SDK default (all) when unset.
    std::optional<std::string> pixel_sampling;
    // Maps to Zivid::Settings2D::Sampling::Pixel — resolution of the 2D color
    // capture (and the color baked into the point cloud). Same valid values.
    std::optional<std::string> color_pixel_sampling;
};

Config parse_config(const viam::sdk::ResourceConfig& cfg);

class ZividCamera : public viam::sdk::Camera {
   public:
    ZividCamera(std::shared_ptr<Zivid::Application> app, viam::sdk::Dependencies deps, const viam::sdk::ResourceConfig& cfg);

    ~ZividCamera() override;

    viam::sdk::Camera::image_collection get_images(std::vector<std::string> filter_source_names,
                                                   const viam::sdk::ProtoStruct& extra) override;

    viam::sdk::Camera::point_cloud get_point_cloud(std::string mime_type, const viam::sdk::ProtoStruct& extra) override;

    viam::sdk::Camera::properties get_properties() override;

    std::vector<viam::sdk::GeometryConfig> get_geometries(const viam::sdk::ProtoStruct& extra) override;

    viam::sdk::ProtoStruct do_command(const viam::sdk::ProtoStruct& command) override;

    viam::sdk::ProtoStruct get_status() override;

    // Returns a fresh frame captured with the current settings, bypassing the cache.
    // Used by ZividHandEyeCalibration.
    Zivid::Frame capture_for_calibration();

    // Returns the camera serial number string.
    std::string serial_number() const;

    // Registry of live ZividCamera instances keyed by resource name.
    // Used by ZividHandEyeCalibration to obtain the real object, since Viam passes
    // gRPC proxy objects as dependencies rather than the actual C++ instances.
    static ZividCamera* find(const std::string& name);

   private:
    static std::mutex registry_mutex_;
    static std::unordered_map<std::string, ZividCamera*> registry_;
    // Returns the cached frame if it is younger than kFrameCacheTtl, otherwise
    // triggers a new capture and updates the cache.
    Zivid::Frame get_or_capture();

    static constexpr std::chrono::milliseconds kFrameCacheTtl{500};

    std::shared_ptr<Zivid::Application> app_;
    Zivid::Camera camera_;
    Zivid::Settings settings_;
    // The 2D color settings embedded in settings_, kept separately so
    // get_properties() can compute (capture-free) the base intrinsics + base
    // resolution that are then scaled to the served 2D color image resolution.
    Zivid::Settings2D settings_2d_;

    std::mutex capture_mutex_;
    std::condition_variable capture_cv_;
    bool capturing_{false};
    std::optional<Zivid::Frame> cached_frame_;
    std::chrono::steady_clock::time_point cached_frame_time_;
};

}  // namespace viam_zivid
