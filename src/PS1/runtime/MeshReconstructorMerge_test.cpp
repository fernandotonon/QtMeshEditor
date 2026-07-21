#ifdef ENABLE_PS1_RIP

// Same-object cross-frame merge tests (#412): a scene capture groups prims by
// the exact per-frame GTE matrix, so a moving object produces one sparse part
// per frame (games NCLIP-cull back faces before GP0 — each frame only submits
// the triangles facing the camera that frame). With mergeSameObjectParts on,
// MeshReconstructor clusters tracked groups by object-space vertex overlap and
// unions their triangles into one full object; a duplicate-triangle cull drops
// the once-per-frame repeats. Groups drawn in the SAME frame must never merge
// (simultaneous instances / hierarchy limbs).
//
// Fixtures follow the MeshReconstructorTiered_test convention: data is built
// the way RipperHooks emits it (provenance flags + record table, packet screen
// XY from the real forward projection).

#include <gtest/gtest.h>

#include "PS1/runtime/CaptureSnapshot.h"
#include "PS1/runtime/GteCapture.h"
#include "PS1/runtime/GteInverse.h"
#include "PS1/runtime/MeshReconstructionStats.h"
#include "PS1/runtime/MeshReconstructor.h"
#include "PS1/runtime/MeshTopologyHash.h"

#include <cmath>
#include <limits>

namespace {

constexpr double kPi = 3.14159265358979323846;

int16_t toFixed412(double v)
{
    return static_cast<int16_t>(std::lround(v * 4096.0));
}

/** 4.12 rotation matrix from intrinsic XYZ Euler angles (same convention as
 *  the tiered-test fixtures: R = Rz * Ry * Rx, model -> camera basis). */
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

const int16_t kCubeVerts[8][3] = {
    {-100, -100, -100}, {100, -100, -100}, {100, 100, -100}, {-100, 100, -100},
    {-100, -100, 100},  {100, -100, 100},  {100, 100, 100},  {-100, 100, 100},
};

const int kCubeTris[12][3] = {
    {0, 1, 2}, {0, 2, 3}, {5, 4, 7}, {5, 7, 6}, {4, 0, 3}, {4, 3, 7},
    {1, 5, 6}, {1, 6, 2}, {4, 5, 1}, {4, 1, 0}, {3, 2, 6}, {3, 6, 7},
};

/** Subpixel forward projection through the 4.12 matrix (PGXP shadow float). */
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

/** Appends the given cube triangles as GteTracked prims drawn under `matrix`
 *  at core frame `frame` — one frame's sparse view of the object, exactly the
 *  way a scene capture accumulates it. `colour` simulates per-frame gouraud
 *  re-lighting (duplicates of one triangle differ only in colour). `warpX`
 *  offsets the ODD cube corners' object-space vx — a per-frame vertex
 *  animation that defeats exact-key matching (#412 stage 2). `tpage` gives
 *  the prims a texture-key identity; `vzOffset` shifts the whole cube in
 *  object space so two fixtures represent genuinely different models. */
void appendCubeFrame(CaptureSnapshot &snap, const MatrixRecord &matrix, uint32_t frame,
                     const QVector<int> &triIndices, uint8_t colour = 128,
                     int16_t warpX = 0, uint16_t tpage = 0, int16_t vzOffset = 0)
{
    for (const int t : triIndices) {
        PrimRecord prim;
        prim.kind = PrimKind::MonoTri;
        prim.vertexCount = 3;
        prim.matrixId = 0;
        prim.frame = frame;
        prim.tpage = tpage;
        for (int v = 0; v < 3; ++v) {
            const int corner = kCubeTris[t][v];
            int16_t warped[3] = {kCubeVerts[corner][0], kCubeVerts[corner][1],
                                 kCubeVerts[corner][2]};
            if ((corner % 2) == 1)
                warped[0] = static_cast<int16_t>(warped[0] + warpX);
            warped[2] = static_cast<int16_t>(warped[2] + vzOffset);
            const int16_t *mv = warped;
            float sx = 0.0f, sy = 0.0f, sz = 0.0f;
            preciseProject(matrix, mv, sx, sy, sz);
            ASSERT_GT(sz, 0.0f) << "fixture model must sit in front of the camera";

            PsxVertex &vert = prim.verts[v];
            vert.x = 160 + static_cast<int32_t>(std::lround(sx));
            vert.y = 120 + static_cast<int32_t>(std::lround(sy));
            vert.z = 0;
            vert.r = vert.g = vert.b = colour;
            vert.preciseX = sx;
            vert.preciseY = sy;
            vert.viewW = sz;
            vert.gteRecordIndex = static_cast<uint32_t>(snap.gteRecords.size());
            vert.provenance = static_cast<uint8_t>(PsxVertexProvenance::GteTracked);

            GteRecordEntry rec{};
            rec.vx = mv[0];
            rec.vy = mv[1];
            rec.vz = mv[2];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    rec.rt[r * 3 + c] = matrix.rt.m[r][c];
            for (int i = 0; i < 3; ++i)
                rec.tr[i] = matrix.tr[i];
            rec.ofx = matrix.ofx;
            rec.ofy = matrix.ofy;
            rec.h = matrix.h;
            rec.frame = frame;
            rec.seq = static_cast<uint32_t>(snap.gteRecords.size());
            snap.gteRecords.append(rec);
        }
        snap.prims.append(prim);
    }
}

int totalTriangles(const ReconstructedCaptureSet &set)
{
    int tris = 0;
    for (const ReconstructedMesh &mesh : set.uniqueMeshes)
        tris += mesh.triangleCount;
    return tris;
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

/** A slowly turning object: per-frame matrix with a small yaw increment (a new
 *  exact matrix hash every frame) and per-frame sparse triangle subsets that
 *  OVERLAP between consecutive frames — the real capture shape. */
CaptureSnapshot rotatingCubeScene()
{
    CaptureSnapshot snap;
    MatrixRecord m0 = rotationMatrix(0.0, 0.0, 0.0);
    m0.tr[2] = 5000;
    MatrixRecord m1 = rotationMatrix(0.0, 0.05, 0.0);
    m1.tr[2] = 5000;
    MatrixRecord m2 = rotationMatrix(0.0, 0.10, 0.0);
    m2.tr[2] = 5000;
    snap.matrices.append(m0);

    appendCubeFrame(snap, m0, /*frame=*/0, {0, 1, 2, 3, 4, 5}, /*colour=*/100);
    appendCubeFrame(snap, m1, /*frame=*/1, {4, 5, 6, 7, 8, 9}, /*colour=*/150);
    appendCubeFrame(snap, m2, /*frame=*/2, {8, 9, 10, 11, 0, 1}, /*colour=*/200);
    return snap;
}

/** Explicit merge-OFF settings. mergeSameObjectParts now defaults to true
 *  (#412 default-on), so the "raw per-frame" baseline must opt out. */
Ps1NormalizerSettings mergeOff()
{
    Ps1NormalizerSettings s;
    s.mergeSameObjectParts = false;
    return s;
}

} // namespace

TEST(MeshReconstructorMergeTest, MergeOffKeepsOneSparsePartPerFrame)
{
    const CaptureSnapshot snap = rotatingCubeScene();

    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, mergeOff(), &stats);

