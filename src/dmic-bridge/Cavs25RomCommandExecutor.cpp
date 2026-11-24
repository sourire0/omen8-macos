#include "Cavs25RomCommandExecutor.hpp"

#include <IOKit/IOLib.h>

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kHipcIdrOffset = 0x000d0;
constexpr uint32_t kHipcIdaOffset = 0x000d4;
constexpr uint32_t kAdspcsOffset = 0x00004;
constexpr uint32_t kRomErrorOffset = 0x80004;
constexpr uint32_t kCore0UpdateMask = (1U << 0) | (1U << 8) | (1U << 16);
constexpr uint32_t kCore0CpaMask = 1U << 24;
constexpr uint32_t kSupportedCoreMask = 0x0fU;

} // namespace

Cavs25RomCommandExecutor::Cavs25RomCommandExecutor(
    IOPCIDevice *device, IOService *owner, IOMemoryMap *dspMap,
    const Cavs25BootLifecycle *lifecycle,
    bool mappingCreatedWritable, bool sharedAppleHdaProvider) noexcept
    : device_(device), owner_(owner), dspMap_(dspMap),
      lifecycle_(lifecycle), mappingCreatedWritable_(mappingCreatedWritable),
      sharedAppleHdaProvider_(sharedAppleHdaProvider) {}

bool Cavs25RomCommandExecutor::deviceOpen() const noexcept {
    return device_ && owner_ &&
        (device_->isOpen(owner_) || sharedAppleHdaProvider_);
}

bool Cavs25RomCommandExecutor::writableMap() const noexcept {
    return dspMap_ && dspMap_->getVirtualAddress() != 0 &&
        (dspMap_->getMapOptions() & kIOMapReadOnly) == 0 &&
        mappingCreatedWritable_;
}

bool Cavs25RomCommandExecutor::validRegister(uint32_t offset) const noexcept {
    if (!dspMap_ || (offset & 3U) != 0) return false;
    const uint64_t mapLength = dspMap_->getLength();
    return static_cast<uint64_t>(offset) <= mapLength &&
        sizeof(uint32_t) <= mapLength - offset;
}

bool Cavs25RomCommandExecutor::readRegister(
    uint32_t offset, uint32_t *value) const noexcept {
    if (!deviceOpen() || !value || !validRegister(offset)) return false;
    const volatile uint32_t *base =
        reinterpret_cast<const volatile uint32_t *>(
            dspMap_->getVirtualAddress());
    OSSynchronizeIO();
    *value = base[offset / sizeof(uint32_t)];
    OSSynchronizeIO();
    return true;
}

bool Cavs25RomCommandExecutor::writeRegister(
    uint32_t offset, uint32_t value) const noexcept {
    if (!deviceOpen() || !writableMap() || !validRegister(offset)) return false;
    volatile uint32_t *base = reinterpret_cast<volatile uint32_t *>(
        dspMap_->getVirtualAddress());
    OSSynchronizeIO();
    base[offset / sizeof(uint32_t)] = value;
    OSSynchronizeIO();
    return true;
}

RomCommandExecutionResult Cavs25RomCommandExecutor::submitPurgeRequest(
    uint8_t codeLoaderStreamTag, uint32_t *doorbellReadback) const noexcept {
    if (!doorbellReadback || codeLoaderStreamTag != 1)
        return RomCommandExecutionResult::InvalidArgument;
    *doorbellReadback = 0;
    if (!lifecycle_ || lifecycle_->phase() != BootPhase::LoaderPrepared)
        return RomCommandExecutionResult::WrongPhase;
    if (!deviceOpen()) return RomCommandExecutionResult::DeviceUnavailable;
    if (!writableMap() || !validRegister(kHipcIdrOffset) ||
        !validRegister(kRomErrorOffset))
        return RomCommandExecutionResult::UnsafeMapping;

    uint32_t request = 0;
    if (!readRegister(kHipcIdrOffset, &request))
        return RomCommandExecutionResult::DeviceUnavailable;
    if (request == UINT32_MAX)
        return RomCommandExecutionResult::InvalidReadback;
    if (request != 0)
        return RomCommandExecutionResult::PreconditionFailed;

    uint32_t doorbell = 0;
    if (!buildCavs25RomPurgeDoorbell(codeLoaderStreamTag, &doorbell))
        return RomCommandExecutionResult::InvalidArgument;
    if (!writeRegister(kHipcIdrOffset, doorbell))
        return RomCommandExecutionResult::WriteFailed;
    if (!readRegister(kHipcIdrOffset, doorbellReadback))
        return RomCommandExecutionResult::DeviceUnavailable;
    if (*doorbellReadback != doorbell)
        return RomCommandExecutionResult::ReadbackMismatch;
    return RomCommandExecutionResult::Ok;
}

