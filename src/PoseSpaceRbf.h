#ifndef POSESPACERBF_H
#define POSESPACERBF_H

// Pose-space deformation by radial basis functions (epic #1095, Phase 4 — generic correctives).
//
// A corrective is a vertex offset that depends on how a few driver joints are rotated: the shoulder
// bulge when an arm goes up, the elbow crease when it bends. MetaHuman stores them as RBF solvers over
// joint-driven corrective bones; this is the same idea for ANY skinned character, with the offsets held
// as per-vertex deltas so a stylised model (or a fitted body whose proportions no longer match the
// authored data) can carry its own.
//
//   delta(q) = sum_i  w_i(q) * delta_i        w(q) = phi(q)^T * (Phi + lambda I)^-1
//
// q is the driver pose (one rotation per driver joint, relative to its bind pose), delta_i the offsets
// authored at sample pose q_i, phi a Gaussian of the geodesic distance between poses. The weights are
// solved so each sample pose reproduces its own offsets EXACTLY (w(q_i) = e_i), the rest pose is an
// implicit sample with zero offsets (so the correction fades out toward the bind pose), and between
// samples the correction blends smoothly. Pure data, Ogre-free: unit-tested without a scene.

#include <QIODevice>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <vector>

namespace PoseSpace {

using Quat = std::array<double, 4>;   // (w,x,y,z), unit length

/// Rotation angle (radians, 0..pi) between two unit quaternions — the sign-independent geodesic distance.
double angleBetween(const Quat& a, const Quat& b);

struct Sample {
    std::vector<Quat> rotations;      ///< one per driver, relative to bind
    std::vector<uint32_t> vertices;   ///< vertices this sample moves (sparse)
    std::vector<float> deltas;        ///< 3 floats per entry of `vertices`
};

class Set {
public:
    QString name;
    QStringList drivers;              ///< driver joint names, in `Sample::rotations` order
    std::vector<Sample> samples;
    double sigma = 0;                 ///< Gaussian width in radians; <= 0 derives it from the samples
    double ridge = 1e-6;              ///< Tikhonov term, keeps the solve conditioned for near-duplicate poses

    /// Distance between two driver poses: sqrt(sum of squared per-driver angles).
    static double poseDistance(const std::vector<Quat>& a, const std::vector<Quat>& b);

    /// Solve the interpolation weights. Returns false (with `*error`) for an empty/mismatched set.
    bool solve(QString* error = nullptr);
    bool solved() const { return m_solved; }

    /// Blend weight of every sample at pose `q` (size == samples.size()). The rest pose's own weight is
    /// not returned: it has no offsets, so its share is simply absent.
    std::vector<double> weights(const std::vector<Quat>& q) const;

    /// Dense offsets (3 per vertex of a mesh with `vertexCount` vertices) at pose `q`.
    std::vector<float> evaluate(const std::vector<Quat>& q, size_t vertexCount) const;

    /// Add the offsets at `q` into `positions` (3 floats per vertex) — the runtime path.
    void apply(const std::vector<Quat>& q, std::vector<float>& positions) const;

    double effectiveSigma() const { return m_sigma; }

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& o, Set* out, QString* error = nullptr);

private:
    std::vector<double> m_inverse;    ///< (n+1)x(n+1), row-major; index 0 is the rest pose
    double m_sigma = 0;
    bool m_solved = false;
};

/// Compact binary container for a collection of sets (the JSON of a MetaHuman body is ~32 MB, this is ~8 MB and
/// reads in milliseconds): "QPSD1", then per set its name, drivers, sigma, ridge and samples. Little-endian via
/// QDataStream; vertex deltas are stored as 32-bit floats. readBinary solves every set and rejects a truncated or
/// malformed stream with a reason (the vertex indices, counts and rotations are all range-checked).
bool writeBinary(QIODevice* out, const std::vector<Set>& sets, QString* error = nullptr);
bool readBinary(QIODevice* in, std::vector<Set>* sets, QString* error = nullptr);

/// The name of the Ogre/blend-shape channel that carries sample `index` of `set`: the runtime looks the poses up by it.
QString channelName(const QString& setName, size_t index);
/// True for names produced by channelName() (UI lists use it to hide these from the morph-target panel).
bool isChannelName(const QString& name);

/// Solve A x = b (dense, partial-pivot LU). Exposed for tests. Returns false when singular.
bool solveLinear(std::vector<double> A, std::vector<double>& B, size_t n, size_t rhs);

}  // namespace PoseSpace

#endif  // POSESPACERBF_H
