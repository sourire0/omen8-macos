#include "HdaDmicCapturePlan.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kDmaConfigTlvType = 0x1000;
constexpr uint32_t kTlvHeaderBytes = 8;
constexpr uint32_t kDmaConfigBytes = 148;
constexpr uint32_t kDmaPrivateConfigSizeOffset = 144;

void storeLittle32(uint8_t *destination, uint32_t value) noexcept {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

} // namespace

HdaCapturePlanResult planHdaDmicCapture(
    const HdaCaptureStreamEvidence &evidence,
    HdaDmicCapturePlan *out) noexcept {
    if (out == nullptr) return HdaCapturePlanResult::InvalidArgument;
    if (!evidence.controllerExclusivelyOwned)
        return HdaCapturePlanResult::OwnershipNotProven;
    if (!evidence.streamReserved || !evidence.descriptorReadbackMatches)
        return HdaCapturePlanResult::StreamNotReserved;
    // HDA stream tags are 4-bit nonzero values. They are not descriptor
    // indices, even when a particular allocator happens to assign them alike.
    if (evidence.inputStreamCount == 0 ||
        evidence.descriptorIndex >= evidence.inputStreamCount ||
        evidence.streamTag == 0 || evidence.streamTag > 15)
        return HdaCapturePlanResult::InvalidStreamTag;

    const uint32_t streamTag = evidence.streamTag;
    out->dmaMethod = 1; // SOF_IPC4_DMA_METHOD_HDA
    out->preAllocatedByHost = 1;
    out->reserved = 0;
    out->daiData = streamTag - 1;
    out->dmaChannelId = streamTag - 1;
    out->streamId = streamTag;
    out->deviceCount = 1;
    out->dmaPrivateConfigSize = 0;
    return HdaCapturePlanResult::Ok;
}

HdaCapturePlanResult serializeHdaDmicCaptureDmaTlv(
    const HdaDmicCapturePlan &plan,
    uint8_t *output,
    uint32_t outputCapacity) noexcept {
    if (!output) return HdaCapturePlanResult::InvalidArgument;
    if (outputCapacity < kHdaDmicDmaConfigTlvBytes)
        return HdaCapturePlanResult::InsufficientOutputCapacity;
    if (plan.dmaMethod != 1 || plan.preAllocatedByHost != 1 ||
        plan.reserved != 0 || plan.streamId == 0 || plan.streamId > 15 ||
        plan.dmaChannelId != plan.streamId - 1 ||
        plan.daiData != plan.streamId - 1 || plan.deviceCount != 1 ||
        plan.dmaPrivateConfigSize != 0)
        return HdaCapturePlanResult::InvalidPlan;

    for (uint32_t i = 0; i < kHdaDmicDmaConfigTlvBytes; ++i)
        output[i] = 0;
    storeLittle32(output + 0, kDmaConfigTlvType);
    storeLittle32(output + 4, kDmaConfigBytes);
    output[8] = plan.dmaMethod;
    output[9] = plan.preAllocatedByHost;
    // bytes 10..11 are the reserved field and remain zero.
    storeLittle32(output + 12, plan.dmaChannelId);
    storeLittle32(output + 16, plan.streamId);
    storeLittle32(output + 20, plan.deviceCount);
    // The fixed 16-entry DMIC channel map is zero-filled, as in the upstream
    // HDA DMIC path; ALH's per-device map population is a separate path.
    storeLittle32(output + kTlvHeaderBytes + kDmaPrivateConfigSizeOffset,
                  plan.dmaPrivateConfigSize);
    return HdaCapturePlanResult::Ok;
}

} // namespace sof::ipc4::kernel
