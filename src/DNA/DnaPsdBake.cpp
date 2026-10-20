#include "DnaPsdBake.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace Dna {
namespace Psd {

namespace {

using PoseSpace::Quat;

Quat aboutAxis(int axis, double deg)
{
    const double h = deg * 3.14159265358979323846 / 360.0;
    Quat q{std::cos(h), 0, 0, 0};
    q[size_t(1 + axis)] = std::sin(h);
    return q;
}

bool invert3(const std::array<float, 9>& m, double out[9])
{
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-12) return false;
    const double inv = 1.0 / det;
    out[0] = (e * i - f * h) * inv; out[1] = (c * h - b * i) * inv; out[2] = (b * f - c * e) * inv;
    out[3] = (f * g - d * i) * inv; out[4] = (a * i - c * g) * inv; out[5] = (c * d - a * f) * inv;
    out[6] = (d * h - e * g) * inv; out[7] = (b * g - a * h) * inv; out[8] = (a * e - b * d) * inv;
    return true;
}


// A fit warp can stretch a body part two- to fourfold along one axis where the target's clothing is baggy (measured
// on Rumba Dancing: 22 % of the offsets would have grown past 2x, up to 4x, and a corrected knee moved 270 mm). A
// corrective should follow the part's ROTATION and its overall SIZE, not that shear: replace J by (volume scale,
// clamped to [0.5, 2.5]) x (its closest rotation, by Newton polar iteration).
void orthogonalWithSize(double J[3][3])
{
    auto det3 = [](const double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    const double det = det3(J);
    double s = std::cbrt(std::max(det, 1e-9));
    s = std::min(2.5, std::max(0.5, s));
    double R[3][3];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) R[r][c] = J[r][c];
    for (int it = 0; it < 12; ++it) {
        const double d = det3(R);
        if (std::abs(d) < 1e-12) break;
        // inverse transpose via cofactors: (R^-T)[r][c] = cofactor(r,c) / det
        double T[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
                T[r][c] = (R[r1][c1] * R[r2][c2] - R[r1][c2] * R[r2][c1]) / d;
            }
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) R[r][c] = 0.5 * (R[r][c] + T[r][c]);
    }
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) J[r][c] = s * R[r][c];
}

}  // namespace

void applyVertexLinear(std::vector<PoseSpace::Set>& sets, const std::vector<std::vector<std::array<float, 9>>>& lin)
{
    std::vector<size_t> start;
    size_t total = 0;
    for (const auto& m : lin) { start.push_back(total); total += m.size(); }
    for (PoseSpace::Set& set : sets)
        for (PoseSpace::Sample& sm : set.samples)
            for (size_t k = 0; k < sm.vertices.size(); ++k) {
                const uint32_t g = sm.vertices[k];
                size_t mi = start.size();
                while (mi-- > 0) if (g >= start[mi]) break;
                if (mi >= lin.size() || g - start[mi] >= lin[mi].size()) continue;
                const auto& A = lin[mi][g - start[mi]];
                float* d = &sm.deltas[k * 3];
                const double x = d[0], y = d[1], z = d[2];
                d[0] = float(A[0] * x + A[1] * y + A[2] * z);
                d[1] = float(A[3] * x + A[4] * y + A[5] * z);
                d[2] = float(A[6] * x + A[7] * y + A[8] * z);
            }
}

