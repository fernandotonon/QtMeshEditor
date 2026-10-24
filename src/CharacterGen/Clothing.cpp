#include "Clothing.h"

#include <QColor>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace CharacterGen {
namespace {
using V3 = std::array<double, 3>;
constexpr float kTopThickness = 0.0050f;
constexpr float kBottomThickness = 0.0045f;

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double length(const V3& a) { return std::sqrt(dot(a, a)); }
double clamp(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }

bool jointAt(const JointPositions& joints, const char* name, V3* out)
{
    const auto it = joints.find(QLatin1String(name));
    if (it == joints.end()) return false;
    *out = it->second;
    return true;
}

// The position of a point along the limb polyline A -> B -> C (metres from A, negative before A, beyond C past the end) and its
// distance from the polyline's axis.
struct LimbCoord {
    double along = 0.0;
    double dist = 0.0;
};

LimbCoord limbCoord(const V3& p, const V3& a, const V3& b, const V3& c)
{
    const V3 ab = sub(b, a);
    const V3 bc = sub(c, b);
    const double l1 = length(ab);
    const double l2 = length(bc);
    const V3 ap = sub(p, a);
    const double t1 = l1 > 1e-9 ? dot(ap, ab) / (l1 * l1) : 0.0;
    LimbCoord out;
    if (t1 < 1.0 || l2 <= 1e-9) {   // before the elbow / knee: the first segment (extended before A)
        const double t = clamp(t1, -1.0, 1.0);
        const V3 q{a[0] + ab[0] * t, a[1] + ab[1] * t, a[2] + ab[2] * t};
        out.along = t1 * l1;
        out.dist = length(sub(p, q));
        return out;
    }
    const V3 bp = sub(p, b);
    const double t2 = dot(bp, bc) / (l2 * l2);
    const double t = clamp(t2, 0.0, 1.0);
    const V3 q{b[0] + bc[0] * t, b[1] + bc[1] * t, b[2] + bc[2] * t};
    out.along = l1 + t2 * l2;
    out.dist = length(sub(p, q));
    return out;
}

double segmentLength(const JointPositions& joints, const char* a, const char* b)
{
    V3 pa{};
    V3 pb{};
    if (!jointAt(joints, a, &pa) || !jointAt(joints, b, &pb)) return 0.0;
    return length(sub(pb, pa));
}

struct TopParts {
    double hemY = 0.0, neckY = 0.0, neckZ = 0.0, shoulderX = 0.0;
    bool ok = false;
};

TopParts topParts(const JointPositions& joints)
{
    TopParts t;
    V3 hips{};
    V3 neck{};
    V3 arm{};
    if (!jointAt(joints, "Hips", &hips) || !jointAt(joints, "Neck", &neck) || !jointAt(joints, "LeftArm", &arm)) return t;
    t.hemY = hips[1] - 0.03;
    t.neckY = neck[1];
    t.neckZ = neck[2];
    t.shoulderX = std::fabs(arm[0]);
    t.ok = true;
    return t;
}

// Sleeve field of one arm: positive along the arm from just before the shoulder to `endAlong`, within `radius` of the arm's axis.
double sleeveField(const JointPositions& joints, bool left, double endAlong, double radius, const V3& p)
{
    V3 a{};
    V3 b{};
    V3 c{};
    if (!jointAt(joints, left ? "LeftArm" : "RightArm", &a) || !jointAt(joints, left ? "LeftForeArm" : "RightForeArm", &b) ||
        !jointAt(joints, left ? "LeftHand" : "RightHand", &c))
        return -1.0;
    const LimbCoord lc = limbCoord(p, a, b, c);
    return std::min({endAlong - lc.along, radius - lc.dist, lc.along + 0.075});
}

double armLength1(const JointPositions& joints) { return segmentLength(joints, "LeftArm", "LeftForeArm"); }
double armLength2(const JointPositions& joints) { return segmentLength(joints, "LeftForeArm", "LeftHand"); }

// Leg field of one side: positive from the waist down to a LEVEL hem (a fraction of the way from the hip to the knee, or from the knee to
// the foot, measured in height: a cut across the leg's own axis tilts when the legs are not vertical), within `radius` of the leg's axis.
double legField(const JointPositions& joints, bool left, double waistY, bool pants, double hemFraction, double radius, const V3& p)
{
    V3 a{};
    V3 b{};
    V3 c{};
    if (!jointAt(joints, left ? "LeftUpLeg" : "RightUpLeg", &a) || !jointAt(joints, left ? "LeftLeg" : "RightLeg", &b) ||
        !jointAt(joints, left ? "LeftFoot" : "RightFoot", &c))
        return -1.0;
    const LimbCoord lc = limbCoord(p, a, b, c);
    const double hemY = pants ? b[1] + (c[1] - b[1]) * hemFraction : a[1] + (b[1] - a[1]) * hemFraction;
    return std::min({waistY - p[1], p[1] - hemY, radius - lc.dist});
}

std::vector<float> positionNormals(const Dna::Mesh& m)
{
    std::vector<float> n(m.positions.size(), 0.0f);
    for (const auto& f : m.faces)
        for (size_t k = 1; k + 1 < f.size(); ++k) {
            const uint32_t a = m.layouts[f[0]].position;
            const uint32_t b = m.layouts[f[k]].position;
            const uint32_t c = m.layouts[f[k + 1]].position;
            const float e1[3] = {m.positions[b * 3] - m.positions[a * 3], m.positions[b * 3 + 1] - m.positions[a * 3 + 1], m.positions[b * 3 + 2] - m.positions[a * 3 + 2]};
            const float e2[3] = {m.positions[c * 3] - m.positions[a * 3], m.positions[c * 3 + 1] - m.positions[a * 3 + 1], m.positions[c * 3 + 2] - m.positions[a * 3 + 2]};
            const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            for (uint32_t v : {a, b, c})
                for (int i = 0; i < 3; ++i) n[size_t(v) * 3 + size_t(i)] += cr[i];
        }
    for (size_t v = 0; v + 2 < n.size(); v += 3) {
        const float l = std::sqrt(n[v] * n[v] + n[v + 1] * n[v + 1] + n[v + 2] * n[v + 2]);
        if (l > 1e-20f) { n[v] /= l; n[v + 1] /= l; n[v + 2] /= l; } else { n[v] = 0; n[v + 1] = 1; n[v + 2] = 0; }
    }
    return n;
}

// ---- the clipper ----------------------------------------------------------------------------------------------------------------
struct CornerPoint {
    uint32_t a = 0;      // position index (or the first end of the cut edge)
    uint32_t b = 0;      // == a for an original corner
    double t = 0.0;      // from a to b
    uint32_t uvA = 0;
    uint32_t uvB = 0;
};

uint64_t pairKey(uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; }

std::vector<Dna::SkinInfluence> blendSkin(const std::vector<Dna::SkinInfluence>& x, const std::vector<Dna::SkinInfluence>& y, double t)
{
    std::vector<Dna::SkinInfluence> out;
    auto add = [&out](const Dna::SkinInfluence& si, double w) {
        for (Dna::SkinInfluence& o : out)
            if (o.joint == si.joint) { o.weight += float(w) * si.weight; return; }
        out.push_back({si.joint, float(w) * si.weight});
    };
    for (const auto& si : x) add(si, 1.0 - t);
    for (const auto& si : y) add(si, t);
    std::sort(out.begin(), out.end(), [](const auto& l, const auto& r) { return l.weight > r.weight; });
    if (out.size() > 4) out.resize(4);
    float total = 0.0f;
    for (const auto& si : out) total += si.weight;
    if (total > 1e-8f) for (auto& si : out) si.weight /= total;
    return out;
}

class Clipper {
public:
    Clipper(const Dna::Mesh& body, const std::vector<float>& field, float thickness)
        : m_body(body), m_field(field), m_normals(positionNormals(body)), m_thickness(thickness)
    {
        m_out.name = QStringLiteral("cloth");
    }

