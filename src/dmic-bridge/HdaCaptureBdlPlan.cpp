#include "HdaCaptureBdlPlan.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint64_t kBdlEntryBytes = 16;
constexpr uint64_t kBdlAlignment = 128;
constexpr uint64_t kPageBytes = 4096;
constexpr uint16_t kMaximumBdlEntries = 256;

bool rangeFits(uint64_t address, uint64_t bytes) noexcept {
    return bytes != 0 && address <= UINT64_MAX - (bytes - 1);
}

void splitAddress(uint64_t address, uint32_t *low, uint32_t *high) noexcept {
    *low = static_cast<uint32_t>(address & UINT64_C(0xffffffff));
    *high = static_cast<uint32_t>(address >> 32);
}

void storeLittle32(uint8_t *destination, uint32_t value) noexcept {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

} // namespace

HdaBdlPlanResult planHdaCaptureBdl(
    const HdaCaptureRingMapping &ring,
    const HdaCaptureBdlMapping &bdl,
    uint32_t periodBytes,
    bool splitAt4KiBBoundaries,
    HdaCaptureBdlEntry *entries,
    uint16_t entryCapacity,
    HdaCaptureBdlPlan *out) noexcept {
    if (!out || !entries || entryCapacity == 0)
        return HdaBdlPlanResult::InvalidArgument;
    if (!ring.mappingLive || !bdl.mappingLive)
        return HdaBdlPlanResult::MappingNotLive;
    if (!ring.segments || ring.segmentCount == 0 || ring.ringBytes == 0 ||
        ring.ringBytes > UINT32_MAX || periodBytes == 0 ||
        ring.ringBytes % periodBytes != 0)
        return HdaBdlPlanResult::InvalidBufferGeometry;
    if ((bdl.deviceAddress & (kBdlAlignment - 1)) != 0)
        return HdaBdlPlanResult::MisalignedDescriptorList;
    if (!rangeFits(bdl.deviceAddress, bdl.mappedBytes))
        return HdaBdlPlanResult::AddressOverflow;

    uint64_t totalMappedBytes = 0;
    for (uint32_t i = 0; i < ring.segmentCount; ++i) {
        const HdaCaptureDmaSegment &segment = ring.segments[i];
        if (!rangeFits(segment.deviceAddress, segment.byteLength))
            return HdaBdlPlanResult::InvalidSegment;
        if (totalMappedBytes > UINT64_MAX - segment.byteLength)
            return HdaBdlPlanResult::AddressOverflow;
        totalMappedBytes += segment.byteLength;
    }
    if (totalMappedBytes != ring.ringBytes)
        return HdaBdlPlanResult::InvalidBufferGeometry;

    uint32_t segmentIndex = 0;
    uint64_t segmentOffset = 0;
    uint64_t ringOffset = 0;
    uint16_t entryCount = 0;
    while (ringOffset < ring.ringBytes) {
        if (segmentIndex >= ring.segmentCount)
            return HdaBdlPlanResult::InvalidBufferGeometry;
        const HdaCaptureDmaSegment &segment = ring.segments[segmentIndex];
        if (segmentOffset >= segment.byteLength) {
            ++segmentIndex;
            segmentOffset = 0;
            continue;
        }
        if (entryCount >= entryCapacity ||
            entryCount >= kMaximumBdlEntries)
            return HdaBdlPlanResult::TooManyEntries;

        const uint64_t periodRemaining =
            periodBytes - (ringOffset % periodBytes);
        const uint64_t segmentRemaining = segment.byteLength - segmentOffset;
        uint64_t chunk = periodRemaining < segmentRemaining
            ? periodRemaining : segmentRemaining;
        uint64_t address = segment.deviceAddress + segmentOffset;
        if (splitAt4KiBBoundaries) {
            const uint64_t pageRemaining =
                kPageBytes - (address & (kPageBytes - 1));
            if (chunk > pageRemaining) chunk = pageRemaining;
        }
        if (chunk == 0 || chunk > UINT32_MAX ||
            !rangeFits(address, chunk))
            return HdaBdlPlanResult::InvalidSegment;

        HdaCaptureBdlEntry &entry = entries[entryCount];
        splitAddress(address, &entry.addressLow, &entry.addressHigh);
        entry.byteCount = static_cast<uint32_t>(chunk);
        entry.interruptOnCompletion =
            ((ringOffset + chunk) % periodBytes) == 0 ? 1U : 0U;
        ++entryCount;
        segmentOffset += chunk;
        ringOffset += chunk;
    }

    const uint64_t descriptorBytes =
        static_cast<uint64_t>(entryCount) * kBdlEntryBytes;
    if (descriptorBytes > bdl.mappedBytes)
        return HdaBdlPlanResult::InsufficientDescriptorMapping;

    out->cyclicBufferLength = static_cast<uint32_t>(ring.ringBytes);
    out->periodLength = periodBytes;
    out->entryCount = entryCount;
    out->lastValidIndex = static_cast<uint8_t>(entryCount - 1);
    out->reserved = 0;
    splitAddress(bdl.deviceAddress, &out->descriptorBaseLow,
                 &out->descriptorBaseHigh);
    return HdaBdlPlanResult::Ok;
}

