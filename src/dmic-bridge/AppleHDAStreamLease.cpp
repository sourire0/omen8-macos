#include "AppleHDAStreamLease.hpp"

#include <IOKit/IOCommandGate.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/OSKextLib.h>
namespace {

// AppleHDAController 600.2 (UUID 127B008B-0862-3232-9A2B-05A8B1C75BEE).
// The executable in MULL SimpleHDA.kext is byte-identical to this payload.
extern "C" IOHDAStream *hpipc4AllocateAudioStream(IOService *)
    __asm__("__ZN18AppleHDAController19allocateAudioStreamEv")
    __attribute__((weak_import));
extern "C" void hpipc4DeallocateAudioStream(IOService *, IOHDAStream *)
    __asm__("__ZN18AppleHDAController21deallocateAudioStreamEP11IOHDAStream")
    __attribute__((weak_import));
extern "C" IOReturn hpipc4AllocateStreamMemory(
    IOService *, void *, uint32_t)
    __asm__("__ZN18AppleHDAController20allocateStreamMemoryEP10StreamInfoj")
    __attribute__((weak_import));
extern "C" IOReturn hpipc4SetDataFormat(IOHDAStream *, uint32_t)
    __asm__("__ZN11IOHDAStream13setDataFormatEj") __attribute__((weak_import));
extern "C" IOReturn hpipc4SetSampleRate(IOHDAStream *, uint32_t, bool)
    __asm__("__ZN11IOHDAStream13setSampleRateEjb") __attribute__((weak_import));
extern "C" IOReturn hpipc4ActivateAudioStream(IOService *, IOHDAStream *,
                                                uint32_t, uint32_t, bool)
    __asm__("__ZN18AppleHDAController19activateAudioStreamEP11IOHDAStreamjjb")
    __attribute__((weak_import));
extern "C" IOReturn hpipc4DeactivateAudioStream(IOService *, IOHDAStream *,
                                                  uint32_t)
    __asm__("__ZN18AppleHDAController21deactivateAudioStreamEP11IOHDAStreamj")
    __attribute__((weak_import));
extern "C" IOReturn hpipc4StartDmaEngine(IOHDAStream *, uint32_t)
    __asm__("__ZN11IOHDAStream14startDMAEngineEj") __attribute__((weak_import));
extern "C" IOReturn hpipc4StopDmaEngine(IOHDAStream *, uint32_t)
    __asm__("__ZN11IOHDAStream13stopDMAEngineEj") __attribute__((weak_import));
extern "C" IOReturn hpipc4SetBitsPerSample(IOHDAStream *, uint32_t)
    __asm__("__ZN11IOHDAStream16setBitsPerSampleEj") __attribute__((weak_import));
extern "C" IOReturn hpipc4SetStreamDirection(IOHDAStream *, uint32_t)
    __asm__("__ZN11IOHDAStream18setStreamDirectionEj") __attribute__((weak_import));
extern "C" IOReturn hpipc4SetLinkChannelCount(IOHDAStream *, uint32_t)
    __asm__("__ZN11IOHDAStream19setLinkChannelCountEj") __attribute__((weak_import));
extern "C" IOReturn hpipc4RegisterStreamInterrupt(
    IOService *, IOHDAStream *, void *,
    void (*)(void *, void *, bool, uint64_t *), void *, uint32_t)
    __asm__("__ZN18AppleHDAController28registerAudioStreamInterruptEP11IOHDAStreamPvPFvS2_S2_bPyES2_j")
    __attribute__((weak_import));
extern "C" IOReturn hpipc4UnregisterStreamInterrupt(IOService *, IOHDAStream *)
    __asm__("__ZN18AppleHDAController30unregisterAudioStreamInterruptEP11IOHDAStream")
    __attribute__((weak_import));
extern "C" IOReturn hpipc4GetCyclicBuffer(IOService *, IOHDAStream *,
                                           IOBufferMemoryDescriptor **)
    __asm__("__ZN18AppleHDAController32getAudioStreamCyclicBufferMemoryEP11IOHDAStreamPP24IOBufferMemoryDescriptor")
    __attribute__((weak_import));
extern "C" IOReturn hpipc4GetLinkPosition(IOService *, IOHDAStream *,
                                            uint64_t *)
    __asm__("__ZN18AppleHDAController37getAudioStreamLinkPositionInDMABufferEP11IOHDAStreamPy")
    __attribute__((weak_import));
extern "C" void *hpipc4GetControlData(const IOHDAStream *)
    __asm__("__ZNK11IOHDAStream14getControlDataEv") __attribute__((weak_import));
extern "C" int32_t hpipc4GetStreamNumber(const IOHDAStream *)
    __asm__("__ZNK11IOHDAStream15getStreamNumberEv") __attribute__((weak_import));
extern "C" uint32_t hpipc4GetStreamDirection(const IOHDAStream *)
    __asm__("__ZNK11IOHDAStream18getStreamDirectionEv") __attribute__((weak_import));
extern "C" uint32_t hpipc4GetDataFormat(const IOHDAStream *)
    __asm__("__ZNK11IOHDAStream13getDataFormatEv") __attribute__((weak_import));
extern "C" uint32_t hpipc4GetSampleRate(const IOHDAStream *)
    __asm__("__ZNK11IOHDAStream13getSampleRateEv") __attribute__((weak_import));
extern "C" uint32_t hpipc4GetBitsPerSample(const IOHDAStream *)
    __asm__("__ZNK11IOHDAStream16getBitsPerSampleEv") __attribute__((weak_import));
extern "C" uint32_t hpipc4GetLinkChannelCount(const IOHDAStream *)
    __asm__("__ZNK11IOHDAStream19getLinkChannelCountEv") __attribute__((weak_import));

constexpr uint32_t kHdaInputDirection = 0;
constexpr uint32_t kHdaOutputDirection = 1;
constexpr uint32_t kStreamInfoCyclicBufferOffset = 0x20;
constexpr uint32_t kStreamInfoAllocatedChannelCountOffset = 0x44;
constexpr uint32_t kStreamInfoCyclicDmaCommandOffset = 0x78;
constexpr uint32_t kStreamInfoDescriptorIndexOffset = 0x08;
constexpr uint32_t kStreamInfoStreamTagOffset = 0x14;

IOReturn allocateStreamMemoryOnAppleHdaWorkLoop(
    OSObject *owner, void *streamInfo, void *allocationUnits,
    void *streamContext, void *) {
    auto *controller = OSDynamicCast(IOService, owner);
    auto *stream = static_cast<IOHDAStream *>(streamContext);
    const uintptr_t units = reinterpret_cast<uintptr_t>(allocationUnits);
    if (!controller || !streamInfo || !stream || units == 0 || units > UINT32_MAX)
        return kIOReturnBadArgument;

    const uint32_t channels = hpipc4GetLinkChannelCount(stream);
    if (channels == 0) return kIOReturnNotReady;
    const IOReturn result = hpipc4AllocateStreamMemory(
        controller, streamInfo, static_cast<uint32_t>(units));
    if (result != kIOReturnSuccess) return result;

    // AppleHDA 600.2 invalidates StreamInfo's buffers on activation when
    // the link channel count differs from this cache. A new StreamInfo has
    // a zero cache even after direct memory allocation. Set it just as
    // activateAudioStreamGated does, while the controller workloop is gated.
    auto *bytes = static_cast<uint8_t *>(streamInfo);
    auto *cachedChannels = reinterpret_cast<uint32_t *>(
        bytes + kStreamInfoAllocatedChannelCountOffset);
    *cachedChannels = channels;
    return kIOReturnSuccess;
}
} // namespace

