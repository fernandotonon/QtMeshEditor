#include "CaptureSnapshot.h"
#include "GteInverse.h"
#include "MeshReconstructor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace {

MatrixRecord unitCubeMatrix()
{
    MatrixRecord m{};
    m.rt.m[0][0] = 1 << 12;
    m.rt.m[1][1] = 1 << 12;
    m.rt.m[2][2] = 1 << 12;
    m.ofx = 160 << 16;
    m.ofy = 120 << 16;
    m.h = 256;
    return m;
}

/** Model-space cube corners (PS1 1/4096 units), axis-aligned. */
void modelCubeCorners(int &x0, int &y0, int &z0, int &x1, int &y1, int &z1)
{
    x0 = -80 * 4096;
    y0 = -80 * 4096;
    z0 = 4096;
    x1 = 80 * 4096;
    y1 = 80 * 4096;
    z1 = 3 * 4096;
}

PrimRecord triFromScreen(const MatrixRecord &matrix, int sx0, int sy0, int sz0, int sx1, int sy1,
                         int sz1, int sx2, int sy2, int sz2)
{
    PrimRecord prim{};
    prim.kind = PrimKind::MonoTri;
    prim.vertexCount = 3;
    prim.matrixId = 0;
    prim.verts[0] = {sx0, sy0, sz0, 200, 80, 80, 0, 0};
    prim.verts[1] = {sx1, sy1, sz1, 200, 80, 80, 0, 0};
    prim.verts[2] = {sx2, sy2, sz2, 200, 80, 80, 0, 0};
    (void)matrix;
    return prim;
}

void appendProjectedCubeFace(CaptureSnapshot &snap, const MatrixRecord &matrix, int mx0, int my0,
                            int mz0, int mx1, int my1, int mz1, int mx2, int my2, int mz2)
{
    int sx0 = 0;
    int sy0 = 0;
    int sz0 = 0;
    int sx1 = 0;
    int sy1 = 0;
    int sz1 = 0;
    int sx2 = 0;
    int sy2 = 0;
    int sz2 = 0;
    ASSERT_TRUE(GteInverse::modelToScreen(matrix, mx0, my0, mz0, sx0, sy0, sz0));
    ASSERT_TRUE(GteInverse::modelToScreen(matrix, mx1, my1, mz1, sx1, sy1, sz1));
    ASSERT_TRUE(GteInverse::modelToScreen(matrix, mx2, my2, mz2, sx2, sy2, sz2));
    snap.prims.append(triFromScreen(matrix, sx0, sy0, sz0, sx1, sy1, sz1, sx2, sy2, sz2));
}

void appendCapturedCube(CaptureSnapshot &snap)
{
    const MatrixRecord matrix = unitCubeMatrix();
    snap.matrices.append(matrix);
    snap.cameraMatrixId = 0;

    int x0 = 0;
    int y0 = 0;
    int z0 = 0;
    int x1 = 0;
    int y1 = 0;
    int z1 = 0;
    modelCubeCorners(x0, y0, z0, x1, y1, z1);

    appendProjectedCubeFace(snap, matrix, x0, y0, z0, x1, y0, z0, x1, y1, z0);
    appendProjectedCubeFace(snap, matrix, x0, y0, z0, x1, y1, z0, x0, y1, z0);
    appendProjectedCubeFace(snap, matrix, x0, y0, z1, x1, y0, z1, x1, y1, z1);
    appendProjectedCubeFace(snap, matrix, x0, y0, z1, x1, y1, z1, x0, y1, z1);
    appendProjectedCubeFace(snap, matrix, x0, y0, z0, x0, y0, z1, x0, y1, z1);
    appendProjectedCubeFace(snap, matrix, x0, y0, z0, x0, y1, z1, x0, y1, z0);
    appendProjectedCubeFace(snap, matrix, x1, y0, z0, x1, y0, z1, x1, y1, z1);
    appendProjectedCubeFace(snap, matrix, x1, y0, z0, x1, y1, z1, x1, y1, z0);
    appendProjectedCubeFace(snap, matrix, x0, y0, z0, x0, y0, z1, x1, y0, z1);
    appendProjectedCubeFace(snap, matrix, x0, y0, z0, x1, y0, z1, x1, y0, z0);
    appendProjectedCubeFace(snap, matrix, x0, y1, z0, x0, y1, z1, x1, y1, z1);
    appendProjectedCubeFace(snap, matrix, x0, y1, z0, x1, y1, z1, x1, y1, z0);
}

struct Bounds {
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
    float minZ = 0.0f;
    float maxZ = 0.0f;
};

Bounds meshBounds(const ReconstructedMesh &mesh)
{
    Bounds bounds{};
    bool first = true;
    for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
        for (const ReconstructedVertex &vertex : sub.vertices) {
            if (first) {
                bounds.minX = bounds.maxX = vertex.px;
                bounds.minY = bounds.maxY = vertex.py;
                bounds.minZ = bounds.maxZ = vertex.pz;
                first = false;
            } else {
                bounds.minX = std::min(bounds.minX, vertex.px);
                bounds.maxX = std::max(bounds.maxX, vertex.px);
                bounds.minY = std::min(bounds.minY, vertex.py);
                bounds.maxY = std::max(bounds.maxY, vertex.py);
                bounds.minZ = std::min(bounds.minZ, vertex.pz);
                bounds.maxZ = std::max(bounds.maxZ, vertex.pz);
            }
        }
    }
    return bounds;
}

} // namespace

TEST(MeshReconstructorTest, GtePipelineCubeRoundTripsToUnitCubeMesh)
{
    CaptureSnapshot snap;
    appendCapturedCube(snap);

    ASSERT_EQ(snap.matrices.size(), 1);
    ASSERT_GE(snap.prims.size(), 8);

    const ReconstructedMesh mesh = MeshReconstructor::reconstruct(snap);
    ASSERT_FALSE(mesh.isEmpty());
    EXPECT_GE(mesh.triangleCount, 8);

    const Bounds bounds = meshBounds(mesh);
    const float extentX = bounds.maxX - bounds.minX;
    const float extentY = bounds.maxY - bounds.minY;
    const float extentZ = bounds.maxZ - bounds.minZ;

    EXPECT_GT(extentX, 0.8f);
    EXPECT_GT(extentY, 0.8f);
    EXPECT_GT(extentZ, 0.08f);

    const float maxExtent = std::max(extentX, std::max(extentY, extentZ));
    const float minExtent = std::min(extentX, std::min(extentY, extentZ));
    EXPECT_NEAR(maxExtent / minExtent, 1.0f, 0.6f);
}
