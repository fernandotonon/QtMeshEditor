#include "CaptureSnapshot.h"
#include "MeshReconstructor.h"
#include "MeshTopologyHash.h"
#include "PsxCaptureFilters.h"

#include <gtest/gtest.h>

static MatrixRecord identityMatrix()
{
    MatrixRecord m{};
    m.rt.m[0][0] = 1 << 12;
    m.rt.m[1][1] = 1 << 12;
    m.rt.m[2][2] = 1 << 12;
    m.h = 256;
    return m;
}

static PrimRecord coloredTri(int x0, int y0, uint32_t matrixId)
{
    PrimRecord prim{};
    prim.kind = PrimKind::MonoTri;
    prim.vertexCount = 3;
    prim.matrixId = matrixId;
    prim.tpage = 0x100;
    prim.clut = 0x200;
    prim.verts[0] = {x0, y0, 0, 255, 0, 0, 0, 0};
    prim.verts[1] = {x0 + 32, y0, 0, 0, 255, 0, 0, 0};
    prim.verts[2] = {x0 + 16, y0 + 24, 0, 0, 0, 255, 0, 0};
    return prim;
}

TEST(MeshReconstructorTest, GroupsByMatrixAndTexture)
{
    CaptureSnapshot snap;
    snap.matrices.append(identityMatrix());
    MatrixRecord other = identityMatrix();
    other.tr[0] = 1024;
    snap.matrices.append(other);

    snap.prims.append(coloredTri(16, 16, 0));
    snap.prims.append(coloredTri(80, 16, 1));

    const ReconstructedMesh mesh = MeshReconstructor::reconstruct(snap);
    EXPECT_FALSE(mesh.isEmpty());
    EXPECT_EQ(mesh.subMeshes.size(), 2);
    EXPECT_GE(mesh.vertexCount, 6);
    EXPECT_GE(mesh.triangleCount, 2);
}

TEST(MeshReconstructorTest, TriangulatesQuad)
{
    CaptureSnapshot snap;
    snap.matrices.append(identityMatrix());

    PrimRecord quad{};
    quad.kind = PrimKind::MonoQuad;
    quad.vertexCount = 4;
    quad.matrixId = 0;
    quad.tpage = 0;
    quad.clut = 0;
    quad.verts[0] = {10, 10, 0, 255, 255, 255, 0, 0};
    quad.verts[1] = {40, 10, 0, 255, 255, 255, 0, 0};
    quad.verts[2] = {40, 40, 0, 255, 255, 255, 0, 0};
    quad.verts[3] = {10, 40, 0, 255, 255, 255, 0, 0};
    snap.prims.append(quad);

    const ReconstructedMesh mesh = MeshReconstructor::reconstruct(snap);
    ASSERT_EQ(mesh.subMeshes.size(), 1);
    EXPECT_EQ(mesh.subMeshes[0].indices.size(), 6u);
    EXPECT_EQ(mesh.triangleCount, 2);
}

TEST(MeshReconstructorTest, EmptySnapshotReturnsEmpty)
{
    const ReconstructedMesh mesh = MeshReconstructor::reconstruct(CaptureSnapshot{});
    EXPECT_TRUE(mesh.isEmpty());
}

TEST(MeshReconstructorTest, IgnoresOffscreenPrimitives)
{
    CaptureSnapshot snap;
    snap.matrices.append(identityMatrix());

    PrimRecord offscreen = coloredTri(16, 16, 0);
    for (int v = 0; v < 3; ++v) {
        offscreen.verts[v].x = 50000;
        offscreen.verts[v].y = 50000;
    }
    snap.prims.append(offscreen);
    snap.prims.append(coloredTri(32, 32, 0));

    const ReconstructedMesh mesh = MeshReconstructor::reconstruct(snap);
    ASSERT_FALSE(mesh.isEmpty());
    EXPECT_EQ(mesh.triangleCount, 1);
}

