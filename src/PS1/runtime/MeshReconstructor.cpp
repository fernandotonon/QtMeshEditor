#include "MeshReconstructor.h"

#include "GteCapture.h"
#include "GteInverse.h"
#include "MeshTopologyHash.h"
#include "PsxCaptureFilters.h"

#include <OgreColourValue.h>

#include <QHash>
#include <QPair>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

// Tier 2 only (#816): screen-space GTE-inverse results still go through the
// fixed radius gate. Tier 0/1 vertices are trusted per-vertex and outliers
// are culled by the per-part policy in applyPartOutlierPolicy instead.
constexpr float kMaxVertexRadius = 64.0f;

// Per-part outlier policy (#816): a Tier 0/1 vertex is dropped when it sits
// farther than kOutlierRadiusFactor × the part's 99th-percentile radius from
// the part centroid. Catches stale-ring garbage without clamping legitimate
// large models the way the fixed kMaxVertexRadius gate did.
constexpr float kOutlierRadiusFactor = 8.0f;

/** Which reconstruction tier placed a vertex (#816). */
enum class VertexTier : uint8_t {
    Screen = 0, // Tier 2: legacy screen-space inverse / psxScreenToWorld fallback
    Depth,      // Tier 1: PGXP subpixel screen coords + view depth inverted
    Tracked,    // Tier 0: exact object-space vertex from a resolved GTE record
};

