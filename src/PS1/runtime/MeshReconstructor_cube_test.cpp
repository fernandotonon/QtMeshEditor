#include "CaptureSnapshot.h"
#include "MeshReconstructor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace {

PrimRecord screenTri(int x0, int y0, int z0, int x1, int y1, int z1, int x2, int y2, int z2)
{
    PrimRecord out{};
    out.kind = PrimKind::MonoTri;
    out.vertexCount = 3;
    out.verts[0] = {x0, y0, z0, 255, 255, 255, 0, 0};
    out.verts[1] = {x1, y1, z1, 255, 255, 255, 0, 0};
    out.verts[2] = {x2, y2, z2, 255, 255, 255, 0, 0};
    return out;
}

/** Screen-space cube (PSX coords) — exercises capture → reconstruct without GTE projection. */
void appendCubeFaces(CaptureSnapshot &snap)
{
    constexpr int l = 48;
    constexpr int r = 208;
    constexpr int t = 48;
    constexpr int b = 168;
    constexpr int zNear = 4096;
    constexpr int zFar = 8192;

    auto tri = [&](int x0, int y0, int z0, int x1, int y1, int z1, int x2, int y2, int z2) {
        snap.prims.append(screenTri(x0, y0, z0, x1, y1, z1, x2, y2, z2));
    };

    tri(l, t, zNear, r, t, zNear, r, b, zNear);
    tri(l, t, zNear, r, b, zNear, l, b, zNear);
    tri(l, t, zFar, r, t, zFar, r, b, zFar);
    tri(l, t, zFar, r, b, zFar, l, b, zFar);
    tri(l, t, zNear, l, t, zFar, l, b, zFar);
    tri(l, t, zNear, l, b, zFar, l, b, zNear);
    tri(r, t, zNear, r, t, zFar, r, b, zFar);
    tri(r, t, zNear, r, b, zFar, r, b, zNear);
    tri(l, t, zNear, l, t, zFar, r, t, zFar);
    tri(l, t, zNear, r, t, zFar, r, t, zNear);
    tri(l, b, zNear, l, b, zFar, r, b, zFar);
    tri(l, b, zNear, r, b, zFar, r, b, zNear);
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

TEST(MeshReconstructorTest, CapturedCubeRoundTripsToUnitCubeMesh)
{
    CaptureSnapshot snap;
    appendCubeFaces(snap);

    const ReconstructedMesh mesh = MeshReconstructor::reconstruct(snap);
    ASSERT_FALSE(mesh.isEmpty());
    EXPECT_GE(mesh.triangleCount, 8);

    const Bounds bounds = meshBounds(mesh);
    const float extentX = bounds.maxX - bounds.minX;
    const float extentY = bounds.maxY - bounds.minY;
    const float extentZ = bounds.maxZ - bounds.minZ;

    EXPECT_GT(extentX, 0.5f);
    EXPECT_GT(extentY, 0.5f);
    EXPECT_GT(extentZ, 0.1f);

    const float maxExtent = std::max(extentX, std::max(extentY, extentZ));
    const float minExtent = std::min(extentX, std::min(extentY, extentZ));
    EXPECT_GT(maxExtent, 1.0f);
    EXPECT_GT(minExtent, 0.5f);
}
