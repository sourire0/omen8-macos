#pragma once

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/audio/IOAudioTypes.h>
#include <stdint.h>

class IOHDAStream;
class IODMACommand;

// Narrow ABI shim for the exact AppleHDAController/IOHDAFamily payload that
// SimpleHDA 1.0.0 restores on this EFI. Keep private stream state opaque here;
// callers must obtain descriptor/tag values through the guarded accessors.
class AppleHDAStreamLease final {
public:
    AppleHDAStreamLease() noexcept;
    ~AppleHDAStreamLease();

    AppleHDAStreamLease(const AppleHDAStreamLease &) = delete;
    AppleHDAStreamLease &operator=(const AppleHDAStreamLease &) = delete;

    bool allocate(IOService *controller) noexcept;
    static bool apiAvailable() noexcept;
    // allocationUnits is AppleHDAController's private allocator input, not a
    // byte count. The resulting cyclic buffer is read back and size checked.
    IOReturn preallocateCyclicBuffer(uint32_t allocationUnits,
                                    uint32_t minimumBytes,
                                    uint64_t &allocatedBytes) noexcept;
    IOReturn configurePcm(uint32_t direction, uint32_t dataFormat,
                          uint32_t sampleRate, bool variableRate,
                          uint32_t bitsPerSample,
                          uint32_t linkChannelCount) noexcept;
    IOReturn setDirection(uint32_t direction) noexcept;
    IOReturn setDataFormat(uint32_t format) noexcept;
    IOReturn setSampleRate(uint32_t rate, bool variableRate) noexcept;
    IOReturn setBitsPerSample(uint32_t bits) noexcept;
    IOReturn setLinkChannelCount(uint32_t channels) noexcept;
    IOReturn activate(uint32_t activationOption, uint32_t dmaBlockSizeShift,
                      bool preferHighStreamTags) noexcept;
    IOReturn startDmaEngine(uint32_t timeout) noexcept;
    IOReturn stopDmaEngine(uint32_t timeout) noexcept;
    IOReturn deactivate(uint32_t timeout) noexcept;
    IOReturn registerInterrupt(void *target,
        void (*action)(void *, void *, bool, uint64_t *), void *refcon,
        uint32_t options) noexcept;
    IOReturn unregisterInterrupt() noexcept;

    IOHDAStream *stream() const noexcept { return stream_; }
    IOService *controller() const noexcept { return controller_; }
    IOMemoryDescriptor *cyclicBuffer() const noexcept;
    IODMACommand *cyclicBufferCommand(const IOMemoryDescriptor *buffer) const noexcept;
    bool linkPositionBytes(uint32_t *position) const noexcept;
    void *controlData() const noexcept;
    bool active() const noexcept { return active_; }

    // Valid only for the matched 600.2 AppleHDAController payload. Its
    // activateAudioStreamGated writes StreamInfo.descriptorIndex at +0x08
    // and streamTag at +0x14; this method validates those values before use.
    bool activeDescriptor(uint8_t inputCount, uint8_t outputCount,
                          uint8_t totalCount, uint8_t *descriptorIndex,
                          uint8_t *streamTag) const noexcept;

    void release() noexcept;

private:
    IOService *controller_;
    IOHDAStream *stream_;
    bool active_;
    bool interruptRegistered_;
};
