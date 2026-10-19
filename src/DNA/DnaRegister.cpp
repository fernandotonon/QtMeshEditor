#include "DnaRegister.h"

#include "FaceRig/NonRigidICP.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Dna {
namespace Fit {

namespace {

struct Bounds {
    std::array<float, 3> mn{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    std::array<float, 3> mx{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    void add(const float* p) { for (int k = 0; k < 3; ++k) { mn[size_t(k)] = std::min(mn[size_t(k)], p[k]); mx[size_t(k)] = std::max(mx[size_t(k)], p[k]); } }
    double diag() const { double d = 0; for (int k = 0; k < 3; ++k) d += double(mx[size_t(k)] - mn[size_t(k)]) * double(mx[size_t(k)] - mn[size_t(k)]); return std::sqrt(d); }
};

}  // namespace

RegisterResult registerToSurface(const Rig& rig, const std::vector<float>& targetV, const std::vector<int>& targetF,
                                 const RegisterOptions& options)
{
    RegisterResult r;
    if (targetV.size() < 9 || targetF.size() < 3) { r.error = QStringLiteral("the target surface is empty"); return r; }
    for (float v : targetV) if (!std::isfinite(v)) { r.error = QStringLiteral("the target surface has non-finite positions"); return r; }
    for (int i : targetF) if (i < 0 || size_t(i) * 3 + 2 >= targetV.size()) { r.error = QStringLiteral("the target surface has an out-of-range triangle index"); return r; }

    // ---- the template: every imported mesh's positions + triangles, concatenated ----
    const std::vector<size_t> imported = importedMeshIndices(rig);
    if (imported.empty()) { r.error = QStringLiteral("the rig has no mesh to register"); return r; }
    std::vector<float> tv;
    std::vector<int> tf;
    std::vector<size_t> offset(rig.meshes.size(), 0);
    for (size_t mi : imported) {
        const Mesh& m = rig.meshes[mi];
        offset[mi] = tv.size() / 3;
        tv.insert(tv.end(), m.positions.begin(), m.positions.end());
        for (const auto& face : m.faces) {
            if (face.size() < 3) continue;
            auto pos = [&](size_t k) { return int(offset[mi] + m.layouts[face[k]].position); };
            for (size_t k = 1; k + 1 < face.size(); ++k) { tf.push_back(pos(0)); tf.push_back(pos(k)); tf.push_back(pos(k + 1)); }
        }
    }
    r.templateVertices = int(tv.size() / 3);
    if (tf.empty()) { r.error = QStringLiteral("the rig mesh has no triangles"); return r; }

    // ---- pre-align: uniform scale by height, feet and centre matched (Y is up) ----
    Bounds tb, ub;
    for (size_t i = 0; i + 2 < tv.size(); i += 3) tb.add(&tv[i]);
    for (size_t i = 0; i + 2 < targetV.size(); i += 3) ub.add(&targetV[i]);
    const double th = double(tb.mx[1] - tb.mn[1]), uh = double(ub.mx[1] - ub.mn[1]);
    if (th <= 0 || uh <= 0) { r.error = QStringLiteral("a surface with no height cannot be aligned"); return r; }
    double s = 1.0;
    std::array<double, 3> shift{0, 0, 0};
    if (options.alignHeight) {
        s = uh / th;
        const double tcx = 0.5 * double(tb.mn[0] + tb.mx[0]), tcz = 0.5 * double(tb.mn[2] + tb.mx[2]);
        const double ucx = 0.5 * double(ub.mn[0] + ub.mx[0]), ucz = 0.5 * double(ub.mn[2] + ub.mx[2]);
        shift = {ucx - s * tcx, double(ub.mn[1]) - s * double(tb.mn[1]), ucz - s * tcz};
    }
    r.alignScale = s;
    std::vector<float> aligned(tv.size());
    for (size_t i = 0; i + 2 < tv.size(); i += 3)
        for (int k = 0; k < 3; ++k) aligned[i + size_t(k)] = float(s * double(tv[i + size_t(k)]) + shift[size_t(k)]);

    const std::vector<std::array<double, 3>> jointPos = jointWorldPositions(rig);
    FaceRig::NricpOptions no;
    if (!options.stiffness.empty()) no.stiffness = options.stiffness;
    for (const auto& a : options.jointAnchors) {
        int ji = -1;
        for (size_t j = 0; j < rig.joints.size(); ++j) if (rig.joints[j].name == a.first) { ji = int(j); break; }
        if (ji < 0) { r.warnings << QStringLiteral("anchor joint '%1' not found").arg(a.first); continue; }
        const std::array<double, 3> w = jointPos[size_t(ji)];
        double best = 1e300; int bv = -1;
        for (size_t v = 0; v + 2 < aligned.size(); v += 3) {
            double d = 0;
            for (int k = 0; k < 3; ++k) { const double dd = double(aligned[v + size_t(k)]) - (s * w[size_t(k)] + shift[size_t(k)]); d += dd * dd; }
            if (d < best) { best = d; bv = int(v / 3); }
        }
        if (bv >= 0) no.landmarks.push_back({bv, a.second});
    }

    const FaceRig::NricpResult nr = FaceRig::fit(aligned, tf, targetV, targetF, no);
    if (!nr.ok || nr.fitted.size() != tv.size()) { r.error = QStringLiteral("the non-rigid registration did not converge"); return r; }

    r.positions.assign(rig.meshes.size(), {});
    for (size_t mi : imported) {
        const size_t np = rig.meshes[mi].positions.size() / 3;
        r.positions[mi].assign(nr.fitted.begin() + std::ptrdiff_t(offset[mi] * 3), nr.fitted.begin() + std::ptrdiff_t((offset[mi] + np) * 3));
    }
    std::vector<float> res = nr.residual;
    std::sort(res.begin(), res.end());
    const double diag = nr.diag > 0 ? nr.diag : ub.diag();
    r.meanResidualPct = 100.0 * nr.meanResidual / diag;
    r.maxResidualPct = 100.0 * nr.maxResidual / diag;
    r.p95ResidualPct = res.empty() ? 0 : 100.0 * double(res[size_t(double(res.size() - 1) * 0.95)]) / diag;
    r.ok = true;
    return r;
}

std::vector<std::pair<QString, QStringList>> defaultJointNameMap()
{
    auto side = [](const char* base, const QStringList& l, const QStringList& r, std::vector<std::pair<QString, QStringList>>& out) {
        out.push_back({QString::fromLatin1(base) + "_l", l});
        out.push_back({QString::fromLatin1(base) + "_r", r});
    };
    std::vector<std::pair<QString, QStringList>> m;
    m.push_back({"pelvis", {"Hips", "hips", "pelvis", "Pelvis", "mixamorig:Hips", "mixamorig_Hips"}});
    m.push_back({"spine_03", {"Spine1", "spine1", "Spine_1", "mixamorig:Spine1", "mixamorig_Spine1"}});
    m.push_back({"spine_05", {"Spine2", "spine2", "Chest", "UpperChest", "mixamorig:Spine2", "mixamorig_Spine2"}});
    m.push_back({"neck_01", {"Neck", "neck", "mixamorig:Neck", "mixamorig_Neck"}});
    m.push_back({"head", {"Head", "head", "mixamorig:Head", "mixamorig_Head"}});
    side("upperarm", {"LeftArm", "left_arm", "L_UpperArm", "UpperArm_L", "mixamorig:LeftArm", "mixamorig_LeftArm"},
         {"RightArm", "right_arm", "R_UpperArm", "UpperArm_R", "mixamorig:RightArm", "mixamorig_RightArm"}, m);
    side("lowerarm", {"LeftForeArm", "left_forearm", "L_LowerArm", "LowerArm_L", "mixamorig:LeftForeArm", "mixamorig_LeftForeArm"},
         {"RightForeArm", "right_forearm", "R_LowerArm", "LowerArm_R", "mixamorig:RightForeArm", "mixamorig_RightForeArm"}, m);
    side("hand", {"LeftHand", "left_hand", "L_Hand", "Hand_L", "mixamorig:LeftHand", "mixamorig_LeftHand"},
         {"RightHand", "right_hand", "R_Hand", "Hand_R", "mixamorig:RightHand", "mixamorig_RightHand"}, m);
    side("thigh", {"LeftUpLeg", "left_upleg", "L_UpperLeg", "UpperLeg_L", "mixamorig:LeftUpLeg", "mixamorig_LeftUpLeg"},
         {"RightUpLeg", "right_upleg", "R_UpperLeg", "UpperLeg_R", "mixamorig:RightUpLeg", "mixamorig_RightUpLeg"}, m);
    side("calf", {"LeftLeg", "left_leg", "L_LowerLeg", "LowerLeg_L", "mixamorig:LeftLeg", "mixamorig_LeftLeg"},
         {"RightLeg", "right_leg", "R_LowerLeg", "LowerLeg_R", "mixamorig:RightLeg", "mixamorig_RightLeg"}, m);
    side("foot", {"LeftFoot", "left_foot", "L_Foot", "Foot_L", "mixamorig:LeftFoot", "mixamorig_LeftFoot"},
         {"RightFoot", "right_foot", "R_Foot", "Foot_R", "mixamorig:RightFoot", "mixamorig_RightFoot"}, m);
    return m;
}

std::vector<JointAnchor> matchJointAnchors(const std::vector<std::pair<QString, std::array<double, 3>>>& targetBones,
                                           QStringList* unmatched)
{
    std::vector<JointAnchor> out;
    for (const auto& entry : defaultJointNameMap()) {
        bool found = false;
        for (const QString& cand : entry.second) {
            for (const auto& b : targetBones)
                if (b.first.compare(cand, Qt::CaseInsensitive) == 0) { out.push_back({entry.first, b.second}); found = true; break; }
            if (found) break;
        }
        if (!found && unmatched) *unmatched << entry.first;
    }
    return out;
}

JointRegisterResult registerByJoints(const Rig& rigIn, const std::vector<JointAnchor>& anchorsIn, bool alignLimbPose)
{
    JointRegisterResult r;
    std::vector<JointAnchor> anchors = anchorsIn;
    Rig rig = rigIn;
    r.alignedRig = rigIn;
    // The two characters may face different ways (a MetaHuman faces -Z; a glTF/Mixamo asset
    // usually +Z). Left/right pair correctly BY NAME in both, but the body must be turned to the
    // target's facing first or every direction step sees a ~180 degree twist. Facing is derived
    // from the skeleton: left-minus-right is the lateral axis and facing = lateral x up (Y).
    {
        auto facingOf = [](const std::vector<std::pair<std::array<double, 3>, std::array<double, 3>>>& pairs) {
            std::array<double, 3> l{0, 0, 0};
            for (const auto& p : pairs) for (int k = 0; k < 3; ++k) l[size_t(k)] += p.first[size_t(k)] - p.second[size_t(k)];
            return std::array<double, 2>{-l[2], l[0]};   // (x, z) of lateral x Y
        };
        const std::vector<std::array<double, 3>> jp0 = jointWorldPositions(rigIn);
        auto rigPos = [&](const QString& n, std::array<double, 3>& p) { for (size_t j = 0; j < rigIn.joints.size(); ++j) if (rigIn.joints[j].name == n) { p = jp0[j]; return true; } return false; };
        auto tgtPos = [&](const QString& n, std::array<double, 3>& p) { for (const auto& a : anchors) if (a.bodyJoint == n) { p = a.target; return true; } return false; };
        std::vector<std::pair<std::array<double, 3>, std::array<double, 3>>> sp, tp;
        for (const char* base : {"upperarm", "hand", "thigh", "foot"}) {
            std::array<double, 3> sl, sr, tl, tr;
            if (!rigPos(QString::fromLatin1(base) + "_l", sl) || !rigPos(QString::fromLatin1(base) + "_r", sr) ||
                !tgtPos(QString::fromLatin1(base) + "_l", tl) || !tgtPos(QString::fromLatin1(base) + "_r", tr)) continue;
            sp.push_back({sl, sr});
            tp.push_back({tl, tr});
        }
        if (!sp.empty()) {
            const auto fs = facingOf(sp), ft = facingOf(tp);
            if (std::hypot(fs[0], fs[1]) > 1e-9 && std::hypot(ft[0], ft[1]) > 1e-9) {
                // yaw about +Y taking fs onto ft: R(theta) maps (x,z) -> (x c + z s, -x s + z c)
                const double theta = std::atan2(fs[0] * ft[1] - fs[1] * ft[0] , fs[0] * ft[0] + fs[1] * ft[1]);
                const double deg = std::abs(theta) * 180.0 / 3.14159265358979;
                if (deg > 30.0) {
                    yawRig(rig, -theta);
                    r.yawDegrees = -theta * 180.0 / 3.14159265358979;
                    r.alignedRig = rig;
                    r.warnings << QStringLiteral("the target faces %1 degrees away from the rig; the body was turned about Y to face it")
                                      .arg(std::abs(r.yawDegrees), 0, 'f', 0);
                }
            }
        }
    }
    if (alignLimbPose) {
        std::vector<std::pair<QString, std::array<double, 3>>> tj;
        for (const JointAnchor& a : anchors) tj.push_back({a.bodyJoint, a.target});
        LimbAlignment la = alignLimbs(rig, tj);
        r.limbChainsAligned = la.chainsAligned;
        r.limbAngles = la.applied;
        if (la.chainsAligned > 0) { rig = la.rig; r.alignedRig = la.rig; }
    }
    const std::vector<std::array<double, 3>> jp = jointWorldPositions(rig);
    std::vector<std::array<double, 3>> src, dst;
    for (const JointAnchor& a : anchors) {
        int ji = -1;
        for (size_t j = 0; j < rig.joints.size(); ++j) if (rig.joints[j].name == a.bodyJoint) { ji = int(j); break; }
        if (ji < 0) { r.warnings << QStringLiteral("rig joint '%1' not found").arg(a.bodyJoint); continue; }
        src.push_back(jp[size_t(ji)]);
        dst.push_back(a.target);
    }
    r.anchorsUsed = int(src.size());
    const size_t anchorCount = src.size();
    if (src.size() < 6) { r.error = QStringLiteral("only %1 joints could be paired with the target's skeleton (need at least 6)").arg(src.size()); return r; }
    // A T-posed character's joints are (nearly) coplanar, so a warp fitted to them alone
    // has no depth information and squashes the body flat. Constrain depth to the overall
    // similarity scale with ghost points offset along the depth axis (Z) of every anchor.
    {
        std::array<double, 3> sc{0, 0, 0}, dc{0, 0, 0};
        for (size_t i = 0; i < src.size(); ++i) for (int k = 0; k < 3; ++k) { sc[size_t(k)] += src[i][size_t(k)] / double(src.size()); dc[size_t(k)] += dst[i][size_t(k)] / double(src.size()); }
        double ss = 0, ds = 0, ymin = 1e300, ymax = -1e300;
        for (size_t i = 0; i < src.size(); ++i) {
            for (int k = 0; k < 3; ++k) { ss += (src[i][size_t(k)] - sc[size_t(k)]) * (src[i][size_t(k)] - sc[size_t(k)]); ds += (dst[i][size_t(k)] - dc[size_t(k)]) * (dst[i][size_t(k)] - dc[size_t(k)]); }
            ymin = std::min(ymin, src[i][1]); ymax = std::max(ymax, src[i][1]);
        }
        const double scale = ss > 0 ? std::sqrt(ds / ss) : 1.0;
        const double e = 0.12 * (ymax - ymin);
        const size_t n = src.size();
        for (size_t i = 0; i < n; ++i)
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                auto a = src[i]; auto b = dst[i];
                a[2] += sgn * e; b[2] += sgn * e * scale;
                src.push_back(a); dst.push_back(b);
            }
    }
    Options o;
    o.maxControlPoints = 64;
    QString err;
    const Warp w = buildWarp(src, dst, o, &err);
    if (!w.valid) { r.error = QStringLiteral("could not build the joint warp: %1").arg(err); return r; }
    double worst = 0;
    for (size_t i = 0; i < anchorCount; ++i) {
        const auto d = w.displacement(src[i]);
        double e = 0;
        for (int k = 0; k < 3; ++k) { const double v = src[i][size_t(k)] + d[size_t(k)] - dst[i][size_t(k)]; e += v * v; }
        worst = std::max(worst, std::sqrt(e));
    }
    r.anchorResidualMm = worst * 1000.0;
    r.positions.assign(rig.meshes.size(), {});
    for (size_t mi : importedMeshIndices(rig)) {
        const auto& P = rig.meshes[mi].positions;
        auto& out = r.positions[mi];
        out.resize(P.size());
        for (size_t i = 0; i + 2 < P.size(); i += 3) {
            const std::array<double, 3> p{double(P[i]), double(P[i + 1]), double(P[i + 2])};
            const auto d = w.displacement(p);
            for (int k = 0; k < 3; ++k) out[i + size_t(k)] = float(p[size_t(k)] + d[size_t(k)]);
        }
    }
    r.ok = true;
    return r;
}

}  // namespace Fit
}  // namespace Dna
