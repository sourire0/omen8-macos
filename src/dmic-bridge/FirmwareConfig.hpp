#pragma once

#include <cstddef>
#include <cstdint>
#include "Ipc4Messages.hpp"

namespace sof::ipc4 {

// Extracts SOF_IPC4_FW_CFG_MAX_PPL_COUNT from the FW_CONFIG reply's tuple
// buffer. This is offline parsing only; the caller must provide the exact
// returned byte count from a transport implementation.
uint16_t parseMaxPipelineIdInclusive(const uint8_t *data, size_t size);

// Validates a successful IPC4 module LARGE_CONFIG_GET reply header, then
// parses its FW_CONFIG tuple payload. `payloadBytes` must be the exact
// returned byte count, not the mailbox capacity.
uint16_t parseFirmwareConfigReply(uint32_t replyPrimary,
                                  const uint8_t *payload,
                                  size_t payloadBytes);

struct FirmwareConfigRequest {
    Message message;
    size_t replyCapacityBytes{};
};

FirmwareConfigRequest makeFirmwareConfigRequest(size_t replyCapacityBytes);

} // namespace sof::ipc4
