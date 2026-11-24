#pragma once

#include "Ipc4Messages.hpp"

#include <cstdint>
#include <vector>

namespace sof::ipc4 {

// cAVS 2.5 DSP BAR register offsets. These are descriptions for an offline
// operation plan; this module does not map BARs or perform register writes.
constexpr uint16_t kCavs25Hipctdr = 0x00c0;
constexpr uint16_t kCavs25Hipctda = 0x00c4;
constexpr uint16_t kCavs25Hipctdd = 0x00c8;
constexpr uint16_t kCavs25Hipcidr = 0x00d0;
constexpr uint16_t kCavs25Hipcida = 0x00d4;
constexpr uint16_t kCavs25Hipcidd = 0x00d8;

struct RegisterWrite {
    uint16_t offset{};
    uint32_t value{};
};

struct HostRequestPlan {
    std::vector<uint8_t> downlinkPayload;
    std::vector<RegisterWrite> doorbellWrites;
};

HostRequestPlan planCavs25HostRequest(const Message &message,
                                     size_t mailboxCapacity);
std::vector<RegisterWrite> planCavs25HostDoneAcknowledgement();
std::vector<RegisterWrite> planCavs25IncomingAcknowledgement();

} // namespace sof::ipc4
