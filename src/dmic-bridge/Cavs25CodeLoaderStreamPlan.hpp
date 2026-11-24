#pragma once

#include "HdaInputStreamAllocatorPlan.hpp"
#include "HdaCaptureRegisterPlan.hpp"

namespace sof::ipc4::kernel {

enum class CodeLoaderStreamPlanResult : uint8_t {
    Ok,
    InvalidArgument,
    OwnershipNotProven,
    InvalidCapabilities,
    IncompleteSnapshot,
    InconsistentTagMask,
    NoOutputDescriptor,
    StreamBusy,
    StreamTagUnavailable,
    InsufficientBar,
};

struct Cavs25CodeLoaderStreamReservationPlan {
    uint32_t descriptorControlOffset;
    uint32_t reservationBaseControl;
    uint32_t previousControl;
    uint32_t controlWriteValue;
    uint32_t expectedReadbackMask;
    uint32_t expectedReadbackValue;
    uint8_t descriptorIndex;
    uint8_t streamTag;
    uint16_t reserved;
};

// Plans a caller-selected output descriptor and stream tag. The default
// preserves the standalone controller selection of the first output and tag 1. Inputs must come from one fresh, complete HDA snapshot;
// this function performs no I/O and does not make a reservation itself.
CodeLoaderStreamPlanResult planCavs25CodeLoaderStreamReservation(
    uint16_t globalCapabilities,
    uint8_t inputStreamCount,
    uint8_t outputStreamCount,
    uint8_t totalStreamCount,
    const HdaStreamDescriptorSnapshot *allStreamControls,
    uint8_t snapshotCount,
    uint16_t allocatedStreamTagMask,
    uint32_t previousControlBeforeReset,
    uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    bool audioOwnerQuiesced,
    Cavs25CodeLoaderStreamReservationPlan *out,
    uint8_t outputOrdinal = 0, uint8_t streamTag = 1) noexcept;

bool cavs25CodeLoaderStreamReservationMatches(
    const Cavs25CodeLoaderStreamReservationPlan &plan,
    uint32_t controlReadback) noexcept;

enum class CodeLoaderReservationSequenceResult : uint8_t {
    WriteRequired,
    Complete,
    InvalidArgument,
    InvalidState,
    InvalidPlan,
    InvalidReadback,
    ReadbackMismatch,
    CommandFailed,
};

// One-write, readback-confirmed reservation state machine. It performs no I/O.
class Cavs25CodeLoaderReservationSequence final {
public:
    Cavs25CodeLoaderReservationSequence() noexcept;
    CodeLoaderReservationSequenceResult initialize(
        const Cavs25CodeLoaderStreamReservationPlan &plan,
        uint64_t hdaBarLength) noexcept;
    CodeLoaderReservationSequenceResult next(
        HdaStreamRegisterWrite *write) noexcept;
    CodeLoaderReservationSequenceResult confirm(uint32_t readback) noexcept;
    CodeLoaderReservationSequenceResult commandFailed() noexcept;
    bool complete() const noexcept;
    bool failed() const noexcept { return failed_; }
    uint32_t streamControlOffset() const noexcept {
        return initialized_ ? plan_.descriptorControlOffset : 0;
    }
    uint32_t reservationBaseControl() const noexcept {
        return initialized_ ? plan_.reservationBaseControl : UINT32_MAX;
    }
private:
    Cavs25CodeLoaderStreamReservationPlan plan_;
    bool initialized_;
    bool awaitingConfirmation_;
    bool failed_;
    bool confirmed_;
};

} // namespace sof::ipc4::kernel