uint32_t packDiffuse(uint8_t r, uint8_t g, uint8_t b)
{
    const Ogre::ColourValue cv(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    return cv.getAsBYTE();
}

/** Translates an in-core GTE record's transform block into the MatrixRecord
 *  layout shared with the RAM-scan path (#816): row-major m[r][c] = rt[r*3+c],
 *  hash via GteCapture so tracked prims group exactly like scanned matrices. */
MatrixRecord matrixRecordFromGteRecord(const GteRecordEntry &rec)
{
    MatrixRecord m{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m.rt.m[r][c] = rec.rt[r * 3 + c];
    for (int i = 0; i < 3; ++i)
        m.tr[i] = rec.tr[i];
    m.ofx = rec.ofx;
    m.ofy = rec.ofy;
    m.h = rec.h;
    m.hash = GteCapture::hashMatrix(m);
    return m;
}

struct SubMeshAccumulator {
    QString materialName;
    QVector<ReconstructedVertex> vertices;
    QVector<uint32_t> indices;
    /** Vertex indices that legacy stats folded into the bounds AABB — every
     *  emitted tri vertex, but only the two captured sprite corners (the
     *  synthesized +0.05 py copies were never counted pre-#816 and still
     *  aren't). Folded after the outlier pass so dropped garbage can't
     *  poison the slab-metric canary. Only maintained when stats are on. */
    QVector<uint32_t> boundsVertexIndices;
    /** True when any vertex landed via Tier 0/1 — gates the outlier pass so
     *  pure Tier-2 parts keep the pre-#816 constant-radius behavior. */
    bool hasTieredVertices = false;

    void addTriangle(const ReconstructedVertex &a, const ReconstructedVertex &b, const ReconstructedVertex &c)
    {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.append(a);
        vertices.append(b);
        vertices.append(c);
        indices.append(base);
        indices.append(base + 1);
        indices.append(base + 2);
    }
};

/** Tiered per-vertex placement (#816). Tier 0: resolved GTE record → exact
 *  object space. Tier 1: PGXP precise XY + view depth through the float
 *  screenToModel (record matrix when resolvable, else the prim's). Tier 2:
 *  pre-#816 behavior verbatim, including the constant radius gate. A prim
 *  may mix tiers — each vertex degrades independently. */
ReconstructedVertex vertexFromPsx(const PsxVertex &v, const MatrixRecord *matrix,
                                  const QVector<GteRecordEntry> &gteRecords, bool textured,
                                  bool *usedGteInverseOut = nullptr, VertexTier *tierOut = nullptr,
                                  const MatrixRecord *primTrackedMatrix = nullptr)
{
    ReconstructedVertex out;
    out.diffuseArgb = packDiffuse(v.r, v.g, v.b);

    bool usedGte = false;
    VertexTier tier = VertexTier::Screen;
    bool placed = false;

    const bool recordResolves =
        v.gteRecordIndex < static_cast<uint32_t>(gteRecords.size());

    if (v.provenance == static_cast<uint8_t>(PsxVertexProvenance::GteTracked) && recordResolves) {
        // Tier 0 — the record carries the raw GTE V register (s16 model units).
        const GteRecordEntry &rec = gteRecords[static_cast<int>(v.gteRecordIndex)];
        GteInverse::modelToEditor(static_cast<float>(rec.vx), static_cast<float>(rec.vy),
                                  static_cast<float>(rec.vz), out.px, out.py, out.pz);
        tier = VertexTier::Tracked;
        placed = true;
    }

    if (!placed && v.provenance != static_cast<uint8_t>(PsxVertexProvenance::None)) {
        // Tier 1 — DepthOnly, or a tracked vertex whose ring index went stale.
        // viewW == 0 means "depth unknown" and screenToModel refuses it, so the
        // vertex degrades to Tier 2 below.
        //
        // #816 spike fix: prefer the prim's canonical tracked matrix so every
        // Tier-1 vertex of a partly-tracked prim inverts into the SAME model
        // space as its Tier-0 corners. Without this, a triangle mixing a
        // tracked corner (raw object space) with a depth corner (inverted
        // against a different/own matrix) stretched between two spaces and
        // radiated the spike artifact. Fall back to this vertex's own record
        // matrix, then the draw matrix, only when the prim supplied none.
        MatrixRecord recordMatrix;
        const MatrixRecord *drawMatrix = primTrackedMatrix ? primTrackedMatrix : matrix;
        if (!primTrackedMatrix && recordResolves) {
            recordMatrix = matrixRecordFromGteRecord(gteRecords[static_cast<int>(v.gteRecordIndex)]);
            drawMatrix = &recordMatrix;
        }
        float mx = 0.0f;
        float my = 0.0f;
        float mz = 0.0f;
        if (drawMatrix
            && GteInverse::screenToModel(*drawMatrix, v.preciseX, v.preciseY, v.viewW, mx, my, mz)) {
            GteInverse::modelToEditor(mx, my, mz, out.px, out.py, out.pz);
            tier = VertexTier::Depth;
            usedGte = true;
            placed = true;
        }
    }

    if (!placed) {
        // Tier 2 — pre-#816 behavior verbatim for RAM-scan captures and
        // degraded Tier 0/1 vertices.
        float mx = 0.0f;
        float my = 0.0f;
        float mz = 0.0f;
        if (matrix && GteInverse::screenToModel(*matrix, v.x, v.y, v.z, mx, my, mz)) {
            float wx = 0.0f;
            float wy = 0.0f;
            float wz = 0.0f;
            GteInverse::modelToEditor(mx, my, mz, wx, wy, wz);
            const float radius = std::sqrt(wx * wx + wy * wy + wz * wz);
            if (std::isfinite(radius) && radius <= kMaxVertexRadius) {
                out.px = wx;
                out.py = wy;
                out.pz = wz;
                usedGte = true;
            } else {
                GteInverse::psxScreenToWorld(static_cast<float>(v.x), static_cast<float>(v.y),
                                             static_cast<float>(v.z), out.px, out.py, out.pz);
            }
        } else {
            GteInverse::psxScreenToWorld(static_cast<float>(v.x), static_cast<float>(v.y),
                                         static_cast<float>(v.z), out.px, out.py, out.pz);
        }
    }
    if (usedGteInverseOut)
        *usedGteInverseOut = usedGte;
    if (tierOut)
        *tierOut = tier;

    if (textured) {
        out.u = static_cast<float>(v.u) / 256.0f;
        out.v = static_cast<float>(v.v) / 256.0f;
    }
    return out;
}

/** Expand `stats`' AABB to include `v`. First call initialises the bounds to the
 *  vertex (we can't anchor at 0 — meshes that live entirely on one side of the
 *  origin would lose their min or max). Shared by every code path that needs to
 *  fold a vertex into stats so the behavior stays in lock-step (#674 review).
 *  Seeding is tracked explicitly (#816): gating on hasBounds() re-initialised
 *  the AABB on every fold — a freshly-seeded min==max box never passes the
 *  non-zero-extent check — so bounds degenerated to the last folded vertex. */
void expandBounds(MeshReconstructionStats &stats, const ReconstructedVertex &v)
{
    if (!stats.boundsSeeded) {
        stats.boundsSeeded = true;
        stats.boundsMinX = stats.boundsMaxX = v.px;
        stats.boundsMinY = stats.boundsMaxY = v.py;
        stats.boundsMinZ = stats.boundsMaxZ = v.pz;
        return;
    }
    stats.boundsMinX = std::min(stats.boundsMinX, v.px);
    stats.boundsMaxX = std::max(stats.boundsMaxX, v.px);
    stats.boundsMinY = std::min(stats.boundsMinY, v.py);
    stats.boundsMaxY = std::max(stats.boundsMaxY, v.py);
    stats.boundsMinZ = std::min(stats.boundsMinZ, v.pz);
    stats.boundsMaxZ = std::max(stats.boundsMaxZ, v.pz);
}

/** Counts only — bounds are folded after the per-part outlier pass (#816),
 *  via SubMeshAccumulator::boundsVertexIndices, so dropped vertices never
 *  reach the AABB. Min/max folding is order-independent, so legacy captures
 *  (no drops) produce bit-identical bounds. */
void accumulateVertexStats(MeshReconstructionStats &stats, bool usedGte, VertexTier tier)
{
    ++stats.totalVertices;
    if (tier == VertexTier::Tracked)
        ++stats.gteTrackedVertices;
    else if (usedGte)
        ++stats.gteInverseVertices;
    else
        ++stats.screenFallbackVertices;
    if (tier == VertexTier::Depth)
        ++stats.depthOnlyVertices;
}

PsxVertex midpointPsx(const PsxVertex &a, const PsxVertex &b)
{
    // Integer division on screen coords is fine: even at the coarsest PS1
    // resolution (640px-wide modes) a 1-pixel rounding error along an edge
    // is well below the perspective-correct UV subdivision's own tolerance
    // (1.3 default depth ratio). UV is i16/256 fixed-point so the same
    // integer halving preserves screen-space affine interpolation exactly.
    PsxVertex m;
    m.x = (a.x + b.x) / 2;
    m.y = (a.y + b.y) / 2;
    m.z = (a.z + b.z) / 2;
    m.u = static_cast<int16_t>((static_cast<int>(a.u) + static_cast<int>(b.u)) / 2);
    m.v = static_cast<int16_t>((static_cast<int>(a.v) + static_cast<int>(b.v)) / 2);
    m.r = static_cast<uint8_t>((static_cast<int>(a.r) + static_cast<int>(b.r)) / 2);
    m.g = static_cast<uint8_t>((static_cast<int>(a.g) + static_cast<int>(b.g)) / 2);
    m.b = static_cast<uint8_t>((static_cast<int>(a.b) + static_cast<int>(b.b)) / 2);
    // #816: a midpoint of two tiered parents keeps Tier 1 eligibility so a
    // subdivided tracked/depth prim stays in model space instead of mixing
    // model-space corners with screen-space midpoints. There is no midpoint
    // GTE record, so the midpoint is at best DepthOnly; both parents must
    // carry a valid depth for the interpolated depth to mean anything.
    const auto kNone = static_cast<uint8_t>(PsxVertexProvenance::None);
    if (a.provenance != kNone && b.provenance != kNone && a.viewW != 0.0f && b.viewW != 0.0f) {
        m.preciseX = (a.preciseX + b.preciseX) * 0.5f;
        m.preciseY = (a.preciseY + b.preciseY) * 0.5f;
        m.viewW = (a.viewW + b.viewW) * 0.5f;
        m.provenance = static_cast<uint8_t>(PsxVertexProvenance::DepthOnly);
    }
    return m;
}

/** True when the triangle's per-vertex screen-space depth (sz) varies enough
 *  that the perspective-vs-affine UV gap is visibly larger than `tolerance`.
 *  Falls back to "don't subdivide" when any sz is zero (GP0-only captures
 *  have no depth — see GteInverse::screenToModel sz==0 guard, #675). */
bool depthRatioExceedsTolerance(const PsxVertex &a, const PsxVertex &b, const PsxVertex &c,
                                float tolerance)
{
    const int zs[3] = { a.z, b.z, c.z };
    int zmin = zs[0], zmax = zs[0];
    for (int i = 1; i < 3; ++i) {
        zmin = std::min(zmin, zs[i]);
        zmax = std::max(zmax, zs[i]);
    }
    if (zmin <= 0)
        return false;
    return static_cast<float>(zmax) / static_cast<float>(zmin) > tolerance;
}

void emitTriDirect(const PsxVertex &a, const PsxVertex &b, const PsxVertex &c,
                   const MatrixRecord *matrix, const QVector<GteRecordEntry> &gteRecords,
                   bool textured, SubMeshAccumulator &acc, MeshReconstructionStats *statsOut,
                   const MatrixRecord *primTrackedMatrix)
{
    auto vtx = [&](const PsxVertex &pv) {
        bool usedGte = false;
        VertexTier tier = VertexTier::Screen;
        ReconstructedVertex out =
            vertexFromPsx(pv, matrix, gteRecords, textured, &usedGte, &tier, primTrackedMatrix);
        if (tier != VertexTier::Screen)
            acc.hasTieredVertices = true;
        if (statsOut)
            accumulateVertexStats(*statsOut, usedGte, tier);
        return out;
    };
    const uint32_t base = static_cast<uint32_t>(acc.vertices.size());
    acc.addTriangle(vtx(a), vtx(b), vtx(c));
    if (statsOut)
        acc.boundsVertexIndices << base << (base + 1) << (base + 2);
}

/** Recursive midpoint subdivision: when the triangle's depth ratio exceeds
 *  `tolerance`, split into 4 sub-tris via edge midpoints and recurse.
 *  New midpoint vertices' UVs are computed via screen-space linear interp
 *  (the PS1 affine convention) so Ogre's perspective-correct rendering of
 *  the resulting fine mesh approximates what the original PS1 GPU showed.
 *  Bounded by `maxDepth` so a single very-warped prim can't blow up to
 *  thousands of tris (4^3 = 64 sub-tris at the default depth=3). */
void emitTriSubdivided(const PsxVertex &a, const PsxVertex &b, const PsxVertex &c,
                       const MatrixRecord *matrix, const QVector<GteRecordEntry> &gteRecords,
                       bool textured, SubMeshAccumulator &acc,
                       const MatrixRecord *primTrackedMatrix,
                       MeshReconstructionStats *statsOut, float tolerance, int remainingDepth)
{
    if (remainingDepth <= 0 || !depthRatioExceedsTolerance(a, b, c, tolerance)) {
        emitTriDirect(a, b, c, matrix, gteRecords, textured, acc, statsOut, primTrackedMatrix);
        return;
    }
    const PsxVertex ab = midpointPsx(a, b);
    const PsxVertex bc = midpointPsx(b, c);
    const PsxVertex ca = midpointPsx(c, a);
    const int next = remainingDepth - 1;
    emitTriSubdivided(a,  ab, ca, matrix, gteRecords, textured, acc, primTrackedMatrix, statsOut, tolerance, next);
    emitTriSubdivided(ab, b,  bc, matrix, gteRecords, textured, acc, primTrackedMatrix, statsOut, tolerance, next);
    emitTriSubdivided(ca, bc, c,  matrix, gteRecords, textured, acc, primTrackedMatrix, statsOut, tolerance, next);
    emitTriSubdivided(ab, bc, ca, matrix, gteRecords, textured, acc, primTrackedMatrix, statsOut, tolerance, next);
}

void emitTri(const PsxVertex &a, const PsxVertex &b, const PsxVertex &c,
             const MatrixRecord *matrix, const QVector<GteRecordEntry> &gteRecords, bool textured,
             SubMeshAccumulator &acc, MeshReconstructionStats *statsOut,
             const Ps1NormalizerSettings &settings, const MatrixRecord *primTrackedMatrix)
{
    // Perspective-correct subdivision exists to reproduce PS1 affine UV
    // sampling at fine grain. There's no UV channel on mono / shaded prims
    // (HUDs, flat-shaded geometry), so tessellating them would just inflate
    // triangle counts without changing the visual — skip them.
    if (textured && settings.perspectiveCorrectUVs && settings.perspectiveMaxDepth > 0) {
        emitTriSubdivided(a, b, c, matrix, gteRecords, textured, acc, primTrackedMatrix,
                          statsOut, settings.perspectiveTolerance, settings.perspectiveMaxDepth);
        return;
    }
    emitTriDirect(a, b, c, matrix, gteRecords, textured, acc, statsOut, primTrackedMatrix);
}

/** "Clean up" filter (#816 follow-up): true when every vertex the prim will
 *  emit lands via the in-core GTE path (Tier 0 GteTracked or Tier 1 DepthOnly)
 *  rather than the Tier 2 screen-space fallback. Mirrors vertexFromPsx's
 *  tier decision from the raw PsxVertex so we can reject before reconstructing.
 *  Screen-fallback prims are the HUD / sprite / 2D overlay junk that clutters
 *  the whole-frame draw list. */
bool primIsTrackedGeometry(const PrimRecord &prim, const QVector<GteRecordEntry> &gteRecords)
{
    const int n = std::min<int>(prim.vertexCount, 4);
    for (int i = 0; i < n; ++i) {
        const PsxVertex &v = prim.verts[i];
        const bool tracked =
            v.provenance == static_cast<uint8_t>(PsxVertexProvenance::GteTracked)
            && v.gteRecordIndex < static_cast<uint32_t>(gteRecords.size());
        const bool depth = v.provenance != static_cast<uint8_t>(PsxVertexProvenance::None)
                           && v.viewW != 0.0f;
        if (!tracked && !depth)
            return false;
    }
    return true;
}

void emitPrimitive(const PrimRecord &prim, const MatrixRecord *matrix,
                   const QVector<GteRecordEntry> &gteRecords, SubMeshAccumulator &acc,
                   MeshReconstructionStats *statsOut, const Ps1NormalizerSettings &settings,
                   const MatrixRecord *groupMatrix = nullptr)
{
    // Clean-up filter: drop screen-space-fallback prims entirely when the user
    // asked to keep only tracked geometry. Guarded by the caller so an
    // all-None (RAM-scan) capture doesn't filter to empty.
    if (settings.trackedGeometryOnly && !primIsTrackedGeometry(prim, gteRecords))
        return;

    // #816 spike fix: every Tier-1 (DepthOnly) vertex must invert against ONE
    // canonical matrix so the whole prim — and the whole group it belongs to —
    // lands in a single coherent model space. Priority:
    //   1. this prim's own tracked vertex's record matrix (in-prim mixing), then
    //   2. the group's canonical matrix (cross-prim: pure-DepthOnly prims of an
    //      object share the group matrix instead of each inverting alone).
    // Without (2), a busy scene's ~90% DepthOnly prims each used their own
    // per-draw matrix and depth prims of one object spanned/spiked across
    // slightly different spaces.
    MatrixRecord primMatrixStorage;
    const MatrixRecord *primTrackedMatrix = nullptr;
    const int nvChk = std::min<int>(prim.vertexCount, 4);
    for (int i = 0; i < nvChk; ++i) {
        const PsxVertex &pv = prim.verts[i];
        if (pv.provenance == static_cast<uint8_t>(PsxVertexProvenance::GteTracked)
            && pv.gteRecordIndex < static_cast<uint32_t>(gteRecords.size())) {
            primMatrixStorage =
                matrixRecordFromGteRecord(gteRecords[static_cast<int>(pv.gteRecordIndex)]);
            primTrackedMatrix = &primMatrixStorage;
            break;
        }
    }
    if (!primTrackedMatrix)
        primTrackedMatrix = groupMatrix;

    const bool textured = prim.kind == PrimKind::TexturedTri || prim.kind == PrimKind::TexturedQuad
                          || prim.kind == PrimKind::Sprite;

    if (prim.kind == PrimKind::MonoTri || prim.kind == PrimKind::ShadedTri
        || prim.kind == PrimKind::TexturedTri) {
        if (prim.vertexCount >= 3)
            emitTri(prim.verts[0], prim.verts[1], prim.verts[2], matrix, gteRecords, textured,
                    acc, statsOut, settings, primTrackedMatrix);
        return;
    }

    if (prim.kind == PrimKind::MonoQuad || prim.kind == PrimKind::ShadedQuad
        || prim.kind == PrimKind::TexturedQuad) {
        if (prim.vertexCount >= 4) {
            emitTri(prim.verts[0], prim.verts[1], prim.verts[2], matrix, gteRecords, textured,
                    acc, statsOut, settings, primTrackedMatrix);
            emitTri(prim.verts[0], prim.verts[2], prim.verts[3], matrix, gteRecords, textured,
                    acc, statsOut, settings, primTrackedMatrix);
        }
        return;
    }

    if (prim.kind == PrimKind::Sprite && prim.vertexCount >= 2) {
        // Sprites are screen-aligned billboards (no depth variance across the
        // pair), so subdivision is a no-op for them — we keep the original
        // 2-tri expansion. The pinned 0.05f py offset stays because the
        // captured pair lacks the second pair of corners.
        auto vtx = [&](int i) {
            bool usedGte = false;
            VertexTier tier = VertexTier::Screen;
            ReconstructedVertex out =
                vertexFromPsx(prim.verts[i], matrix, gteRecords, textured, &usedGte, &tier,
                              primTrackedMatrix);
            if (tier != VertexTier::Screen)
                acc.hasTieredVertices = true;
            if (statsOut)
                accumulateVertexStats(*statsOut, usedGte, tier);
            return out;
        };
        const uint32_t base = static_cast<uint32_t>(acc.vertices.size());
        ReconstructedVertex a = vtx(0);
        ReconstructedVertex b = vtx(1);
        ReconstructedVertex c = b;
        ReconstructedVertex d = a;
        c.py = b.py + 0.05f;
        d.py = a.py + 0.05f;
        acc.addTriangle(a, b, c);
        acc.addTriangle(a, c, d);
        // Legacy stats only folded the two captured corners into bounds — the
        // synthesized +0.05 copies stay excluded (#816 keeps parity).
        if (statsOut)
            acc.boundsVertexIndices << base << (base + 1);
    }
}

/** Group identity for one part (#816): `first == 0` → legacy matrixId group
 *  (`second` = prim.matrixId), `first == 1` → tracked GTE group (`second` =
 *  GteCapture::hashMatrix of the resolved record's rt/tr block). The
 *  discriminator keeps small matrixIds from ever colliding with hashes. */
using PartGroupKey = QPair<quint64, quint64>;

/** Per-prim matrix resolution (#816): majority vote across the prim's
 *  tracked vertices' resolved GTE records. Multi-matrix prims (skinned)
 *  take the majority and are counted in `mixedMatrixPrims`. */
struct PrimMatrixResolution {
    PartGroupKey key;
    int recordIndex = -1; // majority record (into snapshot.gteRecords), or -1
};

PrimMatrixResolution resolvePrimMatrix(const PrimRecord &prim,
                                       const QVector<GteRecordEntry> &gteRecords,
                                       MeshReconstructionStats *statsOut)
{
    struct Candidate {
        quint64 hash = 0;
        int recordIndex = -1;
        int votes = 0;
    };
    Candidate candidates[4];
    int candidateCount = 0;

    const int vertCount = std::min<int>(prim.vertexCount, 4);
    for (int i = 0; i < vertCount; ++i) {
        const PsxVertex &v = prim.verts[i];
        if (v.provenance != static_cast<uint8_t>(PsxVertexProvenance::GteTracked))
            continue;
        if (v.gteRecordIndex >= static_cast<uint32_t>(gteRecords.size()))
            continue;
        const quint64 hash =
            matrixRecordFromGteRecord(gteRecords[static_cast<int>(v.gteRecordIndex)]).hash;
        bool found = false;
        for (int c = 0; c < candidateCount; ++c) {
            if (candidates[c].hash == hash) {
                ++candidates[c].votes;
                found = true;
                break;
            }
        }
        if (!found)
            candidates[candidateCount++] = {hash, static_cast<int>(v.gteRecordIndex), 1};
    }

    if (candidateCount == 0)
        return {PartGroupKey(quint64(0), quint64(prim.matrixId)), -1};

    int best = 0;
    for (int c = 1; c < candidateCount; ++c) {
        if (candidates[c].votes > candidates[best].votes)
            best = c;
    }
    if (candidateCount > 1 && statsOut)
        ++statsOut->mixedMatrixPrims;
    return {PartGroupKey(quint64(1), candidates[best].hash), candidates[best].recordIndex};
}

/** One part-in-progress: the per-texture accumulators plus the group's
 *  resolved GTE matrix when the group was keyed by a tracked record (#816). */
struct GroupBucket {
    QHash<quint64, SubMeshAccumulator> byTexKey;
    MatrixRecord trackedMatrix{};
    bool hasTrackedMatrix = false;

    bool hasTieredVertices() const
    {
        for (auto it = byTexKey.constBegin(); it != byTexKey.constEnd(); ++it) {
            if (it.value().hasTieredVertices)
                return true;
        }
        return false;
    }
};

struct MatrixGroupsResult {
    QHash<PartGroupKey, GroupBucket> groups;
    /** Parallel to snapshot.prims — the group key each prim resolved to,
     *  threaded through to provenance resolution so the mapping is computed
     *  exactly once (#816 review requirement). */
    QVector<PartGroupKey> primGroupKeys;
};

/** Degenerate-triangle cull (#816 spike follow-up): drop any triangle whose
 *  longest edge exceeds `factor` × the submesh's median edge length. A spanning
 *  spike triangle (a corner in the wrong model space) has one runaway edge, so
 *  this removes the visible artifact directly. Runs per-submesh over the part's
 *  accumulators; compacts vertices afterward. No-op when factor <= 0. */
void applyDegenerateTriangleCull(GroupBucket &bucket, float factor,
                                 MeshReconstructionStats *statsOut)
{
    if (!(factor > 0.0f))
        return;

    auto edgeLen = [](const ReconstructedVertex &a, const ReconstructedVertex &b) {
        const double dx = a.px - b.px, dy = a.py - b.py, dz = a.pz - b.pz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };

    for (auto it = bucket.byTexKey.begin(); it != bucket.byTexKey.end(); ++it) {
        SubMeshAccumulator &acc = it.value();
        const int triCount = acc.indices.size() / 3;
        if (triCount < 4) // too few triangles for a meaningful median
            continue;

        // Longest edge per triangle + the median across the submesh.
        QVector<double> longest;
        longest.reserve(triCount);
        for (int t = 0; t + 2 < acc.indices.size(); t += 3) {
            const ReconstructedVertex &v0 = acc.vertices[static_cast<int>(acc.indices[t])];
            const ReconstructedVertex &v1 = acc.vertices[static_cast<int>(acc.indices[t + 1])];
            const ReconstructedVertex &v2 = acc.vertices[static_cast<int>(acc.indices[t + 2])];
            longest.append(std::max({edgeLen(v0, v1), edgeLen(v1, v2), edgeLen(v2, v0)}));
        }
        QVector<double> sortedLen = longest;
        std::sort(sortedLen.begin(), sortedLen.end());
        const double median = sortedLen[sortedLen.size() / 2];
        if (!(median > 0.0) || !std::isfinite(median))
            continue;
        const double threshold = static_cast<double>(factor) * median;

        // Keep triangles under threshold; compact referenced vertices.
        QVector<int> remap(acc.vertices.size(), -1);
        QVector<ReconstructedVertex> newVerts;
        QVector<uint32_t> newIdx;
        newVerts.reserve(acc.vertices.size());
        newIdx.reserve(acc.indices.size());
        int dropped = 0;
        for (int t = 0, ti = 0; t + 2 < acc.indices.size(); t += 3, ++ti) {
            if (longest[ti] > threshold) {
                ++dropped;
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                const uint32_t src = acc.indices[t + k];
                if (remap[static_cast<int>(src)] < 0) {
                    remap[static_cast<int>(src)] = newVerts.size();
                    newVerts.append(acc.vertices[static_cast<int>(src)]);
                }
                newIdx.append(static_cast<uint32_t>(remap[static_cast<int>(src)]));
            }
        }
        if (dropped == 0)
            continue;
        acc.vertices = newVerts;
        acc.indices = newIdx;
        QVector<uint32_t> newBounds;
        newBounds.reserve(acc.boundsVertexIndices.size());
        for (const uint32_t idx : acc.boundsVertexIndices) {
            const int mapped = remap[static_cast<int>(idx)];
            if (mapped >= 0)
                newBounds.append(static_cast<uint32_t>(mapped));
        }
        acc.boundsVertexIndices = newBounds;
        if (statsOut)
            statsOut->outlierDroppedVertices += dropped * 3;
    }
}

/** Per-part outlier policy for tiered parts (#816): vertices farther than
 *  kOutlierRadiusFactor × the part's 99th-percentile centroid radius are
 *  dropped along with every triangle that references them. Replaces the
 *  fixed kMaxVertexRadius gate for Tier 0/1 vertices. */
void applyPartOutlierPolicy(GroupBucket &bucket, MeshReconstructionStats *statsOut)
{
    double cx = 0.0;
    double cy = 0.0;
    double cz = 0.0;
    qint64 vertexTotal = 0;
    for (auto it = bucket.byTexKey.constBegin(); it != bucket.byTexKey.constEnd(); ++it) {
        for (const ReconstructedVertex &v : it.value().vertices) {
            cx += v.px;
            cy += v.py;
            cz += v.pz;
        }
        vertexTotal += it.value().vertices.size();
    }
    if (vertexTotal < 4) // too few samples for a meaningful percentile
        return;
    cx /= static_cast<double>(vertexTotal);
    cy /= static_cast<double>(vertexTotal);
    cz /= static_cast<double>(vertexTotal);

    QVector<float> radii;
    radii.reserve(static_cast<int>(vertexTotal));
    for (auto it = bucket.byTexKey.constBegin(); it != bucket.byTexKey.constEnd(); ++it) {
        for (const ReconstructedVertex &v : it.value().vertices) {
            const double dx = v.px - cx;
            const double dy = v.py - cy;
            const double dz = v.pz - cz;
            radii.append(static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz)));
        }
    }
    QVector<float> sorted = radii;
    std::sort(sorted.begin(), sorted.end());
    const int p99Index = static_cast<int>(0.99 * static_cast<double>(sorted.size() - 1));
    const float p99 = sorted[p99Index];
    const float threshold = kOutlierRadiusFactor * p99;
    if (!(threshold > 0.0f) || !std::isfinite(threshold))
        return; // degenerate part (all vertices at the centroid)

    int radiusCursor = 0;
    for (auto it = bucket.byTexKey.begin(); it != bucket.byTexKey.end(); ++it) {
        SubMeshAccumulator &acc = it.value();
        const int vcount = acc.vertices.size();
        QVector<bool> outlier(vcount, false);
        int outlierCount = 0;
        for (int i = 0; i < vcount; ++i) {
            if (radii[radiusCursor + i] > threshold) {
                outlier[i] = true;
                ++outlierCount;
            }
        }
        radiusCursor += vcount;
        if (outlierCount == 0)
            continue;
        if (statsOut)
            statsOut->outlierDroppedVertices += outlierCount;

        // Drop every triangle referencing an outlier, then compact vertices.
        QVector<int> remap(vcount, -1);
        QVector<ReconstructedVertex> newVertices;
        QVector<uint32_t> newIndices;
        newVertices.reserve(vcount);
        newIndices.reserve(acc.indices.size());
        for (int t = 0; t + 2 < acc.indices.size(); t += 3) {
            const uint32_t i0 = acc.indices[t];
            const uint32_t i1 = acc.indices[t + 1];
            const uint32_t i2 = acc.indices[t + 2];
            if (outlier[static_cast<int>(i0)] || outlier[static_cast<int>(i1)]
                || outlier[static_cast<int>(i2)])
                continue;
            for (const uint32_t src : {i0, i1, i2}) {
                if (remap[static_cast<int>(src)] < 0) {
                    remap[static_cast<int>(src)] = newVertices.size();
                    newVertices.append(acc.vertices[static_cast<int>(src)]);
                }
                newIndices.append(static_cast<uint32_t>(remap[static_cast<int>(src)]));
            }
        }
        acc.vertices = newVertices;
        acc.indices = newIndices;

        QVector<uint32_t> newBoundsIndices;
        newBoundsIndices.reserve(acc.boundsVertexIndices.size());
        for (const uint32_t idx : acc.boundsVertexIndices) {
            const int mapped = remap[static_cast<int>(idx)];
            if (mapped >= 0)
                newBoundsIndices.append(static_cast<uint32_t>(mapped));
        }
        acc.boundsVertexIndices = newBoundsIndices;
    }
}

