#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sof::ipc4::kernel {

enum class FirmwareImageHashResult : uint8_t {
    Ok,
    InvalidArgument,
    UnsupportedSize,
    WrongDigest,
};

// Verifies the complete .ri file against the SHA-256 of the Intel-signed
// Alder Lake IPC4 v2.12 image in the official SOF binary release. This is an
// exact-file allowlist check; it does not parse or substitute for Intel's
// cryptographic signature verification.
FirmwareImageHashResult verifyKnownAdlIpc4V212Image(
    const uint8_t *image, size_t imageBytes) noexcept;

} // namespace sof::ipc4::kernel
