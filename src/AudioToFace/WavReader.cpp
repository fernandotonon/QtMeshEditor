#include "WavReader.h"

#include <QByteArray>
#include <QFile>

#include <cstring>

namespace AudioToFace {

namespace {

constexpr quint16 kFormatPcm        = 1;
constexpr quint16 kFormatFloat      = 3;
constexpr quint16 kFormatALaw       = 6;
constexpr quint16 kFormatMuLaw      = 7;
constexpr quint16 kFormatExtensible = 0xFFFE;

quint16 rd16(const char* p)
{
    return quint16(quint8(p[0])) | (quint16(quint8(p[1])) << 8);
}
quint32 rd32(const char* p)
{
    return quint32(quint8(p[0])) | (quint32(quint8(p[1])) << 8) |
           (quint32(quint8(p[2])) << 16) | (quint32(quint8(p[3])) << 24);
}

QString formatName(quint16 f)
{
    switch (f) {
        case kFormatPcm:        return QStringLiteral("PCM");
        case kFormatFloat:      return QStringLiteral("IEEE float");
        case kFormatALaw:       return QStringLiteral("A-law");
        case kFormatMuLaw:      return QStringLiteral("mu-law");
        case kFormatExtensible: return QStringLiteral("WAVE_FORMAT_EXTENSIBLE");
        default:                return QStringLiteral("format 0x%1").arg(f, 0, 16);
    }
}

}  // namespace

WavData parseWav(const QByteArray& bytes, QString* error)
{
    WavData out;
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return WavData{};
    };

    if (bytes.size() < 44)
        return fail(QStringLiteral("file is too small to be a WAV"));
    const char* d = bytes.constData();
    if (std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0)
        return fail(QStringLiteral("not a RIFF/WAVE file"));

    quint16 format = 0, channels = 0, bits = 0;
    quint32 rate = 0;
    qint64 dataOfs = -1;
    qint64 dataLen = 0;

    // Walk the chunk list rather than assuming fmt-then-data: real files carry
    // LIST/INFO and fact chunks between them, and a fixed 44-byte offset reads
    // metadata as audio (which is audible as a burst of noise at the start).
    // Offsets are computed in 64-bit and bounds-checked every step. A chunk
    // header may declare any quint32 length, and a length above INT_MAX made
    // `int(len)` negative, walked `p` BACKWARDS and read before the buffer --
    // reproduced as an ASan out-of-bounds read (a hard BUS crash) from a
    // 60-byte crafted file. Audio comes from wherever the user points us, so
    // a malformed header has to be rejected, not trusted.
    qint64 p = 12;
    while (p + 8 <= bytes.size()) {
        const char* id = d + p;
        const quint32 len = rd32(d + p + 4);
        const qint64 body = p + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= bytes.size()) {
            format   = rd16(d + body);
            channels = rd16(d + body + 2);
            rate     = rd32(d + body + 4);
            bits     = rd16(d + body + 14);
            // EXTENSIBLE carries the real format in its sub-format GUID; the
            // first two bytes match the classic tag.
            if (format == kFormatExtensible && len >= 40 && body + 26 <= bytes.size())
                format = rd16(d + body + 24);
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataOfs = body;
            dataLen = qMin<qint64>(len, bytes.size() - body);
        }
        // Chunks are word-aligned: an odd length is followed by a pad byte.
        const qint64 next = body + qint64(len) + qint64(len & 1);
        if (next <= p || next > bytes.size()) break;  // malformed or truncated
        p = next;
    }

    if (channels == 0 || rate == 0)
        return fail(QStringLiteral("WAV has no usable fmt chunk"));
    if (dataOfs < 0 || dataLen <= 0)
        return fail(QStringLiteral("WAV has no audio data"));
    if (format != kFormatPcm && format != kFormatFloat) {
        // Named explicitly: decoding companded audio as PCM yields loud noise
        // rather than an obvious failure, and the user can re-export.
        return fail(QStringLiteral("unsupported WAV encoding (%1); export as "
                                   "PCM or float WAV").arg(formatName(format)));
    }

    const char* s = d + dataOfs;
    const qint64 n = dataLen;
    if (format == kFormatFloat) {
        if (bits == 32) {
            const qint64 count = n / 4;
            out.samples.resize(size_t(count));
            for (qint64 i = 0; i < count; ++i) {
                float v;
                std::memcpy(&v, s + i * 4, 4);
                out.samples[size_t(i)] = v;
            }
        } else if (bits == 64) {
            const qint64 count = n / 8;
            out.samples.resize(size_t(count));
            for (qint64 i = 0; i < count; ++i) {
                double v;
                std::memcpy(&v, s + i * 8, 8);
                out.samples[size_t(i)] = float(v);
            }
        } else {
            return fail(QStringLiteral("unsupported float WAV depth (%1-bit)").arg(bits));
        }
    } else {
        switch (bits) {
            case 8: {
                // 8-bit WAV is UNSIGNED, unlike every other depth. Reading it
                // as signed shifts the waveform by half full scale, which
                // clips and sounds like heavy distortion.
                out.samples.resize(size_t(n));
                for (qint64 i = 0; i < n; ++i)
                    out.samples[size_t(i)] = (float(quint8(s[i])) - 128.0f) / 128.0f;
                break;
            }
            case 16: {
                const qint64 count = n / 2;
                out.samples.resize(size_t(count));
                for (qint64 i = 0; i < count; ++i)
                    out.samples[size_t(i)] = float(qint16(rd16(s + i * 2))) / 32768.0f;
                break;
            }
            case 24: {
                const qint64 count = n / 3;
                out.samples.resize(size_t(count));
                for (qint64 i = 0; i < count; ++i) {
                    const char* q = s + i * 3;
                    qint32 v = (qint32(quint8(q[0]))) | (qint32(quint8(q[1])) << 8) |
                               (qint32(qint8(q[2])) << 16);
                    out.samples[size_t(i)] = float(v) / 8388608.0f;
                }
                break;
            }
            case 32: {
                const qint64 count = n / 4;
                out.samples.resize(size_t(count));
                for (qint64 i = 0; i < count; ++i)
                    out.samples[size_t(i)] = float(qint32(rd32(s + i * 4))) / 2147483648.0f;
                break;
            }
            default:
                return fail(QStringLiteral("unsupported PCM depth (%1-bit)").arg(bits));
        }
    }

    if (out.samples.empty())
        return fail(QStringLiteral("WAV decoded to no samples"));
    out.sampleRate = int(rate);
    out.channels = int(channels);
    return out;
}

WavData readWav(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open %1").arg(path);
        return {};
    }
    return parseWav(f.readAll(), error);
}

}  // namespace AudioToFace
