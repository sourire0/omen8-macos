#pragma once

#include "Cavs25BootLifecycle.hpp"
#include "Ipc4KernelTransport.hpp"
#include "Ipc4MmioPolicy.hpp"

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace sof::ipc4::kernel {

// Bounded IPC-only callback adapter over the controller's DSP BAR mapping.
// The caller owns the mapping and lifecycle and must serialize calls. It has
// no API for HDA stream registers, ROM boot registers, or DMA control.
class Ipc4MmioAdapter final {
public:
    using IncomingHandler = bool (*)(void *context,
                                     const HeaderFields *header);

    Ipc4MmioAdapter() noexcept;
    Ipc4MmioAdapter(IOPCIDevice *pciDevice, IOService *owner,
                    IOMemoryMap *dspMap,
                    const Cavs25BootLifecycle *lifecycle,
                    bool mappingCreatedWritable,
                    IOService *sharedController,
                    IncomingHandler incomingHandler,
                    void *incomingContext) noexcept;

    bool configure(IOPCIDevice *pciDevice, IOService *owner,
                   IOMemoryMap *dspMap,
                   const Cavs25BootLifecycle *lifecycle,
                   bool mappingCreatedWritable,
                   IOService *sharedController,
                   IncomingHandler incomingHandler,
                   void *incomingContext) noexcept;
    void reset() noexcept;

    IoOps ioOps() noexcept;

    // Pure policy helpers, exposed for offline review and simulation.
    static bool supportedRegister(uint16_t offset) noexcept;
    static bool allowedRegisterWrite(uint16_t offset, uint32_t value,
                                     bool firmwareReady) noexcept;
    static bool validMailboxRange(uint64_t mapLength, uint32_t offset,
                                  size_t length, bool writeToDevice) noexcept;

private:
    bool deviceOpen() const noexcept;
    static bool readRegister(void *context, uint16_t offset,
                             uint32_t *value) noexcept;
    static bool writeRegister(void *context, uint16_t offset,
                              uint32_t value) noexcept;
    static bool writeMailbox(void *context, uint32_t offset,
                             const uint8_t *bytes, size_t length) noexcept;
    static bool readMailbox(void *context, uint32_t offset,
                            uint8_t *bytes, size_t length) noexcept;
    static void delayMicroseconds(void *context, uint32_t delay) noexcept;
    static bool incomingMessage(void *context,
                                const HeaderFields *header) noexcept;

    IOPCIDevice *pciDevice_;
    IOService *owner_;
    IOMemoryMap *dspMap_;
    const Cavs25BootLifecycle *lifecycle_;
    bool mappingCreatedWritable_;
    IOService *sharedController_;
    IncomingHandler incomingHandler_;
    void *incomingContext_;
};

} // namespace sof::ipc4::kernel
