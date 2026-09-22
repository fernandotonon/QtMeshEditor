#ifndef LATTICEDEFORMER_H
#define LATTICEDEFORMER_H

#include <OgreVector.h>
#include <OgreAxisAlignedBox.h>

#include <QJsonObject>
#include <QString>

#include <vector>

/**
 * @brief Pure-data lattice (free-form) deformer — the Blender "Lattice
 *        modifier" core.
 *
 * A lattice is a regular `nx × ny × nz` grid of control points that boxes a
 * mesh. Every mesh vertex is expressed in the lattice's normalised
 * coordinates `(s, t, u) ∈ [0,1]³` once, at creation. Dragging control
 * points then deforms the enclosed vertices by tensor-product interpolation
 * of the (displaced) control points — Sederberg & Parry's free-form
 * deformation (SIGGRAPH 1986) generalised to Blender's three interpolation
 * modes.
 *
 * Everything here is Ogre-buffer-free (positions in, positions out), so the
 * math is unit-tested without a scene. `LatticeController` is the Ogre
 * adapter that reads an entity's vertices, drives this, and writes them back.
 *
 * **Identity at rest** is exact for every mode: with the control points on
 * their rest grid, `deform(p) == p` for every `p` (partition of unity + linear
 * precision of each 1-D basis). Points OUTSIDE the box are not extrapolated
 * (a high-degree Bernstein basis explodes past `[0,1]`); they receive the
 * displacement of the nearest boundary point instead, which is continuous
 * and matches how Blender's lattice carries vertices just past its shell.
 */
namespace Lattice {

/// Interpolation across the control grid.
enum class Interpolation {
    Linear,   ///< piecewise trilinear (C0; every cell independent — creases).
    Smooth,   ///< Catmull-Rom / cardinal (C1, local 4-point support, interpolates the points). Default.
    Bezier    ///< Bernstein FFD (global smooth influence; degree = resolution − 1).
};

QString interpolationId(Interpolation interp);             ///< "linear" | "smooth" | "bezier"
bool interpolationFromId(const QString& id, Interpolation& out);

/** The lattice: resolution, rest box, live control points. */
struct Grid {
    int nx = 2, ny = 2, nz = 2;
    Ogre::Vector3 origin = Ogre::Vector3::ZERO;   ///< rest box minimum (mesh-local).
    Ogre::Vector3 size = Ogre::Vector3::UNIT_SCALE; ///< rest box extents (every component > 0).
    Interpolation interpolation = Interpolation::Smooth;
    std::vector<Ogre::Vector3> points;            ///< live control points, mesh-local, `nx*ny*nz` entries.

    /** Build a rest lattice boxing `bounds`, grown by `padding` (fraction of
     *  the largest extent, applied per side) so boundary vertices sit strictly
     *  inside. Resolutions are clamped to [2, 16]; a flat box axis (a plane) is
     *  given a small thickness so normalisation never divides by zero. */
    static Grid fromBounds(const Ogre::AxisAlignedBox& bounds, int nx, int ny, int nz,
                           float padding = 0.02f);

    int pointCount() const { return nx * ny * nz; }
    int index(int i, int j, int k) const { return (k * ny + j) * nx + i; }
    void coordsOf(int index, int& i, int& j, int& k) const;
    bool isValid() const;

    /** Rest position of control point (i, j, k). */
    Ogre::Vector3 restPoint(int i, int j, int k) const;
    /** Snap every control point back to the rest grid. */
    void reset();
    /** true when every point is within `eps` (mesh units) of its rest position. */
    bool isAtRest(float eps = 1e-6f) const;

    /** Normalised lattice coordinates of a mesh-local point (unclamped). */
    Ogre::Vector3 toLocal(const Ogre::Vector3& p) const;

    /** Deform one mesh-local point. */
    Ogre::Vector3 deform(const Ogre::Vector3& p) const;
    /** Deform a batch of REST positions (never feed already-deformed
     *  positions back in — the deformation is a function of the rest
     *  position, which is what makes it non-accumulating and undoable). */
    void deformAll(const std::vector<Ogre::Vector3>& rest, std::vector<Ogre::Vector3>& out) const;

    QJsonObject toJson() const;
    /** Strict parse: rejects a missing/short `points` array or a bad resolution. */
    static bool fromJson(const QJsonObject& obj, Grid& out, QString* error = nullptr);
};

/// 1-D basis weights for normalised coordinate `s` over `n` control indices.
/// Exposed for tests; each returns (index, weight) pairs summing to 1.
using Basis = std::vector<std::pair<int, float>>;
Basis basisLinear(float s, int n);
Basis basisSmooth(float s, int n);
Basis basisBezier(float s, int n);

/// Schema tag written by `Grid::toJson`.
inline constexpr const char* kJsonSchema = "qtmesh-lattice-v1";

} // namespace Lattice

#endif // LATTICEDEFORMER_H
