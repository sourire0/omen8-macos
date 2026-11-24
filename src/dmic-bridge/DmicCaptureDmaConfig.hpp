#pragma once

#include <stdint.h>

namespace sof::ipc4::dmic::kernel {

// Candidate HDA capture geometry for the audited DMIC graph. Its 384-byte
// period is one 48-frame (1 ms) block at 48 kHz, stereo S32, matching the
// graph's selected SOF module IB/OB quantum. This is an implementation choice,
// not a hardware observation; validate it before enabling HDA RUN.
constexpr uint32_t kDmicCaptureCandidatePeriodBytes = 384U;
// 64 x 1 ms IOC blocks absorb short scheduler/workloop delays while keeping
// the hardware's one-millisecond module quantum unchanged.
constexpr uint32_t kDmicCaptureCandidatePeriodCount = 192U;
constexpr uint32_t kDmicCaptureCandidateRingBytes =
    kDmicCaptureCandidatePeriodBytes * kDmicCaptureCandidatePeriodCount;
constexpr uint32_t kDmicCaptureCandidateMaximumQueuedBytes =
    kDmicCaptureCandidateRingBytes -
    2U * kDmicCaptureCandidatePeriodBytes;
// HDA SDn.FMT: 48 kHz base rate, 32 bits/sample, two channels.
constexpr uint16_t kDmicCaptureCandidateHdaFormatWord = 0x0041U;

static_assert(kDmicCaptureCandidatePeriodBytes % 8U == 0U,
              "S32 stereo periods must contain complete frames");
static_assert(kDmicCaptureCandidateRingBytes %
                  kDmicCaptureCandidatePeriodBytes == 0U,
              "capture ring must contain whole IOC periods");
static_assert((kDmicCaptureCandidateHdaFormatWord & 0x0fU) == 1U,
              "HDA channel count field must encode stereo");
static_assert(((kDmicCaptureCandidateHdaFormatWord >> 4) & 0x7U) == 4U,
              "HDA sample-size field must encode 32-bit samples");
static_assert((kDmicCaptureCandidateHdaFormatWord & 0xf800U) == 0,
              "48 kHz uses the HDA 48 kHz base rate without multiplier");

} // namespace sof::ipc4::dmic::kernel
