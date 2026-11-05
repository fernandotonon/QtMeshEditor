#ifndef CHARACTERGEN_PROCEDURALHAIR_H
#define CHARACTERGEN_PROCEDURALHAIR_H

// Hair Studio, procedural half: hairstyles generated natively (no Python, no Blender, no ML) as polygonal hair CARDS on the shaped
// MHR head. Pure data (Qt core + the DNA mesh type only): every step is unit-testable without Ogre or assets.
//
//   scalp region (hairline, from the head's own geometry)  ->  sparse GUIDE curves (flow field + gravity + collision with the
//   body)  ->  clumps of cards that interpolate the nearby guides (clumping, curl, waves, frizz, layers, taper)  ->  tapered
//   ribbon strips with stable UVs  ->  validation.  The generation is a pure function of (params, scalp, seed): the same inputs
//   always give the same mesh, so a hairstyle is stored as its small parameter set (+ guide edits), never as a baked mesh.

#include "../DNA/DnaReader.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace CharacterGen {
namespace Hair {

struct V3 {
    float x = 0, y = 0, z = 0;
    V3() = default;
    V3(float a, float b, float c) : x(a), y(b), z(c) {}
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(float s) const { return {x * s, y * s, z * s}; }
    V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};
inline float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 normalized(const V3& a, const V3& fallback = V3(0, 1, 0))
{
    const float l = length(a);
    return l > 1e-9f ? a * (1.0f / l) : fallback;
}
inline V3 lerp(const V3& a, const V3& b, float t) { return a + (b - a) * t; }
inline bool finite(const V3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

/// Deterministic generator (splitmix64): the same sequence on every platform and standard library, unlike <random>'s distributions.
class Rng {
public:
    explicit Rng(uint64_t seed = 1) : m_s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t next();
    float uniform();                                ///< [0,1)
    float range(float a, float b) { return a + (b - a) * uniform(); }
    float gauss();                                  ///< ~N(0,1), clamped to +-3
private:
    uint64_t m_s;
};

// ---------------------------------------------------------------------------------------------------------------------------
// Head, scalp, body collision
// ---------------------------------------------------------------------------------------------------------------------------

/// The head as an ellipsoid plus its vertical extent (body coordinates, metres; the face looks +Z, up is +Y).
struct HeadFrame {
    bool valid = false;
    V3 centre;
    V3 radii{0.075f, 0.11f, 0.095f};
    float top = 0, chin = 0;
};

/// Spatial hash over the body surface: nearest vertex and a signed "outside" test, used to keep hair out of the head, neck and
/// shoulders.
class BodyCollider {
public:
    void build(const std::vector<float>& positions, const std::vector<std::array<uint32_t, 3>>& triangles);
    bool empty() const { return m_pos.empty(); }
    /// Index of the nearest vertex (or -1) and its distance.
    int nearest(const V3& p, float* distance = nullptr) const;
    /// Signed distance estimate: positive outside the body (along the nearest vertex's normal).
    float signedDistance(const V3& p) const;
    /// Move `p` outward so it is at least `margin` outside the body (smoothly: only along the local normal) and out of the ear spheres
    /// (`avoidEars` false for strand ROOTS, which sit on the scalp next to the ear).
    V3 pushOut(const V3& p, float margin, bool avoidEars = true) const;
    /// A solid ball hair may not enter (an ear: the ear mesh is too thin for the normal push to keep a strand from poking through to its
    /// far side). `side` is +1/-1 for the +x / -x ear: a strand inside the ball is pushed out and towards that side, i.e. over the ear.
    void addSphere(const V3& centre, float radius, float side) { m_spheres.push_back({centre, radius, side}); }
    const std::vector<V3>& positions() const { return m_pos; }
    const std::vector<V3>& normals() const { return m_nrm; }
private:
    std::vector<V3> m_pos, m_nrm;
    struct Sphere { V3 c; float r, side; };
    std::vector<Sphere> m_spheres;
    float m_cell = 0.02f;
    V3 m_min;
    int m_nx = 1, m_ny = 1, m_nz = 1;
    std::vector<std::vector<int>> m_grid;
    int cellIndex(int ix, int iy, int iz) const { return (iz * m_ny + iy) * m_nx + ix; }
};

/// The part of the head surface that grows hair, with per-vertex normals and a hairline.
struct Scalp {
    HeadFrame head;
    std::vector<V3> pos, nrm;
    std::vector<std::array<int, 3>> tri;
    std::vector<float> triArea;
    float totalArea = 0;
    bool valid() const { return head.valid && !tri.empty(); }
    /// Point on the surface for a (triangle, barycentric) pair.
    V3 pointAt(int tri, float u, float v) const;
    V3 normalAt(int tri, float u, float v) const;
};

/// Fraction (0 chin .. 1 crown) of the head height below which no hair grows in the direction (xn, zn) (head-ellipsoid units;
/// z>0 = face): high on the forehead, lower at the temples, lowest at the nape. `offset` raises (+) or lowers (-) it.
float hairlineFraction(float xn, float zn, float offset);

/// Add the two ear spheres to the collider: the ears are the head vertices that stick out furthest sideways beyond the skull.
void addEarColliders(BodyCollider* collider, const std::vector<float>& positions, const std::vector<uint8_t>& headMask);

/// 0..1 per body vertex: how much of the SKIN at this vertex is scalp (where hair grows), soft at the hairline; 0 off the head and on the
/// ears. Used to paint the scalp in the skin texture.
std::vector<float> scalpWeights(const std::vector<float>& positions, const std::vector<uint8_t>& headMask, float hairlineOffset = 0.0f);

/// Estimate the head frame from the body vertices flagged in `headMask` (the vertices driven by the head).
HeadFrame estimateHeadFrame(const std::vector<float>& positions, const std::vector<uint8_t>& headMask);
/// Cut the scalp out of the head: every head triangle whose three vertices lie above the hairline (+ `hairlineOffset`).
Scalp extractScalp(const std::vector<float>& positions, const std::vector<std::array<uint32_t, 3>>& triangles, const std::vector<uint8_t>& headMask,
                   float hairlineOffset = 0.0f);

// ---------------------------------------------------------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------------------------------------------------------

struct GuideEdit {
    int guide = 0;
    int controlPoint = -1;      ///< -1: the whole guide
    V3 delta;                   ///< metres (control point move); for -1, delta.x is a LENGTH scale offset (1 + delta.x)
};

struct Params {
    QString preset = QStringLiteral("short_straight");   ///< informational: which preset these values started from
    float length = 0.10f;       ///< metres of the longest hair measured along the strand (from the scalp)
    float density = 0.6f;       ///< 0..1: number of cards
    float volume = 0.4f;        ///< 0..1: lift and distance from the scalp
    float curliness = 0.0f;     ///< 0..1: wave -> curl amplitude and frequency
    float clumping = 0.5f;      ///< 0..1: how much the cards of one clump converge at the tips
    float frizz = 0.1f;         ///< 0..1: random irregularity
    float parting = 0.0f;       ///< -1..1: where the hair parts (0 = centre, +- = to the character's side) — the flow turns away from it
    float layers = 0.0f;        ///< 0..1: the crown is shorter than the lengths
    float taper = 0.0f;         ///< 0..1: the sides and the back are shorter than the top (undercut / fade look)
    float thickness = 0.034f;   ///< metres: card width at the widest
    float fringe = 0.0f;        ///< 0..1: front hair falls forward over the forehead
    float tipCurl = 0.0f;       ///< -1..1: the ends turn in (+, bob) or flick out (-)
    float gravity = 0.6f;       ///< 0..1: how soon hair hangs straight down (0 = stiff)
    float hairlineOffset = 0.0f;  ///< fraction of the head height
    float whorl = 0.7f;         ///< 0..1: the crown whorl, an off-centre growth point at the back of the head that the hair swirls around
    float ponytail = 0.0f;      ///< 0..1: pull the hair to a tie point at the back (length of the tail follows `length`)
    float tieHeight = 0.55f;    ///< 0..1 of the head height: where the tie sits
    int quality = 1;            ///< 0 preview, 1 normal, 2 high: segments per card
    int maxTriangles = 20000;   ///< hard budget for the generated mesh
    bool symmetric = false;     ///< mirror the guides across the head's centre plane
    uint32_t seed = 1;
    std::vector<GuideEdit> edits;
};

/// The hairline offset actually used: hair pulled back into a ponytail shows more forehead.
float effectiveHairlineOffset(const Params& p);

QStringList presetIds();
QString presetLabel(const QString& id);
/// The parameters of a bundled preset (an unknown id gives the default `Params`).
Params presetParams(const QString& id);
bool isPreset(const QString& id);

/// What a UI slider / CLI `--hair-param key=value` can change: key, label, range, step.
struct ParamSpec {
    QString key, label;
    float min, max, step;
};
std::vector<ParamSpec> paramSpecs();
/// Set one named parameter (clamped); false for an unknown key.
bool setParam(Params& p, const QString& key, double value);
double getParam(const Params& p, const QString& key);

QJsonObject toJson(const Params& p);
/// Strict-ish: unknown keys are ignored (forward compatibility), every number is clamped to its range, a non-object fails.
bool fromJson(const QJsonObject& o, Params* out, QString* error = nullptr);
QString toJsonString(const Params& p);
bool fromJsonString(const QString& s, Params* out, QString* error = nullptr);
/// Clamp every field into its documented range.
void sanitize(Params& p);

// ---------------------------------------------------------------------------------------------------------------------------
// Guides and cards
// ---------------------------------------------------------------------------------------------------------------------------

struct Guide {
    V3 root, normal;
    std::vector<V3> cp;         ///< control points (cp[0] == root), absolute positions
};

/// Catmull-Rom point of a control polygon at t in [0,1] (endpoints are kept).
V3 evalCurve(const std::vector<V3>& cp, float t);
/// Arc length of the polyline through the control points.
float curveLength(const std::vector<V3>& cp);

/// The guide set of a hairstyle: deterministic from (params, scalp), edits applied. Roots are spread over the scalp.
std::vector<Guide> generateGuides(const Params& p, const Scalp& scalp, const BodyCollider& collider);
/// Apply one edit to a guide set (also what `Params::edits` replays).
void applyGuideEdit(std::vector<Guide>& guides, const GuideEdit& e);

/// Raw card geometry, one vertex per corner of every strip (so every vertex has its own UV).
struct CardMesh {
    std::vector<V3> pos, nrm;
    std::vector<float> uv;              ///< 2 per vertex
    std::vector<uint32_t> idx;          ///< triangles
    std::vector<int> rootVertex;        ///< per card: index of its first (root) vertex pair
    int cards = 0;
    int triangles() const { return int(idx.size() / 3); }
};

/// Number of texture variants (columns) the card UVs are spread over; see `bakeCardTexturePng`.
constexpr int kCardVariants = 4;

/// Generate the cards from the guides. Respects `maxTriangles` (the card count is reduced, never the card quality).
CardMesh generateCards(const Params& p, const Scalp& scalp, const std::vector<Guide>& guides, const BodyCollider& collider);

struct ValidationReport {
    int nanVertices = 0, badIndices = 0, zeroAreaTriangles = 0, badUvs = 0;
    bool ok() const { return nanVertices == 0 && badIndices == 0 && zeroAreaTriangles == 0 && badUvs == 0; }
};
ValidationReport validate(const CardMesh& m);
/// Drop broken triangles (and non-finite / out-of-range ones); returns what was wrong.
ValidationReport repair(CardMesh& m);

/// Everything in one call: scalp -> guides -> cards -> repair. `error` is filled when the scalp could not be found.
struct Result {
    CardMesh mesh;
    int guides = 0;
    QStringList warnings;
};
Result generate(const Params& p, const Scalp& scalp, const BodyCollider& collider);

/// A DNA mesh ("head_hair"): one vertex per card corner, skin rows taken from the nearest body vertex (`bodySkin`), so the
/// roots follow the head and long hair below the neck follows the neck / shoulders.
Dna::Mesh toDnaMesh(const CardMesh& m, const BodyCollider& collider, const std::vector<std::vector<Dna::SkinInfluence>>& bodySkin);

/// The shading-map texture the cards use: `kCardVariants` columns of tapering, slightly curved strands (greyscale luminance, alpha
/// cut-out), tinted by the existing hair-colour pipeline (HairTint.h). PNG bytes.
QByteArray bakeCardTexturePng(int size = 512);

}  // namespace Hair
}  // namespace CharacterGen

#endif  // CHARACTERGEN_PROCEDURALHAIR_H
