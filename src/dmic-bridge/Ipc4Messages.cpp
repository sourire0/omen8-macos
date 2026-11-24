#include "Ipc4Messages.hpp"

#include <cstddef>
#include <algorithm>
#include <stdexcept>

namespace sof::ipc4 {
namespace {

constexpr uint32_t kTargetModule = 1U << 30;
constexpr uint32_t kMessageTypeShift = 24;
constexpr uint32_t kModuleInstanceShift = 16;
constexpr uint32_t kPipelinePriorityShift = 11;
constexpr uint32_t kPipelineCoreShift = 20;
constexpr uint32_t kModuleCoreShift = 24;
constexpr uint32_t kLargeConfigSizeMask = (1U << 20) - 1U;
constexpr uint32_t kLargeConfigParamShift = 20;
constexpr uint32_t kLargeConfigLastBlock = 1U << 28;
constexpr uint32_t kLargeConfigFirstBlock = 1U << 29;
constexpr uint32_t kLargeConfigGet = 3;

constexpr uint32_t globalType(uint32_t value) { return value << kMessageTypeShift; }
constexpr uint32_t moduleType(uint32_t value) { return kTargetModule | globalType(value); }

void require(bool condition, const char *message) {
    if (!condition) throw std::invalid_argument(message);
}

} // namespace

void validateMailboxRequest(const Message &message, size_t mailboxCapacity) {
    require((message.primary & 0x80000000U) == 0,
            "software IPC4 primary header must not set hardware BUSY");
    require(message.payload.size() <= mailboxCapacity,
            "IPC4 request payload exceeds downlink mailbox capacity");
    require((message.payload.size() & 3U) == 0,
            "IPC4 mailbox payload must be DWORD aligned");
}

Message createPipeline(uint8_t pipelineInstance, uint8_t priority,
                       uint16_t memoryPages, uint8_t core, bool lowPower,
                       uint32_t domainId, uint32_t stackBytes,
                       uint32_t heapBytes) {
    require(priority <= 31, "pipeline priority exceeds IPC4 field");
    require(memoryPages <= 0x7ff, "pipeline memory pages exceed IPC4 field");
    require(core <= 15, "core ID exceeds IPC4 field");
    Message msg;
    msg.primary = globalType(17) |
                  (static_cast<uint32_t>(pipelineInstance) << kModuleInstanceShift) |
                  (static_cast<uint32_t>(priority) << kPipelinePriorityShift) |
                  memoryPages;
    msg.extension = (static_cast<uint32_t>(core) << kPipelineCoreShift) |
                    (lowPower ? 1U : 0U) | (1U << 29);
    // SOF IPC4 requires the pipeline memory object-array payload for this path.
    // word0: 7 DWORDs total and array flag; object: MEM_DATA, 3 DWORDs, LAST;
    // memory data: domain_id, stack_bytes, heap_bytes (zero defaults here).
    const uint32_t words[] = {0x01000007U, 0U, 0U, 0x00030003U,
                              domainId, stackBytes, heapBytes};
    msg.payload.resize(sizeof(words));
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        const auto value = words[i];
        for (size_t byte = 0; byte < 4; ++byte)
            msg.payload[i * 4 + byte] = static_cast<uint8_t>(value >> (byte * 8));
    }
    return msg;
}

Message deletePipeline(uint8_t pipelineInstance) {
    Message msg;
    msg.primary = globalType(18) |
                  (static_cast<uint32_t>(pipelineInstance) << kModuleInstanceShift);
    return msg;
}

Message setPipelineState(uint8_t pipelineInstance, uint16_t state) {
    Message msg;
    msg.primary = globalType(19) |
                  (static_cast<uint32_t>(pipelineInstance) << kModuleInstanceShift) |
                  state;
    return msg;
}

