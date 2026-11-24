#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sof::ipc4 {

struct Module {
    // SOF assigns the in-library module ID from this table index. The on-disk
    // leading word is a record marker ($AME), not the runtime module ID.
    uint32_t id{};
    std::string name;
    std::array<uint8_t, 16> uuid{};
    uint32_t type{};
    uint16_t configOffset{};
    uint16_t configCount{};
    uint32_t affinityMask{};
    uint16_t instanceMaxCount{};
    uint16_t instanceStackSize{};
};

struct FirmwareManifest {
    uint32_t extensionLength{};
    uint16_t extensionMajor{};
    uint16_t extensionMinor{};
    uint32_t moduleCount{};
    std::string imageName;
    std::array<uint16_t, 4> version{};
    std::vector<Module> modules;

    static FirmwareManifest parse(const uint8_t *data, size_t size);
};

std::string uuidString(const std::array<uint8_t, 16> &uuid);

} // namespace sof::ipc4
