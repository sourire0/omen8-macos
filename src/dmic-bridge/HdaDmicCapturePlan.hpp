#pragma once

#include <stdint.h>

namespace sof::ipc4::kernel {

enum class HdaCapturePlanResult : uint8_t {
    Ok,
    InvalidArgument,
    OwnershipNotProven,
    StreamNotReserved,
    InvalidStreamTag,
    InvalidPlan,
    InsufficientOutputCapacity,
};

constexpr uint32_t kHdaDmicDmaConfigTlvBytes = 156;

// Evidence must come from a future serialized HDA allocator and descriptor
// readback. This structure does not reserve streams or access registers.
struct HdaCaptureStreamEvidence {
    uint8_t inputStreamCount;
    uint8_t descriptorIndex;
    uint8_t streamTag;
    bool controllerExclusivelyOwned;
    bool streamReserved;
    bool descriptorReadbackMatches;
};

// Runtime identity values consumed by the HDA DAI and IPC4 copier DMA paths.
// serializeHdaDmicCaptureDmaTlv() encodes the fixed channel-map payload and
// TLV framing. The plan only records runtime stream identity; it does not
// reserve an HDA stream or prove that a live descriptor readback matches.
struct HdaDmicCapturePlan {
    uint8_t dmaMethod;
    uint8_t preAllocatedByHost;
    uint16_t reserved;
    uint32_t daiData;
    uint32_t dmaChannelId;
    uint32_t streamId;
    uint32_t deviceCount;
    uint32_t dmaPrivateConfigSize;
};

HdaCapturePlanResult planHdaDmicCapture(
    const HdaCaptureStreamEvidence &evidence,
    HdaDmicCapturePlan *out) noexcept;

// Serializes the upstream SOF IPC4 DMA-config TLV layout (fixed 16-entry
// channel-map array) into caller-owned bytes. This TLV is a component of
// copier configuration, not a standalone IPC message.
HdaCapturePlanResult serializeHdaDmicCaptureDmaTlv(
    const HdaDmicCapturePlan &plan,
    uint8_t *output,
    uint32_t outputCapacity) noexcept;

} // namespace sof::ipc4::kernel
