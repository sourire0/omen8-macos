#include "DmicKernelOperationPlan.hpp"
#include "DmicFrontendReviewPlan.hpp"

#include "generated/DmicKernelStaticData.hpp"

namespace sof::ipc4::dmic::kernel {
namespace {

using sof::ipc4::kernel::Result;
using sof::ipc4::kernel::FirmwareModuleInfo;
using sof::ipc4::kernel::FirmwareManifestView;
using sof::ipc4::kernel::kMaximumFirmwareModules;
using sof::ipc4::kernel::Message;
using sof::ipc4::kernel::HdaDmicCapturePlan;
using sof::ipc4::kernel::kHdaDmicDmaConfigTlvBytes;
using sof::ipc4::kernel::bindModules;
using sof::ipc4::kernel::createPipeline;
using sof::ipc4::kernel::deletePipeline;
using sof::ipc4::kernel::initModule;
using sof::ipc4::kernel::largeConfigSet;
using sof::ipc4::kernel::appendInitModulePayload;
using sof::ipc4::kernel::serializeHdaDmicCaptureDmaTlv;

constexpr uint8_t kCopierUuid[16] = {
    0x83, 0x0c, 0xa0, 0x9b, 0x12, 0xca, 0x83, 0x4a,
    0x94, 0x3c, 0x1f, 0xa2, 0xe8, 0x2f, 0x9d, 0xda,
};
constexpr uint8_t kGainUuid[16] = {
    0xa8, 0xa9, 0xbc, 0x61, 0xd0, 0x18, 0x18, 0x4a,
    0x8e, 0x7b, 0x26, 0x39, 0x21, 0x98, 0x04, 0xb7,
};
constexpr uint8_t kEqIirUuid[16] = {
    0xe6, 0xc0, 0x50, 0x51, 0xf9, 0x27, 0xc8, 0x4e,
    0x83, 0x51, 0xc7, 0x05, 0xb6, 0x42, 0xd1, 0x2f,
};
constexpr uint8_t kTdfbUuid[16] = {0x49,0x17,0x51,0xdd,0xfa,0xd9,0x5c,0x45,0xb3,0xa7,0x13,0x58,0x56,0x93,0xf1,0xaf};
constexpr uint8_t kDrcUuid[16] = {0xda,0xe4,0x6e,0xb3,0x6f,0x00,0xf9,0x47,0xa0,0x6d,0xfe,0xcb,0xe2,0xd8,0xb6,0xce};
constexpr uint8_t kBringupUuid[16] = {
    0xf3, 0xe4, 0x79, 0x2b, 0x75, 0x46, 0x49, 0xf6,
    0x89, 0xdf, 0x3b, 0xc1, 0x94, 0xa9, 0x1a, 0xeb,
};
constexpr uint8_t kBaseFirmwareUuid[16] = {
    0x32, 0x8c, 0x39, 0x0e, 0xde, 0x5a, 0x4b, 0xba,
    0x93, 0xb1, 0xc5, 0x04, 0x32, 0x28, 0x0e, 0xe4,
};
constexpr uint8_t kBringupSha256[32] = {
    0xe6, 0x81, 0x73, 0x83, 0x4a, 0x78, 0x2a, 0xf3,
    0xa0, 0x4d, 0xc0, 0x1b, 0x49, 0x34, 0x05, 0x25,
    0x5d, 0x14, 0x80, 0xee, 0x5e, 0x7e, 0xcf, 0xc9,
    0xd5, 0x4a, 0xb0, 0xcc, 0x68, 0xd2, 0x2e, 0x5e,
};
constexpr uint8_t kBaseFirmwareSha256[32] = {
    0x9b, 0xae, 0x67, 0x6d, 0x1d, 0xd1, 0xa9, 0x10,
    0x93, 0x2d, 0x2f, 0x66, 0xf6, 0x4f, 0xf7, 0x03,
    0x1e, 0x5e, 0xc5, 0x1d, 0xb8, 0x66, 0x15, 0x54,
    0xfa, 0x93, 0x50, 0xf7, 0xa7, 0x6d, 0xf7, 0x56,
};

constexpr uint32_t kDaiCopierGatewayConfigLengthOffset = 76;
constexpr uint32_t kDaiCopierGatewayConfigDataOffset = 80;
constexpr uint32_t kDmaConfigTlvDwords =
    static_cast<uint32_t>(kHdaDmicDmaConfigTlvBytes / 4);

uint32_t loadLittle32(const uint8_t *source) noexcept {
    return static_cast<uint32_t>(source[0]) |
           (static_cast<uint32_t>(source[1]) << 8) |
           (static_cast<uint32_t>(source[2]) << 16) |
           (static_cast<uint32_t>(source[3]) << 24);
}

void storeLittle32(uint8_t *destination, uint32_t value) noexcept {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

static_assert(kDaiCopierGatewayConfigDataOffset <
              kernel_data::kDaiCopierInitBytes);
static_assert(((kernel_data::kDaiCopierInitBytes -
                kDaiCopierGatewayConfigDataOffset) & 3U) == 0);

bool matchesName(const char actual[9], const char *expected) noexcept {
    uint8_t i = 0;
    while (expected[i] != '\0') {
        if (i >= 8 || actual[i] != expected[i]) return false;
        ++i;
    }
    return i < 9 && actual[i] == '\0';
}

bool matchesBytes(const uint8_t *actual, const uint8_t *expected,
                  size_t length) noexcept {
    for (size_t i = 0; i < length; ++i) {
        if (actual[i] != expected[i]) return false;
    }
    return true;
}

const FirmwareModuleInfo *findModule(const FirmwareManifestView *manifest,
                                     const uint8_t uuid[16],
                                     const char *name) noexcept {
    const FirmwareModuleInfo *module =
        sof::ipc4::kernel::findFirmwareModuleByUuid(manifest, uuid);
    return module && matchesName(module->name, name) ? module : nullptr;
}

bool isKnownAdlV212Profile(const FirmwareManifestView *manifest) noexcept {
    if (!manifest || manifest->extensionMajor != 1 ||
        manifest->extensionMinor != 0 || manifest->moduleCount != 25 ||
        !matchesName(manifest->imageName, "ADSPFW") ||
        manifest->preloadPageCount != 172 || manifest->firmwareImageFlags != 0 ||
        manifest->featureMask != 0xffffU || manifest->version[0] != 2 ||
        manifest->version[1] != 12 || manifest->version[2] != 0 ||
        manifest->version[3] != 1 ||
        manifest->hardwareBufferBaseAddress != 0 ||
        manifest->hardwareBufferLength != 0 || manifest->loadOffset != 0x30000U)
        return false;

    const FirmwareModuleInfo *bringup =
        findModule(manifest, kBringupUuid, "BRNGUP");
    const FirmwareModuleInfo *base =
        findModule(manifest, kBaseFirmwareUuid, "BASEFW");
    return bringup && bringup->id == 0 &&
           matchesBytes(bringup->sha256, kBringupSha256, sizeof(kBringupSha256)) &&
           base && base->id == 1 &&
           matchesBytes(base->sha256, kBaseFirmwareSha256,
                        sizeof(kBaseFirmwareSha256)) &&
           base->segments[0].sizeBytes == 0x48000U &&
           base->segments[1].sizeBytes == 0x5b000U &&
           base->segments[2].sizeBytes == 0x3000U;
}

OperationPlanResult fromCodec(Result result) noexcept {
    return result == Result::Ok ? OperationPlanResult::Ok
                                : OperationPlanResult::CodecFailure;
}

} // namespace

OperationPlan::OperationPlan() noexcept
    : copierModuleId_(0), gainModuleId_(0), eqiirModuleId_(0), tdfbModuleId_(0), drcModuleId_(0),
      ids_{}, position_(0), initialized_(false),
      awaitingConfirmation_(false), failed_(false) {}

OperationPlanResult OperationPlan::initialize(
    const FirmwareManifestView *manifest, const RuntimeIds &ids,
    const HdaDmicCapturePlan &capturePlan) noexcept {
    initialized_ = false;
    position_ = 0;
    awaitingConfirmation_ = false;
    failed_ = false;
    if (!manifest || manifest->moduleCount == 0 ||
        manifest->moduleCount > kMaximumFirmwareModules)
        return OperationPlanResult::InvalidArgument;
    if (!isKnownAdlV212Profile(manifest))
        return OperationPlanResult::WrongManifest;

    const FirmwareModuleInfo *copier = findModule(manifest, kCopierUuid, "COPIER");
    const FirmwareModuleInfo *gain = findModule(manifest, kGainUuid, "GAIN");
    const FirmwareModuleInfo *eqiir = findModule(manifest, kEqIirUuid, "EQIIR");
    const FirmwareModuleInfo *tdfb = findModule(manifest, kTdfbUuid, "TDFB");
    const FirmwareModuleInfo *drc = findModule(manifest, kDrcUuid, "DRC");
    if (!copier || !gain || !eqiir || !tdfb || !drc || tdfb->id > UINT16_MAX || drc->id > UINT16_MAX ||
        ids.tdfb > tdfb->instanceMaxCount || ids.drc > drc->instanceMaxCount ||
        ids.hostCopier > copier->instanceMaxCount || ids.frontendPipeline == ids.pipeline ||
        ids.hostCopier == ids.daiCopier || ids.hostCopier == ids.moduleCopier)
        return OperationPlanResult::InvalidRuntimeIds;
    if (!copier || !gain || !eqiir || copier->id > UINT16_MAX ||
        gain->id > UINT16_MAX || eqiir->id > UINT16_MAX)
        return OperationPlanResult::WrongManifest;
    if (ids.daiCopier > copier->instanceMaxCount ||
        ids.moduleCopier > copier->instanceMaxCount ||
        ids.gain > gain->instanceMaxCount ||
        ids.eqiir > eqiir->instanceMaxCount ||
        ids.daiCopier == ids.moduleCopier)
        return OperationPlanResult::InvalidRuntimeIds;

    uint8_t dmaConfigTlv[kHdaDmicDmaConfigTlvBytes];
    if (serializeHdaDmicCaptureDmaTlv(
            capturePlan, dmaConfigTlv, sizeof(dmaConfigTlv)) !=
        sof::ipc4::kernel::HdaCapturePlanResult::Ok)
        return OperationPlanResult::InvalidArgument;

    copierModuleId_ = static_cast<uint16_t>(copier->id);
    gainModuleId_ = static_cast<uint16_t>(gain->id);
    eqiirModuleId_ = static_cast<uint16_t>(eqiir->id);
    tdfbModuleId_ = static_cast<uint16_t>(tdfb->id);
    drcModuleId_ = static_cast<uint16_t>(drc->id);
    ids_.pipeline = ids.pipeline;
    ids_.daiCopier = ids.daiCopier;
    ids_.gain = ids.gain;
    ids_.eqiir = ids.eqiir;
    ids_.moduleCopier = ids.moduleCopier;
    ids_.frontendPipeline = ids.frontendPipeline;
    ids_.hostCopier = ids.hostCopier;
    ids_.tdfb = ids.tdfb;
    ids_.drc = ids.drc;
    capturePlan_ = capturePlan;
    initialized_ = true;
    return OperationPlanResult::Ok;
}

OperationPlanResult OperationPlan::emit(uint8_t step, Message *message,
                                         OperationKind *kind) noexcept {
    if (!message || !kind) return OperationPlanResult::InvalidArgument;
    if (!initialized_) return OperationPlanResult::InvalidArgument;
    if (step >= setupOperationCount() + teardownOperationCount())
        return OperationPlanResult::Complete;

    if (step >= 10 && step < 23) {
        const frontend_review::Ids front{ids_.frontendPipeline, ids_.pipeline,
            ids_.hostCopier, ids_.moduleCopier, ids_.tdfb, ids_.drc,
            copierModuleId_, tdfbModuleId_, drcModuleId_,
            static_cast<uint8_t>(capturePlan_.streamId), 6};
        *kind = step < 19 ? OperationKind::FrontendSetup : OperationKind::FrontendTeardown;
        return fromCodec(frontend_review::buildStep(static_cast<uint8_t>(step - 10U), front, message));
    }
    if (step >= 23) step = static_cast<uint8_t>(step - 13U);
    using namespace kernel_data;
    Result result = Result::InvalidField;
    switch (step) {
    case 0:
        *kind = OperationKind::CreatePipeline;
        result = createPipeline(ids_.pipeline, 0, 8, 0, false, message);
        break;
    case 1:
        *kind = OperationKind::InitDaiCopier;
        result = initModule(copierModuleId_, ids_.daiCopier, ids_.pipeline, 0,
                            kDaiCopierInit, kDaiCopierInitBytes, message);
        if (result == Result::Ok) {
            uint8_t dmaConfigTlv[kHdaDmicDmaConfigTlvBytes];
            if (serializeHdaDmicCaptureDmaTlv(
                    capturePlan_, dmaConfigTlv, sizeof(dmaConfigTlv)) !=
                sof::ipc4::kernel::HdaCapturePlanResult::Ok)
                return OperationPlanResult::InvalidState;
            const uint32_t expectedGatewayDwords = static_cast<uint32_t>(
                (kDaiCopierInitBytes - kDaiCopierGatewayConfigDataOffset) / 4);
            if (message->payloadBytes != kDaiCopierInitBytes ||
                loadLittle32(message->payload +
                             kDaiCopierGatewayConfigLengthOffset) !=
                    expectedGatewayDwords)
                return OperationPlanResult::InvalidState;
            storeLittle32(message->payload +
                              kDaiCopierGatewayConfigLengthOffset,
                          expectedGatewayDwords + kDmaConfigTlvDwords);
            result = appendInitModulePayload(dmaConfigTlv,
                                             sizeof(dmaConfigTlv), message);
        }
        break;
    case 2:
        *kind = OperationKind::InitGain;
        result = initModule(gainModuleId_, ids_.gain, ids_.pipeline, 0,
                            kGainInit, kGainInitBytes, message);
        break;
    case 3:
        *kind = OperationKind::InitEqIir;
        result = initModule(eqiirModuleId_, ids_.eqiir, ids_.pipeline, 0,
                            kEqIirInit, kEqIirInitBytes, message);
        break;
    case 4:
        *kind = OperationKind::InitModuleCopier;
        result = initModule(copierModuleId_, ids_.moduleCopier, ids_.pipeline, 0,
                            kModuleCopierInit, kModuleCopierInitBytes, message);
        break;
    case 5:
    case 6:
    case 7:
    case 10:
    case 11:
    case 12: {
        const bool bind = step < 10;
        uint16_t sourceModule = 0;
        uint8_t sourceInstance = 0;
        uint16_t destinationModule = 0;
        uint8_t destinationInstance = 0;
        if (step == 5 || step == 12) {
            sourceModule = copierModuleId_;
            sourceInstance = ids_.daiCopier;
            destinationModule = gainModuleId_;
            destinationInstance = ids_.gain;
            *kind = step == 5 ? OperationKind::BindCopierToGain
                                   : OperationKind::UnbindCopierFromGain;
        } else if (step == 6 || step == 11) {
            sourceModule = gainModuleId_;
            sourceInstance = ids_.gain;
            destinationModule = eqiirModuleId_;
            destinationInstance = ids_.eqiir;
            *kind = step == 6 ? OperationKind::BindGainToEqIir
                                   : OperationKind::UnbindGainFromEqIir;
        } else {
            sourceModule = eqiirModuleId_;
            sourceInstance = ids_.eqiir;
            destinationModule = copierModuleId_;
            destinationInstance = ids_.moduleCopier;
            *kind = step == 7 ? OperationKind::BindEqIirToCopier
                                   : OperationKind::UnbindEqIirFromCopier;
        }
        result = bindModules(bind, sourceModule, sourceInstance, 0,
                             destinationModule, destinationInstance, 0, message);
        break;
    }
    case 8: {
        *kind = OperationKind::ConfigureEqIir;
        result = largeConfigSet(eqiirModuleId_, ids_.eqiir, 0,
                                kEqIirControlPayloadTemplate,
                                kEqIirControlPayloadTemplateBytes, message);
        break;
    }
    case 9: {
        *kind = OperationKind::ConfigureGainSwitch;
        result = largeConfigSet(gainModuleId_, ids_.gain, 200,
                                kSwitchControlPayloadTemplate,
                                kSwitchControlPayloadTemplateBytes, message);
        if (result == Result::Ok) {
            message->payload[0] = 1;
            message->payload[1] = 0;
        }
        break;
    }
    case 13:
        *kind = OperationKind::DeletePipeline;
        result = deletePipeline(ids_.pipeline, message);
        break;
    default:
        return OperationPlanResult::InvalidArgument;
    }

    const OperationPlanResult planResult = fromCodec(result);
    return planResult;
}

OperationPlanResult OperationPlan::nextSetup(Message *message,
                                               OperationKind *kind) noexcept {
    if (!initialized_) return OperationPlanResult::InvalidArgument;
    if (failed_) return OperationPlanResult::OperationFailed;
    if (awaitingConfirmation_) return OperationPlanResult::ConfirmationRequired;
    if (position_ >= setupOperationCount()) return OperationPlanResult::Complete;
    const OperationPlanResult result = emit(position_, message, kind);
    if (result == OperationPlanResult::Ok) awaitingConfirmation_ = true;
    return result;
}

OperationPlanResult OperationPlan::nextTeardown(Message *message,
                                                  OperationKind *kind) noexcept {
    if (!initialized_) return OperationPlanResult::InvalidArgument;
    if (failed_) return OperationPlanResult::OperationFailed;
    if (awaitingConfirmation_) return OperationPlanResult::ConfirmationRequired;
    if (position_ < setupOperationCount()) return OperationPlanResult::WrongPhase;
    if (position_ >= setupOperationCount() + teardownOperationCount())
        return OperationPlanResult::Complete;
    const OperationPlanResult result = emit(position_, message, kind);
    if (result == OperationPlanResult::Ok) awaitingConfirmation_ = true;
    return result;
}

OperationPlanResult OperationPlan::confirmCurrent(bool replySucceeded) noexcept {
    if (!initialized_) return OperationPlanResult::InvalidArgument;
    if (!awaitingConfirmation_) return OperationPlanResult::InvalidState;
    awaitingConfirmation_ = false;
    if (!replySucceeded) {
        failed_ = true;
        return OperationPlanResult::OperationFailed;
    }
    ++position_;
    return OperationPlanResult::Ok;
}

} // namespace sof::ipc4::dmic::kernel
