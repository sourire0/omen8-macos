#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace sof::ipc4 {

struct Message {
    uint32_t primary{};
    uint32_t extension{};
    std::vector<uint8_t> payload;
};

// Validate a host request before an MMIO transport stages its payload in the
// downlink mailbox and rings the doorbell. This has no hardware side effects.
void validateMailboxRequest(const Message &message, size_t mailboxCapacity);

Message createPipeline(uint8_t pipelineInstance, uint8_t priority,
                       uint16_t memoryPages, uint8_t core, bool lowPower,
                       uint32_t domainId = 0, uint32_t stackBytes = 0,
                       uint32_t heapBytes = 0);
Message deletePipeline(uint8_t pipelineInstance);
Message setPipelineState(uint8_t pipelineInstance, uint16_t state);
Message initModule(uint16_t moduleId, uint8_t moduleInstance,
                   uint8_t pipelineInstance, uint8_t core,
                   const std::vector<uint8_t> &payload);
Message largeConfigSet(uint16_t moduleId, uint8_t moduleInstance,
                       uint8_t parameterId, const std::vector<uint8_t> &payload);
Message largeConfigGet(uint16_t moduleId, uint8_t moduleInstance,
                       uint8_t parameterId, uint32_t requestedBytes);
std::vector<uint8_t> controlPayload(uint16_t controlId,
                                    const std::vector<uint8_t> &data);
Message bindModules(uint16_t sourceModuleId, uint8_t sourceInstance,
                    uint8_t sourceQueue, uint16_t destinationModuleId,
                    uint8_t destinationInstance, uint8_t destinationQueue);
Message unbindModules(uint16_t sourceModuleId, uint8_t sourceInstance,
                      uint8_t sourceQueue, uint16_t destinationModuleId,
                      uint8_t destinationInstance, uint8_t destinationQueue);

} // namespace sof::ipc4
