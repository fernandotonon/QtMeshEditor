#include "CaptureSnapshot.h"
#include "MeshReconstructor.h"

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
