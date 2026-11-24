#pragma once

#include "DmicCapturePeriodPublisher.hpp"
#include "DmicKernelCaptureRuntime.hpp"

namespace sof::ipc4::dmic::kernel {

enum class CapturePumpResult : uint8_t {
    BaselineArmed,
    NoInterrupt,
    AckRequired,
    RecoveryAckRequired,
    PeriodPublished,
    NoPeriodReady,
    RecoveryRequired,
    AudioFailure,
    InvalidState,
};

// Two-stage, serialized IOC path. Configure while the runtime is Ready and
// install the interrupt binding disabled. After HDA RUN, arm the position
// baseline before starting the IPC pipeline, then enable the binding after
// the runtime reaches Running. IOC events during StartingPipeline, if polled,
// are acknowledged and tracked but not published. The controller reads SDn.STS, calls
// beginIocInterrupt(), performs the returned W1C ACK itself, reads LPIB, then
// calls finishIocAck(). This class has no MMIO or interrupt-source access.
class CapturePeriodPump final {
public:
    CapturePeriodPump() noexcept;

    bool initialize(CaptureRuntime *runtime,
                    DmicCapturePeriodPublisher *publisher) noexcept;
    bool resetForStart() noexcept;
    // Clears the pump after its runtime has completed confirmed recovery and
    // reset to Uninitialized. It performs no hardware operation.
    bool resetAfterRuntimeRecovery() noexcept;
    CapturePumpResult armPositionBaseline(
        uint32_t linkPositionBytes,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CapturePumpResult beginIocInterrupt(
        uint8_t streamStatus,
        sof::ipc4::kernel::HdaCaptureIocAck *ack,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CapturePumpResult finishIocAck(
        const sof::ipc4::kernel::HdaCaptureIocAck *ackWritten,
        bool ackWriteSucceeded, bool positionReadSucceeded,
        uint32_t linkPositionBytes,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    // Processes an IOC callback delivered by AppleHDA after it has handled
    // the stream-status acknowledgement. The callback owner must serialize
    // this with start/stop and invoke it outside any interrupt/workloop gate.
    CapturePumpResult processAppleHdaIoc(
        uint32_t linkPositionBytes,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CapturePumpResult processAppleHdaPoll(
        uint32_t linkPositionBytes, uint64_t completedPeriodCount,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CapturePumpResult reportCaptureIoFailure(
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    bool ready() const noexcept;
    bool configured() const noexcept;

private:
    CaptureRuntime *runtime_;
    DmicCapturePeriodPublisher *publisher_;
    bool initialized_;
    bool positionBaselineArmed_;
    bool awaitingAck_;
    bool recoveryAck_;
    sof::ipc4::kernel::HdaCaptureIocAck pendingAck_;
    sof::ipc4::kernel::HdaCaptureTriggerAction pendingAction_;
};

} // namespace sof::ipc4::dmic::kernel
