#include "Cavs25RomStatusReader.hpp"

#include <IOKit/IOLib.h>

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kRomStatusOffset = 0x80000;
constexpr uint32_t kRomErrorOrFirmwareStatusOffset = 0x80004;
constexpr uint32_t kHipcIdaOffset = 0x000d4;
constexpr uint32_t kHipcIdrOffset = 0x000d0;
constexpr uint32_t kAdspcsOffset = 0x00004;

} // namespace

Cavs25RomStatusReader::Cavs25RomStatusReader(
    IOPCIDevice *device, IOService *owner, IOMemoryMap *dspMap,
    bool sharedReadOnlyLease) noexcept
    : device_(device), owner_(owner), dspMap_(dspMap),
      sharedReadOnlyLease_(sharedReadOnlyLease) {}

bool Cavs25RomStatusReader::deviceOpen() const noexcept {
    return device_ && owner_ &&
        (device_->isOpen(owner_) || sharedReadOnlyLease_);
}

bool Cavs25RomStatusReader::validMapping(uint32_t offset,
                                         uint32_t length) const noexcept {
    if (!dspMap_ || dspMap_->getVirtualAddress() == 0) return false;
    const uint64_t mapLength = dspMap_->getLength();
    return static_cast<uint64_t>(offset) <= mapLength &&
           static_cast<uint64_t>(length) <= mapLength - offset;
}

bool Cavs25RomStatusReader::readSnapshot(
    uint32_t *status, uint32_t *errorOrFirmwareStatus) const noexcept {
    if (!deviceOpen() || !status || !errorOrFirmwareStatus ||
        !validMapping(kRomStatusOffset, sizeof(uint32_t)) ||
        !validMapping(kRomErrorOrFirmwareStatusOffset, sizeof(uint32_t)))
        return false;

    const volatile uint32_t *base = reinterpret_cast<const volatile uint32_t *>(
        dspMap_->getVirtualAddress());
    OSSynchronizeIO();
    *status = base[kRomStatusOffset / sizeof(uint32_t)];
    *errorOrFirmwareStatus =
        base[kRomErrorOrFirmwareStatusOffset / sizeof(uint32_t)];
    OSSynchronizeIO();
    return true;
}

bool Cavs25RomStatusReader::readPurgeAck(uint32_t *ack,
                                         uint32_t *romError) const noexcept {
    if (!deviceOpen() || !ack || !romError ||
        !validMapping(kHipcIdaOffset, sizeof(uint32_t)) ||
        !validMapping(kRomErrorOrFirmwareStatusOffset, sizeof(uint32_t)))
        return false;
    const volatile uint32_t *base = reinterpret_cast<const volatile uint32_t *>(
        dspMap_->getVirtualAddress());
    OSSynchronizeIO();
    *ack = base[kHipcIdaOffset / sizeof(uint32_t)];
    *romError = base[kRomErrorOrFirmwareStatusOffset / sizeof(uint32_t)];
    OSSynchronizeIO();
    return true;
}

bool Cavs25RomStatusReader::readPurgeAndCoreState(
    uint32_t *hostRequest, uint32_t *adspcs) const noexcept {
    if (!deviceOpen() || !hostRequest || !adspcs ||
        !validMapping(kHipcIdrOffset, sizeof(uint32_t)) ||
        !validMapping(kAdspcsOffset, sizeof(uint32_t)))
        return false;
    const volatile uint32_t *base = reinterpret_cast<const volatile uint32_t *>(
        dspMap_->getVirtualAddress());
    OSSynchronizeIO();
    *hostRequest = base[kHipcIdrOffset / sizeof(uint32_t)];
    *adspcs = base[kAdspcsOffset / sizeof(uint32_t)];
    OSSynchronizeIO();
    return true;
}

bool Cavs25RomStatusReader::observe(Cavs25RomStatusPlan *plan,
                                    RomPollResult *result) const noexcept {
    if (!plan || !result) return false;
    uint32_t status = 0;
    uint32_t errorOrFirmwareStatus = 0;
    if (!readSnapshot(&status, &errorOrFirmwareStatus)) return false;
    *result = plan->observe(status, errorOrFirmwareStatus);
    return true;
}

bool Cavs25RomStatusReader::observePurgeAck(
    Cavs25RomPurgePlan *plan, RomPurgePollResult *result) const noexcept {
    if (!plan || !result) return false;
    uint32_t ack = 0;
    uint32_t romError = 0;
    if (!readPurgeAck(&ack, &romError)) return false;
    *result = plan->observeAck(ack, romError);
    return true;
}

} // namespace sof::ipc4::kernel
