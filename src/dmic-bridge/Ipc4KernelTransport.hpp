#pragma once

#include "Cavs25KernelTransportPlan.hpp"
#include "FirmwareConfigKernel.hpp"
#include "Ipc4KernelTransaction.hpp"

#include <stddef.h>
#include <stdint.h>

namespace sof::ipc4::kernel {

class Cavs25BootLifecycle;

constexpr uint32_t kCavs25DownlinkMailboxOffset = 0x000a0000;
constexpr uint32_t kCavs25UplinkMailboxOffset = 0x00081000;

struct IoOps {
    void *context;
    bool (*readRegister)(void *context, uint16_t offset, uint32_t *value);
    bool (*writeRegister)(void *context, uint16_t offset, uint32_t value);
    bool (*writeMailbox)(void *context, uint32_t offset,
                         const uint8_t *bytes, size_t length);
    bool (*readMailbox)(void *context, uint32_t offset,
                        uint8_t *bytes, size_t length);
    void (*delayMicroseconds)(void *context, uint32_t delay);
    bool (*incomingMessage)(void *context, const HeaderFields *header);
};

struct Reply {
    uint32_t primary;
    uint32_t extension;
    uint32_t firmwareStatus;
    size_t payloadBytes;
};

enum class TransportResult : uint8_t {
    Ok,
    InvalidArgument,
    MissingIoOperation,
    DeviceBusy,
    FirmwareNotReady,
    IoFailure,
    ReplyPayloadTooLarge,
    FirmwareRejected,
    InvalidReply,
    NotificationHandlerMissing,
    NotificationHandlerFailed,
    TimedOut,
    TransactionStateError,
};

enum class FirmwareConfigExchangeResult : uint8_t {
    Ok,
    InvalidArgument,
    TransportFailure,
    InvalidReply,
};

// Bounded synchronous cAVS IPC4 request pump. Caller must hold the driver's
// transport lock for the whole call; callbacks must not issue nested IPC.
// Register/mailbox operations are supplied by the future IOKit layer and are
// never invoked by the offline build itself.
class Ipc4Transport {
public:
    Ipc4Transport() noexcept;
    explicit Ipc4Transport(const IoOps *ops, size_t downlinkMailboxCapacity,
                           size_t uplinkMailboxCapacity) noexcept;

    bool configure(const IoOps *ops, size_t downlinkMailboxCapacity,
                   size_t uplinkMailboxCapacity) noexcept;

    // replyCapacityBytes bounds storage; LARGE_CONFIG_GET replies report their
    // actual payload length in the low 20 bits of the reply extension header.
    TransportResult send(const Message *message, size_t replyCapacityBytes,
                         uint8_t *replyPayload, Reply *reply,
                         uint32_t timeoutMilliseconds,
                         uint32_t pollIntervalMicroseconds) noexcept;
    TransportResult servicePendingMessages(uint8_t maximumMessages) noexcept;
    FirmwareConfigExchangeResult queryFirmwareConfig(
        Message *requestStorage, uint8_t *replyStorage,
        size_t replyCapacityBytes, uint16_t *maxPipelineIdInclusive,
        uint32_t timeoutMilliseconds, uint32_t pollIntervalMicroseconds,
        TransportResult *transportStatus,
        FirmwareConfigResult *replyStatus) noexcept;
    bool clearConfiguration() noexcept;
    bool confirmFirmwareReady(
        const Cavs25BootLifecycle *lifecycle) noexcept;
    bool firmwareReadyNotificationAcknowledged() const noexcept {
        return firmwareReadyNotificationAcknowledged_;
    }
    uint32_t lastReplyPrimary() const noexcept { return lastReplyPrimary_; }
    uint32_t lastReplyExtension() const noexcept { return lastReplyExtension_; }
    uint32_t lastRequestPrimary() const noexcept { return lastRequestPrimary_; }
    uint32_t lastRequestExtension() const noexcept { return lastRequestExtension_; }
    uint32_t lastRequestPayloadBytes() const noexcept { return lastRequestPayloadBytes_; }
    uint32_t lastRequestPayloadWord(uint32_t index) const noexcept {
        return index < 3 ? lastRequestPayloadWords_[index] : 0;
    }
    uint32_t lastPreflightBusyPolls() const noexcept {
        return lastPreflightBusyPolls_;
    }
    uint32_t lastPreflightHipcidr() const noexcept {
        return lastPreflightHipcidr_;
    }
    uint32_t lastPreflightHipcida() const noexcept {
        return lastPreflightHipcida_;
    }
    uint32_t lastPreflightHipctdr() const noexcept {
        return lastPreflightHipctdr_;
    }
    TransactionState state() const noexcept { return tracker_.state(); }
    void resetAfterFirmwareReset() noexcept;

private:
    bool readDoorbells(DoorbellSnapshot *snapshot) noexcept;
    bool acknowledgeHostDone() noexcept;
    bool acknowledgeIncoming() noexcept;
    void poisonAfterFailure() noexcept;

    const IoOps *ops_;
    size_t downlinkMailboxCapacity_;
    size_t uplinkMailboxCapacity_;
    TransactionTracker tracker_;
    DoorbellSnapshot snapshotScratch_;
    Observation observationScratch_;
    DoorbellWritePlan writePlanScratch_;
    Reply replyScratch_;
    uint32_t lastRequestPrimary_ = 0;
    uint32_t lastRequestExtension_ = 0;
    uint32_t lastRequestPayloadBytes_ = 0;
    uint32_t lastRequestPayloadWords_[3] = {};
    uint32_t lastReplyPrimary_ = 0;
    uint32_t lastReplyExtension_ = 0;
    uint32_t lastPreflightBusyPolls_ = 0;
    uint32_t lastPreflightHipcidr_ = 0;
    uint32_t lastPreflightHipcida_ = 0;
    uint32_t lastPreflightHipctdr_ = 0;
    bool firmwareReady_;
    bool firmwareReadyNotificationAcknowledged_;
};

} // namespace sof::ipc4::kernel
