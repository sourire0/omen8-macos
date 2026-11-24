#include "Cavs25CodeLoaderMmioExecutor.hpp"
#include "HdaCapturePositionPlan.hpp"

#include <IOKit/IOLib.h>

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kStreamRunMask = 1U << 1;
constexpr uint32_t kStreamResetMask = 1U;
constexpr uint32_t kStreamTagMask = 0x0fU << 20;
constexpr uint32_t kStreamInterruptMask = 0x1cU;
constexpr uint32_t kGctlOffset = 0x08;
constexpr uint32_t kGctlResetMask = 1U;
constexpr uint32_t kStreamDescriptorBase = 0x80;
constexpr uint32_t kStreamDescriptorStride = 0x20;
constexpr uint32_t kStreamBdlLowOffset = 0x18;
constexpr uint32_t kStreamBdlHighOffset = 0x1c;
constexpr uint32_t kStreamCblOffset = 0x08;
constexpr uint32_t kStreamLviOffset = 0x0c;
constexpr uint32_t kStreamFormatOffset = 0x12;
constexpr uint32_t kPpCtlOffset = 0x04;
constexpr uint32_t kIntctlOffset = 0x20;
constexpr uint32_t kPpGlobalProcessingEnable = 1U << 30;
constexpr uint32_t kPpInterruptEnable = 1U << 31;
constexpr uint32_t kIntGlobalEnable = 1U << 31;
constexpr uint32_t kIntControllerEnable = 1U << 30;

} // namespace

Cavs25CodeLoaderMmioExecutor::Cavs25CodeLoaderMmioExecutor(
    IOPCIDevice *device, IOService *owner, IOMemoryMap *hdaMap,
    const Cavs25BootLifecycle *lifecycle,
    bool sharedAppleHdaProvider) noexcept
    : device_(device), owner_(owner), hdaMap_(hdaMap),
      lifecycle_(lifecycle), sharedAppleHdaProvider_(sharedAppleHdaProvider),
      lastResetFailureStage_("not started"),
      lastResetGctl_(UINT32_MAX), lastResetControl_(UINT32_MAX),
      lastResetReadback_(UINT32_MAX), lastResetStatus_(UINT32_MAX) {}

bool Cavs25CodeLoaderMmioExecutor::deviceOpen() const noexcept {
    // A shared AppleHDA child may inspect the controller, but must not
    // program BAR0 behind AppleHDA's back. The shared path must use the
    // AppleHDAStreamLease API for setup, RUN/STOP and reset instead.
    return device_ && owner_ && hdaMap_ && !sharedAppleHdaProvider_ &&
        device_->isOpen(owner_) &&
        hdaMap_->getVirtualAddress() != 0 &&
        (hdaMap_->getMapOptions() & kIOMapReadOnly) == 0;
}

bool Cavs25CodeLoaderMmioExecutor::validMapping(
    uint32_t offset, uint8_t widthBytes) const noexcept {
    if (!hdaMap_ || (widthBytes != 1 && widthBytes != 2 && widthBytes != 4) ||
        (offset % widthBytes) != 0)
        return false;
    const uint64_t mapLength = hdaMap_->getLength();
    return static_cast<uint64_t>(offset) <= mapLength &&
        widthBytes <= mapLength - offset;
}

