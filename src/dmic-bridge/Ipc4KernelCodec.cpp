#include "Ipc4KernelCodec.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kTargetModule = 1U << 30;
constexpr uint32_t kGlobalTypeShift = 24;
constexpr uint32_t kModuleTypeShift = 24;
constexpr uint32_t kInstanceShift = 16;
constexpr uint32_t kLargeConfigSizeMask = (1U << 20) - 1U;
constexpr uint32_t kFirstBlock = 1U << 29;
constexpr uint32_t kLastBlock = 1U << 28;

Result clear(Message *out) noexcept {
    if (!out) return Result::NullOutput;
    out->primary = 0;
    out->extension = 0;
    out->payloadBytes = 0;
    return Result::Ok;
}

Result copyPayload(const uint8_t *payload, size_t payloadBytes,
                   Message *out, size_t paddedBytes) noexcept {
    if (payloadBytes != 0 && !payload) return Result::NullPayload;
    if (paddedBytes > kMaximumPayloadBytes || payloadBytes > paddedBytes)
        return Result::PayloadTooLarge;
    for (size_t i = 0; i < paddedBytes; ++i)
        out->payload[i] = i < payloadBytes ? payload[i] : 0;
    out->payloadBytes = static_cast<uint32_t>(paddedBytes);
    return Result::Ok;
}

} // namespace

Result createPipeline(uint8_t pipelineInstance, uint8_t priority,
                      uint16_t memoryPages, uint8_t core, bool lowPower,
                      Message *out) noexcept {
    const auto result = clear(out);
    if (result != Result::Ok) return result;
    if (priority > 31 || memoryPages > 0x7ff || core > 15)
        return Result::InvalidField;
    out->primary = (17U << kGlobalTypeShift) |
                   (static_cast<uint32_t>(pipelineInstance) << kInstanceShift) |
                   (static_cast<uint32_t>(priority) << 11) | memoryPages;
    out->extension = (static_cast<uint32_t>(core) << 20) |
                     (lowPower ? 1U : 0U) | (1U << 29);
    const uint32_t words[7] = {0x01000007U, 0U, 0U, 0x00030003U, 0U, 0U, 0U};
    for (size_t i = 0; i < 7; ++i) {
        for (size_t byte = 0; byte < 4; ++byte)
            out->payload[i * 4 + byte] =
                static_cast<uint8_t>(words[i] >> (byte * 8));
    }
    out->payloadBytes = 28;
    return Result::Ok;
}

Result deletePipeline(uint8_t pipelineInstance, Message *out) noexcept {
    const auto result = clear(out);
    if (result != Result::Ok) return result;
    out->primary = (18U << kGlobalTypeShift) |
                   (static_cast<uint32_t>(pipelineInstance) << kInstanceShift);
    return Result::Ok;
}

Result setPipelineState(uint8_t pipelineInstance, uint16_t state,
                        Message *out) noexcept {
    const auto result = clear(out);
    if (result != Result::Ok) return result;
    out->primary = (19U << kGlobalTypeShift) |
                   (static_cast<uint32_t>(pipelineInstance) << kInstanceShift) |
                   state;
    return Result::Ok;
}

Result setTwoPipelineState(uint8_t first, uint8_t second, uint16_t state, Message *out) noexcept {
    if (!out) return Result::NullOutput;
    if (first == second || (state != kIpc4PipelineStateReset && state != kIpc4PipelineStatePaused && state != kIpc4PipelineStateRunning)) return Result::InvalidField;
    const Result r = clear(out); if (r != Result::Ok) return r;
    out->primary = (19U << kGlobalTypeShift) | state; out->extension = 1;
    const uint32_t words[] = {2U, first, second};
    for (unsigned i=0;i<3;++i) for (unsigned b=0;b<4;++b) out->payload[4*i+b]=static_cast<uint8_t>(words[i]>>(8*b));
    out->payloadBytes=12; return Result::Ok;
}

