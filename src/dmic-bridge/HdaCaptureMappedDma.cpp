#include "HdaCaptureMappedDma.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kBdlAllocationBytes = 4096;
constexpr uint32_t kDmaAlignment = 128;
constexpr uint32_t kStreamRunMask = 1U << 1;

} // namespace

HdaCaptureMappedDma::HdaCaptureMappedDma() noexcept
    : ringMemory_(nullptr), bdlMemory_(nullptr), ringCommand_(nullptr),
      bdlCommand_(nullptr), mapper_(nullptr), bdlPlan_{}, ringBytes_(0),
      periodBytes_(0), segmentCount_(0), bdlDeviceAddress_(0),
      bdlMappedBytes_(0), splitAt4KiBBoundaries_(false),
      deviceMapperFound_(false), failureBuffer_("none"),
      failureStage_("not started"), lastSegmentOffset_(0),
      lastSegmentBytes_(0), lastRequiredBytes_(0), lastSegmentCount_(0),
      lastReadStage_("not started"), lastReadPlanResult_(UINT32_MAX),
      lastReadOffset_(UINT64_MAX), lastReadFirstLength_(0),
      lastReadSecondLength_(0), lastReadSyncResult_(UINT32_MAX),
      lastReadFirstBytes_(UINT64_MAX), lastReadSecondBytes_(UINT64_MAX),
      lastReadCompletionResult_(UINT32_MAX),
      prepared_(false), streamMayBeActive_(false),
      quiescenceConfirmed_(true) {}

bool HdaCaptureMappedDma::prepare(IOPCIDevice *device, IOService *owner,
                                  uint32_t ringBytes, uint32_t periodBytes,
                                  bool splitAt4KiBBoundaries) noexcept {
    failureBuffer_ = "none";
    failureStage_ = "prepare preconditions";
    deviceMapperFound_ = false;
    lastSegmentOffset_ = 0;
    lastSegmentBytes_ = 0;
    lastRequiredBytes_ = 0;
    lastSegmentCount_ = 0;
    if (prepared_ || ringMemory_ || bdlMemory_ || ringCommand_ ||
        bdlCommand_ || mapper_ || !device || !owner ||
        !device->isOpen(owner) || ringBytes == 0 || periodBytes == 0 ||
        (ringBytes % periodBytes) != 0)
        return false;

    failureStage_ = "IOMapper lookup";
    mapper_ = IOMapper::copyMapperForDevice(device);
    deviceMapperFound_ = mapper_ != nullptr;
    ringBytes_ = ringBytes;
    periodBytes_ = periodBytes;
    splitAt4KiBBoundaries_ = splitAt4KiBBoundaries;

    if (!mapRing(device) || !mapBdl(device)) {
        releaseMappings();
        return false;
    }

    const HdaCaptureRingMapping ringMapping = {
        segments_, segmentCount_, ringBytes_, true};
    const HdaCaptureBdlMapping bdlMapping = {
        bdlDeviceAddress_, bdlMappedBytes_, true};
    if (planHdaCaptureBdl(ringMapping, bdlMapping, periodBytes_,
                          splitAt4KiBBoundaries_, entries_, kMaximumSegments,
                          &bdlPlan_) != HdaBdlPlanResult::Ok) {
        failureBuffer_ = "capture BDL";
        failureStage_ = "BDL plan validation";
        releaseMappings();
        return false;
    }

    uint8_t *bdlBytes = static_cast<uint8_t *>(bdlMemory_->getBytesNoCopy());
    if (!bdlBytes ||
        serializeHdaCaptureBdl(bdlPlan_, entries_, bdlBytes,
                               kBdlAllocationBytes) != HdaBdlPlanResult::Ok) {
        failureBuffer_ = "capture BDL";
        failureStage_ = "BDL serialization";
        releaseMappings();
        return false;
    }
    failureBuffer_ = "capture BDL";
    failureStage_ = "BDL synchronization";
    if (bdlCommand_->synchronize(kIODirectionOut) != kIOReturnSuccess) {
        releaseMappings();
        return false;
    }
    prepared_ = true;
    streamMayBeActive_ = false;
    quiescenceConfirmed_ = true;
    failureBuffer_ = "none";
    failureStage_ = "complete";
    return true;
}

