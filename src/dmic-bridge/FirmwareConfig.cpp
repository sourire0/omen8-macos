#include "FirmwareConfig.hpp"

#include <limits>
#include <algorithm>
#include <stdexcept>

namespace sof::ipc4 {
namespace {

constexpr uint32_t kFwCfgMaxPipelineCount = 9;
constexpr uint8_t kFwConfigParameterId = 7;
constexpr uint32_t kReplyDirection = 1U << 29;
constexpr uint32_t kModuleTarget = 1U << 30;
constexpr uint32_t kMessageTypeMask = 0x1fU;
constexpr uint32_t kLargeConfigGetType = 3U;
constexpr uint32_t kReplyStatusMask = 0x00ffffffU;
constexpr size_t kIpc4MaxPayloadBytes = 4096;
constexpr size_t kTupleHeaderBytes = 8;
constexpr size_t kDwordBytes = 4;

uint32_t read32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

} // namespace

FirmwareConfigRequest makeFirmwareConfigRequest(size_t replyCapacityBytes) {
    if (replyCapacityBytes == 0 || replyCapacityBytes > kIpc4MaxPayloadBytes ||
        (replyCapacityBytes & 3U) != 0)
        throw std::invalid_argument("FW_CONFIG reply capacity must be DWORD-aligned and <= 4096");
    FirmwareConfigRequest request;
    request.message = largeConfigGet(0, 0, kFwConfigParameterId,
                                     static_cast<uint32_t>(replyCapacityBytes));
    request.replyCapacityBytes = replyCapacityBytes;
    validateMailboxRequest(request.message, kIpc4MaxPayloadBytes);
    return request;
}

uint16_t parseMaxPipelineIdInclusive(const uint8_t *data, size_t size) {
    if (!data || size == 0) throw std::invalid_argument("empty FW_CONFIG tuple buffer");

    size_t offset = 0;
    bool found = false;
    uint32_t result = 0;
    while (offset < size) {
        if (std::all_of(data + offset, data + size,
                        [](uint8_t byte) { return byte == 0; }))
            break; // fixed-size FW_CONFIG reply buffer may be zero-padded
        if (size - offset < kTupleHeaderBytes)
            throw std::runtime_error("truncated FW_CONFIG tuple header");
        const uint32_t type = read32(data + offset);
        const uint32_t valueBytes = read32(data + offset + 4);
        if (valueBytes > size - offset - kTupleHeaderBytes)
            throw std::runtime_error("FW_CONFIG tuple exceeds returned buffer");
        if (type == kFwCfgMaxPipelineCount) {
            if (found) throw std::runtime_error("duplicate FW_CONFIG max-pipeline tuple");
            if (valueBytes != kDwordBytes)
                throw std::runtime_error("FW_CONFIG max-pipeline tuple must contain one DWORD");
            result = read32(data + offset + kTupleHeaderBytes);
            found = true;
        }
        offset += kTupleHeaderBytes + valueBytes;
    }
    if (!found) throw std::runtime_error("FW_CONFIG lacks max-pipeline tuple");
    if (result == 0 || result > std::numeric_limits<uint8_t>::max())
        throw std::runtime_error("FW_CONFIG pipeline ceiling is invalid for IPC4 header");
    // SOF passes max_num_pipelines as the inclusive upper bound to ida_alloc_max.
    return static_cast<uint16_t>(result);
}

uint16_t parseFirmwareConfigReply(uint32_t replyPrimary,
                                  const uint8_t *payload,
                                  size_t payloadBytes) {
    if ((replyPrimary & kReplyDirection) == 0)
        throw std::runtime_error("FW_CONFIG response lacks IPC4 reply direction bit");
    if ((replyPrimary & kModuleTarget) == 0 ||
        ((replyPrimary >> 24) & kMessageTypeMask) != kLargeConfigGetType)
        throw std::runtime_error("FW_CONFIG response is not a module LARGE_CONFIG_GET reply");
    if ((replyPrimary & kReplyStatusMask) != 0)
        throw std::runtime_error("FW_CONFIG LARGE_CONFIG_GET returned a firmware error status");
    return parseMaxPipelineIdInclusive(payload, payloadBytes);
}

} // namespace sof::ipc4
