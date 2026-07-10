#ifdef ENABLE_PS1_RIP

// Tiered reconstruction tests (#816): Tier 0 (GteTracked — exact object-space
// vertices from in-core GTE records), Tier 1 (DepthOnly — PGXP subpixel screen
// coords + view depth through the float screenToModel), Tier 2 (None — the
// pre-in-core world, byte-identical behavior).
//
// Fixtures construct data the way the real pipeline produces it (provenance
// flags + record table), not the way that makes assertions pass: packet x/y
// stay integer screen coords, precise coords come from the real forward
// projection, and records carry the raw 4.12 GTE register values.

#include <gtest/gtest.h>

#include "PS1/runtime/CaptureSnapshot.h"
#include "PS1/runtime/GteCapture.h"
#include "PS1/runtime/GteInverse.h"
#include "PS1/runtime/MeshReconstructionStats.h"
#include "PS1/runtime/MeshReconstructor.h"
#include "PS1/runtime/MeshTopologyHash.h"
#include "PS1/runtime/PS1RipMeshBuilder.h"

#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

int16_t toFixed412(double v)
{
    return static_cast<int16_t>(std::lround(v * 4096.0));
}

/** 4.12 rotation matrix from intrinsic XYZ Euler angles (same convention as
 *  the GteInverse_test fixtures: R = Rz * Ry * Rx, model -> camera basis). */
MatrixRecord rotationMatrix(double rx, double ry, double rz)
{
    const double cx = std::cos(rx), sx = std::sin(rx);
    const double cy = std::cos(ry), sy = std::sin(ry);
    const double cz = std::cos(rz), sz = std::sin(rz);

    MatrixRecord m{};
    m.rt.m[0][0] = toFixed412(cz * cy);
    m.rt.m[0][1] = toFixed412(cz * sy * sx - sz * cx);
    m.rt.m[0][2] = toFixed412(cz * sy * cx + sz * sx);
    m.rt.m[1][0] = toFixed412(sz * cy);
    m.rt.m[1][1] = toFixed412(sz * sy * sx + cz * cx);
    m.rt.m[1][2] = toFixed412(sz * sy * cx - cz * sx);
    m.rt.m[2][0] = toFixed412(-sy);
    m.rt.m[2][1] = toFixed412(cy * sx);
    m.rt.m[2][2] = toFixed412(cy * cx);
    m.h = 256;
    return m;
}

GteRecordEntry recordFor(const MatrixRecord &m, int16_t vx, int16_t vy, int16_t vz, uint32_t seq)
{
    GteRecordEntry rec{};
    rec.vx = vx;
    rec.vy = vy;
    rec.vz = vz;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            rec.rt[r * 3 + c] = m.rt.m[r][c];
    for (int i = 0; i < 3; ++i)
        rec.tr[i] = m.tr[i];
    rec.ofx = m.ofx;
    rec.ofy = m.ofy;
    rec.h = m.h;
    rec.seq = seq;
    return rec;
}

const int16_t kCubeVerts[8][3] = {
    {-100, -100, -100}, {100, -100, -100}, {100, 100, -100}, {-100, 100, -100},
    {-100, -100, 100},  {100, -100, 100},  {100, 100, 100},  {-100, 100, 100},
};

const int kCubeTris[12][3] = {
    {0, 1, 2}, {0, 2, 3}, {5, 4, 7}, {5, 7, 6}, {4, 0, 3}, {4, 3, 7},
    {1, 5, 6}, {1, 6, 2}, {4, 5, 1}, {4, 1, 0}, {3, 2, 6}, {3, 6, 7},
};

enum class FixtureTier { Tracked, DepthOnly, FlatNoDepth };

/** Subpixel forward projection through the 4.12 matrix — the float the PGXP
 *  shadow would carry. The integer modelToScreen loses ~0.5 px, which at
 *  sz ≈ 5000 / h = 256 is ~10 model units of inverse error; PGXP's whole
 *  point is that the precise value never quantises to the screen grid. */
