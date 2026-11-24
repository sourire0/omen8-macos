#include "HdaCaptureMmioExecutor.hpp"

#include <IOKit/IOLib.h>

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kStreamDescriptorBase = 0x80;
constexpr uint32_t kStreamDescriptorStride = 0x20;
constexpr uint32_t kMaximumInputStreams = 15;
constexpr uint32_t kStreamStatusW1cMask = kHdaStreamStatusW1cMask;
constexpr uint32_t kStreamResetMask = 1U;
constexpr uint32_t kStreamRunMask = 1U << 1;
constexpr uint32_t kStreamTagMask = 0x0fU << 20;
constexpr uint32_t kGctlOffset = 0x08;
constexpr uint32_t kGctlResetMask = 1U;

} // namespace

HdaCaptureMmioExecutor::HdaCaptureMmioExecutor() noexcept
    : device_(nullptr), owner_(nullptr), hdaMap_(nullptr) {}

HdaCaptureMmioExecutor::HdaCaptureMmioExecutor(
    IOPCIDevice *device, IOService *owner, IOMemoryMap *hdaMap) noexcept
    : device_(device), owner_(owner), hdaMap_(hdaMap) {}

bool HdaCaptureMmioExecutor::configure(
    IOPCIDevice *device, IOService *owner, IOMemoryMap *hdaMap) noexcept {
    if (!device || !owner || !hdaMap || hdaMap->getVirtualAddress() == 0 ||
        (hdaMap->getMapOptions() & kIOMapReadOnly) != 0 ||
        !device->isOpen(owner))
        return false;
    device_ = device;
    owner_ = owner;
    hdaMap_ = hdaMap;
    return true;
}

void HdaCaptureMmioExecutor::reset() noexcept {
    device_ = nullptr;
    owner_ = nullptr;
    hdaMap_ = nullptr;
}

bool HdaCaptureMmioExecutor::deviceOpen() const noexcept {
    return device_ && owner_ && hdaMap_ && device_->isOpen(owner_) &&
           hdaMap_->getVirtualAddress() != 0 &&
           (hdaMap_->getMapOptions() & kIOMapReadOnly) == 0;
}

bool HdaCaptureMmioExecutor::validStreamControlOffset(
    uint32_t offset) const noexcept {
    return offset >= kStreamDescriptorBase &&
           ((offset - kStreamDescriptorBase) % kStreamDescriptorStride) == 0 &&
           ((offset - kStreamDescriptorBase) / kStreamDescriptorStride) <
               kMaximumInputStreams;
}

