#ifndef UV_UNWRAP_H
#define UV_UNWRAP_H

#include <QString>
#include <QJsonObject>
#include <cstdint>
#include <vector>

namespace Ogre {
    class Entity;
    class Mesh;
}

// Auto UV-unwrap pipeline backed by xatlas (issue #400, epic #397).
//
// xatlas is the same library Blender and Godot use under the hood
// (MIT, ~5k LOC). Algorithm: segment the mesh into "charts" (groups
// of connected faces with low distortion), parameterize each chart
// to 2D, and pack the charts into a single rectangular atlas.
//
// The unwrap is destructive: every vertex along a UV seam is split
// into two (or more) output verts so each chart has its own copy of
// the seam edge with its own UV. We copy every other attribute
// (normals, tangents, vertex colors, bone assignments, UVs from
// other channels) from the source vertex by xref.
//
// Bone assignments live in `SubMesh::getBoneAssignments()`, keyed by
// original vertex index. We rebuild that list against the new vertex
// IDs so skinned meshes survive the unwrap.

struct UvUnwrapOptions {
    // Output atlas dimensions. xatlas uses this as a hint when
    // `texelsPerUnit == 0` (the default), interpolating a
    // texels-per-world-unit value that approximately matches.
    int resolution = 1024;

    // Texels of padding around each chart so MIP-mapped sampling
    // doesn't bleed across chart boundaries. xatlas's default is 0
    // which is unsafe for textured rendering; 4 is the canonical
    // "safe up to MIP level 2" value.
    int padding = 4;

    // Write the unwrap into this UV channel index. Default 0
    // overwrites the primary UV; 1 keeps the original UV0 and
    // writes the unwrap into UV1 (useful for lightmap workflows).
    int channel = 0;

    // When true, the previous UV channel that we're about to
    // overwrite is preserved on a higher channel (UV{channel+1}).
    // No-op when the target channel is empty.
    bool preserveOriginalAsBackup = true;
};

struct UvUnwrapReport {
    QString  meshName;
    int      submeshCount       = 0;
    int      verticesBefore     = 0;
    int      verticesAfter      = 0;     // can exceed `before` (seam splits)
    int      trianglesProcessed = 0;
    int      atlasWidth         = 0;
    int      atlasHeight         = 0;
    int      chartCount         = 0;
    double   utilization        = 0.0;   // 0..1; xatlas's reported fill ratio
    bool     applied            = false;
    QString  error;                       // populated when `applied == false`
};

class UvUnwrap {
public:
    // Generate non-overlapping UVs for every submesh of `entity` and
    // commit them in place. Returns a report with atlas stats.
    // Idempotent w.r.t. bone assignments — they're rebuilt against
    // the new vertex IDs.
    static UvUnwrapReport unwrapEntity(Ogre::Entity* entity,
                                       const UvUnwrapOptions& opts = {});

    // Report current UV channels per submesh without mutating.
    // Used by `qtmesh uv --info`. Returns one entry per submesh.
    struct UvInfo {
        int  submeshIndex   = 0;
        int  vertexCount    = 0;
        int  triangleCount  = 0;
        int  uvChannelCount = 0;     // number of TEXCOORD streams present
        bool hasUv0         = false;
        // `coverage` is the fraction of the [0..1] UV space filled
        // by the bounding box of UV0 (rough proxy for atlas
        // utilization; not the same as xatlas's metric).
        double uv0Coverage  = 0.0;
    };
    static QList<UvInfo> infoForEntity(const Ogre::Entity* entity);

    static QJsonObject reportToJson(const UvUnwrapReport& report);
    static QString     reportToText(const UvUnwrapReport& report);

    static QJsonObject infoToJson(const QString& fileName,
                                  const QList<UvInfo>& info);
    static QString     infoToText(const QString& fileName,
                                  const QList<UvInfo>& info);
};

#endif // UV_UNWRAP_H
