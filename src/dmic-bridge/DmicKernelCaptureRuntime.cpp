#include "DmicKernelCaptureRuntime.hpp"

namespace sof::ipc4::dmic::kernel {
namespace {

constexpr uint32_t kHdaStreamRunMask = 1U << 1;
constexpr uint32_t kHdaStreamDescriptorBase = 0x80;
constexpr uint32_t kHdaStreamDescriptorStride = 0x20;
constexpr uint32_t kHdaMaximumInputStreams = 15;

bool stateAcceptsCaptureInterrupt(
    sof::ipc4::kernel::HdaCaptureTriggerState state)
    noexcept {
    using sof::ipc4::kernel::HdaCaptureTriggerState;
    return state == HdaCaptureTriggerState::StartingDma ||
           state == HdaCaptureTriggerState::StartingPipeline ||
           state == HdaCaptureTriggerState::Running ||
           state == HdaCaptureTriggerState::PausingPipeline ||
           state == HdaCaptureTriggerState::StoppingDma;
}

} // namespace

CaptureRuntime::CaptureRuntime() noexcept
    : session_(nullptr), dma_(nullptr), positionTracker_(nullptr),
      iocCounter_(nullptr), trigger_(), streamControlOffset_(0),
      lastPositionResult_(sof::ipc4::kernel::HdaCapturePositionResult::InvalidState),
      positionBaselineArmed_(false), initialized_(false) {}

CaptureRuntimeResult CaptureRuntime::initialize(
    Session *session, sof::ipc4::kernel::HdaCaptureMappedDma *dma,
    bool controllerExclusivelyOwned,
    const sof::ipc4::kernel::HdaInputStreamReservationSequence
        &reservationSequence,
    uint32_t streamControlOffset,
    uint32_t expectedStreamControlMask,
    uint32_t expectedStreamControlValue,
    uint32_t timeoutMilliseconds, uint32_t pollIntervalMicroseconds,
    sof::ipc4::kernel::TransportResult *transportResult) noexcept {
    if (initialized_ || !session || !dma || !transportResult)
        return CaptureRuntimeResult::InvalidArgument;
    *transportResult = sof::ipc4::kernel::TransportResult::InvalidArgument;
    if (!session->setupComplete() || session->teardownComplete() ||
        !dma->prepared() || !controllerExclusivelyOwned ||
        !reservationSequence.complete() || !reservationSequence.plan() ||
        reservationSequence.plan()->descriptorControlOffset !=
            streamControlOffset ||
        !sof::ipc4::kernel::hdaInputStreamReservationMatches(
            *reservationSequence.plan(),
            reservationSequence.confirmedControlReadback()) ||
        streamControlOffset < kHdaStreamDescriptorBase ||
        ((streamControlOffset - kHdaStreamDescriptorBase) %
            kHdaStreamDescriptorStride) != 0 ||
        ((streamControlOffset - kHdaStreamDescriptorBase) /
            kHdaStreamDescriptorStride) >= kHdaMaximumInputStreams)
        return CaptureRuntimeResult::InvalidState;

    sof::ipc4::kernel::HdaCaptureTriggerPlan triggerCandidate;
    if (!triggerCandidate.initialize(true, true, true, true,
                                     expectedStreamControlMask,
                                     expectedStreamControlValue))
        return CaptureRuntimeResult::InvalidState;

    const RunnerResult pauseResult = session->setPipelineState(
        sof::ipc4::kernel::kIpc4PipelineStatePaused, transportResult,
        timeoutMilliseconds, pollIntervalMicroseconds);
    if (pauseResult != RunnerResult::Ok)
        return pauseResult == RunnerResult::TransportFailure ||
                       pauseResult == RunnerResult::FirmwareRejected
            ? CaptureRuntimeResult::IpcFailure
            : CaptureRuntimeResult::InvalidState;

    trigger_ = triggerCandidate;
    session_ = session;
    dma_ = dma;
    streamControlOffset_ = streamControlOffset;
    initialized_ = true;
    return CaptureRuntimeResult::Ok;
}

CaptureRuntimeResult CaptureRuntime::initializeAppleHda(
    Session *session, sof::ipc4::kernel::HdaCaptureMappedDma *dma,
    bool appleHdaStreamLeaseValidated, uint32_t streamControlOffset,
    uint32_t expectedStreamControlMask,
    uint32_t expectedStreamControlValue, uint32_t timeoutMilliseconds,
    uint32_t pollIntervalMicroseconds,
    sof::ipc4::kernel::TransportResult *transportResult) noexcept {
    if (initialized_ || !session || !dma || !transportResult)
        return CaptureRuntimeResult::InvalidArgument;
    *transportResult = sof::ipc4::kernel::TransportResult::InvalidArgument;
    if (!appleHdaStreamLeaseValidated || !dma->prepared() ||
        !dma->usesAppleHdaBuffer() || !session->setupComplete() ||
        session->teardownComplete() ||
        streamControlOffset < kHdaStreamDescriptorBase ||
        ((streamControlOffset - kHdaStreamDescriptorBase) %
            kHdaStreamDescriptorStride) != 0 ||
        ((streamControlOffset - kHdaStreamDescriptorBase) /
            kHdaStreamDescriptorStride) >= kHdaMaximumInputStreams)
        return CaptureRuntimeResult::InvalidState;

    sof::ipc4::kernel::HdaCaptureTriggerPlan triggerCandidate;
    if (!triggerCandidate.initialize(true, true, true, true,
                                     expectedStreamControlMask,
                                     expectedStreamControlValue))
        return CaptureRuntimeResult::InvalidState;
    const RunnerResult pauseResult = session->setPipelineState(
        sof::ipc4::kernel::kIpc4PipelineStatePaused, transportResult,
        timeoutMilliseconds, pollIntervalMicroseconds);
    if (pauseResult != RunnerResult::Ok)
        return pauseResult == RunnerResult::TransportFailure ||
                       pauseResult == RunnerResult::FirmwareRejected
            ? CaptureRuntimeResult::IpcFailure
            : CaptureRuntimeResult::InvalidState;

    trigger_ = triggerCandidate;
    session_ = session;
    dma_ = dma;
    streamControlOffset_ = streamControlOffset;
    initialized_ = true;
    return CaptureRuntimeResult::Ok;
}

CaptureRuntimeResult CaptureRuntime::beginStart(
    sof::ipc4::kernel::HdaCaptureTriggerAction *action) noexcept {
    if (!initialized_ || !action || !dma_ || !dma_->prepared() ||
        !positionTracker_ || !iocCounter_)
        return CaptureRuntimeResult::InvalidState;
    if (trigger_.state() != sof::ipc4::kernel::HdaCaptureTriggerState::Ready &&
        trigger_.state() != sof::ipc4::kernel::HdaCaptureTriggerState::Stopped)
        return CaptureRuntimeResult::InvalidState;
    if (!dma_->markStreamMayBeActive())
        return CaptureRuntimeResult::InvalidState;
    positionBaselineArmed_ = false;
    *action = trigger_.beginStart();
    return *action == sof::ipc4::kernel::HdaCaptureTriggerAction::StartHdaDma
        ? CaptureRuntimeResult::Ok : CaptureRuntimeResult::InvalidState;
}

CaptureRuntimeResult CaptureRuntime::attachPositionTracker(
    sof::ipc4::kernel::HdaCapturePositionTracker *positionTracker) noexcept {
    if (!initialized_ || !positionTracker || positionTracker_ || !dma_ ||
        trigger_.state() != sof::ipc4::kernel::HdaCaptureTriggerState::Ready)
        return CaptureRuntimeResult::InvalidState;
    const sof::ipc4::kernel::HdaCaptureBdlPlan *bdl = dma_->bdlPlan();
    if (!bdl || positionTracker->failed() ||
        positionTracker->ringBytes() != dma_->ringBytes() ||
        positionTracker->periodBytes() != bdl->periodLength ||
        (iocCounter_ && positionTracker->initialCompletedPeriodCount() !=
            iocCounter_->completedPeriodCount()))
        return CaptureRuntimeResult::InvalidArgument;
    positionTracker_ = positionTracker;
    return CaptureRuntimeResult::Ok;
}

CaptureRuntimeResult CaptureRuntime::attachIocCounter(
    sof::ipc4::kernel::HdaCaptureIocCounter *iocCounter) noexcept {
    if (!initialized_ || !iocCounter || iocCounter_ ||
        trigger_.state() != sof::ipc4::kernel::HdaCaptureTriggerState::Ready)
        return CaptureRuntimeResult::InvalidState;
    if (iocCounter->failed() || iocCounter->streamStatusOffset() !=
            streamControlOffset_ + 3U ||
        (positionTracker_ && positionTracker_->initialCompletedPeriodCount() !=
            iocCounter->completedPeriodCount()))
        return CaptureRuntimeResult::InvalidArgument;
    iocCounter_ = iocCounter;
    return CaptureRuntimeResult::Ok;
}

CaptureRuntimeResult CaptureRuntime::observeIocStatus(
    uint8_t streamStatus,
    sof::ipc4::kernel::HdaCaptureIocAck *ack,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !iocCounter_ || !ack || !nextAction ||
        !stateAcceptsCaptureInterrupt(trigger_.state()))
        return CaptureRuntimeResult::InvalidState;
    *nextAction = sof::ipc4::kernel::HdaCaptureTriggerAction::None;
    *ack = {};
    const sof::ipc4::kernel::HdaCaptureIocResult result =
        iocCounter_->observeStatus(streamStatus, ack);
    if (result == sof::ipc4::kernel::HdaCaptureIocResult::AckRequired)
        return CaptureRuntimeResult::IocAckRequired;
    if (result ==
        sof::ipc4::kernel::HdaCaptureIocResult::StreamErrorAckRequired) {
        // Preserve the exact W1C acknowledgement for the serialized caller,
        // while requesting DMA stop immediately. The error remains sticky.
        return requestCaptureRecovery(CaptureRuntimeResult::IocFailure,
                                      nextAction);
    }
    if (result == sof::ipc4::kernel::HdaCaptureIocResult::NoCompletion)
        return CaptureRuntimeResult::NoIocPending;
    return requestCaptureRecovery(CaptureRuntimeResult::IocFailure, nextAction);
}

