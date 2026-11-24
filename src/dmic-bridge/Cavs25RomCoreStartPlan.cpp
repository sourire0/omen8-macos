#include "Cavs25RomCoreStartPlan.hpp"
#include "Cavs25RomPurgePlan.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kPrimaryCoreReset = 1U << 0;
constexpr uint32_t kPrimaryCoreStall = 1U << 8;
constexpr uint32_t kPrimaryCorePowerRequest = 1U << 16;
constexpr uint32_t kPrimaryCorePowerAck = 1U << 24;
constexpr uint32_t kPrimaryCoreStateMask = kPrimaryCoreReset |
    kPrimaryCoreStall | kPrimaryCorePowerRequest | kPrimaryCorePowerAck;

bool purgeDoorbellMatches(uint32_t expected, uint32_t readback) noexcept {
    if (readback == UINT32_MAX || (expected & kCavs25HipcBusyMask) == 0 ||
        (readback & kCavs25HipcMessageMask) !=
            (expected & kCavs25HipcMessageMask))
        return false;
    for (uint8_t tag = 1; tag <= 15; ++tag) {
        uint32_t canonical = 0;
        if (buildCavs25RomPurgeDoorbell(tag, &canonical) &&
            canonical == expected)
            return true;
    }
    return false;
}

void setStep(Cavs25AdspcsStep *step, uint32_t beforeMask,
             uint32_t beforeValue, uint32_t updateMask,
             uint32_t updateValue, uint32_t afterMask,
             uint32_t afterValue) noexcept {
    step->requiredBeforeMask = beforeMask;
    step->requiredBeforeValue = beforeValue;
    step->updateMask = updateMask;
    step->updateValue = updateValue;
    step->expectedAfterMask = afterMask;
    step->expectedAfterValue = afterValue;
}

} // namespace

RomCoreStartPlanResult planCavs25RomPrimaryCoreStart(
    uint32_t initialAdspcs, bool exclusiveControllerLease,
    uint32_t expectedPurgeDoorbell, uint32_t purgeDoorbellReadback,
    Cavs25RomCoreStartPlan *out) noexcept {
    if (!out) return RomCoreStartPlanResult::InvalidArgument;
    if (!exclusiveControllerLease)
        return RomCoreStartPlanResult::OwnershipNotProven;
    if (!purgeDoorbellMatches(expectedPurgeDoorbell,
                              purgeDoorbellReadback))
        return RomCoreStartPlanResult::PurgeNotConfirmed;
    const uint32_t initialCoreState = initialAdspcs & kPrimaryCoreStateMask;
    const uint32_t resetAndStall = kPrimaryCoreReset | kPrimaryCoreStall;
    const uint32_t poweredResetAndStall = resetAndStall |
        kPrimaryCorePowerRequest | kPrimaryCorePowerAck;
    if (initialAdspcs == UINT32_MAX ||
        (initialCoreState != resetAndStall &&
         initialCoreState != poweredResetAndStall))
        return RomCoreStartPlanResult::InvalidInitialState;

    const uint32_t powerAndAck =
        kPrimaryCorePowerRequest | kPrimaryCorePowerAck;
    const uint32_t allActive = powerAndAck | kPrimaryCoreReset |
        kPrimaryCoreStall;
    setStep(&out->steps[0], kPrimaryCoreStateMask,
            initialCoreState,
            kPrimaryCorePowerRequest, kPrimaryCorePowerRequest,
            kPrimaryCorePowerRequest,
            kPrimaryCorePowerRequest);
    // Do not release reset until the hardware confirms Core 0 power active.
    setStep(&out->steps[1], allActive, allActive,
            kPrimaryCoreReset, kPrimaryCoreReset,
            kPrimaryCoreReset, kPrimaryCoreReset);
    setStep(&out->steps[2], allActive, allActive,
            kPrimaryCoreStall, 0,
            kPrimaryCoreStall, 0);
    setStep(&out->steps[3], allActive,
            powerAndAck | kPrimaryCoreReset,
            kPrimaryCoreReset, 0,
            kPrimaryCoreStateMask, powerAndAck);
    out->stepCount = 4;
    out->reserved[0] = 0;
    out->reserved[1] = 0;
    out->reserved[2] = 0;
    return RomCoreStartPlanResult::Ok;
}

Cavs25RomCoreStartSequence::Cavs25RomCoreStartSequence() noexcept
    : plan_{}, completedSteps_(0), initialized_(false),
      awaitingConfirmation_(false), failed_(false) {}

RomCoreStartPlanResult Cavs25RomCoreStartSequence::initialize(
    uint32_t initialAdspcs, bool exclusiveControllerLease,
    uint32_t expectedPurgeDoorbell,
    uint32_t purgeDoorbellReadback) noexcept {
    if (initialized_) return RomCoreStartPlanResult::InvalidArgument;
    const RomCoreStartPlanResult result = planCavs25RomPrimaryCoreStart(
        initialAdspcs, exclusiveControllerLease, expectedPurgeDoorbell,
        purgeDoorbellReadback, &plan_);
    if (result != RomCoreStartPlanResult::Ok) return result;
    initialized_ = true;
    return RomCoreStartPlanResult::Ok;
}

RomCoreStartSequenceResult Cavs25RomCoreStartSequence::nextUpdate(
    uint32_t adspcsReadback, Cavs25AdspcsStep *update) noexcept {
    if (!update) return RomCoreStartSequenceResult::InvalidArgument;
    if (!initialized_) return RomCoreStartSequenceResult::NotInitialized;
    if (failed_ || awaitingConfirmation_)
        return RomCoreStartSequenceResult::InvalidState;
    if (adspcsReadback == UINT32_MAX) {
        failed_ = true;
        return RomCoreStartSequenceResult::InvalidReadback;
    }
    if (completedSteps_ >= plan_.stepCount)
        return RomCoreStartSequenceResult::Complete;

    const Cavs25AdspcsStep &step = plan_.steps[completedSteps_];
    if ((adspcsReadback & step.requiredBeforeMask) !=
        step.requiredBeforeValue)
        return RomCoreStartSequenceResult::WaitingForHardware;
    *update = step;
    awaitingConfirmation_ = true;
    return RomCoreStartSequenceResult::Ready;
}

RomCoreStartSequenceResult Cavs25RomCoreStartSequence::confirmUpdate(
    uint32_t adspcsReadback) noexcept {
    if (!initialized_) return RomCoreStartSequenceResult::NotInitialized;
    if (failed_ || !awaitingConfirmation_)
        return RomCoreStartSequenceResult::InvalidState;
    if (adspcsReadback == UINT32_MAX) {
        failed_ = true;
        awaitingConfirmation_ = false;
        return RomCoreStartSequenceResult::InvalidReadback;
    }
    const Cavs25AdspcsStep &step = plan_.steps[completedSteps_];
    if ((adspcsReadback & step.expectedAfterMask) !=
        step.expectedAfterValue) {
        failed_ = true;
        awaitingConfirmation_ = false;
        return RomCoreStartSequenceResult::UnexpectedReadback;
    }
    ++completedSteps_;
    awaitingConfirmation_ = false;
    return completedSteps_ == plan_.stepCount
        ? RomCoreStartSequenceResult::Complete
        : RomCoreStartSequenceResult::WaitingForHardware;
}

} // namespace sof::ipc4::kernel
