#pragma once

#include <stdint.h>

namespace sof::ipc4::kernel {

enum class HdaBdlPlanResult : uint8_t {
    Ok,
    InvalidArgument,
    MappingNotLive,
    InvalidBufferGeometry,
    InvalidSegment,
    InsufficientDescriptorMapping,
    InsufficientHostCapacity,
    MisalignedDescriptorList,
    AddressOverflow,
    InvalidPlan,
    TooManyEntries,
};

struct HdaCaptureDmaSegment {
    uint64_t deviceAddress;
    uint64_t byteLength;
};

struct HdaCaptureRingMapping {
    const HdaCaptureDmaSegment *segments;
    uint32_t segmentCount;
    uint64_t ringBytes;
    bool mappingLive;
};

struct HdaCaptureBdlMapping {
    uint64_t deviceAddress;
    uint64_t mappedBytes;
    bool mappingLive;
};

struct HdaCaptureBdlEntry {
    uint32_t addressLow;
    uint32_t addressHigh;
    uint32_t byteCount;
    uint32_t interruptOnCompletion;
};

struct HdaCaptureBdlPlan {
    uint32_t cyclicBufferLength;
    uint32_t periodLength;
    uint8_t lastValidIndex;
    uint8_t reserved;
    uint16_t entryCount;
    uint32_t descriptorBaseLow;
    uint32_t descriptorBaseHigh;
};

// Builds one BDLE at a time, splitting at period boundaries and optionally at
// 4 KiB boundaries for controllers that require that quirk. This is a pure
// plan over live IODMA device addresses; it performs no mapping, sync, or I/O.
HdaBdlPlanResult planHdaCaptureBdl(
    const HdaCaptureRingMapping &ring,
    const HdaCaptureBdlMapping &bdl,
    uint32_t periodBytes,
    bool splitAt4KiBBoundaries,
    HdaCaptureBdlEntry *entries,
    uint16_t entryCapacity,
    HdaCaptureBdlPlan *out) noexcept;

// Serializes a previously validated plan to caller-owned host memory. The
// caller still has to synchronize the mapped BDL for device visibility.
HdaBdlPlanResult serializeHdaCaptureBdl(
    const HdaCaptureBdlPlan &plan,
    const HdaCaptureBdlEntry *entries,
    uint8_t *hostDescriptorBytes,
    uint32_t hostDescriptorCapacity) noexcept;

} // namespace sof::ipc4::kernel
