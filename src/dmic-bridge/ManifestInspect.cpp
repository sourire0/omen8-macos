#include "FirmwareManifest.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: ipc4-manifest-inspect firmware.ri\n";
        return 2;
    }
    try {
        std::ifstream file(argv[1], std::ios::binary);
        if (!file) throw std::runtime_error("cannot open input image");
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
        const auto manifest = sof::ipc4::FirmwareManifest::parse(bytes.data(), bytes.size());
        std::cout << "IPC4 firmware image=" << manifest.imageName
                  << " version=" << manifest.version[0] << '.' << manifest.version[1]
                  << '.' << manifest.version[2] << '.' << manifest.version[3]
                  << " extension=" << manifest.extensionMajor << '.' << manifest.extensionMinor
                  << " modules=" << manifest.modules.size() << '\n';
        for (const auto &module : manifest.modules)
            std::cout << module.id << "  " << module.name << "  "
                      << sof::ipc4::uuidString(module.uuid) << "  cfg="
                      << module.configOffset << '+' << module.configCount << '\n';
    } catch (const std::exception &error) {
        std::cerr << "parse failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