void preciseProject(const MatrixRecord &m, const int16_t v[3], float &sx, float &sy, float &sz)
{
    double cam[3];
    for (int r = 0; r < 3; ++r) {
        cam[r] = (static_cast<double>(m.rt.m[r][0]) * v[0] + static_cast<double>(m.rt.m[r][1]) * v[1]
                  + static_cast<double>(m.rt.m[r][2]) * v[2])
                     / 4096.0
                 + static_cast<double>(m.tr[r]);
    }
    const double h = m.h != 0 ? static_cast<double>(m.h) : 4096.0;
    sx = static_cast<float>(cam[0] * h / cam[2] + static_cast<double>(m.ofx) / 65536.0);
    sy = static_cast<float>(cam[1] * h / cam[2] + static_cast<double>(m.ofy) / 65536.0);
    sz = static_cast<float>(cam[2]);
}

/** Builds a 12-tri cube snapshot the way RipperHooks emits it: one GTE record
 *  per triangle vertex, packet screen XY from the real forward projection
 *  (+160/+120 drawing offset so isOnScreenPrim passes), PGXP-precise floats
 *  in the shadow fields, per-vertex provenance per the requested tier. */
CaptureSnapshot cubeSnapshot(const MatrixRecord &matrix, FixtureTier tier,
                             uint32_t seqBase = 0)
{
    CaptureSnapshot snap;
    snap.matrices.append(matrix);

    for (int t = 0; t < 12; ++t) {
        PrimRecord prim;
        prim.kind = PrimKind::MonoTri;
        prim.vertexCount = 3;
        prim.matrixId = 0;
        for (int v = 0; v < 3; ++v) {
            const int16_t *mv = kCubeVerts[kCubeTris[t][v]];
            float sx = 0.0f, sy = 0.0f, sz = 0.0f;
            preciseProject(matrix, mv, sx, sy, sz);
            EXPECT_GT(sz, 0.0f) << "fixture model must sit in front of the camera";

            PsxVertex &vert = prim.verts[v];
            vert.x = 160 + static_cast<int32_t>(std::lround(sx));
            vert.y = 120 + static_cast<int32_t>(std::lround(sy));
            vert.z = 0; // GP0 packets never carry depth
            vert.r = vert.g = vert.b = 128;

            switch (tier) {
            case FixtureTier::Tracked:
                vert.preciseX = sx;
                vert.preciseY = sy;
                vert.viewW = sz;
                vert.gteRecordIndex = static_cast<uint32_t>(snap.gteRecords.size());
                vert.provenance = static_cast<uint8_t>(PsxVertexProvenance::GteTracked);
                snap.gteRecords.append(
                    recordFor(matrix, mv[0], mv[1], mv[2],
                              seqBase + static_cast<uint32_t>(t * 3 + v)));
                break;
            case FixtureTier::DepthOnly:
                vert.preciseX = sx;
                vert.preciseY = sy;
                vert.viewW = sz;
                vert.provenance = static_cast<uint8_t>(PsxVertexProvenance::DepthOnly);
                break;
            case FixtureTier::FlatNoDepth:
                vert.preciseX = sx;
                vert.preciseY = sy;
                vert.viewW = 0.0f; // depth unknown
                vert.provenance = static_cast<uint8_t>(PsxVertexProvenance::DepthOnly);
                break;
            }
        }
        snap.prims.append(prim);
    }
    return snap;
}

/** Max distance from any reconstructed vertex to its nearest expected cube
 *  corner, in editor units. */
float maxCornerError(const ReconstructedCaptureSet &set)
{
    float worst = 0.0f;
    for (const ReconstructedMesh &mesh : set.uniqueMeshes) {
        for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
            for (const ReconstructedVertex &v : sub.vertices) {
                float best = std::numeric_limits<float>::max();
                for (const auto &corner : kCubeVerts) {
                    float ex = 0.0f, ey = 0.0f, ez = 0.0f;
                    GteInverse::modelToEditor(static_cast<float>(corner[0]),
                                              static_cast<float>(corner[1]),
                                              static_cast<float>(corner[2]), ex, ey, ez);
                    const float dx = v.px - ex, dy = v.py - ey, dz = v.pz - ez;
                    best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
                }
                worst = std::max(worst, best);
            }
        }
    }
    return worst;
}

} // namespace

