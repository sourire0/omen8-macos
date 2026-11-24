#include "Cavs25KernelTransportPlan.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kBusyDone = 1U << 31;
constexpr uint32_t kReplyDirection = 1U << 29;

} // namespace

Result planHostDoorbell(const Message *message, size_t mailboxCapacity,
                        DoorbellWritePlan *out) noexcept {
    if (!out) return Result::NullOutput;
    out->count = 0;
    if (!message) return Result::NullPayload;
    if (mailboxCapacity > kMaximumPayloadBytes ||
        message->payloadBytes > mailboxCapacity ||
        message->payloadBytes > kMaximumPayloadBytes ||
        (message->payloadBytes & 3U) != 0)
        return Result::PayloadTooLarge;
    if ((message->primary & (kBusyDone | kReplyDirection)) != 0 ||
        (message->extension & kBusyDone) != 0)
        return Result::InvalidField;
    out->writes[0].offset = kHipciddOffset;
    out->writes[0].value = message->extension;
    out->writes[1].offset = kHipcidrOffset;
    out->writes[1].value = message->primary | kBusyDone;
    out->count = 2;
    return Result::Ok;
}

void planHostDoneAcknowledgement(DoorbellWritePlan *out) noexcept {
    if (!out) return;
    out->writes[0].offset = kHipcidaOffset;
    out->writes[0].value = kBusyDone;
    out->count = 1;
}

void planIncomingAcknowledgement(DoorbellWritePlan *out) noexcept {
    if (!out) return;
    out->writes[0].offset = kHipctdrOffset;
    out->writes[0].value = kBusyDone;
    out->writes[1].offset = kHipctdaOffset;
    out->writes[1].value = kBusyDone;
    out->count = 2;
}

} // namespace sof::ipc4::kernel
