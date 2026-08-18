/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — SymmetryMirrorMap unit tests (Paint v2 Slice E, issue #548)

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "SymmetryMirrorMap.h"
#include "EditableMesh.h"

namespace {

// Build a mesh symmetric across local X: a +X quad and its mirrored −X quad.
// The mirror half's UVs are OFFSET by +0.5 in U so a purely geometric mirror
// would read the wrong UV — the topology map must use the mirror triangle's
// own (offset) UVs.
EditableMesh makeSymmetricMesh(float uvOffsetOnMirrorHalf)
{
    EditableMesh mesh;
    auto& subs = mesh.subMeshes();
    subs.resize(1);
    auto& sub = subs[0];

    auto addVert = [&](float x, float y, float z, float u, float v) {
        EditableVertex ev;
        ev.position = Ogre::Vector3(x, y, z);
        ev.uv = Ogre::Vector2(u, v);
        ev.hasUV = true;
        sub.vertices.push_back(ev);
    };
    // +X quad (right half): x in [0.2, 1.0], UV in [0..0.5, 0..1].
    addVert(0.2f, -1.0f, 0.0f, 0.00f, 0.0f);  // 0
    addVert(1.0f, -1.0f, 0.0f, 0.50f, 0.0f);  // 1
    addVert(1.0f,  1.0f, 0.0f, 0.50f, 1.0f);  // 2
    addVert(0.2f,  1.0f, 0.0f, 0.00f, 1.0f);  // 3
    // −X quad (left half): exact mirror of the right half across x=0.
    const float o = uvOffsetOnMirrorHalf;
    addVert(-0.2f, -1.0f, 0.0f, 0.00f + o, 0.0f);  // 4  mirror of 0
    addVert(-1.0f, -1.0f, 0.0f, 0.50f + o, 0.0f);  // 5  mirror of 1
    addVert(-1.0f,  1.0f, 0.0f, 0.50f + o, 1.0f);  // 6  mirror of 2
    addVert(-0.2f,  1.0f, 0.0f, 0.00f + o, 1.0f);  // 7  mirror of 3

    auto tri = [&](unsigned a, unsigned b, unsigned c) {
        EditableTriangle t; t.indices[0] = a; t.indices[1] = b; t.indices[2] = c;
        sub.triangles.push_back(t);
    };
    tri(0, 1, 2); tri(0, 2, 3);   // right
    tri(4, 6, 5); tri(4, 7, 6);   // left (reversed winding — reflection)
    return mesh;
}

} // namespace

TEST(SymmetryMirrorMapTest, BuildOnSymmetricMeshFullCoverage) {
    EditableMesh mesh = makeSymmetricMesh(0.0f);
    SymmetryMirrorMap map;
    ASSERT_TRUE(map.build(mesh, /*SymAxisX*/1, Ogre::Vector3::ZERO, 1e-3f));
    EXPECT_TRUE(map.valid());
    EXPECT_NEAR(map.coverage(), 1.0f, 1e-5f);
}

TEST(SymmetryMirrorMapTest, MirrorDabUsesAsymmetricUV) {
    // Mirror half's UVs offset by +0.5 in U.
    EditableMesh mesh = makeSymmetricMesh(0.5f);
    SymmetryMirrorMap map;
    ASSERT_TRUE(map.build(mesh, /*SymAxisX*/1, Ogre::Vector3::ZERO, 1e-3f));
    ASSERT_TRUE(map.valid());

    // A dab at the centre of the right quad's first triangle (verts 0,1,2),
    // barycentric weights favouring corner 1 (UV 0.5,0).
    const int corner[3] = {0, 1, 2};
    const float bary[3] = {0.2f, 0.6f, 0.2f};
    int oSub = -1, oTri = -1; float oBary[3] = {0, 0, 0};
    ASSERT_TRUE(map.mirrorDab(0, corner, bary, oSub, oTri, oBary));
    EXPECT_EQ(oSub, 0);
    EXPECT_GE(oTri, 0);

    // Interpolate the mirror triangle's own UVs with the permuted weights.
    const auto& sub = mesh.subMeshes()[0];
    const auto& mtri = sub.triangles[static_cast<size_t>(oTri)];
    const Ogre::Vector2 mUV =
        sub.vertices[mtri.indices[0]].uv * oBary[0]
      + sub.vertices[mtri.indices[1]].uv * oBary[1]
      + sub.vertices[mtri.indices[2]].uv * oBary[2];

    // The primary dab's UV (right half, no offset).
    const Ogre::Vector2 primaryUV =
        sub.vertices[0].uv * bary[0]
      + sub.vertices[1].uv * bary[1]
      + sub.vertices[2].uv * bary[2];

    // Topology-aware mirror must land on the OFFSET UV (primary U + 0.5), NOT
    // the same U a geometric re-raycast onto mirrored-but-unoffset UVs would give.
    EXPECT_NEAR(mUV.x, primaryUV.x + 0.5f, 1e-4f)
        << "mirror dab should use the mirror triangle's offset UV";
    EXPECT_NEAR(mUV.y, primaryUV.y, 1e-4f);
}

TEST(SymmetryMirrorMapTest, InvalidWhenNoCorrespondence) {
    // A single asymmetric quad — no mirror partner across X.
    EditableMesh mesh;
    auto& subs = mesh.subMeshes();
    subs.resize(1);
    auto& sub = subs[0];
    auto addVert = [&](float x, float y) {
        EditableVertex ev; ev.position = Ogre::Vector3(x, y, 0.0f);
        ev.uv = Ogre::Vector2(0, 0); ev.hasUV = true; sub.vertices.push_back(ev);
    };
    addVert(0.2f, 0.0f); addVert(1.0f, 0.0f); addVert(1.0f, 1.0f);
    EditableTriangle t; t.indices[0] = 0; t.indices[1] = 1; t.indices[2] = 2;
    sub.triangles.push_back(t);

    SymmetryMirrorMap map;
    map.build(mesh, /*SymAxisX*/1, Ogre::Vector3::ZERO, 1e-3f);
    // No vertex has a mirror partner within tolerance → not valid.
    EXPECT_FALSE(map.valid());
}
