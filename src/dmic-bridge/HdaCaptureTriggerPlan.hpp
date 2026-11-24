#pragma once

#include <stdint.h>

namespace sof::ipc4::kernel {

enum class HdaCaptureTriggerAction : uint8_t {
    None,
    StartHdaDma,
    SetPipelineRunning,
    SetPipelinePaused,
    StopHdaDma,
    ResetDsp,
    RetainMappings,
};

enum class HdaCaptureTriggerState : uint8_t {
    Uninitialized,
    Ready,
    StartingDma,
    StartingPipeline,
    Running,
    PausingPipeline,
    StoppingDma,
    Stopped,
    RecoveryStopRequired,
    RecoveryResetRequired,
    SafeToRelease,
};

// Pure ordering tracker for SOF's IPC4 trigger sequence. It describes actions
// only; the caller must perform each action and provide trustworthy readback.
class HdaCaptureTriggerPlan final {
public:
    HdaCaptureTriggerPlan() noexcept;

    bool initialize(bool exclusiveOwnership, bool streamReserved,
                    bool mappingsPrepared,
                    bool pipelineConfiguredAndPaused,
                    uint32_t expectedStreamControlMask,
                    uint32_t expectedStreamControlValue) noexcept;
    HdaCaptureTriggerAction beginStart() noexcept;
    HdaCaptureTriggerAction confirmHdaStarted(
        bool commandSucceeded, uint32_t streamControlReadback) noexcept;
    HdaCaptureTriggerAction confirmPipelineRunning(bool transactionSucceeded)
        noexcept;
    HdaCaptureTriggerAction beginStop() noexcept;
    HdaCaptureTriggerAction confirmPipelinePaused(bool transactionSucceeded)
        noexcept;
    HdaCaptureTriggerAction confirmHdaStopped(
        bool commandSucceeded, uint32_t streamControlReadback,
        bool drainFenceObserved) noexcept;
    HdaCaptureTriggerAction failActiveCapture() noexcept;

    HdaCaptureTriggerAction beginRecovery() noexcept;
    HdaCaptureTriggerAction confirmRecoveryDmaStopped(
        bool runReadbackClear, bool drainFenceObserved) noexcept;
    HdaCaptureTriggerAction confirmRecoveryFirmwareReset(
        bool firmwareResetObserved, bool streamResetObserved) noexcept;

    HdaCaptureTriggerState state() const noexcept { return state_; }
    bool mappingsMayBeReleased() const noexcept;

private:
    HdaCaptureTriggerState state_;
    uint32_t expectedStreamControlMask_;
    uint32_t expectedStreamControlValue_;
};

} // namespace sof::ipc4::kernel
