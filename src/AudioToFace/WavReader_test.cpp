// Audio2Face (#1019) WAV decode. Builds real WAV bytes rather than mocking,
// since the point is to parse the container correctly.
#include <gtest/gtest.h>

#include "AudioToFace/WavReader.h"

#include <QByteArray>

#include <cmath>
#include <cstring>
#include <vector>

using AudioToFace::parseWav;

namespace {

void put16(QByteArray& b, quint16 v) { b.append(char(v & 0xFF)); b.append(char(v >> 8)); }
void put32(QByteArray& b, quint32 v)
{
    for (int i = 0; i < 4; ++i) b.append(char((v >> (8*i)) & 0xFF));
}

/// A minimal but REAL wav: RIFF/WAVE, fmt, data.
QByteArray wav(quint16 format, quint16 channels, quint32 rate, quint16 bits,
               const QByteArray& data, const QByteArray& extraChunk = {})
{
    QByteArray fmt;
    put16(fmt, format); put16(fmt, channels); put32(fmt, rate);
    put32(fmt, rate * channels * bits / 8);          // byte rate
    put16(fmt, quint16(channels * bits / 8));        // block align
    put16(fmt, bits);

    QByteArray body("WAVE");
    body.append(extraChunk);                         // e.g. a LIST chunk first
    body.append("fmt "); put32(body, quint32(fmt.size())); body.append(fmt);
    body.append("data"); put32(body, quint32(data.size())); body.append(data);

    QByteArray out("RIFF");
    put32(out, quint32(body.size()));
    out.append(body);
    return out;
}

QByteArray pcm16(const std::vector<qint16>& v)
{
    QByteArray b;
    for (qint16 s : v) put16(b, quint16(s));
    return b;
}

}  // namespace

TEST(WavReader, Reads16BitPcmWithRateAndChannels)
{
    const auto bytes = wav(1, 1, 16000, 16, pcm16({0, 16384, -16384, 32767}));
    QString err;
    const auto w = parseWav(bytes, &err);
    ASSERT_TRUE(w.valid()) << err.toStdString();
    EXPECT_EQ(w.sampleRate, 16000);
    EXPECT_EQ(w.channels, 1);
    ASSERT_EQ(w.samples.size(), 4u);
    EXPECT_NEAR(w.samples[0], 0.0f, 1e-6);
    EXPECT_NEAR(w.samples[1], 0.5f, 1e-3);
    EXPECT_NEAR(w.samples[2], -0.5f, 1e-3);
    EXPECT_NEAR(w.samples[3], 1.0f, 1e-3);
    for (float s : w.samples) EXPECT_LE(std::abs(s), 1.0f);
}

// 8-bit WAV is UNSIGNED, unlike every other depth. Reading it as signed
// shifts the waveform by half full scale — it clips and sounds like heavy
// distortion rather than failing.
TEST(WavReader, EightBitIsTreatedAsUnsigned)
{
    QByteArray d;
    d.append(char(128));   // silence
    d.append(char(255));   // +full
    d.append(char(0));     // -full
    const auto w = parseWav(wav(1, 1, 8000, 8, d));
    ASSERT_TRUE(w.valid());
    ASSERT_EQ(w.samples.size(), 3u);
    EXPECT_NEAR(w.samples[0], 0.0f, 1e-3) << "128 is the zero point for 8-bit WAV";
    EXPECT_GT(w.samples[1], 0.9f);
    EXPECT_LT(w.samples[2], -0.9f);
}

