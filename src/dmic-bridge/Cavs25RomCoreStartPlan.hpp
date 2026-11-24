#pragma once

#include <stdint.h>

namespace sof::ipc4::kernel {

enum class RomCoreStartPlanResult : uint8_t {
    Ok,
    InvalidArgument,
    OwnershipNotProven,
    PurgeNotConfirmed,
    InvalidInitialState,
};

struct Cavs25AdspcsStep {
    uint32_t requiredBeforeMask;
    uint32_t requiredBeforeValue;
    uint32_t updateMask;
    uint32_t updateValue;
    uint32_t expectedAfterMask;
    uint32_t expectedAfterValue;
};

struct Cavs25RomCoreStartPlan {
    Cavs25AdspcsStep steps[4];
    uint8_t stepCount;
    uint8_t reserved[3];
};

enum class RomCoreStartSequenceResult : uint8_t {
    Ready,
    WaitingForHardware,
    Complete,
    InvalidArgument,
    InvalidState,
    NotInitialized,
    InvalidReadback,
    UnexpectedReadback,
};

// Readback-gated sequencer for the pure plan. It yields one masked update
// only when that step's precondition is observed and requires post-write
// confirmation before yielding the next update. It has no MMIO access.
class Cavs25RomCoreStartSequence final {
public:
    Cavs25RomCoreStartSequence() noexcept;

    RomCoreStartPlanResult initialize(
        uint32_t initialAdspcs, bool exclusiveControllerLease,
        uint32_t expectedPurgeDoorbell,
        uint32_t purgeDoorbellReadback) noexcept;
    RomCoreStartSequenceResult nextUpdate(
        uint32_t adspcsReadback, Cavs25AdspcsStep *update) noexcept;
    RomCoreStartSequenceResult confirmUpdate(
        uint32_t adspcsReadback) noexcept;

    uint8_t completedSteps() const noexcept { return completedSteps_; }
    bool complete() const noexcept {
        return initialized_ && !failed_ && !awaitingConfirmation_ &&
               completedSteps_ == plan_.stepCount;
    }

private:
    Cavs25RomCoreStartPlan plan_;
    uint8_t completedSteps_;
    bool initialized_;
    bool awaitingConfirmation_;
    bool failed_;
};

// Describes the cAVS 2.5 Core 0 power/reset/stall sequence from the local
// Loader.c reference. It produces masked ADSPCS update steps only; no MMIO is
// performed. Each step requires a fresh readback before advancing.
RomCoreStartPlanResult planCavs25RomPrimaryCoreStart(
    uint32_t initialAdspcs, bool exclusiveControllerLease,
    uint32_t expectedPurgeDoorbell, uint32_t purgeDoorbellReadback,
    Cavs25RomCoreStartPlan *out) noexcept;

} // namespace sof::ipc4::kernel