    Dna::Mesh run()
    {
        for (const auto& face : m_body.faces)
            for (size_t k = 1; k + 1 < face.size(); ++k) clipTriangle(face[0], face[k], face[k + 1]);
        if (m_out.faces.empty()) return Dna::Mesh();
        m_out.normals = positionNormals(m_out);
        return std::move(m_out);
    }

private:
    bool inside(uint32_t pos) const { return pos < m_field.size() && m_field[pos] >= 0.0f; }

    void clipTriangle(uint32_t l0, uint32_t l1, uint32_t l2)
    {
        const std::array<uint32_t, 3> ls = {l0, l1, l2};
        std::vector<CornerPoint> poly;
        for (size_t i = 0; i < 3; ++i) {
            const Dna::Mesh::Layout& cur = m_body.layouts[ls[i]];
            const Dna::Mesh::Layout& nxt = m_body.layouts[ls[(i + 1) % 3]];
            const bool in0 = inside(cur.position);
            const bool in1 = inside(nxt.position);
            if (in0) poly.push_back({cur.position, cur.position, 0.0, cur.uv, cur.uv});
            if (in0 != in1) {
                const double f0 = double(m_field[cur.position]);
                const double f1 = double(m_field[nxt.position]);
                poly.push_back({cur.position, nxt.position, f0 / (f0 - f1), cur.uv, nxt.uv});
            }
        }
        if (poly.size() < 3) return;
        std::vector<uint32_t> layouts;
        for (const CornerPoint& cp : poly) layouts.push_back(layoutFor(cp));
        for (size_t k = 1; k + 1 < layouts.size(); ++k) m_out.faces.push_back({layouts[0], layouts[k], layouts[k + 1]});
    }

