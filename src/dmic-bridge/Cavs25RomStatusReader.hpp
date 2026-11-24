#pragma once

#include "Cavs25RomStatusPlan.hpp"
#include "Cavs25RomPurgePlan.hpp"

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace sof::ipc4::kernel {

// Read-only adapter for the cAVS FSR and aliased ROM_ERROR/FW_STATUS words.
// It has no register write, interrupt, power, or DMA-start operation.
class Cavs25RomStatusReader final {
public:
    Cavs25RomStatusReader(IOPCIDevice *device, IOService *owner,
                          IOMemoryMap *dspMap,
                          bool sharedReadOnlyLease = false) noexcept;

    bool readSnapshot(uint32_t *status, uint32_t *errorOrFirmwareStatus) const
        noexcept;
    bool readPurgeAck(uint32_t *ack, uint32_t *romError) const noexcept;
    bool readPurgeAndCoreState(uint32_t *hostRequest,
                               uint32_t *adspcs) const noexcept;
    bool observe(Cavs25RomStatusPlan *plan, RomPollResult *result) const
        noexcept;
    bool observePurgeAck(Cavs25RomPurgePlan *plan,
                         RomPurgePollResult *result) const noexcept;

private:
    bool deviceOpen() const noexcept;
    bool validMapping(uint32_t offset, uint32_t length) const noexcept;

    IOPCIDevice *device_;
    IOService *owner_;
    IOMemoryMap *dspMap_;
    bool sharedReadOnlyLease_;
};

} // namespace sof::ipc4::kernel
