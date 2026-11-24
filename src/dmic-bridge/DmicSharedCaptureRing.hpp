#pragma once

#include "DmicAudioBridgeABI.h"

#include <IOKit/IOBufferMemoryDescriptor.h>

namespace sof::ipc4::dmic::kernel {

// Kernel-to-HAL publication ring. The HAL maps it read-only; audio is copied
// into this ring only after the capture runtime has validated and copied a
// completed HDA period.
class DmicSharedCaptureRing final {
public:
    DmicSharedCaptureRing() noexcept;
    ~DmicSharedCaptureRing();

    bool initialize() noexcept;
    bool beginCapture() noexcept;
    bool setStatus(DmicAudioBridgeStatus status) noexcept;
    bool publish(const void *stereoS32Frames, uint32_t frameCount) noexcept;
    IOBufferMemoryDescriptor *memoryDescriptor() const noexcept {
        return descriptor_;
    }
    bool initialized() const noexcept {
        return descriptor_ != nullptr && header_ != nullptr && samples_ != nullptr;
    }
    const DmicAudioBridgeSharedHeader *header() const noexcept {
        return header_;
    }
    const uint8_t *samples() const noexcept { return samples_; }
    uint64_t writeFrames() const noexcept {
        return header_ ? __atomic_load_n(&header_->writeFrames, __ATOMIC_ACQUIRE) : 0;
    }
    uint32_t status() const noexcept {
        return header_ ? __atomic_load_n(&header_->status, __ATOMIC_ACQUIRE)
                       : static_cast<uint32_t>(kDmicAudioBridgeFailed);
    }

private:
    IOBufferMemoryDescriptor *descriptor_;
    DmicAudioBridgeSharedHeader *header_;
    uint8_t *samples_;
    bool prepared_;
};

} // namespace sof::ipc4::dmic::kernel
