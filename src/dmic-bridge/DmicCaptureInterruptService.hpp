#pragma once

#include "DmicCapturePeriodPump.hpp"
#include "HdaCaptureMmioExecutor.hpp"

namespace sof::ipc4::dmic::kernel {

enum class CaptureInterruptServiceResult : uint8_t {
    NoInterrupt,
    PeriodPublished,
    NoPeriodReady,
    RecoveryRequired,
    AudioFailure,
    InvalidState,
};

// Serialized, non-gated worker body for one HDA stream IOC. A controller may
// defer this from its interrupt event source to a non-gated worker; do not call
// it from the IOInterruptEventSource action because the period DMA copy must
// run outside the workloop gate. HPIPC4Controller installs it disabled during
// explicit capture Start and enables it after the IPC4 pipeline reaches Running.
class DmicCaptureInterruptService final {
public:
    DmicCaptureInterruptService() noexcept;
    bool initialize(sof::ipc4::kernel::HdaCaptureMmioExecutor *mmio,
                    CapturePeriodPump *pump,
                    uint32_t streamControlOffset) noexcept;
    bool initializeAppleHda(CapturePeriodPump *pump,
                            uint32_t streamControlOffset,
                            uint32_t ringBytes, uint32_t periodBytes) noexcept;
    bool armAppleHdaPollBaseline(uint32_t linkPositionBytes,
                                 uint64_t completedPeriodCount) noexcept;

    CaptureInterruptServiceResult serviceOne(
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CaptureInterruptServiceResult serviceAppleHdaOne(
        uint32_t linkPositionBytes,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CaptureInterruptServiceResult serviceAppleHdaPosition(
        uint32_t linkPositionBytes,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    bool lastStatusReadSucceeded() const noexcept { return lastStatusReadSucceeded_; }
    uint8_t lastStreamStatus() const noexcept { return lastStreamStatus_; }
    CapturePumpResult lastPumpResult() const noexcept { return lastPumpResult_; }
    bool lastAckSucceeded() const noexcept { return lastAckSucceeded_; }
    bool lastPositionReadSucceeded() const noexcept { return lastPositionReadSucceeded_; }
    uint32_t lastLinkPosition() const noexcept { return lastLinkPosition_; }
    // Clears borrowed pointers after the owner has uninstalled/quiesced the
    // interrupt binding and reset the pump. This method performs no I/O.
    bool resetAfterRecovery(bool interruptBindingUninstalled) noexcept;
    bool ready() const noexcept;

private:
    sof::ipc4::kernel::HdaCaptureMmioExecutor *mmio_;
    CapturePeriodPump *pump_;
    uint32_t streamControlOffset_;
    bool initialized_;
    bool appleHdaManaged_;
    bool lastStatusReadSucceeded_;
    uint8_t lastStreamStatus_;
    CapturePumpResult lastPumpResult_;
    bool lastAckSucceeded_;
    bool lastPositionReadSucceeded_;
    uint32_t lastLinkPosition_;
    uint32_t appleHdaRingBytes_;
    uint32_t appleHdaPeriodBytes_;
    uint32_t appleHdaLastPolledPosition_;
    uint64_t appleHdaPolledBytes_;
    uint64_t appleHdaBaselinePeriodCount_;
    bool appleHdaPollBaselineArmed_;
};

} // namespace sof::ipc4::dmic::kernel
