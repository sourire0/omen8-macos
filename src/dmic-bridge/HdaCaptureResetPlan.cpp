#include "HdaCaptureResetPlan.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kStreamDescriptorBase = 0x80;
constexpr uint32_t kStreamDescriptorStride = 0x20;
constexpr uint32_t kMaximumInputStreams = 15;
constexpr uint32_t kStreamRunMask = 1U << 1;
constexpr uint32_t kStreamResetMask = 1U << 0;
constexpr uint32_t kStreamTagMask = 0x0fU << 20;

} // namespace

HdaCaptureResetSequence::HdaCaptureResetSequence() noexcept
    : state_(State::Uninitialized), streamControlOffset_(0),
      maximumPollSamples_(0), samplesRemaining_(0) {}

bool HdaCaptureResetSequence::initialize(
    const HdaInputStreamAllocationPlan &allocation,
    uint32_t initialControlReadback, uint64_t hdaBarLength,
    uint32_t maximumPollSamples,
    bool controllerExclusivelyOwned) noexcept {
    if (state_ != State::Uninitialized || maximumPollSamples == 0 ||
        !controllerExclusivelyOwned || allocation.inputStreamCount == 0 ||
        allocation.inputStreamCount > kMaximumInputStreams ||
        allocation.descriptorIndex >= allocation.inputStreamCount ||
        allocation.streamTag == 0 || allocation.streamTag > 15 ||
        allocation.reserved != 0 || allocation.reserved2 != 0 ||
        (allocation.allocatedStreamTagMask &
            (1U << allocation.streamTag)) != 0 ||
        allocation.descriptorControlOffset < kStreamDescriptorBase ||
        ((allocation.descriptorControlOffset - kStreamDescriptorBase) %
            kStreamDescriptorStride) != 0 ||
        ((allocation.descriptorControlOffset - kStreamDescriptorBase) /
            kStreamDescriptorStride) >= kMaximumInputStreams ||
        allocation.descriptorControlOffset != kStreamDescriptorBase +
            static_cast<uint32_t>(allocation.descriptorIndex) *
                kStreamDescriptorStride ||
        static_cast<uint64_t>(allocation.descriptorControlOffset) +
            sizeof(uint32_t) >
            hdaBarLength ||
        initialControlReadback == UINT32_MAX ||
        (initialControlReadback &
            (kStreamRunMask | kStreamResetMask | kStreamTagMask)) != 0)
        return false;
    streamControlOffset_ = allocation.descriptorControlOffset;
    maximumPollSamples_ = maximumPollSamples;
    samplesRemaining_ = maximumPollSamples;
    state_ = State::NeedAssert;
    return true;
}

HdaStreamResetResult HdaCaptureResetSequence::next(
    uint32_t controlReadback, HdaStreamResetWrite *write) noexcept {
    if (!write) return HdaStreamResetResult::InvalidArgument;
    if (state_ == State::Uninitialized || state_ == State::Complete ||
        state_ == State::Failed)
        return HdaStreamResetResult::WrongPhase;
    if (controlReadback == UINT32_MAX) {
        state_ = State::Failed;
        return HdaStreamResetResult::InvalidReadback;
    }
    if ((controlReadback & kStreamRunMask) != 0) {
        state_ = State::Failed;
        return HdaStreamResetResult::UnexpectedReadback;
    }

    if (state_ == State::NeedAssert) {
        if ((controlReadback & (kStreamResetMask | kStreamTagMask)) != 0) {
            state_ = State::Failed;
            return HdaStreamResetResult::InvalidInitialState;
        }
        write->offset = streamControlOffset_;
        write->value = controlReadback | kStreamResetMask;
        write->action = HdaStreamResetAction::AssertReset;
        samplesRemaining_ = maximumPollSamples_;
        state_ = State::AwaitAssert;
        return HdaStreamResetResult::WriteRequired;
    }

    if (state_ == State::AwaitAssert) {
        if ((controlReadback & kStreamTagMask) != 0) {
            state_ = State::Failed;
            return HdaStreamResetResult::UnexpectedReadback;
        }
        if ((controlReadback & kStreamResetMask) != 0) {
            write->offset = streamControlOffset_;
            write->value = controlReadback & ~kStreamResetMask;
            write->action = HdaStreamResetAction::DeassertReset;
            samplesRemaining_ = maximumPollSamples_;
            state_ = State::AwaitDeassert;
            return HdaStreamResetResult::WriteRequired;
        }
        if (--samplesRemaining_ == 0) {
            state_ = State::Failed;
            return HdaStreamResetResult::TimedOut;
        }
        return HdaStreamResetResult::Waiting;
    }

    if (state_ == State::AwaitDeassert) {
        if ((controlReadback & kStreamResetMask) == 0) {
            if ((controlReadback & kStreamTagMask) != 0) {
                state_ = State::Failed;
                return HdaStreamResetResult::UnexpectedReadback;
            }
            state_ = State::Complete;
            return HdaStreamResetResult::Complete;
        }
        if (--samplesRemaining_ == 0) {
            state_ = State::Failed;
            return HdaStreamResetResult::TimedOut;
        }
        return HdaStreamResetResult::Waiting;
    }

    state_ = State::Failed;
    return HdaStreamResetResult::WrongPhase;
}

HdaStreamResetResult HdaCaptureResetSequence::commandFailed() noexcept {
    if (state_ != State::AwaitAssert && state_ != State::AwaitDeassert)
        return HdaStreamResetResult::WrongPhase;
    state_ = State::Failed;
    return HdaStreamResetResult::CommandFailed;
}

bool HdaCaptureResetSequence::complete() const noexcept {
    return state_ == State::Complete;
}

HdaPostResetPlanResult planHdaInputStreamAfterReset(
    uint16_t gcap, const HdaStreamDescriptorSnapshot *freshInputSnapshots,
    uint8_t freshInputSnapshotCount, uint16_t freshAllocatedStreamTagMask,
    const HdaCaptureResetSequence &resetSequence, uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    HdaPostResetInputStreamPlan *out) noexcept {
    if (!out || !freshInputSnapshots)
        return HdaPostResetPlanResult::InvalidArgument;
    if (!controllerExclusivelyOwned)
        return HdaPostResetPlanResult::OwnershipNotProven;
    if (!resetSequence.complete())
        return HdaPostResetPlanResult::ResetNotComplete;

    HdaPostResetInputStreamPlan candidate{};
    if (planHdaInputStreamAllocation(
            gcap, freshInputSnapshots, freshInputSnapshotCount,
            freshAllocatedStreamTagMask, true, &candidate.allocation) !=
        HdaInputStreamPlanResult::Ok)
        return HdaPostResetPlanResult::AllocationFailed;
    if (candidate.allocation.descriptorControlOffset !=
        resetSequence.streamControlOffset())
        return HdaPostResetPlanResult::ResetDescriptorMismatch;
    if (planHdaInputStreamReservation(
            candidate.allocation,
            freshInputSnapshots[candidate.allocation.descriptorIndex],
            freshAllocatedStreamTagMask, hdaBarLength, true,
            &candidate.reservation) != HdaInputStreamPlanResult::Ok)
        return HdaPostResetPlanResult::ReservationFailed;

    *out = candidate;
    return HdaPostResetPlanResult::Ok;
}

} // namespace sof::ipc4::kernel
