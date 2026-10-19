#include "DnaFaceBake.h"
#include "DnaAffine.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace Dna {
namespace FaceBake {

namespace {

using Affine::Aff;
using Affine::compose;
using Affine::inverse;
using Affine::mul;

// World matrix of every joint for the evaluator's CURRENT outputs.
std::vector<Aff> worldPose(const Rig& rig, const RigEvaluator& ev)
{
    const size_t n = rig.joints.size();
    std::vector<Aff> local(n), world(n);
    for (size_t j = 0; j < n; ++j) {
        const JointPose p = ev.jointPose(int(j));
        local[j] = compose(p.translation, p.orientation, p.scale);
    }
    std::vector<char> done(n, 0);
    std::function<void(size_t)> go = [&](size_t j) {
        if (done[j]) return;
        done[j] = 1;
        const int par = rig.joints[j].parent;
        if (par < 0 || size_t(par) >= n || size_t(par) == j) { world[j] = local[j]; return; }
        go(size_t(par));
        world[j] = mul(world[size_t(par)], local[j]);
    };
    for (size_t j = 0; j < n; ++j) go(j);
    return world;
}

int resolveControl(const RigEvaluator& ev, const QString& name)
{
    int i = ev.rawControlIndex(name);
    if (i < 0) i = ev.rawControlIndex(ArkitMap::controlPrefix() + name);
    return i;
}

void applyControls(RigEvaluator& ev, const QList<QPair<QString, double>>& controls, QStringList* missing)
{
    ev.resetControls();
    for (const auto& c : controls) {
        const int idx = resolveControl(ev, c.first);
        if (idx < 0) { if (missing && !missing->contains(c.first)) missing->append(c.first); continue; }
        ev.setRawControl(idx, float(c.second));
    }
    ev.calculate();
}

std::vector<std::vector<float>> skin(const Rig& rig, const std::vector<Aff>& neutralWorld, const std::vector<Aff>& posedWorld)
{
    const size_t nj = rig.joints.size();
    std::vector<Aff> skinM(nj);
    for (size_t j = 0; j < nj; ++j) skinM[j] = mul(posedWorld[j], inverse(neutralWorld[j]));

    std::vector<std::vector<float>> out;
    out.reserve(rig.meshes.size());
    for (const Mesh& m : rig.meshes) {
        std::vector<float> pos(m.positions);
        const size_t nv = m.positions.size() / 3;
        for (size_t v = 0; v < nv && v < m.skin.size(); ++v) {
            const auto& inf = m.skin[v];
            double sum = 0;
            for (const SkinInfluence& i : inf) if (i.joint < nj) sum += i.weight;
            if (sum <= 1e-12) continue;
            const double px = m.positions[v * 3], py = m.positions[v * 3 + 1], pz = m.positions[v * 3 + 2];
            double ox = 0, oy = 0, oz = 0;
            for (const SkinInfluence& i : inf) {
                if (i.joint >= nj) continue;
                const double w = i.weight / sum;
                const Aff& M = skinM[i.joint];
                ox += w * (M.a[0] * px + M.a[1] * py + M.a[2] * pz + M.t[0]);
                oy += w * (M.a[3] * px + M.a[4] * py + M.a[5] * pz + M.t[1]);
                oz += w * (M.a[6] * px + M.a[7] * py + M.a[8] * pz + M.t[2]);
            }
            pos[v * 3] = float(ox); pos[v * 3 + 1] = float(oy); pos[v * 3 + 2] = float(oz);
        }
        out.push_back(std::move(pos));
    }
    return out;
}

}  // namespace

std::vector<std::vector<float>> evaluatePositions(const Rig& rig, RigEvaluator& ev,
                                                  const QList<QPair<QString, double>>& controls,
                                                  QStringList* missing)
{
    applyControls(ev, {}, nullptr);
    const std::vector<Aff> neutral = worldPose(rig, ev);
    applyControls(ev, controls, missing);
    const std::vector<Aff> posed = worldPose(rig, ev);
    return skin(rig, neutral, posed);
}

std::vector<std::vector<float>> evaluatePositionsPosed(const Rig& rig, RigEvaluator& ev,
                                                       const QList<QPair<QString, std::array<double, 4>>>& rotations)
{
    applyControls(ev, {}, nullptr);
    const std::vector<Aff> neutral = worldPose(rig, ev);
    ev.resetControls();
    for (const auto& r : rotations) ev.setDriverRotation(r.first, r.second);
    ev.calculate();
    const size_t n = rig.joints.size();
    std::vector<Aff> local(n), world(n);
    for (size_t j = 0; j < n; ++j) {
        JointPose p = ev.jointPose(int(j));
        for (const auto& r : rotations)
            if (ev.jointIndex(r.first) == int(j)) {
                const auto& a = p.orientation;
                const auto& d = r.second;
                p.orientation = {a[0] * d[0] - a[1] * d[1] - a[2] * d[2] - a[3] * d[3], a[0] * d[1] + a[1] * d[0] + a[2] * d[3] - a[3] * d[2],
                                 a[0] * d[2] - a[1] * d[3] + a[2] * d[0] + a[3] * d[1], a[0] * d[3] + a[1] * d[2] - a[2] * d[1] + a[3] * d[0]};
            }
        local[j] = compose(p.translation, p.orientation, p.scale);
    }
    std::vector<char> done(n, 0);
    std::function<void(size_t)> go = [&](size_t j) {
        if (done[j]) return;
        done[j] = 1;
        const int par = rig.joints[j].parent;
        if (par < 0 || size_t(par) >= n || size_t(par) == j) { world[j] = local[j]; return; }
        go(size_t(par));
        world[j] = mul(world[size_t(par)], local[j]);
    };
    for (size_t j = 0; j < n; ++j) go(j);
    return skin(rig, neutral, world);
}


Result bake(const Rig& rig, RigEvaluator& ev, const QList<ShapeSpec>& specs, const Options& options)
{
    Result res;
    applyControls(ev, {}, nullptr);
    const std::vector<Aff> neutral = worldPose(rig, ev);

    for (const ShapeSpec& spec : specs) {
        BakedShape shape;
        shape.name = spec.name;
        applyControls(ev, spec.controls, &shape.missingControls);
        if (shape.missingControls.size() == spec.controls.size()) {
            res.warnings << QStringLiteral("'%1': none of its controls exist in this DNA — skipped").arg(spec.name);
            continue;
        }
        const std::vector<Aff> posed = worldPose(rig, ev);
        const std::vector<std::vector<float>> pos = skin(rig, neutral, posed);

        shape.perMesh.resize(rig.meshes.size());
        double maxSq = 0;
        for (size_t m = 0; m < rig.meshes.size(); ++m) {
            const std::vector<float>& rest = rig.meshes[m].positions;
            for (size_t v = 0; v * 3 + 2 < rest.size(); ++v) {
                const double dx = pos[m][v * 3] - rest[v * 3], dy = pos[m][v * 3 + 1] - rest[v * 3 + 1],
                             dz = pos[m][v * 3 + 2] - rest[v * 3 + 2];
                const double sq = dx * dx + dy * dy + dz * dz;
                if (sq < options.epsilonMetres * options.epsilonMetres) continue;
                maxSq = std::max(maxSq, sq);
                SparseDelta d;
                d.vertex = uint32_t(v);
                d.delta = {float(dx), float(dy), float(dz)};
                shape.perMesh[m].push_back(d);
                ++shape.vertices;
            }
        }
        shape.maxDeltaMm = std::sqrt(maxSq) * options.unitToMillimetres;
        if (shape.vertices == 0)
            res.warnings << QStringLiteral("'%1' moves no vertices (its controls drive nothing visible at this LOD)")
                                .arg(spec.name);
        if (!shape.missingControls.isEmpty())
            res.warnings << QStringLiteral("'%1' is missing control(s): %2 — baked from the rest")
                                .arg(spec.name, shape.missingControls.join(QStringLiteral(", ")));
        res.shapes.push_back(std::move(shape));
    }
    applyControls(ev, {}, nullptr);   // leave the evaluator at rest
    return res;
}

LinearityReport linearityError(const Rig& rig, RigEvaluator& ev, const QList<ShapeSpec>& parts, const Options& options)
{
    LinearityReport rep;
    const auto neutral = evaluatePositions(rig, ev, {}, nullptr);
    QList<QPair<QString, double>> all;
    for (const ShapeSpec& s : parts) all += s.controls;
    const auto combined = evaluatePositions(rig, ev, all, nullptr);
    std::vector<std::vector<float>> sum = neutral;
    for (const ShapeSpec& s : parts) {
        const auto shaped = evaluatePositions(rig, ev, s.controls, nullptr);
        for (size_t m = 0; m < sum.size(); ++m)
            for (size_t i = 0; i < sum[m].size(); ++i)
                sum[m][i] += shaped[m][i] - neutral[m][i];
    }
    double total = 0, maxSq = 0, combSq = 0;
    for (size_t m = 0; m < sum.size(); ++m)
        for (size_t v = 0; v * 3 + 2 < sum[m].size(); ++v) {
            double e2 = 0, c2 = 0;
            for (int c = 0; c < 3; ++c) {
                const double d = double(sum[m][v * 3 + size_t(c)]) - double(combined[m][v * 3 + size_t(c)]);
                e2 += d * d;
                const double k = double(combined[m][v * 3 + size_t(c)]) - double(neutral[m][v * 3 + size_t(c)]);
                c2 += k * k;
            }
            total += std::sqrt(e2);
            maxSq = std::max(maxSq, e2);
            combSq = std::max(combSq, c2);
            ++rep.vertices;
        }
    rep.maxMm = std::sqrt(maxSq) * options.unitToMillimetres;
    rep.meanMm = rep.vertices ? total / rep.vertices * options.unitToMillimetres : 0.0;
    rep.combinedMaxMm = std::sqrt(combSq) * options.unitToMillimetres;
    return rep;
}

QList<ShapeSpec> arkitSpecs(const QList<ArkitMap::Entry>& map)
{
    QList<ShapeSpec> out;
    for (const ArkitMap::Entry& e : map) {
        ShapeSpec s;
        s.name = e.arkit;
        s.controls = e.controls;
        out << s;
    }
    return out;
}

QList<ShapeSpec> allExpressionSpecs(const RigEvaluator& ev)
{
    QList<ShapeSpec> out;
    const QString prefix = ArkitMap::controlPrefix();
    for (int i = 0; i < ev.rawControlCount(); ++i) {
        const QString n = ev.rawControlName(i);
        if (!n.startsWith(prefix)) continue;
        ShapeSpec s;
        s.name = QStringLiteral("CTRL_expressions_") + n.mid(prefix.size());
        s.controls = {{n, 1.0}};
        out << s;
    }
    return out;
}

int appendAsBlendShapes(Rig& rig, const Result& result)
{
    int added = 0;
    for (const BakedShape& shape : result.shapes) {
        if (shape.vertices == 0) continue;
        if (rig.blendShapeChannels.contains(shape.name)) continue;
        const int channel = rig.blendShapeChannels.size();
        rig.blendShapeChannels << shape.name;
        for (size_t m = 0; m < rig.meshes.size() && m < shape.perMesh.size(); ++m) {
            if (shape.perMesh[m].empty()) continue;
            BlendShapeTarget bt;
            bt.channel = channel;
            bt.vertexIndices.reserve(shape.perMesh[m].size());
            bt.deltas.reserve(shape.perMesh[m].size() * 3);
            for (const SparseDelta& d : shape.perMesh[m]) {
                bt.vertexIndices.push_back(d.vertex);
                bt.deltas.insert(bt.deltas.end(), d.delta.begin(), d.delta.end());
            }
            rig.meshes[m].blendShapeTargets.push_back(std::move(bt));
        }
        ++added;
    }
    return added;
}

}  // namespace FaceBake
}  // namespace Dna