CaptureRuntimeResult CaptureRuntime::confirmIocAck(
    bool commandSucceeded,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    const bool pendingErrorAck = iocCounter_ && iocCounter_->ackPending() &&
        trigger_.state() ==
            sof::ipc4::kernel::HdaCaptureTriggerState::RecoveryStopRequired;
    if (!initialized_ || !iocCounter_ || !nextAction ||
        (!stateAcceptsCaptureInterrupt(trigger_.state()) && !pendingErrorAck))
        return CaptureRuntimeResult::InvalidState;
    *nextAction = sof::ipc4::kernel::HdaCaptureTriggerAction::None;
    const auto result = iocCounter_->confirmAck(commandSucceeded);
    if (result == sof::ipc4::kernel::HdaCaptureIocResult::Acknowledged)
        return CaptureRuntimeResult::Ok;
    if (pendingErrorAck && result ==
            sof::ipc4::kernel::HdaCaptureIocResult::StreamError)
        return CaptureRuntimeResult::IocFailure;
    return requestCaptureRecovery(CaptureRuntimeResult::IocFailure, nextAction);
}

CaptureRuntimeResult CaptureRuntime::armPositionBaseline(
    uint32_t linkPositionBytes,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !positionTracker_ || !iocCounter_ || !nextAction ||
        positionBaselineArmed_ ||
        (trigger_.state() !=
             sof::ipc4::kernel::HdaCaptureTriggerState::StartingPipeline &&
         trigger_.state() !=
             sof::ipc4::kernel::HdaCaptureTriggerState::Running))
        return CaptureRuntimeResult::InvalidState;
    *nextAction = sof::ipc4::kernel::HdaCaptureTriggerAction::None;
    // AppleHDA polling supplies its own cumulative count; the interrupt
    // counter does not advance in that mode.
    const uint64_t completed = dma_->usesAppleHdaBuffer()
        ? positionTracker_->lastCompletedPeriodCount()
        : iocCounter_->completedPeriodCount();
    if (!positionTracker_->rebase(linkPositionBytes, completed))
        return requestCaptureRecovery(
            CaptureRuntimeResult::CapturePositionFailure, nextAction);
    positionBaselineArmed_ = true;
    return CaptureRuntimeResult::Ok;
}

