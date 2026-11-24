#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sof::ipc4::kernel {

const uint8_t *sofFirmwareImage() noexcept;
size_t sofFirmwareImageBytes() noexcept;
const char *sofFirmwareImageSha256() noexcept;

} // namespace sof::ipc4::kernel
