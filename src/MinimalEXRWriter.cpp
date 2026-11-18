#include "MinimalEXRWriter.h"

#include <QFile>
#include <QIODevice>
#include <cstring>

namespace MinimalEXR {

uint16_t floatToHalf(float value)
{
    uint32_t f;
    std::memcpy(&f, &value, sizeof(f));
    const uint32_t sign = (f >> 16) & 0x8000u;
    const uint32_t absf = f & 0x7fffffffu;
    if (absf >= 0x7f800000u) {                       // Inf / NaN
        return static_cast<uint16_t>(sign | 0x7c00u | (absf > 0x7f800000u ? 0x200u : 0u));
    }
    if (absf >= 0x477ff000u) {                       // rounds past 65504 -> Inf
        return static_cast<uint16_t>(sign | 0x7c00u);
    }
    if (absf < 0x38800000u) {                        // subnormal half (or zero)
        if (absf < 0x33000000u) return static_cast<uint16_t>(sign);  // < half of the smallest subnormal
        const uint32_t mant = (absf & 0x007fffffu) | 0x00800000u;
        const int shift = 126 - static_cast<int>(absf >> 23);       // 14..24
        uint32_t h = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (h & 1u))) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    // Normal: rebias exponent 127 -> 15, round mantissa 23 -> 10 bits (nearest even).
    uint32_t h = ((absf - 0x38000000u) >> 13);
    const uint32_t rem = absf & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
    return static_cast<uint16_t>(sign | h);
}

float halfToFloat(uint16_t h)
{
    const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1fu;
    uint32_t mant = h & 0x3ffu;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign;
        } else {                                      // subnormal -> normalise
            int e = -1;
            do { ++e; mant <<= 1; } while ((mant & 0x400u) == 0);
            f = sign | (static_cast<uint32_t>(127 - 15 - e) << 23) | ((mant & 0x3ffu) << 13);
        }
    } else if (exp == 31) {
        f = sign | 0x7f800000u | (mant << 13);
    } else {
        f = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, sizeof(out));
    return out;
}

