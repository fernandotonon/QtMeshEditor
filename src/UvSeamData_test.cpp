#include <gtest/gtest.h>

#include "EditableMesh.h"
#include "TestHelpers.h"
#include "UvSeamData.h"

#include <OgreMeshManager.h>

namespace {

EditableMesh makeTwoTriMesh()
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

TEST(UvSeamDataTest, EdgeKeyIsOrderIndependent)
{
    const auto k1 = UvSeamData::makeEdgeKey(3, 7);
    const auto k2 = UvSeamData::makeEdgeKey(7, 3);
    EXPECT_EQ(k1, k2);
}

TEST(UvSeamDataTest, GlobalVertAndEdgeMapping)
{
    const EditableMesh mesh = makeTwoTriMesh();

    size_t sub = 99;
    unsigned int local = 99;
    EXPECT_TRUE(UvSeamData::globalVertToSubLocal(mesh, 2, sub, local));
    EXPECT_EQ(sub, 0u);
    EXPECT_EQ(local, 2u);

    UvSeamData::EdgeKey localKey = 0;
    EXPECT_NE(UvSeamData::globalEdgeToLocalKey(mesh, 0, 1, sub, localKey), 0u);
    EXPECT_TRUE(UvSeamData::isSeam(mesh.subMeshes()[0], 0, 1) == false);

    auto& sm = const_cast<EditableMesh&>(mesh).subMeshes()[0];
    UvSeamData::setSeam(sm, 0, 1, true);
    EXPECT_TRUE(UvSeamData::isSeam(sm, 1, 0));
}

TEST(UvSeamDataTest, PinRoundTripOnSubmesh)
{
    EditableSubMesh sub;
    sub.vertices.resize(4);
    UvSeamData::setPinned(sub, 2, true);
    EXPECT_TRUE(UvSeamData::isPinned(sub, 2));
    UvSeamData::setPinned(sub, 2, false);
    EXPECT_FALSE(UvSeamData::isPinned(sub, 2));
}

TEST(UvSeamDataTest, MeshBindingRoundTrip)
{
    if (!tryInitOgre())
        GTEST_SKIP() << "Ogre init failed";

    auto ogreMesh = createInMemoryTriangleMesh("UvSeamData_binding_tri");
    EditableMesh mesh;
    ASSERT_TRUE(mesh.loadFromMesh(ogreMesh));

    UvSeamData::setSeam(mesh.subMeshes()[0], 0, 1, true);
    UvSeamData::setPinned(mesh.subMeshes()[0], 1, true);
    UvSeamData::writeBindingsToMesh(ogreMesh.get(), mesh.subMeshes());

    EditableMesh reloaded;
    ASSERT_TRUE(reloaded.loadFromMesh(ogreMesh));
    EXPECT_TRUE(UvSeamData::isSeam(reloaded.subMeshes()[0], 0, 1));
    EXPECT_TRUE(UvSeamData::isPinned(reloaded.subMeshes()[0], 1));
}
