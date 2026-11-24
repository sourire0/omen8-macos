#pragma once

#include <stddef.h>
#include <stdint.h>
#include "Cavs25DmaPlan.hpp"
#include "Cavs25CodeLoaderSetupPlan.hpp"
#include "Cavs25RomCoreStartPlan.hpp"
#include "Cavs25RomPurgePlan.hpp"
#include "Cavs25RomStatusPlan.hpp"
#include "FirmwareImageHashKernel.hpp"

namespace sof::ipc4::kernel {

enum class BootPhase : uint8_t {
    Dormant,
    PreflightAccepted,
    DmaResourcesReady,
    LoaderPrepared,
    RomPurgeSubmitted,
    RomPrimaryCoreStarted,
    RomPurgeAccepted,
    RomInitDone,
    FirmwareDmaRunning,
    FirmwareEntered,
    LoaderStopped,
    FirmwareReady,
    Failed,
    RecoveryRequired,
};

enum class BootResult : uint8_t {
    Ok,
    InvalidArgument,
    WrongPhase,
    PreconditionFailed,
    RecoveryRequired,
};

struct BootPreflightEvidence {
    const uint8_t *firmwareImage;
    size_t firmwareImageBytes;
    bool firmwarePlatformMatched;
    bool exclusiveControllerLease;
    bool audioOwnerQuiesced;
    bool appleHdaStreamManagerAvailable;
    bool pciFunctionInD0;
    bool memoryDecodeEnabled;
    bool busMasterEnabled;
    bool hdaControllerReady;
    bool dspRegisterWindowValid;
    bool codeLoaderStreamIdle;
    bool codeLoaderDescriptorEmpty;
};

struct DmaResourceEvidence {
    DmaMappingEvidence mapping;
    uint64_t firmwarePayloadBytes;
    bool appleHdaBufferReady;
};

struct LoaderPrepareEvidence {
    bool savedControllerSnapshot;
    bool codeLoaderRunClear;
    bool streamResetComplete;
    bool unrelatedInterruptStatePreserved;
    Cavs25DmaPlan observedDmaPlan;
};

struct FirmwareDmaEvidence {
    bool runObserved;
    bool expectedStreamTag;
    bool expectedDescriptorBase;
    bool appleHdaStreamLeaseValidated;
};

struct AbortEvidence {
    bool runClear;
    bool streamResetComplete;
    bool descriptorDetached;
    bool dspCoreResetConfirmed;
};

// Pure gate/state tracker: it does not access hardware or start firmware/DMA.
// Caller serializes all methods and must source each evidence record from
// verified driver state, not from intended writes or stale snapshots.
class Cavs25BootLifecycle {
public:
    Cavs25BootLifecycle() noexcept;

    BootResult acceptPreflight(const BootPreflightEvidence &evidence) noexcept;
    BootResult acceptDmaResources(const DmaResourceEvidence &evidence) noexcept;
    BootResult acceptLoaderPrepared(const LoaderPrepareEvidence &evidence) noexcept;
    BootResult acceptAppleHdaLoaderPrepared(
        bool streamActivated, bool dmaStopped,
        uint8_t descriptorIndex, uint8_t streamTag,
        uint32_t cyclicBufferBytes,
        uint32_t firmwarePayloadBytes) noexcept;
    BootResult acceptRomPurgeSubmitted(uint32_t expectedDoorbell,
                                       uint32_t doorbellReadback) noexcept;
    BootResult acceptRomPrimaryCoreStarted(
        const Cavs25RomCoreStartSequence &sequence,
        uint32_t adspcsReadback) noexcept;
    BootResult acceptRomPurgeAck(RomPurgePollResult result) noexcept;
    BootResult acceptRomInitDone(RomPollResult result) noexcept;
    BootResult acceptFirmwareDmaStarted(
        const FirmwareDmaEvidence &evidence) noexcept;
    BootResult acceptFirmwareEntered(RomPollResult result) noexcept;
    BootResult acceptLoaderStopped(
        const Cavs25CodeLoaderDmaStopSequence &dmaStopSequence,
        const Cavs25CodeLoaderPostDmaResetSequence &resetSequence,
        uint32_t finalControlReadback) noexcept;
    BootResult acceptAppleHdaLoaderStopped(
        bool dmaStopConfirmed, bool streamDeactivated) noexcept;
    BootResult acceptFwReady(bool notificationObserved,
                             bool acknowledgementCompleted) noexcept;
    BootResult quarantineFailure(const AbortEvidence &evidence) noexcept;

    BootPhase phase() const noexcept { return phase_; }
    bool canReleaseLoaderDma() const noexcept { return canReleaseLoaderDma_; }
    bool canIssueIpc4() const noexcept { return phase_ == BootPhase::FirmwareReady; }
    const Cavs25DmaPlan *loaderDmaPlan() const noexcept {
        return dmaPlanValid_ ? &dmaPlan_ : nullptr;
    }

private:
    BootPhase phase_;
    bool canReleaseLoaderDma_;
    Cavs25DmaPlan dmaPlan_;
    bool dmaPlanValid_;
    bool appleHdaManaged_;
    uint8_t loaderDescriptorIndex_;
    uint8_t loaderStreamTag_;
    uint32_t loaderCyclicBufferBytes_;
};

} // namespace sof::ipc4::kernel