bool HdaCaptureMappedDma::prepareWithAppleHdaBuffer(
    IOPCIDevice *device, IOService *owner,
    IOBufferMemoryDescriptor *cyclicBuffer, uint32_t ringBytes,
    uint32_t periodBytes, bool sharedAppleHdaProvider) noexcept {
    failureBuffer_ = "AppleHDA cyclic buffer";
    failureStage_ = "shared-buffer preconditions";
    deviceMapperFound_ = false;
    lastSegmentOffset_ = 0;
    lastSegmentBytes_ = 0;
    lastRequiredBytes_ = ringBytes;
    lastSegmentCount_ = 0;
    if (prepared_ || ringMemory_ || bdlMemory_ || ringCommand_ ||
        bdlCommand_ || mapper_ || !device || !owner || !cyclicBuffer ||
        (!device->isOpen(owner) && !sharedAppleHdaProvider) ||
        ringBytes == 0 || periodBytes == 0 ||
        ringBytes % periodBytes != 0 ||
        cyclicBuffer->getLength() < ringBytes ||
        !cyclicBuffer->getBytesNoCopy())
        return false;

    failureStage_ = "AppleHDA buffer IOMapper lookup";
    mapper_ = IOMapper::copyMapperForDevice(device);
    deviceMapperFound_ = mapper_ != nullptr;

    failureStage_ = "AppleHDA buffer DMA sync command creation";
    ringCommand_ = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped,
        ringBytes, kDmaAlignment, mapper_, nullptr);
    if (!ringCommand_) {
        releaseMappings();
        return false;
    }
    ringMemory_ = cyclicBuffer;
    ringMemory_->retain();
    failureStage_ = "AppleHDA buffer DMA sync mapping";
    IOReturn result = ringCommand_->setMemoryDescriptor(ringMemory_, false);
    if (result != kIOReturnSuccess) {
        releaseMappings();
        return false;
    }
    result = ringCommand_->prepare();
    if (result != kIOReturnSuccess) {
        releaseMappings();
        return false;
    }

    // This synthetic geometry is intentionally limited to the position
    // tracker contract. No BDL is created or programmed by this class.
    bdlPlan_ = {};
    bdlPlan_.cyclicBufferLength = ringBytes;
    bdlPlan_.periodLength = periodBytes;
    bdlPlan_.entryCount = static_cast<uint16_t>(ringBytes / periodBytes);
    ringBytes_ = ringBytes;
    periodBytes_ = periodBytes;
    splitAt4KiBBoundaries_ = false;
    segmentCount_ = 0;
    prepared_ = true;
    streamMayBeActive_ = false;
    quiescenceConfirmed_ = true;
    failureBuffer_ = "none";
    failureStage_ = "AppleHDA buffer ready";
    return true;
}

bool HdaCaptureMappedDma::mapRing(IOPCIDevice *device) noexcept {
    (void)device;
    failureBuffer_ = "capture ring";
    failureStage_ = "IOBufferMemoryDescriptor allocation";
    lastSegmentOffset_ = 0;
    lastSegmentBytes_ = 0;
    lastRequiredBytes_ = ringBytes_;
    lastSegmentCount_ = 0;
    ringMemory_ = IOBufferMemoryDescriptor::withOptions(
        kIODirectionIn, static_cast<vm_size_t>(ringBytes_), kDmaAlignment);
    if (!ringMemory_) return false;
    ringCommand_ = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped,
        ringBytes_, kDmaAlignment, mapper_, nullptr);
    if (!ringCommand_) {
        failureStage_ = "IODMACommand creation";
        return false;
    }
    failureStage_ = "IODMACommand setMemoryDescriptor";
    IOReturn result = ringCommand_->setMemoryDescriptor(ringMemory_, false);
    if (result != kIOReturnSuccess) return false;
    failureStage_ = "IODMACommand prepare";
    result = ringCommand_->prepare();
    if (result != kIOReturnSuccess) return false;

    failureStage_ = "IOVM segment generation";
    IODMACommand::Segment64 mapped[kMaximumSegments];
    UInt64 offset = 0;
    UInt32 count = kMaximumSegments;
    result = ringCommand_->gen64IOVMSegments(&offset, mapped, &count);
    lastSegmentOffset_ = offset;
    lastSegmentCount_ = count;
    if (result != kIOReturnSuccess) return false;
    if (count == 0 || count > kMaximumSegments || offset != ringBytes_) {
        failureStage_ = "IOVM segment constraints";
        return false;
    }

    uint64_t covered = 0;
    for (UInt32 i = 0; i < count; ++i) {
        segments_[i].deviceAddress = mapped[i].fIOVMAddr;
        segments_[i].byteLength = mapped[i].fLength;
        if (segments_[i].byteLength == 0 ||
            covered > UINT64_MAX - segments_[i].byteLength) {
            failureStage_ = "IOVM segment constraints";
            return false;
        }
        covered += segments_[i].byteLength;
    }
    lastSegmentBytes_ = covered;
    if (covered != ringBytes_) {
        failureStage_ = "IOVM segment constraints";
        return false;
    }
    segmentCount_ = count;
    failureStage_ = "complete";
    return true;
}

