#pragma once
#include "Ipc4KernelCodec.hpp"
#include <stdint.h>

// Review-only message builder. It is not connected to the live kernel driver.
namespace sof::ipc4::dmic::frontend_review {
struct Ids {
    uint8_t frontendPipeline, backendPipeline;
    uint8_t hostCopier, backendCopier, tdfb, drc;
    uint16_t copierModule, tdfbModule, drcModule;
    uint8_t hostStreamTag;
    uint16_t frontendPages;
};
// Build one operation: create, three module inits, three binds, two binary
// controls. Ownership, ID leases, acknowledgements, graph teardown, and
// multi-pipeline triggering are the caller's responsibility.
constexpr uint8_t kSetupSteps = 9;
constexpr uint8_t kTeardownSteps = 4;
sof::ipc4::kernel::Result buildStep(uint8_t step, const Ids &ids,
                                   sof::ipc4::kernel::Message *out) noexcept;
}
