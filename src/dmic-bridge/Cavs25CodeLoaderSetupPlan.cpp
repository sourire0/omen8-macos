#include "Cavs25CodeLoaderSetupPlan.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kPpCapabilityIdMask = 0x0fffU << 16;
constexpr uint32_t kPpCapabilityId = 0x003U;
constexpr uint32_t kSpibCapabilityId = 0x004U;
constexpr uint32_t kPpCtlOffset = 0x04;
constexpr uint32_t kPpGlobalProcessingEnable = 1U << 30;
constexpr uint32_t kPpInterruptEnable = 1U << 31;
constexpr uint32_t kIntctlOffset = 0x20;
constexpr uint32_t kIntGlobalEnable = 1U << 31;
constexpr uint32_t kIntControllerEnable = 1U << 30;
constexpr uint32_t kSpbFcCtlOffset = 0x04;
constexpr uint32_t kSpibPositionsOffset = 0x08;
constexpr uint32_t kSpibStreamStride = 0x08;
constexpr uint32_t kStreamInterruptMask = 0x1cU;
constexpr uint32_t kStreamControlWritableMask =
    kHdaStreamResetMask | kHdaStreamRunMask | kHdaStreamTagMask |
    kStreamInterruptMask;
constexpr uint32_t kStreamBdlLowOffset = 0x18;
constexpr uint32_t kStreamBdlHighOffset = 0x1c;
constexpr uint32_t kStreamCblOffset = 0x08;
constexpr uint32_t kStreamLviOffset = 0x0c;
constexpr uint32_t kStreamFormatOffset = 0x12;
constexpr uint32_t kRequiredFormat = 0x0040U;

bool validDmaPlan(const Cavs25DmaPlan &plan) noexcept {
    const uint64_t bdlAddress =
        (static_cast<uint64_t>(plan.descriptorBaseHigh) << 32) |
        plan.descriptorBaseLow;
    const uint64_t firstAddress =
        (static_cast<uint64_t>(plan.entries[0].addressHigh) << 32) |
        plan.entries[0].addressLow;
    const uint64_t secondAddress =
        (static_cast<uint64_t>(plan.entries[1].addressHigh) << 32) |
        plan.entries[1].addressLow;
    const uint64_t combinedBytes =
        static_cast<uint64_t>(plan.entries[0].byteCount) +
        plan.entries[1].byteCount;
    const bool secondAddressFits = firstAddress <= UINT64_MAX -
        plan.entries[0].byteCount;
    const bool secondRangeFits = secondAddress <= UINT64_MAX -
        plan.entries[1].byteCount;
    return plan.reserved == 0 && plan.lastValidIndex == 1 &&
        plan.cyclicBufferLength >= 256 &&
        (plan.cyclicBufferLength & 3U) == 0 &&
        plan.entries[0].byteCount != 0 &&
        (plan.entries[0].byteCount & 127U) == 0 &&
        plan.entries[1].byteCount != 0 &&
        plan.entries[0].interruptOnCompletion == 0 &&
        plan.entries[1].interruptOnCompletion == 1 &&
        bdlAddress != 0 && (bdlAddress & 127U) == 0 &&
        firstAddress != 0 && (firstAddress & 127U) == 0 &&
        secondAddressFits && secondRangeFits && secondAddress ==
            firstAddress + plan.entries[0].byteCount &&
        combinedBytes == plan.cyclicBufferLength;
}

void setWrite(Cavs25CodeLoaderSetupPlan *plan, uint8_t index,
              uint32_t offset, uint32_t value, uint8_t width,
              uint32_t previousValue, uint32_t readbackMask,
              uint32_t readbackValue) noexcept {
    HdaStreamRegisterWrite &write = plan->writes[index];
    write.offset = offset;
    write.value = value;
    write.widthBytes = width;
    write.reserved[0] = 0;
    write.reserved[1] = 0;
    write.reserved[2] = 0;
    plan->previousValues[index] = previousValue;
    plan->readbackMasks[index] = readbackMask;
    plan->readbackValues[index] = readbackValue;
}

bool writeFits(uint32_t offset, uint8_t width, uint64_t barLength) noexcept {
    return (width == 2 || width == 4) && (offset % width) == 0 &&
        static_cast<uint64_t>(offset) + width <= barLength;
}

