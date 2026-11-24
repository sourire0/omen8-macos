#include "Ipc4Header.hpp"

#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>

static uint32_t parseWord(const char *text) {
    char *end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 0);
    if (!text[0] || end == text || *end != '\0' || value > 0xffffffffUL)
        throw std::runtime_error("expected a 32-bit integer (decimal or 0x-prefixed)");
    return static_cast<uint32_t>(value);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: ipc4-header-decode primary extension\n";
        return 2;
    }
    try {
        sof::ipc4::HeaderFields fields{};
        if (!sof::ipc4::decodeHeader(parseWord(argv[1]), parseWord(argv[2]), &fields))
            throw std::runtime_error("null IPC4 header output");
        std::cout << "type=" << static_cast<unsigned>(fields.type)
                  << " module=" << fields.moduleTarget
                  << " reply=" << fields.reply
                  << " notification=" << fields.notification
                  << " notification_type=" << static_cast<unsigned>(fields.notificationType)
                  << " fw_ready=" << fields.fwReady << '\n';
    } catch (const std::exception &error) {
        std::cerr << "decode failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
