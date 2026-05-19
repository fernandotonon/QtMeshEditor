#include "MeshReconstructor.h"
#include "MeshTopologyHash.h"

#include <gtest/gtest.h>

static ReconstructedMesh unitTriangleAt(float ox, float oy, float oz)
{
    ReconstructedMesh mesh;
    ReconstructedSubMesh sub;
    sub.materialName = QStringLiteral("PS1Rip/tpage_0000_clut_0000");
    sub.vertices = {
        {ox, oy, oz, 0, 0, 1, 0, 0, 0xFFFFFFFFu},
        {ox + 1.0f, oy, oz, 0, 0, 1, 0, 0, 0xFFFFFFFFu},
        {ox, oy + 1.0f, oz, 0, 0, 1, 0, 0, 0xFFFFFFFFu},
    };
    sub.indices = {0, 1, 2};
    mesh.subMeshes.append(sub);
    mesh.vertexCount = 3;
    mesh.triangleCount = 1;
    return mesh;
}

TEST(MeshTopologyHashTest, LooseModeIgnoresSmallTranslation)
{
    const ReconstructedMesh a = unitTriangleAt(0.0f, 0.0f, 0.0f);
    const ReconstructedMesh b = unitTriangleAt(0.005f, 0.005f, 0.0f);

    float cx = 0.0f;
    float cy = 0.0f;
    float cz = 0.0f;
    const ReconstructedMesh ca = MeshTopologyHash::centered(a, cx, cy, cz);
    const ReconstructedMesh cb = MeshTopologyHash::centered(b, cx, cy, cz);

    EXPECT_EQ(MeshTopologyHash::hashMesh(ca, MeshDedupeMode::Loose),
              MeshTopologyHash::hashMesh(cb, MeshDedupeMode::Loose));
}

TEST(MeshTopologyHashTest, StrictModeSeparatesSmallTranslation)
{
    const ReconstructedMesh a = unitTriangleAt(0.0f, 0.0f, 0.0f);
    const ReconstructedMesh b = unitTriangleAt(0.05f, 0.0f, 0.0f);

    float cx = 0.0f;
    float cy = 0.0f;
    float cz = 0.0f;
    const ReconstructedMesh ca = MeshTopologyHash::centered(a, cx, cy, cz);
    const ReconstructedMesh cb = MeshTopologyHash::centered(b, cx, cy, cz);

    EXPECT_NE(MeshTopologyHash::hashMesh(ca, MeshDedupeMode::Strict),
              MeshTopologyHash::hashMesh(cb, MeshDedupeMode::Strict));
}
