#include "Cavs25CodeLoaderStreamPlan.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kStreamRunMask = 1U << 1;
constexpr uint32_t kStreamResetMask = 1U;
constexpr uint32_t kStreamTagMask = 0x0fU << 20;

} // namespace

CodeLoaderStreamPlanResult planCavs25CodeLoaderStreamReservation(
    uint16_t globalCapabilities, uint8_t inputStreamCount,
    uint8_t outputStreamCount, uint8_t totalStreamCount,
    const HdaStreamDescriptorSnapshot *allStreamControls,
    uint8_t snapshotCount, uint16_t allocatedStreamTagMask,
    uint32_t previousControlBeforeReset,
    uint64_t hdaBarLength, bool controllerExclusivelyOwned,
    bool audioOwnerQuiesced,
    Cavs25CodeLoaderStreamReservationPlan *out,
    uint8_t outputOrdinal, uint8_t streamTag) noexcept {
    if (!out || !allStreamControls || streamTag == 0 || streamTag > 15)
        return CodeLoaderStreamPlanResult::InvalidArgument;
    *out = {};
    if (!controllerExclusivelyOwned || !audioOwnerQuiesced)
        return CodeLoaderStreamPlanResult::OwnershipNotProven;

    const uint8_t gcapInputCount = static_cast<uint8_t>(
        (globalCapabilities >> 8) & 0x0fU);
    const uint8_t gcapOutputCount = static_cast<uint8_t>(
        (globalCapabilities >> 12) & 0x0fU);
    const uint8_t gcapBidirectionalCount = static_cast<uint8_t>(
        (globalCapabilities >> 3) & 0x1fU);
    const uint16_t expectedTotal = static_cast<uint16_t>(gcapInputCount) +
        gcapOutputCount + gcapBidirectionalCount;
    if (globalCapabilities == UINT16_MAX || gcapInputCount == 0 ||
        gcapInputCount != inputStreamCount ||
        gcapOutputCount != outputStreamCount || expectedTotal == 0 ||
        expectedTotal > kHdaMaximumTotalStreamCount ||
        expectedTotal != totalStreamCount)
        return CodeLoaderStreamPlanResult::InvalidCapabilities;
    if (snapshotCount != totalStreamCount ||
        totalStreamCount > kHdaMaximumTotalStreamCount)
        return CodeLoaderStreamPlanResult::IncompleteSnapshot;
    if (outputOrdinal >= outputStreamCount)
        return CodeLoaderStreamPlanResult::NoOutputDescriptor;
    if (previousControlBeforeReset == UINT32_MAX ||
        (previousControlBeforeReset &
            (kStreamRunMask | kStreamResetMask | kStreamTagMask)) != 0)
        return CodeLoaderStreamPlanResult::StreamBusy;
    uint16_t observedTagMask = 0;
    for (uint8_t i = 0; i < totalStreamCount; ++i) {
        const uint32_t control = allStreamControls[i].control;
        if (control == UINT32_MAX)
            return CodeLoaderStreamPlanResult::IncompleteSnapshot;
        const uint8_t tag = static_cast<uint8_t>(
            (control & kStreamTagMask) >> 20);
        if (tag != 0) {
            const uint16_t tagBit = static_cast<uint16_t>(1U << tag);
            if ((observedTagMask & tagBit) != 0)
                return CodeLoaderStreamPlanResult::InconsistentTagMask;
            observedTagMask = static_cast<uint16_t>(observedTagMask | tagBit);
        }
    }
    if (observedTagMask != allocatedStreamTagMask)
        return CodeLoaderStreamPlanResult::InconsistentTagMask;
    if ((allocatedStreamTagMask & (1U << streamTag)) != 0)
        return CodeLoaderStreamPlanResult::StreamTagUnavailable;

    const auto descriptorIndex = static_cast<uint8_t>(inputStreamCount + outputOrdinal);
    const uint32_t currentControl =
        allStreamControls[descriptorIndex].control;
    if (currentControl == UINT32_MAX ||
        (currentControl &
            (kStreamRunMask | kStreamResetMask | kStreamTagMask)) != 0)
        return CodeLoaderStreamPlanResult::StreamBusy;

    const uint32_t controlOffset = kHdaStreamDescriptorBase +
        static_cast<uint32_t>(descriptorIndex) *
            kHdaStreamDescriptorStride;
    if (static_cast<uint64_t>(controlOffset) + sizeof(uint32_t) >
        hdaBarLength)
        return CodeLoaderStreamPlanResult::InsufficientBar;

    out->descriptorControlOffset = controlOffset;
    out->reservationBaseControl = currentControl;
    out->previousControl = previousControlBeforeReset;
    out->controlWriteValue = (currentControl & ~kStreamTagMask) |
        (static_cast<uint32_t>(streamTag) << 20);
    out->expectedReadbackMask =
        kStreamRunMask | kStreamResetMask | kStreamTagMask;
    out->expectedReadbackValue =
        static_cast<uint32_t>(streamTag) << 20;
    out->descriptorIndex = descriptorIndex;
    out->streamTag = streamTag;
    out->reserved = 0;
    return CodeLoaderStreamPlanResult::Ok;
}

