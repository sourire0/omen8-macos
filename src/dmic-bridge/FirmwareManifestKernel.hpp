#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sof::ipc4::kernel {

constexpr size_t kMaximumFirmwareModules = 64;
constexpr size_t kFirmwareSegmentCount = 3;

struct FirmwareSegmentInfo {
    uint32_t flags;
    uint32_t virtualAddress;
    uint32_t fileOffset;
    uint32_t sizeBytes;
};

struct FirmwareModuleInfo {
    uint32_t id;
    char name[9];
    uint8_t uuid[16];
    uint32_t type;
    uint8_t sha256[32];
    uint32_t entryPoint;
    uint16_t configOffset;
    uint16_t configCount;
    uint32_t affinityMask;
    uint16_t instanceMaxCount;
    uint16_t instanceStackSize;
    FirmwareSegmentInfo segments[kFirmwareSegmentCount];
};

// About 8.3 KiB: keep this view in driver-owned storage, never on a kernel
// stack. The parser checks manifest/segment layout and file bounds; it does
// not authenticate signatures, verify module hashes, or establish board
// compatibility. Ignore the view unless parsing returns Ok.
struct FirmwareManifestView {
    uint32_t extensionLength;
    uint16_t extensionMajor;
    uint16_t extensionMinor;
    uint32_t moduleCount;
    char imageName[9];
    uint32_t preloadPageCount;
    uint32_t firmwareImageFlags;
    uint32_t featureMask;
    uint16_t version[4];
    uint32_t hardwareBufferBaseAddress;
    uint32_t hardwareBufferLength;
    uint32_t loadOffset;
    FirmwareModuleInfo modules[kMaximumFirmwareModules];
};

enum class FirmwareManifestResult : uint8_t {
    Ok,
    InvalidArgument,
    TruncatedImage,
    WrongExtendedManifestMagic,
    InvalidExtendedManifest,
    WrongFirmwareHeaderMagic,
    InvalidFirmwareHeader,
    ModuleCountMismatch,
    InvalidModuleTable,
    InvalidModuleRecord,
    InvalidModuleSegment,
    InvalidModuleConfigTable,
};

FirmwareManifestResult parseFirmwareManifest(
    const uint8_t *data, size_t size,
    FirmwareManifestView *out) noexcept;
const FirmwareModuleInfo *findFirmwareModuleByUuid(
    const FirmwareManifestView *manifest, const uint8_t uuid[16]) noexcept;

} // namespace sof::ipc4::kernel
