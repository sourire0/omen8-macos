#include "FirmwareImageHashKernel.hpp"

namespace sof::ipc4::kernel {
namespace {

constexpr uint32_t kRound[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

constexpr uint8_t kExpectedAdlIpc4V212Sha256[32] = {
    0x35, 0x13, 0x25, 0x78, 0x6e, 0x53, 0xca, 0x7c,
    0xd2, 0x3f, 0xfc, 0x14, 0x62, 0xd8, 0x29, 0x13,
    0xf3, 0xe0, 0xc0, 0x8a, 0xd7, 0x8f, 0x89, 0x05,
    0x3d, 0x85, 0xe4, 0xb6, 0x70, 0x3c, 0xf1, 0x2b,
};

uint32_t rotateRight(uint32_t value, uint32_t amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
}

uint32_t readBig32(const uint8_t *data) noexcept {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

void compress(uint32_t state[8], const uint8_t block[64]) noexcept {
    uint32_t words[64];
    for (size_t i = 0; i < 16; ++i)
        words[i] = readBig32(block + i * 4);
    for (size_t i = 16; i < 64; ++i) {
        const uint32_t s0 = rotateRight(words[i - 15], 7) ^
                            rotateRight(words[i - 15], 18) ^
                            (words[i - 15] >> 3);
        const uint32_t s1 = rotateRight(words[i - 2], 17) ^
                            rotateRight(words[i - 2], 19) ^
                            (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];
    for (size_t i = 0; i < 64; ++i) {
        const uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^
                              rotateRight(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t t1 = h + sum1 + choose + kRound[i] + words[i];
        const uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^
                              rotateRight(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

bool sha256(const uint8_t *data, size_t bytes, uint8_t digest[32]) noexcept {
    if (!data || !digest ||
        bytes > static_cast<size_t>(UINT64_MAX / UINT64_C(8)))
        return false;

    uint32_t state[8];
    state[0] = 0x6a09e667U;
    state[1] = 0xbb67ae85U;
    state[2] = 0x3c6ef372U;
    state[3] = 0xa54ff53aU;
    state[4] = 0x510e527fU;
    state[5] = 0x9b05688cU;
    state[6] = 0x1f83d9abU;
    state[7] = 0x5be0cd19U;

    size_t offset = 0;
    while (bytes - offset >= 64) {
        compress(state, data + offset);
        offset += 64;
    }

    uint8_t tail[128];
    // Keep SHA padding initialization explicit and bounded.
    volatile uint8_t *tailBytes = tail;
    for (size_t i = 0; i < sizeof(tail); ++i) tailBytes[i] = 0;
    const size_t remaining = bytes - offset;
    for (size_t i = 0; i < remaining; ++i) tail[i] = data[offset + i];
    tail[remaining] = 0x80;
    const size_t paddedBlocks = remaining < 56 ? 1 : 2;
    const uint64_t bitLength = static_cast<uint64_t>(bytes) * UINT64_C(8);
    const size_t lengthOffset = paddedBlocks * 64 - 8;
    for (size_t i = 0; i < 8; ++i)
        tail[lengthOffset + i] = static_cast<uint8_t>(
            bitLength >> (56 - static_cast<uint32_t>(i * 8)));
    compress(state, tail);
    if (paddedBlocks == 2) compress(state, tail + 64);

    for (size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<uint8_t>(state[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
    return true;
}

} // namespace

FirmwareImageHashResult verifyKnownAdlIpc4V212Image(
    const uint8_t *image, size_t imageBytes) noexcept {
    if (!image || imageBytes == 0) return FirmwareImageHashResult::InvalidArgument;
    uint8_t digest[32];
    if (!sha256(image, imageBytes, digest))
        return FirmwareImageHashResult::UnsupportedSize;
    uint8_t mismatch = 0;
    for (size_t i = 0; i < sizeof(digest); ++i)
        mismatch = static_cast<uint8_t>(mismatch |
                                        (digest[i] ^ kExpectedAdlIpc4V212Sha256[i]));
    return mismatch == 0 ? FirmwareImageHashResult::Ok
                         : FirmwareImageHashResult::WrongDigest;
}

} // namespace sof::ipc4::kernel
