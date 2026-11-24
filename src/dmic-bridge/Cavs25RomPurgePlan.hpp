#pragma once

#include <stdint.h>

namespace sof::ipc4::kernel {

enum class RomPurgePollResult : uint8_t {
    Waiting,
    Accepted,
    TimedOut,
    RomError,
    InvalidReadback,
    UnexpectedAck,
    WrongPhase,
};

constexpr uint32_t kCavs25RomIpcControl = 0x01000000U;
constexpr uint32_t kCavs25RomPurgeFirmware = 0x00004000U;
constexpr uint32_t kCavs25RomPurgeStreamTagShift = 9;
constexpr uint32_t kCavs25HipcBusyMask = 1U << 31;
constexpr uint32_t kCavs25HipcDoneMask = 1U << 31;
constexpr uint32_t kCavs25HipcMessageMask = 0x7fffffffU;

bool buildCavs25RomPurgeDoorbell(uint8_t streamTag,
                                 uint32_t *doorbell) noexcept;

// Bounded pure poll gate. The caller owns the MMIO reads and delay.
class Cavs25RomPurgePlan final {
public:
    Cavs25RomPurgePlan() noexcept;

    bool beginAckPoll(uint32_t initialAck, uint32_t initialRomError,
                      uint32_t maximumSamples) noexcept;
    RomPurgePollResult observeAck(uint32_t ack, uint32_t romError) noexcept;
    uint32_t samplesRemaining() const noexcept { return samplesRemaining_; }
    bool active() const noexcept { return active_; }

private:
    uint32_t samplesRemaining_;
    bool active_;
};

} // namespace sof::ipc4::kernel