namespace {

// EXR LE writers — the format is always little-endian regardless of host.
void writeU32LE(QByteArray& buf, uint32_t v) {
    char b[4];
    b[0] = static_cast<char>(v & 0xff);
    b[1] = static_cast<char>((v >> 8) & 0xff);
    b[2] = static_cast<char>((v >> 16) & 0xff);
    b[3] = static_cast<char>((v >> 24) & 0xff);
    buf.append(b, 4);
}
void writeI32LE(QByteArray& buf, int32_t v) { writeU32LE(buf, static_cast<uint32_t>(v)); }
void writeU64LE(QByteArray& buf, uint64_t v) {
    writeU32LE(buf, static_cast<uint32_t>(v & 0xffffffffu));
    writeU32LE(buf, static_cast<uint32_t>(v >> 32));
}
void writeF32LE(QByteArray& buf, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    writeU32LE(buf, bits);
}

// Append a NUL-terminated string. EXR uses C-strings for attr names + types.
//
// Bounded against a 256-byte cap so a malformed (un-terminated) caller
// can't walk off the end of the source buffer. All current call sites
// pass string literals at most ~16 chars long, so the cap is a defense-
// in-depth measure; if it ever fires the EXR output is structurally
// malformed and we let the file write succeed but report nothing —
// downstream `file` will fail to parse and the caller sees the error.
void writeCStr(QByteArray& buf, const char* s) {
    if (s == nullptr) return;
    // Bounded scan — portable equivalent of `strnlen` so the function
    // is safe even if a future caller forgets to NUL-terminate.
    constexpr size_t kMaxAttrLen = 256;
    size_t n = 0;
    while (n < kMaxAttrLen && s[n] != '\0') ++n;
    buf.append(s, static_cast<int>(n));
    buf.append('\0');
}

// Attribute header: name\0 type\0 size_u32 payload[size].
// `payload` is appended verbatim — caller has already serialized it LE.
void writeAttr(QByteArray& buf,
               const char* name,
               const char* type,
               const QByteArray& payload) {
    writeCStr(buf, name);
    writeCStr(buf, type);
    writeI32LE(buf, static_cast<int32_t>(payload.size()));
    buf.append(payload);
}

// Channel-list attribute: a sequence of channel records terminated by a NUL
// byte. Each record:
//   name\0  pixelType(u32)  pLinear(u8)  reserved[3]  xSampling(u32)  ySampling(u32)
// pixelType: 0=UINT 1=HALF 2=FLOAT.
//
// Order matters: EXR reads back the channels in the order they appear, and
// the convention for RGB is alphabetical (B, G, R). We write B then G then R
// to match what every EXR-reading tool expects to find.
// Channel list in EXR's required alphabetical order. 3 channels → B,G,R;
// 4 channels → A,B,G,R (alpha sorts first).
QByteArray buildChannelList(int channels, uint32_t pixelType) {
    QByteArray out;
    const char* names3[3] = { "B", "G", "R" };
    const char* names4[4] = { "A", "B", "G", "R" };
    const char** names = (channels == 4) ? names4 : names3;
    for (int ci = 0; ci < channels; ++ci) {
        const char* n = names[ci];
        writeCStr(out, n);
        writeU32LE(out, pixelType);   // 1 = HALF, 2 = FLOAT
        out.append('\0');      // pLinear = 0
        out.append('\0');      // reserved
        out.append('\0');
        out.append('\0');
        writeU32LE(out, 1u);   // xSampling
        writeU32LE(out, 1u);   // ySampling
    }
    out.append('\0');          // terminator
    return out;
}

QByteArray buildBox2i(int xMin, int yMin, int xMax, int yMax) {
    QByteArray out;
    writeI32LE(out, xMin);
    writeI32LE(out, yMin);
    writeI32LE(out, xMax);
    writeI32LE(out, yMax);
    return out;
}

QByteArray buildV2f(float a, float b) {
    QByteArray out;
    writeF32LE(out, a);
    writeF32LE(out, b);
    return out;
}

QByteArray buildF32(float v) {
    QByteArray out;
    writeF32LE(out, v);
    return out;
}

QByteArray buildU32(uint32_t v) {
    QByteArray out;
    writeU32LE(out, v);
    return out;
}

QByteArray buildU8(uint8_t v) {
    QByteArray out(1, static_cast<char>(v));
    return out;
}

} // namespace