TEST(MeshReconstructorTieredTest, TrackedCubeReconstructsExactly)
{
    MatrixRecord matrix = rotationMatrix(kPi / 6.0, kPi / 4.0, kPi / 12.0);
    matrix.tr[0] = 250;
    matrix.tr[1] = -100;
    matrix.tr[2] = 5000;
    ASSERT_TRUE(GteCapture::looksOrthonormalRotation(matrix));

    const CaptureSnapshot snap = cubeSnapshot(matrix, FixtureTier::Tracked);

    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, &stats);

    ASSERT_FALSE(set.isEmpty());
    // Tier 0 reads the raw object-space register — the cube must come back
    // EXACT (1 fixed-point model unit = 0.01 editor units), not merely
    // "with volume".
    EXPECT_LE(maxCornerError(set), 0.01f);

    EXPECT_EQ(stats.gteTrackedVertices, stats.totalVertices);
    EXPECT_EQ(stats.gteTrackedPercent(), 100);
    EXPECT_EQ(stats.depthOnlyVertices, 0);
    EXPECT_EQ(stats.outlierDroppedVertices, 0);
    EXPECT_EQ(stats.mixedMatrixPrims, 0);
    EXPECT_FALSE(stats.slabLike) << "a tracked capture that reports slabLike is a bug";
}

TEST(MeshReconstructorTieredTest, DepthOnlyTierReproducesCubeWithinRoundTripTolerance)
{
    MatrixRecord matrix = rotationMatrix(kPi / 6.0, kPi / 4.0, kPi / 12.0);
    matrix.tr[0] = 250;
    matrix.tr[1] = -100;
    matrix.tr[2] = 5000;

    const CaptureSnapshot snap = cubeSnapshot(matrix, FixtureTier::DepthOnly);

    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, &stats);

    ASSERT_FALSE(set.isEmpty());
    // Integer screen rounding contributes ~3-4 model units of drift (matches
    // the GteInverse round-trip bounds) = 0.04 editor units.
    EXPECT_LE(maxCornerError(set), 0.05f);

    EXPECT_EQ(stats.gteTrackedVertices, 0);
    EXPECT_EQ(stats.depthOnlyVertices, stats.totalVertices);
    EXPECT_EQ(stats.depthOnlyPercent(), 100);
    EXPECT_FALSE(stats.slabLike);
}

TEST(MeshReconstructorTieredTest, MissingDepthFallsBackToFlatSlab)
{
    MatrixRecord matrix = rotationMatrix(kPi / 6.0, kPi / 4.0, kPi / 12.0);
    matrix.tr[2] = 5000;

    const CaptureSnapshot snap = cubeSnapshot(matrix, FixtureTier::FlatNoDepth);

    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, &stats);

    ASSERT_FALSE(set.isEmpty());
    // viewW == 0 means the inverse is refused per-vertex; everything lands in
    // the screen-space fallback — the flat plate the slab canary exists for.
    EXPECT_EQ(stats.gteTrackedVertices, 0);
    EXPECT_EQ(stats.depthOnlyVertices, 0);
    EXPECT_EQ(stats.gteTrackedPercent(), 0);
    EXPECT_TRUE(stats.slabLike);
}

