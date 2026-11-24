#pragma once

#include "Ipc4KernelCodec.hpp"

#include <stddef.h>
#include <stdint.h>

namespace sof::ipc4::kernel {

enum class FirmwareConfigResult : uint8_t {
    Ok,
    InvalidArgument,
    WrongReplyHeader,
    FirmwareError,
    TruncatedTuple,
    DuplicatePipelineLimit,
    MissingPipelineLimit,
    InvalidPipelineLimit,
};

FirmwareConfigResult makeFirmwareConfigRequest(size_t replyCapacityBytes,
                                              Message *out) noexcept;
FirmwareConfigResult parseFirmwareConfigReply(uint32_t replyPrimary,
                                              const uint8_t *payload,
                                              size_t payloadBytes,
                                              uint16_t *maxPipelineIdInclusive) noexcept;

} // namespace sof::ipc4::kernel