std::vector<PoseSpace::Set> cachedBake(const QString& path, const Rig& rig, RigEvaluator& ev, QStringList* notes)
{
    const QFileInfo fi(path);
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(fi.absoluteFilePath().toUtf8());
    h.addData(QByteArray::number(fi.size()));
    h.addData(QByteArray::number(fi.lastModified().toMSecsSinceEpoch()));
    h.addData("qpsd1-sampling-6x3");
    // the rig the sets were baked from depends on how it was READ: LOD, unit scale, facing turn, vertex count
    h.addData(QByteArray::number(rig.lod));
    h.addData(QByteArray::number(rig.unitScale[0], 'g', 12));
    h.addData(rig.facingTurned ? "turned" : "file-facing");
    h.addData(QByteArray::number(qulonglong(rig.meshes.size())));
    for (const Mesh& m : rig.meshes) h.addData(QByteArray::number(qulonglong(m.positions.size())));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/dna_psd");
    const QString cache = dir + QLatin1Char('/') + QString::fromLatin1(h.result().toHex()) + QStringLiteral(".qpsd");
    std::vector<PoseSpace::Set> sets;
    {
        QFile f(cache);
        QString err;
        if (f.exists() && f.open(QIODevice::ReadOnly) && PoseSpace::readBinary(&f, &sets, &err) && !sets.empty()) {
            if (notes) *notes << QStringLiteral("Pose-space correctives loaded from the cache (%1 sets).").arg(sets.size());
            return sets;
        }
        sets.clear();
    }
    const auto t0 = std::chrono::steady_clock::now();
    BakeResult b = bake(rig, ev);
    if (b.sets.empty()) return {};
    sets = std::move(b.sets);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    QDir().mkpath(dir);
    QFile out(cache + QStringLiteral(".tmp"));
    if (out.open(QIODevice::WriteOnly)) {
        QString err;
        const bool ok = PoseSpace::writeBinary(&out, sets, &err);
        out.close();
        if (ok) { QFile::remove(cache); out.rename(cache); } else out.remove();
    }
    if (notes) *notes << QStringLiteral("Pose-space correctives baked in %1 ms (%2 sets) and cached.").arg(ms, 0, 'f', 0).arg(sets.size());
    return sets;
}

int appendAsBlendShapes(Rig& rig, const std::vector<PoseSpace::Set>& sets)
{
    std::vector<uint32_t> start;
    uint32_t total = 0;
    for (const Mesh& m : rig.meshes) { start.push_back(total); total += uint32_t(m.positions.size() / 3); }
    int added = 0;
    for (const PoseSpace::Set& set : sets)
        for (size_t i = 0; i < set.samples.size(); ++i) {
            const PoseSpace::Sample& sm = set.samples[i];
            const QString name = PoseSpace::channelName(set.name, i);
            if (rig.blendShapeChannels.contains(name)) continue;
            const int channel = rig.blendShapeChannels.size();
            std::vector<BlendShapeTarget> per(rig.meshes.size());
            for (BlendShapeTarget& t : per) t.channel = channel;
            for (size_t k = 0; k < sm.vertices.size(); ++k) {
                const uint32_t g = sm.vertices[k];
                size_t mi = start.size();
                while (mi-- > 0) if (g >= start[mi]) break;
                if (mi >= rig.meshes.size()) continue;
                const uint32_t v = g - start[mi];
                if (size_t(v) * 3 + 2 >= rig.meshes[mi].positions.size()) continue;
                per[mi].vertexIndices.push_back(v);
                per[mi].deltas.insert(per[mi].deltas.end(), {sm.deltas[k * 3], sm.deltas[k * 3 + 1], sm.deltas[k * 3 + 2]});
            }
            bool any = false;
            for (size_t mi = 0; mi < per.size(); ++mi)
                if (!per[mi].vertexIndices.empty()) { rig.meshes[mi].blendShapeTargets.push_back(std::move(per[mi])); any = true; }
            if (!any) continue;
            rig.blendShapeChannels << name;
            ++added;
        }
    return added;
}

