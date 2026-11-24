#pragma once

#include "HdaCaptureBdlPlan.hpp"
#include "HdaInputStreamAllocatorPlan.hpp"

namespace sof::ipc4::kernel {

enum class HdaCaptureRegisterPlanResult : uint8_t {
    Ok,
    InvalidArgument,
    OwnershipNotProven,
    StreamNotReserved,
    InvalidStreamReadback,
    InvalidBdlPlan,
    InvalidFormat,
    InvalidCapability,
    InsufficientBar,
};

struct HdaStreamRegisterWrite {
    uint32_t offset;
    uint32_t value;
    uint8_t widthBytes;
    uint8_t reserved[3];
};

struct HdaCaptureRegisterPlan {
    HdaStreamRegisterWrite writes[6];
    uint32_t previousValues[6];
    uint32_t streamControlOffset;
    uint8_t writeCount;
    uint8_t reserved[3];
};

struct HdaCaptureRoutingPlan {
    HdaStreamRegisterWrite writes[2];
    uint32_t previousValues[2];
    uint8_t writeCount;
    uint8_t reserved[3];
};

struct HdaCaptureSetupPlan {
    HdaStreamRegisterWrite writes[8];
    uint32_t previousValues[8];
    uint32_t readbackMasks[8];
    uint32_t readbackValues[8];
    uint32_t streamControlOffset;
    uint8_t writeCount;
    uint8_t reserved[3];
};

struct HdaCaptureRollbackPlan {
    HdaStreamRegisterWrite writes[8];
    uint32_t preconditionValues[8];
    uint32_t readbackMasks[8];
    uint32_t readbackValues[8];
    uint8_t writeCount;
    uint8_t reserved[3];
};

enum class HdaCaptureSetupSequenceResult : uint8_t {
    WriteRequired,
    Matched,
    Complete,
    InvalidArgument,
    InvalidState,
    InvalidPlan,
    InvalidReadback,
    ReadbackMismatch,
    CommandFailed,
};

// Steps through the combined plan and requires a matching readback after each
// requested write before it exposes the next item. It has no MMIO access.
class HdaCaptureSetupSequence final {
public:
    HdaCaptureSetupSequence() noexcept;

    HdaCaptureSetupSequenceResult initialize(
        const HdaCaptureSetupPlan &plan,
        uint64_t hdaBarLength) noexcept;
    HdaCaptureSetupSequenceResult next(
        HdaStreamRegisterWrite *write) noexcept;
    bool pendingWritePrecondition(uint32_t *value,
                                  uint8_t *widthBytes) const noexcept;
    HdaCaptureSetupSequenceResult confirm(uint32_t readback) noexcept;
    HdaCaptureSetupSequenceResult commandFailed() noexcept;
    bool complete() const noexcept;
    bool failed() const noexcept { return failed_; }

private:
    friend class HdaCaptureRollbackSequence;
    HdaCaptureSetupSequenceResult initializeSequenceData(
        const HdaCaptureSetupPlan &plan, uint64_t hdaBarLength,
        bool validateSetupOrder) noexcept;

    HdaCaptureSetupPlan plan_;
    uint8_t cursor_;
    bool initialized_;
    bool awaitingConfirmation_;
    bool failed_;
};

// Readback-gated executor contract for a previously generated rollback plan.
// It only yields one register write at a time and performs no hardware I/O.
class HdaCaptureRollbackSequence final {
public:
    HdaCaptureRollbackSequence() noexcept;

    HdaCaptureSetupSequenceResult initialize(
        const HdaCaptureRollbackPlan &plan,
        uint64_t hdaBarLength) noexcept;
    HdaCaptureSetupSequenceResult next(
        HdaStreamRegisterWrite *write) noexcept;
    bool pendingWritePrecondition(uint32_t *value,
                                  uint8_t *widthBytes) const noexcept;
    HdaCaptureSetupSequenceResult confirm(uint32_t readback) noexcept;
    HdaCaptureSetupSequenceResult commandFailed() noexcept;
    bool complete() const noexcept;
    bool failed() const noexcept;

private:
    HdaCaptureSetupSequence sequence_;
};

// Creates the stopped-stream register sequence for the reserved input
// descriptor. The first five rollback values come from the idle snapshot taken
// before reservation; SDn.CTL uses the confirmed reserved-tag readback because
// the tag must remain reserved during setup rollback. Input snapshot order is
// BDLPL, BDLPU, CBL, LVI, FMT, then CTL.
// The caller must separately perform the SD reset handshake, PPCTL routing,
// writes, and readbacks. This function performs no I/O.
HdaCaptureRegisterPlanResult planHdaCaptureRegisterProgramming(
    const HdaInputStreamAllocationPlan &allocation,
    const HdaCaptureBdlPlan &bdl,
    const uint32_t previousStreamRegisterValues[6],
    uint16_t hdaFormatWord,
    const HdaInputStreamReservationSequence &reservationSequence,
    uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    HdaCaptureRegisterPlan *out) noexcept;

// Plans read/modify/write values for the shared PPCTL and INTCTL registers,
// preserving every unrelated stream's state. Capability discovery, ownership,
// and fresh register reads must already be established by the caller.
HdaCaptureRegisterPlanResult planHdaCaptureRouting(
    const HdaInputStreamAllocationPlan &allocation,
    uint32_t ppCapabilityOffset,
    uint32_t ppCapabilityHeaderReadback,
    uint32_t ppctlReadback,
    uint32_t intctlReadback,
    uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    HdaCaptureRoutingPlan *out) noexcept;

// Combines PPCTL routing, stopped SDn programming, and INTCTL enable in the
// required order: PPCTL, six SDn writes (RUN remains clear), then INTCTL.
HdaCaptureRegisterPlanResult combineHdaCaptureSetupPlans(
    const HdaCaptureRegisterPlan &streamPlan,
    const HdaCaptureRoutingPlan &routingPlan,
    uint64_t hdaBarLength,
    HdaCaptureSetupPlan *out) noexcept;

// Plans restoration of the pre-configuration register state after the caller
// has independently stopped DMA and completed SDn reset. Shared routing bits
// are restored with fresh-readback RMW values; this function performs no I/O.
HdaCaptureRegisterPlanResult planHdaCaptureRollback(
    const HdaCaptureSetupPlan &setupPlan,
    uint32_t freshPpctlReadback,
    uint32_t freshIntctlReadback,
    const uint32_t freshStreamRegisterValues[6],
    uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    bool dmaStoppedAndDrainConfirmed,
    bool streamResetConfirmed,
    HdaCaptureRollbackPlan *out) noexcept;

} // namespace sof::ipc4::kernel
