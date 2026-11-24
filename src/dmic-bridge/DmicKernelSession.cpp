#include "DmicKernelSession.hpp"

namespace sof::ipc4::dmic::kernel {

using sof::ipc4::kernel::FirmwareConfigExchangeResult;
using sof::ipc4::kernel::FirmwareConfigResult;
using sof::ipc4::kernel::TransportResult;

Session::Session() noexcept
    : runner_(), idAllocator_(nullptr), ids_{},
      initialized_(false), setupComplete_(false), teardownComplete_(false),
      failed_(false), idsLeased_(false), pipelineState_(0xffffU) {}

RunnerResult Session::initializeFromFirmwareConfig(
    const sof::ipc4::kernel::FirmwareManifestView *manifest,
    const sof::ipc4::kernel::HdaDmicCapturePlan *capturePlan,
    const sof::ipc4::kernel::Cavs25BootLifecycle *lifecycle,
    sof::ipc4::kernel::Ipc4Transport *transport,
    RuntimeIdAllocator *idAllocator,
    uint32_t timeoutMilliseconds,
    uint32_t pollIntervalMicroseconds,
    FirmwareConfigExchangeResult *configResult,
    TransportResult *transportResult,
    FirmwareConfigResult *replyResult) noexcept {
    if (initialized_) return RunnerResult::InvalidState;
    if (!manifest || !capturePlan || !lifecycle || !transport || !idAllocator ||
        !configResult || !transportResult || !replyResult)
        return RunnerResult::InvalidArgument;
    *configResult = FirmwareConfigExchangeResult::InvalidArgument;
    *transportResult = TransportResult::InvalidArgument;
    *replyResult = FirmwareConfigResult::InvalidArgument;
    if (!lifecycle->canIssueIpc4()) return RunnerResult::InvalidState;

    uint16_t maxPipelineIdInclusive = 0;
    *configResult = transport->queryFirmwareConfig(
        &configRequest_, fwConfigReply_, sizeof(fwConfigReply_),
        &maxPipelineIdInclusive, timeoutMilliseconds,
        pollIntervalMicroseconds, transportResult, replyResult);
    if (*configResult != FirmwareConfigExchangeResult::Ok)
        return *configResult == FirmwareConfigExchangeResult::TransportFailure
            ? RunnerResult::TransportFailure : RunnerResult::InvalidState;

    if (!idAllocator->initialized()) {
        const RuntimeIdAllocatorResult initResult = idAllocator->initialize(
            manifest, maxPipelineIdInclusive);
        if (initResult != RuntimeIdAllocatorResult::Ok)
            return RunnerResult::InvalidState;
    } else if (idAllocator->maxPipelineIdInclusive() != maxPipelineIdInclusive) {
        return RunnerResult::InvalidState;
    }
    // Retain the driver-owned allocator even if the following allocation or
    // runner setup fails; a confirmed firmware reset must be able to clear it.
    idAllocator_ = idAllocator;

    const RuntimeIdAllocatorResult allocation = idAllocator->allocate(&ids_);
    if (allocation != RuntimeIdAllocatorResult::Ok)
        return RunnerResult::InvalidState;
    idsLeased_ = true;

    const RunnerResult runnerResult = runner_.initialize(
        manifest, ids_, *capturePlan, lifecycle, transport);
    if (runnerResult != RunnerResult::Ok) {
        if (idAllocator->release(ids_, true) != RuntimeIdAllocatorResult::Ok) {
            failed_ = true;
            return RunnerResult::RecoveryRequired;
        }
        idsLeased_ = false;
        return runnerResult;
    }

    initialized_ = true;
    return RunnerResult::Ok;
}

RunnerResult Session::setupNext(
    OperationKind *kind, TransportResult *transportResult,
    uint32_t timeoutMilliseconds,
    uint32_t pollIntervalMicroseconds) noexcept {
    if (!initialized_ || failed_ || setupComplete_ || teardownComplete_)
        return RunnerResult::InvalidState;
    const RunnerResult result = runner_.setupNext(
        kind, transportResult, timeoutMilliseconds, pollIntervalMicroseconds);
    return recordOperationResult(result, false);
}

RunnerResult Session::teardownNext(
    OperationKind *kind, TransportResult *transportResult,
    uint32_t timeoutMilliseconds,
    uint32_t pollIntervalMicroseconds) noexcept {
    if (!initialized_ || failed_ || !setupComplete_ || teardownComplete_ ||
        pipelineState_ != sof::ipc4::kernel::kIpc4PipelineStatePaused)
        return RunnerResult::InvalidState;
    const RunnerResult result = runner_.teardownNext(
        kind, transportResult, timeoutMilliseconds, pollIntervalMicroseconds);
    return recordOperationResult(result, true);
}

RunnerResult Session::setPipelineState(
    uint16_t state, TransportResult *transportResult,
    uint32_t timeoutMilliseconds,
    uint32_t pollIntervalMicroseconds) noexcept {
    if (!transportResult)
        return RunnerResult::InvalidArgument;
    *transportResult = TransportResult::InvalidArgument;
    if (!initialized_ || failed_ || !setupComplete_ || teardownComplete_)
        return RunnerResult::InvalidState;
    if ((state == sof::ipc4::kernel::kIpc4PipelineStateRunning ||
         state == sof::ipc4::kernel::kIpc4PipelineStateReset) &&
        pipelineState_ != sof::ipc4::kernel::kIpc4PipelineStatePaused)
        return RunnerResult::InvalidState;
    const RunnerResult result = runner_.setPipelineState(
        state, transportResult, timeoutMilliseconds,
        pollIntervalMicroseconds);
    if (result == RunnerResult::Ok) pipelineState_ = state;
    if (result == RunnerResult::TransportFailure ||
        result == RunnerResult::FirmwareRejected ||
        result == RunnerResult::RecoveryRequired)
        failed_ = true;
    return result;
}

RunnerResult Session::resetAfterFirmwareReset(
    bool firmwareResetConfirmed) noexcept {
    if (!firmwareResetConfirmed) return RunnerResult::InvalidState;
    if (idAllocator_) idAllocator_->resetAfterFirmwareReset();
    runner_ = OperationRunner{};
    idAllocator_ = nullptr;
    ids_ = RuntimeIds{};
    initialized_ = false;
    setupComplete_ = false;
    teardownComplete_ = false;
    failed_ = false;
    idsLeased_ = false;
    pipelineState_ = 0xffffU;
    return RunnerResult::Ok;
}

RunnerResult Session::resetAfterTeardown() noexcept {
    if (!initialized_ || failed_ || !teardownComplete_ || idsLeased_ ||
        !idAllocator_ || idAllocator_->hasActiveLease())
        return RunnerResult::InvalidState;
    runner_ = OperationRunner{};
    ids_ = RuntimeIds{};
    initialized_ = false;
    setupComplete_ = false;
    teardownComplete_ = false;
    pipelineState_ = 0xffffU;
    return RunnerResult::Ok;
}

RunnerResult Session::recordOperationResult(RunnerResult result,
                                            bool teardown) noexcept {
    if (result == RunnerResult::Complete) {
        if (!teardown) {
            setupComplete_ = true;
            return result;
        }
        if (!idAllocator_ || !idsLeased_ ||
            idAllocator_->release(ids_, true) != RuntimeIdAllocatorResult::Ok) {
            failed_ = true;
            return RunnerResult::RecoveryRequired;
        }
        idsLeased_ = false;
        teardownComplete_ = true;
        return result;
    }
    if (result != RunnerResult::Ok) failed_ = true;
    return result;
}

} // namespace sof::ipc4::dmic::kernel