TEST(MeshReconstructorTest, DedupesIdenticalInstances)
{
    CaptureSnapshot snap;
    snap.matrices.append(identityMatrix());
    MatrixRecord shifted = identityMatrix();
    shifted.tr[0] = 512;
    snap.matrices.append(shifted);
    MatrixRecord shifted2 = identityMatrix();
    shifted2.tr[0] = 1024;
    snap.matrices.append(shifted2);

    snap.prims.append(coloredTri(16, 16, 0));
    snap.prims.append(coloredTri(80, 16, 1));
    snap.prims.append(coloredTri(144, 16, 2));

    const ReconstructedCaptureSet loose =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose);
    EXPECT_EQ(loose.capturedPartCount, 3);
    EXPECT_EQ(loose.uniqueCount(), 1);
    EXPECT_EQ(loose.instanceCount(), 3);
    EXPECT_EQ(loose.instances[0].uniqueMeshIndex, 0);
    EXPECT_EQ(loose.instances[1].uniqueMeshIndex, 0);
    EXPECT_EQ(loose.instances[2].uniqueMeshIndex, 0);
}

TEST(MeshReconstructorTest, KeepsPartiallyOnScreenPrimitives)
{
    PrimRecord clip{};
    clip.kind = PrimKind::MonoTri;
    clip.vertexCount = 3;
    clip.verts[0] = {0, 120, 0, 255, 255, 255, 0, 0};
    clip.verts[1] = {400, 120, 0, 255, 255, 255, 0, 0};
    clip.verts[2] = {160, 200, 0, 255, 255, 255, 0, 0};
    EXPECT_TRUE(PsxCaptureFilters::isOnScreenPrim(clip));

    CaptureSnapshot snap;
    snap.matrices.append(identityMatrix());
    snap.prims.append(clip);

    const ReconstructedMesh mesh = MeshReconstructor::reconstruct(snap);
    EXPECT_FALSE(mesh.isEmpty());
    EXPECT_EQ(mesh.triangleCount, 1);
}

// #424 acceptance: with perspective-correct UVs ON, a textured quad whose
// vertex depths vary by more than the tolerance gets subdivided into a
// fan of smaller triangles. New midpoint UVs follow the PS1 affine
// convention so Ogre's perspective-correct rendering reproduces the
// artist's intent — the "warped quad fix" used by modern PSX remasters.
TEST(MeshReconstructorTest, PerspectiveCorrectSubdivisionTessellatesWarpedQuad)
{
    CaptureSnapshot snap;
    snap.matrices.append(identityMatrix());

    PrimRecord warped{};
    warped.kind = PrimKind::TexturedQuad;
    warped.vertexCount = 4;
    warped.matrixId = 0;
    warped.tpage = 0x100;
    warped.clut = 0x200;
    // A receding wall: near corners at z=200, far corners at z=2000 — a 10x
    // depth ratio, well above the default 1.3 tolerance. Linear UV ramp 0..1
    // along the depth axis is the canonical "warpy texture" PS1 case.
    warped.verts[0] = {40,  60, 200,  255, 255, 255, 0,   0};
    warped.verts[1] = {200, 60, 2000, 255, 255, 255, 255, 0};
    warped.verts[2] = {200, 180, 2000, 255, 255, 255, 255, 255};
    warped.verts[3] = {40,  180, 200, 255, 255, 255, 0,   255};
    snap.prims.append(warped);

    // Baseline: no subdivision → 2 triangles per quad.
    const ReconstructedCaptureSet noPC = MeshReconstructor::reconstructDeduped(
        snap, MeshDedupeMode::Loose, Ps1NormalizerSettings{}, nullptr);
    int baselineTris = 0;
    for (const auto &m : noPC.uniqueMeshes) baselineTris += m.triangleCount;
    EXPECT_EQ(baselineTris, 2);

    // Perspective-correct ON: every input tri whose depth ratio > tolerance
    // gets split into 4. At depth=3 with a 10x z-ratio, recursion fires all
    // the way down so each input tri becomes ~64 sub-tris (128 total).
    Ps1NormalizerSettings pc;
    pc.perspectiveCorrectUVs = true;
    pc.perspectiveTolerance = 1.3f;
    pc.perspectiveMaxDepth = 3;
    const ReconstructedCaptureSet withPC = MeshReconstructor::reconstructDeduped(
        snap, MeshDedupeMode::Loose, pc, nullptr);
    int pcTris = 0;
    for (const auto &m : withPC.uniqueMeshes) pcTris += m.triangleCount;
    EXPECT_GT(pcTris, baselineTris);
    EXPECT_LE(pcTris, 2 * 64); // recursion is depth-bounded — can't blow up unboundedly
}