bool validSetupPlan(const Cavs25CodeLoaderSetupPlan &plan,
                    uint64_t barLength) noexcept {
    if ((plan.writeCount != 8 && plan.writeCount != 10) ||
        plan.reserved != 0 ||
        plan.streamIndex >= kHdaMaximumTotalStreamCount ||
        plan.streamControlOffset != kHdaStreamDescriptorBase +
            static_cast<uint32_t>(plan.streamIndex) *
                kHdaStreamDescriptorStride ||
        plan.ppCapabilityOffset < 0x40 ||
        (plan.ppCapabilityOffset & 3U) != 0 ||
        plan.ppCapabilityHeader == UINT32_MAX ||
        ((plan.ppCapabilityHeader & kPpCapabilityIdMask) >> 16) !=
            kPpCapabilityId ||
        plan.ppCapabilityOffset > UINT32_MAX - kPpCtlOffset ||
        plan.writes[0].offset !=
            plan.ppCapabilityOffset + kPpCtlOffset ||
        plan.writes[7].offset != kIntctlOffset)
        return false;
    const uint32_t streamBit = 1U << plan.streamIndex;
    const uint32_t expectedOffsets[8] = {
        plan.writes[0].offset,
        plan.streamControlOffset + kStreamBdlLowOffset,
        plan.streamControlOffset + kStreamBdlHighOffset,
        plan.streamControlOffset + kStreamCblOffset,
        plan.streamControlOffset + kStreamLviOffset,
        plan.streamControlOffset + kStreamFormatOffset,
        plan.streamControlOffset,
        kIntctlOffset,
    };
    const uint8_t expectedWidths[8] = {4, 4, 4, 4, 2, 2, 4, 4};
    const uint32_t expectedMasks[8] = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        0xffU, 0xffffU, kStreamControlWritableMask, UINT32_MAX,
    };
    const uint32_t control = plan.writes[6].value;
    if ((plan.writes[0].value &
            (streamBit | kPpGlobalProcessingEnable | kPpInterruptEnable)) !=
            (streamBit | kPpGlobalProcessingEnable | kPpInterruptEnable) ||
        (plan.writes[3].value == 0) || plan.writes[4].value != 1 ||
        plan.writes[5].value != kRequiredFormat ||
        (control & (kHdaStreamRunMask | kHdaStreamResetMask)) != 0 ||
        ((control & kHdaStreamTagMask) >> 20) != 1 ||
        (control & kStreamInterruptMask) != kStreamInterruptMask ||
        (plan.writes[7].value &
            (streamBit | kIntGlobalEnable | kIntControllerEnable)) !=
            (streamBit | kIntGlobalEnable | kIntControllerEnable))
        return false;

    for (uint8_t i = 0; i < 8; ++i) {
        if (plan.writes[i].offset != expectedOffsets[i] ||
            plan.writes[i].widthBytes != expectedWidths[i] ||
            plan.reserved != 0 || plan.writes[i].reserved[0] != 0 ||
            plan.writes[i].reserved[1] != 0 ||
            plan.writes[i].reserved[2] != 0 ||
            !writeFits(plan.writes[i].offset, plan.writes[i].widthBytes,
                       barLength) ||
            plan.readbackMasks[i] != expectedMasks[i] ||
            (plan.readbackValues[i] & ~plan.readbackMasks[i]) != 0 ||
            (plan.writes[i].value & plan.readbackMasks[i]) !=
                plan.readbackValues[i] ||
            plan.previousValues[i] == UINT32_MAX)
            return false;
    }
    if (plan.writeCount == 8)
        return !plan.spibPresent && plan.spibWriteIndex == 0 &&
            plan.spibCapabilityOffset == 0 &&
            plan.spibCapabilityHeader == 0;
    if (!plan.spibPresent || plan.spibWriteIndex != 8 ||
        plan.spibCapabilityOffset < 0x40 ||
        (plan.spibCapabilityOffset & 3U) != 0 ||
        plan.spibCapabilityHeader == UINT32_MAX ||
        ((plan.spibCapabilityHeader & kPpCapabilityIdMask) >> 16) !=
            kSpibCapabilityId ||
        plan.spibCapabilityOffset > UINT32_MAX - kSpbFcCtlOffset ||
        plan.writes[8].offset !=
            plan.spibCapabilityOffset + kSpbFcCtlOffset ||
        plan.writes[8].widthBytes != 4 ||
        !writeFits(plan.writes[8].offset, 4, barLength) ||
        plan.readbackMasks[8] != streamBit ||
        (plan.readbackValues[8] & ~streamBit) != 0 ||
        (plan.readbackValues[8] & streamBit) == 0 ||
        (plan.writes[8].value & streamBit) == 0 ||
        plan.writes[8].reserved[0] != 0 ||
        plan.writes[8].reserved[1] != 0 ||
        plan.writes[8].reserved[2] != 0 ||
        plan.previousValues[8] == UINT32_MAX ||
        plan.writes[9].widthBytes != 4 ||
        plan.writes[9].offset != plan.writes[8].offset + 4U +
            static_cast<uint32_t>(plan.streamIndex) * kSpibStreamStride ||
        !writeFits(plan.writes[9].offset, 4, barLength) ||
        plan.readbackMasks[9] != UINT32_MAX ||
        plan.writes[9].value != plan.writes[3].value ||
        plan.readbackValues[9] != plan.writes[9].value ||
        plan.previousValues[9] == UINT32_MAX ||
        plan.writes[9].reserved[0] != 0 ||
        plan.writes[9].reserved[1] != 0 ||
        plan.writes[9].reserved[2] != 0)
        return false;
    return true;
}

// Reset action is deliberately kept outside the ordinary setup/rollback
// sequence because SDn.SRST is a handshake, not a stable configuration value.

} // namespace

Cavs25CodeLoaderResetSequence::Cavs25CodeLoaderResetSequence() noexcept
    : state_(State::Uninitialized), controlOffset_(0), initialControl_(0),
      samplesRemaining_(0), maximumPollSamples_(0) {}

bool Cavs25CodeLoaderResetSequence::initialize(
    const Cavs25CodeLoaderStreamReservationPlan &reservation,
    uint32_t initialControlReadback, uint64_t hdaBarLength,
    uint32_t maximumPollSamples, bool controllerExclusivelyOwned,
    bool descriptorVerifiedEmpty) noexcept {
    if (state_ != State::Uninitialized || !controllerExclusivelyOwned ||
        !descriptorVerifiedEmpty || maximumPollSamples == 0 ||
        reservation.reserved != 0 || (reservation.streamTag == 0 || reservation.streamTag > 15) ||
        reservation.descriptorIndex >= kHdaMaximumTotalStreamCount ||
        reservation.descriptorControlOffset != kHdaStreamDescriptorBase +
            static_cast<uint32_t>(reservation.descriptorIndex) *
                kHdaStreamDescriptorStride ||
        initialControlReadback == UINT32_MAX ||
        initialControlReadback != reservation.previousControl ||
        (initialControlReadback &
            (kHdaStreamRunMask | kHdaStreamResetMask | kHdaStreamTagMask)) != 0 ||
        static_cast<uint64_t>(reservation.descriptorControlOffset) + 4U >
            hdaBarLength)
        return false;
    controlOffset_ = reservation.descriptorControlOffset;
    initialControl_ = initialControlReadback;
    samplesRemaining_ = maximumPollSamples;
    maximumPollSamples_ = maximumPollSamples;
    state_ = State::NeedAssert;
    return true;
}

