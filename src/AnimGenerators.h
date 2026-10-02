/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef ANIMGENERATORS_H
#define ANIMGENERATORS_H

// Procedural animation generators (#524, epic #517 Slice G).
//
// A generator fills an animatable property from a formula instead of hand-set
// keyframes: Sine (hover, breathing, sway), Noise (camera shake, weapon sway),
// Ramp, Spring (chase a target with damped-spring physics) and FollowPath (a
// node or bone travels along a smooth curve through editable points).
//
// This header is PURE DATA: types, the target grammar, evaluation, sampling and
// JSON. It has no Ogre scene / Qt GUI dependency (Ogre::Vector3/Quaternion are
// plain maths types), so every formula is unit-tested headless. The scene side
// — binding a generator to a live track, mute/bake, undo — is
// AnimGeneratorManager.
//
// Layer model: every generator except FollowPath ADDS its value to the base
// track (`base(t) + value(t)`). FollowPath REPLACES the position (and, with
// `orientToPath`, the orientation): a path drawn in the viewport is where the
// object should be, not an offset from where it was.

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <OgreQuaternion.h>
#include <OgreVector.h>

#include <vector>

namespace AnimGen {

enum class Type { Sine, Noise, Ramp, FollowPath, Spring };

QString typeId(Type t);                        // sine|noise|ramp|follow-path|spring
bool typeFromId(const QString& id, Type* out); // also accepts "path", "followpath"
QStringList typeIds();
QString typeLabel(Type t);                     // "Sine", "Follow path", …

enum class TargetKind { Bone, Node, Morph, Pose, Light, Material };

QString kindId(TargetKind k);                  // bone|node|morph|pose|light|material
bool kindFromId(const QString& id, TargetKind* out);
QStringList kindIds();

/// What a generator drives.
///
/// String form (CLI / MCP / sidecar): `kind:object[/sub]/channel[@clip]`
///   node:Hand/position.y
///   node:Drone/position@Hover            (vector channel — FollowPath only)
///   bone:Rumba/mixamorig:Spine/rotation.z@Dance
///   morph:Head/jawOpen/weight
///   pose:Rumba/Smile/weight
///   light:KeyLight/intensity
///   material:Body_MAT/diffuse.r
/// `object` is the entity (bone/morph/pose), scene node (node), light or
/// material name; `sub` is the bone, morph target or pose name. Bone names may
/// contain ':' (mixamorig:Hips) — only the FIRST ':' separates the kind.
struct Target {
    TargetKind kind = TargetKind::Node;
    QString object;
    QString sub;
    QString channel;
    QString clip;   ///< bone: skeletal clip; node: node clip; morph: weight clip

    bool operator==(const Target& o) const {
        return kind == o.kind && object == o.object && sub == o.sub
            && channel == o.channel && clip == o.clip;
    }
};

QString formatTarget(const Target& t);
bool parseTarget(const QString& text, Target* out, QString* error = nullptr);

/// Channels a target kind accepts. Bone/Node include the vector channel
/// "position" (FollowPath only).
QStringList channelsFor(TargetKind k);
/// True for the vector channel a FollowPath writes.
bool isVectorChannel(const QString& channel);
/// True when `kind` needs a `sub` (bone / morph target / pose name).
bool kindNeedsSub(TargetKind k);
/// True when the property lives in an Ogre animation track (bone, node,
/// morph) rather than being driven at runtime (pose, light, material).
bool kindIsTrackBacked(TargetKind k);

/// Validate a type against a target: FollowPath needs a bone/node "position"
/// channel, every other type a scalar channel.
bool validate(Type type, const Target& target, QString* error = nullptr);

/// Easing for the ramp.
enum class Ease { Linear, Smooth };

struct Generator {
    QString id;                 ///< unique, stable across save/load
    QString name;               ///< user label ("" → derived from type)
    Type type = Type::Sine;
    Target target;
    bool enabled = true;        ///< live; false = muted (base track only)
    bool baked = false;         ///< baked to keyframes; inactive afterwards

    double startTime = 0.0;     ///< seconds, clip-local
    double duration = 0.0;      ///< seconds; 0 = until the clip ends
    int sampleFps = 30;         ///< bake / materialisation density

    double offset = 0.0;        ///< constant added to every scalar type

    // Sine
    double amplitude = 1.0;     ///< shared with Noise
    double frequency = 1.0;     ///< Hz
    double phaseDeg = 0.0;

    // Noise (1-D Perlin fBm)
    quint32 seed = 1;
    double noiseFrequency = 1.0; ///< features per second
    int octaves = 1;             ///< 1..8

    // Ramp
    double rampFrom = 0.0;
    double rampTo = 1.0;
    Ease ease = Ease::Linear;

