#include "Ipc4KernelTransport.hpp"
#include "Cavs25BootLifecycle.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kDoorbellBusy = 1U << 31;
constexpr uint32_t kTimeoutMaximumMs = 500;
constexpr uint32_t kPollMinimumUs = 50;
constexpr uint32_t kPollMaximumUs = 10000;
constexpr uint32_t kLargeConfigGetType = 3;
constexpr uint32_t kLargeConfigSizeMask = (1U << 20) - 1U;
constexpr uint32_t kReplyStatusMask = 0x00ffffffU;

bool validIo(const IoOps *ops) noexcept {
    return ops && ops->readRegister && ops->writeRegister &&
           ops->writeMailbox && ops->readMailbox && ops->delayMicroseconds;
}

} // namespace

Ipc4Transport::Ipc4Transport() noexcept
    : ops_(nullptr), downlinkMailboxCapacity_(0),
      uplinkMailboxCapacity_(0), firmwareReady_(false),
      firmwareReadyNotificationAcknowledged_(false) {}

Ipc4Transport::Ipc4Transport(const IoOps *ops, size_t downlinkMailboxCapacity,
                             size_t uplinkMailboxCapacity) noexcept
    : ops_(ops), downlinkMailboxCapacity_(downlinkMailboxCapacity),
      uplinkMailboxCapacity_(uplinkMailboxCapacity), firmwareReady_(false),
      firmwareReadyNotificationAcknowledged_(false) {}

bool Ipc4Transport::configure(const IoOps *ops,
                              size_t downlinkMailboxCapacity,
                              size_t uplinkMailboxCapacity) noexcept {
    if (!validIo(ops) || downlinkMailboxCapacity == 0 ||
        uplinkMailboxCapacity == 0 ||
        downlinkMailboxCapacity > kMaximumPayloadBytes ||
        uplinkMailboxCapacity > kMaximumPayloadBytes ||
        tracker_.state() != TransactionState::Idle)
        return false;
    ops_ = ops;
    downlinkMailboxCapacity_ = downlinkMailboxCapacity;
    uplinkMailboxCapacity_ = uplinkMailboxCapacity;
    firmwareReady_ = false;
    firmwareReadyNotificationAcknowledged_ = false;
    return true;
}

bool Ipc4Transport::clearConfiguration() noexcept {
    if (tracker_.state() != TransactionState::Idle) return false;
    ops_ = nullptr;
    downlinkMailboxCapacity_ = 0;
    uplinkMailboxCapacity_ = 0;
    firmwareReady_ = false;
    firmwareReadyNotificationAcknowledged_ = false;
    return true;
}

bool Ipc4Transport::confirmFirmwareReady(
    const Cavs25BootLifecycle *lifecycle) noexcept {
    if (!lifecycle || !lifecycle->canIssueIpc4() || !validIo(ops_) ||
        tracker_.state() != TransactionState::Idle)
        return false;
    firmwareReady_ = true;
    return true;
}

bool Ipc4Transport::readDoorbells(DoorbellSnapshot *snapshot) noexcept {
    if (!snapshot) return false;
    return ops_->readRegister(ops_->context, kHipcidaOffset, &snapshot->hipcida) &&
           ops_->readRegister(ops_->context, kHipctdrOffset, &snapshot->hipctdr) &&
           ops_->readRegister(ops_->context, kHipctddOffset, &snapshot->hipctdd);
}