void transferThroughWarp(std::vector<PoseSpace::Set>& sets, const Rig& original, const Fit::Warp& warp)
{
    if (!warp.valid) return;
    std::vector<const Mesh*> meshOf;
    std::vector<size_t> startOf;
    size_t total = 0;
    for (const Mesh& m : original.meshes) { startOf.push_back(total); total += m.positions.size() / 3; meshOf.push_back(&m); }
    auto locate = [&](uint32_t g, std::array<double, 3>& p) {
        size_t mi = startOf.size();
        while (mi-- > 0) if (g >= startOf[mi]) break;
        if (mi >= meshOf.size()) return false;
        const size_t v = g - startOf[mi];
        if (v * 3 + 2 >= meshOf[mi]->positions.size()) return false;
        p = {double(meshOf[mi]->positions[v * 3]), double(meshOf[mi]->positions[v * 3 + 1]), double(meshOf[mi]->positions[v * 3 + 2])};
        return true;
    };
    const double h = 1e-3;   // metres
    for (PoseSpace::Set& set : sets)
        for (PoseSpace::Sample& sm : set.samples)
            for (size_t k = 0; k < sm.vertices.size(); ++k) {
                std::array<double, 3> p;
                if (!locate(sm.vertices[k], p)) continue;
                double J[3][3];   // J[row][col] = d(p'_row)/dp_col
                for (int c = 0; c < 3; ++c) {
                    std::array<double, 3> a = p, b = p;
                    a[size_t(c)] += h;
                    b[size_t(c)] -= h;
                    const auto da = warp.displacement(a), db = warp.displacement(b);
                    for (int r = 0; r < 3; ++r) J[r][c] = (r == c ? 1.0 : 0.0) + (da[size_t(r)] - db[size_t(r)]) / (2 * h);
                }
                orthogonalWithSize(J);
                float* d = &sm.deltas[k * 3];
                const double x = d[0], y = d[1], z = d[2];
                for (int r = 0; r < 3; ++r) d[r] = float(J[r][0] * x + J[r][1] * y + J[r][2] * z);
            }
}

QList<QPair<QString, std::array<double, 4>>> toRotations(const QStringList& drivers, const std::vector<Quat>& q)
{
    QList<QPair<QString, std::array<double, 4>>> out;
    for (int i = 0; i < drivers.size() && size_t(i) < q.size(); ++i) out.append({drivers[i], q[size_t(i)]});
    return out;
}

BakeResult bake(const Rig& rig, RigEvaluator& ev, const BakeOptions& opt)
{
    BakeResult res;
    std::vector<uint32_t> meshOffset;
    uint32_t total = 0;
    for (const Mesh& m : rig.meshes) { meshOffset.push_back(total); total += uint32_t(m.positions.size() / 3); }
    const double eps = opt.minDeltaMm / opt.unitToMillimetres;

    for (const QString& driver : ev.driverJoints()) {
        PoseSpace::Set set;
        set.name = driver;
        set.drivers = {driver};
        for (int axis = 0; axis < 3; ++axis)
            for (double deg : opt.anglesDeg) {
                const Quat q = aboutAxis(axis, deg);
                const QList<QPair<QString, std::array<double, 4>>> rot{{driver, q}};
                const auto full = FaceBake::evaluatePositionsPosed(rig, ev, rot, true);
                const auto plain = FaceBake::evaluatePositionsPosed(rig, ev, rot, false);
                const auto lin = FaceBake::evaluateBlendedLinear(rig, ev, rot, false);
                PoseSpace::Sample sm;
                sm.rotations = {q};
                for (size_t mi = 0; mi < rig.meshes.size() && mi < full.size(); ++mi) {
                    const size_t nv = rig.meshes[mi].positions.size() / 3;
                    for (size_t v = 0; v < nv; ++v) {
                        const double dx = double(full[mi][v * 3]) - plain[mi][v * 3], dy = double(full[mi][v * 3 + 1]) - plain[mi][v * 3 + 1],
                                     dz = double(full[mi][v * 3 + 2]) - plain[mi][v * 3 + 2];
                        if (dx * dx + dy * dy + dz * dz < eps * eps) continue;
                        double inv[9];
                        if (!invert3(lin[mi][v], inv)) continue;
                        sm.vertices.push_back(meshOffset[mi] + uint32_t(v));
                        sm.deltas.push_back(float(inv[0] * dx + inv[1] * dy + inv[2] * dz));
                        sm.deltas.push_back(float(inv[3] * dx + inv[4] * dy + inv[5] * dz));
                        sm.deltas.push_back(float(inv[6] * dx + inv[7] * dy + inv[8] * dz));
                    }
                }
                if (!sm.vertices.empty()) set.samples.push_back(std::move(sm));
            }
        if (set.samples.empty()) { ++res.driversWithoutEffect; continue; }
        QString err;
        if (!set.solve(&err)) { res.warnings << QStringLiteral("%1: %2").arg(driver, err); continue; }
        res.sets.push_back(std::move(set));
        ++res.driversBaked;
    }
    return res;
}

