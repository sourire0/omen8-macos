#pragma once

#include "DmicKernelOperationPlan.hpp"
#include "KernelIdPool.hpp"

namespace sof::ipc4::dmic::kernel {

enum class RuntimeIdAllocatorResult : uint8_t {
    Ok,
    InvalidArgument,
    InvalidState,
    WrongManifest,
    InvalidMaximum,
    Exhausted,
    DuplicateModule,
    NotAllocated,
    TeardownUnconfirmed,
};

// Driver-owned ID leases for one live IPC4 firmware session. Serialize every
// method under the driver's pipeline-state lock. Pipeline capacity must come
// from the validated FW_CONFIG response, never from a topology template.
class RuntimeIdAllocator final {
public:
    RuntimeIdAllocator() noexcept;

    RuntimeIdAllocatorResult initialize(
        const sof::ipc4::kernel::FirmwareManifestView *manifest,
        uint16_t maxPipelineIdInclusive) noexcept;
    RuntimeIdAllocatorResult allocate(RuntimeIds *outIds) noexcept;
    RuntimeIdAllocatorResult release(const RuntimeIds &ids,
                                     bool teardownConfirmed) noexcept;
    void resetAfterFirmwareReset() noexcept;
    bool initialized() const noexcept { return initialized_; }
    bool hasActiveLease() const noexcept { return leaseActive_; }
    uint16_t maxPipelineIdInclusive() const noexcept {
        return pipelines_.maxInclusive();
    }

private:
    sof::ipc4::kernel::IdPool pipelines_;
    sof::ipc4::kernel::IdPool copiers_;
    sof::ipc4::kernel::IdPool gains_;
    sof::ipc4::kernel::IdPool eqiirs_;
    sof::ipc4::kernel::IdPool tdfbs_;
    sof::ipc4::kernel::IdPool drcs_;
    RuntimeIds leased_;
    bool initialized_;
    bool leaseActive_;
};

} // namespace sof::ipc4::dmic::kernel
