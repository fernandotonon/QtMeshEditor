#ifndef FACEEXPRESSIONCALIBRATION_H
#define FACEEXPRESSIONCALIBRATION_H
// Per-take calibration of ARKit-52 face weights before they are keyed
// (performance capture, epic #869). Pure data: no Qt, no Ogre, no ONNX.
//
// The face blendshape model has two systematic errors that show directly on a
// character:
//  1. BIAS on some channels. A performer looking down at a phone reads as eyes
//     half closed (eyeBlink averaging ~0.5 and never below ~0.12 on a measured
//     take, eyeLookDown ~0.54, eyeSquint ~0.4), so the character looks sleepy
//     for the whole clip. The bias is per take (camera height, lighting, the
//     performer's face), so it is removed with the take's own baseline: a low
//     percentile of the channel, mapped to 0, the rest stretched back to 0..1.
//     Only channels the model is known to bias get a baseline; a smile held for
//     the whole take is real and stays.
//  2. UNDER-REPORTING on others: jawOpen, brows and funnel/pucker rarely pass
//     a third of their range even on a wide expression (jawOpen max 0.04 on the
//     same take). Those get a moderate gain.
// A global `strength` scales every channel after both steps (1 = as calibrated).
#include <array>
#include <vector>

namespace FaceCalibration {

constexpr int kChannels = 52;   // FaceCap::kBlendshapeNames order (index 0 = _neutral)

struct Options {
    bool baseline = true;          // subtract the per-take baseline on biased channels
    // Eyes (blink, squint, gaze) are open and centred most of a take while a
    // real blink lasts a few frames, so their resting value is the MEDIAN; the
    // other biased channels only carry a floor, taken at a low percentile.
    double eyePercentile = 0.5;
    double baselinePercentile = 0.10;
    int minFramesForBaseline = 8;  // fewer confident frames: no baseline (pose photos of unrelated poses)
    bool gains = true;             // per-channel gain on under-reported channels
    double strength = 1.0;         // global multiplier, clamped to [0, 3]
};

struct Report {
    int framesUsed = 0;            // confident frames that set the baselines
    bool baselineApplied = false;
    std::array<float, kChannels> baseline{};   // per channel (0 where none)
    std::array<float, kChannels> gain{};       // per channel (1 = unchanged)
};

// Does this channel get a per-take baseline? (true for the biased channels)
bool hasBaseline(int channel);
// The built-in gain of a channel (1 for most).
float channelGain(int channel);

// Calibrates `weights` in place. `confident[i]` marks the frames that carry a
// face (the others are left untouched and do not set baselines). Results are
// clamped to [0, 1].
Report calibrate(std::vector<std::array<float, kChannels>>& weights,
                 const std::vector<bool>& confident,
                 const Options& options = {});

} // namespace FaceCalibration

#endif // FACEEXPRESSIONCALIBRATION_H
