#include "RuntimeIdAllocator.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace sof::ipc4::dmic {
namespace {

const Module &requireModule(const FirmwareManifest &firmware,
                            const std::string &uuid, const char *name) {
    const auto it = std::find_if(firmware.modules.begin(), firmware.modules.end(),
                                 [&](const Module &module) {
                                     return uuidString(module.uuid) == uuid &&
                                            module.name == name;
                                 });
    if (it == firmware.modules.end())
        throw std::runtime_error(std::string("missing IPC4 module: ") + name);
    return *it;
}

uint8_t firstFree(const std::set<uint8_t> &used, uint16_t maxInclusive,
                  const char *kind) {
    if (maxInclusive > 0xff)
        throw std::invalid_argument(std::string("IPC4 ") + kind +
                                    " ID ceiling exceeds 8-bit field");
    for (uint16_t candidate = 0; candidate <= maxInclusive; ++candidate) {
        const auto id = static_cast<uint8_t>(candidate);
        if (used.count(id) == 0) return id;
    }
    throw std::runtime_error(std::string("IPC4 ") + kind + " ID pool exhausted");
}

} // namespace

RuntimeIdAllocator::RuntimeIdAllocator(uint16_t maxPipelineIdInclusive)
    : maxPipelineIdInclusive_(maxPipelineIdInclusive) {
    if (maxPipelineIdInclusive > 0xff)
        throw std::invalid_argument("IPC4 pipeline ID ceiling exceeds 8-bit field");
}

RuntimeIds RuntimeIdAllocator::allocate(const FirmwareManifest &firmware) {
    constexpr const char *kCopierUuid = "9ba00c83-ca12-4a83-943c-1fa2e82f9dda";
    constexpr const char *kGainUuid = "61bca9a8-18d0-4a18-8e7b-2639219804b7";
    constexpr const char *kEqIirUuid = "5150c0e6-27f9-4ec8-8351-c705b642d12f";
    const Module &copier = requireModule(firmware, kCopierUuid, "COPIER");
    const Module &gain = requireModule(firmware, kGainUuid, "GAIN");
    const Module &eqiir = requireModule(firmware, kEqIirUuid, "EQIIR");

    RuntimeIds ids{};
    // Match Linux SOF's ida_alloc_max(..., manifest instance_max_count): the
    // upper bound is inclusive, so ID == instance_max_count is valid.
    ids.pipeline = firstFree(pipelines_, maxPipelineIdInclusive_, "pipeline");
    ids.daiCopier = firstFree(moduleInstances_[copier.id], copier.instanceMaxCount,
                              "COPIER module");
    auto copierUsed = moduleInstances_[copier.id];
    copierUsed.insert(ids.daiCopier);
    ids.moduleCopier = firstFree(copierUsed, copier.instanceMaxCount,
                                 "COPIER module");
    ids.gain = firstFree(moduleInstances_[gain.id], gain.instanceMaxCount,
                         "GAIN module");
    ids.eqiir = firstFree(moduleInstances_[eqiir.id], eqiir.instanceMaxCount,
                          "EQIIR module");

    // Commit atomically only after every required ID has been found.
    pipelines_.insert(ids.pipeline);
    moduleInstances_[copier.id].insert(ids.daiCopier);
    moduleInstances_[copier.id].insert(ids.moduleCopier);
    moduleInstances_[gain.id].insert(ids.gain);
    moduleInstances_[eqiir.id].insert(ids.eqiir);
    return ids;
}

void RuntimeIdAllocator::release(const FirmwareManifest &firmware,
                                 const RuntimeIds &ids) {
    constexpr const char *kCopierUuid = "9ba00c83-ca12-4a83-943c-1fa2e82f9dda";
    constexpr const char *kGainUuid = "61bca9a8-18d0-4a18-8e7b-2639219804b7";
    constexpr const char *kEqIirUuid = "5150c0e6-27f9-4ec8-8351-c705b642d12f";
    const Module &copier = requireModule(firmware, kCopierUuid, "COPIER");
    const Module &gain = requireModule(firmware, kGainUuid, "GAIN");
    const Module &eqiir = requireModule(firmware, kEqIirUuid, "EQIIR");
    auto eraseExactly = [](std::set<uint8_t> &pool, uint8_t id, const char *kind) {
        if (pool.erase(id) != 1)
            throw std::invalid_argument(std::string("release of unallocated ") + kind + " ID");
    };

    // Validate all ownership first so a bad release cannot partially free IDs.
    if (pipelines_.count(ids.pipeline) == 0 ||
        moduleInstances_[copier.id].count(ids.daiCopier) == 0 ||
        moduleInstances_[copier.id].count(ids.moduleCopier) == 0 ||
        ids.daiCopier == ids.moduleCopier ||
        moduleInstances_[gain.id].count(ids.gain) == 0 ||
        moduleInstances_[eqiir.id].count(ids.eqiir) == 0)
        throw std::invalid_argument("runtime ID lease does not match allocator state");

    eraseExactly(pipelines_, ids.pipeline, "pipeline");
    eraseExactly(moduleInstances_[copier.id], ids.daiCopier, "COPIER module");
    eraseExactly(moduleInstances_[copier.id], ids.moduleCopier, "COPIER module");
    eraseExactly(moduleInstances_[gain.id], ids.gain, "GAIN module");
    eraseExactly(moduleInstances_[eqiir.id], ids.eqiir, "EQIIR module");
}

} // namespace sof::ipc4::dmic
