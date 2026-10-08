/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "AnimGenerators.h"

#include <QJsonArray>

#include <algorithm>
#include <array>
#include <cmath>

namespace AnimGen {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDefaultRuntimeWindow = 4.0;

struct TypeRow { Type type; const char* id; const char* label; };
const TypeRow kTypes[] = {
    {Type::Sine, "sine", "Sine"},
    {Type::Noise, "noise", "Noise"},
    {Type::Ramp, "ramp", "Ramp"},
    {Type::FollowPath, "follow-path", "Follow path"},
    {Type::Spring, "spring", "Spring"},
};

struct KindRow { TargetKind kind; const char* id; };
const KindRow kKinds[] = {
    {TargetKind::Bone, "bone"},
    {TargetKind::Node, "node"},
    {TargetKind::Morph, "morph"},
    {TargetKind::Pose, "pose"},
    {TargetKind::Light, "light"},
    {TargetKind::Material, "material"},
};

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

// --- Perlin 1-D ------------------------------------------------------------

quint32 hash32(quint32 x)
{
    // lowbias32 (Chris Wellons) — well distributed, deterministic everywhere.
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

double gradientAt(qint64 cell, quint32 seed)
{
    const quint32 h = hash32(quint32(cell) ^ hash32(seed * 0x9E3779B9U + 0x632BE5ABU));
    return (double(h & 0xFFFFFF) / double(0xFFFFFF)) * 2.0 - 1.0; // [-1, 1]
}

double perlin1(double x, quint32 seed)
{
    // Shift the lattice by a seed-dependent fraction so the lattice points
    // (where gradient noise is exactly 0) do not land on round times.
    x += double(hash32(seed ^ 0xA511E9B3U) & 0xFFFF) / 65536.0;
    const double fl = std::floor(x);
    const qint64 i = qint64(fl);
    const double f = x - fl;
    const double g0 = gradientAt(i, seed);
    const double g1 = gradientAt(i + 1, seed);
    const double fade = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    // Gradient noise is 0 at every lattice point (|n| <= 0.5); a value-noise
    // term (random lattice values, same fade) fills those zeros so a camera
    // shake never passes through rest on a regular beat.
    const double grad = ((1.0 - fade) * (g0 * f) + fade * (g1 * (f - 1.0))) * 2.0;
    const double v0 = gradientAt(i, seed ^ 0x5bd1e995U), v1 = gradientAt(i + 1, seed ^ 0x5bd1e995U);
    const double value = v0 + (v1 - v0) * fade;
    return 0.6 * grad + 0.4 * value;
}

} // namespace

// ---------------------------------------------------------------------------
// Ids
// ---------------------------------------------------------------------------

QString typeId(Type t)
{
    for (const auto& r : kTypes)
        if (r.type == t) return QString::fromLatin1(r.id);
    return QStringLiteral("sine");
}

bool typeFromId(const QString& id, Type* out)
{
    const QString k = id.trimmed().toLower();
    for (const auto& r : kTypes) {
        if (k == QLatin1String(r.id)) { if (out) *out = r.type; return true; }
    }
    if (k == QLatin1String("path") || k == QLatin1String("followpath")
        || k == QLatin1String("follow_path")) {
        if (out) *out = Type::FollowPath;
        return true;
    }
    if (k == QLatin1String("linear") || k == QLatin1String("linear-ramp")) {
        if (out) *out = Type::Ramp;
        return true;
    }
    return false;
}

QStringList typeIds()
{
    QStringList l;
    for (const auto& r : kTypes) l << QString::fromLatin1(r.id);
    return l;
}

QString typeLabel(Type t)
{
    for (const auto& r : kTypes)
        if (r.type == t) return QString::fromLatin1(r.label);
    return QStringLiteral("Sine");
}

QString kindId(TargetKind k)
{
    for (const auto& r : kKinds)
        if (r.kind == k) return QString::fromLatin1(r.id);
    return QStringLiteral("node");
}

bool kindFromId(const QString& id, TargetKind* out)
{
    const QString k = id.trimmed().toLower();
    for (const auto& r : kKinds) {
        if (k == QLatin1String(r.id)) { if (out) *out = r.kind; return true; }
    }
    return false;
}

QStringList kindIds()
{
    QStringList l;
    for (const auto& r : kKinds) l << QString::fromLatin1(r.id);
    return l;
}

bool kindNeedsSub(TargetKind k)
{
    return k == TargetKind::Bone || k == TargetKind::Morph || k == TargetKind::Pose;
}

bool kindIsTrackBacked(TargetKind k)
{
    return k == TargetKind::Bone || k == TargetKind::Node || k == TargetKind::Morph;
}

QStringList channelsFor(TargetKind k)
{
    switch (k) {
    case TargetKind::Bone:
    case TargetKind::Node:
        return {QStringLiteral("position.x"), QStringLiteral("position.y"), QStringLiteral("position.z"),
                QStringLiteral("rotation.x"), QStringLiteral("rotation.y"), QStringLiteral("rotation.z"),
                QStringLiteral("scale.x"), QStringLiteral("scale.y"), QStringLiteral("scale.z"),
                QStringLiteral("position")};
    case TargetKind::Morph:
    case TargetKind::Pose:
        return {QStringLiteral("weight")};
    case TargetKind::Light:
        return {QStringLiteral("intensity"),
                QStringLiteral("diffuse.r"), QStringLiteral("diffuse.g"), QStringLiteral("diffuse.b"),
                QStringLiteral("specular.r"), QStringLiteral("specular.g"), QStringLiteral("specular.b")};
    case TargetKind::Material:
        return {QStringLiteral("diffuse.r"), QStringLiteral("diffuse.g"), QStringLiteral("diffuse.b"),
                QStringLiteral("diffuse.a"),
                QStringLiteral("ambient.r"), QStringLiteral("ambient.g"), QStringLiteral("ambient.b"),
                QStringLiteral("specular.r"), QStringLiteral("specular.g"), QStringLiteral("specular.b"),
                QStringLiteral("emissive.r"), QStringLiteral("emissive.g"), QStringLiteral("emissive.b"),
                QStringLiteral("shininess")};
    }
    return {};
}

bool isVectorChannel(const QString& channel)
{
    return channel == QLatin1String("position");
}

// ---------------------------------------------------------------------------
// Target grammar
// ---------------------------------------------------------------------------

QString formatTarget(const Target& t)
{
    QString s = kindId(t.kind) + QLatin1Char(':') + t.object;
    if (kindNeedsSub(t.kind))
        s += QLatin1Char('/') + t.sub;
    s += QLatin1Char('/') + t.channel;
    if (!t.clip.isEmpty())
        s += QLatin1Char('@') + t.clip;
    return s;
}

bool parseTarget(const QString& text, Target* out, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    const QString s = text.trimmed();
    const int colon = s.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return fail(QStringLiteral("target '%1' must look like kind:object/channel "
                                   "(kinds: %2)").arg(text, kindIds().join(QLatin1Char('|'))));
    Target t;
    if (!kindFromId(s.left(colon), &t.kind))
        return fail(QStringLiteral("unknown target kind '%1' (expected %2)")
                        .arg(s.left(colon), kindIds().join(QLatin1Char('|'))));
    QString rest = s.mid(colon + 1);
    const int at = rest.lastIndexOf(QLatin1Char('@'));
    if (at >= 0) {
        t.clip = rest.mid(at + 1).trimmed();
        rest = rest.left(at);
    }
    const int lastSlash = rest.lastIndexOf(QLatin1Char('/'));
    if (lastSlash <= 0)
        return fail(QStringLiteral("target '%1' has no channel (e.g. .../position.y)").arg(text));
    t.channel = rest.mid(lastSlash + 1).trimmed().toLower();
    const QString head = rest.left(lastSlash);
    if (kindNeedsSub(t.kind)) {
        const int firstSlash = head.indexOf(QLatin1Char('/'));
        if (firstSlash <= 0 || firstSlash == head.size() - 1)
            return fail(QStringLiteral("a %1 target needs object/%2/channel")
                            .arg(kindId(t.kind),
                                 t.kind == TargetKind::Bone ? QStringLiteral("bone")
                                 : t.kind == TargetKind::Morph ? QStringLiteral("morph-target")
                                                               : QStringLiteral("pose")));
        t.object = head.left(firstSlash).trimmed();
        t.sub = head.mid(firstSlash + 1).trimmed();
    } else {
        t.object = head.trimmed();
    }
    if (t.object.isEmpty())
        return fail(QStringLiteral("target '%1' has no object name").arg(text));
    if (!channelsFor(t.kind).contains(t.channel))
        return fail(QStringLiteral("channel '%1' is not valid for a %2 target (expected %3)")
                        .arg(t.channel, kindId(t.kind),
                             channelsFor(t.kind).join(QLatin1Char('|'))));
    if (out) *out = t;
    return true;
}

bool validate(Type type, const Target& target, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    if (!channelsFor(target.kind).contains(target.channel))
        return fail(QStringLiteral("channel '%1' is not valid for a %2 target")
                        .arg(target.channel, kindId(target.kind)));
    if (type == Type::FollowPath) {
        if (target.kind != TargetKind::Node && target.kind != TargetKind::Bone)
            return fail(QStringLiteral("follow-path drives a node or bone position"));
        if (!isVectorChannel(target.channel))
            return fail(QStringLiteral("follow-path needs the 'position' channel"));
    } else if (isVectorChannel(target.channel)) {
        return fail(QStringLiteral("the 'position' channel is for follow-path; use position.x/y/z"));
    }
    return true;
}

QString displayName(const Generator& g)
{
    if (!g.name.isEmpty()) return g.name;
    return typeLabel(g.type) + QStringLiteral(" → ") + formatTarget(g.target);
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

double windowEnd(const Generator& g, double clipLength)
{
    if (g.duration > 0.0) return g.startTime + g.duration;
    if (clipLength > g.startTime) return clipLength;
    return g.startTime + kDefaultRuntimeWindow;
}

double noise1D(double x, quint32 seed, int octaves)
{
    octaves = std::clamp(octaves, 1, 8);
    double sum = 0.0, amp = 1.0, norm = 0.0, freq = 1.0;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * perlin1(x * freq, seed + quint32(o) * 7919U);
        norm += amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return norm > 0.0 ? sum / norm : 0.0;
}

double springResponse(double t, double omega, double zeta)
{
    if (t <= 0.0) return 1.0;
    if (omega <= 0.0) return 1.0;
    if (zeta < 0.0) zeta = 0.0;
    if (std::abs(zeta - 1.0) < 1e-6) {
        return std::exp(-omega * t) * (1.0 + omega * t);
    }
    if (zeta < 1.0) {
        const double wd = omega * std::sqrt(1.0 - zeta * zeta);
        return std::exp(-zeta * omega * t)
             * (std::cos(wd * t) + (zeta * omega / wd) * std::sin(wd * t));
    }
    const double s = std::sqrt(zeta * zeta - 1.0);
    const double r1 = -omega * (zeta - s);
    const double r2 = -omega * (zeta + s);
    return (r2 * std::exp(r1 * t) - r1 * std::exp(r2 * t)) / (r2 - r1);
}

double evaluateScalar(const Generator& g, double t, double clipLength)
{
    const double end = windowEnd(g, clipLength);
    const double len = std::max(1e-9, end - g.startTime);
    const double u = t - g.startTime;
    const bool inside = t >= g.startTime - 1e-9 && t <= end + 1e-9;
    switch (g.type) {
    case Type::Sine:
        if (!inside) return 0.0;
        return g.offset + g.amplitude * std::sin(2.0 * kPi * g.frequency * u + g.phaseDeg * kPi / 180.0);
    case Type::Noise:
        if (!inside) return 0.0;
        return g.offset + g.amplitude * noise1D(u * g.noiseFrequency, g.seed, g.octaves);
    case Type::Ramp: {
        double k = clamp01(u / len);
        if (g.ease == Ease::Smooth) k = k * k * (3.0 - 2.0 * k);
        return g.offset + g.rampFrom + (g.rampTo - g.rampFrom) * k;
    }
    case Type::Spring:
        return g.offset + g.springTo
             + (g.springFrom - g.springTo) * springResponse(u, g.stiffness, g.damping);
    case Type::FollowPath:
        return 0.0;
    }
    return 0.0;
}

double pathParameter(const Generator& g, double t, double clipLength)
{
    const double end = windowEnd(g, clipLength);
    const double len = std::max(1e-9, end - g.startTime);
    const double loops = g.loops > 0.0 ? g.loops : 1.0;
    double k = clamp01((t - g.startTime) / len) * loops;
    if (k <= 0.0) return 0.0;
    double s = k - std::floor(k);
    // Exact integer loop counts end on the final point, not back on 0.
    if (s < 1e-9 && k > 0.0) s = 1.0;
    return s;
}

Ogre::Quaternion orientationAlong(const Ogre::Vector3& tangent)
{
    if (tangent.squaredLength() < 1e-12) return Ogre::Quaternion::IDENTITY;
    const Ogre::Vector3 fwd = tangent.normalisedCopy();
    Ogre::Vector3 up = Ogre::Vector3::UNIT_Y;
    if (std::abs(fwd.dotProduct(up)) > 0.999) up = Ogre::Vector3::UNIT_Z;
    // Ogre convention: local -Z looks forward.
    const Ogre::Vector3 zAxis = -fwd;
    const Ogre::Vector3 xAxis = up.crossProduct(zAxis).normalisedCopy();
    const Ogre::Vector3 yAxis = zAxis.crossProduct(xAxis);
    Ogre::Quaternion q;
    q.FromAxes(xAxis, yAxis, zAxis);
    q.normalise();
    return q;
}

std::vector<double> sampleTimes(double from, double to, int fps)
{
    std::vector<double> out;
    if (to < from) std::swap(from, to);
    fps = std::clamp(fps, 1, 480);
    const int n = std::max(1, int(std::ceil((to - from) * fps - 1e-6)));
    out.reserve(size_t(n) + 1);
    for (int i = 0; i <= n; ++i)
        out.push_back(i == n ? to : from + double(i) / fps);
    return out;
}

// ---------------------------------------------------------------------------
// PathSampler
// ---------------------------------------------------------------------------

PathSampler::PathSampler(const std::vector<Ogre::Vector3>& anchors, bool closed)
    : m_anchors(anchors)
{
    const int n = int(anchors.size());
    if (n < 2) return;
    const int segs = closed ? n : n - 1;
    auto P = [&](int i) -> Ogre::Vector3 {
        if (closed) return anchors[size_t(((i % n) + n) % n)];
        return anchors[size_t(std::clamp(i, 0, n - 1))];
    };
    for (int i = 0; i < segs; ++i) {
        const Ogre::Vector3 p0 = P(i), p3 = P(i + 1);
        // Open ends: mirror the neighbour so the end tangent points along the
        // first/last chord instead of collapsing to zero length.
        const Ogre::Vector3 prev = (!closed && i == 0) ? p0 - (p3 - p0) : P(i - 1);
        const Ogre::Vector3 next = (!closed && i + 1 == n - 1) ? p3 + (p3 - p0) : P(i + 2);
        Segment s;
        s.p0 = p0;
        s.p1 = p0 + (p3 - prev) / 6.0f;
        s.p2 = p3 - (next - p0) / 6.0f;
        s.p3 = p3;
        m_segments.push_back(s);
    }
    // Arc-length table.
    const int perSeg = 64;
    const int total = segs * perSeg;
    m_arcU.reserve(size_t(total) + 1);
    m_arcLen.reserve(size_t(total) + 1);
    Ogre::Vector3 prev = at(0.0);
    double acc = 0.0;
    m_arcU.push_back(0.0);
    m_arcLen.push_back(0.0);
    for (int k = 1; k <= total; ++k) {
        const double u = double(k) / perSeg;
        const Ogre::Vector3 p = at(u);
        acc += double((p - prev).length());
        prev = p;
        m_arcU.push_back(u);
        m_arcLen.push_back(acc);
    }
    m_length = acc;
}

Ogre::Vector3 PathSampler::at(double u) const
{
    if (m_segments.empty())
        return m_anchors.empty() ? Ogre::Vector3::ZERO : m_anchors.front();
    const int segs = int(m_segments.size());
    u = std::clamp(u, 0.0, double(segs));
    int i = std::min(int(u), segs - 1);
    const float t = float(u - i);
    const Segment& s = m_segments[size_t(i)];
    const float a = 1.0f - t;
    return s.p0 * (a * a * a) + s.p1 * (3.0f * a * a * t) + s.p2 * (3.0f * a * t * t) + s.p3 * (t * t * t);
}

Ogre::Vector3 PathSampler::derivative(double u) const
{
    if (m_segments.empty()) return Ogre::Vector3::ZERO;
    const int segs = int(m_segments.size());
    u = std::clamp(u, 0.0, double(segs));
    int i = std::min(int(u), segs - 1);
    const float t = float(u - i);
    const Segment& s = m_segments[size_t(i)];
    const float a = 1.0f - t;
    return (s.p1 - s.p0) * (3.0f * a * a) + (s.p2 - s.p1) * (6.0f * a * t) + (s.p3 - s.p2) * (3.0f * t * t);
}

double PathSampler::uniformParam(double s) const
{
    s = clamp01(s);
    if (m_arcLen.size() < 2 || m_length <= 1e-12) return s * double(m_segments.size());
    const double target = s * m_length;
    auto it = std::lower_bound(m_arcLen.begin(), m_arcLen.end(), target);
    if (it == m_arcLen.begin()) return 0.0;
    if (it == m_arcLen.end()) return m_arcU.back();
    const size_t hi = size_t(it - m_arcLen.begin());
    const size_t lo = hi - 1;
    const double span = m_arcLen[hi] - m_arcLen[lo];
    const double f = span > 1e-12 ? (target - m_arcLen[lo]) / span : 0.0;
    return m_arcU[lo] + f * (m_arcU[hi] - m_arcU[lo]);
}

Ogre::Vector3 PathSampler::position(double s, bool constantSpeed) const
{
    if (m_segments.empty())
        return m_anchors.empty() ? Ogre::Vector3::ZERO : m_anchors.front();
    const double u = constantSpeed ? uniformParam(s) : clamp01(s) * double(m_segments.size());
    return at(u);
}

Ogre::Vector3 PathSampler::tangent(double s, bool constantSpeed) const
{
    if (m_segments.empty()) return Ogre::Vector3::ZERO;
    const double u = constantSpeed ? uniformParam(s) : clamp01(s) * double(m_segments.size());
    Ogre::Vector3 d = derivative(u);
    if (d.squaredLength() < 1e-12) {
        // Coincident handles: fall back to a finite difference.
        const double eps = 1e-3;
        d = at(std::min(u + eps, double(m_segments.size()))) - at(std::max(u - eps, 0.0));
    }
    return d.squaredLength() < 1e-12 ? Ogre::Vector3::ZERO : d.normalisedCopy();
}

std::vector<Ogre::Vector3> PathSampler::polyline(int perSegment) const
{
    std::vector<Ogre::Vector3> out;
    if (m_segments.empty()) {
        if (!m_anchors.empty()) out.push_back(m_anchors.front());
        return out;
    }
    perSegment = std::max(2, perSegment);
    const int segs = int(m_segments.size());
    for (int k = 0; k <= segs * perSegment; ++k)
        out.push_back(at(double(k) / perSegment));
    return out;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

QString schemaId() { return QStringLiteral("qtmesh-anim-generators-v1"); }

QJsonObject toJson(const Generator& g)
{
    QJsonObject o;
    o[QStringLiteral("id")] = g.id;
    if (!g.name.isEmpty()) o[QStringLiteral("name")] = g.name;
    o[QStringLiteral("type")] = typeId(g.type);
    o[QStringLiteral("target")] = formatTarget(g.target);
    o[QStringLiteral("enabled")] = g.enabled;
    o[QStringLiteral("baked")] = g.baked;
    o[QStringLiteral("start")] = g.startTime;
    o[QStringLiteral("duration")] = g.duration;
    o[QStringLiteral("fps")] = g.sampleFps;
    o[QStringLiteral("offset")] = g.offset;
    switch (g.type) {
    case Type::Sine:
        o[QStringLiteral("amplitude")] = g.amplitude;
        o[QStringLiteral("frequency")] = g.frequency;
        o[QStringLiteral("phase")] = g.phaseDeg;
        break;
    case Type::Noise:
        o[QStringLiteral("amplitude")] = g.amplitude;
        o[QStringLiteral("seed")] = double(g.seed);
        o[QStringLiteral("noise_frequency")] = g.noiseFrequency;
        o[QStringLiteral("octaves")] = g.octaves;
        break;
    case Type::Ramp:
        o[QStringLiteral("from")] = g.rampFrom;
        o[QStringLiteral("to")] = g.rampTo;
        o[QStringLiteral("ease")] = g.ease == Ease::Smooth ? QStringLiteral("smooth") : QStringLiteral("linear");
        break;
    case Type::Spring:
        o[QStringLiteral("from")] = g.springFrom;
        o[QStringLiteral("to")] = g.springTo;
        o[QStringLiteral("stiffness")] = g.stiffness;
        o[QStringLiteral("damping")] = g.damping;
        break;
    case Type::FollowPath: {
        QJsonArray pts;
        for (const auto& p : g.pathPoints)
            pts.append(QJsonArray{double(p.x), double(p.y), double(p.z)});
        o[QStringLiteral("points")] = pts;
        o[QStringLiteral("closed")] = g.pathClosed;
        o[QStringLiteral("constant_speed")] = g.constantSpeed;
        o[QStringLiteral("orient")] = g.orientToPath;
        o[QStringLiteral("loops")] = g.loops;
        break;
    }
    }
    if (!g.state.isEmpty()) o[QStringLiteral("state")] = g.state;
    return o;
}

namespace {
bool finiteNumber(const QJsonValue& v, double* out)
{
    if (!v.isDouble()) return false;
    const double d = v.toDouble();
    if (!std::isfinite(d)) return false;
    *out = d;
    return true;
}
} // namespace

bool fromJson(const QJsonObject& o, Generator* out, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    Generator g;
    g.id = o.value(QStringLiteral("id")).toString();
    if (g.id.isEmpty()) return fail(QStringLiteral("generator without an id"));
    g.name = o.value(QStringLiteral("name")).toString();
    if (!typeFromId(o.value(QStringLiteral("type")).toString(), &g.type))
        return fail(QStringLiteral("generator %1: unknown type '%2'")
                        .arg(g.id, o.value(QStringLiteral("type")).toString()));
    QString err;
    if (!parseTarget(o.value(QStringLiteral("target")).toString(), &g.target, &err))
        return fail(QStringLiteral("generator %1: %2").arg(g.id, err));
    if (!validate(g.type, g.target, &err))
        return fail(QStringLiteral("generator %1: %2").arg(g.id, err));
    g.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    g.baked = o.value(QStringLiteral("baked")).toBool(false);

    auto num = [&](const char* key, double* dst) -> bool {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined() || v.isNull()) return true;  // keep the default
        double d = 0.0;
        if (!finiteNumber(v, &d)) {
            fail(QStringLiteral("generator %1: '%2' must be a finite number").arg(g.id, QLatin1String(key)));
            return false;
        }
        *dst = d;
        return true;
    };
    double fps = g.sampleFps, seed = g.seed, octaves = g.octaves;
    double from = 0.0, to = 1.0;
    const bool hasFrom = o.contains(QStringLiteral("from"));
    const bool hasTo = o.contains(QStringLiteral("to"));
    if (!num("start", &g.startTime) || !num("duration", &g.duration) || !num("fps", &fps)
        || !num("offset", &g.offset) || !num("amplitude", &g.amplitude)
        || !num("frequency", &g.frequency) || !num("phase", &g.phaseDeg) || !num("seed", &seed)
        || !num("noise_frequency", &g.noiseFrequency) || !num("octaves", &octaves)
        || !num("from", &from) || !num("to", &to) || !num("stiffness", &g.stiffness)
        || !num("damping", &g.damping) || !num("loops", &g.loops))
        return false;
    g.sampleFps = std::clamp(int(std::lround(fps)), 1, 480);
    g.seed = quint32(std::max(0.0, seed));
    g.octaves = std::clamp(int(std::lround(octaves)), 1, 8);
    if (g.duration < 0.0) g.duration = 0.0;
    if (g.type == Type::Ramp) {
        if (hasFrom) g.rampFrom = from;
        if (hasTo) g.rampTo = to;
        g.ease = o.value(QStringLiteral("ease")).toString() == QLatin1String("smooth") ? Ease::Smooth : Ease::Linear;
    } else if (g.type == Type::Spring) {
        if (hasFrom) g.springFrom = from;
        if (hasTo) g.springTo = to;
    }
    if (g.type == Type::FollowPath) {
        const QJsonArray pts = o.value(QStringLiteral("points")).toArray();
        for (const QJsonValue& v : pts) {
            const QJsonArray p = v.toArray();
            double x = 0, y = 0, z = 0;
            if (p.size() != 3 || !finiteNumber(p[0], &x) || !finiteNumber(p[1], &y) || !finiteNumber(p[2], &z))
                return fail(QStringLiteral("generator %1: every path point must be [x, y, z] numbers").arg(g.id));
            g.pathPoints.emplace_back(float(x), float(y), float(z));
        }
        g.pathClosed = o.value(QStringLiteral("closed")).toBool(false);
        g.constantSpeed = o.value(QStringLiteral("constant_speed")).toBool(true);
        g.orientToPath = o.value(QStringLiteral("orient")).toBool(false);
    }
    g.state = o.value(QStringLiteral("state")).toObject();
    if (out) *out = g;
    return true;
}

QJsonObject toDocument(const std::vector<Generator>& gens)
{
    QJsonArray arr;
    for (const auto& g : gens) arr.append(toJson(g));
    QJsonObject doc;
    doc[QStringLiteral("schema")] = schemaId();
    doc[QStringLiteral("generators")] = arr;
    return doc;
}

bool fromDocument(const QJsonObject& doc, std::vector<Generator>* out, QString* error)
{
    if (doc.value(QStringLiteral("schema")).toString() != schemaId()) {
        if (error) *error = QStringLiteral("not a %1 document").arg(schemaId());
        return false;
    }
    std::vector<Generator> gens;
    QStringList seen;
    for (const QJsonValue& v : doc.value(QStringLiteral("generators")).toArray()) {
        Generator g;
        if (!fromJson(v.toObject(), &g, error)) return false;
        if (seen.contains(g.id)) {
            if (error) *error = QStringLiteral("duplicate generator id '%1'").arg(g.id);
            return false;
        }
        seen << g.id;
        gens.push_back(std::move(g));
    }
    if (out) *out = std::move(gens);
    return true;
}

bool applyParam(Generator* g, const QString& keyIn, const QString& valueIn, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    const QString key = keyIn.trimmed().toLower().replace(QLatin1Char('-'), QLatin1Char('_'));
    const QString value = valueIn.trimmed();
    auto number = [&](double* dst) -> bool {
        bool ok = false;
        const double d = value.toDouble(&ok);
        if (!ok || !std::isfinite(d))
            return fail(QStringLiteral("'%1' needs a number, got '%2'").arg(keyIn, valueIn));
        *dst = d;
        return true;
    };
    auto boolean = [&](bool* dst) -> bool {
        const QString v = value.toLower();
        if (v == QLatin1String("1") || v == QLatin1String("true") || v == QLatin1String("yes") || v == QLatin1String("on")) { *dst = true; return true; }
        if (v == QLatin1String("0") || v == QLatin1String("false") || v == QLatin1String("no") || v == QLatin1String("off")) { *dst = false; return true; }
        return fail(QStringLiteral("'%1' needs true/false, got '%2'").arg(keyIn, valueIn));
    };
    double d = 0.0;
    if (key == QLatin1String("amplitude")) return number(&g->amplitude);
    if (key == QLatin1String("frequency") || key == QLatin1String("freq")) return number(&g->frequency);
    if (key == QLatin1String("phase")) return number(&g->phaseDeg);
    if (key == QLatin1String("offset")) return number(&g->offset);
    if (key == QLatin1String("noise_frequency") || key == QLatin1String("scale")) return number(&g->noiseFrequency);
    if (key == QLatin1String("stiffness")) return number(&g->stiffness);
    if (key == QLatin1String("damping")) return number(&g->damping);
    if (key == QLatin1String("start") || key == QLatin1String("start_time")) return number(&g->startTime);
    if (key == QLatin1String("loops")) return number(&g->loops);
    if (key == QLatin1String("duration")) {
        if (!number(&d)) return false;
        g->duration = std::max(0.0, d);
        return true;
    }
    if (key == QLatin1String("seed")) {
        if (!number(&d)) return false;
        g->seed = quint32(std::max(0.0, d));
        return true;
    }
    if (key == QLatin1String("octaves")) {
        if (!number(&d)) return false;
        g->octaves = std::clamp(int(std::lround(d)), 1, 8);
        return true;
    }
    if (key == QLatin1String("fps")) {
        if (!number(&d)) return false;
        g->sampleFps = std::clamp(int(std::lround(d)), 1, 480);
        return true;
    }
    if (key == QLatin1String("from")) {
        if (!number(&d)) return false;
        if (g->type == Type::Spring) g->springFrom = d; else g->rampFrom = d;
        return true;
    }
    if (key == QLatin1String("to") || key == QLatin1String("target_value")) {
        if (!number(&d)) return false;
        if (g->type == Type::Spring) g->springTo = d; else g->rampTo = d;
        return true;
    }
    if (key == QLatin1String("ease")) {
        if (value.compare(QLatin1String("smooth"), Qt::CaseInsensitive) == 0) g->ease = Ease::Smooth;
        else if (value.compare(QLatin1String("linear"), Qt::CaseInsensitive) == 0) g->ease = Ease::Linear;
        else return fail(QStringLiteral("ease must be linear or smooth"));
        return true;
    }
    if (key == QLatin1String("closed")) return boolean(&g->pathClosed);
    if (key == QLatin1String("constant_speed")) return boolean(&g->constantSpeed);
    if (key == QLatin1String("orient") || key == QLatin1String("orient_to_path")) return boolean(&g->orientToPath);
    if (key == QLatin1String("name")) { g->name = value; return true; }
    if (key == QLatin1String("points")) {
        std::vector<Ogre::Vector3> pts;
        for (const QString& p : value.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const QStringList c = p.split(QLatin1Char(','));
            bool ok1 = false, ok2 = false, ok3 = false;
            if (c.size() != 3)
                return fail(QStringLiteral("path point '%1' must be x,y,z").arg(p));
            const double x = c[0].trimmed().toDouble(&ok1), y = c[1].trimmed().toDouble(&ok2),
                         z = c[2].trimmed().toDouble(&ok3);
            if (!ok1 || !ok2 || !ok3 || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                return fail(QStringLiteral("path point '%1' must be three numbers").arg(p));
            pts.emplace_back(float(x), float(y), float(z));
        }
        g->pathPoints = std::move(pts);
        return true;
    }
    return fail(QStringLiteral("unknown generator parameter '%1'").arg(keyIn));
}

QString uniqueId(const std::vector<Generator>& existing)
{
    int n = int(existing.size()) + 1;
    for (;; ++n) {
        const QString id = QStringLiteral("gen_%1").arg(n);
        bool used = false;
        for (const auto& g : existing) if (g.id == id) { used = true; break; }
        if (!used) return id;
    }
}

} // namespace AnimGen
