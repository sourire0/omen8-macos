#pragma once

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "HdaCaptureBdlPlan.hpp"
#include "HdaCapturePositionPlan.hpp"

namespace sof::ipc4::kernel {

enum class HdaCaptureDmaReadResult : uint8_t {
    PeriodRead,
    NoPeriodReady,
    InvalidArgument,
    InvalidState,
    PositionFailure,
    ReadFailure,
};

// Owns capture-ring (device writes host memory) and BDL (device reads host
// memory) mappings. It has no BAR/register interface and never starts DMA.
// The owner must call from a non-gated context; mappings stay pinned until a
// controller has confirmed stream quiescence and releaseAfterQuiescence runs.
class HdaCaptureMappedDma final {
public:
    static constexpr uint32_t kMaximumSegments = 256;

    HdaCaptureMappedDma() noexcept;
    ~HdaCaptureMappedDma() = default; // uncertain mappings intentionally pin

    HdaCaptureMappedDma(const HdaCaptureMappedDma &) = delete;
    HdaCaptureMappedDma &operator=(const HdaCaptureMappedDma &) = delete;

    bool prepare(IOPCIDevice *device, IOService *owner,
                 uint32_t ringBytes, uint32_t periodBytes,
                 bool splitAt4KiBBoundaries) noexcept;
    // Uses AppleHDA's already allocated cyclic buffer and maps it only for
    // synchronization/CPU reads. AppleHDA remains owner of the stream BDL.
    bool prepareWithAppleHdaBuffer(IOPCIDevice *device, IOService *owner,
                                  IOBufferMemoryDescriptor *cyclicBuffer,
                                  uint32_t ringBytes,
                                   uint32_t periodBytes,
                                   bool sharedAppleHdaProvider) noexcept;
    // Copies one completed IOC period after DMA synchronization. The tracker
    // must have observed matching LPIB and monotonic IOC evidence first.
    HdaCaptureDmaReadResult readNextCompletedPeriod(
        HdaCapturePositionTracker *positionTracker,
        uint8_t *destination) noexcept;
    bool readCapturedBytes(uint32_t offset, uint8_t *destination,
                           uint32_t byteCount) noexcept;

    // Call before submitting a stream RUN command. A later readback must show
    // RUN clear and the controller's drain/quiescence fence before release.
    bool markStreamMayBeActive() noexcept;
    bool confirmStreamQuiescent(uint32_t controlReadback,
                                bool drainFenceObserved) noexcept;
    bool releaseAfterQuiescence() noexcept;

    bool prepared() const noexcept { return prepared_; }
    bool usesAppleHdaBuffer() const noexcept {
        return prepared_ && ringCommand_ && ringMemory_ && !bdlCommand_ &&
               !bdlMemory_;
    }
    bool quiescentForRelease() const noexcept {
        return !streamMayBeActive_ && quiescenceConfirmed_;
    }
    uint32_t ringBytes() const noexcept { return ringBytes_; }
    bool deviceMapperFound() const noexcept { return deviceMapperFound_; }
    const char *failureBuffer() const noexcept { return failureBuffer_; }
    const char *failureStage() const noexcept { return failureStage_; }
    uint64_t lastSegmentOffset() const noexcept { return lastSegmentOffset_; }
    uint64_t lastSegmentBytes() const noexcept { return lastSegmentBytes_; }
    uint64_t lastRequiredBytes() const noexcept { return lastRequiredBytes_; }
    uint32_t lastSegmentCount() const noexcept { return lastSegmentCount_; }
    const char *lastReadStage() const noexcept { return lastReadStage_; }
    uint32_t lastReadPlanResult() const noexcept { return lastReadPlanResult_; }
    uint64_t lastReadOffset() const noexcept { return lastReadOffset_; }
    uint32_t lastReadFirstLength() const noexcept { return lastReadFirstLength_; }
    uint32_t lastReadSecondLength() const noexcept { return lastReadSecondLength_; }
    uint32_t lastReadSyncResult() const noexcept { return lastReadSyncResult_; }
    uint64_t lastReadFirstBytes() const noexcept { return lastReadFirstBytes_; }
    uint64_t lastReadSecondBytes() const noexcept { return lastReadSecondBytes_; }
    uint32_t lastReadCompletionResult() const noexcept { return lastReadCompletionResult_; }
    const HdaCaptureBdlPlan *bdlPlan() const noexcept {
        return prepared_ ? &bdlPlan_ : nullptr;
    }

private:
    bool mapRing(IOPCIDevice *device) noexcept;
    bool mapBdl(IOPCIDevice *device) noexcept;
    bool releaseMappings() noexcept;

    IOBufferMemoryDescriptor *ringMemory_;
    IOBufferMemoryDescriptor *bdlMemory_;
    IODMACommand *ringCommand_;
    IODMACommand *bdlCommand_;
    IOMapper *mapper_;
    HdaCaptureDmaSegment segments_[kMaximumSegments];
    HdaCaptureBdlEntry entries_[kMaximumSegments];
    HdaCaptureBdlPlan bdlPlan_;
    uint32_t ringBytes_;
    uint32_t periodBytes_;
    uint32_t segmentCount_;
    uint64_t bdlDeviceAddress_;
    uint64_t bdlMappedBytes_;
    bool splitAt4KiBBoundaries_;
    bool deviceMapperFound_;
    const char *failureBuffer_;
    const char *failureStage_;
    uint64_t lastSegmentOffset_;
    uint64_t lastSegmentBytes_;
    uint64_t lastRequiredBytes_;
    uint32_t lastSegmentCount_;
    const char *lastReadStage_;
    uint32_t lastReadPlanResult_;
    uint64_t lastReadOffset_;
    uint32_t lastReadFirstLength_;
    uint32_t lastReadSecondLength_;
    uint32_t lastReadSyncResult_;
    uint64_t lastReadFirstBytes_;
    uint64_t lastReadSecondBytes_;
    uint32_t lastReadCompletionResult_;
    bool prepared_;
    bool streamMayBeActive_;
    bool quiescenceConfirmed_;
};

} // namespace sof::ipc4::kernel
