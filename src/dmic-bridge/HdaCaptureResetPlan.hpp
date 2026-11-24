#pragma once

#include "HdaInputStreamAllocatorPlan.hpp"

#include <stdint.h>

namespace sof::ipc4::kernel {

enum class HdaStreamResetResult : uint8_t {
    WriteRequired,
    Waiting,
    Complete,
    InvalidArgument,
    InvalidInitialState,
    InvalidReadback,
    UnexpectedReadback,
    TimedOut,
    CommandFailed,
    WrongPhase,
};

enum class HdaStreamResetAction : uint8_t {
    AssertReset,
    DeassertReset,
};

enum class HdaPostResetPlanResult : uint8_t {
    Ok,
    InvalidArgument,
    OwnershipNotProven,
    ResetNotComplete,
    ResetDescriptorMismatch,
    AllocationFailed,
    ReservationFailed,
};

struct HdaPostResetInputStreamPlan {
    HdaInputStreamAllocationPlan allocation;
    HdaInputStreamReservationPlan reservation;
};

struct HdaStreamResetWrite {
    uint32_t offset;
    uint32_t value;
    HdaStreamResetAction action;
};

// Bounded, readback-gated SDn.SRST handshake. This only produces a write
// request; the owner serializes and performs it, then supplies fresh reads.
class HdaCaptureResetSequence final {
public:
    HdaCaptureResetSequence() noexcept;

    bool initialize(const HdaInputStreamAllocationPlan &allocation,
                    uint32_t initialControlReadback,
                    uint64_t hdaBarLength,
                    uint32_t maximumPollSamples,
                    bool controllerExclusivelyOwned) noexcept;
    HdaStreamResetResult next(uint32_t controlReadback,
                              HdaStreamResetWrite *write) noexcept;
    HdaStreamResetResult commandFailed() noexcept;

    bool complete() const noexcept;
    uint32_t streamControlOffset() const noexcept {
        return streamControlOffset_;
    }
    uint32_t samplesRemaining() const noexcept { return samplesRemaining_; }

private:
    enum class State : uint8_t {
        Uninitialized,
        NeedAssert,
        AwaitAssert,
        AwaitDeassert,
        Complete,
        Failed,
    };

    State state_;
    uint32_t streamControlOffset_;
    uint32_t maximumPollSamples_;
    uint32_t samplesRemaining_;
};

// Rebuilds allocation and reservation data from a complete fresh snapshot
// taken after the selected input descriptor's reset completed. The chosen
// descriptor must be the one that was reset. This performs no I/O.
HdaPostResetPlanResult planHdaInputStreamAfterReset(
    uint16_t gcap, const HdaStreamDescriptorSnapshot *freshInputSnapshots,
    uint8_t freshInputSnapshotCount, uint16_t freshAllocatedStreamTagMask,
    const HdaCaptureResetSequence &resetSequence, uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    HdaPostResetInputStreamPlan *out) noexcept;

} // namespace sof::ipc4::kernel