    uint32_t vertexFor(const CornerPoint& cp)
    {
        // a cut vertex is shared by the two triangles of its edge; the interpolation parameter is measured from the lower index
        const bool swapped = cp.a != cp.b && cp.a > cp.b;
        const uint32_t lo = swapped ? cp.b : cp.a;
        const uint32_t hi = swapped ? cp.a : cp.b;
        const double t = swapped ? 1.0 - cp.t : cp.t;
        const uint64_t key = cp.a == cp.b ? (uint64_t(cp.a) << 32) | cp.a : pairKey(lo, hi);
        const auto found = m_vertex.find(key);
        if (found != m_vertex.end()) return found->second;
        const uint32_t index = uint32_t(m_out.positions.size() / 3);
        for (size_t c = 0; c < 3; ++c) {
            const float p = m_body.positions[size_t(lo) * 3 + c] * float(1.0 - t) + m_body.positions[size_t(hi) * 3 + c] * float(t);
            const float n = m_normals[size_t(lo) * 3 + c] * float(1.0 - t) + m_normals[size_t(hi) * 3 + c] * float(t);
            m_out.positions.push_back(p);
            m_pushDir.push_back(n);
        }
        if (m_body.skin.size() > std::max(lo, hi)) m_out.skin.push_back(lo == hi ? m_body.skin[lo] : blendSkin(m_body.skin[lo], m_body.skin[hi], t));
        else m_out.skin.emplace_back();
        applyOffset(index);
        m_vertex.emplace(key, index);
        return index;
    }

    void applyOffset(uint32_t index)
    {
        float* n = &m_pushDir[size_t(index) * 3];
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l < 1e-12f) return;
        for (size_t c = 0; c < 3; ++c) m_out.positions[size_t(index) * 3 + c] += n[c] / l * m_thickness;
    }

    uint32_t layoutFor(const CornerPoint& cp)
    {
        const uint32_t v = vertexFor(cp);
        uint32_t uv = cp.uvA;
        if (cp.a != cp.b) {   // a new texture coordinate for a cut corner (the UVs of its two ends may differ across a seam)
            const uint64_t ukey = (uint64_t(pairKey(cp.a, cp.b)) * 1000003ULL) ^ (uint64_t(cp.uvA) << 21) ^ (uint64_t(cp.uvB) << 42) ^ (cp.a > cp.b ? 1ULL : 0ULL);
            const auto it = m_uvOfCut.find(ukey);
            if (it != m_uvOfCut.end()) {
                uv = it->second;
            } else {
                uv = uint32_t(m_out.uvs.size() / 2);
                for (size_t c = 0; c < 2; ++c)
                    m_out.uvs.push_back(m_body.uvs[size_t(cp.uvA) * 2 + c] * float(1.0 - cp.t) + m_body.uvs[size_t(cp.uvB) * 2 + c] * float(cp.t));
                m_uvOfCut.emplace(ukey, uv);
            }
        } else {
            uv = copiedUv(cp.uvA);
        }
        const uint64_t lkey = (uint64_t(v) << 32) | uv;
        const auto it = m_layout.find(lkey);
        if (it != m_layout.end()) return it->second;
        const uint32_t index = uint32_t(m_out.layouts.size());
        m_out.layouts.push_back({v, uv, v});
        m_layout.emplace(lkey, index);
        return index;
    }

    uint32_t copiedUv(uint32_t uv)
    {
        const auto it = m_uvCopy.find(uv);
        if (it != m_uvCopy.end()) return it->second;
        const uint32_t index = uint32_t(m_out.uvs.size() / 2);
        m_out.uvs.push_back(m_body.uvs[size_t(uv) * 2]);
        m_out.uvs.push_back(m_body.uvs[size_t(uv) * 2 + 1]);
        m_uvCopy.emplace(uv, index);
        return index;
    }

