#ifndef CCORDIS_BLOBFRAME_H
#define CCORDIS_BLOBFRAME_H

#include <ccordis/Blob.h>

#include <cstddef>
#include <cstdint>

namespace ccordis {

/**
 * @brief Data-plane on-wire frame header — 32-byte POD, design §9.3.
 *
 * Layout (offsets are contractual; do not change — extend via new type values):
 *
 *   0   4  magic      'BLOB' (0x42 0x4C 0x4F 0x42)
 *   4   1  version    frame format version (current = 1)
 *   5   1  type       frame type (see FrameType)
 *   6   1  subType    sub-class within type
 *   7   1  reserved   zero-filled
 *   8   4  seq        frame sequence (gap = dropped frames)
 *  12   4  payloadLen bytes of payload following this header
 *  16   8  timestampNs monotonic sample timestamp (ns)
 *  24   8  ext        type-specific extension (see ext layout per type)
 *
 * Multi-byte numeric fields are **big-endian (network byte order)** on the
 * wire; helpers below convert to/from host order. The 8-byte `ext` area is
 * reinterpreted per `type` (see the table in design §9.3).
 *
 * This struct is memcpy-safe (trivially copyable, packed to 32 bytes) and
 * carries no pointers — it can be stored in a Blob's prefix or sent over a
 * socket directly.
 */
struct BlobFrameHeader {
    std::uint8_t magic[4];     // 'B','L','O','B'
    std::uint8_t version;      // = 1
    std::uint8_t type;         // FrameType
    std::uint8_t subType;      // type-specific sub-class
    std::uint8_t reserved_;    // = 0
    std::uint32_t seq;         // big-endian on wire
    std::uint32_t payloadLen;  // big-endian on wire
    std::uint64_t timestampNs; // big-endian on wire
    std::uint8_t ext[8];       // type-specific, zero unused
};

static_assert(sizeof(BlobFrameHeader) == 32, "BlobFrameHeader must be 32 bytes");

/** Frame type enumeration (design §9.3 type table). */
enum class FrameType : std::uint8_t {
    Spectrum      = 0x01,  // ext: startFreq(f32 BE) + segment(u8) + 3B pad
    Threshold     = 0x02,  // ext: startFreq(f32 BE) + segment(u8) + 3B pad
    Occupancy     = 0x03,  // ext: startFreq(f32 BE) + segment(u8) + 3B pad
    IQ            = 0x04,  // ext: centerFreq(f32 BE) + sampleRate(f32 BE)
    DirectionFind = 0x05,  // ext: startFreq(f32 BE) + 4B reserved
    SignalDetect  = 0x06,  // ext: startFreq(f32 BE) + 4B reserved
    Waterfall     = 0x07,  // ext: startFreq(f32 BE) + segment(u8) + 3B pad
    UserDefined   = 0x0F,  // ext: custom
};

/** True when the header begins with the 'BLOB' magic. */
inline bool validMagic(const BlobFrameHeader &h) {
    return h.magic[0] == 'B' && h.magic[1] == 'L' && h.magic[2] == 'O'
        && h.magic[3] == 'B';
}

/** True when the header is well-formed (magic + known version). */
inline bool isValid(const BlobFrameHeader &h) {
    return validMagic(h) && h.version == 1;
}

// ── Construction / field access ─────────────────────────────────────────────

/** Fill common fields (magic='BLOB', version=1, zero ext). Payload length is
 *  set separately via setPayloadLen(). */
void makeHeader(BlobFrameHeader &h, FrameType type, std::uint8_t subType,
                std::uint32_t seq, std::uint64_t timestampNs);

void setPayloadLen(BlobFrameHeader &h, std::uint32_t len);
std::uint32_t getPayloadLen(const BlobFrameHeader &h);

// ── Spectrum / Threshold / Occupancy / Waterfall extension ────────────────
// ext layout: [0:4] startFreq (float32 BE, MHz)  [4:5] segment (u8)  [5:8] pad 0

void setStartFreq(BlobFrameHeader &h, float freqMHz);
float getStartFreq(const BlobFrameHeader &h);
void setSegment(BlobFrameHeader &h, std::uint8_t segment);
std::uint8_t getSegment(const BlobFrameHeader &h);

// ── IQ extension ──────────────────────────────────────────────────────────
// ext layout: [0:4] centerFreq (float32 BE, MHz)  [4:8] sampleRate (float32 BE, Hz)

void setCenterFreq(BlobFrameHeader &h, float freqMHz);
float getCenterFreq(const BlobFrameHeader &h);
void setSampleRate(BlobFrameHeader &h, float hz);
float getSampleRate(const BlobFrameHeader &h);

} // namespace ccordis

#endif // CCORDIS_BLOBFRAME_H