    // Spring (closed-form damped harmonic motion from `springFrom` toward
    // `springTo`, zero initial velocity)
    double springFrom = 0.0;
    double springTo = 1.0;
    double stiffness = 10.0;     ///< natural angular frequency ω (rad/s)
    double damping = 0.3;        ///< damping ratio ζ (1 = critical)

    // FollowPath — anchors in the space of the target's position channel
    // (the node's parent / the bone's parent bone), joined by a smooth
    // (Catmull-Rom → cubic Bézier) curve.
    std::vector<Ogre::Vector3> pathPoints;
    bool pathClosed = false;
    bool constantSpeed = true;   ///< arc-length parameterised
    bool orientToPath = false;   ///< also face along the tangent
    double loops = 1.0;          ///< traversals over the duration

    /// Adapter-owned state (base-track snapshot, baked property keys). Opaque
    /// to this header; round-trips through JSON unchanged.
    QJsonObject state;
};

QString displayName(const Generator& g);

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

/// Effective window end for a clip of length `clipLength` (duration 0 → clip
/// end; a non-positive clip length with duration 0 → start + 4 s).
double windowEnd(const Generator& g, double clipLength);

/// Scalar value of a non-path generator at clip time `t` (seconds). Sine and
/// Noise are 0 outside [start, end]; Ramp and Spring hold their boundary
/// values (`rampFrom`/`springFrom` before, the end state after) so a ramp to 1
/// stays at 1. `offset` is added inside the window for every type, and
/// outside it for Ramp/Spring.
double evaluateScalar(const Generator& g, double t, double clipLength);

/// Deterministic 1-D Perlin fBm in roughly [-1, 1].
double noise1D(double x, quint32 seed, int octaves);

/// Unit-response of the damped spring from 1 → 0 (x(0)=1, x'(0)=0).
double springResponse(double t, double omega, double zeta);

/// A smooth curve through anchors: Catmull-Rom tangents expressed as cubic
/// Bézier segments, with an arc-length table for constant speed.
class PathSampler {
public:
    PathSampler() = default;
    PathSampler(const std::vector<Ogre::Vector3>& anchors, bool closed);

    bool empty() const { return m_anchors.empty(); }
    int segmentCount() const { return int(m_segments.size()); }
    double length() const { return m_length; }

    /// Position at normalized curve parameter s in [0, 1].
    Ogre::Vector3 position(double s, bool constantSpeed) const;
    /// Unit tangent at s (Vector3::ZERO for a degenerate curve).
    Ogre::Vector3 tangent(double s, bool constantSpeed) const;

    /// The four Bézier control points of every segment (for drawing).
    struct Segment { Ogre::Vector3 p0, p1, p2, p3; };
    const std::vector<Segment>& segments() const { return m_segments; }

    /// A polyline approximation (`perSegment` points per segment).
    std::vector<Ogre::Vector3> polyline(int perSegment = 16) const;

private:
    double uniformParam(double s) const; // arc-length s → segment-uniform u
    Ogre::Vector3 at(double u) const;    // u in [0, segmentCount]
    Ogre::Vector3 derivative(double u) const;

    std::vector<Ogre::Vector3> m_anchors;
    std::vector<Segment> m_segments;
    std::vector<double> m_arcU;   // sample u
    std::vector<double> m_arcLen; // cumulative length at m_arcU
    double m_length = 0.0;
};

/// Curve parameter s ∈ [0,1] for clip time `t` (loops wrap; holds 0 before the
/// window and the final value after it).
double pathParameter(const Generator& g, double t, double clipLength);

/// Orientation facing along `tangent` with +Y up (Ogre's -Z forward
/// convention: the node's local -Z points along the path). Identity for a
/// zero tangent.
Ogre::Quaternion orientationAlong(const Ogre::Vector3& tangent);

/// Sample times from `from` to `to` inclusive at `fps` (at least the two ends).
std::vector<double> sampleTimes(double from, double to, int fps);

// ---------------------------------------------------------------------------
// JSON — schema `qtmesh-anim-generators-v1`
// ---------------------------------------------------------------------------

QString schemaId();
QJsonObject toJson(const Generator& g);
bool fromJson(const QJsonObject& o, Generator* out, QString* error = nullptr);
QJsonObject toDocument(const std::vector<Generator>& gens);
bool fromDocument(const QJsonObject& doc, std::vector<Generator>* out, QString* error = nullptr);

/// Apply `key=value` parameter overrides (CLI / MCP): amplitude, frequency,
/// phase, offset, seed, noise_frequency, octaves, from, to, ease, stiffness,
/// damping, start, duration, fps, loops, closed, constant_speed, orient,
/// points ("x,y,z;x,y,z;…"), name. Unknown keys → error.
bool applyParam(Generator* g, const QString& key, const QString& value, QString* error = nullptr);

/// A fresh id ("gen_<n>") not used in `existing`.
QString uniqueId(const std::vector<Generator>& existing);

} // namespace AnimGen

#endif // ANIMGENERATORS_H
