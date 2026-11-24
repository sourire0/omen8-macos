#pragma once

#include "FirmwareManifest.hpp"
#include "Ipc4Messages.hpp"

#include <cstdint>
#include <vector>

namespace sof::ipc4::dmic {

struct RuntimeIds {
    uint8_t pipeline{};
    uint8_t daiCopier{};
    uint8_t gain{};
    uint8_t eqiir{};
    uint8_t moduleCopier{};
};

struct MessagePlan {
    Message createPipeline;
    std::vector<Message> initModules;
    std::vector<Message> bindRoutes;
    std::vector<Message> configureControls;
    std::vector<Message> unbindRoutes;
    Message deletePipeline;
};

// Pure serialization only. This function has no transport and cannot access hardware.
MessagePlan buildMessagePlan(const FirmwareManifest &firmware, const RuntimeIds &ids);

} // namespace sof::ipc4::dmic
