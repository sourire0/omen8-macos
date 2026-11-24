#pragma once

#include "Cavs25CodeLoaderStreamPlan.hpp"
#include "Cavs25DmaPlan.hpp"
#include "HdaCaptureRegisterPlan.hpp"

namespace sof::ipc4::kernel {

struct Cavs25CodeLoaderRegisterSnapshot {
    uint32_t ppCapabilityOffset;
    uint32_t ppCapabilityHeader;
    uint32_t ppctl;
    uint32_t intctl;
    uint32_t streamRegisters[6]; // BDLPL, BDLPU, CBL, LVI, FMT, CTL
    uint32_t reservedStreamControlReadback;
    uint8_t streamStatus;
    bool spibPresent;
    uint32_t spibCapabilityOffset;
    uint32_t spibCapabilityHeader;
    uint32_t spbfcctl;
    uint32_t spibPosition;
    bool controllerReady;
};

struct Cavs25CodeLoaderSetupPlan {
    HdaStreamRegisterWrite writes[10];
    uint32_t previousValues[10];
    uint32_t readbackMasks[10];
    uint32_t readbackValues[10];
    uint32_t streamControlOffset;
    uint32_t ppCapabilityOffset;
    uint32_t ppCapabilityHeader;
    uint32_t spibCapabilityOffset;
    uint32_t spibCapabilityHeader;
    uint8_t streamIndex;
    uint8_t writeCount;
    uint8_t spibWriteIndex;
    uint8_t reserved;
    bool spibPresent;
};

struct Cavs25CodeLoaderRollbackPlan {
    HdaStreamRegisterWrite writes[12];
    uint32_t expectedBeforeValues[12];
    uint32_t readbackMasks[12];
    uint32_t readbackValues[12];
    uint32_t streamControlOffset;
    uint32_t ppCapabilityOffset;
    uint32_t ppCapabilityHeader;
    uint32_t spibCapabilityOffset;
    uint32_t spibCapabilityHeader;
    uint8_t streamIndex;
    uint8_t writeCount;
    uint8_t reserved[2];
    bool spibPresent;
};

enum class CodeLoaderSetupPlanResult : uint8_t {
    Ok,
    InvalidArgument,
    OwnershipNotProven,
    ControllerNotReady,
    InvalidCapability,
    InvalidReservation,
    InvalidDmaPlan,
    InvalidRegisterSnapshot,
    InsufficientBar,
};

enum class CodeLoaderSetupSequenceResult : uint8_t {
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

enum class CodeLoaderResetResult : uint8_t {
    WriteRequired,
    Waiting,
    Complete,
    InvalidArgument,
    InvalidState,
    InvalidReadback,
    UnexpectedReadback,
    CommandFailed,
    TimedOut,
};

enum class CodeLoaderDmaStopResult : uint8_t {
    WriteRequired,
    Waiting,
    Stopped,
    InvalidArgument,
    InvalidState,
    InvalidReadback,
    UnexpectedReadback,
    TimedOut,
    CommandFailed,
};

class Cavs25CodeLoaderSetupSequence;

// Output-descriptor SRST handshake. It only proposes one SDn.CTL write at a
// time and requires fresh readbacks; no RUN/DMA start operation is represented.
class Cavs25CodeLoaderResetSequence final {
public:
    Cavs25CodeLoaderResetSequence() noexcept;
    bool initialize(const Cavs25CodeLoaderStreamReservationPlan &reservation,
                    uint32_t initialControlReadback,
                    uint64_t hdaBarLength,
                    uint32_t maximumPollSamples,
                    bool controllerExclusivelyOwned,
                    bool descriptorVerifiedEmpty) noexcept;
    CodeLoaderResetResult next(uint32_t controlReadback,
                               HdaStreamRegisterWrite *write) noexcept;
    CodeLoaderResetResult commandFailed() noexcept;
    bool complete() const noexcept;
    uint32_t streamControlOffset() const noexcept { return controlOffset_; }
private:
    enum class State : uint8_t { Uninitialized, NeedAssert, AwaitAssert,
                                 AwaitDeassert, Complete, Failed };
    State state_;
    uint32_t controlOffset_;
    uint32_t initialControl_;
    uint32_t samplesRemaining_;
    uint32_t maximumPollSamples_;
};

// Bounded SDn.RUN clear sequence for the code-loader descriptor. Initialization
// requires the completed SOF ROM firmware-entered checkpoint as a drain fence.
class Cavs25CodeLoaderDmaStopSequence final {
public:
    Cavs25CodeLoaderDmaStopSequence() noexcept;
    bool initialize(const Cavs25CodeLoaderSetupSequence &setupSequence,
                    uint32_t initialControlReadback,
                    uint32_t maximumPollSamples,
                    bool controllerExclusivelyOwned,
                    bool firmwareEnteredCheckpoint) noexcept;
    CodeLoaderDmaStopResult next(uint32_t controlReadback,
                                 HdaStreamRegisterWrite *write) noexcept;
    CodeLoaderDmaStopResult commandFailed() noexcept;
    bool complete() const noexcept;
    bool confirmsStoppedControlReadback(uint32_t controlReadback) const noexcept;
    uint32_t streamControlOffset() const noexcept { return controlOffset_; }
private:
    enum class State : uint8_t { Uninitialized, NeedStop, AwaitStop,
                                 Complete, Failed };
    State state_;
    uint32_t controlOffset_;
    uint32_t expectedMask_;
    uint32_t expectedValue_;
    uint32_t samplesRemaining_;
    uint32_t maximumPollSamples_;
};

// Post-firmware SDn.SRST handshake. Requires the completed RUN-clear fence
// and keeps the reserved stream tag/configuration intact across reset.
class Cavs25CodeLoaderPostDmaResetSequence final {
public:
    Cavs25CodeLoaderPostDmaResetSequence() noexcept;
    bool initialize(const Cavs25CodeLoaderSetupSequence &setupSequence,
                    const Cavs25CodeLoaderDmaStopSequence &dmaStopSequence,
                    uint32_t initialControlReadback,
                    uint32_t maximumPollSamples,
                    bool controllerExclusivelyOwned) noexcept;
    CodeLoaderResetResult next(uint32_t controlReadback,
                               HdaStreamRegisterWrite *write) noexcept;
    CodeLoaderResetResult commandFailed() noexcept;
    bool complete() const noexcept;
    bool confirmsResetControlReadback(uint32_t controlReadback) const noexcept;
    uint32_t streamControlOffset() const noexcept { return controlOffset_; }
private:
    enum class State : uint8_t { Uninitialized, NeedAssert, AwaitAssert,
                                 AwaitDeassert, Complete, Failed };
    State state_;
    uint32_t controlOffset_;
    uint32_t expectedMask_;
    uint32_t expectedValue_;
    uint32_t samplesRemaining_;
    uint32_t maximumPollSamples_;
};

// Restores the saved code-loader descriptor state after the caller proves
// DMA has stopped/drained and SD reset completed. Shared routing registers
// are restored only for bits changed by setup. This is a pure plan.
CodeLoaderSetupPlanResult planCavs25CodeLoaderRollback(
    const Cavs25CodeLoaderSetupPlan &setupPlan,
    uint32_t freshPpctlReadback,
    uint32_t freshIntctlReadback,
    uint32_t freshStreamControlReadback,
    uint32_t freshSpbfcctlReadback,
    uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    bool dmaStoppedAndDrained,
    bool streamResetConfirmed,
    Cavs25CodeLoaderRollbackPlan *out) noexcept;

class Cavs25CodeLoaderRollbackSequence final {
public:
    Cavs25CodeLoaderRollbackSequence() noexcept;
    CodeLoaderSetupSequenceResult initialize(
        const Cavs25CodeLoaderRollbackPlan &plan,
        uint64_t hdaBarLength) noexcept;
    CodeLoaderSetupSequenceResult next(HdaStreamRegisterWrite *write) noexcept;
    CodeLoaderSetupSequenceResult confirm(uint32_t readback) noexcept;
    CodeLoaderSetupSequenceResult commandFailed() noexcept;
    bool complete() const noexcept;
    bool failed() const noexcept { return failed_; }
    uint32_t streamControlOffset() const noexcept {
        return initialized_ ? plan_.streamControlOffset : 0;
    }
    uint32_t expectedBeforeValue() const noexcept {
        return initialized_ && cursor_ < plan_.writeCount
            ? plan_.expectedBeforeValues[cursor_] : UINT32_MAX;
    }
    uint32_t ppCapabilityOffset() const noexcept {
        return initialized_ ? plan_.ppCapabilityOffset : 0;
    }
    uint32_t ppCapabilityHeader() const noexcept {
        return initialized_ ? plan_.ppCapabilityHeader : 0;
    }
    uint32_t spibCapabilityOffset() const noexcept {
        return initialized_ && plan_.spibPresent
            ? plan_.spibCapabilityOffset : 0;
    }
    uint32_t spibCapabilityHeader() const noexcept {
        return initialized_ && plan_.spibPresent
            ? plan_.spibCapabilityHeader : 0;
    }
    bool spibPresent() const noexcept {
        return initialized_ && plan_.spibPresent;
    }
private:
    Cavs25CodeLoaderRollbackPlan plan_;
    uint8_t cursor_;
    bool initialized_;
    bool awaitingConfirmation_;
    bool failed_;
};

// Produces a stopped code-loader stream setup. All shared routing bits are
// preserved from the supplied snapshot; SDn.RUN remains clear. This is a
// pure plan and performs no register access.
CodeLoaderSetupPlanResult planCavs25CodeLoaderSetup(
    const Cavs25CodeLoaderStreamReservationPlan &reservation,
    const Cavs25DmaPlan &dmaPlan,
    const Cavs25CodeLoaderRegisterSnapshot &snapshot,
    uint64_t hdaBarLength,
    bool controllerExclusivelyOwned,
    bool audioOwnerQuiesced,
    Cavs25CodeLoaderSetupPlan *out) noexcept;

// Issues one setup register write at a time and requires its readback before
// returning the next. The sequence never sets SDn.RUN.
class Cavs25CodeLoaderSetupSequence final {
public:
    Cavs25CodeLoaderSetupSequence() noexcept;