bool HdaCaptureMappedDma::mapBdl(IOPCIDevice *device) noexcept {
    (void)device;
    failureBuffer_ = "capture BDL";
    failureStage_ = "IOBufferMemoryDescriptor allocation";
    lastSegmentOffset_ = 0;
    lastSegmentBytes_ = 0;
    lastRequiredBytes_ = kBdlAllocationBytes;
    lastSegmentCount_ = 1;
    bdlMemory_ = IOBufferMemoryDescriptor::withOptions(
        kIODirectionOut, kBdlAllocationBytes, kDmaAlignment);
    if (!bdlMemory_) return false;
    bdlCommand_ = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped,
        kBdlAllocationBytes, kDmaAlignment, mapper_, nullptr);
    if (!bdlCommand_) {
        failureStage_ = "IODMACommand creation";
        return false;
    }
    failureStage_ = "IODMACommand setMemoryDescriptor";
    IOReturn result = bdlCommand_->setMemoryDescriptor(bdlMemory_, false);
    if (result != kIOReturnSuccess) return false;
    failureStage_ = "IODMACommand prepare";
    result = bdlCommand_->prepare();
    if (result != kIOReturnSuccess) return false;
    failureStage_ = "IOVM segment generation";
    IODMACommand::Segment64 segment{};
    UInt64 offset = 0;
    UInt32 count = 1;
    result = bdlCommand_->gen64IOVMSegments(&offset, &segment, &count);
    lastSegmentOffset_ = offset;
    lastSegmentBytes_ = segment.fLength;
    lastSegmentCount_ = count;
    if (result != kIOReturnSuccess) return false;
    if (count != 1 || offset != kBdlAllocationBytes ||
        segment.fLength < kBdlAllocationBytes) {
        failureStage_ = "IOVM segment constraints";
        return false;
    }
    bdlDeviceAddress_ = segment.fIOVMAddr;
    bdlMappedBytes_ = segment.fLength;
    failureStage_ = "complete";
    return true;
}

bool HdaCaptureMappedDma::readCapturedBytes(uint32_t offset,
                                            uint8_t *destination,
                                            uint32_t byteCount) noexcept {
    if (!prepared_ || streamMayBeActive_ || !destination || byteCount == 0 ||
        offset > ringBytes_ || byteCount > ringBytes_ - offset ||
        !ringCommand_)
        return false;
    if (ringCommand_->synchronize(kIODirectionIn) != kIOReturnSuccess)
        return false;
    return ringCommand_->readBytes(offset, destination, byteCount) == byteCount;
}

