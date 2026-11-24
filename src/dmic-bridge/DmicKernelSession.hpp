#pragma once

#include "DmicKernelOperationRunner.hpp"
#include "KernelRuntimeIdAllocator.hpp"

namespace sof::ipc4::dmic::kernel {

// One capture-pipeline lifetime. This object and allocator are driver-owned,
// never stack allocated. The caller serializes methods under the pipeline
// lock. Initialization requires firmware READY and sends FW_CONFIG only; it
// never loads firmware or starts an HDA DMA stream.
class Session final {
public:
    Session() noexcept;

    RunnerResult initializeFromFirmwareConfig(
        const sof::ipc4::kernel::FirmwareManifestView *manifest,
        const sof::ipc4::kernel::HdaDmicCapturePlan *capturePlan,
        const sof::ipc4::kernel::Cavs25BootLifecycle *lifecycle,
        sof::ipc4::kernel::Ipc4Transport *transport,
        RuntimeIdAllocator *idAllocator,
        uint32_t timeoutMilliseconds,
        uint32_t pollIntervalMicroseconds,
        sof::ipc4::kernel::FirmwareConfigExchangeResult *configResult,
        sof::ipc4::kernel::TransportResult *transportResult,
        sof::ipc4::kernel::FirmwareConfigResult *replyResult) noexcept;

    RunnerResult setupNext(OperationKind *kind,
                           sof::ipc4::kernel::TransportResult *transportResult,
                           uint32_t timeoutMilliseconds,
                           uint32_t pollIntervalMicroseconds) noexcept;
    RunnerResult teardownNext(OperationKind *kind,
                              sof::ipc4::kernel::TransportResult *transportResult,
                              uint32_t timeoutMilliseconds,
                              uint32_t pollIntervalMicroseconds) noexcept;
    RunnerResult setPipelineState(
        uint16_t state,
        sof::ipc4::kernel::TransportResult *transportResult,
        uint32_t timeoutMilliseconds,
        uint32_t pollIntervalMicroseconds) noexcept;

    // Clears poisoned runtime IDs and operation state only after the caller
    // has independently confirmed a DSP firmware reset. This performs no I/O.
    RunnerResult resetAfterFirmwareReset(
        bool firmwareResetConfirmed) noexcept;
    // Makes a normally torn-down graph reusable while retaining the validated
    // firmware ID allocator. Requires all graph IDs to have been released.
    RunnerResult resetAfterTeardown() noexcept;

    uint32_t lastFirmwareStatus() const noexcept {
        return runner_.lastFirmwareStatus();
    }
    bool setupComplete() const noexcept { return setupComplete_; }
    bool initialized() const noexcept { return initialized_; }
    bool teardownComplete() const noexcept { return teardownComplete_; }

private:
    RunnerResult recordOperationResult(RunnerResult result,
                                       bool teardown) noexcept;

    OperationRunner runner_;
    RuntimeIdAllocator *idAllocator_;
    RuntimeIds ids_;
    sof::ipc4::kernel::Message configRequest_;
    uint8_t fwConfigReply_[sof::ipc4::kernel::kMaximumPayloadBytes];
    bool initialized_;
    bool setupComplete_;
    bool teardownComplete_;
    bool failed_;
    bool idsLeased_;
    uint16_t pipelineState_;
};

} // namespace sof::ipc4::dmic::kernel
