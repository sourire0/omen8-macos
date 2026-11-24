#include "Ipc4KernelTransaction.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kBusyDone = 1U << 31;
constexpr uint32_t kReplyDirection = 1U << 29;
constexpr uint32_t kHeaderValueMask = 0x7fffffffU;

} // namespace

TransactionResult TransactionTracker::begin(uint32_t hostPrimary) noexcept {
    if (state_ != TransactionState::Idle) return TransactionResult::InvalidState;
    if ((hostPrimary & (kBusyDone | kReplyDirection)) != 0)
        return TransactionResult::InvalidHeader;
    state_ = TransactionState::AwaitingReply;
    replyObserved_ = false;
    return TransactionResult::Ok;
}

TransactionResult TransactionTracker::observe(const DoorbellSnapshot &snapshot,
                                               Observation *out) noexcept {
    if (!out) return TransactionResult::NullOutput;
    out->hostRequestConsumed = (snapshot.hipcida & kBusyDone) != 0;
    out->hostRequestDoneNeedsAcknowledgement = out->hostRequestConsumed;
    out->incomingNeedsAcknowledgement = false;
    out->completesOutstandingRequest = false;
    out->incoming = IncomingKind::None;
    out->header.primary = 0;
    out->header.extension = 0;
    out->header.type = 0;
    out->header.notificationType = 0;
    out->header.moduleTarget = false;
    out->header.reply = false;
    out->header.notification = false;
    out->header.fwReady = false;
    if ((snapshot.hipctdr & kBusyDone) == 0) return TransactionResult::Ok;

    out->incomingNeedsAcknowledgement = true;
    const uint32_t primary = snapshot.hipctdr & kHeaderValueMask;
    decodeHeader(primary, snapshot.hipctdd & kHeaderValueMask, &out->header);
    if (out->header.notification) {
        out->incoming = IncomingKind::Notification;
    } else if ((primary & kReplyDirection) != 0) {
        if (state_ == TransactionState::AwaitingReply && !replyObserved_) {
            out->incoming = IncomingKind::Reply;
            out->completesOutstandingRequest = true;
            replyObserved_ = true;
        } else {
            out->incoming = IncomingKind::Unsolicited;
        }
    } else {
        out->incoming = IncomingKind::Unsolicited;
    }
    return TransactionResult::Ok;
}

TransactionResult TransactionTracker::completeReply() noexcept {
    if (state_ != TransactionState::AwaitingReply || !replyObserved_)
        return TransactionResult::InvalidState;
    state_ = TransactionState::Idle;
    replyObserved_ = false;
    return TransactionResult::Ok;
}

TransactionResult TransactionTracker::markTimeout() noexcept {
    if (state_ != TransactionState::AwaitingReply)
        return TransactionResult::InvalidState;
    state_ = TransactionState::PoisonedAfterTimeout;
    replyObserved_ = false;
    return TransactionResult::Ok;
}

void TransactionTracker::resetAfterFirmwareReset() noexcept {
    state_ = TransactionState::Idle;
    replyObserved_ = false;
}

} // namespace sof::ipc4::kernel
