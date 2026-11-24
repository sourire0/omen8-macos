#include "FirmwareKernelData.hpp"
#include "generated/SofFirmwareKernelData.hpp"

namespace sof::ipc4::kernel {

const uint8_t *sofFirmwareImage() noexcept { return kSofFirmwareImage; }
size_t sofFirmwareImageBytes() noexcept { return kSofFirmwareImageBytes; }
const char *sofFirmwareImageSha256() noexcept {
    return kSofFirmwareImageSha256;
}

} // namespace sof::ipc4::kernel