MatrixGroupsResult buildMatrixGroups(const CaptureSnapshot &snapshot,
                                     MeshReconstructionStats *statsOut,
                                     const Ps1NormalizerSettings &settings)
{
    MatrixGroupsResult out;
    out.primGroupKeys.reserve(snapshot.prims.size());

    if (statsOut) {
        statsOut->primsTotal = snapshot.prims.size();
        for (const PrimRecord &prim : snapshot.prims) {
            if (prim.matrixId < static_cast<uint32_t>(snapshot.matrices.size()))
                ++statsOut->primsWithMatrixId;
        }
    }

    // Pass 1 — resolve every prim's group key and pre-seed each group's
    // canonical matrix from the first tracked prim that lands in it. This is
    // done BEFORE any placement so pass 2 can invert pure-DepthOnly prims
    // against their group's shared matrix (#816 cross-prim spike fix): a
    // busy scene is ~90%+ DepthOnly, and without a group-wide matrix each such
    // prim inverted against its own per-draw matrix, so depth prims of one
    // object landed in slightly different model spaces and spanned/spiked.
    QVector<PrimMatrixResolution> resolutions;
    resolutions.reserve(snapshot.prims.size());
    for (const PrimRecord &prim : snapshot.prims) {
        const PrimMatrixResolution resolved =
            resolvePrimMatrix(prim, snapshot.gteRecords, statsOut);
        resolutions.append(resolved);
        out.primGroupKeys.append(resolved.key);
        if (resolved.recordIndex >= 0) {
            GroupBucket &bucket = out.groups[resolved.key];
            if (!bucket.hasTrackedMatrix) {
                bucket.trackedMatrix =
                    matrixRecordFromGteRecord(snapshot.gteRecords[resolved.recordIndex]);
                bucket.hasTrackedMatrix = true;
            }
        } else {
            out.groups[resolved.key]; // ensure the bucket exists
        }
    }

    // Pass 2 — place. Every prim inverts its Tier-1 vertices against the
    // group's canonical matrix when the group has one, so all depth geometry
    // of one object shares a single model space.
    int primIndex = -1;
    for (const PrimRecord &prim : snapshot.prims) {
        ++primIndex;
        if (!PsxCaptureFilters::isOnScreenPrim(prim))
            continue;

        const MatrixRecord *matrix = nullptr;
        if (prim.matrixId < static_cast<uint32_t>(snapshot.matrices.size()))
            matrix = &snapshot.matrices[static_cast<int>(prim.matrixId)];

        const PartGroupKey &key = resolutions[primIndex].key;
        GroupBucket &bucket = out.groups[key];
        const MatrixRecord *groupMatrix =
            bucket.hasTrackedMatrix ? &bucket.trackedMatrix : nullptr;

        const quint64 texKey = MeshReconstructor::textureGroupKey(
            prim.tpage, prim.clut, prim.semiTrans, prim.drawModeBits);
        SubMeshAccumulator &acc = bucket.byTexKey[texKey];
        if (acc.materialName.isEmpty())
            acc.materialName = MeshReconstructor::textureMaterialName(
                prim.tpage, prim.clut, prim.semiTrans, prim.drawModeBits);
        emitPrimitive(prim, matrix, snapshot.gteRecords, acc, statsOut, settings, groupMatrix);
    }

    // Outlier + spike passes run only on parts that contain Tier 0/1 vertices
    // — pure Tier-2 parts keep the fixed radius gate and stay byte-identical
    // (#816). The degenerate-triangle cull runs first (removes spanning spikes
    // by edge length), then the per-part radius outlier policy.
    for (auto it = out.groups.begin(); it != out.groups.end(); ++it) {
        if (!it.value().hasTieredVertices())
            continue;
        applyDegenerateTriangleCull(it.value(), settings.spikeEdgeFactor, statsOut);
        applyPartOutlierPolicy(it.value(), statsOut);
    }

    // Bounds fold after the outlier pass so dropped garbage can't poison the
    // slab canary. The folded set matches the legacy per-emit fold exactly
    // when nothing is dropped (min/max is order-independent).
    if (statsOut) {
        for (auto groupIt = out.groups.constBegin(); groupIt != out.groups.constEnd(); ++groupIt) {
            const GroupBucket &bucket = groupIt.value();
            for (auto texIt = bucket.byTexKey.constBegin(); texIt != bucket.byTexKey.constEnd();
                 ++texIt) {
                const SubMeshAccumulator &acc = texIt.value();
                for (const uint32_t idx : acc.boundsVertexIndices)
                    expandBounds(*statsOut, acc.vertices[static_cast<int>(idx)]);
            }
        }
        statsOut->finalizeSlabMetric();
    }
    return out;
}

