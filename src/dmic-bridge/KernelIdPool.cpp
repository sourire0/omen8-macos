#include "KernelIdPool.hpp"

namespace sof::ipc4::kernel {

IdPoolResult IdPool::initialize(uint16_t maxInclusive) noexcept {
    if (initialized_) return IdPoolResult::InvalidState;
    if (maxInclusive > 255) return IdPoolResult::InvalidMaximum;
    for (uint8_t &byte : allocated_) byte = 0;
    maxInclusive_ = maxInclusive;
    initialized_ = true;
    return IdPoolResult::Ok;
}

IdPoolResult IdPool::allocate(uint8_t *outId) noexcept {
    if (!outId) return IdPoolResult::InvalidId;
    if (!initialized_) return IdPoolResult::InvalidMaximum;
    for (uint16_t candidate = 0; candidate <= maxInclusive_; ++candidate) {
        const uint8_t id = static_cast<uint8_t>(candidate);
        const uint8_t mask = static_cast<uint8_t>(1U << (id & 7U));
        uint8_t &slot = allocated_[id >> 3U];
        if ((slot & mask) == 0) {
            slot = static_cast<uint8_t>(slot | mask);
            *outId = id;
            return IdPoolResult::Ok;
        }
    }
    return IdPoolResult::Exhausted;
}

IdPoolResult IdPool::release(uint8_t id, bool teardownConfirmed) noexcept {
    if (!initialized_ || id > maxInclusive_) return IdPoolResult::InvalidId;
    if (!teardownConfirmed) return IdPoolResult::TeardownUnconfirmed;
    if (!isAllocated(id)) return IdPoolResult::NotAllocated;
    const uint8_t mask = static_cast<uint8_t>(~(1U << (id & 7U)));
    allocated_[id >> 3U] = static_cast<uint8_t>(allocated_[id >> 3U] & mask);
    return IdPoolResult::Ok;
}

void IdPool::resetAfterFirmwareReset() noexcept {
    for (uint8_t &byte : allocated_) byte = 0;
    initialized_ = false;
    maxInclusive_ = 0;
}

bool IdPool::isAllocated(uint8_t id) const noexcept {
    if (!initialized_ || id > maxInclusive_) return false;
    const uint8_t mask = static_cast<uint8_t>(1U << (id & 7U));
    return (allocated_[id >> 3U] & mask) != 0;
}

} // namespace sof::ipc4::kernel
