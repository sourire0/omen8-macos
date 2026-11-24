#include "HdaCaptureTriggerPlan.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kStreamRunMask = 1U << 1;
constexpr uint32_t kStreamResetMask = 1U << 0;
constexpr uint32_t kStreamTagMask = 0x0fU << 20;
constexpr uint32_t kStreamInterruptEnables = (1U << 2) | (1U << 3) | (1U << 4);
constexpr uint32_t kExpectedStreamControlMask = kStreamRunMask |
    kStreamResetMask | kStreamInterruptEnables | kStreamTagMask;
constexpr uint32_t kAppleHdaStreamControlMask = kStreamRunMask |
    kStreamResetMask | kStreamTagMask;

} // namespace

HdaCaptureTriggerPlan::HdaCaptureTriggerPlan() noexcept
    : state_(HdaCaptureTriggerState::Uninitialized),
      expectedStreamControlMask_(0), expectedStreamControlValue_(0) {}

bool HdaCaptureTriggerPlan::initialize(bool exclusiveOwnership,
                                       bool streamReserved,
                                       bool mappingsPrepared,
                                       bool pipelineConfiguredAndPaused,
                                       uint32_t expectedStreamControlMask,
                                       uint32_t expectedStreamControlValue) noexcept {
    const uint32_t expectedTag =
        (expectedStreamControlValue & kStreamTagMask) >> 20;
    if (state_ != HdaCaptureTriggerState::Uninitialized ||
        !exclusiveOwnership || !streamReserved || !mappingsPrepared ||
        !pipelineConfiguredAndPaused ||
        (expectedStreamControlMask != kExpectedStreamControlMask &&
         expectedStreamControlMask != kAppleHdaStreamControlMask) ||
        expectedTag == 0 || expectedTag > 15 ||
        (expectedStreamControlValue & kStreamRunMask) == 0 ||
        (expectedStreamControlValue & kStreamResetMask) != 0 ||
        (expectedStreamControlValue & ~expectedStreamControlMask) != 0)
        return false;
    expectedStreamControlMask_ = expectedStreamControlMask;
    expectedStreamControlValue_ = expectedStreamControlValue;
    state_ = HdaCaptureTriggerState::Ready;
    return true;
}

HdaCaptureTriggerAction HdaCaptureTriggerPlan::beginStart() noexcept {
    if (state_ != HdaCaptureTriggerState::Ready &&
        state_ != HdaCaptureTriggerState::Stopped)
        return HdaCaptureTriggerAction::None;
    state_ = HdaCaptureTriggerState::StartingDma;
    return HdaCaptureTriggerAction::StartHdaDma;
}

HdaCaptureTriggerAction HdaCaptureTriggerPlan::confirmHdaStarted(
    bool commandSucceeded, uint32_t streamControlReadback) noexcept {
    if (state_ != HdaCaptureTriggerState::StartingDma)
        return HdaCaptureTriggerAction::None;
    if (!commandSucceeded || streamControlReadback == UINT32_MAX ||
        (streamControlReadback & expectedStreamControlMask_) !=
            expectedStreamControlValue_) {
        state_ = HdaCaptureTriggerState::RecoveryStopRequired;
        return HdaCaptureTriggerAction::StopHdaDma;
    }
    state_ = HdaCaptureTriggerState::StartingPipeline;
    return HdaCaptureTriggerAction::SetPipelineRunning;
}

HdaCaptureTriggerAction HdaCaptureTriggerPlan::confirmPipelineRunning(
    bool transactionSucceeded) noexcept {
    if (state_ != HdaCaptureTriggerState::StartingPipeline)
        return HdaCaptureTriggerAction::None;
    if (!transactionSucceeded) {
        state_ = HdaCaptureTriggerState::RecoveryStopRequired;
        return HdaCaptureTriggerAction::StopHdaDma;
    }
    state_ = HdaCaptureTriggerState::Running;
    return HdaCaptureTriggerAction::None;
}

HdaCaptureTriggerAction HdaCaptureTriggerPlan::beginStop() noexcept {
    if (state_ != HdaCaptureTriggerState::Running)
        return HdaCaptureTriggerAction::None;
    state_ = HdaCaptureTriggerState::PausingPipeline;
    return HdaCaptureTriggerAction::SetPipelinePaused;
}