CaptureRuntimeResult CaptureRuntime::observeCapturePosition(
    uint32_t linkPositionBytes,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    return observeCapturePositionWithCount(
        linkPositionBytes,
        iocCounter_ ? iocCounter_->completedPeriodCount() : UINT64_MAX,
        nextAction);
}

CaptureRuntimeResult CaptureRuntime::observeCapturePositionWithCount(
    uint32_t linkPositionBytes, uint64_t completedPeriodCount,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !positionTracker_ || !iocCounter_ || !nextAction ||
        !positionBaselineArmed_ ||
        !stateAcceptsCaptureInterrupt(trigger_.state()))
        return CaptureRuntimeResult::InvalidState;
    *nextAction = sof::ipc4::kernel::HdaCaptureTriggerAction::None;
    lastPositionResult_ = positionTracker_->observe(
        linkPositionBytes, completedPeriodCount);
    if (lastPositionResult_ ==
        sof::ipc4::kernel::HdaCapturePositionResult::Ready)
        return CaptureRuntimeResult::Ok;
    return requestCaptureRecovery(CaptureRuntimeResult::CapturePositionFailure,
                                  nextAction);
}

CaptureRuntimeResult CaptureRuntime::readNextCompletedPeriod(
    uint8_t *destination,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !dma_ || !positionTracker_ || !iocCounter_ ||
        !positionBaselineArmed_ || !destination || !nextAction ||
        trigger_.state() != sof::ipc4::kernel::HdaCaptureTriggerState::Running)
        return CaptureRuntimeResult::InvalidState;
    *nextAction = sof::ipc4::kernel::HdaCaptureTriggerAction::None;
    const sof::ipc4::kernel::HdaCaptureDmaReadResult result =
        dma_->readNextCompletedPeriod(positionTracker_, destination);
    switch (result) {
        case sof::ipc4::kernel::HdaCaptureDmaReadResult::PeriodRead:
            return CaptureRuntimeResult::PeriodRead;
        case sof::ipc4::kernel::HdaCaptureDmaReadResult::NoPeriodReady:
            return CaptureRuntimeResult::NoPeriodReady;
        case sof::ipc4::kernel::HdaCaptureDmaReadResult::InvalidArgument:
        case sof::ipc4::kernel::HdaCaptureDmaReadResult::InvalidState:
            return CaptureRuntimeResult::InvalidState;
        case sof::ipc4::kernel::HdaCaptureDmaReadResult::PositionFailure:
            return requestCaptureRecovery(
                CaptureRuntimeResult::CapturePositionFailure, nextAction);
        case sof::ipc4::kernel::HdaCaptureDmaReadResult::ReadFailure:
            return requestCaptureRecovery(
                CaptureRuntimeResult::CaptureReadFailure, nextAction);
    }
    return CaptureRuntimeResult::InvalidState;
}

