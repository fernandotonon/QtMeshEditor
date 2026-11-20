#include "FaceExpressionCalibration.h"

#include "FaceCapCanonicalData.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace FaceCalibration {
namespace {

enum class Base { None, Eye, Floor };

struct ChannelRule {
    const char* name;
    Base base;
    float gain;
};

// Channels not listed: no baseline, gain 1.
constexpr ChannelRule kRules[] = {
    // eyes: resting value = the take's median
    {"eyeBlinkLeft", Base::Eye, 1.0f},     {"eyeBlinkRight", Base::Eye, 1.0f},
    {"eyeSquintLeft", Base::Eye, 1.0f},    {"eyeSquintRight", Base::Eye, 1.0f},
    {"eyeLookDownLeft", Base::Eye, 1.0f},  {"eyeLookDownRight", Base::Eye, 1.0f},
    {"eyeLookUpLeft", Base::Eye, 1.0f},    {"eyeLookUpRight", Base::Eye, 1.0f},
    {"eyeLookInLeft", Base::Eye, 1.0f},    {"eyeLookInRight", Base::Eye, 1.0f},
    {"eyeLookOutLeft", Base::Eye, 1.0f},   {"eyeLookOutRight", Base::Eye, 1.0f},
    // floors the model leaves on a neutral face
    {"browDownLeft", Base::Floor, 1.3f},   {"browDownRight", Base::Floor, 1.3f},
    {"cheekSquintLeft", Base::Floor, 1.0f},{"cheekSquintRight", Base::Floor, 1.0f},
    {"mouthShrugLower", Base::Floor, 1.0f},{"mouthShrugUpper", Base::Floor, 1.0f},
    {"mouthPressLeft", Base::Floor, 1.0f}, {"mouthPressRight", Base::Floor, 1.0f},
    {"mouthRollLower", Base::Floor, 1.0f}, {"mouthRollUpper", Base::Floor, 1.0f},
    {"mouthClose", Base::Floor, 1.0f},
    // under-reported: gain only
    {"jawOpen", Base::None, 1.8f},
    {"jawLeft", Base::None, 1.3f},         {"jawRight", Base::None, 1.3f},
    {"browInnerUp", Base::None, 1.6f},
    {"browOuterUpLeft", Base::None, 1.6f}, {"browOuterUpRight", Base::None, 1.6f},
    {"eyeWideLeft", Base::None, 1.3f},     {"eyeWideRight", Base::None, 1.3f},
    {"mouthFunnel", Base::None, 1.5f},     {"mouthPucker", Base::None, 1.5f},
    {"mouthLowerDownLeft", Base::None, 1.4f}, {"mouthLowerDownRight", Base::None, 1.4f},
    {"mouthStretchLeft", Base::None, 1.3f},{"mouthStretchRight", Base::None, 1.3f},
    {"mouthFrownLeft", Base::None, 1.3f},  {"mouthFrownRight", Base::None, 1.3f},
};

const ChannelRule* ruleFor(int channel)
{
    if (channel < 0 || channel >= kChannels)
        return nullptr;
    const char* name = FaceCap::kBlendshapeNames[static_cast<size_t>(channel)];
    for (const ChannelRule& r : kRules)
        if (std::strcmp(r.name, name) == 0)
            return &r;
    return nullptr;
}

float percentile(std::vector<float> values, double p)
{
    if (values.empty())
        return 0.0f;
    const double q = std::clamp(p, 0.0, 1.0) * static_cast<double>(values.size() - 1);
    const auto k = static_cast<size_t>(std::floor(q));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

} // namespace

bool hasBaseline(int channel)
{
    const ChannelRule* r = ruleFor(channel);
    return r && r->base != Base::None;
}

float channelGain(int channel)
{
    const ChannelRule* r = ruleFor(channel);
    return r ? r->gain : 1.0f;
}

Report calibrate(std::vector<std::array<float, kChannels>>& weights,
                 const std::vector<bool>& confident,
                 const Options& options)
{
    Report report;
    report.gain.fill(1.0f);
    const size_t n = weights.size();
    auto isConfident = [&](size_t i) { return i < confident.size() && confident[i]; };

    std::vector<size_t> used;
    for (size_t i = 0; i < n; ++i)
        if (isConfident(i))
            used.push_back(i);
    report.framesUsed = static_cast<int>(used.size());

    report.baselineApplied = options.baseline
        && report.framesUsed >= std::max(2, options.minFramesForBaseline);
    for (int c = 0; c < kChannels; ++c) {
        const ChannelRule* r = ruleFor(c);
        if (report.baselineApplied && r && r->base != Base::None) {
            std::vector<float> values;
            values.reserve(used.size());
            for (size_t i : used)
                values.push_back(weights[i][static_cast<size_t>(c)]);
            const double p = r->base == Base::Eye ? options.eyePercentile : options.baselinePercentile;
            // never stretch by more than 5x: a channel that rests near 1 has no range left to recover
            report.baseline[static_cast<size_t>(c)] = std::min(percentile(std::move(values), p), 0.8f);
        }
        if (options.gains && r)
            report.gain[static_cast<size_t>(c)] = r->gain;
    }

    const float strength = static_cast<float>(std::clamp(options.strength, 0.0, 3.0));
    for (size_t i : used) {
        for (int c = 0; c < kChannels; ++c) {
            float& w = weights[i][static_cast<size_t>(c)];
            const float b = report.baseline[static_cast<size_t>(c)];
            float v = b > 0.0f ? (w - b) / (1.0f - b) : w;
            v *= report.gain[static_cast<size_t>(c)] * strength;
            w = std::clamp(v, 0.0f, 1.0f);
        }
    }
    return report;
}

} // namespace FaceCalibration
