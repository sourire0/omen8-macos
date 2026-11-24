#include "HdaControllerStatusReader.hpp"

#include <IOKit/IOLib.h>

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kGcapOffset = 0x00;
constexpr uint32_t kGctlOffset = 0x08;
constexpr uint32_t kLlchOffset = 0x14;
constexpr uint32_t kIntctlOffset = 0x20;
constexpr uint32_t kStreamBase = 0x80;
constexpr uint32_t kStreamStride = 0x20;
constexpr uint32_t kStreamControlOffset = 0x00;
constexpr uint32_t kPpCtlOffset = 0x04;
constexpr uint32_t kSpbFcCtlOffset = 0x04;
constexpr uint32_t kSpibPositionsOffset = 0x08;
constexpr uint32_t kSpibStreamStride = 0x08;
constexpr uint32_t kCapabilityIdShift = 16;
constexpr uint32_t kCapabilityIdMask = 0x0fffU << 16;
constexpr uint32_t kPpCapabilityId = 0x003U;
constexpr uint32_t kSpibCapabilityId = 0x004U;
constexpr uint8_t kMaximumCapabilities = 10;
constexpr uint32_t kStreamTagMask = 0x0fU << 20;

} // namespace

HdaControllerStatusReader::HdaControllerStatusReader(
    IOPCIDevice *device, IOService *owner, IOMemoryMap *hdaMap,
    bool sharedReadOnlyLease) noexcept
    : device_(device), owner_(owner), hdaMap_(hdaMap),
      sharedReadOnlyLease_(sharedReadOnlyLease) {}

bool HdaControllerStatusReader::deviceOpen() const noexcept {
    return device_ && owner_ &&
        (device_->isOpen(owner_) || sharedReadOnlyLease_);
}

bool HdaControllerStatusReader::validMapping(uint32_t offset,
                                              uint32_t length) const noexcept {
    if (!hdaMap_ || hdaMap_->getVirtualAddress() == 0) return false;
    const uint64_t mapLength = hdaMap_->getLength();
    return static_cast<uint64_t>(offset) <= mapLength &&
           static_cast<uint64_t>(length) <= mapLength - offset;
}

