#include "PoseSpaceRbf.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace PoseSpace {

double angleBetween(const Quat& a, const Quat& b)
{
    double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    d = std::min(1.0, std::abs(d));
    return 2.0 * std::acos(d);
}

double Set::poseDistance(const std::vector<Quat>& a, const std::vector<Quat>& b)
{
    double s = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) { const double t = angleBetween(a[i], b[i]); s += t * t; }
    return std::sqrt(s);
}

bool solveLinear(std::vector<double> A, std::vector<double>& B, size_t n, size_t rhs)
{
    if (A.size() != n * n || B.size() != n * rhs) return false;
    for (size_t c = 0; c < n; ++c) {
        size_t piv = c;
        for (size_t r = c + 1; r < n; ++r) if (std::abs(A[r * n + c]) > std::abs(A[piv * n + c])) piv = r;
        if (std::abs(A[piv * n + c]) < 1e-14) return false;
        if (piv != c) {
            for (size_t k = 0; k < n; ++k) std::swap(A[c * n + k], A[piv * n + k]);
            for (size_t k = 0; k < rhs; ++k) std::swap(B[c * rhs + k], B[piv * rhs + k]);
        }
        for (size_t r = c + 1; r < n; ++r) {
            const double f = A[r * n + c] / A[c * n + c];
            if (f == 0) continue;
            for (size_t k = c; k < n; ++k) A[r * n + k] -= f * A[c * n + k];
            for (size_t k = 0; k < rhs; ++k) B[r * rhs + k] -= f * B[c * rhs + k];
        }
    }
    for (size_t k = 0; k < rhs; ++k)
        for (size_t ri = n; ri-- > 0;) {
            double v = B[ri * rhs + k];
            for (size_t c = ri + 1; c < n; ++c) v -= A[ri * n + c] * B[c * rhs + k];
            B[ri * rhs + k] = v / A[ri * n + ri];
        }
    return true;
}

bool Set::solve(QString* error)
{
    m_solved = false;
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    if (drivers.isEmpty()) return fail(QStringLiteral("a corrective set needs at least one driver joint"));
    if (samples.empty()) return fail(QStringLiteral("a corrective set needs at least one sample pose"));
    for (const Sample& s : samples) {
        if (s.rotations.size() != size_t(drivers.size()))
            return fail(QStringLiteral("a sample has %1 rotations for %2 drivers").arg(s.rotations.size()).arg(drivers.size()));
        if (s.deltas.size() != s.vertices.size() * 3)
            return fail(QStringLiteral("a sample's deltas do not match its vertex list"));
    }
    const size_t n = samples.size() + 1;   // + the rest pose
    std::vector<std::vector<Quat>> poses(n);
    poses[0].assign(size_t(drivers.size()), Quat{1, 0, 0, 0});
    for (size_t i = 0; i < samples.size(); ++i) poses[i + 1] = samples[i].rotations;

    // width: the typical distance to the nearest other pose, widened a little so neighbours overlap
    m_sigma = sigma;
    if (m_sigma <= 0) {
        double sum = 0;
        for (size_t i = 0; i < n; ++i) {
            double nn = 1e30;
            for (size_t j = 0; j < n; ++j) if (j != i) nn = std::min(nn, poseDistance(poses[i], poses[j]));
            sum += (nn < 1e29 ? nn : 1.0);
        }
        m_sigma = std::max(1e-3, 1.5 * sum / double(n));
    }
    std::vector<double> Phi(n * n), I(n * n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        I[i * n + i] = 1.0;
        for (size_t j = 0; j < n; ++j) {
            const double d = poseDistance(poses[i], poses[j]) / m_sigma;
            Phi[i * n + j] = std::exp(-d * d) + (i == j ? ridge : 0.0);
        }
    }
    if (!solveLinear(Phi, I, n, n)) return fail(QStringLiteral("the sample poses are degenerate (two samples coincide?)"));
    m_inverse = std::move(I);
    m_solved = true;
    return true;
}

std::vector<double> Set::weights(const std::vector<Quat>& q) const
{
    std::vector<double> out(samples.size(), 0.0);
    if (!m_solved) return out;
    const size_t n = samples.size() + 1;
    std::vector<double> phi(n);
    std::vector<Quat> rest(size_t(drivers.size()), Quat{1, 0, 0, 0});
    {
        const double d = poseDistance(q, rest) / m_sigma;
        phi[0] = std::exp(-d * d);
    }
    for (size_t i = 0; i < samples.size(); ++i) {
        const double d = poseDistance(q, samples[i].rotations) / m_sigma;
        phi[i + 1] = std::exp(-d * d);
    }
    // w = phi^T * Inverse (row vector times matrix)
    for (size_t j = 1; j < n; ++j) {
        double w = 0;
        for (size_t i = 0; i < n; ++i) w += phi[i] * m_inverse[i * n + j];
        out[j - 1] = w;
    }
    return out;
}