/** Build the mesh for a single matrix group, recording which texture key
 *  produced which submesh index in `texKeyToSubMesh` so the per-prim
 *  provenance pass can resolve `prim → (groupKey, texKey) → submeshIndex`
 *  (#679 review feedback, key generalised for #816). */
/** Mesh cleanup (Step 4): weld coincident vertices then recompute smoothed
 *  per-vertex normals. The captured stream is unindexed triangle soup (3 fresh
 *  verts per tri), so shading is flat/faceted and vertex counts are 3× the
 *  unique geometry. This quantizes position/uv/colour to a grid, merges
 *  identical vertices into shared indices, and area-weight-averages the face
 *  normals into each shared vertex. In-place on one submesh. */
void weldAndSmoothSubMesh(ReconstructedSubMesh &sub)
{
    if (sub.indices.size() < 3 || sub.vertices.isEmpty())
        return;

    // Quantize to a fine grid so float noise doesn't defeat the weld. 1e-4
    // editor units (~0.01 model units) is well below PS1 vertex spacing.
    constexpr double kPosQuant = 10000.0; // 1/1e-4
    constexpr double kUvQuant = 4096.0;
    auto keyOf = [&](const ReconstructedVertex &v) {
        auto q = [](float f, double s) {
            return static_cast<long long>(std::llround(static_cast<double>(f) * s));
        };
        // Fold position (dominant), uv and colour into a string key. Colour
        // matters: a shared corner with two vertex colours must stay split so
        // the gouraud shading survives.
        return QStringLiteral("%1,%2,%3|%4,%5|%6")
            .arg(q(v.px, kPosQuant)).arg(q(v.py, kPosQuant)).arg(q(v.pz, kPosQuant))
            .arg(q(v.u, kUvQuant)).arg(q(v.v, kUvQuant))
            .arg(v.diffuseArgb);
    };

    QHash<QString, uint32_t> uniqueIndex;
    QVector<ReconstructedVertex> welded;
    welded.reserve(sub.vertices.size());
    QVector<uint32_t> newIdx;
    newIdx.reserve(sub.indices.size());
    for (const uint32_t oldIdx : sub.indices) {
        const ReconstructedVertex &v = sub.vertices[static_cast<int>(oldIdx)];
        const QString k = keyOf(v);
        auto it = uniqueIndex.constFind(k);
        uint32_t idx;
        if (it == uniqueIndex.constEnd()) {
            idx = static_cast<uint32_t>(welded.size());
            ReconstructedVertex nv = v;
            nv.nx = nv.ny = nv.nz = 0.0f; // accumulate below
            welded.append(nv);
            uniqueIndex.insert(k, idx);
        } else {
            idx = it.value();
        }
        newIdx.append(idx);
    }

    // Area-weighted face normals accumulated into each shared vertex, then
    // normalized. Cross-product magnitude already encodes 2× triangle area, so
    // adding the raw cross product weights by area for free.
    for (int t = 0; t + 2 < newIdx.size(); t += 3) {
        ReconstructedVertex &a = welded[static_cast<int>(newIdx[t])];
        ReconstructedVertex &b = welded[static_cast<int>(newIdx[t + 1])];
        ReconstructedVertex &c = welded[static_cast<int>(newIdx[t + 2])];
        const float ux = b.px - a.px, uy = b.py - a.py, uz = b.pz - a.pz;
        const float vx = c.px - a.px, vy = c.py - a.py, vz = c.pz - a.pz;
        const float nx = uy * vz - uz * vy;
        const float ny = uz * vx - ux * vz;
        const float nz = ux * vy - uy * vx;
        a.nx += nx; a.ny += ny; a.nz += nz;
        b.nx += nx; b.ny += ny; b.nz += nz;
        c.nx += nx; c.ny += ny; c.nz += nz;
    }
    for (ReconstructedVertex &v : welded) {
        const float len = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
        if (len > 1e-6f) {
            v.nx /= len; v.ny /= len; v.nz /= len;
        } else {
            v.nx = 0.0f; v.ny = 1.0f; v.nz = 0.0f; // degenerate — point up
        }
    }

    sub.vertices = welded;
    sub.indices = newIdx;
}