Validation validate(const Rig& rig, RigEvaluator& ev, const std::vector<PoseSpace::Set>& sets, int poses, unsigned seed,
                    double maxAngleDeg, double unitToMillimetres)
{
    Validation v;
    // A fixed, seeded splitmix64 stream: the validation poses must be reproducible, and nothing here needs
    // (or must claim) cryptographic randomness.
    uint64_t state = uint64_t(seed) * 0x9E3779B97F4A7C15ULL + 1;
    auto next01 = [&state]() {
        state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        return double(z >> 11) / double(1ULL << 53);
    };
    auto ang = [&]() { return (next01() * 2.0 - 1.0) * maxAngleDeg; };
    auto pick = [&]() { return next01(); };
    auto axisPick = [&]() { return std::min(2, int(next01() * 3.0)); };
    const QStringList drivers = ev.driverJoints();
    std::vector<double> plainErr, psdErr;
    for (int p = 0; p < poses; ++p) {
        // a handful of drivers at once, each rotated about one random axis
        QList<QPair<QString, std::array<double, 4>>> rot;
        for (const QString& d : drivers)
            if (pick() < 0.25) rot.append({d, aboutAxis(axisPick(), ang())});
        if (rot.isEmpty()) continue;
        const auto full = FaceBake::evaluatePositionsPosed(rig, ev, rot, true);
        const auto plain = FaceBake::evaluatePositionsPosed(rig, ev, rot, false);
        const auto lin = FaceBake::evaluateBlendedLinear(rig, ev, rot, false);
        // total rest-space offset per global vertex from every set
        std::vector<float> off;
        {
            size_t total = 0;
            for (const Mesh& m : rig.meshes) total += m.positions.size() / 3;
            off.assign(total * 3, 0.0f);
        }
        for (const PoseSpace::Set& s : sets) {
            std::vector<Quat> q;
            for (const QString& dn : s.drivers) {
                Quat id{1, 0, 0, 0};
                for (const auto& r : rot) if (r.first == dn) id = r.second;
                q.push_back(id);
            }
            s.apply(q, off);
        }
        size_t base = 0;
        for (size_t mi = 0; mi < rig.meshes.size() && mi < full.size(); ++mi) {
            const size_t nv = rig.meshes[mi].positions.size() / 3;
            for (size_t i = 0; i < nv; ++i) {
                const auto& A = lin[mi][i];
                const double ox = off[(base + i) * 3], oy = off[(base + i) * 3 + 1], oz = off[(base + i) * 3 + 2];
                const double px = double(plain[mi][i * 3]) + A[0] * ox + A[1] * oy + A[2] * oz;
                const double py = double(plain[mi][i * 3 + 1]) + A[3] * ox + A[4] * oy + A[5] * oz;
                const double pz = double(plain[mi][i * 3 + 2]) + A[6] * ox + A[7] * oy + A[8] * oz;
                auto len = [](double x, double y, double z) { return std::sqrt(x * x + y * y + z * z); };
                plainErr.push_back(len(double(full[mi][i * 3]) - plain[mi][i * 3], double(full[mi][i * 3 + 1]) - plain[mi][i * 3 + 1],
                                       double(full[mi][i * 3 + 2]) - plain[mi][i * 3 + 2]) * unitToMillimetres);
                psdErr.push_back(len(double(full[mi][i * 3]) - px, double(full[mi][i * 3 + 1]) - py, double(full[mi][i * 3 + 2]) - pz) * unitToMillimetres);
            }
            base += nv;
        }
        ++v.poses;
    }
    auto stats = [](std::vector<double>& e, double& mean, double& p95, double& mx) {
        if (e.empty()) return;
        double s = 0; for (double x : e) s += x;
        mean = s / double(e.size());
        std::sort(e.begin(), e.end());
        p95 = e[size_t(double(e.size() - 1) * 0.95)];
        mx = e.back();
    };
    stats(plainErr, v.plainMeanMm, v.plainP95Mm, v.plainMaxMm);
    stats(psdErr, v.psdMeanMm, v.psdP95Mm, v.psdMaxMm);
    return v;
}

}  // namespace Psd
}  // namespace Dna
