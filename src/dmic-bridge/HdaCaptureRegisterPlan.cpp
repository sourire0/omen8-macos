#include "HdaCaptureRegisterPlan.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kStreamRunMask = 1U << 1;
constexpr uint32_t kStreamResetMask = 1U << 0;
constexpr uint32_t kStreamTagMask = 0x0fU << 20;
constexpr uint32_t kStreamIoce = 1U << 2;
constexpr uint32_t kStreamFeie = 1U << 3;
constexpr uint32_t kStreamDeie = 1U << 4;
constexpr uint32_t kStreamInterruptEnables =
    kStreamIoce | kStreamFeie | kStreamDeie;
constexpr uint32_t kStreamWritableControlMask = kStreamRunMask |
    kStreamResetMask | kStreamTagMask | kStreamInterruptEnables;
constexpr uint32_t kStreamCblOffset = 0x08;
constexpr uint32_t kStreamLviOffset = 0x0c;
constexpr uint32_t kStreamFormatOffset = 0x12;
constexpr uint32_t kStreamBdlLowOffset = 0x18;
constexpr uint32_t kStreamBdlHighOffset = 0x1c;
constexpr uint32_t kHdaIntctlOffset = 0x20;
constexpr uint32_t kPpctlRegisterOffset = 0x04;
constexpr uint32_t kPpctlGlobalProcessingEnable = 1U << 30;
constexpr uint32_t kPpctlInterruptEnable = 1U << 31;
constexpr uint32_t kHdaGlobalInterruptEnable = 1U << 31;
constexpr uint32_t kHdaCapabilityIdShift = 16;
constexpr uint32_t kHdaCapabilityIdMask = 0x0fffU << 16;
constexpr uint32_t kHdaPpCapabilityId = 0x003U;
constexpr uint32_t kBdlAlignmentMask = 127;
constexpr uint32_t kMaximumBdlEntries = 256;

bool validPcmFormat(uint16_t format) noexcept {
    if ((format & 0x8080U) != 0) return false;
    const uint16_t multiple = static_cast<uint16_t>((format >> 11) & 0x7U);
    const uint16_t bitsPerSample = static_cast<uint16_t>((format >> 4) & 0x7U);
    return multiple <= 3 && bitsPerSample <= 4;
}

template <typename Plan>
void setWrite(Plan *out, uint8_t index,
              uint32_t offset, uint32_t value,
              uint8_t widthBytes) noexcept {
    HdaStreamRegisterWrite &write = out->writes[index];
    write.offset = offset;
    write.value = value;
    write.widthBytes = widthBytes;
    write.reserved[0] = 0;
    write.reserved[1] = 0;
    write.reserved[2] = 0;
}

} // namespace

