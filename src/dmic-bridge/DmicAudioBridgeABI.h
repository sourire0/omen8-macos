#ifndef SOF_IPC4_DMIC_AUDIO_BRIDGE_ABI_H
#define SOF_IPC4_DMIC_AUDIO_BRIDGE_ABI_H

#include <stddef.h>
#include <stdint.h>

// Stable C ABI shared by the kernel service and AudioServerPlugIn. The audio
// samples are signed 32-bit little-endian stereo frames at 48 kHz. The plug-in
// receives a read-only mapping; only the kernel capture publisher advances the
// monotonic writeFrames counter.
#define DMIC_AUDIO_BRIDGE_ABI_VERSION 1U
#define DMIC_AUDIO_BRIDGE_MAGIC 0x444d4943U /* "DMIC" */
#define DMIC_AUDIO_BRIDGE_SAMPLE_RATE 48000U
#define DMIC_AUDIO_BRIDGE_CHANNELS 2U
#define DMIC_AUDIO_BRIDGE_BYTES_PER_SAMPLE 4U
#define DMIC_AUDIO_BRIDGE_BYTES_PER_FRAME 8U
#define DMIC_AUDIO_BRIDGE_RING_FRAMES 16384U
#define DMIC_AUDIO_BRIDGE_RING_BYTES \
    (DMIC_AUDIO_BRIDGE_RING_FRAMES * DMIC_AUDIO_BRIDGE_BYTES_PER_FRAME)
#define DMIC_AUDIO_BRIDGE_MAPPING_TYPE 0U

enum DmicAudioBridgeUserClientSelector {
    kDmicAudioBridgeStartCapture = 0,
    kDmicAudioBridgeStopCapture = 1,
    kDmicAudioBridgeGetInfo = 2,
    kDmicAudioBridgeSelectorCount = 3,
};

enum DmicAudioBridgeStatus {
    kDmicAudioBridgeStopped = 0,
    kDmicAudioBridgeRunning = 1,
    kDmicAudioBridgeFailed = 2,
};

typedef struct __attribute__((aligned(64))) DmicAudioBridgeSharedHeader {
    uint32_t magic;
    uint16_t abiVersion;
    uint16_t headerBytes;
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t bytesPerSample;
    uint32_t capacityFrames;
    uint32_t reserved0;
    uint64_t writeFrames;
    uint64_t generation;
    uint32_t status;
    uint32_t reserved[5];
} DmicAudioBridgeSharedHeader;

#if defined(__cplusplus)
static_assert(sizeof(DmicAudioBridgeSharedHeader) == 64,
              "Audio bridge header ABI must remain 64 bytes");
static_assert(offsetof(DmicAudioBridgeSharedHeader, writeFrames) == 24,
              "Audio bridge write cursor offset changed");
static_assert(offsetof(DmicAudioBridgeSharedHeader, generation) == 32,
              "Audio bridge generation offset changed");
static_assert(offsetof(DmicAudioBridgeSharedHeader, status) == 40,
              "Audio bridge status offset changed");
#else
_Static_assert(sizeof(DmicAudioBridgeSharedHeader) == 64,
               "Audio bridge header ABI must remain 64 bytes");
_Static_assert(offsetof(DmicAudioBridgeSharedHeader, writeFrames) == 24,
               "Audio bridge write cursor offset changed");
_Static_assert(offsetof(DmicAudioBridgeSharedHeader, generation) == 32,
               "Audio bridge generation offset changed");
_Static_assert(offsetof(DmicAudioBridgeSharedHeader, status) == 40,
               "Audio bridge status offset changed");
#endif

#endif