ReconstructedMesh meshFromMatrixGroup(const PartGroupKey &key,
                                      const QHash<quint64, SubMeshAccumulator> &texGroups,
                                      QHash<quint64, int> *texKeyToSubMesh = nullptr,
                                      bool cleanup = false)
{
    ReconstructedMesh result;
    result.meshName = key.first == 0
                          ? QStringLiteral("ps1_part_%1").arg(key.second)
                          : QStringLiteral("ps1_part_gte_%1").arg(key.second, 0, 16);

    for (auto texIt = texGroups.constBegin(); texIt != texGroups.constEnd(); ++texIt) {
        const SubMeshAccumulator &acc = texIt.value();
        if (acc.vertices.isEmpty() || acc.indices.size() < 3)
            continue;

        const int subMeshIndex = result.subMeshes.size();
        if (texKeyToSubMesh)
            texKeyToSubMesh->insert(texIt.key(), subMeshIndex);

        ReconstructedSubMesh sub;
        sub.materialName = acc.materialName;
        sub.vertices = acc.vertices;
        sub.indices = acc.indices;
        if (cleanup)
            weldAndSmoothSubMesh(sub);
        result.subMeshes.append(sub);
        result.vertexCount += sub.vertices.size();
        result.triangleCount += sub.indices.size() / 3;
    }
    return result;
}