HdaCaptureRegisterPlanResult planHdaCaptureRegisterProgramming(
    const HdaInputStreamAllocationPlan &allocation,
    const HdaCaptureBdlPlan &bdl,
    const uint32_t previousStreamRegisterValues[6],
    uint16_t hdaFormatWord,
    const HdaInputStreamReservationSequence &reservationSequence,
    uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    HdaCaptureRegisterPlan *out) noexcept {
    if (!out) return HdaCaptureRegisterPlanResult::InvalidArgument;
    if (!controllerExclusivelyOwned)
        return HdaCaptureRegisterPlanResult::OwnershipNotProven;
    const HdaInputStreamReservationPlan *reservationPlan =
        reservationSequence.plan();
    const uint32_t reservedDescriptorControlReadback =
        reservationSequence.confirmedControlReadback();
    if (!reservationSequence.complete() || !reservationPlan ||
        reservationPlan->descriptorControlOffset !=
            allocation.descriptorControlOffset ||
        !hdaInputStreamReservationMatches(
            *reservationPlan, reservedDescriptorControlReadback))
        return HdaCaptureRegisterPlanResult::StreamNotReserved;
    if (!previousStreamRegisterValues)
        return HdaCaptureRegisterPlanResult::InvalidArgument;
    if (allocation.inputStreamCount == 0 ||
        allocation.inputStreamCount > kHdaMaximumStreamDescriptors ||
        allocation.descriptorIndex >= allocation.inputStreamCount ||
        allocation.streamTag == 0 || allocation.streamTag > 15 ||
        allocation.reserved != 0 || allocation.reserved2 != 0 ||
        (allocation.allocatedStreamTagMask &
            (1U << allocation.streamTag)) != 0 ||
        allocation.descriptorControlOffset != kHdaStreamDescriptorBase +
            static_cast<uint32_t>(allocation.descriptorIndex) *
                kHdaStreamDescriptorStride)
        return HdaCaptureRegisterPlanResult::InvalidArgument;

    if (reservedDescriptorControlReadback == UINT32_MAX ||
        (reservedDescriptorControlReadback &
            (kStreamRunMask | kStreamResetMask)) != 0 ||
        ((reservedDescriptorControlReadback & kStreamTagMask) >> 20) !=
            allocation.streamTag)
        return HdaCaptureRegisterPlanResult::InvalidStreamReadback;
    if (previousStreamRegisterValues[0] == UINT32_MAX ||
        previousStreamRegisterValues[1] == UINT32_MAX ||
        previousStreamRegisterValues[2] == UINT32_MAX ||
        previousStreamRegisterValues[3] == UINT32_MAX ||
        previousStreamRegisterValues[4] == UINT32_MAX ||
        previousStreamRegisterValues[5] == UINT32_MAX ||
        (previousStreamRegisterValues[5] &
            (kStreamRunMask | kStreamResetMask | kStreamTagMask)) != 0)
        return HdaCaptureRegisterPlanResult::InvalidStreamReadback;

    const uint64_t descriptorAddress =
        (static_cast<uint64_t>(bdl.descriptorBaseHigh) << 32) |
        bdl.descriptorBaseLow;
    if (bdl.entryCount == 0 || bdl.entryCount > kMaximumBdlEntries ||
        bdl.lastValidIndex != bdl.entryCount - 1 || bdl.reserved != 0 ||
        bdl.cyclicBufferLength == 0 || bdl.periodLength == 0 ||
        bdl.cyclicBufferLength % bdl.periodLength != 0 ||
        (descriptorAddress & kBdlAlignmentMask) != 0 ||
        descriptorAddress > UINT64_MAX -
            static_cast<uint64_t>(bdl.entryCount) * 16U)
        return HdaCaptureRegisterPlanResult::InvalidBdlPlan;
    if (!validPcmFormat(hdaFormatWord))
        return HdaCaptureRegisterPlanResult::InvalidFormat;

    const uint64_t streamEnd =
        static_cast<uint64_t>(allocation.descriptorControlOffset) + 0x20;
    if (streamEnd > hdaBarLength)
        return HdaCaptureRegisterPlanResult::InsufficientBar;

    const uint32_t base = allocation.descriptorControlOffset;
    out->streamControlOffset = base;
    // Follow the local reference sequence: describe the DMA ring first, then
    // enable stream interrupts using the confirmed tag readback as the RMW
    // base so unrelated SDn.CTL bits are preserved.
    setWrite(out, 0, base + kStreamBdlLowOffset,
             bdl.descriptorBaseLow, 4);
    setWrite(out, 1, base + kStreamBdlHighOffset,
             bdl.descriptorBaseHigh, 4);
    setWrite(out, 2, base + kStreamCblOffset,
             bdl.cyclicBufferLength, 4);
    setWrite(out, 3, base + kStreamLviOffset,
             bdl.lastValidIndex, 2);
    setWrite(out, 4, base + kStreamFormatOffset,
             hdaFormatWord, 2);
    setWrite(out, 5, base,
             (reservedDescriptorControlReadback &
                 ~kStreamInterruptEnables) | kStreamInterruptEnables,
             4);
    out->writeCount = 6;
    for (uint8_t i = 0; i < 6; ++i)
        out->previousValues[i] = previousStreamRegisterValues[i];
    // Reservation changed SDn.CTL after the fresh idle snapshot. Preserve the
    // confirmed tagged value as the setup transaction's true rollback base.
    out->previousValues[5] = reservedDescriptorControlReadback;
    out->reserved[0] = 0;
    out->reserved[1] = 0;
    out->reserved[2] = 0;
    return HdaCaptureRegisterPlanResult::Ok;
}