CodeLoaderResetResult Cavs25CodeLoaderResetSequence::next(
    uint32_t controlReadback, HdaStreamRegisterWrite *write) noexcept {
    if (!write) return CodeLoaderResetResult::InvalidArgument;
    if (state_ == State::Uninitialized || state_ == State::Complete ||
        state_ == State::Failed)
        return CodeLoaderResetResult::InvalidState;
    if (controlReadback == UINT32_MAX) {
        state_ = State::Failed;
        return CodeLoaderResetResult::InvalidReadback;
    }
    if ((controlReadback & (kHdaStreamRunMask | kHdaStreamTagMask)) != 0) {
        state_ = State::Failed;
        return CodeLoaderResetResult::UnexpectedReadback;
    }
    if (state_ == State::NeedAssert) {
        if (controlReadback != initialControl_ ||
            (controlReadback & kHdaStreamResetMask) != 0) {
            state_ = State::Failed;
            return CodeLoaderResetResult::UnexpectedReadback;
        }
        write->offset = controlOffset_;
        write->value = controlReadback | kHdaStreamResetMask;
        write->widthBytes = 4;
        write->reserved[0] = write->reserved[1] = write->reserved[2] = 0;
        samplesRemaining_ = maximumPollSamples_;
        state_ = State::AwaitAssert;
        return CodeLoaderResetResult::WriteRequired;
    }
    if (state_ == State::AwaitAssert) {
        if ((controlReadback & kHdaStreamResetMask) != 0) {
            write->offset = controlOffset_;
            write->value = controlReadback & ~kHdaStreamResetMask;
            write->widthBytes = 4;
            write->reserved[0] = write->reserved[1] = write->reserved[2] = 0;
            samplesRemaining_ = maximumPollSamples_;
            state_ = State::AwaitDeassert;
            return CodeLoaderResetResult::WriteRequired;
        }
        if (--samplesRemaining_ == 0) {
            state_ = State::Failed;
            return CodeLoaderResetResult::TimedOut;
        }
        return CodeLoaderResetResult::Waiting;
    }
    if (state_ == State::AwaitDeassert) {
        if ((controlReadback & kHdaStreamResetMask) == 0) {
            state_ = State::Complete;
            return CodeLoaderResetResult::Complete;
        }
        if (--samplesRemaining_ == 0) {
            state_ = State::Failed;
            return CodeLoaderResetResult::TimedOut;
        }
        return CodeLoaderResetResult::Waiting;
    }
    state_ = State::Failed;
    return CodeLoaderResetResult::InvalidState;
}

bool Cavs25CodeLoaderResetSequence::complete() const noexcept {
    return state_ == State::Complete;
}

CodeLoaderResetResult Cavs25CodeLoaderResetSequence::commandFailed() noexcept {
    if (state_ != State::AwaitAssert && state_ != State::AwaitDeassert)
        return CodeLoaderResetResult::InvalidState;
    state_ = State::Failed;
    return CodeLoaderResetResult::CommandFailed;
}

Cavs25CodeLoaderDmaStopSequence::Cavs25CodeLoaderDmaStopSequence() noexcept
    : state_(State::Uninitialized), controlOffset_(0), expectedMask_(0),
      expectedValue_(0), samplesRemaining_(0), maximumPollSamples_(0) {}

bool Cavs25CodeLoaderDmaStopSequence::initialize(
    const Cavs25CodeLoaderSetupSequence &setupSequence,
    uint32_t initialControlReadback, uint32_t maximumPollSamples,
    bool controllerExclusivelyOwned,
    bool firmwareEnteredCheckpoint) noexcept {
    constexpr uint32_t runMask = 1U << 1;
    constexpr uint32_t resetMask = 1U;
    constexpr uint32_t tagMask = 0x0fU << 20;
    if (state_ != State::Uninitialized || !controllerExclusivelyOwned ||
        !firmwareEnteredCheckpoint || !setupSequence.complete() ||
        maximumPollSamples == 0 || initialControlReadback == UINT32_MAX)
        return false;
    const uint32_t expectedMask = setupSequence.streamControlReadbackMask();
    const uint32_t expectedValue = setupSequence.streamControlReadbackValue();
    const uint32_t stableMask = expectedMask & ~runMask;
    if ((expectedMask & (runMask | resetMask | tagMask)) !=
            (runMask | resetMask | tagMask) ||
        (expectedValue & (runMask | resetMask)) != 0 ||
        (expectedValue & tagMask) == 0 ||
        (initialControlReadback & stableMask) !=
            (expectedValue & stableMask) ||
        (initialControlReadback & resetMask) != 0)
        return false;
    controlOffset_ = setupSequence.streamControlOffset();
    expectedMask_ = expectedMask;
    expectedValue_ = expectedValue;
    samplesRemaining_ = maximumPollSamples;
    maximumPollSamples_ = maximumPollSamples;
    state_ = State::NeedStop;
    return true;
}

CodeLoaderDmaStopResult Cavs25CodeLoaderDmaStopSequence::next(
    uint32_t controlReadback, HdaStreamRegisterWrite *write) noexcept {
    constexpr uint32_t runMask = 1U << 1;
    constexpr uint32_t resetMask = 1U;
    if (!write) return CodeLoaderDmaStopResult::InvalidArgument;
    if (state_ == State::Uninitialized || state_ == State::Complete ||
        state_ == State::Failed)
        return CodeLoaderDmaStopResult::InvalidState;
    if (controlReadback == UINT32_MAX) {
        state_ = State::Failed;
        return CodeLoaderDmaStopResult::InvalidReadback;
    }
    const uint32_t stableMask = expectedMask_ & ~runMask;
    if ((controlReadback & stableMask) != (expectedValue_ & stableMask) ||
        (controlReadback & resetMask) != 0) {
        state_ = State::Failed;
        return CodeLoaderDmaStopResult::UnexpectedReadback;
    }
    if (state_ == State::NeedStop) {
        if ((controlReadback & runMask) == 0) {
            state_ = State::Complete;
            return CodeLoaderDmaStopResult::Stopped;
        }
        write->offset = controlOffset_;
        write->value = controlReadback & ~runMask;
        write->widthBytes = 4;
        write->reserved[0] = write->reserved[1] = write->reserved[2] = 0;
        samplesRemaining_ = maximumPollSamples_;
        state_ = State::AwaitStop;
        return CodeLoaderDmaStopResult::WriteRequired;
    }
    if ((controlReadback & runMask) == 0) {
        state_ = State::Complete;
        return CodeLoaderDmaStopResult::Stopped;
    }
    if (--samplesRemaining_ == 0) {
        state_ = State::Failed;
        return CodeLoaderDmaStopResult::TimedOut;
    }
    return CodeLoaderDmaStopResult::Waiting;
}