struct PartsBuildResult {
    QVector<ReconstructedMesh> parts;
    // partIndexByGroupKey[key] = index into `parts` for this group. Groups
    // whose bucket produced no surviving submesh (every accumulator got
    // skipped by the < 3 indices guard) are simply absent.
    QHash<PartGroupKey, int> partIndexByGroupKey;
    // subMeshIndexByTexKey[partIndex][texKey] = submeshIndex within parts[partIndex].
    // Empty for parts contributed by `snapshot.modelMeshes` (those bypass the
    // matrix-group path and don't carry a texKey-addressable identity).
    QVector<QHash<quint64, int>> subMeshIndexByTexKey;
    // Parallel to snapshot.prims — computed once in buildMatrixGroups (#816).
    QVector<PartGroupKey> primGroupKeys;
    // Parallel to `parts`: the group's resolved GTE matrix for tracked groups
    // (partHasMatrix true), identity/default otherwise (#816).
    QVector<MatrixRecord> partMatrices;
    QVector<bool> partHasMatrix;
};

PartsBuildResult buildParts(const CaptureSnapshot &snapshot,
                            MeshReconstructionStats *statsOut,
                            const Ps1NormalizerSettings &settings)
{
    PartsBuildResult out;
    MatrixGroupsResult groups = buildMatrixGroups(snapshot, statsOut, settings);
    out.primGroupKeys = std::move(groups.primGroupKeys);

    for (auto groupIt = groups.groups.constBegin(); groupIt != groups.groups.constEnd();
         ++groupIt) {
        QHash<quint64, int> texKeyToSubMesh;
        ReconstructedMesh part =
            meshFromMatrixGroup(groupIt.key(), groupIt.value().byTexKey, &texKeyToSubMesh,
                                settings.cleanupWeldNormals);
        if (part.isEmpty())
            continue;
        out.partIndexByGroupKey.insert(groupIt.key(), out.parts.size());
        out.subMeshIndexByTexKey.append(texKeyToSubMesh);
        out.partMatrices.append(groupIt.value().trackedMatrix);
        out.partHasMatrix.append(groupIt.value().hasTrackedMatrix);
        out.parts.append(part);
    }

    // #674 — Model-space meshes from PsxTmdRamScanner / PsxHmdRamScanner. These bypass the
    // screen-space inverse-projection path entirely and arrive in editor world units, so
    // they're appended as additional parts. The dedupe pass in `reconstructDeduped` then
    // collapses byte-identical copies via MeshTopologyHash, the same way it does for
    // matrix-grouped screen-space parts.
    for (const CapturedModelMesh &cap : snapshot.modelMeshes) {
        if (cap.mesh.isEmpty())
            continue;
        out.subMeshIndexByTexKey.append(QHash<quint64, int>{});
        out.partMatrices.append(MatrixRecord{});
        out.partHasMatrix.append(false);
        out.parts.append(cap.mesh);
        if (statsOut) {
            int verts = 0;
            for (const ReconstructedSubMesh &sub : cap.mesh.subMeshes) {
                verts += sub.vertices.size();
                // Share the bounds-update helper with the matrix-group fold so a
                // future tweak to the bounds anchoring stays in lock-step
                // (#674 review).
                for (const ReconstructedVertex &v : sub.vertices)
                    expandBounds(*statsOut, v);
            }
            statsOut->modelMeshVertices += verts;
            statsOut->totalVertices += verts;
        }
    }

    return out;
}

