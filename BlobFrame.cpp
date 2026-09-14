#include "BlobFrame.h"

#include <cstring>
#include <type_traits>

namespace ccordis {

static_assert(std::is_trivially_copyable<BlobFrameHeader>::value,
              "BlobFrameHeader must be trivially copyable");

// ── Portable big-endian conversion (kernel red line: no OS headers) ──

namespace {

inline bool hostIsBigEndian() {
    static const std::uint32_t one = 1;
    // NOLINTNEXTLINE
    return *reinterpret_cast<const std::uint8_t *>(&one) == 0;
}

inline std::uint16_t swap16(std::uint16_t v) {
    return static_cast<std::uint16_t>(((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8));
}
inline std::uint32_t swap32(std::uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8)
         | ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}
inline std::uint64_t swap64(std::uint64_t v) {
    return (static_cast<std::uint64_t>(swap32(static_cast<std::uint32_t>(v))) << 32)
         | static_cast<std::uint64_t>(swap32(static_cast<std::uint32_t>(v >> 32)));
}

inline std::uint16_t toBE16(std::uint16_t v) { return hostIsBigEndian() ? v : swap16(v); }
inline std::uint32_t toBE32(std::uint32_t v) { return hostIsBigEndian() ? v : swap32(v); }
inline std::uint64_t toBE64(std::uint64_t v) { return hostIsBigEndian() ? v : swap64(v); }
inline std::uint16_t fromBE16(std::uint16_t v) { return toBE16(v); }
inline std::uint32_t fromBE32(std::uint32_t v) { return toBE32(v); }
inline std::uint64_t fromBE64(std::uint64_t v) { return toBE64(v); }

// Float32 <-> big-endian uint32 bit-cast (IEEE-754 assumed).
inline std::uint32_t float32ToBE(float f) {
    std::uint32_t bits;
    static_assert(sizeof(f) == sizeof(bits));
    std::memcpy(&bits, &f, sizeof(bits));
    return toBE32(bits);
}
inline float float32FromBE(std::uint32_t be) {
    std::uint32_t bits = fromBE32(be);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

} // namespace

// ── Lifecycle helpers ───────────────────────────────────────────────────────

void makeHeader(BlobFrameHeader &h, FrameType type, std::uint8_t subType,
                std::uint32_t seq, std::uint64_t timestampNs) {
    h.magic[0] = 'B'; h.magic[1] = 'L'; h.magic[2] = 'O'; h.magic[3] = 'B';
    h.version  = 1;
    h.type     = static_cast<std::uint8_t>(type);
    h.subType  = subType;
    h.reserved_ = 0;
    h.seq      = toBE32(seq);
    h.payloadLen = 0;   // caller sets via setPayloadLen
    h.timestampNs = toBE64(timestampNs);
    std::memset(h.ext, 0, sizeof(h.ext));
}

void setPayloadLen(BlobFrameHeader &h, std::uint32_t len) {
    h.payloadLen = toBE32(len);
}

std::uint32_t getPayloadLen(const BlobFrameHeader &h) {
    return fromBE32(h.payloadLen);
}

// ── Spectrum / Threshold / Occupancy / Waterfall extension ────────────────
// ext layout: [0:4] startFreq (float32 BE, MHz)  [4:5] segment (u8)  [5:8] pad 0

void setStartFreq(BlobFrameHeader &h, float freqMHz) {
    std::uint32_t be = float32ToBE(freqMHz);
    std::memcpy(&h.ext[0], &be, 4);
}

float getStartFreq(const BlobFrameHeader &h) {
    std::uint32_t be = 0;
    std::memcpy(&be, &h.ext[0], 4);
    return float32FromBE(be);
}

void setSegment(BlobFrameHeader &h, std::uint8_t segment) {
    h.ext[4] = segment;
}

std::uint8_t getSegment(const BlobFrameHeader &h) {
    return h.ext[4];
}

// ── IQ extension ──────────────────────────────────────────────────────────
// ext layout: [0:4] centerFreq (float32 BE, MHz)  [4:8] sampleRate (float32 BE, Hz)

void setCenterFreq(BlobFrameHeader &h, float freqMHz) {
    setStartFreq(h, freqMHz);   // ext[0:4]
}

float getCenterFreq(const BlobFrameHeader &h) {
    return getStartFreq(h);     // ext[0:4]
}

void setSampleRate(BlobFrameHeader &h, float hz) {
    std::uint32_t be = float32ToBE(hz);
    std::memcpy(&h.ext[4], &be, 4);
}

float getSampleRate(const BlobFrameHeader &h) {
    std::uint32_t be = 0;
    std::memcpy(&be, &h.ext[4], 4);
    return float32FromBE(be);
}

} // namespace ccordis
