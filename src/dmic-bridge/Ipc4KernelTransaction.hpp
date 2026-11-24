#pragma once

#include "Ipc4Header.hpp"

#include <stdint.h>

namespace sof::ipc4::kernel {

enum class TransactionState : uint8_t {
    Idle,
    AwaitingReply,
    PoisonedAfterTimeout,
};

enum class TransactionResult : uint8_t {
    Ok,
    InvalidState,
    InvalidHeader,
    NullOutput,
};

enum class IncomingKind : uint8_t {
    None,
    Reply,
    Notification,
    Unsolicited,
};

struct DoorbellSnapshot {
    uint32_t hipcida;
    uint32_t hipctdr;
    uint32_t hipctdd;
};

struct Observation {
    bool hostRequestConsumed;
    bool hostRequestDoneNeedsAcknowledgement;
    bool incomingNeedsAcknowledgement;
    bool completesOutstandingRequest;
    IncomingKind incoming;
    HeaderFields header;
};

// Fixed-state tracker for a transport that serializes callers with one lock.
// Timeout leaves it poisoned until resetAfterFirmwareReset() is explicitly
// called by the firmware lifecycle code.
class TransactionTracker {
public:
    TransactionState state() const noexcept { return state_; }
    TransactionResult begin(uint32_t hostPrimary) noexcept;
    TransactionResult observe(const DoorbellSnapshot &snapshot,
                              Observation *out) noexcept;
    TransactionResult completeReply() noexcept;
    TransactionResult markTimeout() noexcept;
    void resetAfterFirmwareReset() noexcept;

private:
    TransactionState state_{TransactionState::Idle};
    bool replyObserved_{};
};

} // namespace sof::ipc4::kernel