HdaCaptureDmaReadResult HdaCaptureMappedDma::readNextCompletedPeriod(
    HdaCapturePositionTracker *positionTracker,
    uint8_t *destination) noexcept {
    lastReadStage_ = "preconditions";
    lastReadPlanResult_ = UINT32_MAX;
    lastReadOffset_ = UINT64_MAX;
    lastReadFirstLength_ = 0;
    lastReadSecondLength_ = 0;
    lastReadSyncResult_ = UINT32_MAX;
    lastReadFirstBytes_ = UINT64_MAX;
    lastReadSecondBytes_ = UINT64_MAX;
    lastReadCompletionResult_ = UINT32_MAX;
    if (!positionTracker || !destination)
        return HdaCaptureDmaReadResult::InvalidArgument;
    if (!prepared_ || !ringCommand_ || !ringMemory_ ||
        positionTracker->ringBytes() != ringBytes_ ||
        positionTracker->periodBytes() != periodBytes_ ||
        positionTracker->failed())
        return HdaCaptureDmaReadResult::InvalidState;

    HdaCapturePeriodReadPlan plan{};
    const HdaCapturePositionResult planResult =
        positionTracker->nextPeriod(&plan);
    lastReadPlanResult_ = static_cast<uint32_t>(planResult);
    lastReadStage_ = "period plan";
    if (planResult == HdaCapturePositionResult::NoCompletePeriod)
        return HdaCaptureDmaReadResult::NoPeriodReady;
    if (planResult != HdaCapturePositionResult::Ready)
        return HdaCaptureDmaReadResult::PositionFailure;
    if (plan.firstLength == 0 ||
        plan.firstOffset > ringBytes_ ||
        plan.firstLength > ringBytes_ - plan.firstOffset ||
        plan.firstLength > periodBytes_ ||
        plan.secondLength > periodBytes_ - plan.firstLength ||
        plan.firstLength + plan.secondLength != periodBytes_) {
        (void)positionTracker->completeRead(plan.sequence, false);
        return HdaCaptureDmaReadResult::PositionFailure;
    }
    lastReadOffset_ = plan.firstOffset;
    lastReadFirstLength_ = plan.firstLength;
    lastReadSecondLength_ = plan.secondLength;

    lastReadStage_ = "DMA synchronize for CPU";
    const IOReturn syncResult = ringCommand_->synchronize(kIODirectionIn);
    lastReadSyncResult_ = static_cast<uint32_t>(syncResult);
    auto *ringBytes = static_cast<uint8_t *>(ringMemory_->getBytesNoCopy());
    bool readSucceeded = syncResult == kIOReturnSuccess && ringBytes != nullptr;
    if (readSucceeded) {
        lastReadStage_ = "first period copy";
        bcopy(ringBytes + plan.firstOffset, destination, plan.firstLength);
        lastReadFirstBytes_ = plan.firstLength;
    }
    if (readSucceeded && plan.secondLength != 0) {
        lastReadStage_ = "wrapped period copy";
        bcopy(ringBytes, destination + plan.firstLength, plan.secondLength);
        lastReadSecondBytes_ = plan.secondLength;
    }
    if (readSucceeded) lastReadStage_ = "position commit";
    const HdaCapturePositionResult completion =
        positionTracker->completeRead(plan.sequence, readSucceeded);
    lastReadCompletionResult_ = static_cast<uint32_t>(completion);
    if (!readSucceeded || completion != HdaCapturePositionResult::Ready)
        return HdaCaptureDmaReadResult::ReadFailure;
    lastReadStage_ = "complete";
    return HdaCaptureDmaReadResult::PeriodRead;
}

bool HdaCaptureMappedDma::markStreamMayBeActive() noexcept {
    if (!prepared_) return false;
    streamMayBeActive_ = true;
    quiescenceConfirmed_ = false;
    return true;
}

bool HdaCaptureMappedDma::confirmStreamQuiescent(
    uint32_t controlReadback, bool drainFenceObserved) noexcept {
    if (!prepared_ ||
        (controlReadback & kStreamRunMask) != 0 || !drainFenceObserved)
        return false;
    if (!streamMayBeActive_) return quiescenceConfirmed_;
    streamMayBeActive_ = false;
    quiescenceConfirmed_ = true;
    return true;
}

bool HdaCaptureMappedDma::releaseAfterQuiescence() noexcept {
    if (streamMayBeActive_ || !quiescenceConfirmed_) return false;
    const bool released = releaseMappings();
    if (released) prepared_ = false;
    return released;
}

bool HdaCaptureMappedDma::releaseMappings() noexcept {
    if (ringCommand_ &&
        ringCommand_->clearMemoryDescriptor() == kIOReturnSuccess) {
        ringCommand_->release();
        ringCommand_ = nullptr;
    }
    if (bdlCommand_ &&
        bdlCommand_->clearMemoryDescriptor() == kIOReturnSuccess) {
        bdlCommand_->release();
        bdlCommand_ = nullptr;
    }
    if (ringMemory_ && !ringCommand_) {
        ringMemory_->release();
        ringMemory_ = nullptr;
    }
    if (bdlMemory_ && !bdlCommand_) {
        bdlMemory_->release();
        bdlMemory_ = nullptr;
    }
    if (!ringCommand_ && !bdlCommand_ && !ringMemory_ && !bdlMemory_ &&
        mapper_) {
        mapper_->release();
        mapper_ = nullptr;
    }
    if (!ringCommand_ && !bdlCommand_ && !ringMemory_ && !bdlMemory_) {
        ringBytes_ = 0;
        periodBytes_ = 0;
        segmentCount_ = 0;
        bdlDeviceAddress_ = 0;
        bdlMappedBytes_ = 0;
        bdlPlan_ = {};
    }
    return !ringCommand_ && !bdlCommand_ && !ringMemory_ && !bdlMemory_ &&
           !mapper_;
}

} // namespace sof::ipc4::kernel