RomCommandExecutionResult Cavs25RomCommandExecutor::executeCoreStartStep(
    const Cavs25AdspcsStep *step, uint32_t *adspcsReadback) const noexcept {
    if (!step || !adspcsReadback || step->updateMask == 0 ||
        (step->updateMask & ~kCore0UpdateMask) != 0 ||
        (step->updateValue & ~step->updateMask) != 0 ||
        (step->requiredBeforeValue & ~step->requiredBeforeMask) != 0 ||
        (step->expectedAfterValue & ~step->expectedAfterMask) != 0)
        return RomCommandExecutionResult::InvalidArgument;
    *adspcsReadback = 0;
    if (!lifecycle_ || lifecycle_->phase() != BootPhase::RomPurgeSubmitted)
        return RomCommandExecutionResult::WrongPhase;
    if (!deviceOpen()) return RomCommandExecutionResult::DeviceUnavailable;
    if (!writableMap() || !validRegister(kAdspcsOffset))
        return RomCommandExecutionResult::UnsafeMapping;

    uint32_t before = 0;
    if (!readRegister(kAdspcsOffset, &before))
        return RomCommandExecutionResult::DeviceUnavailable;
    if (before == UINT32_MAX)
        return RomCommandExecutionResult::InvalidReadback;
    if ((before & step->requiredBeforeMask) != step->requiredBeforeValue)
        return RomCommandExecutionResult::PreconditionFailed;

    const uint32_t requested =
        (before & ~step->updateMask) | step->updateValue;
    if (!writeRegister(kAdspcsOffset, requested))
        return RomCommandExecutionResult::WriteFailed;
    if (!readRegister(kAdspcsOffset, adspcsReadback))
        return RomCommandExecutionResult::DeviceUnavailable;
    if (*adspcsReadback == UINT32_MAX)
        return RomCommandExecutionResult::InvalidReadback;
    if ((*adspcsReadback & step->expectedAfterMask) !=
        step->expectedAfterValue ||
        ((*adspcsReadback ^ requested) &
         ~(step->updateMask | kCore0CpaMask)) != 0)
        return RomCommandExecutionResult::ReadbackMismatch;
    return RomCommandExecutionResult::Ok;
}

