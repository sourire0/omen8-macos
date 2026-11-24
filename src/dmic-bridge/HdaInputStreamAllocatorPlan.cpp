#include "HdaInputStreamAllocatorPlan.hpp"

namespace sof::ipc4::kernel {

HdaInputStreamReservationSequence::HdaInputStreamReservationSequence() noexcept
    : plan_{}, initialized_(false), awaitingConfirmation_(false),
      complete_(false), failed_(false),
      confirmedControlReadback_(UINT32_MAX) {}

HdaInputStreamReservationSequenceResult
HdaInputStreamReservationSequence::initialize(
    const HdaInputStreamReservationPlan &plan,
    uint64_t hdaBarLength) noexcept {
    if (initialized_) return HdaInputStreamReservationSequenceResult::InvalidState;
    if (plan.descriptorControlOffset < kHdaStreamDescriptorBase ||
        ((plan.descriptorControlOffset - kHdaStreamDescriptorBase) %
            kHdaStreamDescriptorStride) != 0 ||
        ((plan.descriptorControlOffset - kHdaStreamDescriptorBase) /
            kHdaStreamDescriptorStride) >= kHdaMaximumStreamDescriptors ||
        static_cast<uint64_t>(plan.descriptorControlOffset) + sizeof(uint32_t) >
            hdaBarLength)
        return HdaInputStreamReservationSequenceResult::InvalidPlan;
    const uint8_t tag = static_cast<uint8_t>(
        (plan.controlWriteValue & kHdaStreamTagMask) >> 20);
    const uint32_t expectedMask = kHdaStreamRunMask | kHdaStreamResetMask |
        kHdaStreamTagMask;
    if (plan.previousControlReadback == UINT32_MAX || tag == 0 || tag > 15 ||
        (plan.previousControlReadback & expectedMask) != 0 ||
        (plan.controlWriteValue & ~kHdaStreamTagMask) !=
            (plan.previousControlReadback & ~kHdaStreamTagMask) ||
        (plan.controlWriteValue &
            (kHdaStreamRunMask | kHdaStreamResetMask)) != 0 ||
        plan.expectedReadbackMask != expectedMask ||
        plan.expectedReadbackValue != (static_cast<uint32_t>(tag) << 20))
        return HdaInputStreamReservationSequenceResult::InvalidPlan;
    plan_ = plan;
    initialized_ = true;
    return HdaInputStreamReservationSequenceResult::Ready;
}

HdaInputStreamReservationSequenceResult
HdaInputStreamReservationSequence::next(
    HdaInputStreamRegisterWrite *write) noexcept {
    if (!write) return HdaInputStreamReservationSequenceResult::InvalidArgument;
    if (!initialized_ || failed_ || awaitingConfirmation_)
        return HdaInputStreamReservationSequenceResult::InvalidState;
    if (complete_) return HdaInputStreamReservationSequenceResult::Complete;
    write->offset = plan_.descriptorControlOffset;
    write->value = plan_.controlWriteValue;
    write->widthBytes = sizeof(uint32_t);
    write->reserved[0] = 0;
    write->reserved[1] = 0;
    write->reserved[2] = 0;
    awaitingConfirmation_ = true;
    return HdaInputStreamReservationSequenceResult::WriteRequired;
}

HdaInputStreamReservationSequenceResult
HdaInputStreamReservationSequence::confirm(
    uint32_t controlReadback) noexcept {
    if (!initialized_ || failed_ || complete_ || !awaitingConfirmation_)
        return HdaInputStreamReservationSequenceResult::InvalidState;
    awaitingConfirmation_ = false;
    if (controlReadback == UINT32_MAX) {
        failed_ = true;
        return HdaInputStreamReservationSequenceResult::InvalidReadback;
    }
    if (!hdaInputStreamReservationMatches(plan_, controlReadback)) {
        failed_ = true;
        return HdaInputStreamReservationSequenceResult::ReadbackMismatch;
    }
    confirmedControlReadback_ = controlReadback;
    complete_ = true;
    return HdaInputStreamReservationSequenceResult::Complete;
}

HdaInputStreamReservationSequenceResult
HdaInputStreamReservationSequence::commandFailed() noexcept {
    if (!initialized_ || failed_ || complete_ || !awaitingConfirmation_)
        return HdaInputStreamReservationSequenceResult::InvalidState;
    awaitingConfirmation_ = false;
    failed_ = true;
    return HdaInputStreamReservationSequenceResult::CommandFailed;
}

HdaInputStreamPlanResult planHdaInputStreamAllocation(
    uint16_t gcap, const HdaStreamDescriptorSnapshot *inputSnapshots,
    uint8_t snapshotCount, uint16_t allocatedStreamTagMask,
    bool controllerExclusivelyOwned,
    HdaInputStreamAllocationPlan *out) noexcept {
    if (!out || !inputSnapshots)
        return HdaInputStreamPlanResult::InvalidArgument;
    if (!controllerExclusivelyOwned)
        return HdaInputStreamPlanResult::OwnershipNotProven;
    if (gcap == 0xffffU)
        return HdaInputStreamPlanResult::InvalidCapabilities;

    const uint8_t inputCount = static_cast<uint8_t>((gcap >> 8) & 0x0fU);
    if (inputCount == 0 || inputCount > kHdaMaximumStreamDescriptors)
        return HdaInputStreamPlanResult::InvalidCapabilities;
    if (snapshotCount != inputCount ||
        inputCount > kHdaMaximumStreamDescriptors)
        return HdaInputStreamPlanResult::IncompleteSnapshot;

    uint16_t observedInputTags = 0;
    for (uint8_t i = 0; i < inputCount; ++i) {
        const uint8_t tag = static_cast<uint8_t>(
            (inputSnapshots[i].control & kHdaStreamTagMask) >> 20);
        if (tag != 0)
            observedInputTags = static_cast<uint16_t>(
                observedInputTags | (1U << tag));
    }
    if ((observedInputTags & allocatedStreamTagMask) != observedInputTags)
        return HdaInputStreamPlanResult::InconsistentTagMask;

    uint8_t selectedIndex = inputCount;
    for (uint8_t i = 0; i < inputCount; ++i) {
        const uint32_t control = inputSnapshots[i].control;
        const uint8_t tag = static_cast<uint8_t>(
            (control & kHdaStreamTagMask) >> 20);
        const bool descriptorEmpty =
            inputSnapshots[i].status == 0 &&
            inputSnapshots[i].cyclicBufferLength == 0 &&
            inputSnapshots[i].bdlLow == 0 &&
            inputSnapshots[i].bdlHigh == 0 &&
            inputSnapshots[i].lastValidIndex == 0 &&
            inputSnapshots[i].format == 0;
        if ((control & (kHdaStreamRunMask | kHdaStreamResetMask)) == 0 &&
            tag == 0 && descriptorEmpty) {
            selectedIndex = i;
            break;
        }
    }
    if (selectedIndex == inputCount)
        return HdaInputStreamPlanResult::NoFreeInputDescriptor;

    uint8_t selectedTag = 0;
    for (uint8_t tag = 1; tag <= 15; ++tag) {
        if ((allocatedStreamTagMask & (1U << tag)) == 0) {
            selectedTag = tag;
            break;
        }
    }
    if (selectedTag == 0)
        return HdaInputStreamPlanResult::NoFreeStreamTag;

    out->inputStreamCount = inputCount;
    out->descriptorIndex = selectedIndex;
    out->streamTag = selectedTag;
    out->reserved = 0;
    out->descriptorControlOffset = kHdaStreamDescriptorBase +
        static_cast<uint32_t>(selectedIndex) * kHdaStreamDescriptorStride +
        kHdaStreamControlOffset;
    out->allocatedStreamTagMask = allocatedStreamTagMask;
    out->reserved2 = 0;
    return HdaInputStreamPlanResult::Ok;
}

HdaInputStreamPlanResult planHdaInputStreamReservation(
    const HdaInputStreamAllocationPlan &allocation,
    const HdaStreamDescriptorSnapshot &currentDescriptor,
    uint16_t currentAllocatedStreamTagMask,
    uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    HdaInputStreamReservationPlan *out) noexcept {
    if (!out) return HdaInputStreamPlanResult::InvalidArgument;
    if (!controllerExclusivelyOwned)
        return HdaInputStreamPlanResult::OwnershipNotProven;
    if (allocation.inputStreamCount == 0 ||
        allocation.inputStreamCount > kHdaMaximumStreamDescriptors ||
        allocation.descriptorIndex >= allocation.inputStreamCount ||
        allocation.streamTag == 0 || allocation.streamTag > 15 ||
        allocation.reserved != 0 || allocation.reserved2 != 0 ||
        allocation.descriptorControlOffset != kHdaStreamDescriptorBase +
            static_cast<uint32_t>(allocation.descriptorIndex) *
                kHdaStreamDescriptorStride + kHdaStreamControlOffset ||
        (allocation.allocatedStreamTagMask &
            (1U << allocation.streamTag)) != 0)
        return HdaInputStreamPlanResult::InvalidArgument;
    if ((currentAllocatedStreamTagMask & (1U << allocation.streamTag)) != 0)
        return HdaInputStreamPlanResult::StreamTagAlreadyAllocated;
    if (currentDescriptor.control == UINT32_MAX ||
        currentDescriptor.status != 0 ||
        currentDescriptor.cyclicBufferLength != 0 ||
        currentDescriptor.bdlLow != 0 || currentDescriptor.bdlHigh != 0 ||
        currentDescriptor.lastValidIndex != 0 || currentDescriptor.format != 0 ||
        (currentDescriptor.control &
            (kHdaStreamRunMask | kHdaStreamResetMask | kHdaStreamTagMask)) !=
            0)
        return HdaInputStreamPlanResult::InvalidReservationReadback;
    if (static_cast<uint64_t>(allocation.descriptorControlOffset) +
            sizeof(uint32_t) > hdaBarLength)
        return HdaInputStreamPlanResult::InsufficientBar;

    out->descriptorControlOffset = allocation.descriptorControlOffset;
    out->previousControlReadback = currentDescriptor.control;
    out->controlWriteValue =
        (currentDescriptor.control & ~kHdaStreamTagMask) |
        (static_cast<uint32_t>(allocation.streamTag) << 20);
    out->expectedReadbackMask = kHdaStreamRunMask | kHdaStreamResetMask |
        kHdaStreamTagMask;
    out->expectedReadbackValue =
        static_cast<uint32_t>(allocation.streamTag) << 20;
    return HdaInputStreamPlanResult::Ok;
}

bool hdaInputStreamReservationMatches(
    const HdaInputStreamReservationPlan &plan,
    uint32_t controlReadback) noexcept {
    const uint32_t tag =
        (plan.controlWriteValue & kHdaStreamTagMask) >> 20;
    const uint32_t expectedMask = kHdaStreamRunMask |
        kHdaStreamResetMask | kHdaStreamTagMask;
    return plan.descriptorControlOffset >= kHdaStreamDescriptorBase &&
        ((plan.descriptorControlOffset - kHdaStreamDescriptorBase) %
            kHdaStreamDescriptorStride) == 0 &&
        ((plan.descriptorControlOffset - kHdaStreamDescriptorBase) /
            kHdaStreamDescriptorStride) < kHdaMaximumStreamDescriptors &&
        plan.previousControlReadback != UINT32_MAX &&
        tag != 0 && tag <= 15 &&
        (plan.previousControlReadback & expectedMask) == 0 &&
        (plan.controlWriteValue &
            (kHdaStreamRunMask | kHdaStreamResetMask)) == 0 &&
        (plan.controlWriteValue & ~kHdaStreamTagMask) ==
            (plan.previousControlReadback & ~kHdaStreamTagMask) &&
        plan.expectedReadbackMask == expectedMask &&
        plan.expectedReadbackValue == (tag << 20) &&
        controlReadback != UINT32_MAX &&
        (controlReadback & plan.expectedReadbackMask) ==
            plan.expectedReadbackValue;
}

} // namespace sof::ipc4::kernel
