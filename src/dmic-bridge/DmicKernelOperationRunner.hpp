#pragma once

#include "DmicKernelOperationPlan.hpp"
#include "Ipc4KernelTransport.hpp"
#include "Cavs25BootLifecycle.hpp"

namespace sof::ipc4::dmic::kernel {

using sof::ipc4::kernel::Ipc4Transport;
using sof::ipc4::kernel::Cavs25BootLifecycle;
using sof::ipc4::kernel::Reply;
using sof::ipc4::kernel::TransportResult;

enum class RunnerResult : uint8_t {
    Ok,
    Complete,
    InvalidArgument,
    InvalidState,
    WrongPhase,
    RecoveryRequired,
    TransportFailure,
    FirmwareRejected,
};

// Executes one graph operation per call. The caller must serialize calls and
// construct a new runner only after a confirmed firmware reset if this runner
// enters its terminal failure state. The 4 KiB request lives in the runner,
// never on the kernel stack.
class OperationRunner {
public:
    OperationRunner() noexcept;

    RunnerResult initialize(const FirmwareManifestView *manifest,
                            const RuntimeIds &ids,
                            const sof::ipc4::kernel::HdaDmicCapturePlan &capturePlan,
                            const Cavs25BootLifecycle *lifecycle,
                            Ipc4Transport *transport) noexcept;
    RunnerResult setupNext(OperationKind *kind, TransportResult *transportResult,
                           uint32_t timeoutMilliseconds,
                           uint32_t pollIntervalMicroseconds) noexcept;
    RunnerResult teardownNext(OperationKind *kind,
                              TransportResult *transportResult,
                              uint32_t timeoutMilliseconds,
                              uint32_t pollIntervalMicroseconds) noexcept;
    RunnerResult setPipelineState(uint16_t state,
                                  TransportResult *transportResult,
                                  uint32_t timeoutMilliseconds,
                                  uint32_t pollIntervalMicroseconds) noexcept;
    uint32_t lastFirmwareStatus() const noexcept {
        return reply_.firmwareStatus;
    }

private:
    RunnerResult runOne(bool teardown, OperationKind *kind,
                        TransportResult *transportResult,
                        uint32_t timeoutMilliseconds,
                        uint32_t pollIntervalMicroseconds) noexcept;

    OperationPlan plan_;
    const Cavs25BootLifecycle *lifecycle_;
    Ipc4Transport *transport_;
    Message message_;
    Reply reply_;
    bool initialized_;
    bool failed_;
    bool setupComplete_;
    bool teardownComplete_;
};

} // namespace sof::ipc4::dmic::kernel