CaptureRuntimeResult CaptureRuntime::failCapture(
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !stateAcceptsCaptureInterrupt(trigger_.state()))
        return CaptureRuntimeResult::InvalidState;
    return requestCaptureRecovery(CaptureRuntimeResult::CaptureReadFailure,
                                  nextAction);
}

CaptureRuntimeResult CaptureRuntime::requestCaptureRecovery(
    CaptureRuntimeResult failure,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!nextAction) return CaptureRuntimeResult::InvalidArgument;
    *nextAction = trigger_.failActiveCapture();
    return *nextAction == sof::ipc4::kernel::HdaCaptureTriggerAction::StopHdaDma
        ? failure : CaptureRuntimeResult::InvalidState;
}

CaptureRuntimeResult CaptureRuntime::confirmHdaStarted(
    bool commandSucceeded, uint32_t controlReadback,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !nextAction)
        return CaptureRuntimeResult::InvalidArgument;
    *nextAction = trigger_.confirmHdaStarted(commandSucceeded,
                                              controlReadback);
    if (*nextAction !=
        sof::ipc4::kernel::HdaCaptureTriggerAction::SetPipelineRunning)
        return CaptureRuntimeResult::RecoveryRequired;

    positionBaselineArmed_ = false;
    return CaptureRuntimeResult::Ok;
}

