#pragma once

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "Cavs25DmaPlan.hpp"
#include "Cavs25BootLifecycle.hpp"

namespace sof::ipc4::kernel {

// Allocates and maps host buffers through the PCI provider's mapper. This
// class deliberately has no BAR pointer, register writer, or DMA start method.
// Call prepare/release from a non-gated driver context; its caller must retain
// the mappings until hardware quiescence has been independently established.
class Cavs25MappedDma final {
public:
    Cavs25MappedDma() noexcept;
    // Intentionally does not unmap: on uncertain hardware state, losing the
    // object must leak/pin the mapping rather than risk DMA use-after-free.
    ~Cavs25MappedDma() = default;

    Cavs25MappedDma(const Cavs25MappedDma &) = delete;
    Cavs25MappedDma &operator=(const Cavs25MappedDma &) = delete;

    bool prepare(IOPCIDevice *device, IOService *owner,
                 uint32_t firmwarePayloadBytes) noexcept;
    // Borrow AppleHDA's output cyclic buffer as the SOF loader source. This
    // maps the existing buffer only for CPU/device synchronization; AppleHDA
    // remains the sole owner of its BDL and HDA stream registers.
    bool prepareWithAppleHdaBuffer(
        IOPCIDevice *device, IOService *owner,
        IOBufferMemoryDescriptor *cyclicBuffer,
        IODMACommand *cyclicCommand,
        uint32_t firmwarePayloadBytes,
        bool sharedAppleHdaProvider) noexcept;
    // Copies into the prepared host buffer and synchronizes it for the device;
    // it does not submit the buffer to hardware.
    bool stageFirmwarePayload(const uint8_t *payload,
                              uint32_t payloadBytes) noexcept;
    // Plans and serializes the two BDLEs into the mapped host descriptor
    // buffer, then synchronizes it. It does not program SD7 or start DMA.
    bool buildDescriptorList(Cavs25DmaPlan *outPlan) noexcept;
    // Releases mappings only while the caller guarantees no descriptor/base
    // register was programmed and the device never received a start command.
    bool releaseBeforeHardwareSubmission() noexcept;
    bool releaseAfterQuiescence(
        const Cavs25BootLifecycle &lifecycle) noexcept;

    const DmaMappingEvidence *mappingEvidence() const noexcept;
    uint32_t firmwarePayloadBytes() const noexcept { return firmwareBytes_; }
    bool deviceMapperFound() const noexcept { return deviceMapperFound_; }
    bool usesAppleHdaBuffer() const noexcept { return appleHdaBuffer_; }
    bool appleHdaBufferReady() const noexcept {
        return prepared_ && appleHdaBuffer_ && firmwareContentsSynchronized_;
    }
    uint64_t lastSegmentOffset() const noexcept { return lastSegmentOffset_; }
    uint64_t lastSegmentBytes() const noexcept { return lastSegmentBytes_; }
    uint64_t lastRequiredBytes() const noexcept { return lastRequiredBytes_; }
    uint32_t lastSegmentCount() const noexcept { return lastSegmentCount_; }
    const char *failureBuffer() const noexcept { return failureBuffer_; }
    const char *failureStage() const noexcept { return failureStage_; }

private:
    bool mapOne(IOPCIDevice *device,
                IOBufferMemoryDescriptor **memory,
                IODMACommand **command,
                uint64_t capacity,
                bool requireFullSegment,
                const char *bufferName,
                uint64_t *deviceAddress,
                uint64_t *mappedBytes) noexcept;
    bool releaseMappings() noexcept;

    IOBufferMemoryDescriptor *firmwareMemory_;
    IOBufferMemoryDescriptor *descriptorMemory_;
    IODMACommand *firmwareCommand_;
    IODMACommand *descriptorCommand_;
    IOMapper *mapper_;
    IODMACommand::Segment64 segment_;
    DmaMappingEvidence evidence_;
    uint32_t firmwareBytes_;
    const char *failureBuffer_;
    const char *failureStage_;
    bool deviceMapperFound_;
    uint64_t lastSegmentOffset_;
    uint64_t lastSegmentBytes_;
    uint64_t lastRequiredBytes_;
    uint32_t lastSegmentCount_;
    bool prepared_;
    bool firmwareContentsSynchronized_;
    bool descriptorContentsSynchronized_;
    bool appleHdaBuffer_;
};

} // namespace sof::ipc4::kernel