    // Baseline (#816 behavior): three distinct matrix hashes → three sparse
    // parts, each carrying only its frame's triangles.
    EXPECT_EQ(set.capturedPartCount, 3);
    EXPECT_EQ(stats.mergedPartGroups, 0);
    EXPECT_EQ(stats.duplicateTrianglesDropped, 0);
    EXPECT_EQ(totalTriangles(set), 18) << "3 frames x 6 tris, repeats kept per part";
}

TEST(MeshReconstructorMergeTest, CrossFrameSparsePartsMergeIntoFullObject)
{
    const CaptureSnapshot snap = rotatingCubeScene();

    Ps1NormalizerSettings merge;
    merge.mergeSameObjectParts = true;
    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, merge, &stats);

    // The three per-frame groups share the cube's object-space vertex set, so
    // they cluster into ONE part whose triangle union is the FULL cube —
    // including triangles no single frame carried.
    EXPECT_EQ(set.capturedPartCount, 1);
    EXPECT_EQ(set.uniqueCount(), 1);
    ASSERT_EQ(set.instanceCount(), 1);
    EXPECT_EQ(stats.mergedPartGroups, 2) << "frames 1 and 2 merge into frame 0's group";

    // 18 emitted triangles − 12 unique = 6 once-per-frame repeats dropped
    // (colour differs per frame and must not defeat the duplicate key).
    EXPECT_EQ(stats.duplicateTrianglesDropped, 6);
    EXPECT_EQ(totalTriangles(set), 12);

    // Tier-0 object-space placement stays exact through the merge.
    EXPECT_LE(maxCornerError(set), 0.01f);

    // The merged part keeps a canonical placement matrix — the first frame's.
    EXPECT_TRUE(set.instances[0].hasMatrix);
    float t0[3] = {0.0f, 0.0f, 0.0f};
    GteInverse::modelToEditor(0.0f, 0.0f, 5000.0f, t0[0], t0[1], t0[2]);
    EXPECT_NEAR(set.instances[0].trWorld[0], t0[0], 1e-4f);
    EXPECT_NEAR(set.instances[0].trWorld[1], t0[1], 1e-4f);
    EXPECT_NEAR(set.instances[0].trWorld[2], t0[2], 1e-4f);
}