HdaBdlPlanResult serializeHdaCaptureBdl(
    const HdaCaptureBdlPlan &plan,
    const HdaCaptureBdlEntry *entries,
    uint8_t *hostDescriptorBytes,
    uint32_t hostDescriptorCapacity) noexcept {
    if (!entries || !hostDescriptorBytes)
        return HdaBdlPlanResult::InvalidArgument;
    if (plan.entryCount == 0 || plan.entryCount > kMaximumBdlEntries ||
        plan.lastValidIndex != plan.entryCount - 1 ||
        plan.cyclicBufferLength == 0 || plan.periodLength == 0 ||
        plan.cyclicBufferLength % plan.periodLength != 0)
        return HdaBdlPlanResult::InvalidPlan;

    const uint64_t requiredBytes =
        static_cast<uint64_t>(plan.entryCount) * kBdlEntryBytes;
    if (requiredBytes > hostDescriptorCapacity)
        return HdaBdlPlanResult::InsufficientHostCapacity;

    uint64_t totalBytes = 0;
    for (uint16_t i = 0; i < plan.entryCount; ++i) {
        const HdaCaptureBdlEntry &entry = entries[i];
        const uint64_t address =
            (static_cast<uint64_t>(entry.addressHigh) << 32) |
            entry.addressLow;
        if (entry.byteCount == 0 || entry.interruptOnCompletion > 1 ||
            !rangeFits(address, entry.byteCount) ||
            totalBytes > UINT64_MAX - entry.byteCount)
            return HdaBdlPlanResult::InvalidPlan;
        totalBytes += entry.byteCount;
        const uint32_t expectedIoc =
            (totalBytes % plan.periodLength) == 0 ? 1U : 0U;
        if (entry.interruptOnCompletion != expectedIoc)
            return HdaBdlPlanResult::InvalidPlan;
    }
    if (totalBytes != plan.cyclicBufferLength)
        return HdaBdlPlanResult::InvalidPlan;

    for (uint16_t i = 0; i < plan.entryCount; ++i) {
        uint8_t *destination = hostDescriptorBytes +
            static_cast<uint32_t>(i) * static_cast<uint32_t>(kBdlEntryBytes);
        storeLittle32(destination + 0, entries[i].addressLow);
        storeLittle32(destination + 4, entries[i].addressHigh);
        storeLittle32(destination + 8, entries[i].byteCount);
        storeLittle32(destination + 12,
                      entries[i].interruptOnCompletion);
    }
    return HdaBdlPlanResult::Ok;
}

} // namespace sof::ipc4::kernel