bool cavs25CodeLoaderStreamReservationMatches(
    const Cavs25CodeLoaderStreamReservationPlan &plan,
    uint32_t controlReadback) noexcept {
    const uint32_t expectedTag =
        static_cast<uint32_t>(plan.streamTag) << 20;
    return plan.descriptorIndex < kHdaMaximumTotalStreamCount &&
        plan.streamTag > 0 && plan.streamTag <= 15 && plan.reserved == 0 &&
        plan.descriptorControlOffset == kHdaStreamDescriptorBase +
            static_cast<uint32_t>(plan.descriptorIndex) *
                kHdaStreamDescriptorStride &&
        plan.expectedReadbackMask ==
            (kStreamRunMask | kStreamResetMask | kStreamTagMask) &&
        plan.expectedReadbackValue == expectedTag &&
        (plan.controlWriteValue & plan.expectedReadbackMask) ==
            expectedTag && controlReadback != UINT32_MAX &&
        (controlReadback & plan.expectedReadbackMask) == expectedTag &&
        ((controlReadback ^ plan.reservationBaseControl) &
            ~plan.expectedReadbackMask) == 0;
}

Cavs25CodeLoaderReservationSequence::Cavs25CodeLoaderReservationSequence()
    noexcept : plan_{}, initialized_(false), awaitingConfirmation_(false),
      failed_(false), confirmed_(false) {}

CodeLoaderReservationSequenceResult
Cavs25CodeLoaderReservationSequence::initialize(
    const Cavs25CodeLoaderStreamReservationPlan &plan,
    uint64_t hdaBarLength) noexcept {
    if (initialized_) return CodeLoaderReservationSequenceResult::InvalidState;
    if (plan.reserved != 0 || plan.streamTag == 0 || plan.streamTag > 15 ||
        plan.descriptorIndex >= kHdaMaximumTotalStreamCount ||
        plan.descriptorControlOffset != kHdaStreamDescriptorBase +
            static_cast<uint32_t>(plan.descriptorIndex) *
                kHdaStreamDescriptorStride ||
        plan.expectedReadbackMask != ((1U << 1) | 1U | (0xfU << 20)) ||
        plan.expectedReadbackValue != (static_cast<uint32_t>(plan.streamTag) << 20) ||
        (plan.reservationBaseControl & plan.expectedReadbackMask) != 0 ||
        (plan.controlWriteValue & plan.expectedReadbackMask) !=
            plan.expectedReadbackValue ||
        (plan.controlWriteValue & ~plan.expectedReadbackMask) !=
            (plan.reservationBaseControl & ~plan.expectedReadbackMask) ||
        static_cast<uint64_t>(plan.descriptorControlOffset) + 4U >
            hdaBarLength)
        return CodeLoaderReservationSequenceResult::InvalidPlan;
    plan_ = plan;
    initialized_ = true;
    awaitingConfirmation_ = false;
    failed_ = false;
    confirmed_ = false;
    return CodeLoaderReservationSequenceResult::WriteRequired;
}

CodeLoaderReservationSequenceResult
Cavs25CodeLoaderReservationSequence::next(
    HdaStreamRegisterWrite *write) noexcept {
    if (!write) return CodeLoaderReservationSequenceResult::InvalidArgument;
    if (!initialized_ || failed_ || awaitingConfirmation_)
        return CodeLoaderReservationSequenceResult::InvalidState;
    if (complete()) return CodeLoaderReservationSequenceResult::Complete;
    *write = {plan_.descriptorControlOffset, plan_.controlWriteValue, 4,
              {0, 0, 0}};
    awaitingConfirmation_ = true;
    return CodeLoaderReservationSequenceResult::WriteRequired;
}

CodeLoaderReservationSequenceResult
Cavs25CodeLoaderReservationSequence::confirm(uint32_t readback) noexcept {
    if (!initialized_ || failed_ || !awaitingConfirmation_)
        return CodeLoaderReservationSequenceResult::InvalidState;
    if (readback == UINT32_MAX) {
        failed_ = true;
        awaitingConfirmation_ = false;
        return CodeLoaderReservationSequenceResult::InvalidReadback;
    }
    if (!cavs25CodeLoaderStreamReservationMatches(plan_, readback)) {
        failed_ = true;
        awaitingConfirmation_ = false;
        return CodeLoaderReservationSequenceResult::ReadbackMismatch;
    }
    awaitingConfirmation_ = false;
    confirmed_ = true;
    return CodeLoaderReservationSequenceResult::Complete;
}

CodeLoaderReservationSequenceResult
Cavs25CodeLoaderReservationSequence::commandFailed() noexcept {
    if (!initialized_ || failed_ || !awaitingConfirmation_)
        return CodeLoaderReservationSequenceResult::InvalidState;
    failed_ = true;
    awaitingConfirmation_ = false;
    return CodeLoaderReservationSequenceResult::CommandFailed;
}

bool Cavs25CodeLoaderReservationSequence::complete() const noexcept {
    return initialized_ && !failed_ && confirmed_ && !awaitingConfirmation_;
}

} // namespace sof::ipc4::kernel