CodeLoaderDmaStopResult
Cavs25CodeLoaderDmaStopSequence::commandFailed() noexcept {
    if (state_ != State::AwaitStop) return CodeLoaderDmaStopResult::InvalidState;
    state_ = State::Failed;
    return CodeLoaderDmaStopResult::CommandFailed;
}

bool Cavs25CodeLoaderDmaStopSequence::complete() const noexcept {
    return state_ == State::Complete;
}

bool Cavs25CodeLoaderDmaStopSequence::confirmsStoppedControlReadback(
    uint32_t controlReadback) const noexcept {
    constexpr uint32_t runMask = 1U << 1;
    constexpr uint32_t resetMask = 1U;
    if (state_ != State::Complete || controlReadback == UINT32_MAX ||
        (controlReadback & (runMask | resetMask)) != 0)
        return false;
    const uint32_t stableMask = expectedMask_ & ~(runMask | resetMask);
    return (controlReadback & stableMask) == (expectedValue_ & stableMask);
}

Cavs25CodeLoaderPostDmaResetSequence::Cavs25CodeLoaderPostDmaResetSequence() noexcept
    : state_(State::Uninitialized), controlOffset_(0), expectedMask_(0),
      expectedValue_(0), samplesRemaining_(0), maximumPollSamples_(0) {}

bool Cavs25CodeLoaderPostDmaResetSequence::initialize(
    const Cavs25CodeLoaderSetupSequence &setupSequence,
    const Cavs25CodeLoaderDmaStopSequence &dmaStopSequence,
    uint32_t initialControlReadback, uint32_t maximumPollSamples,
    bool controllerExclusivelyOwned) noexcept {
    constexpr uint32_t runMask = 1U << 1;
    constexpr uint32_t resetMask = 1U;
    constexpr uint32_t tagMask = 0x0fU << 20;
    if (state_ != State::Uninitialized || !controllerExclusivelyOwned ||
        !dmaStopSequence.complete() || !setupSequence.complete() ||
        maximumPollSamples == 0 || initialControlReadback == UINT32_MAX)
        return false;
    const uint32_t expectedMask = setupSequence.streamControlReadbackMask();
    const uint32_t expectedValue = setupSequence.streamControlReadbackValue();
    const uint32_t invariantMask = expectedMask & ~resetMask;
    if ((expectedMask & (runMask | resetMask | tagMask)) !=
            (runMask | resetMask | tagMask) ||
        (expectedValue & (runMask | resetMask)) != 0 ||
        (expectedValue & tagMask) == 0 ||
        (initialControlReadback & invariantMask) !=
            (expectedValue & invariantMask) ||
        (initialControlReadback & (runMask | resetMask)) != 0)
        return false;
    controlOffset_ = setupSequence.streamControlOffset();
    expectedMask_ = expectedMask;
    expectedValue_ = expectedValue;
    samplesRemaining_ = maximumPollSamples;
    maximumPollSamples_ = maximumPollSamples;
    state_ = State::NeedAssert;
    return true;
}

CodeLoaderResetResult Cavs25CodeLoaderPostDmaResetSequence::next(
    uint32_t controlReadback, HdaStreamRegisterWrite *write) noexcept {
    constexpr uint32_t resetMask = 1U;
    if (!write) return CodeLoaderResetResult::InvalidArgument;
    if (state_ == State::Uninitialized || state_ == State::Complete ||
        state_ == State::Failed)
        return CodeLoaderResetResult::InvalidState;
    if (controlReadback == UINT32_MAX) {
        state_ = State::Failed;
        return CodeLoaderResetResult::InvalidReadback;
    }
    const uint32_t invariantMask = (state_ == State::NeedAssert) ? (expectedMask_ & ~resetMask) : (expectedMask_ & (1U << 1));
    if ((controlReadback & invariantMask) != (expectedValue_ & invariantMask)) {
        state_ = State::Failed;
        return CodeLoaderResetResult::UnexpectedReadback;
    }
    if (state_ == State::NeedAssert) {
        if ((controlReadback & resetMask) != 0) {
            state_ = State::Failed;
            return CodeLoaderResetResult::UnexpectedReadback;
        }
        write->offset = controlOffset_;
        write->value = controlReadback | resetMask;
        write->widthBytes = 4;
        write->reserved[0] = write->reserved[1] = write->reserved[2] = 0;
        samplesRemaining_ = maximumPollSamples_;
        state_ = State::AwaitAssert;
        return CodeLoaderResetResult::WriteRequired;
    }
    if (state_ == State::AwaitAssert) {
        if ((controlReadback & resetMask) != 0) {
            write->offset = controlOffset_;
            write->value = controlReadback & ~resetMask;
            write->widthBytes = 4;
            write->reserved[0] = write->reserved[1] = write->reserved[2] = 0;
            samplesRemaining_ = maximumPollSamples_;
            state_ = State::AwaitDeassert;
            return CodeLoaderResetResult::WriteRequired;
        }
    } else if (state_ == State::AwaitDeassert) {
        if ((controlReadback & resetMask) == 0) {
            state_ = State::Complete;
            return CodeLoaderResetResult::Complete;
        }
    } else {
        state_ = State::Failed;
        return CodeLoaderResetResult::InvalidState;
    }
    if (--samplesRemaining_ == 0) {
        state_ = State::Failed;
        return CodeLoaderResetResult::TimedOut;
    }
    return CodeLoaderResetResult::Waiting;
}

CodeLoaderResetResult
Cavs25CodeLoaderPostDmaResetSequence::commandFailed() noexcept {
    if (state_ != State::AwaitAssert && state_ != State::AwaitDeassert)
        return CodeLoaderResetResult::InvalidState;
    state_ = State::Failed;
    return CodeLoaderResetResult::CommandFailed;
}

