#pragma once

#include "Cavs25BootLifecycle.hpp"
#include "Cavs25CodeLoaderSetupPlan.hpp"

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace sof::ipc4::kernel {

enum class CodeLoaderDmaStartResult : uint8_t {
    Started,
    InvalidArgument,
    WrongPhase,
    DeviceUnavailable,
    UnsafeMapping,
    ControllerNotReady,
    InvalidDescriptor,
    WriteFailed,
    ReadbackMismatch,
    RecoveryRequired,
};

enum class CodeLoaderDmaStopMmioResult : uint8_t {
    WriteIssued,
    Waiting,
    Stopped,
    InvalidArgument,
    WrongPhase,
    DeviceUnavailable,
    InvalidStatus,
    StreamFault,
    WriteFailed,
};

// Executes one prevalidated, stopped code-loader setup write per call. A
// separate, explicit firmware-DMA start method is gated to RomInitDone; the
// controller does not call it from start().
class Cavs25CodeLoaderMmioExecutor final {
public:
    Cavs25CodeLoaderMmioExecutor(
        IOPCIDevice *device, IOService *owner, IOMemoryMap *hdaMap,
        const Cavs25BootLifecycle *lifecycle,
        bool sharedAppleHdaProvider = false) noexcept;

    CodeLoaderSetupSequenceResult advanceSetup(
        Cavs25CodeLoaderSetupSequence *sequence) const noexcept;
    CodeLoaderReservationSequenceResult advanceReservation(
        Cavs25CodeLoaderReservationSequence *sequence) const noexcept;
    CodeLoaderResetResult advanceReset(
        Cavs25CodeLoaderResetSequence *sequence) const noexcept;
    const char *lastResetFailureStage() const noexcept {
        return lastResetFailureStage_;
    }
    uint32_t lastResetGctl() const noexcept { return lastResetGctl_; }
    uint32_t lastResetControl() const noexcept { return lastResetControl_; }
    uint32_t lastResetReadback() const noexcept { return lastResetReadback_; }
    uint32_t lastResetStatus() const noexcept { return lastResetStatus_; }
    // Explicit firmware-loader DMA start. The controller does not call this
    // from start(); callers must reach RomInitDone after completing setup.
    CodeLoaderDmaStartResult startFirmwareDma(
        const Cavs25CodeLoaderSetupSequence *setupSequence,
        uint32_t *controlReadback) const noexcept;
    // Clears SDn.RUN only after FirmwareEntered and acknowledges BCIS only.
    CodeLoaderDmaStopMmioResult advanceDmaStop(
        Cavs25CodeLoaderDmaStopSequence *sequence) const noexcept;
    // Post-DMA stream reset fence; only valid after RUN is observed clear.
    CodeLoaderResetResult advancePostDmaReset(
        Cavs25CodeLoaderPostDmaResetSequence *sequence) const noexcept;
    CodeLoaderSetupSequenceResult advanceRollback(
        Cavs25CodeLoaderRollbackSequence *sequence) const noexcept;

private:
    bool deviceOpen() const noexcept;
    bool validMapping(uint32_t offset, uint8_t widthBytes) const noexcept;
    bool readRegister(uint32_t offset, uint32_t *value) const noexcept;
    bool writeAndReadback(const HdaStreamRegisterWrite &write,
                          uint32_t streamControlOffset,
                          bool rollback,
                          uint32_t *readback) const noexcept;

    IOPCIDevice *device_;
    IOService *owner_;
    IOMemoryMap *hdaMap_;
    const Cavs25BootLifecycle *lifecycle_;
    bool sharedAppleHdaProvider_;
    mutable const char *lastResetFailureStage_;
    mutable uint32_t lastResetGctl_;
    mutable uint32_t lastResetControl_;
    mutable uint32_t lastResetReadback_;
    mutable uint32_t lastResetStatus_;
};

} // namespace sof::ipc4::kernel
