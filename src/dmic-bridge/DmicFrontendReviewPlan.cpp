#include "DmicFrontendReviewPlan.hpp"
#include "generated/DmicFrontendReviewData.hpp"

namespace sof::ipc4::dmic::frontend_review {
using namespace sof::ipc4::kernel;
Result buildStep(uint8_t step, const Ids &id, Message *out) noexcept {
    if (!out) return Result::NullOutput;
    if (id.frontendPipeline == id.backendPipeline || id.hostStreamTag == 0 ||
        id.hostStreamTag > 15 || id.frontendPages == 0 ||
        id.hostCopier == id.backendCopier) return Result::InvalidField;
    switch (step) {
    case 0: return createPipeline(id.frontendPipeline, 0, id.frontendPages, 0, false, out);
    case 1: return initModule(id.tdfbModule, id.tdfb, id.frontendPipeline, 0, kTdfbInit, kTdfbInitBytes, out);
    case 2: return initModule(id.drcModule, id.drc, id.frontendPipeline, 0, kDrcInit, kDrcInitBytes, out);
    case 3: {
        Result r = initModule(id.copierModule, id.hostCopier, id.frontendPipeline, 0, kHostInitTag1, kHostInitTag1Bytes, out);
        if (r != Result::Ok) return r;
        // Gateway node_id at byte 68: HDA host-output node type 1, stream tag minus one.
        out->payload[68] = static_cast<uint8_t>(id.hostStreamTag - 1U);
        return Result::Ok;
    }
    case 4: return bindModules(true, id.copierModule, id.backendCopier, 0, id.tdfbModule, id.tdfb, 0, out);
    case 5: return bindModules(true, id.tdfbModule, id.tdfb, 0, id.drcModule, id.drc, 0, out);
    case 6: return bindModules(true, id.drcModule, id.drc, 0, id.copierModule, id.hostCopier, 0, out);
    case 7: return largeConfigSet(id.tdfbModule, id.tdfb, 0, kTdfbControl, kTdfbControlBytes, out);
    case 8: return largeConfigSet(id.drcModule, id.drc, 0, kDrcControl, kDrcControlBytes, out);
    case 9: return bindModules(false, id.drcModule, id.drc, 0, id.copierModule, id.hostCopier, 0, out);
    case 10: return bindModules(false, id.tdfbModule, id.tdfb, 0, id.drcModule, id.drc, 0, out);
    case 11: return bindModules(false, id.copierModule, id.backendCopier, 0, id.tdfbModule, id.tdfb, 0, out);
    case 12: return deletePipeline(id.frontendPipeline, out);
    default: return Result::InvalidField;
    }
}
}
