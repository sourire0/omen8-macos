#include "Cavs25TransportPlan.hpp"

namespace sof::ipc4 {
namespace {

constexpr uint32_t kBusyDone = 1U << 31;
constexpr uint32_t kMessageMask = 0x7fffffffU;

} // namespace

HostRequestPlan planCavs25HostRequest(const Message &message,
                                     size_t mailboxCapacity) {
    validateMailboxRequest(message, mailboxCapacity);
    HostRequestPlan plan;
    plan.downlinkPayload = message.payload;
    // The caller must stage downlinkPayload before applying these writes.
    // Extension precedes the primary doorbell, which asserts BUSY last.
    plan.doorbellWrites.push_back({kCavs25Hipcidd, message.extension & kMessageMask});
    plan.doorbellWrites.push_back({kCavs25Hipcidr,
                                   (message.primary & kMessageMask) | kBusyDone});
    return plan;
}

std::vector<RegisterWrite> planCavs25HostDoneAcknowledgement() {
    // HIPCIDA DONE is write-one-to-clear on cAVS 2.5.
    return {{kCavs25Hipcida, kBusyDone}};
}

std::vector<RegisterWrite> planCavs25IncomingAcknowledgement() {
    // cAVS 2.5: acknowledge the incoming doorbell, then report host completion.
    return {{kCavs25Hipctdr, kBusyDone}, {kCavs25Hipctda, kBusyDone}};
}

} // namespace sof::ipc4
