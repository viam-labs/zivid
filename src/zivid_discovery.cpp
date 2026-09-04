#include "zivid_discovery.hpp"

#include <stdexcept>

#include <Zivid/CameraInfo.h>
#include <Zivid/CameraState.h>

#include <viam/sdk/log/logging.hpp>

#include "zivid_locks.hpp"

namespace viam_zivid {

ZividDiscovery::ZividDiscovery(std::shared_ptr<Zivid::Application> app,
                               viam::sdk::Dependencies /*deps*/,
                               const viam::sdk::ResourceConfig& cfg)
    : viam::sdk::Discovery(cfg.name()), app_(std::move(app)) {}

std::vector<viam::sdk::ResourceConfig> ZividDiscovery::discover_resources(const viam::sdk::ProtoStruct& /*extra*/) {
    std::vector<viam::sdk::ResourceConfig> configs;

    // Held across the whole scan: querying each camera's info/state counts as operating
    // it, which Zivid forbids while any thread is enumerating or connecting.
    std::lock_guard<std::mutex> device_guard(device_lock());

    std::vector<Zivid::Camera> cameras;
    try {
        cameras = app_->cameras();
    } catch (const std::exception& e) {
        VIAM_RESOURCE_LOG(error) << "failed to enumerate cameras: " << e.what();
        return configs;
    }

    for (auto& camera : cameras) {
        try {
            const auto info = camera.info();
            const auto state = camera.state();

            const std::string serial = info.serialNumber().value();
            const std::string model_name = info.modelName().value();
            const auto status = state.status().value();

            // Only surface cameras that can actually be connected to.
            if (status != Zivid::CameraState::Status::ValueType::available &&
                status != Zivid::CameraState::Status::ValueType::firmwareUpdateRequired) {
                VIAM_RESOURCE_LOG(info) << "skipping " << serial << " (status: " << state.status().toString() << ")";
                continue;
            }

            viam::sdk::ProtoStruct attrs;
            attrs["serial_number"] = serial;
            attrs["model_name"] = model_name;

            if (status == Zivid::CameraState::Status::ValueType::firmwareUpdateRequired) {
                attrs["firmware_update_required"] = true;
            }

            const std::string name = "zivid-" + serial;

            configs.push_back(viam::sdk::ResourceConfig{
                "camera", name, "rdk", std::move(attrs), "rdk:component:camera", viam::sdk::Model{"viam", "camera", "zivid"}});

            VIAM_RESOURCE_LOG(info) << "found " << model_name << " S/N " << serial;
        } catch (const std::exception& e) {
            VIAM_RESOURCE_LOG(warn) << "error processing camera: " << e.what();
        }
    }

    VIAM_RESOURCE_LOG(info) << "discovered " << configs.size() << " camera(s)";
    return configs;
}

viam::sdk::ProtoStruct ZividDiscovery::do_command(const viam::sdk::ProtoStruct& /*command*/) {
    return {};
}

viam::sdk::ProtoStruct ZividDiscovery::get_status() {
    return {};
}

}  // namespace viam_zivid
