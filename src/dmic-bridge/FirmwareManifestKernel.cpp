#include "FirmwareManifestKernel.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr size_t kExtendedHeaderSize = 16;
constexpr size_t kFirmwareHeaderSize = 52;
constexpr size_t kModuleEntrySize = 116;
constexpr size_t kModuleConfigSize = 44;
constexpr size_t kFirmwareHeaderOffset = 0x2000;
constexpr uint32_t kSegmentContainsFileData = 1U << 0;
constexpr uint32_t kSegmentPageShift = 16;
constexpr uint32_t kFirmwarePageBytes = 4096;

bool inRange(size_t size, size_t offset, size_t length) noexcept {
    return offset <= size && length <= size - offset;
}

uint16_t read16(const uint8_t *data, size_t offset) noexcept {
    return static_cast<uint16_t>(data[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t read32(const uint8_t *data, size_t offset) noexcept {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

bool hasMagic(const uint8_t *data, size_t offset, const char *magic) noexcept {
    for (size_t i = 0; i < 4; ++i) {
        if (data[offset + i] != static_cast<uint8_t>(magic[i])) return false;
    }
    return true;
}

void copyFixedString(const uint8_t *source, size_t sourceBytes,
                     char *destination) noexcept {
    size_t i = 0;
    while (i < sourceBytes && source[i] != 0) {
        destination[i] = static_cast<char>(source[i]);
        ++i;
    }
    destination[i] = '\0';
}

} // namespace

FirmwareManifestResult parseFirmwareManifest(
    const uint8_t *data, size_t size,
    FirmwareManifestView *out) noexcept {
    if (!data || !out) return FirmwareManifestResult::InvalidArgument;
    out->moduleCount = 0;
    if (!inRange(size, 0, kExtendedHeaderSize))
        return FirmwareManifestResult::TruncatedImage;
    if (!hasMagic(data, 0, "$AE1"))
        return FirmwareManifestResult::WrongExtendedManifestMagic;

    const uint32_t extensionLength = read32(data, 4);
    const uint16_t extensionMajor = read16(data, 8);
    const uint16_t extensionMinor = read16(data, 10);
    const uint32_t moduleCount = read32(data, 12);
    if (extensionLength < kExtendedHeaderSize || extensionLength > size ||
        moduleCount == 0 || moduleCount > kMaximumFirmwareModules)
        return FirmwareManifestResult::InvalidExtendedManifest;
    if (static_cast<size_t>(extensionLength) >
        static_cast<size_t>(-1) - kFirmwareHeaderOffset)
        return FirmwareManifestResult::InvalidExtendedManifest;

    const size_t fwOffset = static_cast<size_t>(extensionLength) +
                            kFirmwareHeaderOffset;
    if (!inRange(size, fwOffset, kFirmwareHeaderSize))
        return FirmwareManifestResult::TruncatedImage;
    if (!hasMagic(data, fwOffset, "$AM1"))
        return FirmwareManifestResult::WrongFirmwareHeaderMagic;

    const uint32_t fwHeaderLength = read32(data, fwOffset + 4);
    if (fwHeaderLength < kFirmwareHeaderSize ||
        !inRange(size, fwOffset, fwHeaderLength))
        return FirmwareManifestResult::InvalidFirmwareHeader;
    if (read32(data, fwOffset + 36) != moduleCount)
        return FirmwareManifestResult::ModuleCountMismatch;
    if (fwOffset > static_cast<size_t>(-1) - fwHeaderLength)
        return FirmwareManifestResult::InvalidFirmwareHeader;

    const size_t moduleTableOffset = fwOffset + fwHeaderLength;
    if (static_cast<size_t>(moduleCount) >
        static_cast<size_t>(-1) / kModuleEntrySize)
        return FirmwareManifestResult::InvalidModuleTable;
    const size_t moduleTableBytes = static_cast<size_t>(moduleCount) *
                                    kModuleEntrySize;
    if (!inRange(size, moduleTableOffset, moduleTableBytes))
        return FirmwareManifestResult::InvalidModuleTable;
    const size_t configTableOffset = moduleTableOffset + moduleTableBytes;
    size_t maxConfigEnd = 0;

    out->extensionLength = extensionLength;
    out->extensionMajor = extensionMajor;
    out->extensionMinor = extensionMinor;
    copyFixedString(data + fwOffset + 8, 8, out->imageName);
    out->preloadPageCount = read32(data, fwOffset + 16);
    out->firmwareImageFlags = read32(data, fwOffset + 20);
    out->featureMask = read32(data, fwOffset + 24);
    for (size_t i = 0; i < 4; ++i)
        out->version[i] = read16(data, fwOffset + 28 + i * 2);
    out->hardwareBufferBaseAddress = read32(data, fwOffset + 40);
    out->hardwareBufferLength = read32(data, fwOffset + 44);
    out->loadOffset = read32(data, fwOffset + 48);

    for (size_t i = 0; i < moduleCount; ++i) {
        const size_t offset = moduleTableOffset + i * kModuleEntrySize;
        if (!hasMagic(data, offset, "$AME"))
            return FirmwareManifestResult::InvalidModuleRecord;
        FirmwareModuleInfo &module = out->modules[i];
        module.id = static_cast<uint32_t>(i);
        copyFixedString(data + offset + 4, 8, module.name);
        for (size_t byte = 0; byte < 16; ++byte)
            module.uuid[byte] = data[offset + 12 + byte];
        module.type = read32(data, offset + 28);
        for (size_t byte = 0; byte < sizeof(module.sha256); ++byte)
            module.sha256[byte] = data[offset + 32 + byte];
        module.entryPoint = read32(data, offset + 64);
        module.configOffset = read16(data, offset + 68);
        module.configCount = read16(data, offset + 70);
        module.affinityMask = read32(data, offset + 72);
        module.instanceMaxCount = read16(data, offset + 76);
        module.instanceStackSize = read16(data, offset + 78);
        for (size_t segmentIndex = 0;
             segmentIndex < kFirmwareSegmentCount; ++segmentIndex) {
            const size_t segmentOffset = offset + 80 + segmentIndex * 12;
            FirmwareSegmentInfo &segment = module.segments[segmentIndex];
            segment.flags = read32(data, segmentOffset);
            segment.virtualAddress = read32(data, segmentOffset + 4);
            segment.fileOffset = read32(data, segmentOffset + 8);
            const uint16_t segmentPages = static_cast<uint16_t>(
                segment.flags >> kSegmentPageShift);
            segment.sizeBytes = static_cast<uint32_t>(segmentPages) *
                                kFirmwarePageBytes;
            if ((segment.flags & kSegmentContainsFileData) != 0 &&
                !inRange(size, segment.fileOffset, segment.sizeBytes))
                return FirmwareManifestResult::InvalidModuleSegment;
        }
        const size_t configEnd = static_cast<size_t>(module.configOffset) +
                                 module.configCount;
        if (configEnd > maxConfigEnd) maxConfigEnd = configEnd;
    }

    if (maxConfigEnd > static_cast<size_t>(-1) / kModuleConfigSize ||
        !inRange(size, configTableOffset, maxConfigEnd * kModuleConfigSize))
        return FirmwareManifestResult::InvalidModuleConfigTable;
    out->moduleCount = moduleCount;
    return FirmwareManifestResult::Ok;
}

const FirmwareModuleInfo *findFirmwareModuleByUuid(
    const FirmwareManifestView *manifest, const uint8_t uuid[16]) noexcept {
    if (!manifest || !uuid || manifest->moduleCount == 0 ||
        manifest->moduleCount > kMaximumFirmwareModules)
        return nullptr;
    for (size_t i = 0; i < manifest->moduleCount; ++i) {
        bool matches = true;
        for (size_t byte = 0; byte < 16; ++byte) {
            if (manifest->modules[i].uuid[byte] != uuid[byte]) {
                matches = false;
                break;
            }
        }
        if (matches) return &manifest->modules[i];
    }
    return nullptr;
}

} // namespace sof::ipc4::kernel
