#include "HdaCapturePositionPlan.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kStreamDescriptorBase = 0x80;
constexpr uint32_t kStreamDescriptorStride = 0x20;
constexpr uint32_t kMaximumStreamDescriptors = 30;

uint32_t normalizeRingPosition(uint32_t position, uint32_t ringBytes) noexcept {
    // Some HDA controllers expose the exact CBL endpoint for one observation
    // at wrap. IOC count provides the monotonic evidence that distinguishes
    // this boundary from a full-ring overrun, so represent CBL as byte 0.
    return ringBytes != 0 && position == ringBytes ? 0 : position;
}
} // namespace

HdaCaptureIocCounter::HdaCaptureIocCounter() noexcept
    : streamStatusOffset_(0), completedPeriodCount_(0), initialized_(false),
      awaitingAck_(false), pendingStreamError_(false), failed_(false) {}

bool HdaCaptureIocCounter::initialize(
    uint32_t streamControlOffset,
    uint64_t initialCompletedPeriodCount) noexcept {
    if (initialized_ || streamControlOffset < kStreamDescriptorBase ||
        ((streamControlOffset - kStreamDescriptorBase) %
            kStreamDescriptorStride) != 0 ||
        ((streamControlOffset - kStreamDescriptorBase) /
            kStreamDescriptorStride) >= kMaximumStreamDescriptors)
        return false;
    streamStatusOffset_ = streamControlOffset + 3U;
    completedPeriodCount_ = initialCompletedPeriodCount;
    awaitingAck_ = false;
    pendingStreamError_ = false;
    failed_ = false;
    initialized_ = true;
    return true;
}

bool HdaCaptureIocCounter::resetForStart(
    uint64_t initialCompletedPeriodCount,
    bool streamResetConfirmed,
    bool statusClearConfirmed) noexcept {
    if (!initialized_ || awaitingAck_ || !streamResetConfirmed ||
        !statusClearConfirmed)
        return false;
    completedPeriodCount_ = initialCompletedPeriodCount;
    awaitingAck_ = false;
    pendingStreamError_ = false;
    failed_ = false;
    return true;
}

HdaCaptureIocResult HdaCaptureIocCounter::observeStatus(
    uint8_t streamStatus, HdaCaptureIocAck *ack) noexcept {
    if (!initialized_ || failed_ || awaitingAck_)
        return HdaCaptureIocResult::InvalidState;
    const bool streamError =
        (streamStatus & (kHdaStreamStatusFifoError |
                         kHdaStreamStatusDescriptorError)) != 0 ||
        (streamStatus & ~kHdaStreamStatusKnownMask) != 0;
    if (streamError) {
        const uint8_t ackValue = streamStatus & kHdaStreamStatusW1cMask;
        if (ack && ackValue != 0) {
            ack->offset = streamStatusOffset_;
            ack->value = ackValue;
            ack->widthBytes = 1;
            ack->reserved[0] = 0;
            ack->reserved[1] = 0;
            awaitingAck_ = true;
            pendingStreamError_ = true;
            return HdaCaptureIocResult::StreamErrorAckRequired;
        }
        failed_ = true;
        return HdaCaptureIocResult::StreamError;
    }
    if ((streamStatus & kHdaStreamStatusBufferCompletion) == 0)
        return HdaCaptureIocResult::NoCompletion;
    if (!ack) return HdaCaptureIocResult::InvalidArgument;
    ack->offset = streamStatusOffset_;
    ack->value = kHdaStreamStatusBufferCompletion;
    ack->widthBytes = 1;
    ack->reserved[0] = 0;
    ack->reserved[1] = 0;
    awaitingAck_ = true;
    return HdaCaptureIocResult::AckRequired;
}

HdaCaptureIocResult HdaCaptureIocCounter::confirmAck(
    bool commandSucceeded) noexcept {
    if (!initialized_ || failed_ || !awaitingAck_)
        return HdaCaptureIocResult::InvalidState;
    awaitingAck_ = false;
    if (!commandSucceeded) {
        pendingStreamError_ = false;
        failed_ = true;
        return HdaCaptureIocResult::CommandFailed;
    }
    if (pendingStreamError_) {
        pendingStreamError_ = false;
        failed_ = true;
        return HdaCaptureIocResult::StreamError;
    }
    if (completedPeriodCount_ == UINT64_MAX) {
        failed_ = true;
        return HdaCaptureIocResult::CounterOverflow;
    }
    ++completedPeriodCount_;
    return HdaCaptureIocResult::Acknowledged;
}

