#include "Cavs25BootLifecycle.hpp"

namespace sof::ipc4::kernel {
namespace {

bool dmaPlansMatch(const Cavs25DmaPlan &expected,
                   const Cavs25DmaPlan &observed) noexcept {
    if (expected.cyclicBufferLength != observed.cyclicBufferLength ||
        expected.descriptorBaseLow != observed.descriptorBaseLow ||
        expected.descriptorBaseHigh != observed.descriptorBaseHigh ||
        expected.lastValidIndex != observed.lastValidIndex ||
        expected.reserved != observed.reserved ||
        expected.entries[0].addressLow != observed.entries[0].addressLow ||
        expected.entries[0].addressHigh != observed.entries[0].addressHigh ||
        expected.entries[0].byteCount != observed.entries[0].byteCount ||
        expected.entries[0].interruptOnCompletion !=
            observed.entries[0].interruptOnCompletion ||
        expected.entries[1].addressLow != observed.entries[1].addressLow ||
        expected.entries[1].addressHigh != observed.entries[1].addressHigh ||
        expected.entries[1].byteCount != observed.entries[1].byteCount ||
        expected.entries[1].interruptOnCompletion !=
            observed.entries[1].interruptOnCompletion)
        return false;
    return true;
}

} // namespace

Cavs25BootLifecycle::Cavs25BootLifecycle() noexcept
    : phase_(BootPhase::Dormant), canReleaseLoaderDma_(false),
      dmaPlanValid_(false), appleHdaManaged_(false),
      loaderDescriptorIndex_(UINT8_MAX), loaderStreamTag_(0),
      loaderCyclicBufferBytes_(0) {}

BootResult Cavs25BootLifecycle::acceptPreflight(
    const BootPreflightEvidence &evidence) noexcept {
    if (phase_ != BootPhase::Dormant) return BootResult::WrongPhase;
    if (verifyKnownAdlIpc4V212Image(evidence.firmwareImage,
                                   evidence.firmwareImageBytes) !=
            FirmwareImageHashResult::Ok ||
        !evidence.firmwarePlatformMatched ||
        (!(evidence.exclusiveControllerLease && evidence.audioOwnerQuiesced) &&
         !evidence.appleHdaStreamManagerAvailable) ||
        !evidence.pciFunctionInD0 || !evidence.memoryDecodeEnabled ||
        !evidence.busMasterEnabled || !evidence.hdaControllerReady ||
        !evidence.dspRegisterWindowValid || !evidence.codeLoaderStreamIdle ||
        !evidence.codeLoaderDescriptorEmpty)
        return BootResult::PreconditionFailed;
    phase_ = BootPhase::PreflightAccepted;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::acceptDmaResources(
    const DmaResourceEvidence &evidence) noexcept {
    if (phase_ != BootPhase::PreflightAccepted) return BootResult::WrongPhase;
    if (!evidence.mapping.firmwareContentsSynchronized)
        return BootResult::PreconditionFailed;
    if (evidence.appleHdaBufferReady) {
        if (!evidence.mapping.firmwareMappingLive ||
            evidence.mapping.descriptorMappingLive ||
            evidence.mapping.descriptorContentsSynchronized ||
            evidence.firmwarePayloadBytes == 0 ||
            evidence.firmwarePayloadBytes > evidence.mapping.mappedBytes)
            return BootResult::PreconditionFailed;
        appleHdaManaged_ = true;
        dmaPlanValid_ = false;
        canReleaseLoaderDma_ = false;
        phase_ = BootPhase::DmaResourcesReady;
        return BootResult::Ok;
    }
    if (!evidence.mapping.descriptorContentsSynchronized)
        return BootResult::PreconditionFailed;
    if (planCavs25CodeLoaderDma(evidence.mapping,
                                evidence.firmwarePayloadBytes,
                                &dmaPlan_) != DmaPlanResult::Ok)
        return BootResult::PreconditionFailed;
    dmaPlanValid_ = true;
    canReleaseLoaderDma_ = false;
    phase_ = BootPhase::DmaResourcesReady;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::acceptLoaderPrepared(
    const LoaderPrepareEvidence &evidence) noexcept {
    if (phase_ != BootPhase::DmaResourcesReady) return BootResult::WrongPhase;
    if (!dmaPlanValid_ || !evidence.savedControllerSnapshot ||
        !dmaPlansMatch(dmaPlan_, evidence.observedDmaPlan) ||
        !evidence.codeLoaderRunClear ||
        !evidence.streamResetComplete ||
        !evidence.unrelatedInterruptStatePreserved)
        return BootResult::PreconditionFailed;
    phase_ = BootPhase::LoaderPrepared;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::acceptAppleHdaLoaderPrepared(
    bool streamActivated, bool dmaStopped, uint8_t descriptorIndex,
    uint8_t streamTag, uint32_t cyclicBufferBytes,
    uint32_t firmwarePayloadBytes) noexcept {
    if (phase_ != BootPhase::DmaResourcesReady) return BootResult::WrongPhase;
    if (!appleHdaManaged_ || !streamActivated || !dmaStopped ||
        descriptorIndex >= 30 || streamTag == 0 || streamTag > 15 ||
        cyclicBufferBytes == 0 || firmwarePayloadBytes == 0 ||
        cyclicBufferBytes < firmwarePayloadBytes)
        return BootResult::PreconditionFailed;
    loaderDescriptorIndex_ = descriptorIndex;
    loaderStreamTag_ = streamTag;
    loaderCyclicBufferBytes_ = cyclicBufferBytes;
    phase_ = BootPhase::LoaderPrepared;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::acceptRomPurgeAck(
    RomPurgePollResult result) noexcept {
    if (phase_ != BootPhase::RomPrimaryCoreStarted)
        return BootResult::WrongPhase;
    if (result != RomPurgePollResult::Accepted)
        return BootResult::PreconditionFailed;
    phase_ = BootPhase::RomPurgeAccepted;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::acceptRomPurgeSubmitted(
    uint32_t expectedDoorbell, uint32_t doorbellReadback) noexcept {
    if (phase_ != BootPhase::LoaderPrepared) return BootResult::WrongPhase;
    if ((expectedDoorbell & kCavs25HipcBusyMask) == 0 ||
        doorbellReadback == UINT32_MAX ||
        (doorbellReadback & kCavs25HipcMessageMask) !=
            (expectedDoorbell & kCavs25HipcMessageMask))
        return BootResult::PreconditionFailed;

    bool canonicalRequest = false;
    for (uint8_t tag = 1; tag <= 15; ++tag) {
        uint32_t canonical = 0;
        if (buildCavs25RomPurgeDoorbell(tag, &canonical) &&
            canonical == expectedDoorbell) {
            canonicalRequest = true;
            break;
        }
    }
    if (!canonicalRequest) return BootResult::PreconditionFailed;
    phase_ = BootPhase::RomPurgeSubmitted;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::acceptRomPrimaryCoreStarted(
    const Cavs25RomCoreStartSequence &sequence,
    uint32_t adspcsReadback) noexcept {
    if (phase_ != BootPhase::RomPurgeSubmitted)
        return BootResult::WrongPhase;
    constexpr uint32_t kPrimaryCoreReset = 1U << 0;
    constexpr uint32_t kPrimaryCoreStall = 1U << 8;
    constexpr uint32_t kPrimaryCorePowerRequest = 1U << 16;
    constexpr uint32_t kPrimaryCorePowerAck = 1U << 24;
    constexpr uint32_t kExpectedMask = kPrimaryCoreReset |
        kPrimaryCoreStall | kPrimaryCorePowerRequest | kPrimaryCorePowerAck;
    constexpr uint32_t kExpectedValue = kPrimaryCorePowerRequest |
        kPrimaryCorePowerAck;
    if (!sequence.complete() || adspcsReadback == UINT32_MAX ||
        (adspcsReadback & kExpectedMask) != kExpectedValue)
        return BootResult::PreconditionFailed;
    phase_ = BootPhase::RomPrimaryCoreStarted;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::acceptRomInitDone(
    RomPollResult result) noexcept {
    if (phase_ != BootPhase::RomPurgeAccepted) return BootResult::WrongPhase;
    if (result != RomPollResult::InitDoneReached)
        return BootResult::PreconditionFailed;
    phase_ = BootPhase::RomInitDone;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::acceptFirmwareDmaStarted(
    const FirmwareDmaEvidence &evidence) noexcept {
    if (phase_ != BootPhase::RomInitDone) return BootResult::WrongPhase;
    if (!evidence.runObserved) return BootResult::PreconditionFailed;
    if (appleHdaManaged_ &&
        (!evidence.expectedStreamTag ||
         !evidence.appleHdaStreamLeaseValidated)) {
        canReleaseLoaderDma_ = false;
        phase_ = BootPhase::RecoveryRequired;
        return BootResult::RecoveryRequired;
    }
    if (!appleHdaManaged_ &&
        (!evidence.expectedStreamTag || !evidence.expectedDescriptorBase)) {
        canReleaseLoaderDma_ = false;
        phase_ = BootPhase::RecoveryRequired;
        return BootResult::RecoveryRequired;
    }
    phase_ = BootPhase::FirmwareDmaRunning;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::acceptAppleHdaLoaderStopped(
    bool dmaStopConfirmed, bool streamDeactivated) noexcept {
    if (phase_ != BootPhase::FirmwareEntered) return BootResult::WrongPhase;
    if (!appleHdaManaged_ || !dmaStopConfirmed || !streamDeactivated) {
        canReleaseLoaderDma_ = false;
        phase_ = BootPhase::RecoveryRequired;
        return BootResult::RecoveryRequired;
    }
    canReleaseLoaderDma_ = true;
    phase_ = BootPhase::LoaderStopped;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::acceptFirmwareEntered(
    RomPollResult result) noexcept {
    if (phase_ != BootPhase::FirmwareDmaRunning) return BootResult::WrongPhase;
    if (result != RomPollResult::FirmwareEnteredReached)
        return BootResult::PreconditionFailed;
    phase_ = BootPhase::FirmwareEntered;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::acceptLoaderStopped(
    const Cavs25CodeLoaderDmaStopSequence &dmaStopSequence,
    const Cavs25CodeLoaderPostDmaResetSequence &resetSequence,
    uint32_t finalControlReadback) noexcept {
    if (phase_ != BootPhase::FirmwareEntered) return BootResult::WrongPhase;
    if (!dmaStopSequence.complete() || !resetSequence.complete() ||
        !resetSequence.confirmsResetControlReadback(finalControlReadback)) {
        canReleaseLoaderDma_ = false;
        phase_ = BootPhase::RecoveryRequired;
        return BootResult::RecoveryRequired;
    }
    canReleaseLoaderDma_ = true;
    phase_ = BootPhase::LoaderStopped;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::acceptFwReady(
    bool notificationObserved, bool acknowledgementCompleted) noexcept {
    if (phase_ != BootPhase::LoaderStopped) return BootResult::WrongPhase;
    if (!notificationObserved || !acknowledgementCompleted)
        return BootResult::PreconditionFailed;
    phase_ = BootPhase::FirmwareReady;
    return BootResult::Ok;
}

BootResult Cavs25BootLifecycle::quarantineFailure(
    const AbortEvidence &evidence) noexcept {
    if (phase_ == BootPhase::Dormant || phase_ == BootPhase::Failed ||
        phase_ == BootPhase::FirmwareReady)
        return BootResult::WrongPhase;
    if (!evidence.runClear || !evidence.streamResetComplete ||
        !evidence.descriptorDetached || !evidence.dspCoreResetConfirmed) {
        canReleaseLoaderDma_ = false;
        phase_ = BootPhase::RecoveryRequired;
        return BootResult::RecoveryRequired;
    }
    canReleaseLoaderDma_ = true;
    phase_ = BootPhase::Failed;
    return BootResult::Ok;
}

} // namespace sof::ipc4::kernel