TransportResult Ipc4Transport::servicePendingMessages(
    uint8_t maximumMessages) noexcept {
    if (!validIo(ops_)) return TransportResult::MissingIoOperation;
    if (maximumMessages == 0 || maximumMessages > 16 ||
        tracker_.state() != TransactionState::Idle)
        return TransportResult::InvalidArgument;

    for (uint8_t i = 0; i < maximumMessages; ++i) {
        uint32_t hipcidr = 0;
        if (!ops_->readRegister(ops_->context, kHipcidrOffset, &hipcidr) ||
            !readDoorbells(&snapshotScratch_))
            return TransportResult::IoFailure;
        if ((hipcidr & kDoorbellBusy) != 0) return TransportResult::DeviceBusy;
        if ((snapshotScratch_.hipcida & kDoorbellBusy) != 0 && !acknowledgeHostDone())
            return TransportResult::IoFailure;
        if ((snapshotScratch_.hipctdr & kDoorbellBusy) == 0) return TransportResult::Ok;

        if (tracker_.observe(snapshotScratch_, &observationScratch_) != TransactionResult::Ok)
            return TransportResult::TransactionStateError;
        // Preserve unpaired replies: their payload cannot safely be discarded.
        if (observationScratch_.incoming == IncomingKind::Unsolicited &&
            observationScratch_.header.reply)
            return TransportResult::DeviceBusy;
        if (observationScratch_.incoming == IncomingKind::Notification ||
            observationScratch_.incoming == IncomingKind::Unsolicited) {
            if (!ops_->incomingMessage)
                return TransportResult::NotificationHandlerMissing;
            if (!ops_->incomingMessage(ops_->context, &observationScratch_.header))
                return TransportResult::NotificationHandlerFailed;
        }
        if (!acknowledgeIncoming()) return TransportResult::IoFailure;
        if (observationScratch_.incoming == IncomingKind::Notification &&
            observationScratch_.header.notification &&
            observationScratch_.header.fwReady) {
            firmwareReady_ = true;
            firmwareReadyNotificationAcknowledged_ = true;
        }
    }

    if (!readDoorbells(&snapshotScratch_)) return TransportResult::IoFailure;
    return (snapshotScratch_.hipctdr & kDoorbellBusy) != 0
        ? TransportResult::DeviceBusy : TransportResult::Ok;
}

bool Ipc4Transport::acknowledgeHostDone() noexcept {
    planHostDoneAcknowledgement(&writePlanScratch_);
    return ops_->writeRegister(ops_->context, writePlanScratch_.writes[0].offset,
                               writePlanScratch_.writes[0].value);
}

bool Ipc4Transport::acknowledgeIncoming() noexcept {
    planIncomingAcknowledgement(&writePlanScratch_);
    return ops_->writeRegister(ops_->context, writePlanScratch_.writes[0].offset,
                               writePlanScratch_.writes[0].value) &&
           ops_->writeRegister(ops_->context, writePlanScratch_.writes[1].offset,
                               writePlanScratch_.writes[1].value);
}

void Ipc4Transport::poisonAfterFailure() noexcept {
    if (tracker_.state() == TransactionState::AwaitingReply)
        tracker_.markTimeout();
}

