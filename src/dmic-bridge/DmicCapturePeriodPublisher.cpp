#include "DmicCapturePeriodPublisher.hpp"

#include <IOKit/IOLib.h>

namespace sof::ipc4::dmic::kernel {
namespace {
constexpr uint32_t kBridgeBytesPerFrame =
    DMIC_AUDIO_BRIDGE_CHANNELS * DMIC_AUDIO_BRIDGE_BYTES_PER_SAMPLE;
}

DmicCapturePeriodPublisher::DmicCapturePeriodPublisher() noexcept
    : ring_(nullptr), scratch_(nullptr), periodBytes_(0), periodFrames_(0),
      initialized_(false), lastRuntimeReadResult_(UINT32_MAX),
      lastPublishResult_(UINT32_MAX), lastRingPublishSucceeded_(false),
      lastNonzeroSampleCount_(0), lastPeakSampleMagnitude_(0) {}

DmicCapturePeriodPublisher::~DmicCapturePeriodPublisher() {
    if (scratch_) IOFree(scratch_, periodBytes_);
}

bool DmicCapturePeriodPublisher::initialize(
    DmicSharedCaptureRing *ring, uint32_t periodBytes) noexcept {
    if (initialized_ || !ring || !ring->initialized() || !periodBytes ||
        periodBytes % kBridgeBytesPerFrame != 0)
        return false;
    const uint32_t periodFrames = periodBytes / kBridgeBytesPerFrame;
    if (!periodFrames || periodFrames > DMIC_AUDIO_BRIDGE_RING_FRAMES)
        return false;
    auto *scratch = static_cast<uint8_t *>(IOMalloc(periodBytes));
    if (!scratch) return false;
    bzero(scratch, periodBytes);
    ring_ = ring;
    scratch_ = scratch;
    periodBytes_ = periodBytes;
    periodFrames_ = periodFrames;
    initialized_ = true;
    return true;
}

IOReturn DmicCapturePeriodPublisher::publishCompletedPeriod(
    CaptureRuntime *runtime,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!ready() || !runtime || !nextAction) {
        lastPublishResult_ = static_cast<uint32_t>(kIOReturnBadArgument);
        return kIOReturnBadArgument;
    }
    const CaptureRuntimeResult result =
        runtime->readNextCompletedPeriod(scratch_, nextAction);
    lastRuntimeReadResult_ = static_cast<uint32_t>(result);
    lastRingPublishSucceeded_ = false;
    if (result == CaptureRuntimeResult::NoPeriodReady) {
        lastPublishResult_ = static_cast<uint32_t>(kIOReturnNotReady);
        return kIOReturnNotReady;
    }
    if (result != CaptureRuntimeResult::PeriodRead) {
        lastPublishResult_ = static_cast<uint32_t>(kIOReturnIOError);
        return kIOReturnIOError;
    }
    lastNonzeroSampleCount_ = 0;
    lastPeakSampleMagnitude_ = 0;
    const auto *samples = reinterpret_cast<const int32_t *>(scratch_);
    const uint32_t sampleCount = periodBytes_ / sizeof(int32_t);
    for (uint32_t index = 0; index < sampleCount; ++index) {
        const int64_t sample = samples[index];
        const uint64_t magnitude = static_cast<uint64_t>(
            sample < 0 ? -sample : sample);
        if (magnitude != 0) ++lastNonzeroSampleCount_;
        if (magnitude > lastPeakSampleMagnitude_)
            lastPeakSampleMagnitude_ = magnitude;
    }
    lastRingPublishSucceeded_ = ring_->publish(scratch_, periodFrames_);
    lastPublishResult_ = static_cast<uint32_t>(lastRingPublishSucceeded_
        ? kIOReturnSuccess : kIOReturnIOError);
    return lastRingPublishSucceeded_ ? kIOReturnSuccess : kIOReturnIOError;
}

} // namespace sof::ipc4::dmic::kernel
