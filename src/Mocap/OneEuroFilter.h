#ifndef ONEEUROFILTER_H
#define ONEEUROFILTER_H

// One-Euro filter (Casiez, Roussel & Vogel, CHI 2012) for mocap channel
// smoothing (epic #869, Slice C #872). Pure data — no Qt/Ogre/ONNX — so the
// recorder, the live controller and the tests all share it headless.
//
// Defaults are tuned for 30 fps face capture: minCutoff 1.0 Hz keeps slow
// drift smooth, beta 0.05 lets fast expressions track without lag.

#include <array>

class OneEuroFilter {
public:
    struct Params {
        double minCutoff = 1.0;  // Hz — smoothing at rest (lower = smoother)
        double beta = 0.05;      // speed coefficient (higher = faster tracking)
        double dCutoff = 1.0;    // Hz — derivative smoothing
    };

    OneEuroFilter() : m_params() {}
    explicit OneEuroFilter(const Params& params) : m_params(params) {}

    // value at timeSec (monotonically increasing). The first sample passes
    // through unchanged.
    double filter(double value, double timeSec);

    void reset() { m_initialized = false; }
    const Params& params() const { return m_params; }

private:
    Params m_params;
    bool m_initialized = false;
    double m_lastTime = 0.0;
    double m_lastValue = 0.0;
    double m_lastDerivative = 0.0;
};

// Quaternion variant: hemisphere-aligns each sample against the previous
// output (dot >= 0) and slerps toward the new sample with a One-Euro-derived
// alpha where "speed" is the angular velocity between consecutive samples.
// Quaternions are (x, y, z, w), unit length.
class OneEuroQuatFilter {
public:
    OneEuroQuatFilter() : m_params() {}
    explicit OneEuroQuatFilter(const OneEuroFilter::Params& params)
        : m_params(params) {}

    std::array<float, 4> filter(const std::array<float, 4>& quat, double timeSec);

    void reset() { m_initialized = false; }

private:
    OneEuroFilter::Params m_params;
    bool m_initialized = false;
    double m_lastTime = 0.0;
    double m_lastSpeed = 0.0;  // rad/s, low-passed with dCutoff
    std::array<float, 4> m_lastQuat{0.f, 0.f, 0.f, 1.f};
};

#endif // ONEEUROFILTER_H
