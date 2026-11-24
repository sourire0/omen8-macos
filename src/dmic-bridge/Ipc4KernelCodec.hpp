#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sof::ipc4::kernel {

constexpr size_t kMaximumPayloadBytes = 4096;
constexpr uint16_t kIpc4PipelineStateReset = 2;
constexpr uint16_t kIpc4PipelineStatePaused = 3;
constexpr uint16_t kIpc4PipelineStateRunning = 4;

enum class Result : uint8_t {
    Ok,
    NullOutput,
    NullPayload,
    PayloadTooLarge,
    InvalidField,
};

// POD message storage suitable for kernel code: fixed-size, no allocator,
// exceptions, RTTI, or C++ standard-library containers. It is >4 KiB; keep it
// in device-owned/nonpaged storage and never allocate it as a kernel stack local.
struct Message {
    uint32_t primary;
    uint32_t extension;
    uint32_t payloadBytes;
    uint8_t payload[kMaximumPayloadBytes];
};

Result createPipeline(uint8_t pipelineInstance, uint8_t priority,
                      uint16_t memoryPages, uint8_t core, bool lowPower,
                      Message *out) noexcept;
Result deletePipeline(uint8_t pipelineInstance, Message *out) noexcept;
Result setPipelineState(uint8_t pipelineInstance, uint16_t state,
                        Message *out) noexcept;
Result setTwoPipelineState(uint8_t first, uint8_t second, uint16_t state, Message *out) noexcept;
Result initModule(uint16_t moduleId, uint8_t moduleInstance,
                  uint8_t pipelineInstance, uint8_t core,
                  const uint8_t *payload, size_t payloadBytes,
                  Message *out) noexcept;
Result appendInitModulePayload(const uint8_t *payload, size_t payloadBytes,
                               Message *message) noexcept;
Result largeConfigGet(uint16_t moduleId, uint8_t moduleInstance,
                      uint8_t parameterId, uint32_t requestedBytes,
                      Message *out) noexcept;
Result largeConfigSet(uint16_t moduleId, uint8_t moduleInstance,
                      uint8_t parameterId, const uint8_t *payload,
                      size_t payloadBytes, Message *out) noexcept;
Result bindModules(bool bind, uint16_t sourceModuleId,
                   uint8_t sourceInstance, uint8_t sourceQueue,
                   uint16_t destinationModuleId, uint8_t destinationInstance,
                   uint8_t destinationQueue, Message *out) noexcept;

} // namespace sof::ipc4::kernel
