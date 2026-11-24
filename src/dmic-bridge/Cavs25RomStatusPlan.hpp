#pragma once

#include <stdint.h>

namespace sof::ipc4::kernel {

enum class RomCheckpoint : uint8_t {
    InitDone,
    FirmwareEntered,
};

enum class RomPollResult : uint8_t {
    Waiting,
    InitDoneReached,
    FirmwareEnteredReached,
    TimedOut,
    RomError,
    Halted,
    InvalidReadback,
    UnexpectedProgress,
    WrongPhase,
};

constexpr uint32_t kCavs25RomStateMask = 0x00ffffffU;
constexpr uint32_t kCavs25RomModuleMask = 0x70000000U;
constexpr uint32_t kCavs25RomModuleShift = 28;
constexpr uint32_t kCavs25RomHaltedMask = 1U << 31;
constexpr uint32_t kCavs25RomModuleRom = 0;
constexpr uint32_t kCavs25RomInit = 0x00000000U;
constexpr uint32_t kCavs25RomInitDone = 0x00000001U;
constexpr uint32_t kCavs25RomFirmwareEntered = 0x00000005U;

// Bounded, pure polling gate for cAVS FSR and the aliased ROM_ERROR/FW_STATUS
// word. The caller owns delays and MMIO reads; no timeout is success.
class Cavs25RomStatusPlan final {
public:
    Cavs25RomStatusPlan() noexcept;

    bool begin(RomCheckpoint checkpoint, uint32_t maximumSamples,
               uint32_t baselineStatus, uint32_t baselineError) noexcept;
    RomPollResult observe(uint32_t status, uint32_t error) noexcept;
    uint32_t samplesRemaining() const noexcept { return samplesRemaining_; }
    bool active() const noexcept { return active_; }

private:
    RomCheckpoint checkpoint_;
    uint32_t samplesRemaining_;
    bool active_;
};

} // namespace sof::ipc4::kernel
