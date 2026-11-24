#pragma once

#include <stdint.h>

namespace sof::ipc4::kernel {

enum class HdaInputStreamPlanResult : uint8_t {
    Ok,
    InvalidArgument,
    OwnershipNotProven,
    InvalidCapabilities,
    IncompleteSnapshot,
    InconsistentTagMask,
    NoFreeInputDescriptor,
    NoFreeStreamTag,
    InvalidReservationReadback,
    StreamTagAlreadyAllocated,
    InsufficientBar,
};

constexpr uint32_t kHdaGcapOffset = 0x00;
constexpr uint32_t kHdaStreamDescriptorBase = 0x80;
constexpr uint32_t kHdaStreamDescriptorStride = 0x20;
constexpr uint32_t kHdaStreamControlOffset = 0x00;
constexpr uint32_t kHdaStreamRunMask = 1U << 1;
constexpr uint32_t kHdaStreamResetMask = 1U << 0;
constexpr uint32_t kHdaStreamTagMask = 0x0fU << 20;
constexpr uint8_t kHdaMaximumStreamDescriptors = 15;
constexpr uint8_t kHdaMaximumTotalStreamCount = 30;

struct HdaStreamDescriptorSnapshot {
    uint32_t control;
    uint32_t cyclicBufferLength;
    uint32_t bdlLow;
    uint32_t bdlHigh;
    uint16_t lastValidIndex;
    uint16_t format;
    uint8_t status;
    uint8_t reserved[3];
};

struct HdaInputStreamAllocationPlan {
    uint8_t inputStreamCount;
    uint8_t descriptorIndex;
    uint8_t streamTag;
    uint8_t reserved;
    uint32_t descriptorControlOffset;
    uint16_t allocatedStreamTagMask;
    uint16_t reserved2;
};

struct HdaInputStreamReservationPlan {
    uint32_t descriptorControlOffset;
    uint32_t previousControlReadback;
    uint32_t controlWriteValue;
    uint32_t expectedReadbackMask;
    uint32_t expectedReadbackValue;
};

struct HdaInputStreamRegisterWrite {
    uint32_t offset;
    uint32_t value;
    uint8_t widthBytes;
    uint8_t reserved[3];
};

enum class HdaInputStreamReservationSequenceResult : uint8_t {
    Ready,
    WriteRequired,
    Complete,
    InvalidArgument,
    InvalidState,
    InvalidPlan,
    InvalidReadback,
    ReadbackMismatch,
    CommandFailed,
};

// Readback-gated, one-write SDn.CTL tag reservation. This sequencer proposes
// the MMIO write but has no hardware access; the controller must serialize
// next()/confirm() around the actual register operation.
class HdaInputStreamReservationSequence final {
public:
    HdaInputStreamReservationSequence() noexcept;

    HdaInputStreamReservationSequenceResult initialize(
        const HdaInputStreamReservationPlan &plan,
        uint64_t hdaBarLength) noexcept;
    HdaInputStreamReservationSequenceResult next(
        HdaInputStreamRegisterWrite *write) noexcept;
    HdaInputStreamReservationSequenceResult confirm(
        uint32_t controlReadback) noexcept;
    HdaInputStreamReservationSequenceResult commandFailed() noexcept;
    bool complete() const noexcept { return complete_; }
    bool failed() const noexcept { return failed_; }
    const HdaInputStreamReservationPlan *plan() const noexcept {
        return initialized_ ? &plan_ : nullptr;
    }
    uint32_t confirmedControlReadback() const noexcept {
        return complete_ ? confirmedControlReadback_ : UINT32_MAX;
    }

private:
    HdaInputStreamReservationPlan plan_;
    bool initialized_;
    bool awaitingConfirmation_;
    bool complete_;
    bool failed_;
    uint32_t confirmedControlReadback_;
};

// Uses a complete read-only stream snapshot to suggest one idle, empty input
// descriptor and globally unused nonzero stream tag. Reservation remains a
// separate explicit plan, built from a fresh descriptor readback and tag mask.
HdaInputStreamPlanResult planHdaInputStreamAllocation(
    uint16_t gcap, const HdaStreamDescriptorSnapshot *inputSnapshots,
    uint8_t snapshotCount, uint16_t allocatedStreamTagMask,
    bool controllerExclusivelyOwned,
    HdaInputStreamAllocationPlan *out) noexcept;

// Verifies that a fresh complete descriptor snapshot is idle and empty, and
// that the global stream tag is still free, then describes the tag claim while
// preserving all other SDn.CTL bits.
HdaInputStreamPlanResult planHdaInputStreamReservation(
    const HdaInputStreamAllocationPlan &allocation,
    const HdaStreamDescriptorSnapshot &currentDescriptor,
    uint16_t currentAllocatedStreamTagMask,
    uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    HdaInputStreamReservationPlan *out) noexcept;

bool hdaInputStreamReservationMatches(
    const HdaInputStreamReservationPlan &plan,
    uint32_t controlReadback) noexcept;

} // namespace sof::ipc4::kernel
