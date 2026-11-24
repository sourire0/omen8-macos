#include "Cavs25RomStatusPlan.hpp"

namespace sof::ipc4::kernel {

Cavs25RomStatusPlan::Cavs25RomStatusPlan() noexcept
    : checkpoint_(RomCheckpoint::InitDone), samplesRemaining_(0),
      active_(false) {}

bool Cavs25RomStatusPlan::begin(RomCheckpoint checkpoint,
                                uint32_t maximumSamples,
                                uint32_t baselineStatus,
                                uint32_t baselineError) noexcept {
    if (active_ || maximumSamples == 0 ||
        (checkpoint != RomCheckpoint::InitDone &&
         checkpoint != RomCheckpoint::FirmwareEntered) ||
        baselineStatus == UINT32_MAX || baselineError != 0 ||
        (baselineStatus & kCavs25RomHaltedMask) != 0 ||
        ((baselineStatus & kCavs25RomModuleMask) >>
             kCavs25RomModuleShift) != kCavs25RomModuleRom)
        return false;
    const uint32_t baselineState = baselineStatus & kCavs25RomStateMask;
    if (checkpoint == RomCheckpoint::FirmwareEntered &&
        baselineState != kCavs25RomInitDone)
        return false;
    checkpoint_ = checkpoint;
    samplesRemaining_ = maximumSamples;
    active_ = true;
    return true;
}

RomPollResult Cavs25RomStatusPlan::observe(uint32_t status,
                                           uint32_t error) noexcept {
    if (!active_) return RomPollResult::WrongPhase;
    if (status == UINT32_MAX || error == UINT32_MAX) {
        active_ = false;
        return RomPollResult::InvalidReadback;
    }
    if ((status & kCavs25RomHaltedMask) != 0) {
        active_ = false;
        return RomPollResult::Halted;
    }
    // HDA_DSP_SRAM_REG_ROM_ERROR and HDA_DSP_SRAM_REG_FW_STATUS alias at
    // offset 0x80004 on cAVS 2.5. The word is an error code before handoff,
    // but a live firmware-status field after FW_ENTERED.
    if (checkpoint_ == RomCheckpoint::InitDone && error != 0) {
        active_ = false;
        return RomPollResult::RomError;
    }

    const uint32_t state = status & kCavs25RomStateMask;
    const uint32_t target = checkpoint_ == RomCheckpoint::InitDone
        ? kCavs25RomInitDone : kCavs25RomFirmwareEntered;
    if (state == target) {
        active_ = false;
        return checkpoint_ == RomCheckpoint::InitDone
            ? RomPollResult::InitDoneReached
            : RomPollResult::FirmwareEnteredReached;
    }
    if (checkpoint_ == RomCheckpoint::InitDone &&
        state == kCavs25RomFirmwareEntered) {
        active_ = false;
        return RomPollResult::UnexpectedProgress;
    }

    if (--samplesRemaining_ == 0) {
        active_ = false;
        return RomPollResult::TimedOut;
    }
    return RomPollResult::Waiting;
}

} // namespace sof::ipc4::kernel
