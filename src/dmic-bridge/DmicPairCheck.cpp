#include "FirmwareManifest.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct RequiredModule {
    const char *name;
    const char *uuid;
};

constexpr std::array<RequiredModule, 3> kDmicModules{{
    {"COPIER", "9ba00c83-ca12-4a83-943c-1fa2e82f9dda"},
    {"GAIN", "61bca9a8-18d0-4a18-8e7b-2639219804b7"},
    {"EQIIR", "5150c0e6-27f9-4ec8-8351-c705b642d12f"},
}};

std::vector<uint8_t> readFile(const char *path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error(std::string("cannot open ") + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
}

std::array<uint8_t, 16> uuidBytesLe(const std::string &text) {
    std::string hex;
    for (char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch == '-') continue;
        if (!std::isxdigit(ch)) throw std::runtime_error("invalid UUID constant");
        hex.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (hex.size() != 32) throw std::runtime_error("invalid UUID length");
    std::array<uint8_t, 16> canonical{};
    for (size_t i = 0; i < canonical.size(); ++i)
        canonical[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    std::array<uint8_t, 16> le = canonical;
    std::reverse(le.begin(), le.begin() + 4);
    std::reverse(le.begin() + 4, le.begin() + 6);
    std::reverse(le.begin() + 6, le.begin() + 8);
    return le;
}

bool contains(const std::vector<uint8_t> &blob, const std::array<uint8_t, 16> &needle) {
    return std::search(blob.begin(), blob.end(), needle.begin(), needle.end()) != blob.end();
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: ipc4-dmic-pair-check firmware.ri dmic-topology.tplg\n";
        return 2;
    }
    try {
        const auto firmwareBytes = readFile(argv[1]);
        const auto topologyBytes = readFile(argv[2]);
        const auto firmware = sof::ipc4::FirmwareManifest::parse(
            firmwareBytes.data(), firmwareBytes.size());

        bool okay = true;
        for (const auto &required : kDmicModules) {
            const auto expected = uuidBytesLe(required.uuid);
            const bool inTopology = contains(topologyBytes, expected);
            const auto module = std::find_if(firmware.modules.begin(), firmware.modules.end(),
                [&expected](const sof::ipc4::Module &candidate) {
                    return candidate.uuid == expected;
                });
            if (!inTopology) {
                std::cerr << "topology is missing expected " << required.name << " UUID\n";
                okay = false;
            } else if (module == firmware.modules.end()) {
                std::cerr << "firmware " << firmware.imageName << ' '
                          << firmware.version[0] << '.' << firmware.version[1] << '.'
                          << firmware.version[2] << '.' << firmware.version[3]
                          << " lacks topology module " << required.name << " UUID "
                          << required.uuid << '\n';
                okay = false;
            } else {
                std::cout << required.name << " UUID matched firmware module index "
                          << module->id << " (manifest name " << module->name << ")\n";
            }
        }
        if (!okay) return 1;
        std::cout << "firmware/topology DMIC module UUID set is compatible; this is a "
                     "static check only\n";
    } catch (const std::exception &error) {
        std::cerr << "pair check failed: " << error.what() << '\n';
        return 2;
    }
    return 0;
}