Message initModule(uint16_t moduleId, uint8_t moduleInstance,
                   uint8_t pipelineInstance, uint8_t core,
                   const std::vector<uint8_t> &payload) {
    require(core <= 15, "core ID exceeds IPC4 field");
    require(payload.size() <= 4096, "module-init payload exceeds IPC4 message limit");
    const size_t dwords = (payload.size() + 3U) / 4U;
    require(dwords <= 0xffff, "module-init payload size exceeds IPC4 field");
    Message msg;
    msg.primary = moduleType(0) |
                  (static_cast<uint32_t>(moduleInstance) << kModuleInstanceShift) |
                  moduleId;
    msg.extension = static_cast<uint32_t>(dwords) |
                   (static_cast<uint32_t>(pipelineInstance) << 16) |
                   (static_cast<uint32_t>(core) << kModuleCoreShift);
    msg.payload = payload;
    msg.payload.resize(dwords * 4U, 0);
    return msg;
}

Message largeConfigSet(uint16_t moduleId, uint8_t moduleInstance,
                       uint8_t parameterId, const std::vector<uint8_t> &payload) {
    require(!payload.empty(), "large-config payload must not be empty");
    require(payload.size() <= kLargeConfigSizeMask,
            "large-config payload exceeds IPC4 size field");
    Message msg;
    msg.primary = moduleType(4) |
                  (static_cast<uint32_t>(moduleInstance) << kModuleInstanceShift) |
                  moduleId;
    msg.extension = (static_cast<uint32_t>(parameterId) << kLargeConfigParamShift) |
                    static_cast<uint32_t>(payload.size()) |
                    kLargeConfigFirstBlock | kLargeConfigLastBlock;
    msg.payload = payload;
    return msg;
}

Message largeConfigGet(uint16_t moduleId, uint8_t moduleInstance,
                       uint8_t parameterId, uint32_t requestedBytes) {
    require(requestedBytes <= kLargeConfigSizeMask,
            "large-config get size exceeds IPC4 size field");
    Message msg;
    msg.primary = moduleType(kLargeConfigGet) |
                  (static_cast<uint32_t>(moduleInstance) << kModuleInstanceShift) |
                  moduleId;
    msg.extension = (static_cast<uint32_t>(parameterId) << kLargeConfigParamShift) |
                    requestedBytes | kLargeConfigFirstBlock;
    return msg;
}

std::vector<uint8_t> controlPayload(uint16_t controlId,
                                    const std::vector<uint8_t> &data) {
    require(data.size() <= 0xffff, "control data exceeds IPC4 num_elems field");
    std::vector<uint8_t> payload(20U + data.size(), 0);
    payload[0] = static_cast<uint8_t>(controlId & 0xffU);
    payload[1] = static_cast<uint8_t>(controlId >> 8);
    payload[2] = static_cast<uint8_t>(data.size() & 0xffU);
    payload[3] = static_cast<uint8_t>((data.size() >> 8) & 0xffU);
    std::copy(data.begin(), data.end(), payload.begin() + 20);
    return payload;
}

static Message bindLike(uint32_t messageType, uint16_t sourceModuleId,
                        uint8_t sourceInstance, uint8_t sourceQueue,
                        uint16_t destinationModuleId, uint8_t destinationInstance,
                        uint8_t destinationQueue) {
    require(sourceQueue <= 7 && destinationQueue <= 7,
            "module queue ID exceeds IPC4 field");
    Message msg;
    msg.primary = moduleType(messageType) |
                  (static_cast<uint32_t>(sourceInstance) << kModuleInstanceShift) |
                  sourceModuleId;
    msg.extension = destinationModuleId |
                    (static_cast<uint32_t>(destinationInstance) << 16) |
                    (static_cast<uint32_t>(destinationQueue) << 24) |
                    (static_cast<uint32_t>(sourceQueue) << 27);
    return msg;
}

Message bindModules(uint16_t sourceModuleId, uint8_t sourceInstance,
                    uint8_t sourceQueue, uint16_t destinationModuleId,
                    uint8_t destinationInstance, uint8_t destinationQueue) {
    return bindLike(5, sourceModuleId, sourceInstance, sourceQueue,
                    destinationModuleId, destinationInstance, destinationQueue);
}

Message unbindModules(uint16_t sourceModuleId, uint8_t sourceInstance,
                      uint8_t sourceQueue, uint16_t destinationModuleId,
                      uint8_t destinationInstance, uint8_t destinationQueue) {
    return bindLike(6, sourceModuleId, sourceInstance, sourceQueue,
                    destinationModuleId, destinationInstance, destinationQueue);
}

} // namespace sof::ipc4