bool Cavs25CodeLoaderPostDmaResetSequence::complete() const noexcept {
    return state_ == State::Complete;
}

bool Cavs25CodeLoaderPostDmaResetSequence::confirmsResetControlReadback(
    uint32_t controlReadback) const noexcept {
    constexpr uint32_t runMask = 1U << 1;
    constexpr uint32_t resetMask = 1U;
    if (state_ != State::Complete || controlReadback == UINT32_MAX ||
        (controlReadback & (runMask | resetMask)) != 0)
        return false;
    return true;
}

CodeLoaderSetupPlanResult planCavs25CodeLoaderSetup(
    const Cavs25CodeLoaderStreamReservationPlan &reservation,
    const Cavs25DmaPlan &dmaPlan,
    const Cavs25CodeLoaderRegisterSnapshot &snapshot,
    uint64_t hdaBarLength, bool controllerExclusivelyOwned,
    bool audioOwnerQuiesced, Cavs25CodeLoaderSetupPlan *out) noexcept {
    if (!out) return CodeLoaderSetupPlanResult::InvalidArgument;
    *out = {};
    if (!controllerExclusivelyOwned || !audioOwnerQuiesced)
        return CodeLoaderSetupPlanResult::OwnershipNotProven;
    if (!snapshot.controllerReady)
        return CodeLoaderSetupPlanResult::ControllerNotReady;
    if (!cavs25CodeLoaderStreamReservationMatches(
            reservation, snapshot.reservedStreamControlReadback) ||
        (reservation.streamTag == 0 || reservation.streamTag > 15) || reservation.descriptorIndex >=
            kHdaMaximumTotalStreamCount ||
        reservation.previousControl != snapshot.streamRegisters[5])
        return CodeLoaderSetupPlanResult::InvalidReservation;
    if (!validDmaPlan(dmaPlan))
        return CodeLoaderSetupPlanResult::InvalidDmaPlan;
    if (snapshot.ppCapabilityHeader == UINT32_MAX ||
        ((snapshot.ppCapabilityHeader & kPpCapabilityIdMask) >> 16) !=
            kPpCapabilityId || snapshot.ppCapabilityOffset < 0x40 ||
        (snapshot.ppCapabilityOffset & 3U) != 0 ||
        snapshot.ppctl == UINT32_MAX || snapshot.intctl == UINT32_MAX)
        return CodeLoaderSetupPlanResult::InvalidCapability;
    for (uint8_t i = 0; i < 6; ++i) {
        if (snapshot.streamRegisters[i] == UINT32_MAX)
            return CodeLoaderSetupPlanResult::InvalidRegisterSnapshot;
    }
    if (snapshot.streamStatus != 0)
        return CodeLoaderSetupPlanResult::InvalidRegisterSnapshot;

    const uint32_t streamIndex = reservation.descriptorIndex;
    if (streamIndex >= kHdaMaximumTotalStreamCount || streamIndex >= 32)
        return CodeLoaderSetupPlanResult::InvalidReservation;
    const uint32_t streamBit = 1U << streamIndex;
    const uint32_t ctlOffset = reservation.descriptorControlOffset;
    if (ctlOffset != kHdaStreamDescriptorBase +
            streamIndex * kHdaStreamDescriptorStride)
        return CodeLoaderSetupPlanResult::InvalidRegisterSnapshot;

    uint32_t count = 8;
    if (snapshot.ppCapabilityOffset > UINT32_MAX - kPpCtlOffset)
        return CodeLoaderSetupPlanResult::InsufficientBar;
    const uint32_t ppctlOffset = snapshot.ppCapabilityOffset + kPpCtlOffset;
    uint32_t spbfcctlOffset = 0;
    uint32_t spibPositionOffset = 0;
    if (snapshot.spibPresent) {
        if (snapshot.spibCapabilityOffset < 0x40 ||
            (snapshot.spibCapabilityOffset & 3U) != 0 ||
            snapshot.spibCapabilityHeader == UINT32_MAX ||
            ((snapshot.spibCapabilityHeader & kPpCapabilityIdMask) >> 16) !=
                kSpibCapabilityId ||
            snapshot.spbfcctl == UINT32_MAX ||
            snapshot.spibPosition == UINT32_MAX)
            return CodeLoaderSetupPlanResult::InvalidCapability;
        const uint64_t spbfcctlOffset64 =
            static_cast<uint64_t>(snapshot.spibCapabilityOffset) +
            kSpbFcCtlOffset;
        const uint64_t positionOffset =
            static_cast<uint64_t>(snapshot.spibCapabilityOffset) +
            kSpibPositionsOffset +
            static_cast<uint64_t>(streamIndex) * kSpibStreamStride;
        if (spbfcctlOffset64 > UINT32_MAX || positionOffset > UINT32_MAX)
            return CodeLoaderSetupPlanResult::InsufficientBar;
        spbfcctlOffset = static_cast<uint32_t>(spbfcctlOffset64);
        spibPositionOffset = static_cast<uint32_t>(positionOffset);
        count = 10;
    }
    if (!writeFits(ppctlOffset, 4, hdaBarLength) ||
        !writeFits(kIntctlOffset, 4, hdaBarLength) ||
        !writeFits(ctlOffset + kStreamBdlLowOffset, 4, hdaBarLength) ||
        !writeFits(ctlOffset + kStreamBdlHighOffset, 4, hdaBarLength) ||
        !writeFits(ctlOffset + kStreamCblOffset, 4, hdaBarLength) ||
        !writeFits(ctlOffset + kStreamLviOffset, 2, hdaBarLength) ||
        !writeFits(ctlOffset + kStreamFormatOffset, 2, hdaBarLength) ||
        !writeFits(ctlOffset, 4, hdaBarLength) ||
        (snapshot.spibPresent &&
         (!writeFits(spbfcctlOffset, 4, hdaBarLength) ||
          !writeFits(spibPositionOffset, 4, hdaBarLength))))
        return CodeLoaderSetupPlanResult::InsufficientBar;

    const uint32_t ppctlValue = snapshot.ppctl | streamBit |
        kPpGlobalProcessingEnable | kPpInterruptEnable;
    const uint32_t intctlValue = snapshot.intctl | streamBit |
        kIntGlobalEnable | kIntControllerEnable;
    const uint32_t controlValue =
        (snapshot.reservedStreamControlReadback &
            ~kStreamControlWritableMask) |
        (static_cast<uint32_t>(reservation.streamTag) << 20) | kStreamInterruptMask;
    const uint32_t offsets[8] = {
        ppctlOffset,
        ctlOffset + kStreamBdlLowOffset,
        ctlOffset + kStreamBdlHighOffset,
        ctlOffset + kStreamCblOffset,
        ctlOffset + kStreamLviOffset,
        ctlOffset + kStreamFormatOffset,
        ctlOffset,
        kIntctlOffset,
    };
    const uint32_t values[8] = {
        ppctlValue,
        dmaPlan.descriptorBaseLow,
        dmaPlan.descriptorBaseHigh,
        dmaPlan.cyclicBufferLength,
        dmaPlan.lastValidIndex,
        kRequiredFormat,
        controlValue,
        intctlValue,
    };
    const uint32_t previous[8] = {
        snapshot.ppctl,
        snapshot.streamRegisters[0],
        snapshot.streamRegisters[1],
        snapshot.streamRegisters[2],
        snapshot.streamRegisters[3],
        snapshot.streamRegisters[4],
        reservation.previousControl,
        snapshot.intctl,
    };
    const uint8_t widths[8] = {4, 4, 4, 4, 2, 2, 4, 4};
    const uint32_t masks[8] = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        0xffU, 0xffffU, kStreamControlWritableMask, UINT32_MAX,
    };
    for (uint8_t i = 0; i < 8; ++i) {
        setWrite(out, i, offsets[i], values[i], widths[i], previous[i],
                 masks[i], values[i] & masks[i]);
    }
    if (snapshot.spibPresent) {
        const uint32_t spibEnableValue = snapshot.spbfcctl | streamBit;
        setWrite(out, 8, spbfcctlOffset, spibEnableValue, 4,
                 snapshot.spbfcctl, streamBit, streamBit);
        setWrite(out, 9, spibPositionOffset, dmaPlan.cyclicBufferLength, 4,
                 snapshot.spibPosition, UINT32_MAX,
                 dmaPlan.cyclicBufferLength);
        out->spibWriteIndex = 8;
    }
    out->streamControlOffset = ctlOffset;
    out->ppCapabilityOffset = snapshot.ppCapabilityOffset;
    out->ppCapabilityHeader = snapshot.ppCapabilityHeader;
    out->streamIndex = reservation.descriptorIndex;
    out->writeCount = static_cast<uint8_t>(count);
    out->reserved = 0;
    out->spibPresent = snapshot.spibPresent;
    if (snapshot.spibPresent) {
        out->spibCapabilityOffset = snapshot.spibCapabilityOffset;
        out->spibCapabilityHeader = snapshot.spibCapabilityHeader;
    }
    return CodeLoaderSetupPlanResult::Ok;
}