bool HdaControllerStatusReader::readSnapshot(
    HdaControllerStatusSnapshot *snapshot) const noexcept {
    if (!deviceOpen() || !snapshot ||
        !validMapping(kGcapOffset, sizeof(uint16_t)) ||
        !validMapping(kGctlOffset, sizeof(uint32_t)) ||
        !validMapping(kLlchOffset, sizeof(uint16_t)) ||
        !validMapping(kIntctlOffset, sizeof(uint32_t)))
        return false;

    HdaControllerStatusSnapshot observed{};
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    observed.globalCapabilities = *reinterpret_cast<volatile uint16_t *>(
        base + kGcapOffset);
    observed.globalControl = *reinterpret_cast<volatile uint32_t *>(
        base + kGctlOffset);
    observed.lowerLinkCapabilitiesHeader =
        *reinterpret_cast<volatile uint16_t *>(base + kLlchOffset);
    observed.intctl = *reinterpret_cast<volatile uint32_t *>(
        base + kIntctlOffset);
    OSSynchronizeIO();

    if (observed.globalCapabilities == 0xffffU ||
        observed.globalControl == UINT32_MAX ||
        observed.lowerLinkCapabilitiesHeader == 0xffffU ||
        observed.intctl == UINT32_MAX)
        return false;

    const uint8_t inputCount = static_cast<uint8_t>(
        (observed.globalCapabilities >> 8) & 0x0fU);
    const uint8_t outputCount = static_cast<uint8_t>(
        (observed.globalCapabilities >> 12) & 0x0fU);
    const uint8_t bidirectionalCount = static_cast<uint8_t>(
        (observed.globalCapabilities >> 3) & 0x1fU);
    const uint16_t totalCount = static_cast<uint16_t>(inputCount) +
        outputCount + bidirectionalCount;
    if (inputCount == 0 || inputCount > kHdaMaximumStreamDescriptors ||
        totalCount == 0 || totalCount > kHdaMaximumTotalStreamCount)
        return false;
    observed.inputStreamCount = inputCount;
    observed.outputStreamCount = outputCount;
    observed.bidirectionalStreamCount = bidirectionalCount;
    observed.totalStreamCount = static_cast<uint8_t>(totalCount);

    bool ppCapabilityFound = false;
    bool spibCapabilityFound = false;
    uint16_t capabilityOffset = observed.lowerLinkCapabilitiesHeader;
    uint16_t visited[kMaximumCapabilities]{};
    uint8_t visitedCount = 0;
    while (capabilityOffset != 0 && capabilityOffset != 0xffffU) {
        if (visitedCount >= kMaximumCapabilities ||
            capabilityOffset < 0x40 || (capabilityOffset & 3U) != 0 ||
            !validMapping(capabilityOffset, sizeof(uint32_t)))
            return false;
        for (uint8_t i = 0; i < visitedCount; ++i) {
            if (visited[i] == capabilityOffset) return false;
        }
        visited[visitedCount++] = capabilityOffset;

        OSSynchronizeIO();
        const uint32_t capabilityHeader =
            *reinterpret_cast<volatile uint32_t *>(base + capabilityOffset);
        OSSynchronizeIO();
        if (capabilityHeader == UINT32_MAX) return false;
        const uint32_t capabilityId =
            (capabilityHeader & kCapabilityIdMask) >> kCapabilityIdShift;
        if (capabilityId == kPpCapabilityId) {
            if (ppCapabilityFound ||
                !validMapping(static_cast<uint32_t>(capabilityOffset) +
                                  kPpCtlOffset,
                              sizeof(uint32_t)))
                return false;
            observed.ppCapabilityOffset = capabilityOffset;
            observed.ppCapabilityHeader = capabilityHeader;
            OSSynchronizeIO();
            observed.ppctl = *reinterpret_cast<volatile uint32_t *>(
                base + capabilityOffset + kPpCtlOffset);
            OSSynchronizeIO();
            if (observed.ppctl == UINT32_MAX) return false;
            ppCapabilityFound = true;
        } else if (capabilityId == kSpibCapabilityId) {
            const uint64_t spibEnd =
                static_cast<uint64_t>(capabilityOffset) +
                kSpibPositionsOffset +
                static_cast<uint64_t>(observed.totalStreamCount) *
                    kSpibStreamStride;
            if (spibCapabilityFound || spibEnd > hdaMap_->getLength() ||
                !validMapping(static_cast<uint32_t>(capabilityOffset) +
                                  kSpbFcCtlOffset,
                              sizeof(uint32_t)))
                return false;
            observed.spibCapabilityOffset = capabilityOffset;
            observed.spibCapabilityHeader = capabilityHeader;
            OSSynchronizeIO();
            observed.spbfcctl = *reinterpret_cast<volatile uint32_t *>(
                base + capabilityOffset + kSpbFcCtlOffset);
            OSSynchronizeIO();
            if (observed.spbfcctl == UINT32_MAX) return false;
            for (uint8_t i = 0; i < observed.totalStreamCount; ++i) {
                const uint32_t positionOffset =
                    static_cast<uint32_t>(capabilityOffset) +
                    kSpibPositionsOffset +
                    static_cast<uint32_t>(i) * kSpibStreamStride;
                OSSynchronizeIO();
                observed.spibPositions[i] =
                    *reinterpret_cast<volatile uint32_t *>(
                        base + positionOffset);
                OSSynchronizeIO();
                if (observed.spibPositions[i] == UINT32_MAX) return false;
            }
            observed.spibCapabilityPresent = true;
            spibCapabilityFound = true;
        }
        capabilityOffset = static_cast<uint16_t>(capabilityHeader & 0xffffU);
    }
    if (!ppCapabilityFound) return false;

    for (uint8_t i = 0; i < observed.totalStreamCount; ++i) {
        const uint32_t descriptorOffset = kStreamBase +
            static_cast<uint32_t>(i) * kStreamStride;
        const uint32_t streamOffset = descriptorOffset + kStreamControlOffset;
        if (!validMapping(streamOffset, 4) ||
            !validMapping(descriptorOffset + 0x03U, 1) ||
            !validMapping(descriptorOffset + 0x08U, 4) ||
            !validMapping(descriptorOffset + 0x0cU, 2) ||
            !validMapping(descriptorOffset + 0x12U, 2) ||
            !validMapping(descriptorOffset + 0x18U, 4) ||
            !validMapping(descriptorOffset + 0x1cU, 4)) return false;
        OSSynchronizeIO();
        const uint32_t control = *reinterpret_cast<volatile uint32_t *>(
            base + streamOffset);
        const uint8_t status = base[descriptorOffset + 0x03U];
        const uint32_t cbl = *reinterpret_cast<volatile uint32_t *>(
            base + descriptorOffset + 0x08U);
        const uint16_t lvi = *reinterpret_cast<volatile uint16_t *>(
            base + descriptorOffset + 0x0cU);
        const uint16_t format = *reinterpret_cast<volatile uint16_t *>(
            base + descriptorOffset + 0x12U);
        const uint32_t bdlLow = *reinterpret_cast<volatile uint32_t *>(
            base + descriptorOffset + 0x18U);
        const uint32_t bdlHigh = *reinterpret_cast<volatile uint32_t *>(
            base + descriptorOffset + 0x1cU);
        OSSynchronizeIO();
        if (control == UINT32_MAX || cbl == UINT32_MAX ||
            status == 0xffU || lvi == UINT16_MAX ||
            format == UINT16_MAX || bdlLow == UINT32_MAX ||
            bdlHigh == UINT32_MAX)
            return false;
        observed.allStreamControls[i].control = control;
        observed.allStreamControls[i].status = status;
        observed.allStreamControls[i].cyclicBufferLength = cbl;
        observed.allStreamControls[i].lastValidIndex = lvi;
        observed.allStreamControls[i].format = format;
        observed.allStreamControls[i].bdlLow = bdlLow;
        observed.allStreamControls[i].bdlHigh = bdlHigh;

        const uint8_t tag = static_cast<uint8_t>(
            (control & kStreamTagMask) >> 20);
        if (tag != 0) {
            const uint16_t tagBit = static_cast<uint16_t>(1U << tag);
            if ((observed.allocatedStreamTagMask & tagBit) != 0)
                return false;
            observed.allocatedStreamTagMask = static_cast<uint16_t>(
                observed.allocatedStreamTagMask | tagBit);
        }
        if (i < inputCount)
            observed.inputStreams[i] = observed.allStreamControls[i];
    }

    *snapshot = observed;
    return true;
}

} // namespace sof::ipc4::kernel
