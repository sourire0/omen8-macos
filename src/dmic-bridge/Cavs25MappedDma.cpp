#include "Cavs25MappedDma.hpp"

#include <IOKit/IOMapper.h>

namespace sof::ipc4::kernel {
namespace {

constexpr uint64_t kDescriptorPlanBytes = 32;
constexpr uint64_t kAlignment = 128;
constexpr uint32_t kDescriptorAllocationBytes = 4096;

void storeLittle32(uint8_t *destination, uint32_t value) noexcept {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

} // namespace

Cavs25MappedDma::Cavs25MappedDma() noexcept
    : firmwareMemory_(nullptr), descriptorMemory_(nullptr),
      firmwareCommand_(nullptr), descriptorCommand_(nullptr), mapper_(nullptr),
      firmwareBytes_(0),
      failureBuffer_("none"), failureStage_("not started"),
      deviceMapperFound_(false),
      lastSegmentOffset_(0), lastSegmentBytes_(0), lastRequiredBytes_(0),
      lastSegmentCount_(0),
      prepared_(false), firmwareContentsSynchronized_(false),
      descriptorContentsSynchronized_(false), appleHdaBuffer_(false) {
    segment_.fIOVMAddr = 0;
    segment_.fLength = 0;
    evidence_.deviceAddress = 0;
    evidence_.mappedBytes = 0;
    evidence_.descriptorDeviceAddress = 0;
    evidence_.descriptorMappedBytes = 0;
    evidence_.firmwareMappingLive = false;
    evidence_.descriptorMappingLive = false;
    evidence_.cacheVisibilityEstablished = false;
    evidence_.firmwareIsSingleContiguousSegment = false;
    evidence_.firmwareContentsSynchronized = false;
    evidence_.descriptorContentsSynchronized = false;
}

bool Cavs25MappedDma::mapOne(
    IOPCIDevice *device,
    IOBufferMemoryDescriptor **memory,
    IODMACommand **command,
    uint64_t capacity,
    bool requireFullSegment,
    const char *bufferName,
    uint64_t *deviceAddress,
    uint64_t *mappedBytes) noexcept {
    failureBuffer_ = bufferName ? bufferName : "unknown";
    failureStage_ = "mapping arguments";
    if (!device || !memory || !command || capacity == 0 ||
        !deviceAddress || !mappedBytes)
        return false;

    failureStage_ = "IOBufferMemoryDescriptor allocation";
    // The cAVS code-loader descriptor currently represents the firmware as
    // two adjacent-address entries. Ask the allocator for one physically
    // contiguous firmware buffer; the segment length check below remains
    // authoritative and rejects any mapping that does not cover the payload.
    const IOOptionBits memoryOptions = kIODirectionOut |
        (requireFullSegment ? kIOMemoryPhysicallyContiguous : 0U);
    *memory = IOBufferMemoryDescriptor::withOptions(
        memoryOptions, static_cast<vm_size_t>(capacity),
        static_cast<vm_offset_t>(kAlignment));
    if (!*memory) return false;

    failureStage_ = "IODMACommand creation";
    *command = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped, 0,
        static_cast<uint32_t>(kAlignment), mapper_, nullptr);
    if (!*command) return false;

    failureStage_ = "IODMACommand setMemoryDescriptor";
    IOReturn result = (*command)->setMemoryDescriptor(*memory, false);
    if (result != kIOReturnSuccess) return false;
    failureStage_ = "IODMACommand prepare";
    result = (*command)->prepare();
    if (result != kIOReturnSuccess) return false;

    failureStage_ = "IOVM segment generation";
    lastSegmentOffset_ = 0;
    lastSegmentBytes_ = 0;
    lastRequiredBytes_ = requireFullSegment ? capacity : kDescriptorPlanBytes;
    lastSegmentCount_ = 1;
    segment_.fIOVMAddr = 0;
    segment_.fLength = 0;
    UInt64 offset = 0;
    UInt32 segmentCount = 1;
    result = (*command)->gen64IOVMSegments(&offset, &segment_, &segmentCount);
    if (result != kIOReturnSuccess) return false;
    lastSegmentOffset_ = offset;
    lastSegmentBytes_ = segment_.fLength;
    lastSegmentCount_ = segmentCount;
    failureStage_ = "IOVM segment constraints";
    const uint64_t requiredContiguousBytes = lastRequiredBytes_;
    if (segmentCount != 1 || offset < requiredContiguousBytes ||
        segment_.fLength < requiredContiguousBytes)
        return false;
    if (requireFullSegment && segment_.fLength != capacity) return false;
    failureStage_ = "DMA buffer synchronization";
    if ((*command)->synchronize(kIODirectionOut) != kIOReturnSuccess)
        return false;

