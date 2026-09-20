#ifndef AUDIOTOFACE_AUDIOPREPROCESS_H
#define AUDIOTOFACE_AUDIOPREPROCESS_H

// Audio2Face (#1019): get arbitrary user audio into the exact shape the
// network expects.
//
// The contract, from the shipped `network_info.json`:
//
//     samplerate  16000
//     buffer_len   8320 samples (0.52 s)
//     buffer_ofs   4160 samples
//
// The offset is the important part and is easy to misread. The window is
// CENTRED on the frame it predicts, not started at it — the network sees
// 0.26 s before and after. That is what lets it anticipate a plosive instead
// of reacting a frame late. Windowing from the frame time instead shifts the
// whole performance half a second late against the audio, which reads as bad
// lipsync rather than as a bug.
//
// Because the window is centred and each inference is independent (for the
// v2.3 regression model), frames can be sampled at any rate the caller wants.
// Frames near the ends need the audio padded rather than truncated, or the
// first and last half-second get a window that is partly garbage.
//
// Pure data — no Qt, no ONNX. Decoding a container (.wav/.ogg) is the
// caller's job; this takes samples and does the maths.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace AudioToFace {

/// Fixed by the network. Not options — changing them silently desynchronises
/// the prediction from the audio.
inline constexpr int kSampleRate = 16000;
inline constexpr int kWindowSamples = 8320;
inline constexpr int kWindowOffset = 4160;

/// Mix interleaved multi-channel audio down to mono by averaging.
///
/// Averaging rather than taking channel 0: a stereo recording with the voice
/// panned, or with one dead channel, would otherwise come out silent or half
/// amplitude. Returns the input unchanged when it is already mono.
std::vector<float> toMono(const std::vector<float>& interleaved, int channels);

/// Resample to 16 kHz with linear interpolation.
///
/// Linear is adequate here and deliberately chosen over a windowed-sinc
/// filter: the network's own first layers are a learned feature extractor
/// operating well below Nyquist, and speech content that matters for visemes
/// sits under 4 kHz. The artefacts linear introduces are far above that band.
/// Returns the input unchanged when it is already at the target rate.
std::vector<float> resampleTo16k(const std::vector<float>& mono, int sourceRate);

/// The centred window of `samples` for the frame at `centreSample`.
///
/// Always returns exactly kWindowSamples values, zero-padded where the window
/// runs off either end, so the first and last frames of a take are usable
/// rather than being fed truncated buffers.
std::vector<float> windowAt(const std::vector<float>& samples, int64_t centreSample);

/// Sample positions for `fps` frames per second across `sampleCount` samples.
///
/// Returns the CENTRE sample of each frame, which is what windowAt expects.
std::vector<int64_t> frameCentres(size_t sampleCount, double fps);

/// Peak-normalise to `target` (0 disables).
///
/// A quiet recording otherwise produces a correspondingly quiet face: the
/// network's response scales with input level, so a take recorded 20 dB down
/// barely opens the mouth. Silence is left alone rather than amplified into
/// noise.
std::vector<float> normalisePeak(const std::vector<float>& samples, float target = 0.95f);

}  // namespace AudioToFace

#endif  // AUDIOTOFACE_AUDIOPREPROCESS_H