RomCommandExecutionResult Cavs25RomCommandExecutor::requestCorePower(
    uint32_t coreMask, bool powerOn,
    uint32_t *adspcsReadback) const noexcept {
    if (!adspcsReadback) return RomCommandExecutionResult::InvalidArgument;
    *adspcsReadback = UINT32_MAX;
    if (coreMask == 0 || (coreMask & ~kSupportedCoreMask) != 0)
        return RomCommandExecutionResult::InvalidArgument;
    if (!lifecycle_ || (powerOn &&
        lifecycle_->phase() != BootPhase::LoaderPrepared) ||
        (!powerOn && lifecycle_->phase() != BootPhase::LoaderPrepared &&
         lifecycle_->phase() != BootPhase::RomPurgeAccepted))
        return RomCommandExecutionResult::WrongPhase;
    if (!deviceOpen()) return RomCommandExecutionResult::DeviceUnavailable;
    if (!writableMap() || !validRegister(kAdspcsOffset))
        return RomCommandExecutionResult::UnsafeMapping;

    uint32_t before = UINT32_MAX;
    if (!readRegister(kAdspcsOffset, &before))
        return RomCommandExecutionResult::DeviceUnavailable;
    if (before == UINT32_MAX)
        return RomCommandExecutionResult::InvalidReadback;
    const uint32_t resetMask = coreMask;
    const uint32_t stallMask = coreMask << 8;
    const uint32_t resetAndStall = resetMask | stallMask;
    if ((before & resetAndStall) != resetAndStall)
        return RomCommandExecutionResult::PreconditionFailed;

    const uint32_t powerBits = coreMask << 16;
    const uint32_t cpaBits = coreMask << 24;
    if (powerOn) {
        const uint32_t powerState = before & (powerBits | cpaBits);
        if (powerState == (powerBits | cpaBits)) {
            *adspcsReadback = before;
            return RomCommandExecutionResult::Ok;
        }
        if (powerState != 0)
            return RomCommandExecutionResult::PreconditionFailed;
    } else if ((before & powerBits) == 0) {
        return RomCommandExecutionResult::PreconditionFailed;
    }

    const uint32_t requested = powerOn
        ? (before | powerBits) : (before & ~powerBits);
    if (!writeRegister(kAdspcsOffset, requested))
        return RomCommandExecutionResult::WriteFailed;
    if (!readRegister(kAdspcsOffset, adspcsReadback))
        return RomCommandExecutionResult::DeviceUnavailable;
    if (*adspcsReadback == UINT32_MAX ||
        ((*adspcsReadback ^ requested) &
         ~(powerBits | cpaBits)) != 0 ||
        ((*adspcsReadback & powerBits) !=
         (requested & powerBits)))
        return RomCommandExecutionResult::ReadbackMismatch;
    return RomCommandExecutionResult::Ok;
}

bool Cavs25RomCommandExecutor::readAdspcs(
    uint32_t *adspcsReadback) const noexcept {
    if (!adspcsReadback) return false;
    *adspcsReadback = UINT32_MAX;
    return readRegister(kAdspcsOffset, adspcsReadback) &&
        *adspcsReadback != UINT32_MAX;
}

RomCommandExecutionResult Cavs25RomCommandExecutor::acknowledgePurge(
    RomPurgePollResult pollResult, uint32_t *ackReadback) const noexcept {
    if (!ackReadback) return RomCommandExecutionResult::InvalidArgument;
    *ackReadback = 0;
    if (!lifecycle_ || lifecycle_->phase() != BootPhase::RomPrimaryCoreStarted ||
        pollResult != RomPurgePollResult::Accepted)
        return RomCommandExecutionResult::WrongPhase;
    if (!deviceOpen()) return RomCommandExecutionResult::DeviceUnavailable;
    if (!writableMap() || !validRegister(kHipcIdaOffset) ||
        !validRegister(kRomErrorOffset))
        return RomCommandExecutionResult::UnsafeMapping;

    uint32_t ack = 0;
    uint32_t romError = 0;
    if (!readRegister(kHipcIdaOffset, &ack) ||
        !readRegister(kRomErrorOffset, &romError))
        return RomCommandExecutionResult::DeviceUnavailable;
    if (ack == UINT32_MAX || romError == UINT32_MAX)
        return RomCommandExecutionResult::InvalidReadback;
    if ((ack & kCavs25HipcDoneMask) == 0 || romError != 0)
        return RomCommandExecutionResult::PreconditionFailed;
    if (!writeRegister(kHipcIdaOffset, kCavs25HipcDoneMask))
        return RomCommandExecutionResult::WriteFailed;
    if (!readRegister(kHipcIdaOffset, ackReadback))
        return RomCommandExecutionResult::DeviceUnavailable;
    if (*ackReadback == UINT32_MAX)
        return RomCommandExecutionResult::InvalidReadback;
    uint32_t postAckRomError = 0;
    if (!readRegister(kRomErrorOffset, &postAckRomError))
        return RomCommandExecutionResult::DeviceUnavailable;
    if (postAckRomError == UINT32_MAX)
        return RomCommandExecutionResult::InvalidReadback;
    if ((*ackReadback & kCavs25HipcDoneMask) != 0 || postAckRomError != 0)
        return RomCommandExecutionResult::ReadbackMismatch;
    return RomCommandExecutionResult::Ok;
}

} // namespace sof::ipc4::kernel