    CodeLoaderSetupSequenceResult initialize(
        const Cavs25CodeLoaderSetupPlan &plan,
        uint64_t hdaBarLength) noexcept;
    CodeLoaderSetupSequenceResult next(
        HdaStreamRegisterWrite *write) noexcept;
    CodeLoaderSetupSequenceResult confirm(uint32_t readback) noexcept;
    CodeLoaderSetupSequenceResult commandFailed() noexcept;
    bool complete() const noexcept;
    bool failed() const noexcept { return failed_; }
    uint32_t streamControlOffset() const noexcept {
        return initialized_ ? plan_.streamControlOffset : 0;
    }
    uint32_t streamControlReadbackMask() const noexcept {
        return initialized_ && plan_.writeCount >= 7
            ? plan_.readbackMasks[6] : 0;
    }
    uint32_t streamControlReadbackValue() const noexcept {
        return initialized_ && plan_.writeCount >= 7
            ? plan_.readbackValues[6] : UINT32_MAX;
    }
    uint8_t writeCount() const noexcept {
        return initialized_ ? plan_.writeCount : 0;
    }
    uint8_t streamIndex() const noexcept {
        return initialized_ ? plan_.streamIndex : UINT8_MAX;
    }
    uint32_t plannedWriteValue(uint8_t index) const noexcept {
        return initialized_ && index < plan_.writeCount
            ? plan_.writes[index].value : UINT32_MAX;
    }
    uint32_t plannedReadbackMask(uint8_t index) const noexcept {
        return initialized_ && index < plan_.writeCount
            ? plan_.readbackMasks[index] : 0;
    }
    uint32_t plannedReadbackValue(uint8_t index) const noexcept {
        return initialized_ && index < plan_.writeCount
            ? plan_.readbackValues[index] : UINT32_MAX;
    }
    uint32_t ppCapabilityOffset() const noexcept {
        return initialized_ ? plan_.ppCapabilityOffset : 0;
    }
    uint32_t ppCapabilityHeader() const noexcept {
        return initialized_ ? plan_.ppCapabilityHeader : 0;
    }
    uint32_t spibCapabilityOffset() const noexcept {
        return initialized_ && plan_.spibPresent
            ? plan_.spibCapabilityOffset : 0;
    }
    uint32_t spibCapabilityHeader() const noexcept {
        return initialized_ && plan_.spibPresent
            ? plan_.spibCapabilityHeader : 0;
    }
    bool spibPresent() const noexcept {
        return initialized_ && plan_.spibPresent;
    }

private:
    Cavs25CodeLoaderSetupPlan plan_;
    uint8_t cursor_;
    bool initialized_;
    bool awaitingConfirmation_;
    bool failed_;
};

} // namespace sof::ipc4::kernel
