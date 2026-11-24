#include "Cavs25RomPurgePlan.hpp"

namespace sof::ipc4::kernel {

bool buildCavs25RomPurgeDoorbell(uint8_t streamTag,
                                 uint32_t *doorbell) noexcept {
    if (!doorbell || streamTag == 0 || streamTag > 15) return false;
    *doorbell = kCavs25RomIpcControl | kCavs25RomPurgeFirmware |
        (static_cast<uint32_t>(streamTag - 1U) <<
         kCavs25RomPurgeStreamTagShift) |
        kCavs25HipcBusyMask;
    return true;
}

Cavs25RomPurgePlan::Cavs25RomPurgePlan() noexcept
    : samplesRemaining_(0), active_(false) {}

bool Cavs25RomPurgePlan::beginAckPoll(uint32_t initialAck,
                                      uint32_t initialRomError,
                                      uint32_t maximumSamples) noexcept {
    if (active_ || maximumSamples == 0 || initialAck == UINT32_MAX ||
        (initialAck & kCavs25HipcDoneMask) != 0)
        return false;
    (void)initialRomError;
    samplesRemaining_ = maximumSamples;
    active_ = true;
    return true;
}

RomPurgePollResult Cavs25RomPurgePlan::observeAck(
    uint32_t ack, uint32_t romError) noexcept {
    if (!active_) return RomPurgePollResult::WrongPhase;
    (void)romError;
    if (ack == UINT32_MAX) {
        active_ = false;
        return RomPurgePollResult::InvalidReadback;
    }
    if ((ack & kCavs25HipcDoneMask) != 0) {
        active_ = false;
        return RomPurgePollResult::Accepted;
    }
    if (ack != 0) {
        active_ = false;
        return RomPurgePollResult::UnexpectedAck;
    }
    if (--samplesRemaining_ == 0) {
        active_ = false;
        return RomPurgePollResult::TimedOut;
    }
    return RomPurgePollResult::Waiting;
}

} // namespace sof::ipc4::kernel
