#pragma once

#include "FirmwareManifestKernel.hpp"
#include "HdaDmicCapturePlan.hpp"
#include "Ipc4KernelCodec.hpp"

#include <stdint.h>

namespace sof::ipc4::dmic::kernel {

using sof::ipc4::kernel::FirmwareManifestView;
using sof::ipc4::kernel::Message;

struct RuntimeIds {
    uint8_t pipeline;
    uint8_t daiCopier;
    uint8_t gain;
    uint8_t eqiir;
    uint8_t moduleCopier;
    uint8_t frontendPipeline;
    uint8_t hostCopier;
    uint8_t tdfb;
    uint8_t drc;
};

enum class OperationKind : uint8_t {
    CreatePipeline,
    InitDaiCopier,
    InitGain,
    InitEqIir,
    InitModuleCopier,
    BindCopierToGain,
    BindGainToEqIir,
    BindEqIirToCopier,
    ConfigureEqIir,
    ConfigureGainSwitch,
    UnbindEqIirFromCopier,
    UnbindGainFromEqIir,
    UnbindCopierFromGain,
    DeletePipeline,
    FrontendSetup,
    FrontendTeardown,
};

enum class OperationPlanResult : uint8_t {
    Ok,
    Complete,
    InvalidArgument,
    WrongManifest,
    InvalidRuntimeIds,
    WrongPhase,
    ConfirmationRequired,
    OperationFailed,
    InvalidState,
    CodecFailure,
};

// Fixed-size sequential builder for the audited two-channel DMIC graph.
// It emits one message into caller-owned storage at a time and performs no I/O.
class OperationPlan {
public:
    OperationPlan() noexcept;

    OperationPlanResult initialize(const FirmwareManifestView *manifest,
                                   const RuntimeIds &ids,
                                   const sof::ipc4::kernel::HdaDmicCapturePlan &capturePlan) noexcept;
    // Each emitted operation remains pending until confirmCurrent(true).
    // Teardown becomes available only after all 10 setup replies are confirmed.
    OperationPlanResult nextSetup(Message *message,
                                  OperationKind *kind) noexcept;
    OperationPlanResult nextTeardown(Message *message,
                                     OperationKind *kind) noexcept;
    OperationPlanResult confirmCurrent(bool replySucceeded) noexcept;
    uint8_t position() const noexcept { return position_; }
    uint8_t pipelineInstance() const noexcept { return ids_.pipeline; }
    uint8_t frontendPipelineInstance() const noexcept { return ids_.frontendPipeline; }
    static constexpr uint8_t setupOperationCount() noexcept { return 19; }
    static constexpr uint8_t teardownOperationCount() noexcept { return 8; }

private:
    OperationPlanResult emit(uint8_t step, Message *message,
                             OperationKind *kind) noexcept;
    uint16_t copierModuleId_;
    uint16_t gainModuleId_;
    uint16_t eqiirModuleId_;
    uint16_t tdfbModuleId_;
    uint16_t drcModuleId_;
    RuntimeIds ids_;
    sof::ipc4::kernel::HdaDmicCapturePlan capturePlan_;
    uint8_t position_;
    bool initialized_;
    bool awaitingConfirmation_;
    bool failed_;
};

} // namespace sof::ipc4::dmic::kernel
