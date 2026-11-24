#include "DmicMessagePlan.hpp"
#include "generated/DmicStaticData.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace sof::ipc4::dmic {
namespace {

const Module &requireModule(const FirmwareManifest &firmware,
                            const std::string &uuid, const char *expectedName) {
    const auto it = std::find_if(firmware.modules.begin(), firmware.modules.end(),
                                 [&](const Module &module) {
                                     return uuidString(module.uuid) == uuid;
                                 });
    if (it == firmware.modules.end() || it->name != expectedName)
        throw std::runtime_error(std::string("firmware module UUID/name mismatch: ") + expectedName);
    return *it;
}

void requireInstance(const Module &module, uint8_t instance) {
    if (instance > module.instanceMaxCount)
        throw std::invalid_argument("module instance exceeds firmware manifest maximum");
}

std::vector<uint8_t> withControlId(const std::vector<uint8_t> &payload, uint16_t id) {
    if (payload.size() < 20) throw std::runtime_error("short control payload template");
    auto result = payload;
    result[0] = static_cast<uint8_t>(id & 0xffU);
    result[1] = static_cast<uint8_t>(id >> 8);
    return result;
}

} // namespace

MessagePlan buildMessagePlan(const FirmwareManifest &firmware, const RuntimeIds &ids) {
    constexpr size_t kIpc4MailboxPayloadLimit = 4096;
    constexpr const char *kCopierUuid = "9ba00c83-ca12-4a83-943c-1fa2e82f9dda";
    constexpr const char *kGainUuid = "61bca9a8-18d0-4a18-8e7b-2639219804b7";
    constexpr const char *kEqIirUuid = "5150c0e6-27f9-4ec8-8351-c705b642d12f";
    const Module &copier = requireModule(firmware, kCopierUuid, "COPIER");
    const Module &gain = requireModule(firmware, kGainUuid, "GAIN");
    const Module &eqiir = requireModule(firmware, kEqIirUuid, "EQIIR");
    requireInstance(copier, ids.daiCopier);
    requireInstance(copier, ids.moduleCopier);
    requireInstance(gain, ids.gain);
    requireInstance(eqiir, ids.eqiir);
    if (ids.daiCopier == ids.moduleCopier)
        throw std::invalid_argument("DAI and buffer COPIER instances must be distinct");

    using namespace static_data;
    MessagePlan plan;
    plan.createPipeline = sof::ipc4::createPipeline(ids.pipeline, 0, 8, 0, false);
    plan.initModules = {
        sof::ipc4::initModule(static_cast<uint16_t>(copier.id), ids.daiCopier,
                              ids.pipeline, 0, kDaiCopierInit),
        sof::ipc4::initModule(static_cast<uint16_t>(gain.id), ids.gain,
                              ids.pipeline, 0, kGainInit),
        sof::ipc4::initModule(static_cast<uint16_t>(eqiir.id), ids.eqiir,
                              ids.pipeline, 0, kEqIirInit),
        sof::ipc4::initModule(static_cast<uint16_t>(copier.id), ids.moduleCopier,
                              ids.pipeline, 0, kModuleCopierInit),
    };
    plan.bindRoutes = {
        sof::ipc4::bindModules(static_cast<uint16_t>(copier.id), ids.daiCopier, 0,
                               static_cast<uint16_t>(gain.id), ids.gain, 0),
        sof::ipc4::bindModules(static_cast<uint16_t>(gain.id), ids.gain, 0,
                               static_cast<uint16_t>(eqiir.id), ids.eqiir, 0),
        sof::ipc4::bindModules(static_cast<uint16_t>(eqiir.id), ids.eqiir, 0,
                               static_cast<uint16_t>(copier.id), ids.moduleCopier, 0),
    };
    plan.configureControls = {
        sof::ipc4::largeConfigSet(static_cast<uint16_t>(eqiir.id), ids.eqiir, 0,
                                  withControlId(kEqIirControlPayloadTemplate, 0)),
        sof::ipc4::largeConfigSet(static_cast<uint16_t>(gain.id), ids.gain, 200,
                                  withControlId(kSwitchControlPayloadTemplate, 1)),
    };
    plan.unbindRoutes = {
        sof::ipc4::unbindModules(static_cast<uint16_t>(eqiir.id), ids.eqiir, 0,
                                 static_cast<uint16_t>(copier.id), ids.moduleCopier, 0),
        sof::ipc4::unbindModules(static_cast<uint16_t>(gain.id), ids.gain, 0,
                                 static_cast<uint16_t>(eqiir.id), ids.eqiir, 0),
        sof::ipc4::unbindModules(static_cast<uint16_t>(copier.id), ids.daiCopier, 0,
                                 static_cast<uint16_t>(gain.id), ids.gain, 0),
    };
    plan.deletePipeline = sof::ipc4::deletePipeline(ids.pipeline);

    sof::ipc4::validateMailboxRequest(plan.createPipeline, kIpc4MailboxPayloadLimit);
    for (const auto &message : plan.initModules)
        sof::ipc4::validateMailboxRequest(message, kIpc4MailboxPayloadLimit);
    for (const auto &message : plan.bindRoutes)
        sof::ipc4::validateMailboxRequest(message, kIpc4MailboxPayloadLimit);
    for (const auto &message : plan.configureControls)
        sof::ipc4::validateMailboxRequest(message, kIpc4MailboxPayloadLimit);
    for (const auto &message : plan.unbindRoutes)
        sof::ipc4::validateMailboxRequest(message, kIpc4MailboxPayloadLimit);
    sof::ipc4::validateMailboxRequest(plan.deletePipeline, kIpc4MailboxPayloadLimit);
    return plan;
}

} // namespace sof::ipc4::dmic