HdaCapturePositionTracker::HdaCapturePositionTracker() noexcept
    : ringBytes_(0), periodBytes_(0), initialPosition_(0),
      initialCompletedPeriodCount_(0), lastCompletedPeriodCount_(0),
      maximumQueuedBytes_(0), producedBytes_(0), consumedBytes_(0),
      nextSequence_(1), pendingSequence_(0), initialized_(false),
      pendingRead_(false), failed_(false),
      lastPositionWasCblEndpoint_(false) {}

bool HdaCapturePositionTracker::initialize(
    uint32_t ringBytes, uint32_t periodBytes, uint32_t initialPosition,
    uint64_t initialCompletedPeriodCount,
    uint32_t maximumQueuedBytes) noexcept {
    initialPosition = normalizeRingPosition(initialPosition, ringBytes);
    const uint64_t minimumRingBytes =
        static_cast<uint64_t>(periodBytes) * 3U;
    if (initialized_ || periodBytes == 0 || ringBytes == 0 ||
        (ringBytes % periodBytes) != 0 ||
        static_cast<uint64_t>(ringBytes) < minimumRingBytes ||
        initialPosition >= ringBytes || (initialPosition % periodBytes) != 0 ||
        maximumQueuedBytes < periodBytes ||
        maximumQueuedBytes % periodBytes != 0 ||
        static_cast<uint64_t>(maximumQueuedBytes) >
            static_cast<uint64_t>(ringBytes) - 2U * periodBytes)
        return false;

    ringBytes_ = ringBytes;
    periodBytes_ = periodBytes;
    initialPosition_ = initialPosition;
    initialCompletedPeriodCount_ = initialCompletedPeriodCount;
    lastCompletedPeriodCount_ = initialCompletedPeriodCount;
    maximumQueuedBytes_ = maximumQueuedBytes;
    producedBytes_ = 0;
    consumedBytes_ = 0;
    nextSequence_ = 1;
    pendingSequence_ = 0;
    pendingRead_ = false;
    failed_ = false;
    lastPositionWasCblEndpoint_ = false;
    initialized_ = true;
    return true;
}

bool HdaCapturePositionTracker::resetForStart(
    uint32_t initialPosition,
    uint64_t initialCompletedPeriodCount,
    bool streamResetConfirmed,
    bool statusClearConfirmed) noexcept {
    lastPositionWasCblEndpoint_ = initialPosition == ringBytes_;
    initialPosition = normalizeRingPosition(initialPosition, ringBytes_);
    if (!initialized_ || pendingRead_ || !streamResetConfirmed ||
        !statusClearConfirmed || initialPosition >= ringBytes_ ||
        (initialPosition % periodBytes_) != 0)
        return false;
    initialPosition_ = initialPosition;
    initialCompletedPeriodCount_ = initialCompletedPeriodCount;
    lastCompletedPeriodCount_ = initialCompletedPeriodCount;
    producedBytes_ = 0;
    consumedBytes_ = 0;
    nextSequence_ = 1;
    pendingSequence_ = 0;
    failed_ = false;
    return true;
}

bool HdaCapturePositionTracker::rebase(
    uint32_t linkPositionBytes,
    uint64_t completedPeriodCount) noexcept {
    lastPositionWasCblEndpoint_ = linkPositionBytes == ringBytes_;
    linkPositionBytes = normalizeRingPosition(linkPositionBytes, ringBytes_);
    if (!initialized_ || failed_ || pendingRead_ ||
        linkPositionBytes >= ringBytes_ ||
        completedPeriodCount < lastCompletedPeriodCount_)
        return false;
    initialPosition_ = linkPositionBytes;
    initialCompletedPeriodCount_ = completedPeriodCount;
    lastCompletedPeriodCount_ = completedPeriodCount;
    producedBytes_ = 0;
    consumedBytes_ = 0;
    return true;
}

