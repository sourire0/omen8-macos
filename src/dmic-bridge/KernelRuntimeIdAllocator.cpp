#include "KernelRuntimeIdAllocator.hpp"

namespace sof::ipc4::dmic::kernel {
namespace {

using sof::ipc4::kernel::FirmwareManifestView;
using sof::ipc4::kernel::FirmwareModuleInfo;
using sof::ipc4::kernel::IdPoolResult;
using sof::ipc4::kernel::findFirmwareModuleByUuid;

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

bool moduleNameMatches(const FirmwareModuleInfo *module,
                       const char *expected) noexcept {
    if (!module || !expected) return false;
    size_t i = 0;
    while (expected[i] != '\0') {
        if (i >= sizeof(module->name) - 1 || module->name[i] != expected[i])
            return false;
        ++i;
    }
    return module->name[i] == '\0';
}

const FirmwareModuleInfo *findUniqueModule(
    const FirmwareManifestView *manifest, const uint8_t uuid[16],
    const char *name, bool *duplicate) noexcept {
    const FirmwareModuleInfo *found = nullptr;
    *duplicate = false;
    for (uint32_t i = 0; i < manifest->moduleCount; ++i) {
        const FirmwareModuleInfo *module = &manifest->modules[i];
        bool matches = true;
        for (size_t j = 0; j < 16; ++j)
            matches = matches && module->uuid[j] == uuid[j];
        if (!matches) continue;
        if (found) {
            *duplicate = true;
            return nullptr;
        }
        found = module;
    }
    return moduleNameMatches(found, name) ? found : nullptr;
}

RuntimeIdAllocatorResult fromPool(IdPoolResult result) noexcept {
    switch (result) {
    case IdPoolResult::Ok: return RuntimeIdAllocatorResult::Ok;
    case IdPoolResult::InvalidMaximum: return RuntimeIdAllocatorResult::InvalidMaximum;
    case IdPoolResult::Exhausted: return RuntimeIdAllocatorResult::Exhausted;
    case IdPoolResult::NotAllocated: return RuntimeIdAllocatorResult::NotAllocated;
    case IdPoolResult::TeardownUnconfirmed:
        return RuntimeIdAllocatorResult::TeardownUnconfirmed;
    case IdPoolResult::InvalidState:
    case IdPoolResult::InvalidId:
        return RuntimeIdAllocatorResult::InvalidState;
    }
    return RuntimeIdAllocatorResult::InvalidState;
}

} // namespace

RuntimeIdAllocator::RuntimeIdAllocator() noexcept
    : leased_{}, initialized_(false), leaseActive_(false) {}

RuntimeIdAllocatorResult RuntimeIdAllocator::initialize(
    const FirmwareManifestView *manifest,
    uint16_t maxPipelineIdInclusive) noexcept {
    if (initialized_ || leaseActive_) return RuntimeIdAllocatorResult::InvalidState;
    if (!manifest || manifest->moduleCount == 0 ||
        manifest->moduleCount > sof::ipc4::kernel::kMaximumFirmwareModules)
        return RuntimeIdAllocatorResult::InvalidArgument;
    if (maxPipelineIdInclusive > UINT8_MAX)
        return RuntimeIdAllocatorResult::InvalidMaximum;

    bool copierDuplicate = false;
    bool gainDuplicate = false;
    bool eqiirDuplicate = false;
    const FirmwareModuleInfo *copier = findUniqueModule(
        manifest, kCopierUuid, "COPIER", &copierDuplicate);
    const FirmwareModuleInfo *gain = findUniqueModule(
        manifest, kGainUuid, "GAIN", &gainDuplicate);
    const FirmwareModuleInfo *eqiir = findUniqueModule(
        manifest, kEqIirUuid, "EQIIR", &eqiirDuplicate);
    bool tdfbDuplicate = false, drcDuplicate = false;
    const FirmwareModuleInfo *tdfb = findUniqueModule(manifest, kTdfbUuid, "TDFB", &tdfbDuplicate);
    const FirmwareModuleInfo *drc = findUniqueModule(manifest, kDrcUuid, "DRC", &drcDuplicate);
    if (copierDuplicate || gainDuplicate || eqiirDuplicate || tdfbDuplicate || drcDuplicate)
        return RuntimeIdAllocatorResult::DuplicateModule;
    if (!copier || !gain || !eqiir || !tdfb || !drc)
        return RuntimeIdAllocatorResult::WrongManifest;

    RuntimeIdAllocatorResult result = fromPool(pipelines_.initialize(
        maxPipelineIdInclusive));
    if (result != RuntimeIdAllocatorResult::Ok) return result;
    result = fromPool(copiers_.initialize(copier->instanceMaxCount));
    if (result != RuntimeIdAllocatorResult::Ok) goto fail;
    result = fromPool(gains_.initialize(gain->instanceMaxCount));
    if (result != RuntimeIdAllocatorResult::Ok) goto fail;
    result = fromPool(eqiirs_.initialize(eqiir->instanceMaxCount));
    if (result != RuntimeIdAllocatorResult::Ok) goto fail;
    result = fromPool(tdfbs_.initialize(tdfb->instanceMaxCount));
    if (result != RuntimeIdAllocatorResult::Ok) goto fail;
    result = fromPool(drcs_.initialize(drc->instanceMaxCount));
    if (result != RuntimeIdAllocatorResult::Ok) goto fail;
    initialized_ = true;
    return RuntimeIdAllocatorResult::Ok;

fail:
    pipelines_.resetAfterFirmwareReset();
    copiers_.resetAfterFirmwareReset();
    gains_.resetAfterFirmwareReset();
    eqiirs_.resetAfterFirmwareReset();
    tdfbs_.resetAfterFirmwareReset();
    drcs_.resetAfterFirmwareReset();
    return result;
}

RuntimeIdAllocatorResult RuntimeIdAllocator::allocate(RuntimeIds *outIds) noexcept {
    if (!outIds) return RuntimeIdAllocatorResult::InvalidArgument;
    if (!initialized_ || leaseActive_) return RuntimeIdAllocatorResult::InvalidState;
    RuntimeIds ids{};
    sof::ipc4::kernel::IdPool *pools[] = {&pipelines_, &pipelines_, &copiers_, &copiers_, &copiers_, &gains_, &eqiirs_, &tdfbs_, &drcs_};
    uint8_t *values[] = {&ids.pipeline, &ids.frontendPipeline, &ids.daiCopier, &ids.moduleCopier, &ids.hostCopier, &ids.gain, &ids.eqiir, &ids.tdfb, &ids.drc};
    for (unsigned i = 0; i < 9; ++i) {
        const IdPoolResult result = pools[i]->allocate(values[i]);
        if (result != IdPoolResult::Ok) {
            bool rollbackOk = true;
            while (i > 0) { --i; if (pools[i]->release(*values[i], true) != IdPoolResult::Ok) rollbackOk = false; }
            if (!rollbackOk) { initialized_ = false; return RuntimeIdAllocatorResult::InvalidState; }
            return fromPool(result);
        }
    }
    leased_ = ids; leaseActive_ = true; *outIds = ids;
    return RuntimeIdAllocatorResult::Ok;
}
RuntimeIdAllocatorResult RuntimeIdAllocator::release(const RuntimeIds &ids, bool teardownConfirmed) noexcept {
    if (!initialized_ || !leaseActive_) return RuntimeIdAllocatorResult::InvalidState;
    if (!teardownConfirmed) return RuntimeIdAllocatorResult::TeardownUnconfirmed;
    const uint8_t values[] = {ids.pipeline, ids.frontendPipeline, ids.daiCopier, ids.moduleCopier, ids.hostCopier, ids.gain, ids.eqiir, ids.tdfb, ids.drc};
    const uint8_t expected[] = {leased_.pipeline, leased_.frontendPipeline, leased_.daiCopier, leased_.moduleCopier, leased_.hostCopier, leased_.gain, leased_.eqiir, leased_.tdfb, leased_.drc};
    sof::ipc4::kernel::IdPool *pools[] = {&pipelines_, &pipelines_, &copiers_, &copiers_, &copiers_, &gains_, &eqiirs_, &tdfbs_, &drcs_};
    for (unsigned i=0;i<9;++i) if (values[i]!=expected[i] || !pools[i]->isAllocated(values[i])) return RuntimeIdAllocatorResult::NotAllocated;
    for (unsigned i=9;i>0;) { --i; if (pools[i]->release(values[i], true)!=IdPoolResult::Ok) { initialized_=false; return RuntimeIdAllocatorResult::InvalidState; } }
    leaseActive_=false; leased_=RuntimeIds{}; return RuntimeIdAllocatorResult::Ok;
}

void RuntimeIdAllocator::resetAfterFirmwareReset() noexcept {
    pipelines_.resetAfterFirmwareReset();
    copiers_.resetAfterFirmwareReset();
    gains_.resetAfterFirmwareReset();
    eqiirs_.resetAfterFirmwareReset();
    tdfbs_.resetAfterFirmwareReset();
    drcs_.resetAfterFirmwareReset();
    leased_ = RuntimeIds{};
    initialized_ = false;
    leaseActive_ = false;
}

} // namespace sof::ipc4::dmic::kernel