bool HdaCaptureMmioExecutor::writeAndReadback(
    const HdaStreamRegisterWrite &write, uint32_t *readback) const noexcept {
    if (!readback || !deviceOpen() ||
        (write.widthBytes != 1 && write.widthBytes != 2 &&
         write.widthBytes != 4) ||
        (write.offset % write.widthBytes) != 0)
        return false;

    const uint64_t mapLength = hdaMap_->getLength();
    if (static_cast<uint64_t>(write.offset) > mapLength ||
        write.widthBytes > mapLength - write.offset)
        return false;

    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    switch (write.widthBytes) {
        case 1:
            *reinterpret_cast<volatile uint8_t *>(base + write.offset) =
                static_cast<uint8_t>(write.value);
            OSSynchronizeIO();
            *readback = *reinterpret_cast<volatile uint8_t *>(
                base + write.offset);
            break;
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
        ? UINT32_MAX
        : (1U << (8U * write.widthBytes)) - 1U;
    return (*readback & widthMask) != widthMask;
}

HdaCaptureSetupSequenceResult HdaCaptureMmioExecutor::advanceSetup(
    HdaCaptureSetupSequence *sequence) const noexcept {
    if (!sequence) return HdaCaptureSetupSequenceResult::InvalidArgument;
    HdaStreamRegisterWrite write{};
    const HdaCaptureSetupSequenceResult next = sequence->next(&write);
    if (next != HdaCaptureSetupSequenceResult::WriteRequired) return next;

    uint32_t expectedBeforeWrite = UINT32_MAX;
    uint8_t expectedWidth = 0;
    if (!sequence->pendingWritePrecondition(&expectedBeforeWrite,
                                            &expectedWidth) ||
        expectedWidth != write.widthBytes)
        return sequence->commandFailed();
    const uint64_t mapLength = hdaMap_ ? hdaMap_->getLength() : 0;
    if (!deviceOpen() ||
        static_cast<uint64_t>(write.offset) + write.widthBytes > mapLength)
        return sequence->commandFailed();
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    uint32_t beforeWrite = UINT32_MAX;
    if (write.widthBytes == 2)
        beforeWrite = *reinterpret_cast<volatile uint16_t *>(base + write.offset);
    else if (write.widthBytes == 4)
        beforeWrite = *reinterpret_cast<volatile uint32_t *>(base + write.offset);
    else
        return sequence->commandFailed();
    OSSynchronizeIO();
    const uint32_t preconditionMask = write.widthBytes == 2
        ? UINT16_MAX : UINT32_MAX;
    if (beforeWrite == UINT32_MAX ||
        (beforeWrite & preconditionMask) !=
            (expectedBeforeWrite & preconditionMask))
        return sequence->commandFailed();

    uint32_t readback = UINT32_MAX;
    if (!writeAndReadback(write, &readback)) return sequence->commandFailed();
    return sequence->confirm(readback);
}

HdaCaptureSetupSequenceResult HdaCaptureMmioExecutor::advanceRollback(
    HdaCaptureRollbackSequence *sequence) const noexcept {
    if (!sequence) return HdaCaptureSetupSequenceResult::InvalidArgument;
    HdaStreamRegisterWrite write{};
    const HdaCaptureSetupSequenceResult next = sequence->next(&write);
    if (next != HdaCaptureSetupSequenceResult::WriteRequired) return next;

    uint32_t expectedBeforeWrite = UINT32_MAX;
    uint8_t expectedWidth = 0;
    if (!sequence->pendingWritePrecondition(&expectedBeforeWrite,
                                            &expectedWidth) ||
        expectedWidth != write.widthBytes)
        return sequence->commandFailed();
    const uint64_t mapLength = hdaMap_ ? hdaMap_->getLength() : 0;
    if (!deviceOpen() ||
        static_cast<uint64_t>(write.offset) + write.widthBytes > mapLength)
        return sequence->commandFailed();
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    uint32_t beforeWrite = UINT32_MAX;
    if (write.widthBytes == 2)
        beforeWrite = *reinterpret_cast<volatile uint16_t *>(base + write.offset);
    else if (write.widthBytes == 4)
        beforeWrite = *reinterpret_cast<volatile uint32_t *>(base + write.offset);
    else
        return sequence->commandFailed();
    OSSynchronizeIO();
    const uint32_t preconditionMask = write.widthBytes == 2
        ? UINT16_MAX : UINT32_MAX;
    if (beforeWrite == UINT32_MAX ||
        (beforeWrite & preconditionMask) !=
            (expectedBeforeWrite & preconditionMask))
        return sequence->commandFailed();

    uint32_t readback = UINT32_MAX;
    if (!writeAndReadback(write, &readback)) return sequence->commandFailed();
    return sequence->confirm(readback);
}

HdaStreamResetResult HdaCaptureMmioExecutor::advanceReset(
    HdaCaptureResetSequence *sequence) const noexcept {
    if (!sequence) return HdaStreamResetResult::InvalidArgument;
    if (!deviceOpen()) return HdaStreamResetResult::InvalidInitialState;
    const uint32_t controlOffset = sequence->streamControlOffset();
    if (!validStreamControlOffset(controlOffset) ||
        static_cast<uint64_t>(controlOffset) > hdaMap_->getLength() ||
        sizeof(uint32_t) > hdaMap_->getLength() - controlOffset)
        return HdaStreamResetResult::InvalidInitialState;

    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint32_t controlReadback = *reinterpret_cast<volatile uint32_t *>(
        base + controlOffset);
    OSSynchronizeIO();

    HdaStreamResetWrite resetWrite{};
    const HdaStreamResetResult result =
        sequence->next(controlReadback, &resetWrite);
    if (result != HdaStreamResetResult::WriteRequired) return result;
    if (resetWrite.offset != controlOffset ||
        (resetWrite.action != HdaStreamResetAction::AssertReset &&
         resetWrite.action != HdaStreamResetAction::DeassertReset))
        return sequence->commandFailed();

    HdaStreamRegisterWrite write{};
    write.offset = resetWrite.offset;
    write.value = resetWrite.value;
    write.widthBytes = 4;
    uint32_t readback = UINT32_MAX;
    if (!writeAndReadback(write, &readback)) return sequence->commandFailed();
    if ((readback & (kStreamRunMask | kStreamTagMask)) != 0)
        return sequence->commandFailed();
    return HdaStreamResetResult::Waiting;
}

HdaInputStreamReservationSequenceResult
HdaCaptureMmioExecutor::advanceReservation(
    HdaInputStreamReservationSequence *sequence) const noexcept {
    if (!sequence)
        return HdaInputStreamReservationSequenceResult::InvalidArgument;
    if (!deviceOpen())
        return HdaInputStreamReservationSequenceResult::InvalidState;
    const HdaInputStreamReservationPlan *plan = sequence->plan();
    if (!plan) return HdaInputStreamReservationSequenceResult::InvalidState;

    HdaInputStreamRegisterWrite write{};
    const HdaInputStreamReservationSequenceResult next =
        sequence->next(&write);
    if (next != HdaInputStreamReservationSequenceResult::WriteRequired)
        return next;
    if (write.offset != plan->descriptorControlOffset ||
        write.widthBytes != sizeof(uint32_t) ||
        write.reserved[0] != 0 || write.reserved[1] != 0 ||
        write.reserved[2] != 0 ||
        !validStreamControlOffset(write.offset))
        return sequence->commandFailed();

    const uint64_t mapLength = hdaMap_->getLength();
    if (static_cast<uint64_t>(kGctlOffset) + sizeof(uint32_t) > mapLength ||
        static_cast<uint64_t>(write.offset) + sizeof(uint32_t) > mapLength ||
        static_cast<uint64_t>(write.offset) + 3U >= mapLength)
        return sequence->commandFailed();

    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint32_t gctl = *reinterpret_cast<volatile uint32_t *>(
        base + kGctlOffset);
    const uint32_t control = *reinterpret_cast<volatile uint32_t *>(
        base + write.offset);
    const uint8_t status = *reinterpret_cast<volatile uint8_t *>(
        base + write.offset + 3U);
    OSSynchronizeIO();
    if (gctl == UINT32_MAX || (gctl & kGctlResetMask) == 0 ||
        control != plan->previousControlReadback || status != 0)
        return sequence->commandFailed();

    uint32_t readback = UINT32_MAX;
    HdaStreamRegisterWrite hdaWrite{};
    hdaWrite.offset = write.offset;
    hdaWrite.value = write.value;
    hdaWrite.widthBytes = write.widthBytes;
    hdaWrite.reserved[0] = write.reserved[0];
    hdaWrite.reserved[1] = write.reserved[1];
    hdaWrite.reserved[2] = write.reserved[2];
    if (!writeAndReadback(hdaWrite, &readback))
        return sequence->commandFailed();
    return sequence->confirm(readback);
}

bool HdaCaptureMmioExecutor::readStreamStatus(
    uint32_t streamControlOffset, uint8_t *status) const noexcept {
    if (!status || !deviceOpen() ||
        !validStreamControlOffset(streamControlOffset))
        return false;
    const uint64_t offset = static_cast<uint64_t>(streamControlOffset) + 3U;
    if (offset >= hdaMap_->getLength()) return false;
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint8_t observed = *reinterpret_cast<volatile uint8_t *>(base + offset);
    OSSynchronizeIO();
    if (observed == 0xffU) return false;
    *status = observed;
    return true;
}

bool HdaCaptureMmioExecutor::readLinkPosition(
    uint32_t streamControlOffset, uint32_t *positionBytes) const noexcept {
    if (!positionBytes || !deviceOpen() ||
        !validStreamControlOffset(streamControlOffset))
        return false;
    const uint64_t offset = static_cast<uint64_t>(streamControlOffset) + 4U;
    if ((offset & 3U) != 0 || offset > hdaMap_->getLength() ||
        sizeof(uint32_t) > hdaMap_->getLength() - offset)
        return false;
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint32_t observed = *reinterpret_cast<volatile uint32_t *>(base + offset);
    OSSynchronizeIO();
    if (observed == UINT32_MAX) return false;
    *positionBytes = observed;
    return true;
}

bool HdaCaptureMmioExecutor::writeIocAck(
    uint32_t streamControlOffset,
    const HdaCaptureIocAck &ack) const noexcept {
    if (!deviceOpen() || !validStreamControlOffset(streamControlOffset) ||
        ack.offset != streamControlOffset + 3U || ack.widthBytes != 1 ||
        ack.value == 0 || (ack.value & ~kStreamStatusW1cMask) != 0 ||
        ack.reserved[0] != 0 || ack.reserved[1] != 0 ||
        static_cast<uint64_t>(ack.offset) >= hdaMap_->getLength())
        return false;
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    *reinterpret_cast<volatile uint8_t *>(base + ack.offset) = ack.value;
    OSSynchronizeIO();
    return true;
}

bool HdaCaptureMmioExecutor::executeStreamRunAction(
    HdaCaptureTriggerAction action, uint32_t streamControlOffset,
    uint32_t expectedRunningMask, uint32_t expectedRunningValue,
    uint32_t *controlReadback) const noexcept {
    if (!controlReadback || !deviceOpen() ||
        !validStreamControlOffset(streamControlOffset) ||
        (action != HdaCaptureTriggerAction::StartHdaDma &&
         action != HdaCaptureTriggerAction::StopHdaDma) ||
        (expectedRunningMask & kStreamRunMask) == 0 ||
        (expectedRunningValue & kStreamRunMask) == 0 ||
        (expectedRunningValue & ~expectedRunningMask) != 0 ||
        (expectedRunningValue & kStreamResetMask) != 0 ||
        ((expectedRunningValue & kStreamTagMask) == 0))
        return false;

    const uint64_t mapLength = hdaMap_->getLength();
    if (static_cast<uint64_t>(streamControlOffset) > mapLength ||
        sizeof(uint32_t) > mapLength - streamControlOffset)
        return false;
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(
        hdaMap_->getVirtualAddress());
    OSSynchronizeIO();
    const uint32_t before = *reinterpret_cast<volatile uint32_t *>(
        base + streamControlOffset);
    OSSynchronizeIO();
    const uint32_t nonRunMask = expectedRunningMask & ~kStreamRunMask;
    if (before == UINT32_MAX || (before & kStreamResetMask) != 0 ||
        (before & nonRunMask) != (expectedRunningValue & nonRunMask))
        return false;

    const bool start = action == HdaCaptureTriggerAction::StartHdaDma;
    const bool alreadyRunning = (before & kStreamRunMask) != 0;
    if (start == alreadyRunning) return false;
    if (start) {
        const uint64_t statusOffset =
            static_cast<uint64_t>(streamControlOffset) + 3U;
        if (statusOffset >= mapLength) return false;
        OSSynchronizeIO();
        const uint8_t status = *reinterpret_cast<volatile uint8_t *>(
            base + statusOffset);
        OSSynchronizeIO();
        // FIFO-ready is a normal level indication once the DMIC has been
        // primed. Clear pending BCIS before start; never enter RUN with an
        // uncleared completion, error, or unknown status bit.
        const uint8_t disallowedStatus =
            kHdaStreamStatusBufferCompletion |
            kHdaStreamStatusFifoError |
            kHdaStreamStatusDescriptorError;
        if ((status & disallowedStatus) != 0 ||
            (status & ~kHdaStreamStatusKnownMask) != 0)
            return false;
        HdaStreamRegisterWrite write{};
        write.offset = streamControlOffset;
        write.value = before | kStreamRunMask;
        write.widthBytes = 4;
        if (!writeAndReadback(write, controlReadback)) return false;
        for (uint32_t poll = 0; (*controlReadback & kStreamRunMask) == 0 && poll < 1000; ++poll) {
            IODelay(10);
            OSSynchronizeIO();
            *controlReadback = *reinterpret_cast<volatile uint32_t *>(base + streamControlOffset);
            OSSynchronizeIO();
            if (*controlReadback == UINT32_MAX ||
                (*controlReadback & nonRunMask) != (expectedRunningValue & nonRunMask)) return false;
        }
    } else {
        HdaStreamRegisterWrite write{};
        write.offset = streamControlOffset;
        write.value = before & ~kStreamRunMask;
        write.widthBytes = 4;
        if (!writeAndReadback(write, controlReadback)) return false;
        // Intel HD Audio 1.0a specifies that RUN reads back clear only after
        // the DMA engine has stopped and idled (normally within 40 us).
        // Poll with a bounded margin so this readback is the stop fence.
        constexpr uint32_t kStopPollSamples = 1000;
        for (uint32_t poll = 0;
             (*controlReadback & kStreamRunMask) != 0 &&
             poll < kStopPollSamples; ++poll) {
            IODelay(10);
            OSSynchronizeIO();
            *controlReadback = *reinterpret_cast<volatile uint32_t *>(
                base + streamControlOffset);
            OSSynchronizeIO();
            if (*controlReadback == UINT32_MAX) return false;
            if ((*controlReadback & nonRunMask) !=
                (expectedRunningValue & nonRunMask))
                return false;
        }
    }

    const uint32_t expectedAfter = start
        ? expectedRunningValue
        : (expectedRunningValue & ~kStreamRunMask);
    return (*controlReadback & expectedRunningMask) == expectedAfter;
}

} // namespace sof::ipc4::kernel