CodeLoaderSetupPlanResult planCavs25CodeLoaderRollback(
    const Cavs25CodeLoaderSetupPlan &setupPlan,
    uint32_t freshPpctlReadback, uint32_t freshIntctlReadback,
    uint32_t freshStreamControlReadback, uint32_t freshSpbfcctlReadback,
    uint64_t hdaBarLength, bool controllerExclusivelyOwned,
    bool dmaStoppedAndDrained, bool streamResetConfirmed,
    Cavs25CodeLoaderRollbackPlan *out) noexcept {
    if (!out) return CodeLoaderSetupPlanResult::InvalidArgument;
    *out = {};
    for (uint8_t i = 0; i < 12; ++i)
        out->expectedBeforeValues[i] = UINT32_MAX;
    if (!controllerExclusivelyOwned)
        return CodeLoaderSetupPlanResult::OwnershipNotProven;
    if (!dmaStoppedAndDrained || !streamResetConfirmed)
        return CodeLoaderSetupPlanResult::InvalidRegisterSnapshot;
    if (!validSetupPlan(setupPlan, hdaBarLength) ||
        freshPpctlReadback == UINT32_MAX ||
        freshIntctlReadback == UINT32_MAX ||
        freshStreamControlReadback == UINT32_MAX ||
        (freshStreamControlReadback &
            (kHdaStreamRunMask | kHdaStreamResetMask)) != 0 ||
        (setupPlan.spibPresent && freshSpbfcctlReadback == UINT32_MAX))
        return CodeLoaderSetupPlanResult::InvalidRegisterSnapshot;

    const uint32_t streamBit = 1U << setupPlan.streamIndex;
    const uint32_t intChanged = setupPlan.previousValues[7] ^
        setupPlan.writes[7].value;
    const uint32_t ppChanged = setupPlan.previousValues[0] ^
        setupPlan.writes[0].value;
    if ((intChanged & ~(streamBit | kIntGlobalEnable |
                        kIntControllerEnable)) != 0 ||
        (ppChanged & ~(streamBit | kPpGlobalProcessingEnable |
                       kPpInterruptEnable)) != 0)
        return CodeLoaderSetupPlanResult::InvalidRegisterSnapshot;

    const uint32_t intValue =
        (freshIntctlReadback & ~intChanged) |
        (setupPlan.previousValues[7] & intChanged);
    const uint32_t ppValue = (freshPpctlReadback & ~ppChanged) |
        (setupPlan.previousValues[0] & ppChanged);
    out->writes[0] = {kIntctlOffset, intValue, 4, {0, 0, 0}};
    out->expectedBeforeValues[0] = freshIntctlReadback;
    out->readbackMasks[0] = intChanged;
    out->readbackValues[0] = setupPlan.previousValues[7] & intChanged;
    out->writes[1] = {setupPlan.writes[0].offset, ppValue, 4, {0, 0, 0}};
    out->expectedBeforeValues[1] = freshPpctlReadback;
    out->readbackMasks[1] = ppChanged;
    out->readbackValues[1] = setupPlan.previousValues[0] & ppChanged;

    uint8_t cursor = 2;
    if (setupPlan.spibPresent) {
        const uint32_t spibChanged = setupPlan.previousValues[8] ^
            setupPlan.writes[8].value;
        if (spibChanged != streamBit)
            return CodeLoaderSetupPlanResult::InvalidRegisterSnapshot;
        out->writes[cursor] = setupPlan.writes[8];
        out->writes[cursor].value = (freshSpbfcctlReadback & ~streamBit) |
            (setupPlan.previousValues[8] & streamBit);
        out->expectedBeforeValues[cursor] = freshSpbfcctlReadback;
        out->readbackMasks[cursor] = streamBit;
        out->readbackValues[cursor] = setupPlan.previousValues[8] & streamBit;
        ++cursor;
        out->writes[cursor] = setupPlan.writes[9];
        out->writes[cursor].value = setupPlan.previousValues[9];
        out->readbackMasks[cursor] = UINT32_MAX;
        out->readbackValues[cursor] = setupPlan.previousValues[9];
        ++cursor;
    }

    const uint8_t widths[6] = {4, 4, 4, 2, 2, 4};
    const uint32_t masks[6] = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX, 0xffU, 0xffffU,
        kStreamControlWritableMask,
    };
    for (uint8_t i = 0; i < 6; ++i) {
        const uint8_t setupIndex = static_cast<uint8_t>(i + 1);
        const uint32_t value = setupPlan.previousValues[setupIndex] & masks[i];
        out->writes[cursor] = setupPlan.writes[setupIndex];
        out->writes[cursor].value = value;
        out->writes[cursor].widthBytes = widths[i];
        out->readbackMasks[cursor] = masks[i];
        out->readbackValues[cursor] = value;
        ++cursor;
    }
    out->streamControlOffset = setupPlan.streamControlOffset;
    out->ppCapabilityOffset = setupPlan.ppCapabilityOffset;
    out->ppCapabilityHeader = setupPlan.ppCapabilityHeader;
    out->spibCapabilityOffset = setupPlan.spibCapabilityOffset;
    out->spibCapabilityHeader = setupPlan.spibCapabilityHeader;
    out->streamIndex = setupPlan.streamIndex;
    out->writeCount = cursor;
    out->spibPresent = setupPlan.spibPresent;
    return CodeLoaderSetupPlanResult::Ok;
}

