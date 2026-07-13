#include "OneEuroFilter.h"

#include <cmath>

namespace {

double smoothingAlpha(double cutoffHz, double dt)
{
    const double tau = 1.0 / (2.0 * M_PI * cutoffHz);
    return 1.0 / (1.0 + tau / dt);
}

}  // namespace

double OneEuroFilter::filter(double value, double timeSec)
{
    if (!m_initialized) {
        m_initialized = true;
        m_lastTime = timeSec;
        m_lastValue = value;
        m_lastDerivative = 0.0;
        return value;
    }
    double dt = timeSec - m_lastTime;
    if (dt <= 0.0)
        return m_lastValue;
    m_lastTime = timeSec;

    const double rawDerivative = (value - m_lastValue) / dt;
    const double aD = smoothingAlpha(m_params.dCutoff, dt);
    m_lastDerivative = aD * rawDerivative + (1.0 - aD) * m_lastDerivative;

    const double cutoff = m_params.minCutoff
                          + m_params.beta * std::abs(m_lastDerivative);
    const double a = smoothingAlpha(cutoff, dt);
    m_lastValue = a * value + (1.0 - a) * m_lastValue;
    return m_lastValue;
}

std::array<float, 4> OneEuroQuatFilter::filter(const std::array<float, 4>& quat,
                                               double timeSec)
{
    // hemisphere alignment: q and -q are the same rotation; keep the
    // representation continuous against the previous OUTPUT.
    std::array<float, 4> q = quat;
    if (m_initialized) {
        const double dot = q[0] * m_lastQuat[0] + q[1] * m_lastQuat[1]
                           + q[2] * m_lastQuat[2] + q[3] * m_lastQuat[3];
        if (dot < 0.0)
            for (auto& c : q) c = -c;
    }

    if (!m_initialized) {
        m_initialized = true;
        m_lastTime = timeSec;
        m_lastSpeed = 0.0;
        m_lastQuat = q;
        return q;
    }
    double dt = timeSec - m_lastTime;
    if (dt <= 0.0)
        return m_lastQuat;
    m_lastTime = timeSec;

    // angular distance previous-output -> sample
    double dot = q[0] * m_lastQuat[0] + q[1] * m_lastQuat[1]
                 + q[2] * m_lastQuat[2] + q[3] * m_lastQuat[3];
    dot = std::fmin(1.0, std::fmax(-1.0, dot));
    const double angle = 2.0 * std::acos(std::abs(dot));

    const double aD = smoothingAlpha(m_params.dCutoff, dt);
    m_lastSpeed = aD * (angle / dt) + (1.0 - aD) * m_lastSpeed;

    const double cutoff = m_params.minCutoff + m_params.beta * m_lastSpeed;
    const double a = smoothingAlpha(cutoff, dt);

    // slerp(m_lastQuat, q, a)
    std::array<float, 4> out;
    if (angle < 1e-6) {
        out = q;
    } else {
        const double theta = std::acos(std::abs(dot));
        const double s = std::sin(theta);
        const double w0 = std::sin((1.0 - a) * theta) / s;
        const double w1 = std::sin(a * theta) / s;
        for (int i = 0; i < 4; ++i)
            out[i] = static_cast<float>(w0 * m_lastQuat[i] + w1 * q[i]);
        double n = std::sqrt(out[0] * out[0] + out[1] * out[1]
                             + out[2] * out[2] + out[3] * out[3]);
        if (n > 0.0)
            for (auto& c : out) c = static_cast<float>(c / n);
    }
    m_lastQuat = out;
    return out;
}