HdaCaptureRegisterPlanResult planHdaCaptureRouting(
    const HdaInputStreamAllocationPlan &allocation,
    uint32_t ppCapabilityOffset,
    uint32_t ppCapabilityHeaderReadback,
    uint32_t ppctlReadback,
    uint32_t intctlReadback,
    uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    HdaCaptureRoutingPlan *out) noexcept {
    if (!out) return HdaCaptureRegisterPlanResult::InvalidArgument;
    if (!controllerExclusivelyOwned)
        return HdaCaptureRegisterPlanResult::OwnershipNotProven;
    if (allocation.inputStreamCount == 0 ||
        allocation.inputStreamCount > kHdaMaximumStreamDescriptors ||
        allocation.descriptorIndex >= allocation.inputStreamCount ||
        allocation.streamTag == 0 || allocation.streamTag > 15 ||
        allocation.reserved != 0 || allocation.reserved2 != 0 ||
        (allocation.allocatedStreamTagMask &
            (1U << allocation.streamTag)) != 0 ||
        allocation.descriptorControlOffset != kHdaStreamDescriptorBase +
            static_cast<uint32_t>(allocation.descriptorIndex) *
                kHdaStreamDescriptorStride)
        return HdaCaptureRegisterPlanResult::InvalidArgument;
    if (ppCapabilityOffset < 0x40 || (ppCapabilityOffset & 3U) != 0 ||
        ppCapabilityHeaderReadback == UINT32_MAX ||
        ((ppCapabilityHeaderReadback & kHdaCapabilityIdMask) >>
            kHdaCapabilityIdShift) != kHdaPpCapabilityId)
        return HdaCaptureRegisterPlanResult::InvalidCapability;
    if (
        ppctlReadback == UINT32_MAX || intctlReadback == UINT32_MAX)
        return HdaCaptureRegisterPlanResult::InvalidStreamReadback;

    const uint64_t ppctlOffset =
        static_cast<uint64_t>(ppCapabilityOffset) + kPpctlRegisterOffset;
    if (ppctlOffset + sizeof(uint32_t) > hdaBarLength ||
        static_cast<uint64_t>(kHdaIntctlOffset) + sizeof(uint32_t) >
            hdaBarLength)
        return HdaCaptureRegisterPlanResult::InsufficientBar;

    const uint32_t streamBit = 1U << allocation.descriptorIndex;
    setWrite(out, 0, ppCapabilityOffset + kPpctlRegisterOffset,
             ppctlReadback | streamBit | kPpctlGlobalProcessingEnable |
                 kPpctlInterruptEnable,
             4);
    setWrite(out, 1, kHdaIntctlOffset,
             intctlReadback | streamBit | kHdaGlobalInterruptEnable,
             4);
    out->previousValues[0] = ppctlReadback;
    out->previousValues[1] = intctlReadback;
    out->writeCount = 2;
    out->reserved[0] = 0;
    out->reserved[1] = 0;
    out->reserved[2] = 0;
    return HdaCaptureRegisterPlanResult::Ok;
}