ReconstructedMesh flattenParts(const QVector<ReconstructedMesh> &parts)
{
    ReconstructedMesh merged;
    merged.meshName = QStringLiteral("ps1_capture");
    for (const ReconstructedMesh &part : parts) {
        for (const ReconstructedSubMesh &sub : part.subMeshes)
            merged.subMeshes.append(sub);
        merged.vertexCount += part.vertexCount;
        merged.triangleCount += part.triangleCount;
    }
    return merged;
}

/** Walk `snapshot.prims` in order and resolve each one to the part + submesh
 *  it produced via the build-time `(groupKey, texKey) → (partIndex, subMeshIndex)`
 *  map. `instanceIndex == partIndex` because the dedupe pass emits exactly one
 *  instance per surviving part in the same order. Prims that don't survive
 *  the on-screen filter or whose matrix group was wholly skipped leave their
 *  default `-1` provenance — the inspector silently ignores those rows.
 *  The group key comes from `build.primGroupKeys` (computed once during the
 *  group build) so tracked prims resolve through the same GTE-record key the
 *  grouping used (#816). */
QVector<PrimProvenance> resolvePrimProvenance(const CaptureSnapshot &snapshot,
                                              const PartsBuildResult &build,
                                              const QVector<int> &partIndexToUnique)
{
    QVector<PrimProvenance> provenance(snapshot.prims.size());
    for (int i = 0; i < snapshot.prims.size(); ++i) {
        const PrimRecord &prim = snapshot.prims[i];
        if (!PsxCaptureFilters::isOnScreenPrim(prim))
            continue;
        const bool survives =
            ((prim.kind == PrimKind::MonoTri || prim.kind == PrimKind::ShadedTri
              || prim.kind == PrimKind::TexturedTri) && prim.vertexCount >= 3)
            || ((prim.kind == PrimKind::MonoQuad || prim.kind == PrimKind::ShadedQuad
                 || prim.kind == PrimKind::TexturedQuad) && prim.vertexCount >= 4)
            || (prim.kind == PrimKind::Sprite && prim.vertexCount >= 2);
        if (!survives)
            continue;
        if (i >= build.primGroupKeys.size())
            continue;
        const auto partIt = build.partIndexByGroupKey.constFind(build.primGroupKeys.at(i));
        if (partIt == build.partIndexByGroupKey.constEnd())
            continue;
        const int partIndex = partIt.value();
        if (partIndex < 0 || partIndex >= build.subMeshIndexByTexKey.size())
            continue;
        const QHash<quint64, int> &texMap = build.subMeshIndexByTexKey.at(partIndex);
        const quint64 texKey = MeshReconstructor::textureGroupKey(
            prim.tpage, prim.clut, prim.semiTrans, prim.drawModeBits);
        const auto subIt = texMap.constFind(texKey);
        if (subIt == texMap.constEnd())
            continue;
        if (partIndex >= partIndexToUnique.size())
            continue;
        const int uniqueIndex = partIndexToUnique.at(partIndex);
        if (uniqueIndex < 0)
            continue;
        provenance[i].uniqueMeshIndex = uniqueIndex;
        provenance[i].subMeshIndex = subIt.value();
        // Instance order is part order: `reconstructDeduped` walks `build.parts`
        // and appends one instance per surviving part, so `partIndex` is also
        // the index into `ReconstructedCaptureSet::instances`.
        provenance[i].instanceIndex = partIndex;
    }
    return provenance;
}

} // namespace

