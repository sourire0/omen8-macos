#include "Ipc4Header.hpp"

namespace sof::ipc4 {

bool decodeHeader(uint32_t primary, uint32_t extension,
                  HeaderFields *out) noexcept {
    if (!out) return false;
    out->primary = primary;
    out->extension = extension;
    out->type = static_cast<uint8_t>((primary >> 24) & 0x1fU);
    out->moduleTarget = (primary & (1U << 30)) != 0;
    out->reply = (primary & (1U << 29)) != 0;
    out->notification = !out->moduleTarget && out->type == kGlobalNotification;
    out->notificationType = 0;
    out->fwReady = false;
    if (out->notification) {
        out->notificationType = static_cast<uint8_t>((primary >> 16) & 0xffU);
        out->fwReady = out->notificationType == kNotifyFwReady;
    }
    return true;
}

} // namespace sof::ipc4