CaptureRuntimeResult CaptureRuntime::startPipeline(
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction,
    sof::ipc4::kernel::TransportResult *transportResult,
    uint32_t timeoutMilliseconds,
    uint32_t pollIntervalMicroseconds) noexcept {
    if (!initialized_ || !nextAction || !transportResult || !session_ ||
        !positionBaselineArmed_ ||
        trigger_.state() !=
            sof::ipc4::kernel::HdaCaptureTriggerState::StartingPipeline)
        return CaptureRuntimeResult::InvalidState;
    *transportResult = sof::ipc4::kernel::TransportResult::InvalidArgument;

    const RunnerResult ipcResult = session_->setPipelineState(
        sof::ipc4::kernel::kIpc4PipelineStateRunning, transportResult,
        timeoutMilliseconds, pollIntervalMicroseconds);
    *nextAction = trigger_.confirmPipelineRunning(ipcResult == RunnerResult::Ok);
    if (ipcResult != RunnerResult::Ok)
        return CaptureRuntimeResult::IpcFailure;
    if (*nextAction !=
            sof::ipc4::kernel::HdaCaptureTriggerAction::None ||
        trigger_.state() !=
            sof::ipc4::kernel::HdaCaptureTriggerState::Running)
        return CaptureRuntimeResult::RecoveryRequired;
    return CaptureRuntimeResult::Ok;
}

bool CaptureRuntime::acceptsCaptureInterrupt() const noexcept {
    return initialized_ && stateAcceptsCaptureInterrupt(
        trigger_.state());
}

CaptureRuntimeResult CaptureRuntime::beginStop(
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction,
    sof::ipc4::kernel::TransportResult *transportResult,
    uint32_t timeoutMilliseconds,
    uint32_t pollIntervalMicroseconds) noexcept {
    if (!initialized_ || !nextAction || !transportResult || !session_)
        return CaptureRuntimeResult::InvalidArgument;
    *transportResult = sof::ipc4::kernel::TransportResult::InvalidArgument;
    *nextAction = trigger_.beginStop();
    if (*nextAction !=
        sof::ipc4::kernel::HdaCaptureTriggerAction::SetPipelinePaused)
        return CaptureRuntimeResult::InvalidState;

    const RunnerResult ipcResult = session_->setPipelineState(
        sof::ipc4::kernel::kIpc4PipelineStatePaused, transportResult,
        timeoutMilliseconds, pollIntervalMicroseconds);
    *nextAction = trigger_.confirmPipelinePaused(ipcResult == RunnerResult::Ok);
    if (ipcResult != RunnerResult::Ok)
        return CaptureRuntimeResult::IpcFailure;
    return CaptureRuntimeResult::Ok;
}

CaptureRuntimeResult CaptureRuntime::confirmHdaStopped(
    bool commandSucceeded, uint32_t controlReadback, bool drainFenceObserved,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !nextAction || !dma_ ||
        trigger_.state() !=
            sof::ipc4::kernel::HdaCaptureTriggerState::StoppingDma)
        return CaptureRuntimeResult::InvalidArgument;
    const bool runClear = (controlReadback & kHdaStreamRunMask) == 0;
    const bool dmaQuiescenceConfirmed = runClear && drainFenceObserved &&
        dma_->confirmStreamQuiescent(controlReadback, drainFenceObserved);
    *nextAction = trigger_.confirmHdaStopped(
        commandSucceeded && dmaQuiescenceConfirmed, controlReadback,
        drainFenceObserved);
    if (*nextAction == sof::ipc4::kernel::HdaCaptureTriggerAction::RetainMappings)
        return CaptureRuntimeResult::RecoveryRequired;
    return trigger_.state() == sof::ipc4::kernel::HdaCaptureTriggerState::Stopped
        ? CaptureRuntimeResult::Complete : CaptureRuntimeResult::InvalidState;
}

sof::ipc4::kernel::HdaCaptureTriggerAction
CaptureRuntime::beginRecovery() noexcept {
    return initialized_ ? trigger_.beginRecovery()
                        : sof::ipc4::kernel::HdaCaptureTriggerAction::None;
}

