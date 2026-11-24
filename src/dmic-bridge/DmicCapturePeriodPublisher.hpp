#pragma once

#include "DmicKernelCaptureRuntime.hpp"
#include "DmicSharedCaptureRing.hpp"

namespace sof::ipc4::dmic::kernel {

// Non-gated IOC worker publisher. Owns one preallocated period scratch buffer,
// copies only a completed HDA period through CaptureRuntime, then publishes it
// to the read-only HAL bridge ring. It performs no MMIO or allocation in the
// worker callback.
class DmicCapturePeriodPublisher final {
public:
    DmicCapturePeriodPublisher() noexcept;
    ~DmicCapturePeriodPublisher();

    DmicCapturePeriodPublisher(const DmicCapturePeriodPublisher &) = delete;
    DmicCapturePeriodPublisher &operator=(
        const DmicCapturePeriodPublisher &) = delete;

    bool initialize(DmicSharedCaptureRing *ring,
                    uint32_t periodBytes) noexcept;
    bool ready() const noexcept {
        return initialized_ && ring_ && ring_->initialized() && scratch_;
    }
    IOReturn publishCompletedPeriod(
        CaptureRuntime *runtime,
        sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept;
    uint32_t lastRuntimeReadResult() const noexcept { return lastRuntimeReadResult_; }
    uint32_t lastPublishResult() const noexcept { return lastPublishResult_; }
    bool lastRingPublishSucceeded() const noexcept { return lastRingPublishSucceeded_; }
    uint32_t lastNonzeroSampleCount() const noexcept { return lastNonzeroSampleCount_; }
    uint64_t lastPeakSampleMagnitude() const noexcept { return lastPeakSampleMagnitude_; }
    void resetLastResult() noexcept {
        lastRuntimeReadResult_ = UINT32_MAX;
        lastPublishResult_ = UINT32_MAX;
        lastRingPublishSucceeded_ = false;
        lastNonzeroSampleCount_ = 0;
        lastPeakSampleMagnitude_ = 0;
    }

private:
    DmicSharedCaptureRing *ring_;
    uint8_t *scratch_;
    uint32_t periodBytes_;
    uint32_t periodFrames_;
    bool initialized_;
    uint32_t lastRuntimeReadResult_;
    uint32_t lastPublishResult_;
    bool lastRingPublishSucceeded_;
    uint32_t lastNonzeroSampleCount_;
    uint64_t lastPeakSampleMagnitude_;
};

} // namespace sof::ipc4::dmic::kernel
