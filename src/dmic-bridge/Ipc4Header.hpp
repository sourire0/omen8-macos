#pragma once

#include <cstdint>

namespace sof::ipc4 {

constexpr uint8_t kGlobalNotification = 27;
constexpr uint8_t kNotifyFwReady = 8;

struct HeaderFields {
    uint32_t primary;
    uint32_t extension;
    uint8_t type;
    uint8_t notificationType;
    bool moduleTarget;
    bool reply;
    bool notification;
    bool fwReady;
};

bool decodeHeader(uint32_t primary, uint32_t extension,
                  HeaderFields *out) noexcept;

} // namespace sof::ipc4
