#include "Ipc4Header.hpp"
#include "Ipc4Messages.hpp"
#include "Ipc4Transaction.hpp"
#include "Cavs25TransportPlan.hpp"

#include <iomanip>
#include <iostream>
#include <vector>

static void print(const char *name, const sof::ipc4::Message &message) {
    std::cout << name << " primary=0x" << std::hex << std::setw(8) << std::setfill('0')
              << message.primary << " extension=0x" << std::setw(8) << message.extension
              << std::dec << " payload=" << message.payload.size() << " bytes\n";
}

int main() {
    using namespace sof::ipc4;
    const auto create = createPipeline(2, 0, 16, 0, false);
    const auto init = initModule(4, 0, 2, 0, std::vector<uint8_t>(64, 0));
    const auto eqData = controlPayload(0, std::vector<uint8_t>(88, 0));
    const auto eqConfig = largeConfigSet(10, 0, 0, eqData);
    const auto bind = bindModules(4, 0, 0, 6, 0, 0);
    const auto pause = setPipelineState(2, 3);
    HeaderFields ready{};
    decodeHeader(0x1b080000U, 0, &ready);
    const auto remove = deletePipeline(2);
    const auto unbind = unbindModules(4, 0, 0, 6, 0, 0);
    Ipc4TransactionTracker tracker;
    const auto request = largeConfigSet(10, 0, 0, eqData);
    const auto transportPlan = planCavs25HostRequest(request, 4096);
    const auto hostAck = planCavs25HostDoneAcknowledgement();
    const auto incomingAck = planCavs25IncomingAcknowledgement();
    tracker.begin(request);
    const auto accepted = tracker.observe({1U << 31, 0, 0});
    const auto notification = tracker.observe({0, (1U << 31) | 0x1b080000U, 0});
    const auto reply = tracker.observe({0, (1U << 31) | request.primary | (1U << 29), 0});
    if (transportPlan.doorbellWrites.size() != 2 ||
        transportPlan.doorbellWrites[0].offset != kCavs25Hipcidd ||
        transportPlan.doorbellWrites[0].value != request.extension ||
        transportPlan.doorbellWrites[1].offset != kCavs25Hipcidr ||
        transportPlan.doorbellWrites[1].value != (request.primary | (1U << 31)) ||
        hostAck.size() != 1 || hostAck[0].offset != kCavs25Hipcida ||
        incomingAck.size() != 2 || incomingAck[0].offset != kCavs25Hipctdr ||
        incomingAck[1].offset != kCavs25Hipctda ||
        !accepted.hostRequestConsumed || !accepted.hostRequestDoneNeedsAcknowledgement ||
        accepted.incoming != IncomingKind::None ||
        notification.incoming != IncomingKind::Notification ||
        notification.completesOutstandingRequest ||
        reply.incoming != IncomingKind::Reply || !reply.completesOutstandingRequest)
        return 1;
    tracker.completeReply();
    Ipc4TransactionTracker timedOut;
    timedOut.begin(request);
    timedOut.markTimeout();
    bool refusedAfterTimeout = false;
    try {
        timedOut.begin(request);
    } catch (const std::logic_error &) {
        refusedAfterTimeout = true;
    }
    if (!refusedAfterTimeout || timedOut.state() != TransactionState::PoisonedAfterTimeout)
        return 1;
    timedOut.resetAfterFirmwareReset();
    if (create.primary != 0x11020010U || create.extension != 0x20000000U ||
        create.payload.size() != 28 || create.payload[0] != 7 ||
        create.payload[3] != 1 || create.payload[12] != 3 ||
        init.primary != 0x40000004U ||
        init.extension != 0x00020010U || bind.primary != 0x45000004U ||
        eqConfig.primary != 0x4400000aU || eqConfig.extension != 0x3000006cU ||
        eqConfig.payload.size() != 108 || eqConfig.payload[0] != 0 ||
        eqConfig.payload[2] != 88 || eqConfig.payload[20] != 0 ||
        bind.extension != 0x00000006U || unbind.primary != 0x46000004U ||
        unbind.extension != 0x00000006U || remove.primary != 0x12020000U ||
        pause.primary != 0x13020003U || !ready.fwReady)
        return 1;
    print("create-pipeline", create);
    print("init-copier", init);
    print("large-config-eqiir", eqConfig);
    print("bind-copier-to-gain", bind);
    print("unbind-copier-from-gain", unbind);
    print("pause-pipeline", pause);
    std::cout << "IPC4 header and transaction-state vectors verified; no hardware access\n";
    return 0;
}
