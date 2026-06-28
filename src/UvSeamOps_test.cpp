#include <gtest/gtest.h>

#include "EditableMesh.h"
#include "UvSeamData.h"
#include "UvSeamOps.h"

namespace {

EditableMesh makeSplitQuad()
{
    EditableMesh mesh;
    EditableSubMesh sub;
    sub.vertices = {
        EditableVertex{.position = {0, 0, 0}, .uv = {0, 0}, .hasUV = true},
        EditableVertex{.position = {1, 0, 0}, .uv = {1, 0}, .hasUV = true},
        EditableVertex{.position = {0, 1, 0}, .uv = {0, 1}, .hasUV = true},
        EditableVertex{.position = {1, 1, 0}, .uv = {1, 1}, .hasUV = true},
    };
    sub.triangles = {
        EditableTriangle{.indices = {0, 1, 2}},
        EditableTriangle{.indices = {1, 3, 2}},
    };
    mesh.subMeshes().push_back(std::move(sub));
    return mesh;
}

} // namespace

TEST(UvSeamOpsTest, SplitEdgeDuplicatesVertsAndMarksSeam)
{
    EditableMesh mesh = makeSplitQuad();
    const size_t before = mesh.subMeshes()[0].vertices.size();
    const auto key = UvSeamData::makeEdgeKey(0, 1);

    const auto result = UvSeamOps::splitEdges(mesh, 0, {key});
    EXPECT_TRUE(result.applied);
    EXPECT_EQ(result.edgesSplit, 1);
    EXPECT_EQ(result.vertsAdded, 2);
    EXPECT_EQ(mesh.subMeshes()[0].vertices.size(), before + 2);
    EXPECT_TRUE(UvSeamData::isSeam(mesh.subMeshes()[0], 0, 1));
}

TEST(UvSeamOpsTest, SewAveragesCoincidentUvPositions)
{
    EditableMesh mesh;
    EditableSubMesh sub;
    // Two verts share position (0,0,0) but have different UVs — classic seam dupes.
    sub.vertices = {
        EditableVertex{.position = {0, 0, 0}, .uv = {0.0f, 0.0f}, .hasUV = true},
        EditableVertex{.position = {1, 0, 0}, .uv = {1.0f, 0.0f}, .hasUV = true},
        EditableVertex{.position = {0, 0, 0}, .uv = {0.5f, 0.0f}, .hasUV = true},
        EditableVertex{.position = {0, 1, 0}, .uv = {0.0f, 1.0f}, .hasUV = true},
    };
    sub.triangles = {
        EditableTriangle{.indices = {0, 1, 3}},
        EditableTriangle{.indices = {2, 1, 3}},
    };
    const auto key = UvSeamData::makeEdgeKey(0, 2);
    UvSeamData::setSeam(sub, 0, 2, true);
    mesh.subMeshes().push_back(std::move(sub));

    const auto result = UvSeamOps::sewEdges(mesh, 0, {key});
    EXPECT_TRUE(result.applied);
    EXPECT_EQ(result.edgesSewn, 1);
    EXPECT_FALSE(UvSeamData::isSeam(mesh.subMeshes()[0], 0, 2));

    const float u0 = mesh.subMeshes()[0].vertices[0].uv.x;
    const float u2 = mesh.subMeshes()[0].vertices[2].uv.x;
    EXPECT_NEAR(u0, 0.25f, 1e-5f);
    EXPECT_NEAR(u2, 0.25f, 1e-5f);
}

TEST(UvSeamOpsTest, LocalEdgeKeysRequireSameSubmesh)
{
    EditableMesh mesh;
    EditableSubMesh a;
    a.vertices.resize(3);
    a.triangles.push_back(EditableTriangle{.indices = {0, 1, 2}});
    EditableSubMesh b;
    b.vertices.resize(3);
    b.triangles.push_back(EditableTriangle{.indices = {0, 1, 2}});
    mesh.subMeshes().push_back(std::move(a));
    mesh.subMeshes().push_back(std::move(b));

    size_t sub = 0;
    const auto keys = UvSeamOps::localEdgeKeysFromGlobal(
        mesh, {{0, 1}, {3, 4}}, sub);
    EXPECT_TRUE(keys.empty());
}
