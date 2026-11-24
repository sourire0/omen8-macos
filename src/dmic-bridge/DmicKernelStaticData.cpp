#include "generated/DmicKernelStaticData.hpp"

namespace {
using namespace sof::ipc4::dmic::kernel_data;
static_assert(kDaiCopierInitBytes == 3128);
static_assert(kModuleCopierInitBytes == 84);
static_assert(kGainInitBytes == 64);
static_assert(kEqIirInitBytes == 40);
static_assert(kCreatePipelinePayloadBytes == 28);
static_assert(kEqIirControlPayloadTemplateBytes == 88);
static_assert(kSwitchControlPayloadTemplateBytes == 52);
} // namespace