void Set::apply(const std::vector<Quat>& q, std::vector<float>& positions) const
{
    const std::vector<double> w = weights(q);
    for (size_t s = 0; s < samples.size(); ++s) {
        if (std::abs(w[s]) < 1e-9) continue;
        const Sample& sm = samples[s];
        for (size_t k = 0; k < sm.vertices.size(); ++k) {
            const size_t v = sm.vertices[k];
            if (v * 3 + 2 >= positions.size()) continue;
            for (int a = 0; a < 3; ++a) positions[v * 3 + size_t(a)] += float(w[s] * double(sm.deltas[k * 3 + size_t(a)]));
        }
    }
}

std::vector<float> Set::evaluate(const std::vector<Quat>& q, size_t vertexCount) const
{
    std::vector<float> out(vertexCount * 3, 0.0f);
    apply(q, out);
    return out;
}

QJsonObject Set::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("schema"), QStringLiteral("qtmesh-pose-correctives-v1"));
    o.insert(QStringLiteral("name"), name);
    QJsonArray dr;
    for (const QString& d : drivers) dr.append(d);
    o.insert(QStringLiteral("drivers"), dr);
    o.insert(QStringLiteral("sigma"), sigma);
    o.insert(QStringLiteral("ridge"), ridge);
    QJsonArray ss;
    for (const Sample& s : samples) {
        QJsonObject so;
        QJsonArray rots;
        for (const Quat& q : s.rotations) rots.append(QJsonArray{q[0], q[1], q[2], q[3]});
        so.insert(QStringLiteral("rotations"), rots);
        QJsonArray vs, ds;
        for (uint32_t v : s.vertices) vs.append(double(v));
        for (float d : s.deltas) ds.append(double(d));
        so.insert(QStringLiteral("vertices"), vs);
        so.insert(QStringLiteral("deltas"), ds);
        ss.append(so);
    }
    o.insert(QStringLiteral("samples"), ss);
    return o;
}

bool Set::fromJson(const QJsonObject& o, Set* out, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    if (o.value(QStringLiteral("schema")).toString() != QStringLiteral("qtmesh-pose-correctives-v1"))
        return fail(QStringLiteral("not a qtmesh-pose-correctives-v1 document"));
    Set s;
    s.name = o.value(QStringLiteral("name")).toString();
    for (const auto& d : o.value(QStringLiteral("drivers")).toArray()) s.drivers << d.toString();
    s.sigma = o.value(QStringLiteral("sigma")).toDouble(0.0);
    s.ridge = o.value(QStringLiteral("ridge")).toDouble(1e-6);
    auto number = [&](const QJsonValue& v, double* d) { if (!v.isDouble()) return false; *d = v.toDouble(); return std::isfinite(*d); };
    for (const auto& sv : o.value(QStringLiteral("samples")).toArray()) {
        const QJsonObject so = sv.toObject();
        Sample sm;
        for (const auto& rv : so.value(QStringLiteral("rotations")).toArray()) {
            const QJsonArray ra = rv.toArray();
            if (ra.size() != 4) return fail(QStringLiteral("a rotation needs 4 numbers (w,x,y,z)"));
            Quat q;
            for (int k = 0; k < 4; ++k) if (!number(ra[k], &q[size_t(k)])) return fail(QStringLiteral("a rotation has a non-numeric value"));
            const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (len < 1e-9) return fail(QStringLiteral("a rotation has zero length"));
            for (double& c : q) c /= len;
            sm.rotations.push_back(q);
        }
        for (const auto& vv : so.value(QStringLiteral("vertices")).toArray()) {
            double d; if (!number(vv, &d) || d < 0 || d > 4294967295.0 || d != std::floor(d)) return fail(QStringLiteral("a vertex index must be a whole number in 0..4294967295"));
            sm.vertices.push_back(uint32_t(d));
        }
        for (const auto& dv : so.value(QStringLiteral("deltas")).toArray()) {
            double d; if (!number(dv, &d)) return fail(QStringLiteral("a delta is not a finite number"));
            sm.deltas.push_back(float(d));
        }
        s.samples.push_back(std::move(sm));
    }
    if (!s.solve(error)) return false;
    *out = std::move(s);
    return true;
}

}  // namespace PoseSpace
