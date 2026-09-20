#include "AudioPreprocess.h"

#include <algorithm>
#include <cmath>

namespace AudioToFace {

std::vector<float> toMono(const std::vector<float>& interleaved, int channels)
{
    if (channels <= 1) return interleaved;
    const size_t frames = interleaved.size() / size_t(channels);
    std::vector<float> out(frames, 0.0f);
    const float inv = 1.0f / float(channels);
    for (size_t f = 0; f < frames; ++f) {
        float s = 0.0f;
        for (int c = 0; c < channels; ++c) s += interleaved[f * size_t(channels) + size_t(c)];
        out[f] = s * inv;
    }
    return out;
}

std::vector<float> resampleTo16k(const std::vector<float>& mono, int sourceRate)
{
    if (sourceRate == kSampleRate || sourceRate <= 0 || mono.empty()) return mono;

    const double ratio = double(kSampleRate) / double(sourceRate);
    const size_t outLen = size_t(double(mono.size()) * ratio);
    if (outLen == 0) return {};

    std::vector<float> out(outLen, 0.0f);
    for (size_t i = 0; i < outLen; ++i) {
        const double src = double(i) / ratio;
        const size_t i0 = size_t(src);
        const size_t i1 = std::min(i0 + 1, mono.size() - 1);
        const double frac = src - double(i0);
        out[i] = float(double(mono[i0]) * (1.0 - frac) + double(mono[i1]) * frac);
    }
    return out;
}

std::vector<float> windowAt(const std::vector<float>& samples, int64_t centreSample)
{
    // Centred, not leading: the network is given kWindowOffset samples of
    // context BEFORE the frame it predicts. Starting the window at the frame
    // would shift the whole performance half a second late.
    std::vector<float> w(size_t(kWindowSamples), 0.0f);
    const int64_t start = centreSample - int64_t(kWindowOffset);
    const int64_t n = int64_t(samples.size());
    for (int i = 0; i < kWindowSamples; ++i) {
        const int64_t s = start + i;
        // Zero-pad rather than clamp-extend: repeating the first sample for
        // half a second reads to the feature extractor as a DC tone, which is
        // a signal the network never saw in training. Silence is in
        // distribution.
        if (s >= 0 && s < n) w[size_t(i)] = samples[size_t(s)];
    }
    return w;
}

std::vector<int64_t> frameCentres(size_t sampleCount, double fps)
{
    std::vector<int64_t> out;
    if (fps <= 0.0 || sampleCount == 0) return out;
    const double stride = double(kSampleRate) / fps;
    if (stride <= 0.0) return out;
    const size_t count = size_t(double(sampleCount) / stride) + 1;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const int64_t c = int64_t(double(i) * stride);
        if (c >= int64_t(sampleCount)) break;
        out.push_back(c);
    }
    return out;
}

std::vector<float> normalisePeak(const std::vector<float>& samples, float target)
{
    if (target <= 0.0f || samples.empty()) return samples;
    float peak = 0.0f;
    for (float s : samples) peak = std::max(peak, std::abs(s));
    // Leave silence alone: scaling it up turns the noise floor into audible
    // input and the face mumbles through what should be a pause.
    if (peak <= 1e-6f) return samples;
    const float g = target / peak;
    std::vector<float> out(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) out[i] = samples[i] * g;
    return out;
}

}  // namespace AudioToFace
