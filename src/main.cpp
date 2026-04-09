#include <iostream>
#include <memory>
#include <vector>

#include <Zivid/Application.h>
#include <Zivid/VersionConstants.h>

#include <viam/sdk/common/instance.hpp>
#include <viam/sdk/components/camera.hpp>
#include <viam/sdk/module/service.hpp>
#include <viam/sdk/registry/registry.hpp>
#include <viam/sdk/resource/resource_api.hpp>
#include <viam/sdk/services/discovery.hpp>

#include "zivid_camera.hpp"
#include "zivid_discovery.hpp"
#include "zivid_handeye.hpp"

int main(int argc, char** argv) {
    viam::sdk::Instance inst;

    std::cout << "viam-camera-zivid | Zivid SDK " << ZIVID_CORE_VERSION << "\n";

    auto app = std::make_shared<Zivid::Application>();

    auto camera_reg = std::make_shared<viam::sdk::ModelRegistration>(
        viam::sdk::API::get<viam::sdk::Camera>(),
        viam::sdk::Model{"viam", "camera", "zivid"},
        [app](viam::sdk::Dependencies deps, viam::sdk::ResourceConfig cfg) {
            return std::make_shared<viam_zivid::ZividCamera>(app, std::move(deps), cfg);
        });

    auto discovery_reg = std::make_shared<viam::sdk::ModelRegistration>(
        viam::sdk::API::get<viam::sdk::Discovery>(),
        viam::sdk::Model{"viam", "zivid", "discovery"},
        [app](viam::sdk::Dependencies deps, viam::sdk::ResourceConfig cfg) {
            return std::make_shared<viam_zivid::ZividDiscovery>(app, std::move(deps), cfg);
        });

    auto handeye_reg = std::make_shared<viam::sdk::ModelRegistration>(
        viam::sdk::API::get<viam::sdk::GenericService>(),
        viam::sdk::Model{"viam", "zivid", "handeye-calibration"},
        [](viam::sdk::Dependencies deps, viam::sdk::ResourceConfig cfg) {
            return std::make_shared<viam_zivid::ZividHandEyeCalibration>(std::move(deps), cfg);
        });

    auto service = std::make_shared<viam::sdk::ModuleService>(
        argc, argv,
        std::vector<std::shared_ptr<viam::sdk::ModelRegistration>>{
            camera_reg, discovery_reg, handeye_reg});

    service->serve();
    return 0;
}