// Companion: when perspective-correct is on but the prim has uniform depth
// (e.g. a flat HUD or near-camera billboard), subdivision should be a no-op.
TEST(MeshReconstructorTest, PerspectiveCorrectKeepsFlatPrimsUntouched)
{
    CaptureSnapshot snap;
    snap.matrices.append(identityMatrix());
    PrimRecord flat{};
    flat.kind = PrimKind::TexturedQuad;
    flat.vertexCount = 4;
    flat.matrixId = 0;
    flat.tpage = 0x100;
    flat.clut = 0x200;
    flat.verts[0] = {40,  60,  500, 255, 255, 255, 0, 0};
    flat.verts[1] = {200, 60,  500, 255, 255, 255, 0, 0};
    flat.verts[2] = {200, 180, 500, 255, 255, 255, 0, 0};
    flat.verts[3] = {40,  180, 500, 255, 255, 255, 0, 0};
    snap.prims.append(flat);

    Ps1NormalizerSettings pc;
    pc.perspectiveCorrectUVs = true;
    const ReconstructedCaptureSet result = MeshReconstructor::reconstructDeduped(
        snap, MeshDedupeMode::Loose, pc, nullptr);
    int tris = 0;
    for (const auto &m : result.uniqueMeshes) tris += m.triangleCount;
    EXPECT_EQ(tris, 2);
}

// Perspective-correct subdivision only makes sense for textured prims —
// mono / shaded prims have no UV channel, so subdividing them just inflates
// triangle counts without changing the visual. Verify a high-depth-variance
// MONO prim with the toggle ON stays a single triangle (CodeRabbit Major on
// the original #424 PR).
TEST(MeshReconstructorTest, PerspectiveCorrectSkipsMonoPrims)
{
    CaptureSnapshot snap;
    snap.matrices.append(identityMatrix());
    PrimRecord mono{};
    mono.kind = PrimKind::MonoTri;
    mono.vertexCount = 3;
    mono.matrixId = 0;
    // High depth variance — would trigger subdivision if the gate were absent.
    mono.verts[0] = {40,  60,  200,  255, 0, 0, 0, 0};
    mono.verts[1] = {200, 60,  2000, 0, 255, 0, 0, 0};
    mono.verts[2] = {120, 180, 1000, 0, 0, 255, 0, 0};
    snap.prims.append(mono);

    Ps1NormalizerSettings pc;
    pc.perspectiveCorrectUVs = true;
    const ReconstructedCaptureSet result = MeshReconstructor::reconstructDeduped(
        snap, MeshDedupeMode::Loose, pc, nullptr);
    int tris = 0;
    for (const auto &m : result.uniqueMeshes) tris += m.triangleCount;
    EXPECT_EQ(tris, 1);
}

// Perspective-correct must gracefully handle GP0-only captures where sz=0
// for every vertex (the #675 "no depth" path). Subdivision is skipped — no
// way to compute a meaningful depth ratio — and the prim renders as-is.
TEST(MeshReconstructorTest, PerspectiveCorrectGracefullyHandlesZeroDepth)
{
    CaptureSnapshot snap;
    snap.matrices.append(identityMatrix());
    // Textured prim so the depth-variance gate is reached (the textured-only
    // gate above only short-circuits mono / shaded prims). z=0 on all three
    // vertices simulates the GP0-only capture case (#675) — no usable depth
    // for the depth-ratio test, so subdivision must skip cleanly without
    // dividing by zero or emitting a degenerate sub-triangle.
    PrimRecord prim{};
    prim.kind = PrimKind::TexturedTri;
    prim.vertexCount = 3;
    prim.matrixId = 0;
    prim.tpage = 0x100;
    prim.clut = 0x200;
    prim.verts[0] = {16,  16, 0, 255, 255, 255, 0, 0};
    prim.verts[1] = {48,  16, 0, 255, 255, 255, 255, 0};
    prim.verts[2] = {32,  48, 0, 255, 255, 255, 128, 255};
    snap.prims.append(prim);

    Ps1NormalizerSettings pc;
    pc.perspectiveCorrectUVs = true;
    const ReconstructedCaptureSet result = MeshReconstructor::reconstructDeduped(
        snap, MeshDedupeMode::Loose, pc, nullptr);
    int tris = 0;
    for (const auto &m : result.uniqueMeshes) tris += m.triangleCount;
    EXPECT_EQ(tris, 1);
}
