#include "LidClose.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace CharacterGen {
namespace {
using V3 = std::array<double, 3>;
constexpr double kCmToM = 0.01;
constexpr double kOvershoot = 1.08;        // turn a little past the lower margin so the lids overlap
constexpr double kReachAbove = 0.38;       // radians above the margin where the turn has faded out
constexpr double kReachAround = 0.027;     // metres around the eyeball centre that can move
constexpr double kReachBehind = 0.020;     // metres behind its centre plane (the lid wraps the side of the eyeball at the canthi)
constexpr double kCantusFade = 0.011;      // metres beyond the canthi over which the turn fades

double smootherstep(double t)
{
    t = std::min(1.0, std::max(0.0, t));
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

V3 positionOf(const std::vector<float>& positions, uint32_t v)
{
    const size_t i = size_t(v) * 3;
    return {double(positions[i]), double(positions[i + 1]), double(positions[i + 2])};
}

double phiOf(const V3& centre, const V3& q) { return std::atan2(q[1] - centre[1], q[2] - centre[2]); }

// The lower margin: its angle about the eyeball axis as a function of x, and the 3D polyline.
class LowerMargin {
public:
    LowerMargin(const std::vector<float>& positions, const std::vector<uint32_t>& vertices, const V3& centre)
    {
        for (uint32_t v : vertices) {
            const V3 q = positionOf(positions, v);
            m_phi.push_back({q[0], phiOf(centre, q)});
            m_line.push_back(q);
        }
        std::sort(m_phi.begin(), m_phi.end());
    }

    double phiAt(double x) const
    {
        if (x <= m_phi.front().first) return m_phi.front().second;
        for (size_t k = 0; k + 1 < m_phi.size(); ++k) {
            if (x > m_phi[k + 1].first) continue;
            const double span = m_phi[k + 1].first - m_phi[k].first;
            const double t = span > 1e-12 ? (x - m_phi[k].first) / span : 0.0;
            return m_phi[k].second + (m_phi[k + 1].second - m_phi[k].second) * t;
        }
        return m_phi.back().second;
    }

    V3 nearest(const V3& q) const
    {
        V3 best = m_line.front();
        double bestD = 1e30;
        for (size_t k = 0; k + 1 < m_line.size(); ++k) {
            const V3 c = closestOnSegment(m_line[k], m_line[k + 1], q);
            const double d2 = (c[0] - q[0]) * (c[0] - q[0]) + (c[1] - q[1]) * (c[1] - q[1]) + (c[2] - q[2]) * (c[2] - q[2]);
            if (d2 >= bestD) continue;
            bestD = d2;
            best = c;
        }
        return best;
    }

private:
    static V3 closestOnSegment(const V3& a, const V3& b, const V3& q)
    {
        const V3 e{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const double len2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
        double t = 0.0;
        if (len2 > 1e-18) t = std::min(1.0, std::max(0.0, ((q[0] - a[0]) * e[0] + (q[1] - a[1]) * e[1] + (q[2] - a[2]) * e[2]) / len2));
        return {a[0] + e[0] * t, a[1] + e[1] * t, a[2] + e[2] * t};
    }

    std::vector<std::pair<double, double>> m_phi;   // (x, phi), x ascending
    std::vector<V3> m_line;
};

struct Knot {
    double x = 0.0;
    double phi = 0.0;
    double delta = 0.0;   // how far this upper-margin vertex has to turn (<= 0: downwards)
    uint32_t vertex = 0;  // the body vertex this knot is
    V3 p{};
    V3 res{};             // what the turn leaves, snapped onto the lower margin
};

struct Sample {
    double phi = 0.0;
    double delta = 0.0;
    V3 res{};
};

// The upper margin as knots along x.
class UpperMargin {
public:
    UpperMargin(const std::vector<float>& positions, const std::vector<uint32_t>& vertices, const LowerMargin& lower, const V3& centre)
    {
        for (uint32_t v : vertices) {
            Knot kn;
            kn.vertex = v;
            kn.p = positionOf(positions, v);
            kn.x = kn.p[0];
            kn.phi = phiOf(centre, kn.p);
            kn.delta = std::min(0.0, (lower.phiAt(kn.x) - kn.phi) * kOvershoot);
            m_knots.push_back(kn);
        }
        std::sort(m_knots.begin(), m_knots.end(), [](const Knot& a, const Knot& b) { return a.x < b.x; });
        for (Knot& kn : m_knots) kn.res = remainder(kn, lower, centre);
    }

    const std::vector<Knot>& knots() const { return m_knots; }
    double xMin() const { return m_knots.front().x; }
    double xMax() const { return m_knots.back().x; }

    Sample at(double x) const
    {
        if (x <= m_knots.front().x) return blend(m_knots.front(), m_knots.front(), 0.0);
        for (size_t k = 0; k + 1 < m_knots.size(); ++k) {
            if (x > m_knots[k + 1].x) continue;
            const double span = m_knots[k + 1].x - m_knots[k].x;
            return blend(m_knots[k], m_knots[k + 1], span > 1e-12 ? (x - m_knots[k].x) / span : 0.0);
        }
        return blend(m_knots.back(), m_knots.back(), 0.0);
    }

private:
    static V3 remainder(const Knot& kn, const LowerMargin& lower, const V3& centre)
    {
        const double dy = kn.p[1] - centre[1];
        const double dz = kn.p[2] - centre[2];
        const double rho = std::sqrt(dy * dy + dz * dz);
        const double phi = std::atan2(dy, dz) + kn.delta;
        const V3 turned{kn.p[0], centre[1] + rho * std::sin(phi), centre[2] + rho * std::cos(phi)};
        const V3 target = lower.nearest(turned);
        return {target[0] - turned[0], target[1] - turned[1], target[2] - turned[2]};
    }

    static Sample blend(const Knot& a, const Knot& b, double t)
    {
        Sample s;
        s.phi = a.phi + (b.phi - a.phi) * t;
        s.delta = a.delta + (b.delta - a.delta) * t;
        for (size_t c = 0; c < 3; ++c) s.res[c] = a.res[c] + (b.res[c] - a.res[c]) * t;
        return s;
    }

    std::vector<Knot> m_knots;
};

// How far vertex q moves when the lid closes (metres); false when it is not part of this eye's upper lid.
bool lidMove(const V3& q, const V3& centre, const UpperMargin& upper, V3* move)
{
    const double dx = q[0] - centre[0];
    const double dy = q[1] - centre[1];
    const double dz = q[2] - centre[2];
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > kReachAround || dz < -kReachBehind) return false;
    const double outside = q[0] < upper.xMin() ? upper.xMin() - q[0] : std::max(0.0, q[0] - upper.xMax());
    const double wx = 1.0 - smootherstep(outside / kCantusFade);
    const Sample s = upper.at(q[0]);
    const double above = phiOf(centre, q) - s.phi;
    if (wx <= 0.0 || above < -0.04) return false;   // beyond the canthi; the lower lid and the cheek do not move
    const double w = wx * (1.0 - smootherstep(above / kReachAbove));
    if (w <= 0.0) return false;
    const double rho = std::sqrt(dy * dy + dz * dz);
    const double phi = std::atan2(dy, dz) + w * s.delta;
    (*move)[0] = w * s.res[0];
    (*move)[1] = rho * std::sin(phi) - dy + w * s.res[1];
    (*move)[2] = rho * std::cos(phi) - dz + w * s.res[2];
    return true;
}

// Fill `blink` (V*3, cm) with the closing motion of one eye.
void closeEye(const std::vector<float>& positions, int vertexCount, bool leftEye, const V3& centre, const LidMargins& margins, float* blink)
{
    const LowerMargin lower(positions, margins.lower, centre);
    const UpperMargin upper(positions, margins.upper, lower, centre);
    double largest = 0.0;
    for (int v = 0; v < vertexCount; ++v) {
        const V3 q = positionOf(positions, uint32_t(v));
        if (leftEye != (q[0] > 0.0)) continue;   // this eye's side of the face
        V3 move{};
        if (!lidMove(q, centre, upper, &move)) continue;
        largest = std::max(largest, std::sqrt(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]));
        for (size_t c = 0; c < 3; ++c) blink[size_t(v) * 3 + c] = float(move[c] / kCmToM);
    }
    // The margin vertices themselves always land exactly on the lower margin, whatever the interpolation along x made of a zigzagging
    // margin (two margin vertices at nearly the same x).
    for (const Knot& kn : upper.knots()) {
        const double dy = kn.p[1] - centre[1];
        const double dz = kn.p[2] - centre[2];
        const double rho = std::sqrt(dy * dy + dz * dz);
        const double phi = std::atan2(dy, dz) + kn.delta;
        const V3 move{kn.res[0], rho * std::sin(phi) - dy + kn.res[1], rho * std::cos(phi) - dz + kn.res[2]};
        for (size_t c = 0; c < 3; ++c) blink[size_t(kn.vertex) * 3 + c] = float(move[c] / kCmToM);
    }
    if (std::getenv("QTMESH_LID_DEBUG")) std::fprintf(stderr, "lids: %s eye, largest lid move %.2f mm\n", leftEye ? "left" : "right", largest * 1000.0);
}
}  // namespace

