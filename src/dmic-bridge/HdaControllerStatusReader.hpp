#pragma once

#include "HdaInputStreamAllocatorPlan.hpp"

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace sof::ipc4::kernel {

struct HdaControllerStatusSnapshot {
    uint16_t globalCapabilities;
    uint16_t lowerLinkCapabilitiesHeader;
    uint32_t globalControl;
    uint8_t inputStreamCount;
    uint8_t outputStreamCount;
    uint8_t bidirectionalStreamCount;
    uint8_t totalStreamCount;
    uint16_t allocatedStreamTagMask;
    HdaStreamDescriptorSnapshot inputStreams[kHdaMaximumStreamDescriptors];
    HdaStreamDescriptorSnapshot allStreamControls[
        kHdaMaximumTotalStreamCount];
    uint32_t ppCapabilityOffset;
    uint32_t ppCapabilityHeader;
    uint32_t ppctl;
    uint32_t intctl;
    uint32_t spibCapabilityOffset;
    uint32_t spibCapabilityHeader;
    uint32_t spbfcctl;
    uint32_t spibPositions[kHdaMaximumTotalStreamCount];
    bool spibCapabilityPresent;
};

// Read-only HDA BAR snapshot adapter. It walks the bounded LLCH chain and
// reads stream tags, PPCTL, INTCTL, and optional SPIB state only. It has no
// write or DMA API.
class HdaControllerStatusReader final {
public:
    HdaControllerStatusReader(IOPCIDevice *device, IOService *owner,
                              IOMemoryMap *hdaMap,
                              bool sharedReadOnlyLease = false) noexcept;

    bool readSnapshot(HdaControllerStatusSnapshot *snapshot) const noexcept;

private:
    bool deviceOpen() const noexcept;
    bool validMapping(uint32_t offset, uint32_t length) const noexcept;

    IOPCIDevice *device_;
    IOService *owner_;
    IOMemoryMap *hdaMap_;
    bool sharedReadOnlyLease_;
};

} // namespace sof::ipc4::kernel
