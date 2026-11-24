#pragma once

#include "Ipc4KernelCodec.hpp"

#include <stddef.h>
#include <stdint.h>

namespace sof::ipc4::kernel {

constexpr uint16_t kHipctdrOffset = 0x00c0;
constexpr uint16_t kHipctdaOffset = 0x00c4;
constexpr uint16_t kHipctddOffset = 0x00c8;
constexpr uint16_t kHipcidrOffset = 0x00d0;
constexpr uint16_t kHipcidaOffset = 0x00d4;
constexpr uint16_t kHipciddOffset = 0x00d8;

struct RegisterWrite {
    uint16_t offset;
    uint32_t value;
};

struct DoorbellWritePlan {
    RegisterWrite writes[2];
    uint8_t count;
};

// Output doorbell writes only. The caller stages the downlink payload first,
// verifies live request/ack registers are idle, and serializes the transaction.
Result planHostDoorbell(const Message *message, size_t mailboxCapacity,
                        DoorbellWritePlan *out) noexcept;
void planHostDoneAcknowledgement(DoorbellWritePlan *out) noexcept;
void planIncomingAcknowledgement(DoorbellWritePlan *out) noexcept;

} // namespace sof::ipc4::kernel
