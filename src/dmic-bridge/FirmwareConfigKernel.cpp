#include "FirmwareConfigKernel.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kReplyDirection = 1U << 29;
constexpr uint32_t kModuleTarget = 1U << 30;
constexpr uint32_t kReplyStatusMask = 0x00ffffffU;
constexpr uint32_t kFwConfigLargeGetType = 3U;
constexpr uint32_t kMaxPipelineTupleType = 9U;

uint32_t readLittle32(const uint8_t *bytes) noexcept {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

bool allZero(const uint8_t *bytes, size_t length) noexcept {
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] != 0) return false;
    }
    return true;
}

} // namespace

FirmwareConfigResult makeFirmwareConfigRequest(size_t replyCapacityBytes,
                                              Message *out) noexcept {
    if (!out || replyCapacityBytes == 0 ||
        replyCapacityBytes > kMaximumPayloadBytes ||
        (replyCapacityBytes & 3U) != 0)
        return FirmwareConfigResult::InvalidArgument;
    const Result result = largeConfigGet(0, 0, 7,
                                         static_cast<uint32_t>(replyCapacityBytes), out);
    return result == Result::Ok ? FirmwareConfigResult::Ok
                                : FirmwareConfigResult::InvalidArgument;
}

FirmwareConfigResult parseFirmwareConfigReply(uint32_t replyPrimary,
                                              const uint8_t *payload,
                                              size_t payloadBytes,
                                              uint16_t *maxPipelineIdInclusive) noexcept {
    if (!payload || payloadBytes == 0 || payloadBytes > kMaximumPayloadBytes ||
        !maxPipelineIdInclusive)
        return FirmwareConfigResult::InvalidArgument;
    if ((replyPrimary & kReplyDirection) == 0 ||
        (replyPrimary & kModuleTarget) == 0 ||
        ((replyPrimary >> 24) & 0x1fU) != kFwConfigLargeGetType)
        return FirmwareConfigResult::WrongReplyHeader;
    if ((replyPrimary & kReplyStatusMask) != 0)
        return FirmwareConfigResult::FirmwareError;

    size_t offset = 0;
    bool found = false;
    uint32_t limit = 0;
    while (offset < payloadBytes) {
        if (allZero(payload + offset, payloadBytes - offset)) break;
        if (payloadBytes - offset < 8)
            return FirmwareConfigResult::TruncatedTuple;
        const uint32_t type = readLittle32(payload + offset);
        const uint32_t valueBytes = readLittle32(payload + offset + 4);
        if (valueBytes > payloadBytes - offset - 8)
            return FirmwareConfigResult::TruncatedTuple;
        if (type == kMaxPipelineTupleType) {
            if (found) return FirmwareConfigResult::DuplicatePipelineLimit;
            if (valueBytes != 4) return FirmwareConfigResult::TruncatedTuple;
            limit = readLittle32(payload + offset + 8);
            found = true;
        }
        offset += 8 + valueBytes;
    }
    if (!found) return FirmwareConfigResult::MissingPipelineLimit;
    if (limit == 0 || limit > 255) return FirmwareConfigResult::InvalidPipelineLimit;
    *maxPipelineIdInclusive = static_cast<uint16_t>(limit);
    return FirmwareConfigResult::Ok;
}

} // namespace sof::ipc4::kernel
