#include "PoseIKSolver.h"

#include <cmath>

namespace PoseIK {

namespace {

// MediaPipe pose landmark indices
enum Lm : int {
    Nose = 0, LEar = 7, REar = 8,
    LShoulderLm = 11, RShoulderLm = 12, LElbowLm = 13, RElbowLm = 14,
    LWrist = 15, RWrist = 16,
    LPinky = 17, RPinky = 18, LIndex = 19, RIndex = 20,
    LThumb = 21, RThumb = 22,
    LHipLm = 23, RHipLm = 24, LKneeLm = 25, RKneeLm = 26,
    LAnkle = 27, RAnkle = 28, LFootIndex = 31, RFootIndex = 32,
};

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;

constexpr Quat kIdentity{0.f, 0.f, 0.f, 1.f};

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
Vec3 mul(const Vec3& a, float s) { return {a[0]*s, a[1]*s, a[2]*s}; }
float dot(const Vec3& a, const Vec3& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
bool normalize(Vec3& a)
{
    const float n = length(a);
    if (n < 1e-6f) return false;
    a = mul(a, 1.f / n);
    return true;
}

// column-major orthonormal basis (x, y, z) -> quaternion (x,y,z,w)
Quat quatFromBasis(const Vec3& x, const Vec3& y, const Vec3& z)
{
    const float m00 = x[0], m01 = y[0], m02 = z[0];
    const float m10 = x[1], m11 = y[1], m12 = z[1];
    const float m20 = x[2], m21 = y[2], m22 = z[2];
    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.f) {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s / 4.f};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q = {s / 4.f, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q = {(m01 + m10) / s, s / 4.f, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q = {(m02 + m20) / s, (m12 + m21) / s, s / 4.f, (m10 - m01) / s};
    }
    float n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n > 0.f)
        for (auto& c : q) c /= n;
    return q;
}

Quat nlerp(const Quat& a, const Quat& b, float t)
{
    float dotq = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    const float sign = dotq >= 0.f ? 1.f : -1.f;
    Quat out;
    for (int i = 0; i < 4; ++i)
        out[i] = a[i] * (1.f - t) + sign * b[i] * t;
    float n = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2] + out[3]*out[3]);
    if (n < 1e-6f) return a;
    for (auto& c : out) c /= n;
    return out;
}

// primary axis y + rough secondary -> full basis quat; false when degenerate
bool basisFromPrimary(Vec3 y, Vec3 secondary, Quat& out, Vec3* usedSecondary)
{
    if (!normalize(y))
        return false;
    Vec3 x = sub(secondary, mul(y, dot(secondary, y)));
    if (!normalize(x))
        return false;
    const Vec3 z = cross(x, y);
    out = quatFromBasis(x, y, z);
    if (usedSecondary)
        *usedSecondary = x;
    return true;
}

}  // namespace

void Solver::canonicalizeMediaPipeWorld(
    const float* world33x3,
    std::array<std::array<float, 3>, kLandmarkCount>& out)
{
    for (int i = 0; i < kLandmarkCount; ++i)
        out[static_cast<size_t>(i)] = {
            world33x3[i * 3 + 0], -world33x3[i * 3 + 1], world33x3[i * 3 + 2]};
}

bool Solver::limbSegmentDirection(
    int role, const std::array<std::array<float, 3>, kLandmarkCount>& p,
    const float* visibility, float minVisibility, std::array<float, 3>& outDir)
{
    struct Segment {
        Role r;
        int from, to;
    };
    static const Segment segments[] = {
        {RShoulder, 12, 14}, {RElbow, 14, 16},
        {LShoulder, 11, 13}, {LElbow, 13, 15},
        {RHip, 24, 26}, {RKnee, 26, 28}, {RFoot, 28, 32},
        {LHip, 23, 25}, {LKnee, 25, 27}, {LFoot, 27, 31},
    };
    auto visible = [&](int lm) {
        return !visibility || visibility[lm] >= minVisibility;
    };
    for (const Segment& seg : segments) {
        if (static_cast<int>(seg.r) != role)
            continue;
        if (!visible(seg.from) || !visible(seg.to))
            return false;
        const Vec3& a = p[static_cast<size_t>(seg.from)];
        const Vec3& b = p[static_cast<size_t>(seg.to)];
        Vec3 dir = sub(b, a);
        if (!normalize(dir))
            return false;
        outDir = dir;
        return true;
    }
    // Foot-index landmarks are often occluded — aim the foot bone along the shin.
    if (role == RFoot && visible(26) && visible(28)) {
        Vec3 dir = sub(p[28], p[26]);
        if (normalize(dir)) {
            outDir = dir;
            return true;
        }
    }
    if (role == LFoot && visible(25) && visible(27)) {
        Vec3 dir = sub(p[27], p[25]);
        if (normalize(dir)) {
            outDir = dir;
            return true;
        }
    }
    return false;
}

