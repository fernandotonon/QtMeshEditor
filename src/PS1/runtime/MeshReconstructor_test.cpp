#include "CaptureSnapshot.h"
#include "MeshReconstructor.h"
#include "MeshTopologyHash.h"
#include "PsxCaptureFilters.h"

#include <gtest/gtest.h>

#include <cmath>

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

TEST(MeshReconstructorTest, PreservesTexturedTriangleUvs)
{
    CaptureSnapshot snap;
    snap.matrices.append(identityMatrix());

    PrimRecord tri{};
    tri.kind = PrimKind::TexturedTri;
    tri.vertexCount = 3;
    tri.matrixId = 0;
    tri.tpage = 0x0100;
    tri.clut = 0x0200;
    tri.verts[0] = {16, 16, 4096, 255, 255, 255, 32, 48};
    tri.verts[1] = {48, 16, 4096, 255, 255, 255, 160, 16};
    tri.verts[2] = {32, 40, 4096, 255, 255, 255, 96, 200};
    snap.prims.append(tri);

    const ReconstructedMesh mesh = MeshReconstructor::reconstruct(snap);
    ASSERT_FALSE(mesh.isEmpty());
    ASSERT_EQ(mesh.subMeshes.size(), 1);

    const auto uvMatches = [](const ReconstructedVertex &v, int psxU, int psxV) {
        return std::abs(v.u - static_cast<float>(psxU) / 256.0f) < 1e-5f
               && std::abs(v.v - static_cast<float>(psxV) / 256.0f) < 1e-5f;
    };

    int matched = 0;
    for (const ReconstructedVertex &v : mesh.subMeshes[0].vertices) {
        if (uvMatches(v, 32, 48))
            ++matched;
        if (uvMatches(v, 160, 16))
            ++matched;
        if (uvMatches(v, 96, 200))
            ++matched;
    }
    EXPECT_EQ(matched, 3);
}

TEST(MeshReconstructorTest, PreservesTexturedQuadUvOrder)
{
    CaptureSnapshot snap;
    snap.matrices.append(identityMatrix());

    PrimRecord quad{};
    quad.kind = PrimKind::TexturedQuad;
    quad.vertexCount = 4;
    quad.matrixId = 0;
    quad.tpage = 0;
    quad.clut = 0;
    quad.verts[0] = {10, 10, 0, 255, 255, 255, 0, 0};
    quad.verts[1] = {40, 10, 0, 255, 255, 255, 255, 0};
    quad.verts[2] = {40, 40, 0, 255, 255, 255, 255, 255};
    quad.verts[3] = {10, 40, 0, 255, 255, 255, 0, 255};
    snap.prims.append(quad);

    const ReconstructedMesh mesh = MeshReconstructor::reconstruct(snap);
    ASSERT_EQ(mesh.subMeshes.size(), 1);
    ASSERT_EQ(mesh.subMeshes[0].indices.size(), 6u);

    const QVector<ReconstructedVertex> &verts = mesh.subMeshes[0].vertices;
    ASSERT_EQ(verts.size(), 6);

    const auto near = [](float a, float b) { return std::abs(a - b) < 1e-5f; };
    EXPECT_TRUE(near(verts[0].u, 0.0f) && near(verts[0].v, 0.0f));
    EXPECT_TRUE(near(verts[1].u, 1.0f) && near(verts[1].v, 0.0f));
    EXPECT_TRUE(near(verts[2].u, 1.0f) && near(verts[2].v, 1.0f));
    EXPECT_TRUE(near(verts[3].u, 0.0f) && near(verts[3].v, 0.0f));
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