TransportResult Ipc4Transport::send(const Message *message,
                                    size_t replyCapacityBytes,
                                    uint8_t *replyPayload, Reply *reply,
                                    uint32_t timeoutMilliseconds,
                                    uint32_t pollIntervalMicroseconds) noexcept {
    if (!message || !reply ||
        (replyCapacityBytes != 0 && !replyPayload) ||
        replyCapacityBytes > uplinkMailboxCapacity_ ||
        replyCapacityBytes > kMaximumPayloadBytes ||
        timeoutMilliseconds == 0 || timeoutMilliseconds > kTimeoutMaximumMs ||
        pollIntervalMicroseconds < kPollMinimumUs ||
        pollIntervalMicroseconds > kPollMaximumUs)
        return TransportResult::InvalidArgument;
    reply->primary = 0;
    reply->extension = 0;
    reply->firmwareStatus = 0;
    reply->payloadBytes = 0;
    if (!validIo(ops_)) return TransportResult::MissingIoOperation;

    lastRequestPrimary_ = message->primary;
    lastRequestExtension_ = message->extension;
    lastRequestPayloadBytes_ = static_cast<uint32_t>(message->payloadBytes);
    for (uint32_t i = 0; i < 3; ++i) {
        const size_t offset = static_cast<size_t>(i) * sizeof(uint32_t);
        lastRequestPayloadWords_[i] = offset + sizeof(uint32_t) <= message->payloadBytes
            ? static_cast<uint32_t>(message->payload[offset]) |
              (static_cast<uint32_t>(message->payload[offset + 1]) << 8) |
              (static_cast<uint32_t>(message->payload[offset + 2]) << 16) |
              (static_cast<uint32_t>(message->payload[offset + 3]) << 24)
            : 0;
    }

    const uint32_t timeoutMicroseconds = timeoutMilliseconds * 1000U;
    const uint32_t pollCount = (timeoutMicroseconds + pollIntervalMicroseconds - 1U) /
                               pollIntervalMicroseconds;
    lastPreflightBusyPolls_ = 0;
    lastPreflightHipcidr_ = 0;
    lastPreflightHipcida_ = 0;
    lastPreflightHipctdr_ = 0;
    bool preflightReady = false;
    for (uint32_t poll = 0; poll < pollCount; ++poll) {
        if (tracker_.state() != TransactionState::Idle)
            return TransportResult::DeviceBusy;

        const TransportResult pendingResult = servicePendingMessages(8);
        if (pendingResult != TransportResult::Ok &&
            pendingResult != TransportResult::DeviceBusy)
            return pendingResult;

        if (pendingResult == TransportResult::Ok) {
            if (!firmwareReady_) return TransportResult::FirmwareNotReady;

            uint32_t hipcidr = 0;
            if (!ops_->readRegister(ops_->context, kHipcidrOffset, &hipcidr) ||
                !readDoorbells(&snapshotScratch_))
                return TransportResult::IoFailure;
            lastPreflightHipcidr_ = hipcidr;
            lastPreflightHipcida_ = snapshotScratch_.hipcida;
            lastPreflightHipctdr_ = snapshotScratch_.hipctdr;
            preflightReady = (hipcidr & kDoorbellBusy) == 0 &&
                (snapshotScratch_.hipcida & kDoorbellBusy) == 0 &&
                (snapshotScratch_.hipctdr & kDoorbellBusy) == 0;
        }

        if (preflightReady) {
            lastPreflightBusyPolls_ = poll;
            break;
        }
        lastPreflightBusyPolls_ = poll + 1U;
        if (poll + 1U < pollCount)
            ops_->delayMicroseconds(ops_->context, pollIntervalMicroseconds);
    }
    if (!preflightReady) return TransportResult::TimedOut;

    if (planHostDoorbell(message, downlinkMailboxCapacity_, &writePlanScratch_) != Result::Ok)
        return TransportResult::InvalidArgument;
    if (message->payloadBytes != 0 &&
        !ops_->writeMailbox(ops_->context, kCavs25DownlinkMailboxOffset,
                            message->payload, message->payloadBytes)) {
        return TransportResult::IoFailure;
    }
    if (!ops_->writeRegister(ops_->context, writePlanScratch_.writes[0].offset,
                             writePlanScratch_.writes[0].value))
        return TransportResult::IoFailure;
    if (tracker_.begin(message->primary) != TransactionResult::Ok)
        return TransportResult::TransactionStateError;
    if (!ops_->writeRegister(ops_->context, writePlanScratch_.writes[1].offset,
                             writePlanScratch_.writes[1].value)) {
        poisonAfterFailure();
        return TransportResult::IoFailure;
    }

    for (uint32_t poll = 0; poll < pollCount; ++poll) {
        if (!readDoorbells(&snapshotScratch_)) {
            poisonAfterFailure();
            return TransportResult::IoFailure;
        }
        if (tracker_.observe(snapshotScratch_, &observationScratch_) != TransactionResult::Ok) {
            poisonAfterFailure();
            return TransportResult::TransactionStateError;
        }
        if (observationScratch_.hostRequestDoneNeedsAcknowledgement &&
            !acknowledgeHostDone()) {
            poisonAfterFailure();
            return TransportResult::IoFailure;
        }
        if (observationScratch_.incomingNeedsAcknowledgement) {
            if (observationScratch_.incoming == IncomingKind::Unsolicited &&
                observationScratch_.header.reply) {
                poisonAfterFailure();
                return TransportResult::TransactionStateError;
            }
            if (observationScratch_.incoming == IncomingKind::Notification ||
                observationScratch_.incoming == IncomingKind::Unsolicited) {
                if (!ops_->incomingMessage) {
                    poisonAfterFailure();
                    return TransportResult::NotificationHandlerMissing;
                }
                if (!ops_->incomingMessage(ops_->context, &observationScratch_.header)) {
                    poisonAfterFailure();
                    return TransportResult::NotificationHandlerFailed;
                }
            }

            if (observationScratch_.incoming == IncomingKind::Reply) {
                const bool hasLargeConfigPayload =
                    (observationScratch_.header.primary & kReplyStatusMask) == 0 &&
                    observationScratch_.header.moduleTarget &&
                    observationScratch_.header.type == kLargeConfigGetType;
                const size_t actualReplyBytes = hasLargeConfigPayload
                    ? static_cast<size_t>(observationScratch_.header.extension &
                                          kLargeConfigSizeMask)
                    : 0;
                if (actualReplyBytes > replyCapacityBytes ||
                    actualReplyBytes > uplinkMailboxCapacity_ ||
                    actualReplyBytes > kMaximumPayloadBytes) {
                    poisonAfterFailure();
                    return TransportResult::ReplyPayloadTooLarge;
                }
                if ((actualReplyBytes & 3U) != 0) {
                    poisonAfterFailure();
                    return TransportResult::InvalidReply;
                }
                if (actualReplyBytes != 0 &&
                    !ops_->readMailbox(ops_->context, kCavs25UplinkMailboxOffset,
                                       replyPayload, actualReplyBytes)) {
                    poisonAfterFailure();
                    return TransportResult::IoFailure;
                }
                reply->payloadBytes = actualReplyBytes;
            }
            if (!acknowledgeIncoming()) {
                poisonAfterFailure();
                return TransportResult::IoFailure;
            }
            if (observationScratch_.incoming == IncomingKind::Notification &&
                observationScratch_.header.notification &&
                observationScratch_.header.fwReady) {
                firmwareReady_ = true;
                firmwareReadyNotificationAcknowledged_ = true;
            }
            if (observationScratch_.incoming == IncomingKind::Reply) {
                reply->primary = observationScratch_.header.primary;
                reply->extension = observationScratch_.header.extension;
                lastReplyPrimary_ = reply->primary;
                lastReplyExtension_ = reply->extension;
                reply->firmwareStatus = observationScratch_.header.primary &
                                        kReplyStatusMask;
                if (tracker_.completeReply() != TransactionResult::Ok)
                    return TransportResult::TransactionStateError;
                if (reply->firmwareStatus != 0)
                    return TransportResult::FirmwareRejected;
                return TransportResult::Ok;
            }
        }
        ops_->delayMicroseconds(ops_->context, pollIntervalMicroseconds);
    }

    poisonAfterFailure();
    return TransportResult::TimedOut;
}

