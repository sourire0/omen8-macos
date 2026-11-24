#include "DmicCaptureInterruptService.hpp"

namespace sof::ipc4::dmic::kernel {

DmicCaptureInterruptService::DmicCaptureInterruptService() noexcept
    : mmio_(nullptr), pump_(nullptr), streamControlOffset_(0),
      initialized_(false), appleHdaManaged_(false),
      lastStatusReadSucceeded_(false),
      lastStreamStatus_(0), lastPumpResult_(CapturePumpResult::InvalidState),
      lastAckSucceeded_(false), lastPositionReadSucceeded_(false),
      lastLinkPosition_(UINT32_MAX), appleHdaRingBytes_(0),
      appleHdaPeriodBytes_(0), appleHdaLastPolledPosition_(0),
      appleHdaPolledBytes_(0), appleHdaBaselinePeriodCount_(0),
      appleHdaPollBaselineArmed_(false) {}

bool DmicCaptureInterruptService::initialize(
    sof::ipc4::kernel::HdaCaptureMmioExecutor *mmio,
    CapturePeriodPump *pump, uint32_t streamControlOffset) noexcept {
    if (initialized_ || !mmio || !pump || streamControlOffset < 0x80U ||
        ((streamControlOffset - 0x80U) % 0x20U) != 0 ||
        ((streamControlOffset - 0x80U) / 0x20U) >= 15U)
        return false;
    mmio_ = mmio;
    pump_ = pump;
    streamControlOffset_ = streamControlOffset;
    initialized_ = true;
    appleHdaManaged_ = false;
    return true;
}

bool DmicCaptureInterruptService::initializeAppleHda(
    CapturePeriodPump *pump, uint32_t streamControlOffset,
    uint32_t ringBytes, uint32_t periodBytes) noexcept {
    if (initialized_ || !pump || streamControlOffset < 0x80U ||
        ((streamControlOffset - 0x80U) % 0x20U) != 0 ||
        ((streamControlOffset - 0x80U) / 0x20U) >= 15U ||
        ringBytes < periodBytes * 2U || periodBytes == 0 ||
        (ringBytes % periodBytes) != 0)
        return false;
    mmio_ = nullptr;
    pump_ = pump;
    streamControlOffset_ = streamControlOffset;
    appleHdaRingBytes_ = ringBytes;
    appleHdaPeriodBytes_ = periodBytes;
    appleHdaPolledBytes_ = 0;
    appleHdaPollBaselineArmed_ = false;
    appleHdaManaged_ = true;
    initialized_ = true;
    return true;
}

bool DmicCaptureInterruptService::armAppleHdaPollBaseline(
    uint32_t linkPositionBytes, uint64_t completedPeriodCount) noexcept {
    if (!initialized_ || !appleHdaManaged_ ||
        linkPositionBytes > appleHdaRingBytes_)
        return false;
    appleHdaLastPolledPosition_ =
        linkPositionBytes == appleHdaRingBytes_ ? 0 : linkPositionBytes;
    appleHdaPolledBytes_ = 0;
    appleHdaBaselinePeriodCount_ = completedPeriodCount;
    appleHdaPollBaselineArmed_ = true;
    return true;
}

CaptureInterruptServiceResult DmicCaptureInterruptService::serviceOne(
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || appleHdaManaged_ || !mmio_ || !pump_ || !nextAction)
        return CaptureInterruptServiceResult::InvalidState;
    *nextAction = sof::ipc4::kernel::HdaCaptureTriggerAction::None;
    lastStatusReadSucceeded_ = false;
    lastStreamStatus_ = 0;
    lastPumpResult_ = CapturePumpResult::InvalidState;
    lastAckSucceeded_ = false;
    lastPositionReadSucceeded_ = false;
    lastLinkPosition_ = UINT32_MAX;

    uint8_t streamStatus = 0;
    lastStatusReadSucceeded_ = mmio_->readStreamStatus(streamControlOffset_, &streamStatus);
    if (!lastStatusReadSucceeded_) {
        lastPumpResult_ = pump_->reportCaptureIoFailure(nextAction);
        return lastPumpResult_ ==
                       CapturePumpResult::RecoveryRequired
            ? CaptureInterruptServiceResult::RecoveryRequired
            : CaptureInterruptServiceResult::InvalidState;
    }
    lastStreamStatus_ = streamStatus;

    sof::ipc4::kernel::HdaCaptureIocAck ack{};
    const CapturePumpResult beginResult =
        pump_->beginIocInterrupt(streamStatus, &ack, nextAction);
    lastPumpResult_ = beginResult;
    if (beginResult == CapturePumpResult::NoInterrupt)
        return CaptureInterruptServiceResult::NoInterrupt;
    if (beginResult == CapturePumpResult::RecoveryRequired)
        return CaptureInterruptServiceResult::RecoveryRequired;
    if (beginResult != CapturePumpResult::AckRequired &&
        beginResult != CapturePumpResult::RecoveryAckRequired)
        return CaptureInterruptServiceResult::InvalidState;

    lastAckSucceeded_ = mmio_->writeIocAck(streamControlOffset_, ack);
    uint32_t linkPosition = UINT32_MAX;
    lastPositionReadSucceeded_ = lastAckSucceeded_ &&
        mmio_->readLinkPosition(streamControlOffset_, &linkPosition);
    if (lastPositionReadSucceeded_) lastLinkPosition_ = linkPosition;
    const CapturePumpResult finishResult = pump_->finishIocAck(
        &ack, lastAckSucceeded_, lastPositionReadSucceeded_, linkPosition, nextAction);
    lastPumpResult_ = finishResult;