TEST(MeshReconstructorMergeTest, SameFrameInstancesNeverMerge)
{
    // Two copies of the SAME prop drawn in the SAME frames under different
    // matrices — simultaneous instances. Their object-space vertex sets are
    // identical, so overlap alone would merge them; the frame-disjointness
    // rule must keep them apart so instancing survives.
    CaptureSnapshot snap;
    MatrixRecord matrixA = rotationMatrix(0.0, 0.0, 0.0);
    matrixA.tr[2] = 5000;
    MatrixRecord matrixB = rotationMatrix(0.0, kPi / 2.0, 0.0);
    matrixB.tr[0] = 1500;
    matrixB.tr[2] = 7000;
    snap.matrices.append(matrixA);

    const QVector<int> allTris = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    appendCubeFrame(snap, matrixA, /*frame=*/0, allTris);
    appendCubeFrame(snap, matrixB, /*frame=*/0, allTris);

    Ps1NormalizerSettings merge;
    merge.mergeSameObjectParts = true;
    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, merge, &stats);

    EXPECT_EQ(stats.mergedPartGroups, 0);
    EXPECT_EQ(set.capturedPartCount, 2);
    // Identical model-space geometry still dedupes to one unique mesh with
    // two placed instances — the pre-#412 behavior, preserved.
    EXPECT_EQ(set.uniqueCount(), 1);
    EXPECT_EQ(set.instanceCount(), 2);
}

TEST(MeshReconstructorMergeTest, StaticObjectRepeatsCollapseToOneCopy)
{
    // A static object under a static camera keeps ONE matrix across frames —
    // one group, but every frame re-emits all 12 triangles. The duplicate cull
    // must collapse the repeats even though no group-level merge happens.
    CaptureSnapshot snap;
    MatrixRecord matrix = rotationMatrix(0.0, 0.0, 0.0);
    matrix.tr[2] = 5000;
    snap.matrices.append(matrix);

    const QVector<int> allTris = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    appendCubeFrame(snap, matrix, /*frame=*/0, allTris, /*colour=*/100);
    appendCubeFrame(snap, matrix, /*frame=*/1, allTris, /*colour=*/220);

    Ps1NormalizerSettings merge;
    merge.mergeSameObjectParts = true;
    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, merge, &stats);

    EXPECT_EQ(set.capturedPartCount, 1);
    EXPECT_EQ(stats.mergedPartGroups, 0) << "same matrix hash = one group, no merge needed";
    EXPECT_EQ(stats.duplicateTrianglesDropped, 12);
    EXPECT_EQ(totalTriangles(set), 12);

    // Merge off: both frames' copies survive (the pre-#412 baseline).
    MeshReconstructionStats rawStats;
    const ReconstructedCaptureSet raw =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, mergeOff(), &rawStats);
    EXPECT_EQ(totalTriangles(raw), 24);
}

TEST(MeshReconstructorMergeTest, RamScanCaptureIsUntouchedByMerge)
{
    // No records / all-None provenance (RAM-scan world): there is no
    // object-space identity to cluster on — the merge must be a no-op, not a
    // crash or an accidental legacy-group collapse.
    CaptureSnapshot snap;
    MatrixRecord matrix = rotationMatrix(0.0, 0.0, 0.0);
    matrix.tr[2] = 5000;
    snap.matrices.append(matrix);
    appendCubeFrame(snap, matrix, /*frame=*/0, {0, 1, 2, 3});
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

    Ps1NormalizerSettings merge;
    merge.mergeSameObjectParts = true;
    MeshReconstructionStats mergeStats;
    const ReconstructedCaptureSet merged =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, merge, &mergeStats);

    MeshReconstructionStats rawStats;
    const ReconstructedCaptureSet raw =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, mergeOff(), &rawStats);

    EXPECT_EQ(mergeStats.mergedPartGroups, 0);
    EXPECT_EQ(merged.capturedPartCount, raw.capturedPartCount);
    EXPECT_EQ(totalTriangles(merged), totalTriangles(raw));
}

