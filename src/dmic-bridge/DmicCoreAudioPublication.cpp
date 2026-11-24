#include "DmicCoreAudioPublication.hpp"
#include "DmicKernelCaptureRuntime.hpp"

#include <IOKit/IOLib.h>
#include <limits.h>
#include <stdint.h>

namespace sof::ipc4::dmic::kernel {
namespace {

constexpr uint32_t kDmicSampleRate = 48000;
constexpr uint32_t kDmicChannels = 2;
constexpr uint32_t kBytesPerSample = sizeof(int32_t);
constexpr uint32_t kBytesPerFrame = kDmicChannels * kBytesPerSample;

} // namespace

OSDefineMetaClassAndStructors(DmicIOAudioEngine, IOAudioEngine)
OSDefineMetaClassAndStructors(DmicCoreAudioDevice, IOAudioDevice)

bool DmicIOAudioEngine::init(OSDictionary *properties) {
    if (!IOAudioEngine::init(properties)) return false;
    control_ = nullptr;
    sharedRing_ = nullptr;
    inputStream_ = nullptr;
    sampleBufferDescriptor_ = nullptr;
    sampleBuffer_ = nullptr;
    periodScratch_ = nullptr;
    ringBytes_ = 0;
    periodBytes_ = 0;
    ringFrames_ = 0;
    periodFrames_ = 0;
    publishedFrames_ = 0;
    nextSequence_ = 1;
    pendingSequence_ = 0;
    configured_ = false;
    sampleBufferPrepared_ = false;
    pendingPeriod_ = false;
    failed_ = false;
    captureStarted_ = false;
    return true;
}

bool DmicCoreAudioDevice::init(OSDictionary *properties) {
    if (!IOAudioDevice::init(properties)) return false;
    control_ = nullptr;
    sharedRing_ = nullptr;
    ringBytes_ = 0;
    periodBytes_ = 0;
    configured_ = false;
    return true;
}

bool DmicIOAudioEngine::configure(DmicCaptureControl *control,
                                  uint32_t ringBytes,
                                  uint32_t periodBytes,
                                  DmicSharedCaptureRing *sharedRing) noexcept {
    if (configured_ || !control || !periodBytes || !ringBytes ||
        (ringBytes % kBytesPerFrame) != 0 ||
        (periodBytes % kBytesPerFrame) != 0 ||
        (ringBytes % periodBytes) != 0 ||
        static_cast<uint64_t>(ringBytes) <
            static_cast<uint64_t>(periodBytes) * 3U)
        return false;

    const uint32_t ringFrames = ringBytes / kBytesPerFrame;
    const uint32_t periodFrames = periodBytes / kBytesPerFrame;
    if (!ringFrames || !periodFrames) return false;

    control_ = control;
    sharedRing_ = sharedRing;
    ringBytes_ = ringBytes;
    periodBytes_ = periodBytes;
    ringFrames_ = ringFrames;
    periodFrames_ = periodFrames;
    configured_ = true;
    return true;
}

bool DmicIOAudioEngine::initHardware(IOService *provider) {
    if (!configured_ || !provider || !IOAudioEngine::initHardware(provider))
        return false;

    sampleBufferDescriptor_ = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionIn | kIOMemoryKernelUserShared, ringBytes_,
        kBytesPerFrame);
    if (!sampleBufferDescriptor_) return false;
    if (sampleBufferDescriptor_->prepare() != kIOReturnSuccess) {
        sampleBufferDescriptor_->release();
        sampleBufferDescriptor_ = nullptr;
        return false;
    }
    sampleBufferPrepared_ = true;
    sampleBuffer_ = static_cast<uint8_t *>(
        sampleBufferDescriptor_->getBytesNoCopy());
    if (!sampleBuffer_) return false;
    bzero(sampleBuffer_, ringBytes_);

    periodScratch_ = static_cast<uint8_t *>(IOMalloc(periodBytes_));
    if (!periodScratch_) return false;
    bzero(periodScratch_, periodBytes_);

    IOAudioSampleRate rate = {kDmicSampleRate, 0};
    setNumSampleFramesPerBuffer(ringFrames_);
    setSampleRate(&rate);
    setSampleLatency(periodFrames_ * 2U);
    setDescription("Intel SST DMIC capture");

    inputStream_ = new IOAudioStream;
    if (!inputStream_) return false;
    if (!inputStream_->initWithAudioEngine(
            this, kIOAudioStreamDirectionInput, 1, "Internal Microphone")) {
        inputStream_->release();
        inputStream_ = nullptr;
        return false;
    }

