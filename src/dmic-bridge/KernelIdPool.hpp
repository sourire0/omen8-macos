#pragma once

#include <stdint.h>

namespace sof::ipc4::kernel {

enum class IdPoolResult : uint8_t {
    Ok,
    InvalidMaximum,
    InvalidState,
    Exhausted,
    InvalidId,
    NotAllocated,
    TeardownUnconfirmed,
};

// Fixed bitmap allocator matching SOF's lowest-free, inclusive-ceiling rule.
// An uncertain teardown must retain the ID until a confirmed reset.
class IdPool {
public:
    IdPoolResult initialize(uint16_t maxInclusive) noexcept;
    IdPoolResult allocate(uint8_t *outId) noexcept;
    IdPoolResult release(uint8_t id, bool teardownConfirmed) noexcept;
    void resetAfterFirmwareReset() noexcept;
    bool isAllocated(uint8_t id) const noexcept;
    uint16_t maxInclusive() const noexcept { return maxInclusive_; }

private:
    uint8_t allocated_[32]{};
    uint16_t maxInclusive_{};
    bool initialized_{};
};

} // namespace sof::ipc4::kernel