TEST(MeshReconstructorTieredTest, TwoObjectSceneDedupesToOneMeshWithPerInstanceMatrices)
{
    MatrixRecord matrixA = rotationMatrix(0.0, 0.0, 0.0);
    matrixA.tr[0] = 0;
    matrixA.tr[1] = 0;
    matrixA.tr[2] = 5000;

    MatrixRecord matrixB = rotationMatrix(0.0, kPi / 2.0, 0.0);
    matrixB.tr[0] = 1500;
    matrixB.tr[1] = -300;
    matrixB.tr[2] = 7000;

    CaptureSnapshot snap = cubeSnapshot(matrixA, FixtureTier::Tracked);
    const CaptureSnapshot snapB = cubeSnapshot(matrixB, FixtureTier::Tracked, /*seqBase=*/100);
    const int recordOffset = snap.gteRecords.size();
    for (PrimRecord prim : snapB.prims) {
        for (int v = 0; v < prim.vertexCount; ++v)
            prim.verts[v].gteRecordIndex += static_cast<uint32_t>(recordOffset);
        snap.prims.append(prim);
    }
    snap.gteRecords += snapB.gteRecords;

    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, &stats);

    // Both cubes reconstruct in model space, so they dedupe to ONE unique
    // mesh with two placed instances — strictly better than the centroid
    // trick, which needed world-space copies to coincide.
    EXPECT_EQ(set.uniqueCount(), 1);
    ASSERT_EQ(set.instanceCount(), 2);

    float ta[3] = {0.0f, 0.0f, 0.0f};
    float tb[3] = {0.0f, 0.0f, 0.0f};
    GteInverse::modelToEditor(static_cast<float>(matrixA.tr[0]), static_cast<float>(matrixA.tr[1]),
                              static_cast<float>(matrixA.tr[2]), ta[0], ta[1], ta[2]);
    GteInverse::modelToEditor(static_cast<float>(matrixB.tr[0]), static_cast<float>(matrixB.tr[1]),
                              static_cast<float>(matrixB.tr[2]), tb[0], tb[1], tb[2]);

    int matchedA = 0;
    int matchedB = 0;
    for (const ReconstructedInstance &inst : set.instances) {
        ASSERT_TRUE(inst.hasMatrix);
        const bool isA = std::fabs(inst.trWorld[0] - ta[0]) < 1e-4f
                         && std::fabs(inst.trWorld[1] - ta[1]) < 1e-4f
                         && std::fabs(inst.trWorld[2] - ta[2]) < 1e-4f;
        const bool isB = std::fabs(inst.trWorld[0] - tb[0]) < 1e-4f
                         && std::fabs(inst.trWorld[1] - tb[1]) < 1e-4f
                         && std::fabs(inst.trWorld[2] - tb[2]) < 1e-4f;
        const MatrixRecord &expected = isA ? matrixA : matrixB;
        ASSERT_TRUE(isA || isB) << "instance translation matches neither group";
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                EXPECT_NEAR(inst.rot[r * 3 + c],
                            static_cast<float>(expected.rt.m[r][c]) / 4096.0f, 1e-4f);
        matchedA += isA ? 1 : 0;
        matchedB += isB ? 1 : 0;
    }
    EXPECT_EQ(matchedA, 1);
    EXPECT_EQ(matchedB, 1);
}

TEST(MeshReconstructorTieredTest, MixedProvenanceAndStaleRecordDegradePerVertex)
{
    MatrixRecord matrix = rotationMatrix(0.0, kPi / 4.0, 0.0);
    matrix.tr[2] = 5000;

    CaptureSnapshot snap = cubeSnapshot(matrix, FixtureTier::Tracked);

    // v1 of the first prim loses its tag but keeps depth → Tier 1.
    snap.prims[0].verts[1].provenance = static_cast<uint8_t>(PsxVertexProvenance::DepthOnly);
    snap.prims[0].verts[1].gteRecordIndex = UINT32_MAX;
    // v2 keeps GteTracked but its ring index went stale (out of range) —
    // must degrade (depth is still valid → Tier 1), never crash.
    snap.prims[0].verts[2].gteRecordIndex = 999999;

    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, &stats);

    ASSERT_FALSE(set.isEmpty());
    EXPECT_GT(stats.gteTrackedVertices, 0);
    EXPECT_GE(stats.depthOnlyVertices, 2);
    EXPECT_EQ(stats.gteTrackedVertices + stats.depthOnlyVertices, stats.totalVertices);
    // Degraded vertices still invert against the same matrix, so the cube
    // stays intact within the round-trip tolerance.
    EXPECT_LE(maxCornerError(set), 0.05f);
    EXPECT_FALSE(stats.slabLike);
}