HdaCaptureRegisterPlanResult combineHdaCaptureSetupPlans(
    const HdaCaptureRegisterPlan &streamPlan,
    const HdaCaptureRoutingPlan &routingPlan,
    uint64_t hdaBarLength,
    HdaCaptureSetupPlan *out) noexcept {
    if (!out) return HdaCaptureRegisterPlanResult::InvalidArgument;
    if (streamPlan.writeCount != 6 || routingPlan.writeCount != 2 ||
        streamPlan.streamControlOffset < kHdaStreamDescriptorBase ||
        ((streamPlan.streamControlOffset - kHdaStreamDescriptorBase) %
            kHdaStreamDescriptorStride) != 0)
        return HdaCaptureRegisterPlanResult::InvalidArgument;

    const uint32_t descriptorIndex =
        (streamPlan.streamControlOffset - kHdaStreamDescriptorBase) /
        kHdaStreamDescriptorStride;
    if (descriptorIndex >= kHdaMaximumStreamDescriptors)
        return HdaCaptureRegisterPlanResult::InvalidArgument;
    const uint32_t streamBit = 1U << descriptorIndex;
    const uint32_t tag = (streamPlan.writes[5].value & kStreamTagMask) >> 20;
    if (tag == 0 || tag > 15 ||
        (streamPlan.writes[5].value &
            (kStreamRunMask | kStreamResetMask)) != 0 ||
        routingPlan.writes[0].offset < 0x44 ||
        (routingPlan.writes[0].offset & 3U) != 0 ||
        routingPlan.writes[0].widthBytes != 4 ||
        routingPlan.writes[1].offset != kHdaIntctlOffset ||
        routingPlan.writes[1].widthBytes != 4 ||
        (routingPlan.writes[0].value &
            (streamBit | kPpctlGlobalProcessingEnable |
             kPpctlInterruptEnable)) !=
            (streamBit | kPpctlGlobalProcessingEnable |
             kPpctlInterruptEnable) ||
        (routingPlan.writes[1].value &
            (streamBit | kHdaGlobalInterruptEnable)) !=
            (streamBit | kHdaGlobalInterruptEnable))
        return HdaCaptureRegisterPlanResult::InvalidArgument;

    const uint32_t base = streamPlan.streamControlOffset;
    const uint32_t expectedOffsets[6] = {
        base + kStreamBdlLowOffset,
        base + kStreamBdlHighOffset,
        base + kStreamCblOffset,
        base + kStreamLviOffset,
        base + kStreamFormatOffset,
        base,
    };
    const uint8_t expectedWidths[6] = {4, 4, 4, 2, 2, 4};
    for (uint8_t i = 0; i < 6; ++i) {
        const HdaStreamRegisterWrite &write = streamPlan.writes[i];
        if (write.offset != expectedOffsets[i] ||
            write.widthBytes != expectedWidths[i])
            return HdaCaptureRegisterPlanResult::InvalidArgument;
        if (static_cast<uint64_t>(write.offset) + write.widthBytes >
            hdaBarLength)
            return HdaCaptureRegisterPlanResult::InsufficientBar;
    }
    const uint64_t bdlAddress =
        (static_cast<uint64_t>(streamPlan.writes[1].value) << 32) |
        streamPlan.writes[0].value;
    if ((bdlAddress & kBdlAlignmentMask) != 0 ||
        streamPlan.writes[2].value == 0 ||
        streamPlan.writes[3].value >= kMaximumBdlEntries ||
        streamPlan.writes[4].value > UINT16_MAX)
        return HdaCaptureRegisterPlanResult::InvalidBdlPlan;
    if (static_cast<uint64_t>(routingPlan.writes[0].offset) + 4U >
            hdaBarLength ||
        static_cast<uint64_t>(routingPlan.writes[1].offset) + 4U >
            hdaBarLength)
        return HdaCaptureRegisterPlanResult::InsufficientBar;

    HdaCaptureSetupPlan combined{};
    combined.streamControlOffset = base;
    combined.writeCount = 8;
    combined.writes[0] = routingPlan.writes[0];
    combined.previousValues[0] = routingPlan.previousValues[0];
    combined.readbackMasks[0] = UINT32_MAX;
    combined.readbackValues[0] = routingPlan.writes[0].value;
    for (uint8_t i = 0; i < 6; ++i) {
        combined.writes[i + 1] = streamPlan.writes[i];
        combined.previousValues[i + 1] = streamPlan.previousValues[i];
    }
    combined.writes[7] = routingPlan.writes[1];
    combined.previousValues[7] = routingPlan.previousValues[1];
    combined.readbackMasks[1] = UINT32_MAX;
    combined.readbackValues[1] = streamPlan.writes[0].value;
    combined.readbackMasks[2] = UINT32_MAX;
    combined.readbackValues[2] = streamPlan.writes[1].value;
    combined.readbackMasks[3] = UINT32_MAX;
    combined.readbackValues[3] = streamPlan.writes[2].value;
    combined.readbackMasks[4] = 0xffU;
    combined.readbackValues[4] = streamPlan.writes[3].value & 0xffU;
    combined.readbackMasks[5] = 0xffffU;
    combined.readbackValues[5] = streamPlan.writes[4].value & 0xffffU;
    combined.readbackMasks[6] = kStreamRunMask | kStreamResetMask |
        kStreamTagMask | kStreamInterruptEnables;
    combined.readbackValues[6] = streamPlan.writes[5].value &
        combined.readbackMasks[6];
    combined.readbackMasks[7] = UINT32_MAX;
    combined.readbackValues[7] = routingPlan.writes[1].value;
    *out = combined;
    return HdaCaptureRegisterPlanResult::Ok;
}

