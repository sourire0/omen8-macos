#pragma once

#include <stdint.h>

namespace sof::ipc4::kernel {

enum class DmaPlanResult : uint8_t {
    Ok,
    InvalidArgument,
    Unmapped,
    InsufficientCoverage,
    Misaligned,
    AddressOverflow,
    InvalidPayloadSize,
};

// Values are device-visible DMA addresses returned by the future IODMACommand
// mapping, never CPU virtual or CPU physical addresses. This plan has no I/O.
struct DmaMappingEvidence {
    uint64_t deviceAddress;
    uint64_t mappedBytes;
    uint64_t descriptorDeviceAddress;
    uint64_t descriptorMappedBytes;
    bool firmwareMappingLive;
    bool descriptorMappingLive;
    bool cacheVisibilityEstablished;
    bool firmwareIsSingleContiguousSegment;
    bool firmwareContentsSynchronized;
    bool descriptorContentsSynchronized;
};

struct HdaBdlEntryPlan {
    uint32_t addressLow;
    uint32_t addressHigh;
    uint32_t byteCount;
    uint32_t interruptOnCompletion;
};

struct Cavs25DmaPlan {
    uint32_t cyclicBufferLength;
    uint32_t descriptorBaseLow;
    uint32_t descriptorBaseHigh;
    uint16_t lastValidIndex;
    uint16_t reserved;
    HdaBdlEntryPlan entries[2];
};

// Plans the two-entry, 128-byte-aligned split used by the audited cAVS loader.
// It only describes values; caller must establish and retain real device DMA
// mappings and must not infer hardware ownership from a successful plan.
DmaPlanResult planCavs25CodeLoaderDma(
    const DmaMappingEvidence &mapping,
    uint64_t firmwarePayloadBytes,
    Cavs25DmaPlan *out) noexcept;

} // namespace sof::ipc4::kernel