Result initModule(uint16_t moduleId, uint8_t moduleInstance,
                  uint8_t pipelineInstance, uint8_t core,
                  const uint8_t *payload, size_t payloadBytes,
                  Message *out) noexcept {
    auto result = clear(out);
    if (result != Result::Ok) return result;
    if (core > 15) return Result::InvalidField;
    if (payloadBytes > kMaximumPayloadBytes) return Result::PayloadTooLarge;
    const size_t paddedBytes = (payloadBytes + 3U) & ~static_cast<size_t>(3U);
    result = copyPayload(payload, payloadBytes, out, paddedBytes);
    if (result != Result::Ok) return result;
    out->primary = kTargetModule | (static_cast<uint32_t>(moduleInstance) << kInstanceShift) |
                   moduleId;
    out->extension = static_cast<uint32_t>(paddedBytes / 4U) |
                     (static_cast<uint32_t>(pipelineInstance) << 16) |
                     (static_cast<uint32_t>(core) << 24);
    return Result::Ok;
}

Result appendInitModulePayload(const uint8_t *payload, size_t payloadBytes,
                               Message *message) noexcept {
    if (!message) return Result::NullOutput;
    if (payloadBytes != 0 && !payload) return Result::NullPayload;
    if (payloadBytes > kMaximumPayloadBytes) return Result::PayloadTooLarge;
    if ((message->primary & (1U << 30)) == 0 ||
        message->payloadBytes > kMaximumPayloadBytes ||
        (message->payloadBytes & 3U) != 0 ||
        (message->extension & 0xffffU) != message->payloadBytes / 4U)
        return Result::InvalidField;
    const size_t paddedAppendBytes =
        (payloadBytes + 3U) & ~static_cast<size_t>(3U);
    if (paddedAppendBytes > kMaximumPayloadBytes - message->payloadBytes ||
        (message->payloadBytes + paddedAppendBytes) / 4U > 0xffffU)
        return Result::PayloadTooLarge;

    for (size_t i = 0; i < paddedAppendBytes; ++i)
        message->payload[message->payloadBytes + i] =
            i < payloadBytes ? payload[i] : 0;
    message->payloadBytes += static_cast<uint32_t>(paddedAppendBytes);
    message->extension = (message->extension & 0xffff0000U) |
                         (message->payloadBytes / 4U);
    return Result::Ok;
}

Result largeConfigGet(uint16_t moduleId, uint8_t moduleInstance,
                      uint8_t parameterId, uint32_t requestedBytes,
                      Message *out) noexcept {
    const auto result = clear(out);
    if (result != Result::Ok) return result;
    if (requestedBytes > kLargeConfigSizeMask) return Result::InvalidField;
    out->primary = kTargetModule | (3U << kModuleTypeShift) |
                   (static_cast<uint32_t>(moduleInstance) << kInstanceShift) |
                   moduleId;
    out->extension = (static_cast<uint32_t>(parameterId) << 20) |
                     requestedBytes | kFirstBlock;
    return Result::Ok;
}

Result largeConfigSet(uint16_t moduleId, uint8_t moduleInstance,
                      uint8_t parameterId, const uint8_t *payload,
                      size_t payloadBytes, Message *out) noexcept {
    auto result = clear(out);
    if (result != Result::Ok) return result;
    if (payloadBytes == 0 || payloadBytes > kLargeConfigSizeMask)
        return Result::InvalidField;
    if ((payloadBytes & 3U) != 0) return Result::InvalidField;
    result = copyPayload(payload, payloadBytes, out, payloadBytes);
    if (result != Result::Ok) return result;
    out->primary = kTargetModule | (4U << kModuleTypeShift) |
                   (static_cast<uint32_t>(moduleInstance) << kInstanceShift) |
                   moduleId;
    out->extension = (static_cast<uint32_t>(parameterId) << 20) |
                     static_cast<uint32_t>(payloadBytes) | kFirstBlock | kLastBlock;
    return Result::Ok;
}

Result bindModules(bool bind, uint16_t sourceModuleId,
                   uint8_t sourceInstance, uint8_t sourceQueue,
                   uint16_t destinationModuleId, uint8_t destinationInstance,
                   uint8_t destinationQueue, Message *out) noexcept {
    const auto result = clear(out);
    if (result != Result::Ok) return result;
    if (sourceQueue > 7 || destinationQueue > 7) return Result::InvalidField;
    out->primary = kTargetModule | ((bind ? 5U : 6U) << kModuleTypeShift) |
                   (static_cast<uint32_t>(sourceInstance) << kInstanceShift) |
                   sourceModuleId;
    out->extension = destinationModuleId |
                     (static_cast<uint32_t>(destinationInstance) << 16) |
                     (static_cast<uint32_t>(destinationQueue) << 24) |
                     (static_cast<uint32_t>(sourceQueue) << 27);
    return Result::Ok;
}

} // namespace sof::ipc4::kernel