namespace {

// Shared writer: `channels` is 3 (interleaved RGB) or 4 (interleaved
// RGBA). Scanlines are emitted channel-planar in the declared
// (alphabetical) channel order, as the EXR scanline format requires.
bool writeImpl(const QString& path,
               int width,
               int height,
               int channels,
               bool half,
               const std::vector<float>& rgbData)
{
    if (width <= 0 || height <= 0) return false;
    if (channels != 3 && channels != 4) return false;
    const size_t expected = static_cast<size_t>(width)
                          * static_cast<size_t>(height)
                          * static_cast<size_t>(channels);
    if (rgbData.size() != expected) return false;

    // ── Header ────────────────────────────────────────────────────────
    QByteArray header;
    // Magic + version.
    writeU32LE(header, 0x01312f76u);  // EXR magic
    writeU32LE(header, 2u);            // version 2, no special flags

    // Attributes — name\0 type\0 size payload, terminated by a single NUL.
    writeAttr(header, "channels",            "chlist",
              buildChannelList(channels, half ? 1u : 2u));
    writeAttr(header, "compression",         "compression",
              buildU8(0));                              // NO_COMPRESSION
    writeAttr(header, "dataWindow",          "box2i",
              buildBox2i(0, 0, width - 1, height - 1));
    writeAttr(header, "displayWindow",       "box2i",
              buildBox2i(0, 0, width - 1, height - 1));
    writeAttr(header, "lineOrder",           "lineOrder",
              buildU8(0));                              // INCREASING_Y
    writeAttr(header, "pixelAspectRatio",    "float",
              buildF32(1.0f));
    writeAttr(header, "screenWindowCenter",  "v2f",
              buildV2f(0.0f, 0.0f));
    writeAttr(header, "screenWindowWidth",   "float",
              buildF32(1.0f));

    // Attribute-list terminator.
    header.append('\0');

    // ── Scanline offset table ────────────────────────────────────────
    // One uint64 per scanline. The offset is the absolute file position
    // of that scanline's payload block (which starts with the y-coord
    // u32 then the data-size u32 then the pixel bytes).
    //
    // Each scanline payload = 4 (y) + 4 (size) + (width × 3 channels ×
    // 4 bytes/float) = 8 + width × 12.
    const uint64_t bytesPerSample = half ? 2u : 4u;
    const uint64_t scanlinePayloadSize = 8u
        + static_cast<uint64_t>(width) * static_cast<uint64_t>(channels) * bytesPerSample;
    const uint64_t offsetTableSize = static_cast<uint64_t>(height) * 8u;
    const uint64_t firstScanlineOffset =
        static_cast<uint64_t>(header.size()) + offsetTableSize;

    QByteArray offsetTable;
    offsetTable.reserve(static_cast<int>(offsetTableSize));
    for (int y = 0; y < height; ++y) {
        const uint64_t off = firstScanlineOffset
            + static_cast<uint64_t>(y) * scanlinePayloadSize;
        writeU64LE(offsetTable, off);
    }

    // ── Scanlines ────────────────────────────────────────────────────
    // Channels are written out in the order declared in the channel list
    // — i.e. B, G, R per scanline. Inside each channel block, the entire
    // row of pixels for THAT channel is written contiguously.
    QByteArray pixels;
    pixels.reserve(static_cast<int>(
        static_cast<uint64_t>(height) * scanlinePayloadSize));
    const uint32_t pixelDataSize =
        static_cast<uint32_t>(width) * static_cast<uint32_t>(channels)
        * static_cast<uint32_t>(bytesPerSample);
    auto writeSample = [&](float v) {
        if (half) {
            const uint16_t h = floatToHalf(v);
            const char b[2] = { static_cast<char>(h & 0xff), static_cast<char>(h >> 8) };
            pixels.append(b, 2);
        } else {
            writeF32LE(pixels, v);
        }
    };

    for (int y = 0; y < height; ++y) {
        writeI32LE(pixels, y);
        writeU32LE(pixels, pixelDataSize);
        // Row pointer into the caller's interleaved RGB(A) buffer.
        const size_t c = static_cast<size_t>(channels);
        const float* row = rgbData.data()
            + static_cast<size_t>(y) * static_cast<size_t>(width) * c;
        // A channel first (alphabetical order) when present.
        if (channels == 4)
            for (int x = 0; x < width; ++x) writeSample(row[x * c + 3]);
        // B channel.
        for (int x = 0; x < width; ++x) writeSample(row[x * c + 2]);
        // G channel.
        for (int x = 0; x < width; ++x) writeSample(row[x * c + 1]);
        // R channel.
        for (int x = 0; x < width; ++x) writeSample(row[x * c + 0]);
    }

    // ── Write to disk in one shot ────────────────────────────────────
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (out.write(header) != header.size()) return false;
    if (out.write(offsetTable) != offsetTable.size()) return false;
    if (out.write(pixels) != pixels.size()) return false;
    out.close();
    return out.error() == QFile::NoError;
}

} // namespace

bool writeRGB32F(const QString& path,
                 int width,
                 int height,
                 const std::vector<float>& rgbData)
{
    return writeImpl(path, width, height, 3, false, rgbData);
}

bool writeRGBA32F(const QString& path,
                  int width,
                  int height,
                  const std::vector<float>& rgbaData)
{
    return writeImpl(path, width, height, 4, false, rgbaData);
}

bool writeRGBA16F(const QString& path,
                  int width,
                  int height,
                  const std::vector<float>& rgbaData)
{
    return writeImpl(path, width, height, 4, true, rgbaData);
}

} // namespace MinimalEXR