CaptureRuntimeResult CaptureRuntime::confirmRecoveryDmaStopped(
    uint32_t controlReadback, bool drainFenceObserved,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !nextAction || !dma_ ||
        trigger_.state() !=
            sof::ipc4::kernel::HdaCaptureTriggerState::RecoveryStopRequired)
        return CaptureRuntimeResult::InvalidArgument;
    const bool runClear = (controlReadback & kHdaStreamRunMask) == 0;
    const bool quiescent = runClear && drainFenceObserved &&
        dma_->confirmStreamQuiescent(controlReadback, drainFenceObserved);
    *nextAction = trigger_.confirmRecoveryDmaStopped(quiescent,
                                                      drainFenceObserved);
    return quiescent
        ? CaptureRuntimeResult::Ok : CaptureRuntimeResult::RecoveryRequired;
}

CaptureRuntimeResult CaptureRuntime::confirmRecoveryReset(
    bool firmwareResetObserved, bool streamResetObserved,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !nextAction)
        return CaptureRuntimeResult::InvalidArgument;
    *nextAction = trigger_.confirmRecoveryFirmwareReset(
        firmwareResetObserved, streamResetObserved);
    return trigger_.state() ==
                   sof::ipc4::kernel::HdaCaptureTriggerState::SafeToRelease
        ? CaptureRuntimeResult::Complete
        : CaptureRuntimeResult::RecoveryRequired;
}

CaptureRuntimeResult CaptureRuntime::resetAfterRecovery() noexcept {
    if (!initialized_ || !session_ || !dma_ ||
        trigger_.state() !=
            sof::ipc4::kernel::HdaCaptureTriggerState::SafeToRelease ||
        dma_->prepared())
        return CaptureRuntimeResult::InvalidState;

    if (session_->resetAfterFirmwareReset(true) != RunnerResult::Ok)
        return CaptureRuntimeResult::RecoveryRequired;

    session_ = nullptr;
    dma_ = nullptr;
    positionTracker_ = nullptr;
    iocCounter_ = nullptr;
    trigger_ = sof::ipc4::kernel::HdaCaptureTriggerPlan{};
    streamControlOffset_ = 0;
    positionBaselineArmed_ = false;
    initialized_ = false;
    return CaptureRuntimeResult::Complete;
}

CaptureRuntimeResult CaptureRuntime::resetAfterTeardown() noexcept {
    if (!initialized_ || !session_ || !dma_ ||
        trigger_.state() !=
            sof::ipc4::kernel::HdaCaptureTriggerState::Stopped ||
        dma_->prepared() || !dma_->quiescentForRelease())
        return CaptureRuntimeResult::InvalidState;

    if (session_->resetAfterTeardown() != RunnerResult::Ok)
        return CaptureRuntimeResult::RecoveryRequired;

    session_ = nullptr;
    dma_ = nullptr;
    positionTracker_ = nullptr;
    iocCounter_ = nullptr;
    trigger_ = sof::ipc4::kernel::HdaCaptureTriggerPlan{};
    streamControlOffset_ = 0;
    positionBaselineArmed_ = false;
    initialized_ = false;
    return CaptureRuntimeResult::Complete;
}

CaptureRuntimeResult CaptureRuntime::teardownNext(
    OperationKind *kind,
    sof::ipc4::kernel::TransportResult *transportResult,
    uint32_t timeoutMilliseconds,
    uint32_t pollIntervalMicroseconds) noexcept {
    if (!initialized_ || !session_ ||
        trigger_.state() != sof::ipc4::kernel::HdaCaptureTriggerState::Stopped)
        return CaptureRuntimeResult::InvalidState;
    const RunnerResult result = session_->teardownNext(
        kind, transportResult, timeoutMilliseconds, pollIntervalMicroseconds);
    if (result == RunnerResult::Complete)
        return CaptureRuntimeResult::Complete;
    if (result == RunnerResult::Ok)
        return CaptureRuntimeResult::Ok;
    return result == RunnerResult::TransportFailure ||
                   result == RunnerResult::FirmwareRejected ||
                   result == RunnerResult::RecoveryRequired
        ? CaptureRuntimeResult::RecoveryRequired
        : CaptureRuntimeResult::InvalidState;
}

bool CaptureRuntime::releaseDmaAfterQuiescence() noexcept {
    if (!initialized_ || !dma_ || !trigger_.mappingsMayBeReleased())
        return false;
    return dma_->releaseAfterQuiescence();
}

} // namespace sof::ipc4::dmic::kernel