bool Solver::canonicalLiveDirection(
    int role, const std::array<std::array<float, 3>, kLandmarkCount>& p,
    const float* visibility, float minVisibility,
    std::array<float, 3>& outDir)
{
    if (limbSegmentDirection(role, p, visibility, minVisibility, outDir))
        return true;

    auto visible = [&](int lm) {
        return !visibility || visibility[lm] >= minVisibility;
    };
    auto mid = [&](int a, int b) -> Vec3 {
        return mul(add(p[static_cast<size_t>(a)], p[static_cast<size_t>(b)]), 0.5f);
    };

    switch (role) {
    case Hip:
    case Abdomen:
    case Chest:
        if (!visible(LShoulderLm) || !visible(RShoulderLm) || !visible(LHipLm)
            || !visible(RHipLm))
            return false;
        {
            const Vec3 hipMid = mid(LHipLm, RHipLm);
            const Vec3 shoulderMid = mid(LShoulderLm, RShoulderLm);
            Vec3 dir = sub(shoulderMid, hipMid);
            if (!normalize(dir))
                return false;
            outDir = dir;
            return true;
        }
    case Neck:
    case Neck1:
        if (!visible(LShoulderLm) || !visible(RShoulderLm) || !visible(Nose))
            return false;
        {
            const Vec3 shoulderMid = mid(LShoulderLm, RShoulderLm);
            Vec3 dir = sub(p[Nose], shoulderMid);
            if (!normalize(dir))
                return false;
            outDir = dir;
            return true;
        }
    case Head:
        if (!visible(Nose) || !visible(LEar) || !visible(REar))
            return false;
        {
            const Vec3 earMid = mid(LEar, REar);
            Vec3 dir = sub(p[Nose], earMid);
            if (!normalize(dir))
                return false;
            outDir = dir;
            return true;
        }
    case RHand:
        if (!visible(RElbowLm) || !visible(RWrist))
            return false;
        {
            Vec3 dir = sub(p[RWrist], p[RElbowLm]);
            if (!normalize(dir))
                return false;
            outDir = dir;
            return true;
        }
    case LHand:
        if (!visible(LElbowLm) || !visible(LWrist))
            return false;
        {
            Vec3 dir = sub(p[LWrist], p[LElbowLm]);
            if (!normalize(dir))
                return false;
            outDir = dir;
            return true;
        }
    default:
        return false;
    }
}

void Solver::reset()
{
    m_hasPrev = false;
}