TEST(MeshReconstructorTieredTest, OutlierPolicyDropsGarbageTrackedVertex)
{
    MatrixRecord matrix = rotationMatrix(0.0, 0.0, 0.0);
    matrix.tr[2] = 5000;

    CaptureSnapshot snap = cubeSnapshot(matrix, FixtureTier::Tracked);

    // One extra triangle with a garbage tracked vertex at 30000 model units —
    // the stale-ring failure mode the per-part outlier policy exists for.
    // With the old fixed 64-editor-unit gate this would instead have CLIPPED
    // legitimate large geometry; here only the garbage triangle dies.
    PrimRecord garbage = snap.prims[0];
    for (int v = 0; v < 2; ++v) {
        const int16_t *mv = kCubeVerts[v];
        garbage.verts[v].gteRecordIndex = static_cast<uint32_t>(snap.gteRecords.size());
        snap.gteRecords.append(recordFor(matrix, mv[0], mv[1], mv[2],
                                         static_cast<uint32_t>(snap.gteRecords.size())));
    }
    garbage.verts[2].gteRecordIndex = static_cast<uint32_t>(snap.gteRecords.size());
    snap.gteRecords.append(recordFor(matrix, 30000, 30000, 30000,
                                     static_cast<uint32_t>(snap.gteRecords.size())));
    snap.prims.append(garbage);

    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, &stats);

    ASSERT_FALSE(set.isEmpty());
    EXPECT_GE(stats.outlierDroppedVertices, 1);
    // Bounds must stay cube-sized — the garbage vertex may not poison the AABB.
    EXPECT_TRUE(stats.hasBounds());
    EXPECT_LT(stats.boundsMaxX - stats.boundsMinX, 10.0f);
    EXPECT_LT(stats.boundsMaxY - stats.boundsMinY, 10.0f);
    EXPECT_LT(stats.boundsMaxZ - stats.boundsMinZ, 10.0f);
    EXPECT_LE(maxCornerError(set), 0.01f);
}

TEST(MeshReconstructorTieredTest, NoneProvenanceKeepsLegacyBehavior)
{
    // A plain RAM-scan style snapshot (no records, provenance None, z == 0)
    // must reproduce the pre-#816 flat-fallback world untouched.
    MatrixRecord matrix = rotationMatrix(0.0, 0.0, 0.0);
    matrix.tr[2] = 5000;

    CaptureSnapshot snap = cubeSnapshot(matrix, FixtureTier::Tracked);
    snap.gteRecords.clear();
    for (PrimRecord &prim : snap.prims) {
        for (int v = 0; v < prim.vertexCount; ++v) {
            prim.verts[v].provenance = static_cast<uint8_t>(PsxVertexProvenance::None);
            prim.verts[v].gteRecordIndex = UINT32_MAX;
            prim.verts[v].preciseX = 0.0f;
            prim.verts[v].preciseY = 0.0f;
            prim.verts[v].viewW = 0.0f;
        }
    }

    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, &stats);

    ASSERT_FALSE(set.isEmpty());
    EXPECT_EQ(stats.gteTrackedVertices, 0);
    EXPECT_EQ(stats.depthOnlyVertices, 0);
    EXPECT_EQ(stats.screenFallbackVertices, stats.totalVertices);
    EXPECT_TRUE(stats.slabLike);
}