TEST(MeshReconstructorMergeTest, WarpingObjectCollapsesToRepresentativeFrame)
{
    // Vertex-ANIMATED object (#412 stage 2): the game recomputes object-space
    // verts every frame (warping menu text, breathing characters), so exact
    // vertex keys never match across frames and the rigid stage leaves one
    // part per frame. The continuity stage (texture keys + prim count + draw
    // order + coarse spatial overlap) must chain those frames and keep ONE
    // representative frame — not a triangle union, which would superimpose
    // every warp phase into ghost soup.
    CaptureSnapshot snap;
    MatrixRecord m0 = rotationMatrix(0.0, 0.0, 0.0);
    m0.tr[2] = 5000;
    MatrixRecord m1 = rotationMatrix(0.0, 0.05, 0.0);
    m1.tr[2] = 5000;
    MatrixRecord m2 = rotationMatrix(0.0, 0.10, 0.0);
    m2.tr[2] = 5000;
    snap.matrices.append(m0);

    const QVector<int> allTris = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    appendCubeFrame(snap, m0, /*frame=*/0, allTris, 128, /*warpX=*/0);
    appendCubeFrame(snap, m1, /*frame=*/1, allTris, 128, /*warpX=*/48);
    appendCubeFrame(snap, m2, /*frame=*/2, allTris, 128, /*warpX=*/96);

    // Baseline: the warp defeats the rigid stage — 3 parts without stage 2.
    MeshReconstructionStats rawStats;
    const ReconstructedCaptureSet raw =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, mergeOff(), &rawStats);
    EXPECT_EQ(raw.capturedPartCount, 3);

    Ps1NormalizerSettings merge;
    merge.mergeSameObjectParts = true;
    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, merge, &stats);

    EXPECT_EQ(stats.mergedPartGroups, 0) << "warped frames must not rigid-merge";
    EXPECT_EQ(stats.nonRigidMergedGroups, 2) << "frames 1 and 2 drop for the representative";
    EXPECT_EQ(set.capturedPartCount, 1);
    EXPECT_EQ(set.uniqueCount(), 1);
    EXPECT_EQ(totalTriangles(set), 12) << "exactly one frame's copy survives";
    // Prim counts tie (12 each), so the representative is the EARLIEST frame —
    // frame 0 is unwarped, so the kept geometry is the clean cube.
    EXPECT_LE(maxCornerError(set), 0.01f);
}

TEST(MeshReconstructorMergeTest, DifferentTexturesNeverChainAcrossFrames)
{
    // Two different vertex-animated objects alternating frames: A (tpage 1,
    // frames 0/2) and B (tpage 2, offset model, frames 1/3). Each must chain
    // with ITSELF across its own frames (gap 2 <= 3), never with the other —
    // the texture-key gate is what separates two animated objects that share
    // draw-order rank.
    CaptureSnapshot snap;
    MatrixRecord m = rotationMatrix(0.0, 0.0, 0.0);
    m.tr[2] = 5000;
    snap.matrices.append(m);
    MatrixRecord mB = rotationMatrix(0.0, 0.02, 0.0);
    mB.tr[2] = 6000;
    MatrixRecord mA2 = rotationMatrix(0.0, 0.04, 0.0);
    mA2.tr[2] = 5000;
    MatrixRecord mB2 = rotationMatrix(0.0, 0.06, 0.0);
    mB2.tr[2] = 6000;

    const QVector<int> allTris = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    appendCubeFrame(snap, m, /*frame=*/0, allTris, 128, /*warpX=*/0, /*tpage=*/1);
    appendCubeFrame(snap, mB, /*frame=*/1, allTris, 128, /*warpX=*/16, /*tpage=*/2,
                    /*vzOffset=*/2000);
    appendCubeFrame(snap, mA2, /*frame=*/2, allTris, 128, /*warpX=*/48, /*tpage=*/1);
    appendCubeFrame(snap, mB2, /*frame=*/3, allTris, 128, /*warpX=*/64, /*tpage=*/2,
                    /*vzOffset=*/2000);

    Ps1NormalizerSettings merge;
    merge.mergeSameObjectParts = true;
    MeshReconstructionStats stats;
    const ReconstructedCaptureSet set =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose, merge, &stats);

    // One representative per object: A keeps frame 0, B keeps frame 1.
    EXPECT_EQ(stats.nonRigidMergedGroups, 2);
    EXPECT_EQ(set.capturedPartCount, 2);
    EXPECT_EQ(totalTriangles(set), 24) << "12 tris per surviving object";
}

#endif // ENABLE_PS1_RIP