    const Dna::Mesh& m_body;
    const std::vector<float>& m_field;
    std::vector<float> m_normals;
    float m_thickness;
    Dna::Mesh m_out;
    std::vector<float> m_pushDir;
    std::unordered_map<uint64_t, uint32_t> m_vertex, m_layout, m_uvOfCut;
    std::unordered_map<uint32_t, uint32_t> m_uvCopy;
};

Dna::Mesh finish(Dna::Mesh m, const QString& name)
{
    m.name = name;
    return m;
}
}  // namespace

QStringList topStyleIds() { return {QStringLiteral("none"), QStringLiteral("tshirt"), QStringLiteral("longsleeve"), QStringLiteral("tank")}; }
QStringList bottomStyleIds() { return {QStringLiteral("none"), QStringLiteral("shorts"), QStringLiteral("pants")}; }

bool parseTopStyle(const QString& id, TopKind* out)
{
    const QString s = id.trimmed().toLower();
    const std::array<std::pair<const char*, TopKind>, 4> table = {{{"none", TopKind::None}, {"tshirt", TopKind::TShirt}, {"longsleeve", TopKind::LongSleeve}, {"tank", TopKind::Tank}}};
    for (const auto& e : table)
        if (s == QLatin1String(e.first)) { if (out) *out = e.second; return true; }
    return false;
}

bool parseBottomStyle(const QString& id, BottomKind* out)
{
    const QString s = id.trimmed().toLower();
    const std::array<std::pair<const char*, BottomKind>, 3> table = {{{"none", BottomKind::None}, {"shorts", BottomKind::Shorts}, {"pants", BottomKind::Pants}}};
    for (const auto& e : table)
        if (s == QLatin1String(e.first)) { if (out) *out = e.second; return true; }
    return false;
}

namespace {
struct NamedColour { const char* name; int r, g, b; };
const std::array<NamedColour, 16> kColours = {{
    {"white", 236, 236, 232}, {"black", 28, 28, 30}, {"gray", 128, 128, 132}, {"red", 190, 40, 40}, {"orange", 224, 120, 40},
    {"yellow", 232, 200, 60}, {"green", 60, 140, 70}, {"teal", 40, 140, 140}, {"blue", 50, 90, 180}, {"navy", 28, 40, 90},
    {"purple", 110, 60, 150}, {"pink", 224, 130, 160}, {"brown", 110, 70, 44}, {"beige", 214, 192, 150}, {"khaki", 160, 150, 100},
    {"denim", 60, 80, 120}}};
}  // namespace

QStringList clothColourNames()
{
    QStringList names;
    for (const NamedColour& c : kColours) names << QString::fromLatin1(c.name);
    return names;
}

bool parseClothColour(const QString& text, Rgb* out)
{
    const QString s = text.trimmed().toLower();
    for (const NamedColour& c : kColours)
        if (s == QLatin1String(c.name)) {
            if (out) *out = Rgb{float(c.r) / 255.0f, float(c.g) / 255.0f, float(c.b) / 255.0f};
            return true;
        }
    if (s.size() == 7 && s.startsWith(QLatin1Char('#'))) {
        const QColor c(s);
        if (c.isValid()) {
            if (out) *out = Rgb{float(c.redF()), float(c.greenF()), float(c.blueF())};
            return true;
        }
    }
    return false;
}

JointPositions jointWorldPositions(const Dna::Rig& rig)
{
    JointPositions world;
    std::vector<V3> pos(rig.joints.size(), V3{0, 0, 0});
    std::vector<char> done(rig.joints.size(), 0);
    for (size_t round = 0; round < rig.joints.size(); ++round) {   // parents are not guaranteed to precede their children
        bool any = false;
        for (size_t j = 0; j < rig.joints.size(); ++j) {
            if (done[j]) continue;
            const int parent = rig.joints[j].parent;
            if (parent >= 0 && !done[size_t(parent)]) continue;
            const V3 base = parent >= 0 ? pos[size_t(parent)] : V3{0, 0, 0};
            pos[j] = {base[0] + rig.joints[j].translation[0], base[1] + rig.joints[j].translation[1], base[2] + rig.joints[j].translation[2]};
            done[j] = 1;
            any = true;
        }
        if (!any) break;
    }
    for (size_t j = 0; j < rig.joints.size(); ++j) world[rig.joints[j].name] = pos[j];
    return world;
}