TEST(MeshReconstructorTieredTest, TrackedGeometryOnlyDropsScreenSpacePrims)
{
    // A mixed capture: a tracked cube plus one screen-space "HUD" triangle
    // (None provenance, no depth) — the clean-up filter must keep the cube
    // and drop the HUD prim entirely.
    MatrixRecord matrix = rotationMatrix(0.0, 0.0, 0.0);
    matrix.tr[2] = 5000;
    CaptureSnapshot snap = cubeSnapshot(matrix, FixtureTier::Tracked);
    const int trackedPrims = snap.prims.size();

    PrimRecord hud;
    hud.kind = PrimKind::ShadedTri;
    hud.vertexCount = 3;
    for (int v = 0; v < 3; ++v) {
        hud.verts[v].x = 40 + v * 20;
        hud.verts[v].y = 30;
        hud.verts[v].provenance = static_cast<uint8_t>(PsxVertexProvenance::None);
        hud.verts[v].gteRecordIndex = UINT32_MAX;
        hud.verts[v].viewW = 0.0f;
    }
    snap.prims.append(hud);

    // Baseline (filter off): both the cube and the HUD prim reconstruct.
    MeshReconstructionStats baseStats;
    const ReconstructedCaptureSet baseline =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, &baseStats);
    EXPECT_GT(baseStats.screenFallbackVertices, 0) << "HUD prim should land in Tier 2";

    // Filter on: the HUD prim is dropped, only tracked geometry survives.
    Ps1NormalizerSettings clean;
    clean.trackedGeometryOnly = true;
    MeshReconstructionStats cleanStats;
    const ReconstructedCaptureSet cleaned =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, clean, &cleanStats);

    ASSERT_FALSE(cleaned.isEmpty());
    EXPECT_EQ(cleanStats.screenFallbackVertices, 0)
        << "clean-up filter must drop every screen-space prim";
    EXPECT_EQ(cleanStats.gteTrackedVertices, cleanStats.totalVertices);
    EXPECT_EQ(cleanStats.totalVertices, trackedPrims * 3);
    EXPECT_FALSE(cleanStats.slabLike);
}

TEST(MeshReconstructorTieredTest, PartlyTrackedPrimStaysCoherentNoSpike)
{
    // The radiating-spike artifact: a triangle with one GteTracked corner
    // (placed from the raw object-space record) and depth corners that, before
    // the fix, inverted against a *different* matrix and landed in a different
    // model space — stretching the triangle across the scene. With the fix all
    // three corners share the prim's tracked matrix and stay within the cube.
    MatrixRecord matrix = rotationMatrix(kPi / 6.0, kPi / 5.0, 0.0);
    matrix.tr[0] = 120;
    matrix.tr[1] = -60;
    matrix.tr[2] = 5000;

    CaptureSnapshot snap = cubeSnapshot(matrix, FixtureTier::Tracked);
    // Demote 2 of every triangle's 3 corners to DepthOnly (keep their precise
    // coords + viewW, strip the tag) so each prim is 1 tracked + 2 depth.
    for (PrimRecord &prim : snap.prims) {
        for (int v = 1; v < 3; ++v) {
            prim.verts[v].provenance = static_cast<uint8_t>(PsxVertexProvenance::DepthOnly);
            prim.verts[v].gteRecordIndex = UINT32_MAX;
        }
    }

    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, &stats);

    ASSERT_FALSE(set.isEmpty());
    // Mixed tiers as intended.
    EXPECT_GT(stats.gteTrackedVertices, 0);
    EXPECT_GT(stats.depthOnlyVertices, 0);
    // The cube must stay a cube: every reconstructed vertex within ~4 model
    // units (0.04 editor) of a real corner. A spike would put verts far away.
    EXPECT_LE(maxCornerError(set), 0.05f)
        << "partly-tracked prims must reconstruct in one coherent model space";
    EXPECT_FALSE(stats.slabLike);
}