HdaCaptureTriggerAction HdaCaptureTriggerPlan::confirmPipelinePaused(
    bool transactionSucceeded) noexcept {
    if (state_ != HdaCaptureTriggerState::PausingPipeline)
        return HdaCaptureTriggerAction::None;
    if (!transactionSucceeded) {
        state_ = HdaCaptureTriggerState::RecoveryStopRequired;
        return HdaCaptureTriggerAction::StopHdaDma;
    }
    state_ = HdaCaptureTriggerState::StoppingDma;
    return HdaCaptureTriggerAction::StopHdaDma;
}

HdaCaptureTriggerAction HdaCaptureTriggerPlan::confirmHdaStopped(
    bool commandSucceeded, uint32_t streamControlReadback,
    bool drainFenceObserved) noexcept {
    if (state_ != HdaCaptureTriggerState::StoppingDma)
        return HdaCaptureTriggerAction::None;
    const uint32_t stoppedMask = expectedStreamControlMask_ & ~kStreamRunMask;
    const uint32_t stoppedValue = expectedStreamControlValue_ & ~kStreamRunMask;
    if (!commandSucceeded || streamControlReadback == UINT32_MAX ||
        (streamControlReadback & kStreamRunMask) != 0 ||
        (streamControlReadback & stoppedMask) != stoppedValue ||
        !drainFenceObserved) {
        state_ = HdaCaptureTriggerState::RecoveryStopRequired;
        return HdaCaptureTriggerAction::RetainMappings;
    }
    state_ = HdaCaptureTriggerState::Stopped;
    return HdaCaptureTriggerAction::None;
}

HdaCaptureTriggerAction HdaCaptureTriggerPlan::failActiveCapture() noexcept {
    if (state_ != HdaCaptureTriggerState::StartingDma &&
        state_ != HdaCaptureTriggerState::StartingPipeline &&
        state_ != HdaCaptureTriggerState::Running &&
        state_ != HdaCaptureTriggerState::PausingPipeline &&
        state_ != HdaCaptureTriggerState::StoppingDma)
        return HdaCaptureTriggerAction::None;
    state_ = HdaCaptureTriggerState::RecoveryStopRequired;
    return HdaCaptureTriggerAction::StopHdaDma;
}

HdaCaptureTriggerAction HdaCaptureTriggerPlan::beginRecovery() noexcept {
    if (state_ == HdaCaptureTriggerState::RecoveryStopRequired)
        return HdaCaptureTriggerAction::StopHdaDma;
    if (state_ == HdaCaptureTriggerState::RecoveryResetRequired)
        return HdaCaptureTriggerAction::ResetDsp;
    return HdaCaptureTriggerAction::None;
}

HdaCaptureTriggerAction HdaCaptureTriggerPlan::confirmRecoveryDmaStopped(
    bool runReadbackClear, bool drainFenceObserved) noexcept {
    if (state_ != HdaCaptureTriggerState::RecoveryStopRequired)
        return HdaCaptureTriggerAction::None;
    if (!runReadbackClear || !drainFenceObserved)
        return HdaCaptureTriggerAction::RetainMappings;
    // IPC state may be uncertain after a failed/ambiguous trigger transaction.
    // Keep all resources until the DSP and HDA stream reset are both confirmed.
    state_ = HdaCaptureTriggerState::RecoveryResetRequired;
    return HdaCaptureTriggerAction::ResetDsp;
}

HdaCaptureTriggerAction HdaCaptureTriggerPlan::confirmRecoveryFirmwareReset(
    bool firmwareResetObserved, bool streamResetObserved) noexcept {
    if (state_ != HdaCaptureTriggerState::RecoveryResetRequired)
        return HdaCaptureTriggerAction::None;
    if (!firmwareResetObserved || !streamResetObserved)
        return HdaCaptureTriggerAction::RetainMappings;
    state_ = HdaCaptureTriggerState::SafeToRelease;
    return HdaCaptureTriggerAction::None;
}

bool HdaCaptureTriggerPlan::mappingsMayBeReleased() const noexcept {
    return state_ == HdaCaptureTriggerState::Stopped ||
           state_ == HdaCaptureTriggerState::SafeToRelease;
}

} // namespace sof::ipc4::kernel