HdaCaptureRegisterPlanResult planHdaCaptureRollback(
    const HdaCaptureSetupPlan &setupPlan,
    uint32_t freshPpctlReadback,
    uint32_t freshIntctlReadback,
    const uint32_t freshStreamRegisterValues[6],
    uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    bool dmaStoppedAndDrainConfirmed,
    bool streamResetConfirmed,
    HdaCaptureRollbackPlan *out) noexcept {
    if (!out) return HdaCaptureRegisterPlanResult::InvalidArgument;
    if (!controllerExclusivelyOwned)
        return HdaCaptureRegisterPlanResult::OwnershipNotProven;
    if (!dmaStoppedAndDrainConfirmed || !streamResetConfirmed)
        return HdaCaptureRegisterPlanResult::StreamNotReserved;
    if (setupPlan.writeCount != 8 ||
        setupPlan.streamControlOffset < kHdaStreamDescriptorBase ||
        ((setupPlan.streamControlOffset - kHdaStreamDescriptorBase) %
            kHdaStreamDescriptorStride) != 0 ||
        setupPlan.writes[7].offset != kHdaIntctlOffset ||
        setupPlan.writes[7].widthBytes != 4 ||
        freshPpctlReadback == UINT32_MAX ||
        freshIntctlReadback == UINT32_MAX ||
        !freshStreamRegisterValues ||
        freshStreamRegisterValues[5] == UINT32_MAX ||
        (freshStreamRegisterValues[5] &
            (kStreamRunMask | kStreamResetMask)) != 0)
        return HdaCaptureRegisterPlanResult::InvalidStreamReadback;
    for (uint8_t i = 0; i < 6; ++i) {
        if (freshStreamRegisterValues[i] == UINT32_MAX)
            return HdaCaptureRegisterPlanResult::InvalidStreamReadback;
    }

    const uint32_t descriptorIndex =
        (setupPlan.streamControlOffset - kHdaStreamDescriptorBase) /
        kHdaStreamDescriptorStride;
    if (descriptorIndex >= kHdaMaximumStreamDescriptors)
        return HdaCaptureRegisterPlanResult::InvalidArgument;
    const uint32_t streamBit = 1U << descriptorIndex;
    const uint32_t ppctlMask = streamBit |
        kPpctlGlobalProcessingEnable | kPpctlInterruptEnable;
    const uint32_t intctlMask = streamBit | kHdaGlobalInterruptEnable;
    const uint32_t ppctlOffset = setupPlan.writes[0].offset;
    if (ppctlOffset < 0x44 || (ppctlOffset & 3U) != 0 ||
        static_cast<uint64_t>(ppctlOffset) + 4U > hdaBarLength ||
        static_cast<uint64_t>(kHdaIntctlOffset) + 4U > hdaBarLength)
        return HdaCaptureRegisterPlanResult::InsufficientBar;

    const uint32_t base = setupPlan.streamControlOffset;
    const uint32_t offsets[6] = {
        base + kStreamBdlLowOffset,
        base + kStreamBdlHighOffset,
        base + kStreamCblOffset,
        base + kStreamLviOffset,
        base + kStreamFormatOffset,
        base,
    };
    const uint8_t widths[6] = {4, 4, 4, 2, 2, 4};
    for (uint8_t i = 0; i < 6; ++i) {
        if (setupPlan.writes[i + 1].offset != offsets[i] ||
            setupPlan.writes[i + 1].widthBytes != widths[i] ||
            static_cast<uint64_t>(offsets[i]) + widths[i] > hdaBarLength ||
            setupPlan.previousValues[i + 1] == UINT32_MAX)
            return HdaCaptureRegisterPlanResult::InvalidArgument;
    }
    const uint32_t reservedTag =
        setupPlan.writes[6].value & kStreamTagMask;
    if (reservedTag == 0 ||
        (setupPlan.previousValues[6] &
            (kStreamRunMask | kStreamResetMask)) != 0 ||
        (setupPlan.previousValues[6] & kStreamTagMask) != reservedTag ||
        (freshStreamRegisterValues[5] & kStreamTagMask) != reservedTag ||
        setupPlan.previousValues[0] == UINT32_MAX ||
        setupPlan.previousValues[7] == UINT32_MAX)
        return HdaCaptureRegisterPlanResult::InvalidStreamReadback;

    HdaCaptureRollbackPlan rollback{};
    rollback.writeCount = 8;
    rollback.preconditionValues[0] = freshIntctlReadback;
    rollback.preconditionValues[1] = freshPpctlReadback;
    setWrite(&rollback, 0, kHdaIntctlOffset,
             (freshIntctlReadback & ~intctlMask) |
                 (setupPlan.previousValues[7] & intctlMask), 4);
    rollback.readbackMasks[0] = intctlMask;
    rollback.readbackValues[0] = setupPlan.previousValues[7] & intctlMask;
    setWrite(&rollback, 1, ppctlOffset,
             (freshPpctlReadback & ~ppctlMask) |
                 (setupPlan.previousValues[0] & ppctlMask), 4);
    rollback.readbackMasks[1] = ppctlMask;
    rollback.readbackValues[1] = setupPlan.previousValues[0] & ppctlMask;
    for (uint8_t i = 0; i < 6; ++i) {
        setWrite(&rollback, static_cast<uint8_t>(i + 2), offsets[i],
                 i == 5
                     ? setupPlan.previousValues[i + 1] &
                           kStreamWritableControlMask
                     : setupPlan.previousValues[i + 1],
                 widths[i]);
        rollback.readbackMasks[i + 2] = i == 3 ? 0xffU
            : (i == 4 ? 0xffffU :
                (i == 5 ? kStreamWritableControlMask : UINT32_MAX));
        rollback.readbackValues[i + 2] =
            setupPlan.previousValues[i + 1] & rollback.readbackMasks[i + 2];
        rollback.preconditionValues[i + 2] = freshStreamRegisterValues[i];
    }
    *out = rollback;
    return HdaCaptureRegisterPlanResult::Ok;
}