    IOAudioStreamFormat format = {};
    format.fNumChannels = kDmicChannels;
    format.fSampleFormat = kIOAudioStreamSampleFormatLinearPCM;
    format.fNumericRepresentation =
        kIOAudioStreamNumericRepresentationSignedInt;
    format.fBitDepth = 32;
    format.fBitWidth = 32;
    format.fAlignment = kIOAudioStreamAlignmentLowByte;
    format.fByteOrder = kIOAudioStreamByteOrderLittleEndian;
    format.fIsMixable = 1;
    format.fDriverTag = 0;
    inputStream_->addAvailableFormat(&format, &rate, &rate);
    inputStream_->setSampleBuffer(sampleBuffer_, ringBytes_);
    if (addAudioStream(inputStream_) != kIOReturnSuccess) return false;
    return true;
}

IOReturn DmicIOAudioEngine::beginCapturePeriod(
    uint8_t **destination, uint64_t *sequence) noexcept {
    if (!destination || !sequence) return kIOReturnBadArgument;
    *destination = nullptr;
    *sequence = 0;
    if (!configured_ || !sampleBuffer_ || !periodScratch_ ||
        __atomic_load_n(&failed_, __ATOMIC_ACQUIRE) || pendingPeriod_ ||
        !__atomic_load_n(&captureStarted_, __ATOMIC_ACQUIRE))
        return kIOReturnNotReady;
    if (!nextSequence_) {
        __atomic_store_n(&failed_, true, __ATOMIC_RELEASE);
        return kIOReturnNoResources;
    }
    pendingSequence_ = nextSequence_++;
    pendingPeriod_ = true;
    *destination = periodScratch_;
    *sequence = pendingSequence_;
    return kIOReturnSuccess;
}

IOReturn DmicIOAudioEngine::commitCapturePeriod(
    uint64_t sequence, bool readSucceeded) noexcept {
    if (!pendingPeriod_ || !sequence || sequence != pendingSequence_ ||
        !sampleBuffer_)
        return kIOReturnNotReady;

    pendingPeriod_ = false;
    pendingSequence_ = 0;
    const uint64_t publishedFrames =
        __atomic_load_n(&publishedFrames_, __ATOMIC_RELAXED);
    if (!readSucceeded || publishedFrames > UINT64_MAX - periodFrames_) {
        __atomic_store_n(&failed_, true, __ATOMIC_RELEASE);
        return readSucceeded ? kIOReturnNoResources : kIOReturnIOError;
    }

    const uint32_t frameOffset = static_cast<uint32_t>(
        publishedFrames % ringFrames_);
    const uint32_t firstFrames =
        periodFrames_ < ringFrames_ - frameOffset
            ? periodFrames_ : ringFrames_ - frameOffset;
    const uint32_t firstBytes = firstFrames * kBytesPerFrame;
    memcpy(sampleBuffer_ + frameOffset * kBytesPerFrame, periodScratch_,
           firstBytes);
    if (firstBytes < periodBytes_)
        memcpy(sampleBuffer_, periodScratch_ + firstBytes,
               periodBytes_ - firstBytes);

    if (sharedRing_ && !sharedRing_->publish(periodScratch_, periodFrames_)) {
        __atomic_store_n(&failed_, true, __ATOMIC_RELEASE);
        return kIOReturnIOError;
    }

    const uint64_t newPublishedFrames = publishedFrames + periodFrames_;
    const uint64_t oldLoop = publishedFrames / ringFrames_;
    __atomic_store_n(&publishedFrames_, newPublishedFrames, __ATOMIC_RELEASE);
    if ((newPublishedFrames / ringFrames_) != oldLoop) takeTimeStamp();
    return kIOReturnSuccess;
}

