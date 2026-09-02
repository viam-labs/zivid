#include "zivid_discovery.hpp"

#include <cmath>
#include <sstream>
#include <stdexcept>

#include <Zivid/Camera.h>
#include <Zivid/CameraInfo.h>
#include <Zivid/CameraState.h>
#include <Zivid/Firmware.h>
#include <Zivid/VersionConstants.h>

#include <viam/sdk/log/logging.hpp>

namespace viam_zivid {

namespace {

// Zivid::Firmware::update() refuses a camera that is connected, so flashing is
// only possible from these two states. Everything else is reported back with an
// explanation rather than letting the SDK fail mid-run.
//
// Returns an empty string when the camera can be flashed, otherwise the reason
// it can't, phrased as something the operator can act on.
std::string blocked_reason(Zivid::CameraState::Status::ValueType status) {
    using Status = Zivid::CameraState::Status::ValueType;
    switch (status) {
        case Status::available:
        case Status::firmwareUpdateRequired:
            return "";
        case Status::connected:
        case Status::connecting:
        case Status::disconnecting:
            return "the camera is connected in this module — remove or disable the viam:zivid:camera component configured for this "
                   "serial number, then retry";
        case Status::busy:
            return "the camera is in use by another process — close Zivid Studio and stop any other program using it, then retry";
        case Status::updatingFirmware:
            return "a firmware update is already running on this camera — wait for it to finish";
        case Status::applyingNetworkConfiguration:
            return "the camera is applying a network configuration — wait for it to settle, then retry";
        case Status::inaccessible:
            return "the camera cannot be reached — check its network configuration (run `ZividListCameras` for details)";
        case Status::disappeared:
            return "the camera dropped off the bus — check power and cabling";
        default:
            return "the camera is not in a state that allows flashing";
    }
}

struct CameraPlan {
    size_t index;  // position in the Zivid::Application camera vector
    std::string serial;
    std::string model_name;
    std::string firmware_version;
    std::string status;
    bool needs_update;
    std::string blocked;  // empty when the camera can be flashed
};

std::string describe(const CameraPlan& p) {
    return p.model_name + " S/N " + p.serial + " (firmware " + p.firmware_version + ", status: " + p.status + ")";
}

// Inspects every attached camera. Cameras that can't be inspected at all are
// reported through `problems` instead of being silently dropped: a camera we
// can't read is a camera we can't promise anything about.
std::vector<CameraPlan> build_plan(std::vector<Zivid::Camera>& cameras, std::vector<std::string>& problems) {
    std::vector<CameraPlan> plan;

    for (size_t i = 0; i < cameras.size(); ++i) {
        auto& camera = cameras[i];
        try {
            const auto info = camera.info();
            const auto state = camera.state();

            CameraPlan p;
            p.index = i;
            p.serial = info.serialNumber().value();
            p.model_name = info.modelName().value();
            p.firmware_version = info.firmwareVersion().value();
            p.status = state.status().toString();
            p.needs_update = !Zivid::Firmware::isUpToDate(camera);
            p.blocked = blocked_reason(state.status().value());

            plan.push_back(std::move(p));
        } catch (const std::exception& e) {
            problems.emplace_back(std::string{"could not inspect a camera: "} + e.what());
        }
    }

    return plan;
}

void append_problems(std::ostringstream& oss, const std::vector<std::string>& problems) {
    if (problems.empty()) {
        return;
    }
    oss << "\n\nWarning:\n";
    for (const auto& problem : problems) {
        oss << "  - " << problem << "\n";
    }
}

// Message for the case where no camera can or needs to be flashed. Shared by
// the preflight and the confirmed run, which reach it identically.
std::string build_nothing_to_do_message(const std::vector<CameraPlan>& plan, const std::vector<std::string>& problems) {
    std::ostringstream oss;

    if (plan.empty()) {
        oss << "update_firmware: no Zivid cameras detected. Check that the camera is powered, plugged in, and visible to "
               "the host (run `ZividListCameras`), then try again.";
    } else {
        oss << "update_firmware: nothing to do. All " << plan.size() << " camera(s) already run the firmware required by Zivid SDK "
            << ZIVID_CORE_VERSION << ":\n";
        for (const auto& p : plan) {
            oss << "  - " << describe(p) << "\n";
        }
    }

    append_problems(oss, problems);
    return oss.str();
}

std::string build_preflight_message(const std::vector<CameraPlan>& plan, const std::vector<std::string>& problems) {
    std::vector<const CameraPlan*> to_update;
    std::vector<const CameraPlan*> blocked;
    std::vector<const CameraPlan*> up_to_date;

    for (const auto& p : plan) {
        if (!p.needs_update) {
            up_to_date.push_back(&p);
        } else if (p.blocked.empty()) {
            to_update.push_back(&p);
        } else {
            blocked.push_back(&p);
        }
    }

    if (to_update.empty() && blocked.empty()) {
        return build_nothing_to_do_message(plan, problems);
    }

    std::ostringstream oss;
    oss << "update_firmware requires confirmation — NOTHING HAS BEEN CHANGED.\n\n";

    if (!to_update.empty()) {
        oss << "The following " << to_update.size() << " camera(s) WILL BE UPDATED to the firmware required by Zivid SDK "
            << ZIVID_CORE_VERSION << ":\n";
        for (const auto* p : to_update) {
            oss << "  - " << describe(*p) << "\n";
        }
    }

    if (!blocked.empty()) {
        oss << (to_update.empty() ? "" : "\n");
        oss << "The following " << blocked.size() << " camera(s) need an update but CANNOT be flashed in their current state:\n";
        for (const auto* p : blocked) {
            oss << "  - " << describe(*p) << " — " << p->blocked << "\n";
        }
    }

    if (!up_to_date.empty()) {
        oss << "\nAlready up to date (will be left alone):\n";
        for (const auto* p : up_to_date) {
            oss << "  - " << describe(*p) << "\n";
        }
    }

    oss << "\nBefore confirming:\n"
           "  1. Remove or disable every viam:zivid:camera component configured for the camera(s) to be updated, and close Zivid Studio "
           "and any other process using them. A camera cannot be flashed while anything holds a connection to it.\n"
           "  2. Keep the camera powered and connected for the whole update. Interrupting a firmware update can leave the camera "
           "unusable.\n"
           "  3. Expect several minutes per camera. This call blocks until every camera is done, so use a long client timeout. If your "
           "client times out anyway the update keeps running inside the module — watch the machine logs and re-run this command to "
           "check the result.\n"
           "  4. After the update the camera reboots. Re-add the viam:zivid:camera component (or re-run discovery) once it is back.\n";

    oss << "\nTo proceed, send:\n"
           "  {\"update_firmware\": {\"confirm\": true}}";

    append_problems(oss, problems);
    return oss.str();
}

}  // namespace

ZividDiscovery::ZividDiscovery(std::shared_ptr<Zivid::Application> app,
                               viam::sdk::Dependencies /*deps*/,
                               const viam::sdk::ResourceConfig& cfg)
    : viam::sdk::Discovery(cfg.name()), app_(std::move(app)) {}

std::vector<viam::sdk::ResourceConfig> ZividDiscovery::discover_resources(const viam::sdk::ProtoStruct& /*extra*/) {
    std::vector<viam::sdk::ResourceConfig> configs;

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

viam::sdk::ProtoStruct ZividDiscovery::do_command(const viam::sdk::ProtoStruct& command) {
    auto it = command.find("update_firmware");
    if (it == command.end()) {
        throw std::invalid_argument("ZividDiscovery: unknown command. Valid: update_firmware");
    }

    return cmd_update_firmware(it->second);
}

viam::sdk::ProtoStruct ZividDiscovery::cmd_update_firmware(const viam::sdk::ProtoValue& arg) {
    bool confirmed = false;
    if (const auto* args = arg.get<viam::sdk::ProtoStruct>()) {
        const auto confirm_it = args->find("confirm");
        if (confirm_it != args->end()) {
            if (const bool* value = confirm_it->second.get<bool>()) {
                confirmed = *value;
            }
        }
    }

    std::unique_lock<std::mutex> lock(firmware_update_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        throw std::runtime_error(
            "update_firmware: a firmware update is already running. Wait for it to finish — progress is reported in the "
            "machine logs.");
    }

    std::vector<Zivid::Camera> cameras;
    try {
        cameras = app_->cameras();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string{"update_firmware: failed to enumerate cameras: "} + e.what());
    }

    std::vector<std::string> problems;
    const auto plan = build_plan(cameras, problems);

    if (!confirmed) {
        throw std::invalid_argument(build_preflight_message(plan, problems));
    }

    std::vector<std::string> updated;
    std::vector<std::string> failed;
    viam::sdk::ProtoList updated_list;
    viam::sdk::ProtoList skipped_list;

    for (const auto& p : plan) {
        if (!p.needs_update) {
            VIAM_RESOURCE_LOG(info) << "firmware already up to date on S/N " << p.serial << ", skipping";
            skipped_list.emplace_back(p.serial + ": already up to date");
            continue;
        }
        if (!p.blocked.empty()) {
            VIAM_RESOURCE_LOG(warn) << "cannot update firmware on S/N " << p.serial << ": " << p.blocked;
            failed.push_back(describe(p) + " — " + p.blocked);
            continue;
        }

        VIAM_RESOURCE_LOG(info) << "updating firmware on " << describe(p) << " — do not power off or unplug the camera";
        try {
            Zivid::Firmware::update(cameras[p.index], [this, &p](double progress_percentage, const std::string& stage) {
                VIAM_RESOURCE_LOG(info) << "firmware update S/N " << p.serial << ": " << static_cast<int>(std::round(progress_percentage))
                                        << "% — " << stage;
            });
            VIAM_RESOURCE_LOG(info) << "firmware update complete on S/N " << p.serial << "; the camera is rebooting";
            updated.push_back(p.serial);
            updated_list.emplace_back(p.serial);
        } catch (const std::exception& e) {
            VIAM_RESOURCE_LOG(error) << "firmware update failed on S/N " << p.serial << ": " << e.what();
            failed.push_back(describe(p) + " — " + e.what());
        }
    }

    if (updated.empty() && failed.empty()) {
        // Confirmed, but there was nothing eligible to flash. Say so rather
        // than returning an empty success.
        throw std::runtime_error(build_nothing_to_do_message(plan, problems));
    }

    if (!failed.empty()) {
        std::ostringstream oss;
        oss << "update_firmware: " << failed.size() << " camera(s) were NOT updated:\n";
        for (const auto& failure : failed) {
            oss << "  - " << failure << "\n";
        }
        if (!updated.empty()) {
            oss << "\nSuccessfully updated " << updated.size() << " camera(s):\n";
            for (const auto& serial : updated) {
                oss << "  - S/N " << serial << "\n";
            }
        }
        append_problems(oss, problems);
        throw std::runtime_error(oss.str());
    }

    viam::sdk::ProtoStruct result;
    result["updated"] = std::move(updated_list);
    result["skipped"] = std::move(skipped_list);
    if (!problems.empty()) {
        viam::sdk::ProtoList problem_list;
        for (const auto& problem : problems) {
            problem_list.emplace_back(problem);
        }
        result["warnings"] = std::move(problem_list);
    }
    result["message"] = "Updated firmware on " + std::to_string(updated.size()) +
                        " camera(s). Each updated camera is rebooting — wait for it to come back, then re-run discovery or re-add its "
                        "viam:zivid:camera component.";
    return result;
}

viam::sdk::ProtoStruct ZividDiscovery::get_status() {
    return {};
}

}  // namespace viam_zivid