Cavs25CodeLoaderRollbackSequence::Cavs25CodeLoaderRollbackSequence() noexcept
    : plan_{}, cursor_(0), initialized_(false),
      awaitingConfirmation_(false), failed_(false) {}

CodeLoaderSetupSequenceResult Cavs25CodeLoaderRollbackSequence::initialize(
    const Cavs25CodeLoaderRollbackPlan &plan,
    uint64_t hdaBarLength) noexcept {
    if (initialized_) return CodeLoaderSetupSequenceResult::InvalidState;
    const uint8_t expectedCount = plan.spibPresent ? 10 : 8;
    if (plan.writeCount != expectedCount || plan.streamIndex >= 32 ||
        plan.streamControlOffset != kHdaStreamDescriptorBase +
            static_cast<uint32_t>(plan.streamIndex) *
                kHdaStreamDescriptorStride || plan.reserved[0] != 0 ||
        plan.reserved[1] != 0 || plan.writes[0].offset != kIntctlOffset ||
        plan.ppCapabilityOffset < 0x40 ||
        (plan.ppCapabilityOffset & 3U) != 0 ||
        ((plan.ppCapabilityHeader & kPpCapabilityIdMask) >> 16) !=
            kPpCapabilityId ||
        plan.writes[1].offset != plan.ppCapabilityOffset + kPpCtlOffset ||
        plan.writes[1].widthBytes != 4 ||
        (plan.spibPresent &&
         (plan.spibCapabilityOffset < 0x40 ||
          (plan.spibCapabilityOffset & 3U) != 0 ||
          ((plan.spibCapabilityHeader & kPpCapabilityIdMask) >> 16) !=
              kSpibCapabilityId ||
          plan.writes[2].offset != plan.spibCapabilityOffset +
              kSpbFcCtlOffset ||
          plan.writes[3].offset != plan.spibCapabilityOffset +
              kSpibPositionsOffset +
              static_cast<uint32_t>(plan.streamIndex) * kSpibStreamStride)) ||
        (!plan.spibPresent && (plan.spibCapabilityOffset != 0 ||
                               plan.spibCapabilityHeader != 0)))
        return CodeLoaderSetupSequenceResult::InvalidPlan;
    for (uint8_t i = 0; i < plan.writeCount; ++i) {
        const HdaStreamRegisterWrite &write = plan.writes[i];
        if (!writeFits(write.offset, write.widthBytes, hdaBarLength) ||
            write.reserved[0] != 0 || write.reserved[1] != 0 ||
            write.reserved[2] != 0 || write.value == UINT32_MAX ||
            (plan.readbackValues[i] & ~plan.readbackMasks[i]) != 0 ||
            (write.value & plan.readbackMasks[i]) !=
                plan.readbackValues[i])
            return CodeLoaderSetupSequenceResult::InvalidPlan;
    }
    const uint8_t streamStart = plan.spibPresent ? 4 : 2;
    const uint32_t streamOffsets[6] = {
        plan.streamControlOffset + kStreamBdlLowOffset,
        plan.streamControlOffset + kStreamBdlHighOffset,
        plan.streamControlOffset + kStreamCblOffset,
        plan.streamControlOffset + kStreamLviOffset,
        plan.streamControlOffset + kStreamFormatOffset,
        plan.streamControlOffset,
    };
    const uint8_t streamWidths[6] = {4, 4, 4, 2, 2, 4};
    for (uint8_t i = 0; i < 6; ++i) {
        const HdaStreamRegisterWrite &write =
            plan.writes[static_cast<uint8_t>(streamStart + i)];
        if (write.offset != streamOffsets[i] ||
            write.widthBytes != streamWidths[i])
            return CodeLoaderSetupSequenceResult::InvalidPlan;
    }
    const HdaStreamRegisterWrite &control = plan.writes[plan.writeCount - 1];
    if (control.offset != plan.streamControlOffset ||
        (control.value & (kHdaStreamRunMask | kHdaStreamResetMask)) != 0)
        return CodeLoaderSetupSequenceResult::InvalidPlan;
    plan_ = plan;
    cursor_ = 0;
    initialized_ = true;
    awaitingConfirmation_ = false;
    failed_ = false;
    return CodeLoaderSetupSequenceResult::WriteRequired;
}

