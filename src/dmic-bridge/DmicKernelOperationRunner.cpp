#include "DmicKernelOperationRunner.hpp"

namespace sof::ipc4::dmic::kernel {

OperationRunner::OperationRunner() noexcept
    : plan_(), lifecycle_(nullptr), transport_(nullptr), initialized_(false),
      failed_(false), setupComplete_(false), teardownComplete_(false) {
    reply_.primary = 0;
    reply_.extension = 0;
    reply_.firmwareStatus = 0;
    reply_.payloadBytes = 0;
}

RunnerResult OperationRunner::setPipelineState(
    uint16_t state, TransportResult *transportResult,
    uint32_t timeoutMilliseconds, uint32_t pollIntervalMicroseconds) noexcept {
    if (!transportResult)
        return RunnerResult::InvalidArgument;
    *transportResult = TransportResult::InvalidArgument;
    if (!initialized_ || !setupComplete_ || teardownComplete_)
        return RunnerResult::InvalidState;
    if (failed_ || !lifecycle_ || !lifecycle_->canIssueIpc4())
        return RunnerResult::InvalidState;
    if (state != sof::ipc4::kernel::kIpc4PipelineStateReset &&
        state != sof::ipc4::kernel::kIpc4PipelineStatePaused &&
        state != sof::ipc4::kernel::kIpc4PipelineStateRunning)
        return RunnerResult::InvalidArgument;

    const sof::ipc4::kernel::Result buildResult =
        sof::ipc4::kernel::setTwoPipelineState(
            state != sof::ipc4::kernel::kIpc4PipelineStatePaused ? plan_.frontendPipelineInstance() : plan_.pipelineInstance(),
            state != sof::ipc4::kernel::kIpc4PipelineStatePaused ? plan_.pipelineInstance() : plan_.frontendPipelineInstance(),
            state, &message_);
    if (buildResult != sof::ipc4::kernel::Result::Ok)
        return RunnerResult::InvalidState;
    *transportResult = transport_->send(&message_, 0, nullptr, &reply_,
                                        timeoutMilliseconds,
                                        pollIntervalMicroseconds);
    if (*transportResult != TransportResult::Ok) {
        failed_ = true;
        return *transportResult == TransportResult::FirmwareRejected
            ? RunnerResult::FirmwareRejected : RunnerResult::TransportFailure;
    }
    return RunnerResult::Ok;
}

RunnerResult OperationRunner::initialize(const FirmwareManifestView *manifest,
                                         const RuntimeIds &ids,
                                         const sof::ipc4::kernel::HdaDmicCapturePlan &capturePlan,
                                         const Cavs25BootLifecycle *lifecycle,
                                         Ipc4Transport *transport) noexcept {
    if (initialized_) return RunnerResult::InvalidState;
    if (!transport || !lifecycle) return RunnerResult::InvalidArgument;
    if (!lifecycle->canIssueIpc4()) return RunnerResult::InvalidState;
    const OperationPlanResult result = plan_.initialize(manifest, ids,
                                                        capturePlan);
    if (result != OperationPlanResult::Ok)
        return result == OperationPlanResult::InvalidArgument
            ? RunnerResult::InvalidArgument : RunnerResult::InvalidState;
    lifecycle_ = lifecycle;
    transport_ = transport;
    initialized_ = true;
    return RunnerResult::Ok;
}

RunnerResult OperationRunner::setupNext(
    OperationKind *kind, TransportResult *transportResult,
    uint32_t timeoutMilliseconds, uint32_t pollIntervalMicroseconds) noexcept {
    return runOne(false, kind, transportResult, timeoutMilliseconds,
                  pollIntervalMicroseconds);
}

RunnerResult OperationRunner::teardownNext(
    OperationKind *kind, TransportResult *transportResult,
    uint32_t timeoutMilliseconds, uint32_t pollIntervalMicroseconds) noexcept {
    return runOne(true, kind, transportResult, timeoutMilliseconds,
                  pollIntervalMicroseconds);
}

RunnerResult OperationRunner::runOne(
    bool teardown, OperationKind *kind, TransportResult *transportResult,
    uint32_t timeoutMilliseconds, uint32_t pollIntervalMicroseconds) noexcept {
    if (!initialized_ || !kind || !transportResult)
        return RunnerResult::InvalidArgument;
    if (failed_) return RunnerResult::RecoveryRequired;
    if (!lifecycle_ || !lifecycle_->canIssueIpc4())
        return RunnerResult::InvalidState;

    const OperationPlanResult planResult = teardown
        ? plan_.nextTeardown(&message_, kind)
        : plan_.nextSetup(&message_, kind);
    if (planResult == OperationPlanResult::Complete) {
        if (teardown)
            teardownComplete_ = true;
        else
            setupComplete_ = true;
        return RunnerResult::Complete;
    }
    if (planResult == OperationPlanResult::WrongPhase)
        return RunnerResult::WrongPhase;
    if (planResult == OperationPlanResult::ConfirmationRequired)
        return RunnerResult::InvalidState;
    if (planResult != OperationPlanResult::Ok)
        return RunnerResult::InvalidState;

    *transportResult = transport_->send(&message_, 0, nullptr, &reply_,
                                        timeoutMilliseconds,
                                        pollIntervalMicroseconds);
    if (*transportResult != TransportResult::Ok) {
        failed_ = true;
        (void)plan_.confirmCurrent(false);
        return *transportResult == TransportResult::FirmwareRejected
            ? RunnerResult::FirmwareRejected : RunnerResult::TransportFailure;
    }

    if (plan_.confirmCurrent(true) != OperationPlanResult::Ok) {
        failed_ = true;
        return RunnerResult::InvalidState;
    }
    return RunnerResult::Ok;
}

} // namespace sof::ipc4::dmic::kernel
