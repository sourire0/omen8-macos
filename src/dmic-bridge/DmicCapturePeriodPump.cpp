#include "DmicCapturePeriodPump.hpp"

namespace sof::ipc4::dmic::kernel {

CapturePeriodPump::CapturePeriodPump() noexcept
    : runtime_(nullptr), publisher_(nullptr), initialized_(false),
      positionBaselineArmed_(false), awaitingAck_(false), recoveryAck_(false),
      pendingAck_{},
      pendingAction_(sof::ipc4::kernel::HdaCaptureTriggerAction::None) {}

bool CapturePeriodPump::initialize(CaptureRuntime *runtime,
                                   DmicCapturePeriodPublisher *publisher) noexcept {
    if (initialized_ || !runtime || !publisher || !publisher->ready() ||
        awaitingAck_ ||
        runtime->state() != sof::ipc4::kernel::HdaCaptureTriggerState::Ready)
        return false;
    runtime_ = runtime;
    publisher_ = publisher;
    initialized_ = true;
    return true;
}

CapturePumpResult CapturePeriodPump::armPositionBaseline(
    uint32_t linkPositionBytes,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !runtime_ || !nextAction || awaitingAck_ ||
        positionBaselineArmed_)
        return CapturePumpResult::InvalidState;
    const CaptureRuntimeResult result =
        runtime_->armPositionBaseline(linkPositionBytes, nextAction);
    if (result != CaptureRuntimeResult::Ok)
        return result == CaptureRuntimeResult::RecoveryRequired ||
                       result == CaptureRuntimeResult::CapturePositionFailure
            ? CapturePumpResult::RecoveryRequired
            : CapturePumpResult::InvalidState;
    positionBaselineArmed_ = true;
    return CapturePumpResult::BaselineArmed;
}

bool CapturePeriodPump::resetForStart() noexcept {
    if (!initialized_ || !runtime_ || awaitingAck_ ||
        runtime_->state() !=
            sof::ipc4::kernel::HdaCaptureTriggerState::StartingDma)
        return false;
    positionBaselineArmed_ = false;
    return true;
}

bool CapturePeriodPump::resetAfterRuntimeRecovery() noexcept {
    if (!initialized_ || !runtime_ || awaitingAck_ ||
        runtime_->state() !=
            sof::ipc4::kernel::HdaCaptureTriggerState::Uninitialized)
        return false;
    runtime_ = nullptr;
    publisher_ = nullptr;
    initialized_ = false;
    positionBaselineArmed_ = false;
    recoveryAck_ = false;
    pendingAck_ = {};
    pendingAction_ = sof::ipc4::kernel::HdaCaptureTriggerAction::None;
    return true;
}

CapturePumpResult CapturePeriodPump::beginIocInterrupt(
    uint8_t streamStatus,
    sof::ipc4::kernel::HdaCaptureIocAck *ack,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !runtime_ || !ack || !nextAction ||
        !positionBaselineArmed_ || awaitingAck_ ||
        !runtime_->acceptsCaptureInterrupt())
        return CapturePumpResult::InvalidState;

    *ack = {};
    *nextAction = sof::ipc4::kernel::HdaCaptureTriggerAction::None;
    publisher_->resetLastResult();
    const CaptureRuntimeResult result =
        runtime_->observeIocStatus(streamStatus, ack, nextAction);
    if (result == CaptureRuntimeResult::NoIocPending)
        return CapturePumpResult::NoInterrupt;
    if (result == CaptureRuntimeResult::IocAckRequired) {
        awaitingAck_ = true;
        recoveryAck_ = false;
        pendingAck_ = *ack;
        pendingAction_ = *nextAction;
        return CapturePumpResult::AckRequired;
    }
    if (result == CaptureRuntimeResult::IocFailure && ack->widthBytes != 0) {
        awaitingAck_ = true;
        recoveryAck_ = true;
        pendingAck_ = *ack;
        pendingAction_ = *nextAction;
        return CapturePumpResult::RecoveryAckRequired;
    }
    if (result == CaptureRuntimeResult::RecoveryRequired ||
        result == CaptureRuntimeResult::IocFailure)
        return CapturePumpResult::RecoveryRequired;
    return CapturePumpResult::InvalidState;
}

