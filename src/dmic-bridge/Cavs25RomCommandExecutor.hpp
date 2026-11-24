#pragma once

#include "Cavs25BootLifecycle.hpp"
#include "Cavs25RomCoreStartPlan.hpp"
#include "Cavs25RomPurgePlan.hpp"

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace sof::ipc4::kernel {

enum class RomCommandExecutionResult : uint8_t {
    Ok,
    InvalidArgument,
    WrongPhase,
    DeviceUnavailable,
    UnsafeMapping,
    InvalidReadback,
    PreconditionFailed,
    WriteFailed,
    ReadbackMismatch,
};

// Explicit, readback-checked BAR4 writes for the ROM PURGE and Core 0 start
// stages. The owner must serialize calls with the boot lifecycle. This
// executor is inert until a caller invokes a method; it does not load firmware,
// touch HDA DMA, or start a code-loader stream.
class Cavs25RomCommandExecutor final {
public:
    Cavs25RomCommandExecutor(
        IOPCIDevice *device, IOService *owner, IOMemoryMap *dspMap,
        const Cavs25BootLifecycle *lifecycle,
        bool mappingCreatedWritable,
        bool sharedAppleHdaProvider = false) noexcept;

    RomCommandExecutionResult submitPurgeRequest(
        uint8_t codeLoaderStreamTag, uint32_t *doorbellReadback) const noexcept;
    RomCommandExecutionResult executeCoreStartStep(
        const Cavs25AdspcsStep *step,
        uint32_t *adspcsReadback) const noexcept;
    RomCommandExecutionResult requestCorePower(
        uint32_t coreMask, bool powerOn,
        uint32_t *adspcsReadback) const noexcept;
    bool readAdspcs(uint32_t *adspcsReadback) const noexcept;
    RomCommandExecutionResult acknowledgePurge(
        RomPurgePollResult pollResult,
        uint32_t *ackReadback) const noexcept;

private:
    bool deviceOpen() const noexcept;
    bool writableMap() const noexcept;
    bool validRegister(uint32_t offset) const noexcept;
    bool readRegister(uint32_t offset, uint32_t *value) const noexcept;
    bool writeRegister(uint32_t offset, uint32_t value) const noexcept;

    IOPCIDevice *device_;
    IOService *owner_;
    IOMemoryMap *dspMap_;
    const Cavs25BootLifecycle *lifecycle_;
    bool mappingCreatedWritable_;
    bool sharedAppleHdaProvider_;
};

} // namespace sof::ipc4::kernel