HdaCapturePositionResult HdaCapturePositionTracker::observe(
    uint32_t linkPositionBytes,
    uint64_t completedPeriodCount) noexcept {
    lastPositionWasCblEndpoint_ = linkPositionBytes == ringBytes_;
    linkPositionBytes = normalizeRingPosition(linkPositionBytes, ringBytes_);
    if (!initialized_ || failed_) return HdaCapturePositionResult::InvalidState;
    if (linkPositionBytes >= ringBytes_ ||
        completedPeriodCount < lastCompletedPeriodCount_ ||
        completedPeriodCount < initialCompletedPeriodCount_) {
        failed_ = true;
        return HdaCapturePositionResult::PositionMismatch;
    }

    const uint64_t completedPeriods =
        completedPeriodCount - initialCompletedPeriodCount_;
    const uint64_t positionRemainder = linkPositionBytes % periodBytes_;
    const uint64_t initialRemainder = initialPosition_ % periodBytes_;
    const uint64_t withinPeriodDelta =
        (positionRemainder + periodBytes_ - initialRemainder) % periodBytes_;
    if (completedPeriods > (UINT64_MAX - withinPeriodDelta) / periodBytes_) {
        failed_ = true;
        return HdaCapturePositionResult::Overrun;
    }
    const uint64_t iocProducedLowerBound = completedPeriods * periodBytes_ +
        withinPeriodDelta;
    const uint64_t positionDistance = linkPositionBytes >= initialPosition_
        ? static_cast<uint64_t>(linkPositionBytes - initialPosition_)
        : static_cast<uint64_t>(ringBytes_ - initialPosition_) + linkPositionBytes;
    if ((positionDistance % periodBytes_) != withinPeriodDelta) {
        failed_ = true;
        return HdaCapturePositionResult::PositionMismatch;
    }

    // BCIS is a single sticky W1C bit, so multiple completed periods can
    // coalesce before the worker acknowledges it. LPIB supplies the modulo
    // ring distance; IOC count supplies a lower bound on total completed bytes
    // and disambiguates full wraps. Choose the smallest position consistent
    // with both sources and monotonic consumption.
    const uint64_t lowerBound = iocProducedLowerBound > producedBytes_
        ? iocProducedLowerBound : producedBytes_;
    uint64_t produced = positionDistance;
    if (produced < lowerBound) {
        const uint64_t missing = lowerBound - produced;
        const uint64_t wraps = 1U + (missing - 1U) / ringBytes_;
        if (wraps > (UINT64_MAX - produced) / ringBytes_) {
            failed_ = true;
            return HdaCapturePositionResult::Overrun;
        }
        produced += wraps * ringBytes_;
    }
    if (produced < consumedBytes_) {
        failed_ = true;
        return HdaCapturePositionResult::PositionMismatch;
    }
    if (produced - consumedBytes_ > maximumQueuedBytes_) {
        failed_ = true;
        return HdaCapturePositionResult::Overrun;
    }

    producedBytes_ = produced;
    lastCompletedPeriodCount_ = completedPeriodCount;
    return HdaCapturePositionResult::Ready;
}

HdaCapturePositionResult HdaCapturePositionTracker::nextPeriod(
    HdaCapturePeriodReadPlan *plan) noexcept {
    if (!plan) return HdaCapturePositionResult::InvalidArgument;
    *plan = {};
    if (!initialized_ || failed_) return HdaCapturePositionResult::InvalidState;
    if (pendingRead_) return HdaCapturePositionResult::InvalidState;
    if (producedBytes_ - consumedBytes_ < periodBytes_)
        return HdaCapturePositionResult::NoCompletePeriod;
    if (nextSequence_ == 0) {
        failed_ = true;
        return HdaCapturePositionResult::InvalidState;
    }

    const uint32_t offset = static_cast<uint32_t>(
        (static_cast<uint64_t>(initialPosition_) +
         (consumedBytes_ % ringBytes_)) % ringBytes_);
    const uint32_t firstLength =
        periodBytes_ < ringBytes_ - offset ? periodBytes_ : ringBytes_ - offset;
    plan->firstOffset = offset;
    plan->firstLength = firstLength;
    plan->secondLength = periodBytes_ - firstLength;
    plan->reserved = 0;
    plan->sequence = nextSequence_++;
    pendingSequence_ = plan->sequence;
    pendingRead_ = true;
    return HdaCapturePositionResult::Ready;
}

HdaCapturePositionResult HdaCapturePositionTracker::completeRead(
    uint64_t sequence, bool readSucceeded) noexcept {
    if (!initialized_ || failed_ || !pendingRead_ || sequence == 0 ||
        sequence != pendingSequence_)
        return HdaCapturePositionResult::InvalidState;
    pendingRead_ = false;
    pendingSequence_ = 0;
    if (!readSucceeded) {
        failed_ = true;
        return HdaCapturePositionResult::ReadFailed;
    }
    if (consumedBytes_ > UINT64_MAX - periodBytes_) {
        failed_ = true;
        return HdaCapturePositionResult::Overrun;
    }
    consumedBytes_ += periodBytes_;
    return HdaCapturePositionResult::Ready;
}

} // namespace sof::ipc4::kernel
