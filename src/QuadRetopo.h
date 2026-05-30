#ifndef QUAD_RETOPO_H
#define QUAD_RETOPO_H

#include <QString>
#include <QJsonObject>
#include <QList>
#include <vector>

namespace Ogre {
    class Entity;
    class Mesh;
}

// Quad-dominant retopology (issue #401, epic #397).
//
// The issue title proposes Instant Meshes; the reality is that Instant
// Meshes ships as a research GUI app with no clean C++ library API and
// has been dormant since 2016. Its production-grade successor,
// QuadriFlow, requires Boost + Eigen + LEMON — heavyweight deps the
// project doesn't currently use.
//
// This first slice ships a native triangle-pairing backend with zero
// new dependencies. The pipeline walks every interior edge whose two
// adjacent faces are triangles and scores the merge:
//   1. Coplanarity (dot product of triangle normals)
//   2. Quad shape (deviation of interior angles from 90°)
//   3. Aspect ratio (longest-to-shortest edge of the resulting quad)
//   4. Convexity (the merge is rejected if the resulting quad is concave)
// Pairs are taken greedily in best-score-first order, each triangle
// claimed at most once. Output faces are written to
// `EditableSubMesh::faces` as 4-vert quads (paired) or 3-vert tris
// (unpaired), then committed via the existing n-gon binding
// (`qtme.faces.<i>`) so the FBX / glTF exporters round-trip quads
// cleanly and Edit Mode sees the new topology as quads.
//
// Future backends can plug in behind the `Algorithm` enum without
// breaking the API surface — mirroring `MeshDecimator::Algorithm`
// and `MeshLodController::Algorithm`. The CLI / MCP / GUI already
// expose `--algo` for those tools.
//
// Limitations of the triangle-pairing approach:
//   - Not field-aligned. Instant Meshes / QuadriFlow trace quad strips
//     along principal curvature directions; this just pairs adjacent
//     triangles based on local geometry.
//   - Cannot reach `--target-faces` exactly; the lower bound is
//     ~50% of the input triangle count (every tri paired). With
//     pickier merge scoring it lands closer to 60-70%.
//   - UVs and skin weights are preserved per-vertex (triangle-pairing
//     never introduces new vertices), which is the main practical
//     advantage over field-aligned methods.

struct QuadRetopoOptions {
    // Target face count after retopology. <= 0 means "pair every
    // mergeable pair" (typically ~50% reduction). Otherwise we stop
    // taking pairs once we hit the target.
    int targetFaces = -1;

    // Maximum angle (in degrees) between two adjacent triangle normals
    // for them to be considered for pairing. Lower = more conservative
    // (stricter coplanarity required); higher = more aggressive.
    // 30° preserves curvature features well; 90° pairs almost
    // everything. Default 25°.
    double maxAngleDeg = 25.0;

    // Minimum allowed deviation from a perfect square. Each interior
    // angle of the candidate quad must be within
    // [90 - shapeToleranceDeg, 90 + shapeToleranceDeg] degrees.
    // 60° accepts most reasonable quads; 30° is strict (near-square).
    // Default 65°.
    double shapeToleranceDeg = 65.0;

    // Maximum aspect ratio (longest edge / shortest edge) of the
    // resulting quad. >= 1.0. 4.0 is permissive; 2.0 is strict.
    // Default 6.0 — most character meshes benefit from accepting
    // some elongated quads along limbs.
    double maxAspectRatio = 6.0;
};

struct QuadRetopoSubmeshReport {
    int submeshIndex     = 0;
    int trianglesBefore  = 0;
    int facesAfter       = 0;     // triangles + quads after retopology
    int quadsAfter       = 0;
    int trianglesAfter   = 0;
};

struct QuadRetopoReport {
    QString meshName;
    QList<QuadRetopoSubmeshReport> submeshes;
    int totalTrianglesBefore = 0;
    int totalFacesAfter      = 0;
    int totalQuadsAfter      = 0;
    int totalTrianglesAfterRetopo = 0;
    bool applied             = false;
    QString error;

    /// Fraction of input triangles that got paired into quads.
    /// 0.0 = no pairs found (mesh is preserved as-is).
    /// 1.0 = every triangle paired (face count halved).
    double quadDominance() const
    {
        return totalTrianglesBefore > 0
            ? static_cast<double>(totalQuadsAfter * 2) / totalTrianglesBefore
            : 0.0;
    }
};

class QuadRetopo {
public:
    enum class Algorithm {
        TrianglePair,   // Default — pure-data triangle pairing into quads.
        // Future:
        //   QuadriFlow      — field-aligned via QuadriFlow library
        //   InstantMeshes   — if/when a clean library extraction exists
    };

    // Apply quad retopology to `entity` in place. The base mesh is
    // rewritten: triangle pairs become quads on `EditableSubMesh::
    // faces`, the triangle list is rebuilt by fan-triangulating the
    // n-gon list, and the `qtme.faces.<i>` binding is updated so
    // exporters and Edit Mode see the new topology.
    //
    // `algo` selects the backend (only `TrianglePair` is implemented
    // in this slice).
    static QuadRetopoReport retopologize(Ogre::Entity* entity,
                                         const QuadRetopoOptions& opts = {},
                                         Algorithm algo = Algorithm::TrianglePair);

    // Pure-data variant: in-memory triangle list → quads-plus-tris
    // face list. Used by the Ogre-backed `retopologize` and by tests
    // that don't want an Ogre context. Positions are passed as a
    // flat float array (xyz xyz ...) of length 3 * vertexCount.
    //
    // The output `outFaces` is one inner vector per face; each inner
    // vector holds 3 (triangle) or 4 (quad) source-vertex indices.
    // Vertex IDs in the output reference the same `positions` array
    // unchanged — triangle pairing never introduces new vertices.
    static QuadRetopoReport retopologizeMesh(const float* positions,
                                             int vertexCount,
                                             const unsigned int* indices,
                                             int triangleCount,
                                             const QuadRetopoOptions& opts,
                                             std::vector<std::vector<unsigned int>>& outFaces);

    static QJsonObject reportToJson(const QuadRetopoReport& report);
    static QString     reportToText(const QuadRetopoReport& report);

    static QString algorithmToString(Algorithm algo);
    static Algorithm algorithmFromString(const QString& s);
};

#endif // QUAD_RETOPO_H
