#ifndef AUDIOTOFACE_WAVREADER_H
#define AUDIOTOFACE_WAVREADER_H

// Audio2Face (#1019): decode a WAV file to float samples.
//
// Deliberately self-contained rather than routed through Qt Multimedia, even
// though the mocap feature already pulls that in. Two reasons:
//
//   * Qt Multimedia is gated behind ENABLE_MOCAP (OFF by default, and OFF on
//     Windows/MinGW), and lipsync should not inherit a webcam dependency to
//     read a file that is a header plus an array of samples;
//   * QAudioDecoder is asynchronous, so a CLI path would need an event loop
//     and a nested wait for what is fundamentally a synchronous read.
//
// RIFF/WAVE only, which covers what a user exports from any audio tool. The
// common encodings are handled — 8/16/24/32-bit PCM and 32/64-bit float — and
// anything else is refused BY NAME rather than reinterpreted, since decoding
// A-law as PCM produces loud noise rather than an obvious failure.
//
// Qt is used for file I/O only. No Ogre, no ONNX.

#include <QString>

#include <vector>

namespace AudioToFace {

struct WavData {
    std::vector<float> samples;   ///< interleaved, normalised to [-1, 1]
    int sampleRate = 0;
    int channels = 0;

    bool valid() const { return !samples.empty() && sampleRate > 0 && channels > 0; }
    double durationSec() const {
        return (sampleRate > 0 && channels > 0)
                   ? double(samples.size()) / double(sampleRate) / double(channels)
                   : 0.0;
    }
};

/// Read a RIFF/WAVE file. Returns an invalid result with `error` set on any
/// problem; never throws and never returns a partially-decoded buffer.
WavData readWav(const QString& path, QString* error = nullptr);

/// Decode a WAV already in memory — used by the tests and by callers that
/// have the bytes rather than a path.
WavData parseWav(const QByteArray& bytes, QString* error = nullptr);

}  // namespace AudioToFace

#endif  // AUDIOTOFACE_WAVREADER_H