IOReturn DmicIOAudioEngine::cancelCapturePeriod(
    uint64_t sequence, bool fatalError) noexcept {
    if (!pendingPeriod_ || !sequence || sequence != pendingSequence_)
        return kIOReturnNotReady;
    pendingPeriod_ = false;
    pendingSequence_ = 0;
    if (fatalError) __atomic_store_n(&failed_, true, __ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

IOReturn DmicIOAudioEngine::publishCompletedPeriod(
    CaptureRuntime *runtime,
    sof::ipc4::kernel::HdaCaptureTriggerAction *nextAction) noexcept {
    if (!runtime || !nextAction) return kIOReturnBadArgument;
    uint8_t *destination = nullptr;
    uint64_t sequence = 0;
    IOReturn result = beginCapturePeriod(&destination, &sequence);
    if (result != kIOReturnSuccess) return result;

    const CaptureRuntimeResult readResult =
        runtime->readNextCompletedPeriod(destination, nextAction);
    if (readResult == CaptureRuntimeResult::PeriodRead)
        return commitCapturePeriod(sequence, true);

    const bool fatal = readResult != CaptureRuntimeResult::NoPeriodReady;
    const IOReturn cancelResult = cancelCapturePeriod(sequence, fatal);
    if (cancelResult != kIOReturnSuccess) return cancelResult;
    return fatal ? kIOReturnIOError : kIOReturnNotReady;
}

UInt32 DmicIOAudioEngine::getCurrentSampleFrame() {
    if (!ringFrames_) return 0;
    const uint64_t publishedFrames =
        __atomic_load_n(&publishedFrames_, __ATOMIC_ACQUIRE);
    return static_cast<UInt32>(publishedFrames % ringFrames_);
}

IOReturn DmicIOAudioEngine::convertInputSamples(
    const void *sampleBuffer, void *destination, UInt32 firstSampleFrame,
    UInt32 sampleFrames, const IOAudioStreamFormat *streamFormat,
    IOAudioStream *audioStream) {
    if (!sampleBuffer || !destination || !streamFormat || !audioStream ||
        streamFormat->fNumChannels != kDmicChannels ||
        streamFormat->fBitDepth != 32 || !ringFrames_)
        return kIOReturnBadArgument;

    const int32_t *source = static_cast<const int32_t *>(sampleBuffer);
    float *output = static_cast<float *>(destination);
    for (UInt32 frame = 0; frame < sampleFrames; ++frame) {
        const uint32_t sourceFrame =
            (firstSampleFrame + frame) % ringFrames_;
        for (UInt32 channel = 0; channel < kDmicChannels; ++channel) {
            const int32_t sample =
                source[sourceFrame * kDmicChannels + channel];
            output[frame * kDmicChannels + channel] =
                static_cast<float>(static_cast<double>(sample) /
                                   2147483648.0);
        }
    }
    return kIOReturnSuccess;
}

IOReturn DmicIOAudioEngine::performAudioEngineStart() {
    if (!configured_ || !control_ ||
        __atomic_load_n(&failed_, __ATOMIC_ACQUIRE))
        return kIOReturnNotReady;
    const IOReturn result = control_->startDmicCapture();
    if (result == kIOReturnSuccess)
        __atomic_store_n(&captureStarted_, true, __ATOMIC_RELEASE);
    return result;
}

IOReturn DmicIOAudioEngine::performAudioEngineStop() {
    if (!__atomic_load_n(&captureStarted_, __ATOMIC_ACQUIRE))
        return kIOReturnSuccess;
    if (!control_) return kIOReturnNotReady;
    const IOReturn result = control_->stopDmicCapture();
    if (result == kIOReturnSuccess)
        __atomic_store_n(&captureStarted_, false, __ATOMIC_RELEASE);
    return result;
}

void DmicIOAudioEngine::free() {
    if (inputStream_) {
        inputStream_->setSampleBuffer(nullptr, 0);
        inputStream_->release();
        inputStream_ = nullptr;
    }
    if (periodScratch_) {
        IOFree(periodScratch_, periodBytes_);
        periodScratch_ = nullptr;
    }
    if (sampleBufferDescriptor_) {
        if (sampleBufferPrepared_) sampleBufferDescriptor_->complete();
        sampleBufferDescriptor_->release();
        sampleBufferDescriptor_ = nullptr;
        sampleBuffer_ = nullptr;
        sampleBufferPrepared_ = false;
    }
    IOAudioEngine::free();
}

bool DmicCoreAudioDevice::configureCapture(
    DmicCaptureControl *control, uint32_t ringBytes,
    uint32_t periodBytes, DmicSharedCaptureRing *sharedRing) noexcept {
    if (configured_ || !control || !ringBytes || !periodBytes ||
        (ringBytes % kBytesPerFrame) != 0 ||
        (periodBytes % kBytesPerFrame) != 0 ||
        (ringBytes % periodBytes) != 0 ||
        static_cast<uint64_t>(ringBytes) <
            static_cast<uint64_t>(periodBytes) * 3U)
        return false;
    control_ = control;
    sharedRing_ = sharedRing;
    ringBytes_ = ringBytes;
    periodBytes_ = periodBytes;
    configured_ = true;
    return true;
}

bool DmicCoreAudioDevice::initHardware(IOService *provider) {
    if (!configured_ || !provider || !IOAudioDevice::initHardware(provider))
        return false;

    setDeviceName("HP Internal Microphone");
    setDeviceShortName("HP DMIC");
    setManufacturerName("Hewlett-Packard");

    auto *engine = new DmicIOAudioEngine;
    if (!engine || !engine->init(nullptr) ||
        !engine->configure(control_, ringBytes_, periodBytes_, sharedRing_)) {
        if (engine) engine->release();
        return false;
    }
    if (activateAudioEngine(engine) != kIOReturnSuccess) {
        engine->release();
        return false;
    }
    return true;
}

} // namespace sof::ipc4::dmic::kernel