FrameResult Solver::solveFrame(const float* world, const float* visibility,
                               float minVisibility)
{
    FrameResult result;
    result.quats.fill(kIdentity);
    if (m_hasPrev)
        result.quats = m_prevQuats;  // unresolved roles hold their last pose

    // Canonicalize MediaPipe's frame (+x subject-left, +y down, +z toward camera)
    // into the CMU/canonical rig frame (+Y up, +Z forward, LEFT at +X). Flip Y
    // and keep Z (subject faces +Z); do NOT negate X — (-x,-y,+z) would swap
    // anatomical left/right on rigs whose left bones already sit at +X.
    Vec3 p[kLandmarkCount];
    std::array<std::array<float, 3>, kLandmarkCount> canon{};
    canonicalizeMediaPipeWorld(world, canon);
    for (int i = 0; i < kLandmarkCount; ++i)
        p[i] = canon[static_cast<size_t>(i)];

    auto visible = [&](int lm) {
        return !visibility || visibility[lm] >= minVisibility;
    };
    auto mid = [&](int a, int b) { return mul(add(p[a], p[b]), 0.5f); };

    std::array<Vec3, kCanonicalRoles> secondaryUsed = m_prevSecondary;

    // ---- torso ---------------------------------------------------------------
    const bool torsoVisible = visible(LShoulderLm) && visible(RShoulderLm)
                              && visible(LHipLm) && visible(RHipLm);
    Quat hipQ = kIdentity, chestQ = kIdentity;
    bool torsoOk = false;
    Vec3 torsoUp{0.f, 1.f, 0.f}, torsoRight{1.f, 0.f, 0.f};
    if (torsoVisible) {
        const Vec3 hipMid = mid(LHipLm, RHipLm);
        const Vec3 shoulderMid = mid(LShoulderLm, RShoulderLm);
        Vec3 up = sub(shoulderMid, hipMid);
        Vec3 hipLine = sub(p[RHipLm], p[LHipLm]);
        const Vec3 shoulderLine = sub(p[RShoulderLm], p[LShoulderLm]);
        // Stabilize the hip's horizontal reference. On a seated / partially-
        // occluded subject the hip landmarks are noisy and hipLine can FLIP
        // sign between frames, whipping the whole torso 180° about Y (observed:
        // Hip quat identity → (0,-1,0,0) mid-clip). The SHOULDER line is far
        // more reliable, so:
        //  (a) if the hip line is short/degenerate or points opposite the
        //      shoulder line, fall back to the shoulder line for the hip frame;
        //  (b) keep the hip line temporally continuous (no sudden sign flip).
        if (length(hipLine) < 0.05f * length(shoulderLine)
            || dot(hipLine, shoulderLine) < 0.f)
            hipLine = shoulderLine;
        const Vec3 prevHip{m_prevHipLine[0], m_prevHipLine[1], m_prevHipLine[2]};
        if (m_hasPrev && dot(hipLine, prevHip) < 0.f)
            hipLine = mul(hipLine, -1.f);   // preserve orientation continuity
        m_prevHipLine = {hipLine[0], hipLine[1], hipLine[2]};
        Quat q1, q2;
        const bool ok1 = basisFromPrimary(up, hipLine, q1, nullptr);
        const bool ok2 = basisFromPrimary(up, shoulderLine, q2, nullptr);
        if (ok1 && ok2) {
            torsoOk = true;
            hipQ = q1;
            chestQ = q2;
            torsoUp = up;
            normalize(torsoUp);
            torsoRight = hipLine;
            normalize(torsoRight);
            result.quats[Hip] = q1;
            result.quats[Abdomen] = nlerp(q1, q2, 0.5f);
            result.quats[Chest] = q2;
            result.quats[Neck] = q2;
            result.quats[Neck1] = q2;
            result.resolvedMask |= (1u << Hip) | (1u << Abdomen) | (1u << Chest)
                                   | (1u << Neck) | (1u << Neck1);
        }
    }

    // ---- head ------------------------------------------------------------------
    if (visible(Nose) && visible(LEar) && visible(REar)) {
        const Vec3 earMid = mid(LEar, REar);
        Vec3 x = sub(p[REar], p[LEar]);          // ear line
        Vec3 fwd = sub(p[Nose], earMid);         // facing direction
        // y (head-up) completes the frame: up = fwd x earline is unstable when
        // looking straight down, so orthonormalize fwd against x and derive y.
        if (normalize(x)) {
            Vec3 z = sub(fwd, mul(x, dot(fwd, x)));
            if (normalize(z)) {
                const Vec3 y = cross(z, x);
                result.quats[Head] = quatFromBasis(x, y, z);
                result.resolvedMask |= (1u << Head);
            }
        }
    }

    // ---- limbs -----------------------------------------------------------------
    struct Segment {
        Role role;
        int from, to;
    };
    const Segment segments[] = {
        {RShoulder, RShoulderLm, RElbowLm}, {RElbow, RElbowLm, RWrist},
        {LShoulder, LShoulderLm, LElbowLm}, {LElbow, LElbowLm, LWrist},
        {RHip, RHipLm, RKneeLm}, {RKnee, RKneeLm, RAnkle},
        {RFoot, RAnkle, RFootIndex},
        {LHip, LHipLm, LKneeLm}, {LKnee, LKneeLm, LAnkle},
        {LFoot, LAnkle, LFootIndex},
    };
    const Vec3 torsoFwd = cross(torsoRight, torsoUp);
    for (const Segment& seg : segments) {
        if (!visible(seg.from) || !visible(seg.to))
            continue;
        Vec3 dir = sub(p[seg.to], p[seg.from]);
        Vec3 y = dir;
        if (!normalize(y))
            continue;

        // Twist reference. Tracked roles get TRUE parallel transport: the
        // previous secondary axis rotated by the shortest arc between the
        // previous and current segment directions — well-defined even when
        // the segment swings straight into the old axis, and it adds zero
        // twist by construction. First frame (or a lost track / antiparallel
        // flip) seeds from the most orthogonal torso axis.
        Vec3 secondary{0.f, 0.f, 0.f};
        if (m_hasPrev && length(m_prevSecondary[seg.role]) > 0.5f) {
            const Vec3 prevY = m_prevPrimary[seg.role];
            const float c = dot(prevY, y);
            if (c > -0.999f) {
                // rotate prevSecondary by shortest-arc(prevY -> y):
                // Rodrigues with axis = prevY x y, sin = |axis|
                const Vec3 axis = cross(prevY, y);
                const Vec3 v = m_prevSecondary[seg.role];
                const Vec3 t = cross(axis, v);
                const Vec3 tt = cross(axis, t);
                // R v = v + t + tt/(1+c)   (unit-free Rodrigues form)
                secondary = add(add(v, t), mul(tt, 1.f / (1.f + c)));
            }
        }
        if (length(secondary) < 0.5f) {
            secondary = std::abs(dot(y, torsoRight)) < std::abs(dot(y, torsoFwd))
                            ? torsoRight
                            : torsoFwd;
            if (std::abs(dot(y, secondary)) > 0.99f)
                secondary = std::abs(y[0]) < 0.9f ? Vec3{1.f, 0.f, 0.f}
                                                  : Vec3{0.f, 0.f, 1.f};
        }
        Quat q;
        Vec3 used;
        if (basisFromPrimary(dir, secondary, q, &used)) {
            result.quats[seg.role] = q;
            result.resolvedMask |= (1u << seg.role);
            secondaryUsed[seg.role] = used;
            m_prevPrimary[seg.role] = y;
        }
    }
    (void)torsoOk;
    (void)hipQ;
    (void)chestQ;

    // ---- hands (wrist twist + palm frame from finger tips) ------------------
    struct HandSpec {
        Role role;
        int wrist, index, pinky;
    };
    const HandSpec hands[] = {
        {RHand, RWrist, RIndex, RPinky},
        {LHand, LWrist, LIndex, LPinky},
    };
    for (const HandSpec& h : hands) {
        if (!visible(h.wrist) || !visible(h.index) || !visible(h.pinky))
            continue;
        const Vec3 w = p[h.wrist];
        const Vec3 idx = p[h.index];
        const Vec3 pk = p[h.pinky];
        Vec3 vIdx = sub(idx, w);
        Vec3 vPk = sub(pk, w);
        if (length(vIdx) < 1e-5f || length(vPk) < 1e-5f)
            continue;
        Vec3 y = add(vIdx, vPk);
        if (!normalize(y))
            continue;
        Vec3 x = sub(idx, pk);
        if (!normalize(x)) {
            x = cross(y, torsoUp);
            if (!normalize(x))
                x = cross(y, torsoFwd);
            if (!normalize(x))
                continue;
        }
        Vec3 z = cross(x, y);
        if (!normalize(z))
            continue;
        x = cross(y, z);
        if (!normalize(x))
            continue;
        result.quats[h.role] = quatFromBasis(x, y, z);
        result.resolvedMask |= (1u << h.role);
        secondaryUsed[h.role] = x;
        m_prevPrimary[h.role] = y;
    }

    m_prevSecondary = secondaryUsed;
    m_prevQuats = result.quats;
    m_hasPrev = true;
    return result;
}

}  // namespace PoseIK
