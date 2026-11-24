#include "DmicSharedCaptureRing.hpp"

#include <IOKit/IOLib.h>
#include <limits.h>

namespace sof::ipc4::dmic::kernel {
namespace {

static_assert(sizeof(DmicAudioBridgeSharedHeader) == 64,
              "Audio bridge header must remain one cache line");
constexpr uint64_t kSharedBytes = sizeof(DmicAudioBridgeSharedHeader) +
    static_cast<uint64_t>(DMIC_AUDIO_BRIDGE_RING_BYTES);

} // namespace

DmicSharedCaptureRing::DmicSharedCaptureRing() noexcept
    : descriptor_(nullptr), header_(nullptr), samples_(nullptr),
      prepared_(false) {}

DmicSharedCaptureRing::~DmicSharedCaptureRing() {
    if (!descriptor_) return;
    if (prepared_) descriptor_->complete();
    descriptor_->release();
}

bool DmicSharedCaptureRing::initialize() noexcept {
    if (descriptor_ || kSharedBytes > UINT32_MAX) return false;
    descriptor_ = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionIn | kIOMemoryKernelUserShared,
        static_cast<IOByteCount>(kSharedBytes), 64);
    if (!descriptor_) return false;
    if (descriptor_->prepare() != kIOReturnSuccess) {
        descriptor_->release();
        descriptor_ = nullptr;
        return false;
    }
    prepared_ = true;
    auto *bytes = static_cast<uint8_t *>(descriptor_->getBytesNoCopy());
    if (!bytes || descriptor_->getLength() < kSharedBytes) {
        descriptor_->complete();
        descriptor_->release();
        descriptor_ = nullptr;
        prepared_ = false;
        return false;
    }

    bzero(bytes, static_cast<size_t>(kSharedBytes));
    header_ = reinterpret_cast<DmicAudioBridgeSharedHeader *>(bytes);
    samples_ = bytes + sizeof(DmicAudioBridgeSharedHeader);
    header_->magic = DMIC_AUDIO_BRIDGE_MAGIC;
    header_->abiVersion = DMIC_AUDIO_BRIDGE_ABI_VERSION;
    header_->headerBytes = static_cast<uint16_t>(
        sizeof(DmicAudioBridgeSharedHeader));
    header_->sampleRate = DMIC_AUDIO_BRIDGE_SAMPLE_RATE;
    header_->channels = DMIC_AUDIO_BRIDGE_CHANNELS;
    header_->bytesPerSample = DMIC_AUDIO_BRIDGE_BYTES_PER_SAMPLE;
    header_->capacityFrames = DMIC_AUDIO_BRIDGE_RING_FRAMES;
    __atomic_store_n(&header_->status,
                     static_cast<uint32_t>(kDmicAudioBridgeStopped),
                     __ATOMIC_RELEASE);
    return true;
}

bool DmicSharedCaptureRing::setStatus(
    DmicAudioBridgeStatus status) noexcept {
    if (!header_ || status > kDmicAudioBridgeFailed ||
        status == kDmicAudioBridgeRunning)
        return false;
    __atomic_store_n(&header_->status, static_cast<uint32_t>(status),
                     __ATOMIC_RELEASE);
    return true;
}

bool DmicSharedCaptureRing::beginCapture() noexcept {
    if (!header_ || !samples_) return false;
    if (!setStatus(kDmicAudioBridgeStopped)) return false;
    const uint64_t generation =
        __atomic_load_n(&header_->generation, __ATOMIC_RELAXED);
    if (generation == UINT64_MAX) {
        (void)setStatus(kDmicAudioBridgeFailed);
        return false;
    }
    __atomic_store_n(&header_->generation, generation + 1U,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&header_->writeFrames, 0U, __ATOMIC_RELAXED);
    bzero(samples_, DMIC_AUDIO_BRIDGE_RING_BYTES);
    __atomic_store_n(&header_->status,
                     static_cast<uint32_t>(kDmicAudioBridgeRunning),
                     __ATOMIC_RELEASE);
    return true;
}

bool DmicSharedCaptureRing::publish(const void *stereoS32Frames,
                                    uint32_t frameCount) noexcept {
    if (!header_ || !samples_ || !stereoS32Frames || frameCount == 0 ||
        frameCount > DMIC_AUDIO_BRIDGE_RING_FRAMES ||
        __atomic_load_n(&header_->status, __ATOMIC_ACQUIRE) !=
            static_cast<uint32_t>(kDmicAudioBridgeRunning))
        return false;

    const uint64_t oldWriteFrames =
        __atomic_load_n(&header_->writeFrames, __ATOMIC_RELAXED);
    if (oldWriteFrames > UINT64_MAX - frameCount) return false;

    const uint32_t startFrame = static_cast<uint32_t>(
        oldWriteFrames % DMIC_AUDIO_BRIDGE_RING_FRAMES);
    const uint32_t firstFrames = frameCount <
            DMIC_AUDIO_BRIDGE_RING_FRAMES - startFrame
        ? frameCount : DMIC_AUDIO_BRIDGE_RING_FRAMES - startFrame;
    const uint32_t firstBytes =
        firstFrames * DMIC_AUDIO_BRIDGE_BYTES_PER_FRAME;
    const uint32_t totalBytes =
        frameCount * DMIC_AUDIO_BRIDGE_BYTES_PER_FRAME;
    const auto *source = static_cast<const uint8_t *>(stereoS32Frames);
    memcpy(samples_ + startFrame * DMIC_AUDIO_BRIDGE_BYTES_PER_FRAME,
           source, firstBytes);
    if (firstBytes < totalBytes)
        memcpy(samples_, source + firstBytes, totalBytes - firstBytes);

    // The HAL must see the completed sample bytes before the producer cursor.
    __atomic_store_n(&header_->writeFrames, oldWriteFrames + frameCount,
                     __ATOMIC_RELEASE);
    return true;
}

} // namespace sof::ipc4::dmic::kernel