FirmwareConfigExchangeResult Ipc4Transport::queryFirmwareConfig(
    Message *requestStorage, uint8_t *replyStorage,
    size_t replyCapacityBytes, uint16_t *maxPipelineIdInclusive,
    uint32_t timeoutMilliseconds, uint32_t pollIntervalMicroseconds,
    TransportResult *transportStatus,
    FirmwareConfigResult *replyStatus) noexcept {
    if (!requestStorage || !replyStorage || !maxPipelineIdInclusive ||
        !transportStatus || !replyStatus || replyCapacityBytes == 0 ||
        replyCapacityBytes > kMaximumPayloadBytes ||
        (replyCapacityBytes & 3U) != 0)
        return FirmwareConfigExchangeResult::InvalidArgument;

    *maxPipelineIdInclusive = 0;
    *transportStatus = TransportResult::InvalidArgument;
    *replyStatus = makeFirmwareConfigRequest(replyCapacityBytes, requestStorage);
    if (*replyStatus != FirmwareConfigResult::Ok)
        return FirmwareConfigExchangeResult::InvalidArgument;

    *transportStatus = send(requestStorage, replyCapacityBytes, replyStorage,
                            &replyScratch_, timeoutMilliseconds,
                            pollIntervalMicroseconds);
    if (*transportStatus != TransportResult::Ok)
        return FirmwareConfigExchangeResult::TransportFailure;

    *replyStatus = parseFirmwareConfigReply(replyScratch_.primary, replyStorage,
                                            replyScratch_.payloadBytes,
                                            maxPipelineIdInclusive);
    return *replyStatus == FirmwareConfigResult::Ok
        ? FirmwareConfigExchangeResult::Ok
        : FirmwareConfigExchangeResult::InvalidReply;
}

void Ipc4Transport::resetAfterFirmwareReset() noexcept {
    tracker_.resetAfterFirmwareReset();
    firmwareReady_ = false;
    firmwareReadyNotificationAcknowledged_ = false;
}

} // namespace sof::ipc4::kernel