Dna::Mesh clipBodyByField(const Dna::Mesh& body, const std::vector<float>& field, float thickness)
{
    if (body.positions.empty() || body.layouts.empty() || body.uvs.empty()) return Dna::Mesh();
    if (field.size() < body.positions.size() / 3) return Dna::Mesh();   // one field value per position, or the cut would read past the end
    return Clipper(body, field, thickness).run();
}

float topField(TopKind style, const JointPositions& joints, const V3& p)
{
    if (style == TopKind::None) return -1.0f;
    const TopParts t = topParts(joints);
    if (!t.ok) return -1.0f;
    const bool tank = style == TopKind::Tank;
    // torso: above the hem, inside the shoulders, outside a round collar (an ellipse around the neck column, deeper in front) and below
    // the base of the neck (so the head is never inside)
    const double ax = tank ? 0.100 : 0.078;
    const double azFront = tank ? 0.150 : 0.108;
    const double azBack = tank ? 0.085 : 0.072;
    const double zc = t.neckZ + 0.012;
    const bool front = p[2] > zc;
    const double az = front ? azFront : azBack;
    const double ay = front ? (tank ? 0.13 : 0.10) : (tank ? 0.07 : 0.05);   // how far down the collar reaches
    const double ex = p[0] / ax;
    const double ez = (p[2] - zc) / az;
    const double ey = (p[1] - (t.neckY + 0.045)) / ay;
    const double collar = (std::sqrt(ex * ex + ez * ez + ey * ey) - 1.0) * 0.05;   // an ellipsoid around the neck, deeper in front
    double lateralLimit = t.shoulderX + 0.045;
    if (tank) {   // full width over the ribs, narrowing to straps over the shoulders
        // below the armhole the garment wraps the whole side of the torso (a narrower limit left the sides open); the arm leaves the
        // body above that, so the limit closes smoothly to the straps
        const double strap = clamp((p[1] - (t.neckY - 0.21)) / 0.09, 0.0, 1.0);
        lateralLimit = (t.shoulderX + 0.045) + strap * (t.shoulderX * 0.78 - (t.shoulderX + 0.045));
    }
    const double lateral = lateralLimit - std::fabs(p[0]);
    double field = std::min({p[1] - t.hemY, t.neckY + 0.05 - p[1], collar, lateral});
    if (!tank) {
        const double upper = armLength1(joints);
        const double lower = armLength2(joints);
        const double end = style == TopKind::LongSleeve ? upper + lower * 0.94 : upper * 0.52;
        field = std::max({field, sleeveField(joints, true, end, 0.088, p), sleeveField(joints, false, end, 0.088, p)});
    }
    return float(field);
}

float bottomField(BottomKind style, const JointPositions& joints, const V3& p)
{
    if (style == BottomKind::None) return -1.0f;
    V3 hips{};
    if (!jointAt(joints, "Hips", &hips)) return -1.0f;
    const double waist = hips[1] + 0.075;
    const bool pants = style == BottomKind::Pants;
    const double frac = pants ? 0.96 : 0.52;
    const double left = legField(joints, true, waist, pants, frac, 0.23, p);
    const double right = legField(joints, false, waist, pants, frac, 0.23, p);
    return float(std::max(left, right));
}

namespace {
Dna::Mesh garment(const Dna::Mesh& body, const std::vector<float>& field, float thickness, const QString& name)
{
    return finish(clipBodyByField(body, field, thickness), name);
}
}  // namespace

Dna::Mesh buildTop(TopKind style, const Dna::Mesh& body, const JointPositions& joints)
{
    if (style == TopKind::None) return Dna::Mesh();
    std::vector<float> field(body.positions.size() / 3);
    for (size_t v = 0; v < field.size(); ++v)
        field[v] = topField(style, joints, {double(body.positions[v * 3]), double(body.positions[v * 3 + 1]), double(body.positions[v * 3 + 2])});
    return garment(body, field, kTopThickness, QStringLiteral("cloth_top"));
}

Dna::Mesh buildBottom(BottomKind style, const Dna::Mesh& body, const JointPositions& joints)
{
    if (style == BottomKind::None) return Dna::Mesh();
    std::vector<float> field(body.positions.size() / 3);
    for (size_t v = 0; v < field.size(); ++v)
        field[v] = bottomField(style, joints, {double(body.positions[v * 3]), double(body.positions[v * 3 + 1]), double(body.positions[v * 3 + 2])});
    return garment(body, field, kBottomThickness, QStringLiteral("cloth_bottom"));
}

}  // namespace CharacterGen