CapturePumpResult CapturePeriodPump::finishIocAck(
    const sof::ipc4::kernel::HdaCaptureIocAck *ackWritten,
    bool ackWriteSucceeded, bool positionReadSucceeded,
    uint32_t linkPositionBytes,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !runtime_ || !publisher_ || !ackWritten ||
        !nextAction || !awaitingAck_)
        return CapturePumpResult::InvalidState;

    awaitingAck_ = false;
    *nextAction = pendingAction_;
    const bool wasRecoveryAck = recoveryAck_;
    const auto actionFromInterrupt = pendingAction_;
    const auto requestedAck = pendingAck_;
    recoveryAck_ = false;
    pendingAck_ = {};
    pendingAction_ = sof::ipc4::kernel::HdaCaptureTriggerAction::None;
    const bool ackMatches = ackWritten->offset == requestedAck.offset &&
        ackWritten->value == requestedAck.value &&
        ackWritten->widthBytes == requestedAck.widthBytes &&
        ackWritten->reserved[0] == requestedAck.reserved[0] &&
        ackWritten->reserved[1] == requestedAck.reserved[1];
    const CaptureRuntimeResult ackResult =
        runtime_->confirmIocAck(ackWriteSucceeded && ackMatches, nextAction);
    if (wasRecoveryAck || !ackMatches ||
        ackResult != CaptureRuntimeResult::Ok) {
        if (*nextAction == sof::ipc4::kernel::HdaCaptureTriggerAction::None)
            *nextAction = actionFromInterrupt;
        return CapturePumpResult::RecoveryRequired;
    }

    if (!positionReadSucceeded) {
        (void)runtime_->failCapture(nextAction);
        return *nextAction ==
                       sof::ipc4::kernel::HdaCaptureTriggerAction::StopHdaDma
            ? CapturePumpResult::RecoveryRequired
            : CapturePumpResult::InvalidState;
    }

    const CaptureRuntimeResult positionResult =
        runtime_->observeCapturePosition(linkPositionBytes, nextAction);
    if (positionResult != CaptureRuntimeResult::Ok)
        return positionResult == CaptureRuntimeResult::RecoveryRequired ||
                       positionResult == CaptureRuntimeResult::CapturePositionFailure
            ? CapturePumpResult::RecoveryRequired
            : CapturePumpResult::InvalidState;

    // HDA is running before the IPC pipeline is triggered. Acknowledge and
    // track any IOC in that window, but do not publish samples until the
    // runtime confirms the pipeline is Running.
    if (runtime_->state() !=
        sof::ipc4::kernel::HdaCaptureTriggerState::Running)
        return CapturePumpResult::NoPeriodReady;

    bool publishedAny = false;
    // A single BCIS bit can represent several periods if interrupts coalesce.
    // Drain the bounded HDA ring queue before waiting for another interrupt.
    constexpr uint32_t kMaximumPeriodsPerInterrupt = 192;
    for (uint32_t period = 0; period < kMaximumPeriodsPerInterrupt; ++period) {
        const IOReturn publishResult =
            publisher_->publishCompletedPeriod(runtime_, nextAction);
        if (publishResult == kIOReturnSuccess) {
            publishedAny = true;
            continue;
        }
        if (publishResult == kIOReturnNotReady)
            return publishedAny ? CapturePumpResult::PeriodPublished
                                : CapturePumpResult::NoPeriodReady;
        if (*nextAction == sof::ipc4::kernel::HdaCaptureTriggerAction::None)
            (void)runtime_->failCapture(nextAction);
        return *nextAction ==
                       sof::ipc4::kernel::HdaCaptureTriggerAction::StopHdaDma
            ? CapturePumpResult::RecoveryRequired
            : CapturePumpResult::AudioFailure;
    }
    (void)runtime_->failCapture(nextAction);
    return *nextAction == sof::ipc4::kernel::HdaCaptureTriggerAction::StopHdaDma
        ? CapturePumpResult::RecoveryRequired
        : CapturePumpResult::AudioFailure;
}