// textureMaterialName / textureGroupKey live in MeshReconstructorTexKeys.cpp
// (Ogre-free TU shared with the libretro plugin, #674/#813).

ReconstructedMesh MeshReconstructor::reconstruct(const CaptureSnapshot &snapshot)
{
    return flattenParts(buildParts(snapshot, nullptr, Ps1NormalizerSettings{}).parts);
}

ReconstructedCaptureSet MeshReconstructor::reconstructDeduped(const CaptureSnapshot &snapshot,
                                                            MeshDedupeMode dedupeMode)
{
    return reconstructDeduped(snapshot, dedupeMode, Ps1NormalizerSettings{}, nullptr);
}

ReconstructedCaptureSet MeshReconstructor::reconstructDeduped(const CaptureSnapshot &snapshot,
                                                            MeshDedupeMode dedupeMode,
                                                            MeshReconstructionStats *statsOut)
{
    return reconstructDeduped(snapshot, dedupeMode, Ps1NormalizerSettings{}, statsOut);
}

ReconstructedCaptureSet MeshReconstructor::reconstructDeduped(const CaptureSnapshot &snapshot,
                                                            MeshDedupeMode dedupeMode,
                                                            const Ps1NormalizerSettings &normalize,
                                                            MeshReconstructionStats *statsOut)
{
    ReconstructedCaptureSet result;
    const PartsBuildResult build = buildParts(snapshot, statsOut, normalize);
    result.capturedPartCount = build.parts.size();
    if (build.parts.isEmpty()) {
        // Still emit a default-filled provenance vector so the inspector
        // can index it 1:1 with `snapshot.prims` without checking sizes.
        result.primProvenance.resize(snapshot.prims.size());
        return result;
    }

    QHash<quint64, int> hashToUnique;
    QVector<int> partIndexToUnique;
    partIndexToUnique.reserve(build.parts.size());

    for (int partIdx = 0; partIdx < build.parts.size(); ++partIdx) {
        const ReconstructedMesh &part = build.parts[partIdx];
        float cx = 0.0f;
        float cy = 0.0f;
        float cz = 0.0f;
        const ReconstructedMesh local = MeshTopologyHash::centered(part, cx, cy, cz);
        const quint64 h = MeshTopologyHash::hashMesh(local, dedupeMode);

        int uniqueIndex = hashToUnique.value(h, -1);
        if (uniqueIndex < 0) {
            uniqueIndex = result.uniqueMeshes.size();
            hashToUnique.insert(h, uniqueIndex);
            ReconstructedMesh unique = local;
            unique.meshName = QStringLiteral("ps1_unique_%1").arg(uniqueIndex);
            result.uniqueMeshes.append(unique);
        }

        ReconstructedInstance inst;
        inst.uniqueMeshIndex = uniqueIndex;
        inst.px = cx;
        inst.py = cy;
        inst.pz = cz;
        // #816: tracked groups carry their full GTE matrix so identical props
        // that dedupe into one model-space mesh keep per-instance placement
        // data. Rotation stays in the raw GTE convention (the mesh builder
        // converts axes); translation is pre-converted to editor units.
        if (partIdx < build.partHasMatrix.size() && build.partHasMatrix.at(partIdx)) {
            const MatrixRecord &m = build.partMatrices.at(partIdx);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    inst.rot[r * 3 + c] = static_cast<float>(m.rt.m[r][c]) / 4096.0f;
            GteInverse::modelToEditor(static_cast<float>(m.tr[0]), static_cast<float>(m.tr[1]),
                                      static_cast<float>(m.tr[2]), inst.trWorld[0],
                                      inst.trWorld[1], inst.trWorld[2]);
            inst.hasMatrix = true;
        }
        result.instances.append(inst);
        partIndexToUnique.append(uniqueIndex);
    }

    result.primProvenance = resolvePrimProvenance(snapshot, build, partIndexToUnique);
    return result;
}