    switch (finishResult) {
        case CapturePumpResult::PeriodPublished:
            return CaptureInterruptServiceResult::PeriodPublished;
        case CapturePumpResult::NoPeriodReady:
            return CaptureInterruptServiceResult::NoPeriodReady;
        case CapturePumpResult::RecoveryRequired:
            return CaptureInterruptServiceResult::RecoveryRequired;
        case CapturePumpResult::AudioFailure:
            return CaptureInterruptServiceResult::AudioFailure;
        default:
            return CaptureInterruptServiceResult::InvalidState;
    }
}

CaptureInterruptServiceResult DmicCaptureInterruptService::serviceAppleHdaOne(
    uint32_t linkPositionBytes,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !appleHdaManaged_ || !pump_ || !nextAction)
        return CaptureInterruptServiceResult::InvalidState;
    lastStatusReadSucceeded_ = true; // AppleHDA delivered an IOC callback.
    lastStreamStatus_ = sof::ipc4::kernel::kHdaStreamStatusBufferCompletion;
    lastAckSucceeded_ = true;        // AppleHDA already acknowledged BCIS.
    lastPositionReadSucceeded_ = linkPositionBytes != UINT32_MAX;
    lastLinkPosition_ = linkPositionBytes;
    if (!lastPositionReadSucceeded_) {
        lastPumpResult_ = pump_->reportCaptureIoFailure(nextAction);
    } else {
        lastPumpResult_ = pump_->processAppleHdaIoc(
            linkPositionBytes, nextAction);
    }
    switch (lastPumpResult_) {
        case CapturePumpResult::PeriodPublished:
            return CaptureInterruptServiceResult::PeriodPublished;
        case CapturePumpResult::NoPeriodReady:
        case CapturePumpResult::NoInterrupt:
            return CaptureInterruptServiceResult::NoPeriodReady;
        case CapturePumpResult::RecoveryRequired:
            return CaptureInterruptServiceResult::RecoveryRequired;
        case CapturePumpResult::AudioFailure:
            return CaptureInterruptServiceResult::AudioFailure;
        default:
            return CaptureInterruptServiceResult::InvalidState;
    }
}

CaptureInterruptServiceResult DmicCaptureInterruptService::serviceAppleHdaPosition(
    uint32_t linkPositionBytes,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!initialized_ || !appleHdaManaged_ || !pump_ || !nextAction ||
        !appleHdaPollBaselineArmed_)
        return CaptureInterruptServiceResult::InvalidState;
    if (linkPositionBytes > appleHdaRingBytes_) {
        lastPositionReadSucceeded_ = false;
        lastLinkPosition_ = linkPositionBytes;
        lastPumpResult_ = pump_->reportCaptureIoFailure(nextAction);
        return lastPumpResult_ == CapturePumpResult::RecoveryRequired
            ? CaptureInterruptServiceResult::RecoveryRequired
            : CaptureInterruptServiceResult::InvalidState;
    }
    // The controller can report CBL itself at the wrap boundary. Preserve
    // the raw value for diagnostics and use zero for the circular delta.
    const uint32_t position = linkPositionBytes == appleHdaRingBytes_
        ? 0 : linkPositionBytes;
    const uint32_t delta = position >= appleHdaLastPolledPosition_
        ? position - appleHdaLastPolledPosition_
        : appleHdaRingBytes_ - appleHdaLastPolledPosition_ + position;
    appleHdaLastPolledPosition_ = position;
    lastStatusReadSucceeded_ = true;
    lastStreamStatus_ = 0;
    lastAckSucceeded_ = true;
    lastPositionReadSucceeded_ = true;
    lastLinkPosition_ = linkPositionBytes;
    *nextAction = sof::ipc4::kernel::HdaCaptureTriggerAction::None;
    // A delta over half the ring means the worker missed its safe observation
    // window and the samples are stale.
    if (delta > appleHdaRingBytes_ / 2U ||
        appleHdaPolledBytes_ > UINT64_MAX - delta) {
        lastPumpResult_ = pump_->reportCaptureIoFailure(nextAction);
    } else {
        appleHdaPolledBytes_ += delta;
        const uint64_t completed = appleHdaBaselinePeriodCount_ +
            appleHdaPolledBytes_ / appleHdaPeriodBytes_;
        lastPumpResult_ = pump_->processAppleHdaPoll(
            linkPositionBytes, completed, nextAction);
    }
    switch (lastPumpResult_) {
        case CapturePumpResult::PeriodPublished:
            return CaptureInterruptServiceResult::PeriodPublished;
        case CapturePumpResult::NoPeriodReady:
        case CapturePumpResult::NoInterrupt:
            return CaptureInterruptServiceResult::NoPeriodReady;
        case CapturePumpResult::RecoveryRequired:
            return CaptureInterruptServiceResult::RecoveryRequired;
        case CapturePumpResult::AudioFailure:
            return CaptureInterruptServiceResult::AudioFailure;
        default:
            return CaptureInterruptServiceResult::InvalidState;
    }
}

bool DmicCaptureInterruptService::ready() const noexcept {
    return initialized_ && (appleHdaManaged_ || mmio_) && pump_ &&
           pump_->configured();
}

bool DmicCaptureInterruptService::resetAfterRecovery(
    bool interruptBindingUninstalled) noexcept {
    if (!initialized_ || !interruptBindingUninstalled || !pump_ ||
        pump_->configured())
        return false;
    mmio_ = nullptr;
    pump_ = nullptr;
    streamControlOffset_ = 0;
    appleHdaRingBytes_ = 0;
    appleHdaPeriodBytes_ = 0;
    appleHdaPollBaselineArmed_ = false;
    appleHdaManaged_ = false;
    initialized_ = false;
    return true;
}

} // namespace sof::ipc4::dmic::kernel