bool AppleHDAStreamLease::apiAvailable() noexcept {
    return &hpipc4AllocateAudioStream != nullptr && &hpipc4AllocateAudioStream != gOSKextUnresolved &&
        &hpipc4DeallocateAudioStream != nullptr && &hpipc4DeallocateAudioStream != gOSKextUnresolved &&
        &hpipc4AllocateStreamMemory != nullptr && &hpipc4AllocateStreamMemory != gOSKextUnresolved &&
        &hpipc4SetDataFormat != nullptr && &hpipc4SetDataFormat != gOSKextUnresolved &&
        &hpipc4SetSampleRate != nullptr && &hpipc4SetSampleRate != gOSKextUnresolved &&
        &hpipc4ActivateAudioStream != nullptr && &hpipc4ActivateAudioStream != gOSKextUnresolved &&
        &hpipc4DeactivateAudioStream != nullptr && &hpipc4DeactivateAudioStream != gOSKextUnresolved &&
        &hpipc4StartDmaEngine != nullptr && &hpipc4StartDmaEngine != gOSKextUnresolved &&
        &hpipc4StopDmaEngine != nullptr && &hpipc4StopDmaEngine != gOSKextUnresolved &&
        &hpipc4SetBitsPerSample != nullptr && &hpipc4SetBitsPerSample != gOSKextUnresolved &&
        &hpipc4SetStreamDirection != nullptr && &hpipc4SetStreamDirection != gOSKextUnresolved &&
        &hpipc4SetLinkChannelCount != nullptr && &hpipc4SetLinkChannelCount != gOSKextUnresolved &&
        &hpipc4GetCyclicBuffer != nullptr && &hpipc4GetCyclicBuffer != gOSKextUnresolved &&
        &hpipc4GetLinkPosition != nullptr && &hpipc4GetLinkPosition != gOSKextUnresolved &&
        &hpipc4RegisterStreamInterrupt != nullptr && &hpipc4RegisterStreamInterrupt != gOSKextUnresolved &&
        &hpipc4UnregisterStreamInterrupt != nullptr && &hpipc4UnregisterStreamInterrupt != gOSKextUnresolved &&
        &hpipc4GetControlData != nullptr && &hpipc4GetControlData != gOSKextUnresolved &&
        &hpipc4GetStreamNumber != nullptr && &hpipc4GetStreamNumber != gOSKextUnresolved &&
        &hpipc4GetStreamDirection != nullptr && &hpipc4GetStreamDirection != gOSKextUnresolved &&
        &hpipc4GetDataFormat != nullptr && &hpipc4GetDataFormat != gOSKextUnresolved &&
        &hpipc4GetSampleRate != nullptr && &hpipc4GetSampleRate != gOSKextUnresolved &&
        &hpipc4GetBitsPerSample != nullptr && &hpipc4GetBitsPerSample != gOSKextUnresolved &&
        &hpipc4GetLinkChannelCount != nullptr && &hpipc4GetLinkChannelCount != gOSKextUnresolved;
}