bool Cavs25CodeLoaderMmioExecutor::readRegister(
    uint32_t offset, uint32_t *value) const noexcept {
    if (!value || !deviceOpen() || !validMapping(offset, 4)) return false;
    const volatile uint32_t *base =
        reinterpret_cast<const volatile uint32_t *>(
            hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    *value = base[offset / sizeof(uint32_t)];
    OSSynchronizeIO();
    return *value != UINT32_MAX;
}

bool Cavs25CodeLoaderMmioExecutor::writeAndReadback(
    const HdaStreamRegisterWrite &write, uint32_t streamControlOffset,
    bool rollback, uint32_t *readback) const noexcept {
    if (!readback || !deviceOpen() ||
        !validMapping(write.offset, write.widthBytes) ||
        (write.widthBytes != 2 && write.widthBytes != 4))
        return false;
    if (write.offset == streamControlOffset) {
        if ((write.value & (kStreamRunMask | kStreamResetMask)) != 0)
            return false;
        if (!rollback &&
            (((write.value & kStreamTagMask) >> 20) != 1 ||
             (write.value & kStreamInterruptMask) != kStreamInterruptMask))
            return false;
    }

    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    switch (write.widthBytes) {
        case 2:
            *reinterpret_cast<volatile uint16_t *>(base + write.offset) =
                static_cast<uint16_t>(write.value);
            OSSynchronizeIO();
            *readback = *reinterpret_cast<volatile uint16_t *>(
                base + write.offset);
            break;
        case 4:
            *reinterpret_cast<volatile uint32_t *>(base + write.offset) =
                write.value;
            OSSynchronizeIO();
            *readback = *reinterpret_cast<volatile uint32_t *>(
                base + write.offset);
            break;
        default:
            return false;
    }
    OSSynchronizeIO();
    const uint32_t widthMask = write.widthBytes == 4
        ? UINT32_MAX : 0xffffU;
    return (*readback & widthMask) != widthMask;
}

CodeLoaderSetupSequenceResult
Cavs25CodeLoaderMmioExecutor::advanceSetup(
    Cavs25CodeLoaderSetupSequence *sequence) const noexcept {
    if (!sequence) return CodeLoaderSetupSequenceResult::InvalidArgument;
    if (!lifecycle_ || lifecycle_->phase() != BootPhase::DmaResourcesReady)
        return CodeLoaderSetupSequenceResult::InvalidState;
    HdaStreamRegisterWrite write{};
    const CodeLoaderSetupSequenceResult next = sequence->next(&write);
    if (next != CodeLoaderSetupSequenceResult::WriteRequired) return next;
    if (!deviceOpen()) return sequence->commandFailed();

    uint32_t gctl = UINT32_MAX;
    if (!readRegister(kGctlOffset, &gctl) ||
        (gctl & kGctlResetMask) == 0)
        return sequence->commandFailed();

    uint32_t ppHeader = UINT32_MAX;
    if (!readRegister(sequence->ppCapabilityOffset(), &ppHeader) ||
        ppHeader != sequence->ppCapabilityHeader())
        return sequence->commandFailed();
    if (sequence->spibPresent()) {
        uint32_t spibHeader = UINT32_MAX;
        if (!readRegister(sequence->spibCapabilityOffset(), &spibHeader) ||
            spibHeader != sequence->spibCapabilityHeader())
            return sequence->commandFailed();
    }

    const uint32_t ctlOffset = sequence->streamControlOffset();
    if (!validMapping(ctlOffset, 4) ||
        !validMapping(ctlOffset + 3U, 1))
        return sequence->commandFailed();
    const volatile uint8_t *base = reinterpret_cast<const volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint32_t currentCtl = *reinterpret_cast<const volatile uint32_t *>(
        base + ctlOffset);
    const uint8_t currentStatus = base[ctlOffset + 3U];
    OSSynchronizeIO();
    if ((currentCtl & (kStreamRunMask | kStreamResetMask)) != 0 ||
        ((currentCtl & kStreamTagMask) >> 20) != 1 || currentStatus != 0)
        return sequence->commandFailed();

    uint32_t readback = UINT32_MAX;
    if (!writeAndReadback(write, sequence->streamControlOffset(), false,
                          &readback))
        return sequence->commandFailed();
    return sequence->confirm(readback);
}

CodeLoaderReservationSequenceResult
Cavs25CodeLoaderMmioExecutor::advanceReservation(
    Cavs25CodeLoaderReservationSequence *sequence) const noexcept {
    if (!sequence) return CodeLoaderReservationSequenceResult::InvalidArgument;
    if (!lifecycle_ || lifecycle_->phase() != BootPhase::DmaResourcesReady)
        return CodeLoaderReservationSequenceResult::InvalidState;
    HdaStreamRegisterWrite write{};
    const CodeLoaderReservationSequenceResult next = sequence->next(&write);
    if (next != CodeLoaderReservationSequenceResult::WriteRequired)
        return next;
    if (!deviceOpen()) return sequence->commandFailed();

    uint32_t gctl = UINT32_MAX;
    if (!readRegister(kGctlOffset, &gctl) ||
        (gctl & kGctlResetMask) == 0)
        return sequence->commandFailed();
    const uint32_t ctlOffset = sequence->streamControlOffset();
    if (!validMapping(ctlOffset, 4) || !validMapping(ctlOffset + 3U, 1))
        return sequence->commandFailed();
    uint32_t currentCtl = UINT32_MAX;
    if (!readRegister(ctlOffset, &currentCtl) ||
        currentCtl != sequence->reservationBaseControl())
        return sequence->commandFailed();
    const volatile uint8_t *base = reinterpret_cast<const volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint8_t currentStatus = base[ctlOffset + 3U];
    OSSynchronizeIO();
    if (currentStatus != 0) return sequence->commandFailed();

    uint32_t readback = UINT32_MAX;
    if (!writeAndReadback(write, ctlOffset, true, &readback))
        return sequence->commandFailed();
    return sequence->confirm(readback);
}

CodeLoaderResetResult Cavs25CodeLoaderMmioExecutor::advanceReset(
    Cavs25CodeLoaderResetSequence *sequence) const noexcept {
    lastResetFailureStage_ = "reset sequence arguments";
    lastResetGctl_ = UINT32_MAX;
    lastResetControl_ = UINT32_MAX;
    lastResetReadback_ = UINT32_MAX;
    lastResetStatus_ = UINT32_MAX;
    if (!sequence) return CodeLoaderResetResult::InvalidArgument;
    lastResetFailureStage_ = "reset lifecycle phase";
    if (!lifecycle_ || lifecycle_->phase() != BootPhase::DmaResourcesReady)
        return CodeLoaderResetResult::InvalidState;
    lastResetFailureStage_ = "reset device and writable BAR lease";
    if (!deviceOpen()) return sequence->commandFailed();

    uint32_t gctl = UINT32_MAX;
    lastResetFailureStage_ = "GCTL read and controller reset state";
    if (!readRegister(kGctlOffset, &gctl) ||
        (gctl & kGctlResetMask) == 0)
        return sequence->commandFailed();
    lastResetGctl_ = gctl;

    const uint32_t ctlOffset = sequence->streamControlOffset();
    lastResetFailureStage_ = "SDn register window bounds";
    if (ctlOffset < kStreamDescriptorBase ||
        (ctlOffset - kStreamDescriptorBase) % kStreamDescriptorStride != 0 ||
        !validMapping(ctlOffset, 4) || !validMapping(ctlOffset + 3U, 1))
        return sequence->commandFailed();
    // descriptorControlOffset is SDn.CTL at descriptor base + 0x00.
    const uint32_t descriptorBase = ctlOffset;
    lastResetFailureStage_ = "SDn descriptor register bounds";
    if (!validMapping(descriptorBase + 0x08U, 4) ||
        !validMapping(descriptorBase + 0x0cU, 1) ||
        !validMapping(descriptorBase + 0x12U, 2) ||
        !validMapping(descriptorBase + 0x18U, 4) ||
        !validMapping(descriptorBase + 0x1cU, 4))
        return sequence->commandFailed();

    const volatile uint8_t *base = reinterpret_cast<const volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint32_t currentCtl = *reinterpret_cast<const volatile uint32_t *>(
        base + ctlOffset);
    const uint8_t currentStatus = base[ctlOffset + 3U];
    const uint32_t bdlLow = *reinterpret_cast<const volatile uint32_t *>(
        base + descriptorBase + 0x18U);
    const uint32_t bdlHigh = *reinterpret_cast<const volatile uint32_t *>(
        base + descriptorBase + 0x1cU);
    const uint32_t cbl = *reinterpret_cast<const volatile uint32_t *>(
        base + descriptorBase + 0x08U);
    const uint8_t lvi = base[descriptorBase + 0x0cU];
    const uint16_t format = *reinterpret_cast<const volatile uint16_t *>(
        base + descriptorBase + 0x12U);
    OSSynchronizeIO();
    lastResetControl_ = currentCtl;
    lastResetStatus_ = currentStatus;
    if (currentCtl == UINT32_MAX || currentStatus != 0 || bdlLow != 0 ||
        bdlHigh != 0 || cbl != 0 || lvi != 0 || format != 0)
    {
        lastResetFailureStage_ = "SDn descriptor no longer empty";
        return sequence->commandFailed();
    }

    HdaStreamRegisterWrite write{};
    lastResetFailureStage_ = "reset sequence control readback validation";
    const CodeLoaderResetResult result = sequence->next(currentCtl, &write);
    if (result != CodeLoaderResetResult::WriteRequired) return result;
    lastResetFailureStage_ = "reset write plan validation";
    if (write.offset != ctlOffset || write.widthBytes != 4 ||
        write.reserved[0] != 0 || write.reserved[1] != 0 ||
        write.reserved[2] != 0 ||
        (write.value & ~(kStreamResetMask)) !=
            (currentCtl & ~(kStreamResetMask)) ||
        (write.value & (kStreamRunMask | kStreamTagMask)) != 0 ||
        !validMapping(ctlOffset, 4))
        return sequence->commandFailed();

    volatile uint32_t *ctl = reinterpret_cast<volatile uint32_t *>(
        hdaMap_->getVirtualAddress() + ctlOffset);
    lastResetFailureStage_ = "SDn.SRST assert write and readback";
    OSSynchronizeIO();
    *ctl = write.value;
    OSSynchronizeIO();
    const uint32_t readback = *ctl;
    OSSynchronizeIO();
    lastResetReadback_ = readback;
    if (readback == UINT32_MAX ||
        (readback & (kStreamRunMask | kStreamTagMask)) != 0 ||
        (readback & ~kStreamResetMask) !=
            (write.value & ~kStreamResetMask))
        return sequence->commandFailed();
    lastResetFailureStage_ = (readback & kStreamResetMask) != 0
        ? "SDn.SRST asserted; polling for deassert write"
        : "SDn.SRST assert pending; polling for hardware acknowledgement";
    return CodeLoaderResetResult::Waiting;
}

CodeLoaderDmaStartResult
Cavs25CodeLoaderMmioExecutor::startFirmwareDma(
    const Cavs25CodeLoaderSetupSequence *setupSequence,
    uint32_t *controlReadback) const noexcept {
    if (!setupSequence || !controlReadback)
        return CodeLoaderDmaStartResult::InvalidArgument;
    *controlReadback = UINT32_MAX;
    if (!lifecycle_ || lifecycle_->phase() != BootPhase::RomInitDone)
        return CodeLoaderDmaStartResult::WrongPhase;
    if (!setupSequence->complete() || setupSequence->writeCount() < 8 ||
        setupSequence->writeCount() > 10)
        return CodeLoaderDmaStartResult::InvalidArgument;
    if (!deviceOpen()) return CodeLoaderDmaStartResult::DeviceUnavailable;

    uint32_t gctl = UINT32_MAX;
    if (!readRegister(kGctlOffset, &gctl) ||
        (gctl & kGctlResetMask) == 0)
        return CodeLoaderDmaStartResult::ControllerNotReady;

    const uint32_t ctlOffset = setupSequence->streamControlOffset();
    const uint8_t streamIndex = setupSequence->streamIndex();
    const uint32_t ctlMask = setupSequence->streamControlReadbackMask();
    const uint32_t stoppedCtl =
        setupSequence->streamControlReadbackValue();
    const uint32_t runTagResetMask =
        kStreamRunMask | kStreamResetMask | kStreamTagMask;
    if (streamIndex >= 32 || ctlOffset != kStreamDescriptorBase +
            static_cast<uint32_t>(streamIndex) * kStreamDescriptorStride ||
        (ctlMask & runTagResetMask) != runTagResetMask ||
        (stoppedCtl & (kStreamRunMask | kStreamResetMask)) != 0 ||
        (stoppedCtl & kStreamTagMask) == 0 ||
        !validMapping(ctlOffset, 4) ||
        !validMapping(ctlOffset + 3U, 1))
        return CodeLoaderDmaStartResult::InvalidDescriptor;

    const uint32_t descriptorOffsets[5] = {
        ctlOffset + kStreamBdlLowOffset,
        ctlOffset + kStreamBdlHighOffset,
        ctlOffset + kStreamCblOffset,
        ctlOffset + kStreamLviOffset,
        ctlOffset + kStreamFormatOffset,
    };
    const uint8_t descriptorWidths[5] = {4, 4, 4, 2, 2};
    const uint8_t setupIndices[5] = {1, 2, 3, 4, 5};
    uint32_t descriptorValues[5]{};
    for (uint8_t i = 0; i < 5; ++i) {
        if (!validMapping(descriptorOffsets[i], descriptorWidths[i]))
            return CodeLoaderDmaStartResult::UnsafeMapping;
        const volatile uint8_t *bytes = reinterpret_cast<const volatile uint8_t *>(
            hdaMap_->getVirtualAddress());
        OSSynchronizeIO();
        if (descriptorWidths[i] == 4) {
            descriptorValues[i] = *reinterpret_cast<const volatile uint32_t *>(
                bytes + descriptorOffsets[i]);
        } else {
            descriptorValues[i] = *reinterpret_cast<const volatile uint16_t *>(
                bytes + descriptorOffsets[i]);
        }
        OSSynchronizeIO();
        const uint32_t mask =
            setupSequence->plannedReadbackMask(setupIndices[i]);
        const uint32_t expected =
            setupSequence->plannedReadbackValue(setupIndices[i]);
        const uint32_t widthMask = descriptorWidths[i] == 4
            ? UINT32_MAX : 0xffffU;
        if (mask == 0 || expected == UINT32_MAX ||
            (descriptorValues[i] & mask) != expected ||
            (i >= 2 && (descriptorValues[i] & widthMask) == 0))
            return CodeLoaderDmaStartResult::InvalidDescriptor;
    }
    const uint64_t bdlAddress =
        (static_cast<uint64_t>(descriptorValues[1]) << 32) |
        descriptorValues[0];
    if (bdlAddress == 0 || (bdlAddress & 127U) != 0 ||
        descriptorValues[2] < 256 || (descriptorValues[2] & 3U) != 0)
        return CodeLoaderDmaStartResult::InvalidDescriptor;

    uint32_t ppHeader = UINT32_MAX;
    const uint32_t ppOffset = setupSequence->ppCapabilityOffset();
    if (!readRegister(ppOffset, &ppHeader) ||
        ppHeader != setupSequence->ppCapabilityHeader() ||
        !validMapping(ppOffset + kPpCtlOffset, 4))
        return CodeLoaderDmaStartResult::InvalidDescriptor;
    uint32_t ppctl = UINT32_MAX;
    if (!readRegister(ppOffset + kPpCtlOffset, &ppctl) ||
        ppctl != setupSequence->plannedReadbackValue(0) ||
        (ppctl & (1U << streamIndex)) == 0 ||
        (ppctl & (kPpGlobalProcessingEnable | kPpInterruptEnable)) !=
            (kPpGlobalProcessingEnable | kPpInterruptEnable))
        return CodeLoaderDmaStartResult::InvalidDescriptor;
    uint32_t intctl = UINT32_MAX;
    if (!readRegister(kIntctlOffset, &intctl) ||
        intctl != setupSequence->plannedReadbackValue(7) ||
        (intctl & (1U << streamIndex)) == 0 ||
        (intctl & (kIntGlobalEnable | kIntControllerEnable)) !=
            (kIntGlobalEnable | kIntControllerEnable))
        return CodeLoaderDmaStartResult::InvalidDescriptor;

    if (setupSequence->spibPresent()) {
        const uint32_t spibOffset = setupSequence->spibCapabilityOffset();
        uint32_t spibHeader = UINT32_MAX;
        uint32_t spbfcctl = UINT32_MAX;
        uint32_t position = UINT32_MAX;
        const uint32_t positionOffset = spibOffset + 0x08U +
            static_cast<uint32_t>(streamIndex) * 0x08U;
        if (!readRegister(spibOffset, &spibHeader) ||
            spibHeader != setupSequence->spibCapabilityHeader() ||
            !readRegister(spibOffset + 0x04U, &spbfcctl) ||
            (spbfcctl & setupSequence->plannedReadbackMask(8)) !=
                setupSequence->plannedReadbackValue(8) ||
            !readRegister(positionOffset, &position) ||
            position != descriptorValues[2] ||
            position != setupSequence->plannedReadbackValue(9))
            return CodeLoaderDmaStartResult::InvalidDescriptor;
    }

    uint32_t currentCtl = UINT32_MAX;
    if (!readRegister(ctlOffset, &currentCtl))
        return CodeLoaderDmaStartResult::DeviceUnavailable;
    if ((currentCtl & ctlMask) != stoppedCtl ||
        (currentCtl & (kStreamRunMask | kStreamResetMask)) != 0 ||
        (currentCtl & kStreamTagMask) != (stoppedCtl & kStreamTagMask))
        return CodeLoaderDmaStartResult::InvalidDescriptor;
    const volatile uint8_t *hdaBytes = reinterpret_cast<const volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint8_t streamStatus = hdaBytes[ctlOffset + 3U];
    OSSynchronizeIO();
    if (streamStatus != 0) return CodeLoaderDmaStartResult::InvalidDescriptor;

    if (!validMapping(ctlOffset, 4))
        return CodeLoaderDmaStartResult::UnsafeMapping;
    volatile uint32_t *ctl = reinterpret_cast<volatile uint32_t *>(
        hdaMap_->getVirtualAddress() + ctlOffset);
    const uint32_t requested = currentCtl | kStreamRunMask;
    OSSynchronizeIO();
    *ctl = requested;
    OSSynchronizeIO();
    for (uint32_t poll = 0; poll < 3000; ++poll) {
        *controlReadback = *ctl;
        OSSynchronizeIO();
        if (*controlReadback == UINT32_MAX ||
            ((*controlReadback ^ requested) & ~kStreamRunMask) != 0)
            return CodeLoaderDmaStartResult::RecoveryRequired;
        if ((*controlReadback & kStreamRunMask) != 0)
            return CodeLoaderDmaStartResult::Started;
        IODelay(100);
    }
    return CodeLoaderDmaStartResult::RecoveryRequired;
}

CodeLoaderDmaStopMmioResult
Cavs25CodeLoaderMmioExecutor::advanceDmaStop(
    Cavs25CodeLoaderDmaStopSequence *sequence) const noexcept {
    if (!sequence) return CodeLoaderDmaStopMmioResult::InvalidArgument;
    if (!lifecycle_ || lifecycle_->phase() != BootPhase::FirmwareEntered)
        return CodeLoaderDmaStopMmioResult::WrongPhase;
    if (!deviceOpen()) return CodeLoaderDmaStopMmioResult::DeviceUnavailable;
    const uint32_t ctlOffset = sequence->streamControlOffset();
    if (ctlOffset < kStreamDescriptorBase ||
        (ctlOffset - kStreamDescriptorBase) % kStreamDescriptorStride != 0 ||
        !validMapping(ctlOffset, 4) || !validMapping(ctlOffset + 3U, 1))
        return CodeLoaderDmaStopMmioResult::InvalidArgument;

    uint32_t gctl = UINT32_MAX;
    if (!readRegister(kGctlOffset, &gctl) ||
        (gctl & kGctlResetMask) == 0)
        return CodeLoaderDmaStopMmioResult::DeviceUnavailable;

    volatile uint8_t *bytes = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint32_t currentCtl = *reinterpret_cast<volatile uint32_t *>(
        bytes + ctlOffset);
    uint8_t status = bytes[ctlOffset + 3U];
    OSSynchronizeIO();
    if (currentCtl == UINT32_MAX ||
        (status & ~kHdaStreamStatusKnownMask) != 0)
        return CodeLoaderDmaStopMmioResult::InvalidStatus;
    if ((status & (kHdaStreamStatusFifoError |
                   kHdaStreamStatusDescriptorError)) != 0)
        return CodeLoaderDmaStopMmioResult::StreamFault;

    // BCIS is W1C. Never echo FIFORDY or reserved bits into SDn.STS.
    if ((status & kHdaStreamStatusBufferCompletion) != 0) {
        bytes[ctlOffset + 3U] = kHdaStreamStatusBufferCompletion;
        OSSynchronizeIO();
        status = bytes[ctlOffset + 3U];
        OSSynchronizeIO();
        if ((status & kHdaStreamStatusBufferCompletion) != 0)
            return CodeLoaderDmaStopMmioResult::Waiting;
    }

    HdaStreamRegisterWrite write{};
    const CodeLoaderDmaStopResult result = sequence->next(currentCtl, &write);
    if (result == CodeLoaderDmaStopResult::Stopped)
        return CodeLoaderDmaStopMmioResult::Stopped;
    if (result == CodeLoaderDmaStopResult::Waiting)
        return CodeLoaderDmaStopMmioResult::Waiting;
    if (result != CodeLoaderDmaStopResult::WriteRequired ||
        write.offset != ctlOffset || write.widthBytes != 4 ||
        write.reserved[0] != 0 || write.reserved[1] != 0 ||
        write.reserved[2] != 0 ||
        (write.value & (kStreamRunMask | kStreamResetMask)) != 0 ||
        !validMapping(ctlOffset, 4))
        return CodeLoaderDmaStopMmioResult::InvalidStatus;

    volatile uint32_t *ctl = reinterpret_cast<volatile uint32_t *>(bytes + ctlOffset);
    OSSynchronizeIO();
    *ctl = write.value;
    OSSynchronizeIO();
    const uint32_t readback = *ctl;
    OSSynchronizeIO();
    if (readback == UINT32_MAX ||
        ((readback ^ write.value) & 0x00ffffffU & ~kStreamRunMask) != 0)
    {
        sequence->commandFailed();
        return CodeLoaderDmaStopMmioResult::WriteFailed;
    }
    const CodeLoaderDmaStopResult confirmed = sequence->next(readback, &write);
    if (confirmed == CodeLoaderDmaStopResult::Stopped)
        return CodeLoaderDmaStopMmioResult::Stopped;
    if (confirmed == CodeLoaderDmaStopResult::Waiting)
        return CodeLoaderDmaStopMmioResult::Waiting;
    return CodeLoaderDmaStopMmioResult::WriteIssued;
}

CodeLoaderResetResult
Cavs25CodeLoaderMmioExecutor::advancePostDmaReset(
    Cavs25CodeLoaderPostDmaResetSequence *sequence) const noexcept {
    if (!sequence) return CodeLoaderResetResult::InvalidArgument;
    if (!lifecycle_ || lifecycle_->phase() != BootPhase::FirmwareEntered)
        return CodeLoaderResetResult::InvalidState;
    if (!deviceOpen()) return sequence->commandFailed();
    uint32_t gctl = UINT32_MAX;
    if (!readRegister(kGctlOffset, &gctl) ||
        (gctl & kGctlResetMask) == 0)
        return sequence->commandFailed();

    const uint32_t ctlOffset = sequence->streamControlOffset();
    if (ctlOffset < kStreamDescriptorBase ||
        (ctlOffset - kStreamDescriptorBase) % kStreamDescriptorStride != 0 ||
        !validMapping(ctlOffset, 4) || !validMapping(ctlOffset + 3U, 1))
        return sequence->commandFailed();
    const volatile uint8_t *base = reinterpret_cast<const volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint32_t currentCtl = *reinterpret_cast<const volatile uint32_t *>(
        base + ctlOffset);
    const uint8_t status = base[ctlOffset + 3U];
    OSSynchronizeIO();
    if (currentCtl == UINT32_MAX || (currentCtl & kStreamRunMask) != 0 ||
        (status & ~kHdaStreamStatusKnownMask) != 0 ||
        (status & (kHdaStreamStatusFifoError |
                   kHdaStreamStatusDescriptorError)) != 0)
        return sequence->commandFailed();

    HdaStreamRegisterWrite write{};
    const CodeLoaderResetResult result = sequence->next(currentCtl, &write);
    if (result != CodeLoaderResetResult::WriteRequired) return result;
    if (write.offset != ctlOffset || write.widthBytes != 4 ||
        write.reserved[0] != 0 || write.reserved[1] != 0 ||
        write.reserved[2] != 0 ||
        (write.value & kStreamRunMask) != 0 ||
        ((write.value ^ currentCtl) & ~kStreamResetMask) != 0 ||
        !validMapping(ctlOffset, 4))
        return sequence->commandFailed();
    volatile uint32_t *ctl = reinterpret_cast<volatile uint32_t *>(
        hdaMap_->getVirtualAddress() + ctlOffset);
    OSSynchronizeIO();
    *ctl = write.value;
    OSSynchronizeIO();
    const uint32_t readback = *ctl;
    OSSynchronizeIO();
    if (readback == UINT32_MAX ||
        ((readback ^ write.value) & kStreamRunMask) != 0 ||
        (readback & kStreamRunMask) != 0)
        return sequence->commandFailed();
    HdaStreamRegisterWrite unused{};
    return sequence->next(readback, &unused);
}

CodeLoaderSetupSequenceResult
Cavs25CodeLoaderMmioExecutor::advanceRollback(
    Cavs25CodeLoaderRollbackSequence *sequence) const noexcept {
    if (!sequence) return CodeLoaderSetupSequenceResult::InvalidArgument;
    if (!lifecycle_ ||
        (lifecycle_->phase() != BootPhase::DmaResourcesReady &&
         lifecycle_->phase() != BootPhase::RecoveryRequired))
        return CodeLoaderSetupSequenceResult::InvalidState;
    HdaStreamRegisterWrite write{};
    const CodeLoaderSetupSequenceResult next = sequence->next(&write);
    if (next != CodeLoaderSetupSequenceResult::WriteRequired) return next;
    if (!deviceOpen()) return sequence->commandFailed();

    uint32_t gctl = UINT32_MAX;
    uint32_t ppHeader = UINT32_MAX;
    if (!readRegister(kGctlOffset, &gctl) ||
        (gctl & kGctlResetMask) == 0 ||
        !readRegister(sequence->ppCapabilityOffset(), &ppHeader) ||
        ppHeader != sequence->ppCapabilityHeader())
        return sequence->commandFailed();
    if (sequence->spibPresent()) {
        uint32_t spibHeader = UINT32_MAX;
        if (!readRegister(sequence->spibCapabilityOffset(), &spibHeader) ||
            spibHeader != sequence->spibCapabilityHeader())
            return sequence->commandFailed();
    }

    const uint32_t ctlOffset = sequence->streamControlOffset();
    if (!validMapping(ctlOffset, 4) ||
        !validMapping(ctlOffset + 3U, 1))
        return sequence->commandFailed();
    const volatile uint8_t *base = reinterpret_cast<const volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint32_t currentCtl = *reinterpret_cast<const volatile uint32_t *>(
        base + ctlOffset);
    const uint8_t currentStatus = base[ctlOffset + 3U];
    OSSynchronizeIO();
    if ((currentCtl & (kStreamRunMask | kStreamResetMask)) != 0 ||
        currentStatus != 0)
        return sequence->commandFailed();

    const uint32_t expectedBefore = sequence->expectedBeforeValue();
    if (expectedBefore != UINT32_MAX) {
        uint32_t liveValue = UINT32_MAX;
        if (!readRegister(write.offset, &liveValue) ||
            liveValue != expectedBefore)
            return sequence->commandFailed();
    }

    uint32_t readback = UINT32_MAX;
    if (!writeAndReadback(write, ctlOffset, true, &readback))
        return sequence->commandFailed();
    return sequence->confirm(readback);
}

} // namespace sof::ipc4::kernel
