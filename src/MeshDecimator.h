#ifndef MESHDECIMATOR_H
#define MESHDECIMATOR_H

#include <QString>
#include <QList>
#include <QJsonObject>

namespace Ogre {
    class Entity;
    class MeshLodGenerator;
}

// Per-submesh decimation report — how many triangles each submesh had before
// and after. Slot 0 is the entire base mesh (sum across submeshes).
struct DecimationSubmeshReport {
    QString meshName;
    int submeshIndex = 0;
    int trianglesBefore = 0;
    int trianglesAfter = 0;
};

struct DecimationReport {
    QString meshName;
    QList<DecimationSubmeshReport> submeshes;
    int totalTrianglesBefore = 0;
    int totalTrianglesAfter = 0;
    double appliedReduction = 0.0; // 0..1, the reduction fraction we asked Ogre for
    bool applied = false;           // true when the decimation was committed in place

    double effectiveReduction() const {
        return totalTrianglesBefore > 0
            ? 1.0 - static_cast<double>(totalTrianglesAfter) / totalTrianglesBefore
            : 0.0;
    }
};

// Pure-data + Ogre-backed mesh decimator.  Unlike MeshLodController which
// generates a *chain* of discrete LOD levels (each at increasing distance),
// MeshDecimator runs a single-pass reduction that rewrites the base mesh
// itself — appropriate for "I want this asset to be 3000 triangles" rather
// than "I want LOD 0/1/2/3 for distance-based rendering".
//
// The current backend is Ogre's MeshLodGenerator with a single LOD config.
// A future slice can replace it with a QEM implementation that supports
// per-vertex lock weights and UV/material seam preservation. The pure-data
// helpers (decimationFromTris / decimationFromVerts) work without Ogre, so
// they're testable in any environment.
class MeshDecimator {
public:
    // Sentinel — slider / API caps at 95% to avoid degenerate single-triangle
    // outputs that some downstream paths (Ogre exporter, FBX) trip over.
    static constexpr double kMaxReduction = 0.95;

    // Convert a "target tri count" to a reduction fraction (0..1). When
    // `targetTris` is >= `currentTris`, returns 0.0 (no reduction).
    static double reductionFromTargetTris(int currentTris, int targetTris);

    // Convert a "target vertex budget" to a reduction fraction. Same math
    // — assumes uniform reduction across the mesh.
    static double reductionFromTargetVerts(int currentVerts, int targetVerts);

    // Clamp a reduction fraction into [0.0, kMaxReduction].
    static double clampReduction(double r);

    // Apply a single-pass decimation in place on the entity's mesh. Wipes
    // any existing LOD levels (the base mesh is the thing being reduced).
    // Returns the report. `applied` flips to true on success.
    static DecimationReport decimateEntity(Ogre::Entity* entity, double reduction);

    // Analyze-only: same arithmetic as decimateEntity but does not mutate
    // the mesh. Returns the report with `applied = false` and predicted
    // after-counts proportional to the current submesh distribution.
    // Cheap — does not call into MeshLodGenerator.
    static DecimationReport projectEntity(const Ogre::Entity* entity, double reduction);

    // Count base-mesh triangles and vertices across all submeshes of an
    // entity. Used to resolve target-tris / target-verts targets into a
    // reduction fraction in both the CLI and MCP wrappers.
    static void countBaseline(const Ogre::Entity* entity,
                              int& outTris, int& outVerts);

    static QJsonObject toJson(const DecimationReport& report);
    static QString toText(const DecimationReport& report);
};

#endif // MESHDECIMATOR_H