HdaCaptureSetupSequence::HdaCaptureSetupSequence() noexcept
    : plan_{}, cursor_(0), initialized_(false),
      awaitingConfirmation_(false), failed_(false) {}

HdaCaptureSetupSequenceResult HdaCaptureSetupSequence::initialize(
    const HdaCaptureSetupPlan &plan, uint64_t hdaBarLength) noexcept {
    return initializeSequenceData(plan, hdaBarLength, true);
}

HdaCaptureSetupSequenceResult HdaCaptureSetupSequence::initializeSequenceData(
    const HdaCaptureSetupPlan &plan, uint64_t hdaBarLength,
    bool validateSetupOrder) noexcept {
    if (initialized_) return HdaCaptureSetupSequenceResult::InvalidState;
    if (plan.writeCount != 8 ||
        plan.streamControlOffset < kHdaStreamDescriptorBase ||
        ((plan.streamControlOffset - kHdaStreamDescriptorBase) %
            kHdaStreamDescriptorStride) != 0)
        return HdaCaptureSetupSequenceResult::InvalidPlan;
    const uint32_t descriptorIndex =
        (plan.streamControlOffset - kHdaStreamDescriptorBase) /
        kHdaStreamDescriptorStride;
    if (descriptorIndex >= kHdaMaximumStreamDescriptors ||
        (validateSetupOrder && (
        plan.writes[0].offset < 0x44 ||
        (plan.writes[0].offset & 3U) != 0 ||
        plan.writes[7].offset != kHdaIntctlOffset)))
        return HdaCaptureSetupSequenceResult::InvalidPlan;
    const uint32_t streamBit = 1U << descriptorIndex;
    const uint32_t expectedOffsets[8] = {
        plan.writes[0].offset,
        plan.streamControlOffset + kStreamBdlLowOffset,
        plan.streamControlOffset + kStreamBdlHighOffset,
        plan.streamControlOffset + kStreamCblOffset,
        plan.streamControlOffset + kStreamLviOffset,
        plan.streamControlOffset + kStreamFormatOffset,
        plan.streamControlOffset,
        kHdaIntctlOffset,
    };
    const uint8_t expectedWidths[8] = {4, 4, 4, 4, 2, 2, 4, 4};
    const uint32_t expectedMasks[8] = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        0xffU, 0xffffU, kStreamWritableControlMask, UINT32_MAX,
    };
    const uint32_t expectedTag =
        (plan.writes[6].value & kStreamTagMask) >> 20;
    const uint64_t bdlAddress =
        (static_cast<uint64_t>(plan.writes[2].value) << 32) |
        plan.writes[1].value;
    if (validateSetupOrder && (expectedTag == 0 || expectedTag > 15 ||
        (plan.writes[6].value &
            (kStreamRunMask | kStreamResetMask)) != 0 ||
        (plan.writes[6].value & kStreamInterruptEnables) !=
            kStreamInterruptEnables ||
        (bdlAddress & kBdlAlignmentMask) != 0 ||
        plan.writes[3].value == 0 || plan.writes[4].value >= kMaximumBdlEntries ||
        plan.writes[5].value > UINT16_MAX ||
        !validPcmFormat(static_cast<uint16_t>(plan.writes[5].value)) ||
        (plan.writes[0].value &
            (streamBit | kPpctlGlobalProcessingEnable |
             kPpctlInterruptEnable)) !=
            (streamBit | kPpctlGlobalProcessingEnable |
             kPpctlInterruptEnable) ||
        (plan.writes[7].value &
            (streamBit | kHdaGlobalInterruptEnable)) !=
            (streamBit | kHdaGlobalInterruptEnable)))
        return HdaCaptureSetupSequenceResult::InvalidPlan;
    for (uint8_t i = 0; i < plan.writeCount; ++i) {
        if ((validateSetupOrder &&
             (plan.writes[i].offset != expectedOffsets[i] ||
              plan.writes[i].widthBytes != expectedWidths[i] ||
              plan.readbackMasks[i] != expectedMasks[i])) ||
            (plan.writes[i].widthBytes != 2 &&
             plan.writes[i].widthBytes != 4) ||
            static_cast<uint64_t>(plan.writes[i].offset) +
                plan.writes[i].widthBytes > hdaBarLength ||
            plan.readbackMasks[i] == 0 ||
            (plan.readbackValues[i] & ~plan.readbackMasks[i]) != 0 ||
            (plan.writes[i].value & plan.readbackMasks[i]) !=
                plan.readbackValues[i])
            return HdaCaptureSetupSequenceResult::InvalidPlan;
    }
    plan_ = plan;
    cursor_ = 0;
    awaitingConfirmation_ = false;
    failed_ = false;
    initialized_ = true;
    return HdaCaptureSetupSequenceResult::WriteRequired;
}