    *deviceAddress = segment_.fIOVMAddr;
    *mappedBytes = segment_.fLength;
    failureStage_ = "complete";
    return true;
}

bool Cavs25MappedDma::prepare(IOPCIDevice *device, IOService *owner,
                              uint32_t firmwarePayloadBytes) noexcept {
    failureBuffer_ = "none";
    failureStage_ = "prepare preconditions";
    deviceMapperFound_ = false;
    if (prepared_ || firmwareMemory_ || descriptorMemory_ ||
        firmwareCommand_ || descriptorCommand_ || mapper_ || !device || !owner ||
        !device->isOpen(owner) ||
        firmwarePayloadBytes < 256 ||
        (firmwarePayloadBytes & 3U) != 0)
        return false;

    failureStage_ = "IOMapper lookup";
    mapper_ = IOMapper::copyMapperForDevice(device);
    deviceMapperFound_ = mapper_ != nullptr;
    failureStage_ = deviceMapperFound_
        ? "per-device IOMapper selected"
        : "per-device IOMapper absent; defer to IODMACommand system mapper";

    firmwareBytes_ = firmwarePayloadBytes;
    if (!mapOne(device, &firmwareMemory_, &firmwareCommand_, firmwareBytes_,
                true, "SOF firmware", &evidence_.deviceAddress,
                &evidence_.mappedBytes)) {
        releaseMappings();
        return false;
    }
    evidence_.firmwareMappingLive = true;
    evidence_.firmwareIsSingleContiguousSegment = true;

    if (!mapOne(device, &descriptorMemory_, &descriptorCommand_,
                kDescriptorAllocationBytes, false,
                "code-loader BDL",
                &evidence_.descriptorDeviceAddress,
                &evidence_.descriptorMappedBytes)) {
        releaseMappings();
        return false;
    }
    evidence_.descriptorMappingLive = true;
    evidence_.cacheVisibilityEstablished = true;
    firmwareContentsSynchronized_ = false;
    descriptorContentsSynchronized_ = false;
    prepared_ = true;
    failureBuffer_ = "none";
    failureStage_ = "complete";
    return true;
}

bool Cavs25MappedDma::prepareWithAppleHdaBuffer(
    IOPCIDevice *device, IOService *owner,
    IOBufferMemoryDescriptor *cyclicBuffer,
    IODMACommand *cyclicCommand,
    uint32_t firmwarePayloadBytes,
    bool sharedAppleHdaProvider) noexcept {
    failureBuffer_ = "AppleHDA output cyclic buffer";
    failureStage_ = "shared-buffer preconditions";
    deviceMapperFound_ = false;
    if (prepared_ || firmwareMemory_ || descriptorMemory_ ||
        firmwareCommand_ || descriptorCommand_ || mapper_ || !device ||
        !owner || (!device->isOpen(owner) && !sharedAppleHdaProvider) ||
        !cyclicBuffer ||
        firmwarePayloadBytes < 256 || (firmwarePayloadBytes & 3U) != 0 ||
        cyclicBuffer->getLength() < firmwarePayloadBytes ||
        !cyclicBuffer->getBytesNoCopy())
        return false;

    failureStage_ = "AppleHDA output buffer ownership validation";
    if (!cyclicCommand || cyclicCommand->getMemoryDescriptor() != cyclicBuffer)
        return false;

    firmwareMemory_ = cyclicBuffer;
    firmwareMemory_->retain();
    firmwareCommand_ = cyclicCommand;
    firmwareCommand_->retain();

    firmwareBytes_ = firmwarePayloadBytes;
    evidence_.deviceAddress = 0; // AppleHDA owns all BDL addresses.
    evidence_.mappedBytes = firmwarePayloadBytes;
    evidence_.descriptorDeviceAddress = 0;
    evidence_.descriptorMappedBytes = 0;
    evidence_.firmwareMappingLive = true;
    evidence_.descriptorMappingLive = false;
    evidence_.cacheVisibilityEstablished = true;
    evidence_.firmwareIsSingleContiguousSegment = false;
    evidence_.firmwareContentsSynchronized = false;
    evidence_.descriptorContentsSynchronized = false;
    firmwareContentsSynchronized_ = false;
    descriptorContentsSynchronized_ = false;
    appleHdaBuffer_ = true;
    prepared_ = true;
    failureBuffer_ = "none";
    failureStage_ = "AppleHDA output buffer ready; BDL remains AppleHDA-owned";
    return true;
}

bool Cavs25MappedDma::stageFirmwarePayload(const uint8_t *payload,
                                           uint32_t payloadBytes) noexcept {
    failureBuffer_ = "SOF firmware";
    failureStage_ = "payload staging preconditions";
    if (!prepared_ || !payload || payloadBytes != firmwareBytes_ ||
        !firmwareCommand_)
        return false;
    firmwareContentsSynchronized_ = false;
    evidence_.firmwareContentsSynchronized = false;
    failureStage_ = "payload copy";
    if (appleHdaBuffer_) {
        const vm_size_t bufferLength = firmwareMemory_->getLength();
        auto *bytes = static_cast<uint8_t *>(firmwareMemory_->getBytesNoCopy());
        if (!bytes || payloadBytes > bufferLength) return false;
        memcpy(bytes, payload, payloadBytes);
        // AppleHDA's cyclic BDL may wrap after the firmware image. Clear the
        // unused tail so a short ROM read cannot consume stale kernel memory.
        for (vm_size_t offset = payloadBytes; offset < bufferLength; ++offset)
            bytes[offset] = 0;
    } else if (firmwareCommand_->writeBytes(0, payload, payloadBytes) != payloadBytes) {
        return false;
    }
    failureStage_ = "payload synchronization";
    if (firmwareCommand_->synchronize(kIODirectionOut) != kIOReturnSuccess)
        return false;
    firmwareContentsSynchronized_ = true;
    evidence_.firmwareContentsSynchronized = true;
    failureBuffer_ = "none";
    failureStage_ = "complete";
    return true;
}

bool Cavs25MappedDma::buildDescriptorList(
    Cavs25DmaPlan *outPlan) noexcept {
    failureBuffer_ = "code-loader BDL";
    failureStage_ = "BDL construction preconditions";
    if (!prepared_ || appleHdaBuffer_ || !outPlan || !descriptorCommand_ ||
        !descriptorMemory_)
        return false;
    descriptorContentsSynchronized_ = false;
    evidence_.descriptorContentsSynchronized = false;
    failureStage_ = "BDL plan validation";
    if (planCavs25CodeLoaderDma(evidence_, firmwareBytes_, outPlan) !=
        DmaPlanResult::Ok)
        return false;

    failureStage_ = "BDL host mapping";
    uint8_t *bytes = static_cast<uint8_t *>(descriptorMemory_->getBytesNoCopy());
    if (!bytes) return false;
    storeLittle32(bytes + 0, outPlan->entries[0].addressLow);
    storeLittle32(bytes + 4, outPlan->entries[0].addressHigh);
    storeLittle32(bytes + 8, outPlan->entries[0].byteCount);
    storeLittle32(bytes + 12, outPlan->entries[0].interruptOnCompletion);
    storeLittle32(bytes + 16, outPlan->entries[1].addressLow);
    storeLittle32(bytes + 20, outPlan->entries[1].addressHigh);
    storeLittle32(bytes + 24, outPlan->entries[1].byteCount);
    storeLittle32(bytes + 28, outPlan->entries[1].interruptOnCompletion);
    failureStage_ = "BDL synchronization";
    if (descriptorCommand_->synchronize(kIODirectionOut) != kIOReturnSuccess)
        return false;
    descriptorContentsSynchronized_ = true;
    evidence_.descriptorContentsSynchronized = true;
    failureBuffer_ = "none";
    failureStage_ = "complete";
    return true;
}

bool Cavs25MappedDma::releaseAfterQuiescence(
    const Cavs25BootLifecycle &lifecycle) noexcept {
    if (!lifecycle.canReleaseLoaderDma()) return false;
    return releaseMappings();
}

bool Cavs25MappedDma::releaseBeforeHardwareSubmission() noexcept {
    if (!prepared_) return !firmwareCommand_ && !descriptorCommand_ &&
        !firmwareMemory_ && !descriptorMemory_ && !mapper_;
    return releaseMappings();
}

bool Cavs25MappedDma::releaseMappings() noexcept {
    if (firmwareCommand_) {
        // The controller clears its own command when the lease is released.
        if (appleHdaBuffer_ ||
            firmwareCommand_->clearMemoryDescriptor() == kIOReturnSuccess) {
            firmwareCommand_->release();
            firmwareCommand_ = nullptr;
        }
    }
    if (descriptorCommand_) {
        if (descriptorCommand_->clearMemoryDescriptor() == kIOReturnSuccess) {
            descriptorCommand_->release();
            descriptorCommand_ = nullptr;
        }
    }
    if (firmwareMemory_ && !firmwareCommand_) {
        firmwareMemory_->release();
        firmwareMemory_ = nullptr;
    }
    if (descriptorMemory_ && !descriptorCommand_) {
        descriptorMemory_->release();
        descriptorMemory_ = nullptr;
    }
    if (!firmwareCommand_ && !descriptorCommand_ &&
        !firmwareMemory_ && !descriptorMemory_ && mapper_) {
        mapper_->release();
        mapper_ = nullptr;
    }
    evidence_.deviceAddress = 0;
    evidence_.mappedBytes = 0;
    evidence_.descriptorDeviceAddress = 0;
    evidence_.descriptorMappedBytes = 0;
    evidence_.firmwareMappingLive = false;
    evidence_.descriptorMappingLive = false;
    evidence_.cacheVisibilityEstablished = false;
    evidence_.firmwareIsSingleContiguousSegment = false;
    evidence_.firmwareContentsSynchronized = false;
    evidence_.descriptorContentsSynchronized = false;
    firmwareBytes_ = 0;
    prepared_ = false;
    firmwareContentsSynchronized_ = false;
    descriptorContentsSynchronized_ = false;
    appleHdaBuffer_ = false;
    return !firmwareCommand_ && !descriptorCommand_ && !firmwareMemory_ &&
           !descriptorMemory_;
}

const DmaMappingEvidence *Cavs25MappedDma::mappingEvidence() const noexcept {
    return prepared_ && firmwareContentsSynchronized_ &&
                   (appleHdaBuffer_ || descriptorContentsSynchronized_)
        ? &evidence_ : nullptr;
}

} // namespace sof::ipc4::kernel
