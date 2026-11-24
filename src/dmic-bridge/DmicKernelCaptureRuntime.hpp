#pragma once

#include "DmicKernelSession.hpp"
#include "HdaCaptureMappedDma.hpp"
#include "HdaCaptureTriggerPlan.hpp"
#include "HdaInputStreamAllocatorPlan.hpp"

namespace sof::ipc4::dmic::kernel {

enum class CaptureRuntimeResult : uint8_t {
    Ok,
    Complete,
    InvalidArgument,
    InvalidState,
    IpcFailure,
    RecoveryRequired,
    PeriodRead,
    NoPeriodReady,
    CapturePositionFailure,
    CaptureReadFailure,
    IocAckRequired,
    NoIocPending,
    IocFailure,
};

// Joins the already-configured IPC4 graph, HDA DMA mappings and trigger gate.
// It executes IPC state changes but delegates all HDA register actions to its
// serialized caller. The caller must keep this object driver-owned.
class CaptureRuntime final {
public:
    CaptureRuntime() noexcept;

    CaptureRuntimeResult initialize(
        Session *session,
        sof::ipc4::kernel::HdaCaptureMappedDma *dma,
        bool controllerExclusivelyOwned,
        const sof::ipc4::kernel::HdaInputStreamReservationSequence
            &reservationSequence,
        uint32_t streamControlOffset,
        uint32_t expectedStreamControlMask,
        uint32_t expectedStreamControlValue,
        uint32_t timeoutMilliseconds,
        uint32_t pollIntervalMicroseconds,
        sof::ipc4::kernel::TransportResult *transportResult) noexcept;
    CaptureRuntimeResult initializeAppleHda(
        Session *session,
        sof::ipc4::kernel::HdaCaptureMappedDma *dma,
        bool appleHdaStreamLeaseValidated,
        uint32_t streamControlOffset,
        uint32_t expectedStreamControlMask,
        uint32_t expectedStreamControlValue,
        uint32_t timeoutMilliseconds,
        uint32_t pollIntervalMicroseconds,
        sof::ipc4::kernel::TransportResult *transportResult) noexcept;

    CaptureRuntimeResult beginStart(
        sof::ipc4::kernel::HdaCaptureTriggerAction *action) noexcept;
    CaptureRuntimeResult attachPositionTracker(
        sof::ipc4::kernel::HdaCapturePositionTracker *positionTracker) noexcept;
    CaptureRuntimeResult attachIocCounter(
        sof::ipc4::kernel::HdaCaptureIocCounter *iocCounter) noexcept;
    CaptureRuntimeResult observeIocStatus(
        uint8_t streamStatus,
        sof::ipc4::kernel::HdaCaptureIocAck *ack,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CaptureRuntimeResult confirmIocAck(
        bool commandSucceeded,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CaptureRuntimeResult armPositionBaseline(
        uint32_t linkPositionBytes,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CaptureRuntimeResult observeCapturePosition(
        uint32_t linkPositionBytes,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CaptureRuntimeResult observeCapturePositionWithCount(
        uint32_t linkPositionBytes, uint64_t completedPeriodCount,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CaptureRuntimeResult readNextCompletedPeriod(
        uint8_t *destination,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CaptureRuntimeResult failCapture(
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    // Confirms SDn.RUN and transitions to StartingPipeline without issuing IPC.
    // The caller must arm the position baseline before calling startPipeline().
    CaptureRuntimeResult confirmHdaStarted(
        bool commandSucceeded, uint32_t controlReadback,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    // Sends IPC4 RUN only after the caller has armed the LPIB/IOC baseline.
    CaptureRuntimeResult startPipeline(
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction,
        sof::ipc4::kernel::TransportResult *transportResult,
        uint32_t timeoutMilliseconds,
        uint32_t pollIntervalMicroseconds) noexcept;
    CaptureRuntimeResult beginStop(
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction,
        sof::ipc4::kernel::TransportResult *transportResult,
        uint32_t timeoutMilliseconds,
        uint32_t pollIntervalMicroseconds) noexcept;
    CaptureRuntimeResult confirmHdaStopped(
        bool commandSucceeded, uint32_t controlReadback,
        bool drainFenceObserved,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;

    sof::ipc4::kernel::HdaCaptureTriggerAction beginRecovery() noexcept;
    CaptureRuntimeResult confirmRecoveryDmaStopped(
        uint32_t controlReadback, bool drainFenceObserved,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    CaptureRuntimeResult confirmRecoveryReset(
        bool firmwareResetObserved, bool streamResetObserved,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    // Clears software links only after recovery reached SafeToRelease and the
    // caller released the DMA mappings. It performs no hardware operation.
    CaptureRuntimeResult resetAfterRecovery() noexcept;
    // Clears runtime links after a normally stopped pipeline was torn down and
    // its capture DMA mappings were released. Performs no hardware I/O.
    CaptureRuntimeResult resetAfterTeardown() noexcept;

    CaptureRuntimeResult teardownNext(
        OperationKind *kind,
        sof::ipc4::kernel::TransportResult *transportResult,
        uint32_t timeoutMilliseconds,
        uint32_t pollIntervalMicroseconds) noexcept;
    bool releaseDmaAfterQuiescence() noexcept;

    sof::ipc4::kernel::HdaCaptureTriggerState state() const noexcept {
        return trigger_.state();
    }
    bool acceptsCaptureInterrupt() const noexcept;
    uint32_t lastPositionResult() const noexcept {
        return static_cast<uint32_t>(lastPositionResult_);
    }
    uint64_t positionProducedBytes() const noexcept {
        return positionTracker_ ? positionTracker_->producedBytes() : UINT64_MAX;
    }
    uint64_t positionConsumedBytes() const noexcept {
        return positionTracker_ ? positionTracker_->consumedBytes() : UINT64_MAX;
    }
    uint64_t completedIocPeriods() const noexcept {
        return iocCounter_ ? iocCounter_->completedPeriodCount() : UINT64_MAX;
    }
    bool lastPositionWasCblEndpoint() const noexcept {
        return positionTracker_ && positionTracker_->lastPositionWasCblEndpoint();
    }

private:
    CaptureRuntimeResult requestCaptureRecovery(
        CaptureRuntimeResult failure,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;

    Session *session_;
    sof::ipc4::kernel::HdaCaptureMappedDma *dma_;
    sof::ipc4::kernel::HdaCapturePositionTracker *positionTracker_;
    sof::ipc4::kernel::HdaCaptureIocCounter *iocCounter_;
    sof::ipc4::kernel::HdaCaptureTriggerPlan trigger_;
    uint32_t streamControlOffset_;
    sof::ipc4::kernel::HdaCapturePositionResult lastPositionResult_;
    bool positionBaselineArmed_;
    bool initialized_;
};

} // namespace sof::ipc4::dmic::kernel