HdaCaptureSetupSequenceResult HdaCaptureSetupSequence::next(
    HdaStreamRegisterWrite *write) noexcept {
    if (!write) return HdaCaptureSetupSequenceResult::InvalidArgument;
    if (!initialized_) return HdaCaptureSetupSequenceResult::InvalidState;
    if (failed_ || awaitingConfirmation_)
        return HdaCaptureSetupSequenceResult::InvalidState;
    if (cursor_ >= plan_.writeCount)
        return HdaCaptureSetupSequenceResult::Complete;
    *write = plan_.writes[cursor_];
    awaitingConfirmation_ = true;
    return HdaCaptureSetupSequenceResult::WriteRequired;
}

bool HdaCaptureSetupSequence::pendingWritePrecondition(
    uint32_t *value, uint8_t *widthBytes) const noexcept {
    if (!value || !widthBytes || !initialized_ || failed_ ||
        !awaitingConfirmation_ || cursor_ >= plan_.writeCount)
        return false;
    *value = plan_.previousValues[cursor_];
    *widthBytes = plan_.writes[cursor_].widthBytes;
    return *value != UINT32_MAX;
}

HdaCaptureSetupSequenceResult HdaCaptureSetupSequence::confirm(
    uint32_t readback) noexcept {
    if (!initialized_ || failed_ || !awaitingConfirmation_)
        return HdaCaptureSetupSequenceResult::InvalidState;
    if (readback == UINT32_MAX) {
        failed_ = true;
        awaitingConfirmation_ = false;
        return HdaCaptureSetupSequenceResult::InvalidReadback;
    }
    if ((readback & plan_.readbackMasks[cursor_]) !=
        plan_.readbackValues[cursor_]) {
        failed_ = true;
        awaitingConfirmation_ = false;
        return HdaCaptureSetupSequenceResult::ReadbackMismatch;
    }
    ++cursor_;
    awaitingConfirmation_ = false;
    return cursor_ == plan_.writeCount
        ? HdaCaptureSetupSequenceResult::Complete
        : HdaCaptureSetupSequenceResult::Matched;
}

bool HdaCaptureSetupSequence::complete() const noexcept {
    return initialized_ && !failed_ && !awaitingConfirmation_ &&
           cursor_ == plan_.writeCount;
}

HdaCaptureSetupSequenceResult HdaCaptureSetupSequence::commandFailed()
    noexcept {
    if (!initialized_ || failed_ || !awaitingConfirmation_)
        return HdaCaptureSetupSequenceResult::InvalidState;
    failed_ = true;
    awaitingConfirmation_ = false;
    return HdaCaptureSetupSequenceResult::CommandFailed;
}

