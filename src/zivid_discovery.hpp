#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <Zivid/Application.h>

#include <viam/sdk/config/resource.hpp>
#include <viam/sdk/resource/resource.hpp>
#include <viam/sdk/services/discovery.hpp>

namespace viam_zivid {

class ZividDiscovery : public viam::sdk::Discovery {
   public:
    ZividDiscovery(std::shared_ptr<Zivid::Application> app, viam::sdk::Dependencies deps, const viam::sdk::ResourceConfig& cfg);

    std::vector<viam::sdk::ResourceConfig> discover_resources(const viam::sdk::ProtoStruct& extra) override;

    viam::sdk::ProtoStruct do_command(const viam::sdk::ProtoStruct& command) override;

    viam::sdk::ProtoStruct get_status() override;

   private:
    viam::sdk::ProtoStruct cmd_update_firmware(const viam::sdk::ProtoValue& arg);

    std::shared_ptr<Zivid::Application> app_;

    // Flashing is destructive and exclusive: serialize update runs so two
    // concurrent DoCommands can't target the same camera.
    std::mutex firmware_update_mutex_;
};

}  // namespace viam_zivid
