#pragma once

#include "HdaCaptureRegisterPlan.hpp"
#include "HdaCaptureResetPlan.hpp"
#include "HdaCapturePositionPlan.hpp"
#include "HdaCaptureTriggerPlan.hpp"

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace sof::ipc4::kernel {

// Executes one prevalidated HDA setup/rollback write per call, only while the
// owner holds the PCI provider open and the BAR mapping is writable. The
// caller still controls when these methods are invoked and must serialize
// them. `executeStreamRunAction(StartHdaDma, ...)` can set SDn.RUN when
// explicitly called. The offline build and current controller never call it;
// this class has no DSP firmware startup operation.
class HdaCaptureMmioExecutor final {
public:
    HdaCaptureMmioExecutor() noexcept;
    HdaCaptureMmioExecutor(IOPCIDevice *device, IOService *owner,
                           IOMemoryMap *hdaMap) noexcept;
    bool configure(IOPCIDevice *device, IOService *owner,
                   IOMemoryMap *hdaMap) noexcept;
    void reset() noexcept;

    HdaCaptureSetupSequenceResult advanceSetup(
        HdaCaptureSetupSequence *sequence) const noexcept;
    HdaCaptureSetupSequenceResult advanceRollback(
        HdaCaptureRollbackSequence *sequence) const noexcept;
    HdaStreamResetResult advanceReset(
        HdaCaptureResetSequence *sequence) const noexcept;
    HdaInputStreamReservationSequenceResult advanceReservation(
        HdaInputStreamReservationSequence *sequence) const noexcept;
    bool readStreamStatus(uint32_t streamControlOffset,
                          uint8_t *status) const noexcept;
    bool readLinkPosition(uint32_t streamControlOffset,
                          uint32_t *positionBytes) const noexcept;
    bool writeIocAck(uint32_t streamControlOffset,
                     const HdaCaptureIocAck &ack) const noexcept;
    bool executeStreamRunAction(
        HdaCaptureTriggerAction action, uint32_t streamControlOffset,
        uint32_t expectedRunningMask, uint32_t expectedRunningValue,
        uint32_t *controlReadback) const noexcept;

private:
    bool deviceOpen() const noexcept;
    bool validStreamControlOffset(uint32_t offset) const noexcept;
    bool writeAndReadback(const HdaStreamRegisterWrite &write,
                          uint32_t *readback) const noexcept;

    IOPCIDevice *device_;
    IOService *owner_;
    IOMemoryMap *hdaMap_;
};

} // namespace sof::ipc4::kernel
