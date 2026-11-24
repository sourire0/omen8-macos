#pragma once

#include "Ipc4Header.hpp"
#include "Ipc4Messages.hpp"

#include <cstdint>

namespace sof::ipc4 {

enum class TransactionState {
    Idle,
    AwaitingReply,
    PoisonedAfterTimeout,
};

enum class IncomingKind {
    None,
    Reply,
    Notification,
    Unsolicited,
};

struct Cavs25DoorbellSnapshot {
    uint32_t hipcida{}; // Host request acknowledgement from DSP.
    uint32_t hipctdr{}; // DSP-to-host primary doorbell.
    uint32_t hipctdd{}; // DSP-to-host extension doorbell.
};

struct Ipc4Observation {
    bool hostRequestConsumed{};
    // cAVS host must write DONE back to HIPCIDA after consuming this event.
    bool hostRequestDoneNeedsAcknowledgement{};
    bool incomingNeedsAcknowledgement{};
    IncomingKind incoming{IncomingKind::None};
    HeaderFields header{};
    bool completesOutstandingRequest{};
};

// Pure state tracker for one in-flight IPC4 request. It performs no MMIO,
// acknowledgement, mailbox copy, interrupt management, or timeout waiting.
// The caller must serialize access under its transport lock.
class Ipc4TransactionTracker {
public:
    TransactionState state() const noexcept { return state_; }

    void begin(const Message &request);
    Ipc4Observation observe(const Cavs25DoorbellSnapshot &snapshot);
    void completeReply();
    void markTimeout();
    // Only call after an externally verified DSP firmware reset/reboot.
    void resetAfterFirmwareReset() noexcept;

private:
    TransactionState state_{TransactionState::Idle};
    bool replyObserved_{};
};

} // namespace sof::ipc4