CodeLoaderSetupSequenceResult Cavs25CodeLoaderRollbackSequence::next(
    HdaStreamRegisterWrite *write) noexcept {
    if (!write) return CodeLoaderSetupSequenceResult::InvalidArgument;
    if (!initialized_ || failed_ || awaitingConfirmation_)
        return CodeLoaderSetupSequenceResult::InvalidState;
    if (cursor_ >= plan_.writeCount) return CodeLoaderSetupSequenceResult::Complete;
    *write = plan_.writes[cursor_];
    awaitingConfirmation_ = true;
    return CodeLoaderSetupSequenceResult::WriteRequired;
}

CodeLoaderSetupSequenceResult Cavs25CodeLoaderRollbackSequence::confirm(
    uint32_t readback) noexcept {
    if (!initialized_ || failed_ || !awaitingConfirmation_)
        return CodeLoaderSetupSequenceResult::InvalidState;
    if (readback == UINT32_MAX) {
        failed_ = true;
        awaitingConfirmation_ = false;
        return CodeLoaderSetupSequenceResult::InvalidReadback;
    }
    if ((readback & plan_.readbackMasks[cursor_]) !=
        plan_.readbackValues[cursor_]) {
        failed_ = true;
        awaitingConfirmation_ = false;
        return CodeLoaderSetupSequenceResult::ReadbackMismatch;
    }
    ++cursor_;
    awaitingConfirmation_ = false;
    return cursor_ == plan_.writeCount
        ? CodeLoaderSetupSequenceResult::Complete
        : CodeLoaderSetupSequenceResult::Matched;
}

CodeLoaderSetupSequenceResult
Cavs25CodeLoaderRollbackSequence::commandFailed() noexcept {
    if (!initialized_ || failed_ || !awaitingConfirmation_)
        return CodeLoaderSetupSequenceResult::InvalidState;
    failed_ = true;
    awaitingConfirmation_ = false;
    return CodeLoaderSetupSequenceResult::CommandFailed;
}

bool Cavs25CodeLoaderRollbackSequence::complete() const noexcept {
    return initialized_ && !failed_ && !awaitingConfirmation_ &&
        cursor_ == plan_.writeCount;
}

Cavs25CodeLoaderSetupSequence::Cavs25CodeLoaderSetupSequence() noexcept
    : plan_{}, cursor_(0), initialized_(false),
      awaitingConfirmation_(false), failed_(false) {}

CodeLoaderSetupSequenceResult Cavs25CodeLoaderSetupSequence::initialize(
    const Cavs25CodeLoaderSetupPlan &plan, uint64_t hdaBarLength) noexcept {
    if (initialized_) return CodeLoaderSetupSequenceResult::InvalidState;
    if (!validSetupPlan(plan, hdaBarLength))
        return CodeLoaderSetupSequenceResult::InvalidPlan;
    plan_ = plan;
    cursor_ = 0;
    awaitingConfirmation_ = false;
    failed_ = false;
    initialized_ = true;
    return CodeLoaderSetupSequenceResult::WriteRequired;
}

CodeLoaderSetupSequenceResult Cavs25CodeLoaderSetupSequence::next(
    HdaStreamRegisterWrite *write) noexcept {
    if (!write) return CodeLoaderSetupSequenceResult::InvalidArgument;
    if (!initialized_ || failed_ || awaitingConfirmation_)
        return CodeLoaderSetupSequenceResult::InvalidState;
    if (cursor_ >= plan_.writeCount)
        return CodeLoaderSetupSequenceResult::Complete;
    *write = plan_.writes[cursor_];
    awaitingConfirmation_ = true;
    return CodeLoaderSetupSequenceResult::WriteRequired;
}

CodeLoaderSetupSequenceResult Cavs25CodeLoaderSetupSequence::confirm(
    uint32_t readback) noexcept {
    if (!initialized_ || failed_ || !awaitingConfirmation_)
        return CodeLoaderSetupSequenceResult::InvalidState;
    if (readback == UINT32_MAX) {
        failed_ = true;
        awaitingConfirmation_ = false;
        return CodeLoaderSetupSequenceResult::InvalidReadback;
    }
    if ((readback & plan_.readbackMasks[cursor_]) !=
        plan_.readbackValues[cursor_]) {
        failed_ = true;
        awaitingConfirmation_ = false;
        return CodeLoaderSetupSequenceResult::ReadbackMismatch;
    }
    ++cursor_;
    awaitingConfirmation_ = false;
    return cursor_ == plan_.writeCount
        ? CodeLoaderSetupSequenceResult::Complete
        : CodeLoaderSetupSequenceResult::Matched;
}

CodeLoaderSetupSequenceResult Cavs25CodeLoaderSetupSequence::commandFailed()
    noexcept {
    if (!initialized_ || failed_ || !awaitingConfirmation_)
        return CodeLoaderSetupSequenceResult::InvalidState;
    failed_ = true;
    awaitingConfirmation_ = false;
    return CodeLoaderSetupSequenceResult::CommandFailed;
}

bool Cavs25CodeLoaderSetupSequence::complete() const noexcept {
    return initialized_ && !failed_ && !awaitingConfirmation_ &&
        cursor_ == plan_.writeCount;
}

} // namespace sof::ipc4::kernel