HdaCaptureRollbackSequence::HdaCaptureRollbackSequence() noexcept
    : sequence_() {}

HdaCaptureSetupSequenceResult HdaCaptureRollbackSequence::initialize(
    const HdaCaptureRollbackPlan &plan,
    uint64_t hdaBarLength) noexcept {
    if (plan.writeCount != 8 ||
        plan.writes[0].offset != kHdaIntctlOffset ||
        plan.writes[0].widthBytes != 4 ||
        plan.writes[1].offset < 0x44 ||
        (plan.writes[1].offset & 3U) != 0 ||
        plan.writes[1].widthBytes != 4 ||
        plan.writes[7].offset < kHdaStreamDescriptorBase ||
        ((plan.writes[7].offset - kHdaStreamDescriptorBase) %
            kHdaStreamDescriptorStride) != 0)
        return HdaCaptureSetupSequenceResult::InvalidPlan;

    const uint32_t base = plan.writes[7].offset;
    const uint32_t descriptorIndex =
        (base - kHdaStreamDescriptorBase) / kHdaStreamDescriptorStride;
    if (descriptorIndex >= kHdaMaximumStreamDescriptors)
        return HdaCaptureSetupSequenceResult::InvalidPlan;
    const uint32_t streamBit = 1U << descriptorIndex;
    const uint32_t expectedOffsets[8] = {
        kHdaIntctlOffset,
        plan.writes[1].offset,
        base + kStreamBdlLowOffset,
        base + kStreamBdlHighOffset,
        base + kStreamCblOffset,
        base + kStreamLviOffset,
        base + kStreamFormatOffset,
        base,
    };
    const uint8_t expectedWidths[8] = {4, 4, 4, 4, 4, 2, 2, 4};
    const uint32_t expectedMasks[8] = {
        streamBit | kHdaGlobalInterruptEnable,
        streamBit | kPpctlGlobalProcessingEnable | kPpctlInterruptEnable,
        UINT32_MAX,
        UINT32_MAX,
        UINT32_MAX,
        0xffU,
        0xffffU,
        kStreamWritableControlMask,
    };
    HdaCaptureSetupPlan sequencedPlan{};
    sequencedPlan.streamControlOffset = base;
    sequencedPlan.writeCount = 8;
    for (uint8_t i = 0; i < 8; ++i) {
        if (plan.writes[i].offset != expectedOffsets[i] ||
            plan.writes[i].widthBytes != expectedWidths[i] ||
            static_cast<uint64_t>(plan.writes[i].offset) +
                plan.writes[i].widthBytes > hdaBarLength ||
            plan.preconditionValues[i] == UINT32_MAX ||
            plan.readbackMasks[i] != expectedMasks[i] ||
            (plan.readbackValues[i] & ~plan.readbackMasks[i]) != 0 ||
            (plan.writes[i].value & plan.readbackMasks[i]) !=
                plan.readbackValues[i])
            return HdaCaptureSetupSequenceResult::InvalidPlan;
        sequencedPlan.writes[i] = plan.writes[i];
        sequencedPlan.previousValues[i] = plan.preconditionValues[i];
        sequencedPlan.readbackMasks[i] = plan.readbackMasks[i];
        sequencedPlan.readbackValues[i] = plan.readbackValues[i];
    }
    return sequence_.initializeSequenceData(sequencedPlan, hdaBarLength,
                                            false);
}

HdaCaptureSetupSequenceResult HdaCaptureRollbackSequence::next(
    HdaStreamRegisterWrite *write) noexcept {
    return sequence_.next(write);
}

bool HdaCaptureRollbackSequence::pendingWritePrecondition(
    uint32_t *value, uint8_t *widthBytes) const noexcept {
    return sequence_.pendingWritePrecondition(value, widthBytes);
}

HdaCaptureSetupSequenceResult HdaCaptureRollbackSequence::confirm(
    uint32_t readback) noexcept {
    return sequence_.confirm(readback);
}

HdaCaptureSetupSequenceResult HdaCaptureRollbackSequence::commandFailed()
    noexcept {
    return sequence_.commandFailed();
}

bool HdaCaptureRollbackSequence::complete() const noexcept {
    return sequence_.complete();
}

bool HdaCaptureRollbackSequence::failed() const noexcept {
    return sequence_.failed();
}

} // namespace sof::ipc4::kernel
