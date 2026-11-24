#include "FirmwareManifest.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace sof::ipc4 {
namespace {

constexpr size_t kExtendedHeaderSize = 16;
constexpr size_t kFwHeaderSize = 52;
constexpr size_t kModuleEntrySize = 116;
constexpr size_t kModuleConfigSize = 44;
constexpr size_t kFirmwareHeaderOffset = 0x2000;
constexpr uint32_t kMaxModules = 4096;

void requireRange(size_t size, size_t offset, size_t length, const char *what) {
    if (offset > size || length > size - offset)
        throw std::runtime_error(std::string(what) + " is outside the image");
}

uint16_t read16(const uint8_t *data, size_t size, size_t offset, const char *what) {
    requireRange(size, offset, 2, what);
    return static_cast<uint16_t>(data[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t read32(const uint8_t *data, size_t size, size_t offset, const char *what) {
    requireRange(size, offset, 4, what);
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

std::string fixedString(const uint8_t *data, size_t length) {
    size_t end = 0;
    while (end < length && data[end] != 0) ++end;
    return std::string(reinterpret_cast<const char *>(data), end);
}

size_t checkedAdd(size_t a, size_t b, const char *what) {
    if (b > std::numeric_limits<size_t>::max() - a)
        throw std::runtime_error(std::string("overflow calculating ") + what);
    return a + b;
}

size_t checkedMul(size_t a, size_t b, const char *what) {
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a)
        throw std::runtime_error(std::string("overflow calculating ") + what);
    return a * b;
}

} // namespace

std::string uuidString(const std::array<uint8_t, 16> &u) {
    // GUID byte order on disk is little-endian for the first 3 fields.
    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(2) << static_cast<unsigned>(u[3])
        << std::setw(2) << static_cast<unsigned>(u[2])
        << std::setw(2) << static_cast<unsigned>(u[1])
        << std::setw(2) << static_cast<unsigned>(u[0]) << '-'
        << std::setw(2) << static_cast<unsigned>(u[5])
        << std::setw(2) << static_cast<unsigned>(u[4]) << '-'
        << std::setw(2) << static_cast<unsigned>(u[7])
        << std::setw(2) << static_cast<unsigned>(u[6]) << '-'
        << std::setw(2) << static_cast<unsigned>(u[8])
        << std::setw(2) << static_cast<unsigned>(u[9]) << '-';
    for (size_t i = 10; i < u.size(); ++i)
        out << std::setw(2) << static_cast<unsigned>(u[i]);
    return out.str();
}

FirmwareManifest FirmwareManifest::parse(const uint8_t *data, size_t size) {
    if (data == nullptr) throw std::runtime_error("null image buffer");
    requireRange(size, 0, kExtendedHeaderSize, "extended manifest header");
    if (std::string_view(reinterpret_cast<const char *>(data), 4) != "$AE1")
        throw std::runtime_error("expected $AE1 IPC4 extended manifest");

    FirmwareManifest result;
    result.extensionLength = read32(data, size, 4, "extended manifest length");
    result.extensionMajor = read16(data, size, 8, "extended manifest major version");
    result.extensionMinor = read16(data, size, 10, "extended manifest minor version");
    result.moduleCount = read32(data, size, 12, "extended manifest module count");
    if (result.extensionLength < kExtendedHeaderSize || result.extensionLength > size)
        throw std::runtime_error("invalid extended manifest length");
    if (result.moduleCount == 0 || result.moduleCount > kMaxModules)
        throw std::runtime_error("unreasonable module count");

    const size_t fwHeaderOffset = checkedAdd(result.extensionLength,
                                             kFirmwareHeaderOffset,
                                             "base firmware header offset");
    requireRange(size, fwHeaderOffset, kFwHeaderSize, "base firmware header");
    if (std::string_view(reinterpret_cast<const char *>(data + fwHeaderOffset), 4) != "$AM1")
        throw std::runtime_error("expected $AM1 at ext_len + 0x2000");

    const uint32_t fwHeaderLength = read32(data, size, fwHeaderOffset + 4,
                                           "base firmware header length");
    if (fwHeaderLength < kFwHeaderSize)
        throw std::runtime_error("base firmware header is shorter than the known ABI");
    requireRange(size, fwHeaderOffset, fwHeaderLength, "full base firmware header");
    result.imageName = fixedString(data + fwHeaderOffset + 8, 8);
    const size_t versionOffset = fwHeaderOffset + 28;
    for (size_t i = 0; i < result.version.size(); ++i)
        result.version[i] = read16(data, size, versionOffset + i * 2,
                                   "firmware version field");
    const uint32_t headerModuleCount = read32(data, size, fwHeaderOffset + 36,
                                               "base firmware module count");
    if (headerModuleCount != result.moduleCount)
        throw std::runtime_error("extension and base firmware module counts differ");

    const size_t moduleTableOffset = checkedAdd(fwHeaderOffset, fwHeaderLength,
                                                "module table offset");
    const size_t moduleTableBytes = checkedMul(result.moduleCount, kModuleEntrySize,
                                               "module table size");
    requireRange(size, moduleTableOffset, moduleTableBytes, "module table");

    result.modules.reserve(result.moduleCount);
    size_t maxConfigEnd = 0;
    for (size_t i = 0; i < result.moduleCount; ++i) {
        const size_t off = moduleTableOffset + i * kModuleEntrySize;
        Module module;
        if (std::string_view(reinterpret_cast<const char *>(data + off), 4) != "$AME")
            throw std::runtime_error("module table entry does not start with $AME");
        module.id = static_cast<uint32_t>(i);
        module.name = fixedString(data + off + 4, 8);
        std::copy_n(data + off + 12, module.uuid.size(), module.uuid.begin());
        module.type = read32(data, size, off + 28, "module type");
        module.configOffset = read16(data, size, off + 68, "module config offset");
        module.configCount = read16(data, size, off + 70, "module config count");
        module.affinityMask = read32(data, size, off + 72, "module affinity mask");
        module.instanceMaxCount = read16(data, size, off + 76, "module max instances");
        module.instanceStackSize = read16(data, size, off + 78, "module instance stack size");
        const size_t configEnd = static_cast<size_t>(module.configOffset) + module.configCount;
        maxConfigEnd = std::max(maxConfigEnd, configEnd);
        result.modules.push_back(std::move(module));
    }

    const size_t configOffset = checkedAdd(moduleTableOffset, moduleTableBytes,
                                           "module config table offset");
    const size_t configBytes = checkedMul(maxConfigEnd, kModuleConfigSize,
                                          "module config table size");
    requireRange(size, configOffset, configBytes, "module config table");
    return result;
}

} // namespace sof::ipc4
