#pragma once

#include "DmicMessagePlan.hpp"

#include <cstdint>
#include <map>
#include <set>

namespace sof::ipc4::dmic {

// Host-side IPC4 IDA equivalent. Keep one instance per DSP and serialize calls
// under the driver's pipeline-state lock. maxPipelineIdInclusive must come
// from the firmware FW_CONFIG response; it must not be guessed from topology.
class RuntimeIdAllocator {
public:
    explicit RuntimeIdAllocator(uint16_t maxPipelineIdInclusive);

    RuntimeIds allocate(const FirmwareManifest &firmware);
    // Call only after DSP teardown is confirmed, or after a full firmware
    // reset. If teardown state is uncertain, retain the IDs until reset.
    void release(const FirmwareManifest &firmware, const RuntimeIds &ids);

private:
    uint16_t maxPipelineIdInclusive_;
    std::set<uint8_t> pipelines_;
    std::map<uint32_t, std::set<uint8_t>> moduleInstances_;
};

} // namespace sof::ipc4::dmic