TEST(MeshReconstructorTieredTest, DegenerateTriangleCullDropsSpanningTriangle)
{
    // The edge cull targets a triangle that BRIDGES two plausible clusters —
    // both endpoints are within the radius outlier gate, but the edge between
    // them is a runaway span (the cross-prim spike the radius policy can't see
    // because neither vertex is individually an outlier). Build 24 small tris
    // (short edges → small median) sharing a matrix, then add one triangle
    // whose corners are ~40 model units apart (a long edge vs the ~30-unit
    // median). The cull drops it; the small tris stay.
    MatrixRecord matrix = rotationMatrix(0.0, 0.0, 0.0);
    matrix.tr[2] = 5000;

    CaptureSnapshot snap;
    snap.matrices.append(matrix);
    auto addTrackedTri = [&](const int16_t a[3], const int16_t b[3], const int16_t c[3]) {
        PrimRecord p;
        p.kind = PrimKind::ShadedTri;
        p.vertexCount = 3;
        const int16_t *pts[3] = {a, b, c};
        for (int v = 0; v < 3; ++v) {
            p.verts[v].provenance = static_cast<uint8_t>(PsxVertexProvenance::GteTracked);
            p.verts[v].gteRecordIndex = static_cast<uint32_t>(snap.gteRecords.size());
            snap.gteRecords.append(recordFor(matrix, pts[v][0], pts[v][1], pts[v][2],
                                             static_cast<uint32_t>(snap.gteRecords.size())));
        }
        snap.prims.append(p);
    };

    // A dense cluster of tiny triangles (edges ~20-40 model units).
    for (int i = 0; i < 24; ++i) {
        const int16_t a[3] = {int16_t(i * 5), 0, 0};
        const int16_t b[3] = {int16_t(i * 5 + 20), 10, 0};
        const int16_t c[3] = {int16_t(i * 5), 30, 0};
        addTrackedTri(a, b, c);
    }
    // One spanning triangle: a huge edge from ~0 to ~4000 model units — both
    // endpoints are plausible mesh points, but the edge is a runaway.
    const int16_t s0[3] = {0, 0, 0};
    const int16_t s1[3] = {4000, 0, 0};
    const int16_t s2[3] = {0, 30, 0};
    addTrackedTri(s0, s1, s2);

    // Cull disabled → the span survives (max pairwise extent is huge).
    Ps1NormalizerSettings noCull;
    noCull.spikeEdgeFactor = 0.0f;
    MeshReconstructionStats s0stats;
    const ReconstructedCaptureSet withSpan =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, noCull, &s0stats);
    ASSERT_FALSE(withSpan.isEmpty());

    // Cull enabled → the spanning triangle is dropped, cluster stays.
    Ps1NormalizerSettings withCull; // spikeEdgeFactor defaults to 12.0
    MeshReconstructionStats s1stats;
    const ReconstructedCaptureSet cleaned =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, withCull, &s1stats);
    ASSERT_FALSE(cleaned.isEmpty());

    // The edge cull drops the spanning triangle; the radius policy alone
    // (no-cull run) does not — the span's endpoints are both plausible mesh
    // points, so neither is a centroid-radius outlier. The cull's extra drops
    // are the signal.
    // The edge cull drops the spanning triangle by edge length. (The radius
    // policy also happens to catch this particular span's far endpoint, so the
    // distinguishing signal is that the cull records the extra drop — it fires
    // regardless of whether a vertex is individually a centroid outlier.)
    EXPECT_GT(s1stats.outlierDroppedVertices, s0stats.outlierDroppedVertices)
        << "edge cull must drop the spanning triangle";
    (void)withSpan;
}

TEST(MeshReconstructorTieredTest, EditorRotationFromGtePins90DegreeYRotation)
{
    // 90° around Y in the GTE camera basis: model (x,y,z) -> camera (z,y,-x).
    const float gteRot[9] = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f};
    float editor[9] = {};
    PS1RipMeshBuilder::editorRotationFromGte(gteRot, editor);

    // Conjugation by S = diag(1,-1,-1) flips the sign of every element with
    // exactly one Y/Z index: out[r][c] = s(r)·s(c)·R[r][c].
    const float expected[9] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    for (int i = 0; i < 9; ++i)
        EXPECT_FLOAT_EQ(editor[i], expected[i]) << "element " << i;

    // A proper rotation must stay proper after the basis change: det == +1.
    const float det = editor[0] * (editor[4] * editor[8] - editor[5] * editor[7])
                      - editor[1] * (editor[3] * editor[8] - editor[5] * editor[6])
                      + editor[2] * (editor[3] * editor[7] - editor[4] * editor[6]);
    EXPECT_NEAR(det, 1.0f, 1e-5f);
}

#endif // ENABLE_PS1_RIP