TEST(WavReader, Reads24BitAnd32BitAndFloat)
{
    // 24-bit: little-endian, sign-extended from the top byte.
    QByteArray d24;
    d24.append(char(0x00)); d24.append(char(0x00)); d24.append(char(0x40));  // +0.5
    d24.append(char(0x00)); d24.append(char(0x00)); d24.append(char(0xC0));  // -0.5
    const auto w24 = parseWav(wav(1, 1, 44100, 24, d24));
    ASSERT_TRUE(w24.valid());
    ASSERT_EQ(w24.samples.size(), 2u);
    EXPECT_NEAR(w24.samples[0], 0.5f, 1e-3);
    EXPECT_NEAR(w24.samples[1], -0.5f, 1e-3);

    // 32-bit float passes through unscaled.
    QByteArray df;
    for (float v : {0.25f, -0.75f}) {
        char b[4];
        std::memcpy(b, &v, 4);
        df.append(b, 4);
    }
    const auto wf = parseWav(wav(3, 1, 48000, 32, df));
    ASSERT_TRUE(wf.valid());
    ASSERT_EQ(wf.samples.size(), 2u);
    EXPECT_NEAR(wf.samples[0], 0.25f, 1e-6);
    EXPECT_NEAR(wf.samples[1], -0.75f, 1e-6);
}

TEST(WavReader, StereoKeepsInterleavingAndReportsChannels)
{
    const auto w = parseWav(wav(1, 2, 16000, 16, pcm16({0, 32767, 0, -32768})));
    ASSERT_TRUE(w.valid());
    EXPECT_EQ(w.channels, 2);
    ASSERT_EQ(w.samples.size(), 4u);
    EXPECT_NEAR(w.samples[1], 1.0f, 1e-3) << "right channel of frame 0";
    // Duration counts FRAMES, not samples: 2 frames at 16 kHz.
    EXPECT_NEAR(w.durationSec(), 2.0 / 16000.0, 1e-9);
}

// Real files carry LIST/INFO chunks between fmt and data. Assuming a fixed
// 44-byte header reads that metadata as audio — an audible burst of noise at
// the start of the take.
TEST(WavReader, ChunksBeforeDataAreSkipped)
{
    // RIFF chunks are word-aligned: an ODD-length body is followed by one pad
    // byte that is NOT counted in the length field. Both cases are exercised,
    // because mishandling the pad puts every later chunk one byte out — which
    // is how a first draft of this test produced a file the parser (rightly)
    // rejected.
    auto listChunk = [](const QByteArray& body) {
        QByteArray c("LIST");
        put32(c, quint32(body.size()));
        c.append(body);
        if (body.size() & 1) c.append('\0');   // the alignment pad
        return c;
    };

    for (const QByteArray& body : {QByteArray("INFOISFT test padding"),   // 21, odd
                                   QByteArray("INFOISFT test padding!")}) {  // 22, even
        QString err;
        const auto w = parseWav(wav(1, 1, 16000, 16, pcm16({0, 32767}),
                                    listChunk(body)), &err);
        ASSERT_TRUE(w.valid())
            << "a LIST chunk of " << body.size() << " bytes before fmt/data must "
            << "not break the parse: " << err.toStdString();
        ASSERT_EQ(w.samples.size(), 2u);
        EXPECT_NEAR(w.samples[1], 1.0f, 1e-3)
            << "audio must come from the data chunk, not the LIST";
    }
}

// Decoding companded audio as PCM produces loud noise rather than an obvious
// failure, so the encoding is named and the user can re-export.
TEST(WavReader, UnsupportedEncodingIsRefusedByName)
{
    QString err;
    EXPECT_FALSE(parseWav(wav(6, 1, 8000, 8, QByteArray(16, '\0')), &err).valid());
    EXPECT_TRUE(err.contains(QStringLiteral("A-law"))) << err.toStdString();

    EXPECT_FALSE(parseWav(wav(7, 1, 8000, 8, QByteArray(16, '\0')), &err).valid());
    EXPECT_TRUE(err.contains(QStringLiteral("mu-law"))) << err.toStdString();
}

TEST(WavReader, MalformedInputIsReportedNotCrashed)
{
    QString err;
    EXPECT_FALSE(parseWav(QByteArray(), &err).valid());
    EXPECT_FALSE(err.isEmpty());

    EXPECT_FALSE(parseWav(QByteArray(100, 'x'), &err).valid());
    EXPECT_TRUE(err.contains(QStringLiteral("RIFF"))) << err.toStdString();

    // Valid header, no data chunk.
    QByteArray noData("RIFF");
    put32(noData, 4);
    noData.append("WAVE");
    EXPECT_FALSE(parseWav(noData, &err).valid());
}
