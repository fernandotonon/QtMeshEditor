#include "MinimalEXRWriter.h"

#include <QFile>
#include <QIODevice>
#include <cstring>

namespace MinimalEXR {

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
void writeCStr(QByteArray& buf, const char* s) {
    const size_t n = std::strlen(s);
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
QByteArray buildChannelListBGR_F32() {
    QByteArray out;
    const char* names[3] = { "B", "G", "R" };
    for (const char* n : names) {
        writeCStr(out, n);
        writeU32LE(out, 2u);   // FLOAT
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

bool writeRGB32F(const QString& path,
                 int width,
                 int height,
                 const std::vector<float>& rgbData)
{
    if (width <= 0 || height <= 0) return false;
    const size_t expected = static_cast<size_t>(width)
                          * static_cast<size_t>(height) * 3u;
    if (rgbData.size() != expected) return false;

    // ── Header ────────────────────────────────────────────────────────
    QByteArray header;
    // Magic + version.
    writeU32LE(header, 0x01312f76u);  // EXR magic
    writeU32LE(header, 2u);            // version 2, no special flags

    // Attributes — name\0 type\0 size payload, terminated by a single NUL.
    writeAttr(header, "channels",            "chlist",
              buildChannelListBGR_F32());
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
    const uint64_t scanlinePayloadSize = 8u
        + static_cast<uint64_t>(width) * 3u * 4u;
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
        static_cast<uint32_t>(width) * 3u * 4u;

    for (int y = 0; y < height; ++y) {
        writeI32LE(pixels, y);
        writeU32LE(pixels, pixelDataSize);
        // Row pointer into the caller's interleaved RGB buffer.
        const float* row = rgbData.data()
            + static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
        // B channel.
        for (int x = 0; x < width; ++x) writeF32LE(pixels, row[x * 3 + 2]);
        // G channel.
        for (int x = 0; x < width; ++x) writeF32LE(pixels, row[x * 3 + 1]);
        // R channel.
        for (int x = 0; x < width; ++x) writeF32LE(pixels, row[x * 3 + 0]);
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

} // namespace MinimalEXR
