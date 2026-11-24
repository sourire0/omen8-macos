#pragma once

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/audio/IOAudioEngine.h>
#include <IOKit/audio/IOAudioStream.h>
#include "HdaCaptureTriggerPlan.hpp"
#include "DmicSharedCaptureRing.hpp"

namespace sof::ipc4::dmic::kernel {
class CaptureRuntime;
}

namespace sof::ipc4::dmic::kernel {

// Implemented by the controller owner. Calls must be serialized on its
// workloop; this publication layer never accesses PCI, DSP, or HDA registers.
class DmicCaptureControl {
public:
    virtual IOReturn startDmicCapture() noexcept = 0;
    virtual IOReturn stopDmicCapture() noexcept = 0;
};

class DmicIOAudioEngine final : public IOAudioEngine {
    OSDeclareDefaultStructors(DmicIOAudioEngine)

public:
    bool init(OSDictionary *properties = nullptr) override;
    bool configure(DmicCaptureControl *control, uint32_t ringBytes,
                   uint32_t periodBytes,
                   DmicSharedCaptureRing *sharedRing = nullptr) noexcept;
    IOReturn beginCapturePeriod(uint8_t **destination,
                                uint64_t *sequence) noexcept;
    IOReturn commitCapturePeriod(uint64_t sequence,
                                 bool readSucceeded) noexcept;
    IOReturn cancelCapturePeriod(uint64_t sequence,
                                 bool fatalError) noexcept;
    IOReturn publishCompletedPeriod(CaptureRuntime *runtime,
                                    sof::ipc4::kernel::HdaCaptureTriggerAction
                                        *nextAction) noexcept;

    UInt32 getCurrentSampleFrame() override;
    IOReturn convertInputSamples(const void *sampleBuffer, void *destination,
                                 UInt32 firstSampleFrame,
                                 UInt32 sampleFrames,
                                 const IOAudioStreamFormat *streamFormat,
                                 IOAudioStream *audioStream) override;
    IOReturn performAudioEngineStart() override;
    IOReturn performAudioEngineStop() override;
    bool initHardware(IOService *provider) override;
    void free() override;

private:
    DmicCaptureControl *control_;
    DmicSharedCaptureRing *sharedRing_;
    IOAudioStream *inputStream_;
    IOBufferMemoryDescriptor *sampleBufferDescriptor_;
    uint8_t *sampleBuffer_;
    uint8_t *periodScratch_;
    uint32_t ringBytes_;
    uint32_t periodBytes_;
    uint32_t ringFrames_;
    uint32_t periodFrames_;
    uint64_t publishedFrames_;
    uint64_t nextSequence_;
    uint64_t pendingSequence_;
    bool configured_;
    bool sampleBufferPrepared_;
    bool pendingPeriod_;
    bool failed_;
    bool captureStarted_;
};

class DmicCoreAudioDevice final : public IOAudioDevice {
    OSDeclareDefaultStructors(DmicCoreAudioDevice)

public:
    bool init(OSDictionary *properties = nullptr) override;
    bool configureCapture(DmicCaptureControl *control, uint32_t ringBytes,
                          uint32_t periodBytes,
                          DmicSharedCaptureRing *sharedRing = nullptr) noexcept;
    bool initHardware(IOService *provider) override;

private:
    DmicCaptureControl *control_;
    DmicSharedCaptureRing *sharedRing_;
    uint32_t ringBytes_;
    uint32_t periodBytes_;
    bool configured_;
};

} // namespace sof::ipc4::dmic::kernel
