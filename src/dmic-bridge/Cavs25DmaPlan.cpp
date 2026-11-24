#include "Cavs25DmaPlan.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint64_t kBdlEntryBytes = 16;
constexpr uint64_t kRequiredBdlBytes = 2 * kBdlEntryBytes;
constexpr uint64_t kDmaAlignment = 128;
constexpr uint64_t kDmaAlignmentMask = kDmaAlignment - 1;

bool rangeFits(uint64_t base, uint64_t length) noexcept {
    return length != 0 && base <= UINT64_MAX - (length - 1);
}

void splitAddress(uint64_t address, uint32_t *low, uint32_t *high) noexcept {
    *low = static_cast<uint32_t>(address & UINT64_C(0xffffffff));
    *high = static_cast<uint32_t>(address >> 32);
}

} // namespace

DmaPlanResult planCavs25CodeLoaderDma(
    const DmaMappingEvidence &mapping,
    uint64_t firmwarePayloadBytes,
    Cavs25DmaPlan *out) noexcept {
    if (out == nullptr) return DmaPlanResult::InvalidArgument;
    if (!mapping.firmwareMappingLive || !mapping.descriptorMappingLive ||
        !mapping.cacheVisibilityEstablished ||
        !mapping.firmwareIsSingleContiguousSegment)
        return DmaPlanResult::Unmapped;
    if (firmwarePayloadBytes > UINT32_MAX || firmwarePayloadBytes < 256 ||
        (firmwarePayloadBytes & UINT64_C(3)) != 0)
        return DmaPlanResult::InvalidPayloadSize;
    if (mapping.mappedBytes < firmwarePayloadBytes ||
        mapping.descriptorMappedBytes < kRequiredBdlBytes)
        return DmaPlanResult::InsufficientCoverage;
    if ((mapping.deviceAddress & kDmaAlignmentMask) != 0 ||
        (mapping.descriptorDeviceAddress & kDmaAlignmentMask) != 0)
        return DmaPlanResult::Misaligned;
    if (!rangeFits(mapping.deviceAddress, firmwarePayloadBytes) ||
        !rangeFits(mapping.descriptorDeviceAddress, kRequiredBdlBytes))
        return DmaPlanResult::AddressOverflow;

    const uint64_t firstBytes = (firmwarePayloadBytes / 2) & ~kDmaAlignmentMask;
    const uint64_t secondBytes = firmwarePayloadBytes - firstBytes;
    if (firstBytes == 0 || secondBytes == 0 ||
        firstBytes > UINT32_MAX || secondBytes > UINT32_MAX ||
        firstBytes > UINT64_MAX - mapping.deviceAddress)
        return DmaPlanResult::InvalidPayloadSize;

    out->cyclicBufferLength = static_cast<uint32_t>(firmwarePayloadBytes);
    splitAddress(mapping.descriptorDeviceAddress, &out->descriptorBaseLow,
                 &out->descriptorBaseHigh);
    out->lastValidIndex = 1;
    out->reserved = 0;
    splitAddress(mapping.deviceAddress, &out->entries[0].addressLow,
                 &out->entries[0].addressHigh);
    out->entries[0].byteCount = static_cast<uint32_t>(firstBytes);
    out->entries[0].interruptOnCompletion = 0;
    splitAddress(mapping.deviceAddress + firstBytes,
                 &out->entries[1].addressLow,
                 &out->entries[1].addressHigh);
    out->entries[1].byteCount = static_cast<uint32_t>(secondBytes);
    out->entries[1].interruptOnCompletion = 1;
    return DmaPlanResult::Ok;
}

} // namespace sof::ipc4::kernel