CapturePumpResult CapturePeriodPump::processAppleHdaIoc(
    uint32_t linkPositionBytes,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !runtime_ || !publisher_ || !nextAction ||
        !positionBaselineArmed_ || awaitingAck_ ||
        !runtime_->acceptsCaptureInterrupt())
        return CapturePumpResult::InvalidState;

    sof::ipc4::kernel::HdaCaptureIocAck ack{};
    const CapturePumpResult beginResult = beginIocInterrupt(
        sof::ipc4::kernel::kHdaStreamStatusBufferCompletion, &ack,
        nextAction);
    if (beginResult != CapturePumpResult::AckRequired)
        return beginResult;

    // AppleHDA's registered callback is issued only after its workloop has
    // observed and acknowledged BCIS. Complete the software counter without
    // a second direct write to SDn.STS.
    return finishIocAck(&ack, true, true, linkPositionBytes, nextAction);
}

CapturePumpResult CapturePeriodPump::processAppleHdaPoll(
    uint32_t linkPositionBytes, uint64_t completedPeriodCount,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !runtime_ || !publisher_ || !nextAction ||
        !positionBaselineArmed_ || awaitingAck_ ||
        !runtime_->acceptsCaptureInterrupt())
        return CapturePumpResult::InvalidState;
    *nextAction = sof::ipc4::kernel::HdaCaptureTriggerAction::None;
    const CaptureRuntimeResult positionResult =
        runtime_->observeCapturePositionWithCount(
            linkPositionBytes, completedPeriodCount, nextAction);
    if (positionResult != CaptureRuntimeResult::Ok)
        return positionResult == CaptureRuntimeResult::RecoveryRequired ||
                       positionResult == CaptureRuntimeResult::CapturePositionFailure
            ? CapturePumpResult::RecoveryRequired
            : CapturePumpResult::InvalidState;
    if (runtime_->state() !=
        sof::ipc4::kernel::HdaCaptureTriggerState::Running)
        return CapturePumpResult::NoPeriodReady;

    bool publishedAny = false;
    constexpr uint32_t kMaximumPeriodsPerPoll = 192;
    for (uint32_t period = 0; period < kMaximumPeriodsPerPoll; ++period) {
        const IOReturn publishResult =
            publisher_->publishCompletedPeriod(runtime_, nextAction);
        if (publishResult == kIOReturnSuccess) {
            publishedAny = true;
            continue;
        }
        if (publishResult == kIOReturnNotReady)
            return publishedAny ? CapturePumpResult::PeriodPublished
                                : CapturePumpResult::NoPeriodReady;
        if (*nextAction == sof::ipc4::kernel::HdaCaptureTriggerAction::None)
            (void)runtime_->failCapture(nextAction);
        return *nextAction ==
                       sof::ipc4::kernel::HdaCaptureTriggerAction::StopHdaDma
            ? CapturePumpResult::RecoveryRequired
            : CapturePumpResult::AudioFailure;
    }
    (void)runtime_->failCapture(nextAction);
    return *nextAction ==
                   sof::ipc4::kernel::HdaCaptureTriggerAction::StopHdaDma
        ? CapturePumpResult::RecoveryRequired
        : CapturePumpResult::AudioFailure;
}

CapturePumpResult CapturePeriodPump::reportCaptureIoFailure(
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !runtime_ || !nextAction || awaitingAck_)
        return CapturePumpResult::InvalidState;
    *nextAction = sof::ipc4::kernel::HdaCaptureTriggerAction::None;
    const CaptureRuntimeResult failure = runtime_->failCapture(nextAction);
    return failure == CaptureRuntimeResult::CaptureReadFailure &&
                   *nextAction ==
                       sof::ipc4::kernel::HdaCaptureTriggerAction::StopHdaDma
        ? CapturePumpResult::RecoveryRequired
        : CapturePumpResult::InvalidState;
}

bool CapturePeriodPump::ready() const noexcept {
    return initialized_ && runtime_ && positionBaselineArmed_ &&
           !awaitingAck_ && runtime_->state() ==
               sof::ipc4::kernel::HdaCaptureTriggerState::Running;
}

bool CapturePeriodPump::configured() const noexcept {
    return initialized_ && runtime_ && publisher_ && publisher_->ready();
}

} // namespace sof::ipc4::dmic::kernel
