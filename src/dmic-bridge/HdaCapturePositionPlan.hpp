#pragma once

#include <stdint.h>

namespace sof::ipc4::kernel {

constexpr uint8_t kHdaStreamStatusBufferCompletion = 1U << 2;
constexpr uint8_t kHdaStreamStatusFifoError = 1U << 3;
constexpr uint8_t kHdaStreamStatusDescriptorError = 1U << 4;
constexpr uint8_t kHdaStreamStatusFifoReady = 1U << 5;
constexpr uint8_t kHdaStreamStatusW1cMask =
    kHdaStreamStatusBufferCompletion | kHdaStreamStatusFifoError |
    kHdaStreamStatusDescriptorError;
constexpr uint8_t kHdaStreamStatusKnownMask =
    kHdaStreamStatusW1cMask | kHdaStreamStatusFifoReady;

enum class HdaCapturePositionResult : uint8_t {
    Ready,
    NoCompletePeriod,
    InvalidArgument,
    InvalidState,
    PositionMismatch,
    Overrun,
    ReadFailed,
};

enum class HdaCaptureIocResult : uint8_t {
    AckRequired,
    StreamErrorAckRequired,
    NoCompletion,
    Acknowledged,
    InvalidArgument,
    InvalidState,
    StreamError,
    CommandFailed,
    CounterOverflow,
};

struct HdaCaptureIocAck {
    uint32_t offset;
    uint8_t value;
    uint8_t widthBytes;
    uint8_t reserved[2];
};

// Counts one completed period only after the caller reports a successful
// write-one-to-clear of SDn.STS.BCIS. FIFO/descriptor errors fail the counter.
class HdaCaptureIocCounter final {
public:
    HdaCaptureIocCounter() noexcept;

    bool initialize(uint32_t streamControlOffset,
                    uint64_t initialCompletedPeriodCount) noexcept;
    // Rebase a stopped/recovered counter only after the caller independently
    // confirms SDn.SRST completion and a fresh zero W1C-status readback.
    bool resetForStart(uint64_t initialCompletedPeriodCount,
                       bool streamResetConfirmed,
                       bool statusClearConfirmed) noexcept;
    HdaCaptureIocResult observeStatus(uint8_t streamStatus,
                                     HdaCaptureIocAck *ack) noexcept;
    HdaCaptureIocResult confirmAck(bool commandSucceeded) noexcept;

    uint32_t streamStatusOffset() const noexcept { return streamStatusOffset_; }
    uint64_t completedPeriodCount() const noexcept {
        return completedPeriodCount_;
    }
    bool failed() const noexcept { return failed_; }
    bool ackPending() const noexcept { return awaitingAck_; }

private:
    uint32_t streamStatusOffset_;
    uint64_t completedPeriodCount_;
    bool initialized_;
    bool awaitingAck_;
    bool pendingStreamError_;
    bool failed_;
};

struct HdaCapturePeriodReadPlan {
    uint32_t firstOffset;
    uint32_t firstLength;
    uint32_t secondLength;
    uint32_t reserved;
    uint64_t sequence;
};

// Tracks the HDA cyclic write head against the monotonically counted IOC
// periods. The caller must provide an IOC count from its serialized interrupt
// path; LPIB alone cannot reveal missed full-ring wraps.
class HdaCapturePositionTracker final {
public:
    HdaCapturePositionTracker() noexcept;

    bool initialize(uint32_t ringBytes, uint32_t periodBytes,
                    uint32_t initialPosition,
                    uint64_t initialCompletedPeriodCount,
                    uint32_t maximumQueuedBytes) noexcept;
    // Starts a new IOC/LPIB epoch only after the caller independently confirms
    // SDn.SRST and a fresh cleared stream-status readback. Unlike rebase(),
    // this permits the IOC count to return to zero and clears prior failure.
    bool resetForStart(uint32_t initialPosition,
                       uint64_t initialCompletedPeriodCount,
                       bool streamResetConfirmed,
                       bool statusClearConfirmed) noexcept;
    bool rebase(uint32_t linkPositionBytes,
                uint64_t completedPeriodCount) noexcept;
    HdaCapturePositionResult observe(uint32_t linkPositionBytes,
                                     uint64_t completedPeriodCount) noexcept;
    HdaCapturePositionResult nextPeriod(
        HdaCapturePeriodReadPlan *plan) noexcept;
    HdaCapturePositionResult completeRead(uint64_t sequence,
                                          bool readSucceeded) noexcept;

    uint32_t periodBytes() const noexcept { return periodBytes_; }
    uint32_t ringBytes() const noexcept { return ringBytes_; }
    uint64_t producedBytes() const noexcept { return producedBytes_; }
    uint64_t consumedBytes() const noexcept { return consumedBytes_; }
    uint64_t initialCompletedPeriodCount() const noexcept {
        return initialCompletedPeriodCount_;
    }
    uint64_t lastCompletedPeriodCount() const noexcept {
        return lastCompletedPeriodCount_;
    }
    bool lastPositionWasCblEndpoint() const noexcept {
        return lastPositionWasCblEndpoint_;
    }
    bool failed() const noexcept { return failed_; }

private:
    uint32_t ringBytes_;
    uint32_t periodBytes_;
    uint32_t initialPosition_;
    uint64_t initialCompletedPeriodCount_;
    uint64_t lastCompletedPeriodCount_;
    uint32_t maximumQueuedBytes_;
    uint64_t producedBytes_;
    uint64_t consumedBytes_;
    uint64_t nextSequence_;
    uint64_t pendingSequence_;
    bool initialized_;
    bool pendingRead_;
    bool failed_;
    bool lastPositionWasCblEndpoint_;
};

} // namespace sof::ipc4::kernel