AppleHDAStreamLease::AppleHDAStreamLease() noexcept
    : controller_(nullptr), stream_(nullptr), active_(false),
      interruptRegistered_(false) {}

AppleHDAStreamLease::~AppleHDAStreamLease() {
    release();
}

bool AppleHDAStreamLease::allocate(IOService *controller) noexcept {
    if (controller_ || stream_ || !controller || !apiAvailable()) return false;
    IOHDAStream *stream = hpipc4AllocateAudioStream(controller);
    if (!stream) return false;
    controller_ = controller;
    controller_->retain();
    stream_ = stream;
    return true;
}

IOReturn AppleHDAStreamLease::preallocateCyclicBuffer(
    uint32_t allocationUnits, uint32_t minimumBytes,
    uint64_t &allocatedBytes) noexcept {
    allocatedBytes = 0;
    if (!controller_ || !stream_ || active_ || allocationUnits == 0 ||
        minimumBytes == 0 || !apiAvailable())
        return kIOReturnNotReady;
    void *streamInfo = controlData();
    IOWorkLoop *workLoop = controller_->getWorkLoop();
    if (!streamInfo || !workLoop) return kIOReturnNotReady;
    auto *streamInfoBytes = static_cast<uint8_t *>(streamInfo);
    auto **existingBuffer = reinterpret_cast<IOBufferMemoryDescriptor **>(
        streamInfoBytes + kStreamInfoCyclicBufferOffset);
    if (*existingBuffer) return kIOReturnBusy;

    IOCommandGate *gate = OSTypeAlloc(IOCommandGate);
    if (!gate) return kIOReturnNoMemory;
    if (!gate->init(controller_, nullptr)) {
        gate->release();
        return kIOReturnNoResources;
    }
    const IOReturn addResult = workLoop->addEventSource(gate);
    if (addResult != kIOReturnSuccess) {
        gate->release();
        return addResult;
    }
    const IOReturn allocationResult = gate->runAction(
        allocateStreamMemoryOnAppleHdaWorkLoop, streamInfo,
        reinterpret_cast<void *>(static_cast<uintptr_t>(allocationUnits)), stream_);
    const IOReturn removeResult = workLoop->removeEventSource(gate);
    gate->release();
    if (allocationResult != kIOReturnSuccess) return allocationResult;
    if (removeResult != kIOReturnSuccess) return removeResult;

    // AppleHDAController's public cyclic-buffer getter deliberately refuses
    // inactive streams. For this exact 600.2 payload, its own getter reads
    // StreamInfo + 0x20; validate the just-allocated object there before the
    // normal activateAudioStream path consumes it.
    IOBufferMemoryDescriptor *buffer = *existingBuffer;
    if (!buffer) return kIOReturnNoMemory;
    allocatedBytes = buffer->getLength();
    return allocatedBytes >= minimumBytes ? kIOReturnSuccess : kIOReturnNoSpace;
}