std::vector<float> relaxLids(std::vector<float>& positions, int vertexCount, const std::vector<int32_t>& faces,
                             const std::vector<uint8_t>& dropped, const std::array<LashEye, 2>& eyes, const LidChannels& channels,
                             const std::vector<float>& arkit)
{
    std::vector<float> tuned = arkit;
    const size_t stride = size_t(vertexCount) * 3;
    // 1. the new rest pose: the old open pose with the old blink applied
    for (size_t side = 0; side < 2; ++side) {
        const float* d = arkit.data() + size_t(channels.blink[side]) * stride;
        for (size_t i = 0; i < stride; ++i) positions[i] += d[i] * float(kCmToM);
    }
    const std::array<LidMargins, 2> margins = findLidMargins(positions, faces, dropped, eyes);
    for (size_t side = 0; side < 2; ++side) {
        const float* d = arkit.data() + size_t(channels.blink[side]) * stride;
        float* blink = tuned.data() + size_t(channels.blink[side]) * stride;
        if (channels.wide[side] >= 0) {
            float* wide = tuned.data() + size_t(channels.wide[side]) * stride;
            for (size_t i = 0; i < stride; ++i) wide[i] -= d[i];   // wide still reaches the old open pose (+ its own delta)
        }
        std::fill(blink, blink + stride, 0.0f);                    // rebuilt below: the rest of the way, from the new rest pose
        if (!eyes[side].seated || !margins[side].valid()) continue;
        const V3 centre{double(eyes[side].centre[0]), double(eyes[side].centre[1]), double(eyes[side].centre[2])};
        closeEye(positions, vertexCount, side == 0, centre, margins[side], blink);
    }
    return tuned;
}

}  // namespace CharacterGen
