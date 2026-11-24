#pragma once

#include "Ipc4KernelTransport.hpp"

#include <stddef.h>
#include <stdint.h>

namespace sof::ipc4::kernel::mmio_policy {

constexpr uint32_t kDoorbellAck = 1U << 31;
constexpr size_t kMaximumMailboxBytes = kMaximumPayloadBytes;

constexpr bool supportedRegister(uint16_t offset) noexcept {
    return offset == kHipctdrOffset || offset == kHipctdaOffset ||
           offset == kHipctddOffset || offset == kHipcidrOffset ||
           offset == kHipcidaOffset || offset == kHipciddOffset;
}

constexpr bool allowedRegisterWrite(uint16_t offset, uint32_t value,
                                    bool firmwareReady) noexcept {
    if (offset == kHipcidaOffset || offset == kHipctdrOffset ||
        offset == kHipctdaOffset)
        return value == kDoorbellAck;
    if (!firmwareReady) return false;
    if (offset == kHipciddOffset) return (value & kDoorbellAck) == 0;
    if (offset == kHipcidrOffset) return (value & kDoorbellAck) != 0;
    return false;
}

constexpr bool validMailboxRange(uint64_t mapLength, uint32_t offset,
                                 size_t length,
                                 bool writeToDevice) noexcept {
    const uint32_t expectedOffset = writeToDevice
        ? kCavs25DownlinkMailboxOffset : kCavs25UplinkMailboxOffset;
    if (offset != expectedOffset || length == 0 ||
        length > kMaximumMailboxBytes || (length & 3U) != 0)
        return false;
    return static_cast<uint64_t>(offset) <= mapLength &&
           static_cast<uint64_t>(length) <= mapLength - offset;
}

} // namespace sof::ipc4::kernel::mmio_policy
