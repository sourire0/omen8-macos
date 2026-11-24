#include "Ipc4Transaction.hpp"

#include <stdexcept>

namespace sof::ipc4 {
namespace {

constexpr uint32_t kDoorbellBusy = 1U << 31;
constexpr uint32_t kHeaderValueMask = 0x7fffffffU;
constexpr uint32_t kReplyDirection = 1U << 29;
constexpr uint32_t kHostRequestDone = 1U << 31;

} // namespace

void Ipc4TransactionTracker::begin(const Message &request) {
    if (state_ == TransactionState::PoisonedAfterTimeout)
        throw std::logic_error("IPC4 transport remains poisoned until firmware reset");
    if (state_ != TransactionState::Idle)
        throw std::logic_error("IPC4 request already awaiting a reply");
    if ((request.primary & (kDoorbellBusy | kReplyDirection)) != 0)
        throw std::invalid_argument("begin expects a host request without BUSY or reply bits");
    state_ = TransactionState::AwaitingReply;
    replyObserved_ = false;
}

Ipc4Observation Ipc4TransactionTracker::observe(
    const Cavs25DoorbellSnapshot &snapshot) {
    Ipc4Observation result;
    result.hostRequestConsumed = (snapshot.hipcida & kHostRequestDone) != 0;
    result.hostRequestDoneNeedsAcknowledgement = result.hostRequestConsumed;
    if ((snapshot.hipctdr & kDoorbellBusy) == 0) return result;

    result.incomingNeedsAcknowledgement = true;
    const uint32_t primary = snapshot.hipctdr & kHeaderValueMask;
    const uint32_t extension = snapshot.hipctdd & kHeaderValueMask;
    decodeHeader(primary, extension, &result.header);
    if (result.header.notification) {
        result.incoming = IncomingKind::Notification;
    } else if ((primary & kReplyDirection) != 0) {
        if (state_ == TransactionState::AwaitingReply && !replyObserved_) {
            result.incoming = IncomingKind::Reply;
            result.completesOutstandingRequest = true;
            replyObserved_ = true;
        } else {
            result.incoming = IncomingKind::Unsolicited;
        }
    } else {
        result.incoming = IncomingKind::Unsolicited;
    }
    return result;
}

void Ipc4TransactionTracker::completeReply() {
    if (state_ != TransactionState::AwaitingReply || !replyObserved_)
        throw std::logic_error("cannot complete IPC4 request without an observed reply");
    state_ = TransactionState::Idle;
    replyObserved_ = false;
}

void Ipc4TransactionTracker::markTimeout() {
    if (state_ != TransactionState::AwaitingReply)
        throw std::logic_error("timeout without an in-flight IPC4 request");
    state_ = TransactionState::PoisonedAfterTimeout;
    replyObserved_ = false;
}

void Ipc4TransactionTracker::resetAfterFirmwareReset() noexcept {
    state_ = TransactionState::Idle;
    replyObserved_ = false;
}

} // namespace sof::ipc4