IOReturn AppleHDAStreamLease::configurePcm(
    uint32_t direction, uint32_t dataFormat, uint32_t sampleRate,
    bool variableRate, uint32_t bitsPerSample,
    uint32_t linkChannelCount) noexcept {
    if (!stream_ || active_) return kIOReturnNotReady;
    IOReturn result = setDirection(direction);
    if (result != kIOReturnSuccess) return result;
    result = setDataFormat(dataFormat);
    if (result != kIOReturnSuccess) return result;
    result = setSampleRate(sampleRate, variableRate);
    if (result != kIOReturnSuccess) return result;
    result = setBitsPerSample(bitsPerSample);
    if (result != kIOReturnSuccess) return result;
    result = setLinkChannelCount(linkChannelCount);
    if (result != kIOReturnSuccess) return result;

    // Verify the exact public state that AppleHDA will use to build the HDA
    // stream format and cyclic buffer before asking its controller to activate.
    return hpipc4GetStreamDirection(stream_) == direction &&
            hpipc4GetDataFormat(stream_) == dataFormat &&
            hpipc4GetSampleRate(stream_) == sampleRate &&
            hpipc4GetBitsPerSample(stream_) == bitsPerSample &&
            hpipc4GetLinkChannelCount(stream_) == linkChannelCount
        ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleHDAStreamLease::setDirection(uint32_t direction) noexcept {
    if (!stream_ || active_ ||
        (direction != kHdaInputDirection && direction != kHdaOutputDirection))
        return kIOReturnNotReady;
    return hpipc4SetStreamDirection(stream_, direction);
}

IOReturn AppleHDAStreamLease::setDataFormat(uint32_t format) noexcept {
    if (!stream_ || active_) return kIOReturnNotReady;
    return hpipc4SetDataFormat(stream_, format);
}

IOReturn AppleHDAStreamLease::setSampleRate(uint32_t rate,
                                            bool variableRate) noexcept {
    if (!stream_ || active_ || rate == 0) return kIOReturnNotReady;
    return hpipc4SetSampleRate(stream_, rate, variableRate);
}

IOReturn AppleHDAStreamLease::setBitsPerSample(uint32_t bits) noexcept {
    if (!stream_ || active_ || bits == 0) return kIOReturnNotReady;
    return hpipc4SetBitsPerSample(stream_, bits);
}

IOReturn AppleHDAStreamLease::setLinkChannelCount(uint32_t channels) noexcept {
    if (!stream_ || active_ || channels == 0) return kIOReturnNotReady;
    return hpipc4SetLinkChannelCount(stream_, channels);
}

IOReturn AppleHDAStreamLease::activate(uint32_t activationOption,
                                       uint32_t dmaBlockSizeShift,
                                       bool preferHighStreamTags) noexcept {
    // The matched AppleHDAController 600.2 implementation rejects shifts >= 3
    // before allocating the link stream. This is a shift count, not byte size.
    if (!stream_ || active_ || dmaBlockSizeShift >= 3)
        return kIOReturnNotReady;
    if (!controller_) return kIOReturnNotReady;
    const IOReturn result = hpipc4ActivateAudioStream(controller_, stream_,
        activationOption, dmaBlockSizeShift, preferHighStreamTags);
    if (result == kIOReturnSuccess) active_ = true;
    return result;
}

IOReturn AppleHDAStreamLease::deactivate(uint32_t timeout) noexcept {
    if (!stream_ || !active_) return kIOReturnNotReady;
    if (!controller_) return kIOReturnNotReady;
    const IOReturn result = hpipc4DeactivateAudioStream(
        controller_, stream_, timeout);
    if (result == kIOReturnSuccess) active_ = false;
    return result;
}

IOReturn AppleHDAStreamLease::startDmaEngine(uint32_t timeout) noexcept {
    if (!stream_ || !active_) return kIOReturnNotReady;
    return hpipc4StartDmaEngine(stream_, timeout);
}

IOReturn AppleHDAStreamLease::stopDmaEngine(uint32_t timeout) noexcept {
    if (!stream_ || !active_) return kIOReturnNotReady;
    return hpipc4StopDmaEngine(stream_, timeout);
}

IOReturn AppleHDAStreamLease::registerInterrupt(
    void *target, void (*action)(void *, void *, bool, uint64_t *),
    void *refcon, uint32_t options) noexcept {
    if (!stream_ || !active_ || interruptRegistered_ || !target || !action)
        return kIOReturnNotReady;
    if (!controller_) return kIOReturnNotReady;
    const IOReturn result = hpipc4RegisterStreamInterrupt(controller_, stream_,
        target, action, refcon, options);
    if (result == kIOReturnSuccess) interruptRegistered_ = true;
    return result;
}

IOReturn AppleHDAStreamLease::unregisterInterrupt() noexcept {
    if (!stream_ || !interruptRegistered_) return kIOReturnNotReady;
    if (!controller_) return kIOReturnNotReady;
    const IOReturn result = hpipc4UnregisterStreamInterrupt(controller_, stream_);
    if (result == kIOReturnSuccess) interruptRegistered_ = false;
    return result;
}

IOMemoryDescriptor *AppleHDAStreamLease::cyclicBuffer() const noexcept {
    if (!controller_ || !stream_) return nullptr;
    IOBufferMemoryDescriptor *buffer = nullptr;
    if (hpipc4GetCyclicBuffer(controller_, stream_, &buffer) !=
            kIOReturnSuccess || !buffer)
        return nullptr;
    return buffer;
}

IODMACommand *AppleHDAStreamLease::cyclicBufferCommand(
    const IOMemoryDescriptor *buffer) const noexcept {
    if (!active_ || !buffer) return nullptr;
    const auto *info = static_cast<const uint8_t *>(controlData());
    if (!info) return nullptr;
    auto *command = *reinterpret_cast<IODMACommand *const *>(
        info + kStreamInfoCyclicDmaCommandOffset);
    return command && command->getMemoryDescriptor() == buffer ? command : nullptr;
}

bool AppleHDAStreamLease::linkPositionBytes(uint32_t *position) const noexcept {
    if (!controller_ || !stream_ || !active_ || !position)
        return false;
    uint64_t value = UINT64_MAX;
    if (hpipc4GetLinkPosition(controller_, stream_, &value) !=
            kIOReturnSuccess || value > UINT32_MAX)
        return false;
    *position = static_cast<uint32_t>(value);
    return true;
}

void *AppleHDAStreamLease::controlData() const noexcept {
    return stream_ ? hpipc4GetControlData(stream_) : nullptr;
}

bool AppleHDAStreamLease::activeDescriptor(
    uint8_t inputCount, uint8_t outputCount, uint8_t totalCount,
    uint8_t *descriptorIndex, uint8_t *streamTag) const noexcept {
    if (!stream_ || !active_ || !descriptorIndex || !streamTag ||
        inputCount > totalCount || outputCount > totalCount ||
        totalCount > 30)
        return false;
    const auto *bytes = static_cast<const volatile uint8_t *>(controlData());
    if (!bytes) return false;
    OSSynchronizeIO();
    const uint32_t index = *reinterpret_cast<const volatile uint32_t *>(
        bytes + kStreamInfoDescriptorIndexOffset);
    const uint32_t tag = *reinterpret_cast<const volatile uint32_t *>(
        bytes + kStreamInfoStreamTagOffset);
    OSSynchronizeIO();
    const uint32_t direction = hpipc4GetStreamDirection(stream_);
    uint32_t first = 0;
    uint32_t end = inputCount;
    if (direction == kHdaOutputDirection) {
        first = inputCount;
        end = first + outputCount;
    } else if (direction != kHdaInputDirection) {
        // Bidirectional descriptors aren't exposed by this lease until their
        // exact placement is confirmed against the current GCAP layout.
        return false;
    }
    const uint8_t streamNumber = static_cast<uint8_t>(
        hpipc4GetStreamNumber(stream_));
    if (index < first || index >= end || index >= totalCount ||
        tag == 0 || tag > 15 || streamNumber != tag)
        return false;
    *descriptorIndex = static_cast<uint8_t>(index);
    *streamTag = static_cast<uint8_t>(tag);
    return true;
}

void AppleHDAStreamLease::release() noexcept {
    if (!stream_) {
        OSSafeReleaseNULL(controller_);
        return;
    }
    if (interruptRegistered_) (void)unregisterInterrupt();
    if (active_) (void)deactivate(0);
    if (!active_ && !interruptRegistered_ && controller_)
        hpipc4DeallocateAudioStream(controller_, stream_);
    // If AppleHDA refused teardown, retain the objects rather than freeing
    // memory that could still be referenced by its active stream.
    if (!active_ && !interruptRegistered_) {
        stream_ = nullptr;
        OSSafeReleaseNULL(controller_);
    }
}
