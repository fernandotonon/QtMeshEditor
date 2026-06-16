// Tests for MeshProcessor::processMesh() — the pure-data aiMesh -> SubMeshData
// transform. These exercise branches the existing src/Assimp/MeshProcessor_test.cpp
// does not cover: the Z-up axis bake, the morph-target extraction loop, and the
// various "feature absent" guards (no normals / no UVs / no colors / missing bone).
//
// processMesh is Ogre::Vector math only (no hardware buffers / GL), so it runs
// under a lightweight `Ogre::Root` fixture — no render window required. createMesh()
// is deliberately NOT exercised here (it needs HardwareBufferManager / a GL context).
//
// NOTE: src/Assimp/MeshProcessor_test.cpp already defines `MockMeshProcessor` and a
// `MeshProcessorTest` fixture; both files compile into the single UnitTests binary,
// so this file uses distinct symbol names (MeshProcessorZupMock / MeshProcessorZupTest)
// to avoid ODR / multiple-definition collisions.

#include <gtest/gtest.h>

#include "Assimp/MeshProcessor.h"

namespace {

// Subclass to expose the protected processMesh() — mirrors the wrapper in
// src/Assimp/MeshProcessor_test.cpp but with a unique name and a forwarded
// isZup ctor arg so we can drive the Z-up branch.
class MeshProcessorZupMock : public MeshProcessor {
public:
    MeshProcessorZupMock(Ogre::SkeletonPtr skeleton, bool isZup)
        : MeshProcessor(skeleton, isZup) {}
    SubMeshData* run(aiMesh* mesh, const aiScene* scene) {
        return MeshProcessor::processMesh(mesh, scene);
    }
};

// Tolerant vector compare — the 90° rotation introduces tiny FP error
// (e.g. (0,1,0) -> (0, ~0, 1) where the middle component is ~6e-8).
void expectVec3Near(const Ogre::Vector3& got, const Ogre::Vector3& want,
                    float tol = 1e-5f) {
    EXPECT_NEAR(got.x, want.x, tol);
    EXPECT_NEAR(got.y, want.y, tol);
    EXPECT_NEAR(got.z, want.z, tol);
}

class MeshProcessorZupTest : public ::testing::Test {
protected:
    std::unique_ptr<Ogre::Root> ogreRoot;
    Ogre::SkeletonPtr skeleton;
    aiScene scene;  // empty scene is fine — processMesh only reads from aiMesh

    void SetUp() override {
        ogreRoot = std::make_unique<Ogre::Root>();
        // Unique skeleton name per test process; manual=true so no resource load.
        skeleton = Ogre::SkeletonManager::getSingleton().create(
            "MeshProcessorZupSkeleton",
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
        skeleton->createBone("ZupBone");
    }

    void TearDown() override {
        if (skeleton) {
            Ogre::SkeletonManager::getSingleton().remove(skeleton->getHandle());
            skeleton.reset();
        }
        ogreRoot.reset();
    }

    // Builds a minimal aiMesh with the given vertex count and a positions array.
    // The caller fills/overrides the optional channels afterward.
    static std::unique_ptr<aiMesh> makeMesh(const std::vector<aiVector3D>& verts) {
        auto mesh = std::make_unique<aiMesh>();
        mesh->mNumVertices = static_cast<unsigned int>(verts.size());
        mesh->mVertices = new aiVector3D[verts.size()];
        for (size_t i = 0; i < verts.size(); ++i)
            mesh->mVertices[i] = verts[i];
        return mesh;
    }
};

// ---------------------------------------------------------------------------
// Z-up vertex rotation: R_x90 = Quaternion(Degree(90), UNIT_X)
//   v=(1,0,0) -> (1,0,0)   (axis is unchanged)
//   v=(0,1,0) -> (0,0,1)
//   v=(0,0,1) -> (0,-1,0)
// ---------------------------------------------------------------------------
TEST_F(MeshProcessorZupTest, ZupRotationBakedIntoVertices) {
    auto mesh = makeMesh({aiVector3D(1, 0, 0), aiVector3D(0, 1, 0), aiVector3D(0, 0, 1)});

    MeshProcessorZupMock processor(skeleton, /*isZup=*/true);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    ASSERT_NE(out, nullptr);
    ASSERT_EQ(out->vertices.size(), 3u);

    expectVec3Near(out->vertices[0], Ogre::Vector3(1, 0, 0));
    expectVec3Near(out->vertices[1], Ogre::Vector3(0, 0, 1));
    expectVec3Near(out->vertices[2], Ogre::Vector3(0, -1, 0));
}

// Control: with isZup=false the same input stays verbatim (identity rotation).
TEST_F(MeshProcessorZupTest, NonZupLeavesVerticesUnrotated) {
    auto mesh = makeMesh({aiVector3D(0, 1, 0), aiVector3D(0, 0, 1)});

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    ASSERT_NE(out, nullptr);
    ASSERT_EQ(out->vertices.size(), 2u);
    EXPECT_EQ(out->vertices[0], Ogre::Vector3(0, 1, 0));
    EXPECT_EQ(out->vertices[1], Ogre::Vector3(0, 0, 1));
}

// ---------------------------------------------------------------------------
// Z-up rotation applied to normals (HasNormals() == true).
// ---------------------------------------------------------------------------
TEST_F(MeshProcessorZupTest, ZupRotationBakedIntoNormals) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0), aiVector3D(0, 0, 0)});
    mesh->mNormals = new aiVector3D[2]{aiVector3D(0, 1, 0), aiVector3D(0, 0, 1)};

    MeshProcessorZupMock processor(skeleton, /*isZup=*/true);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    ASSERT_EQ(out->normals.size(), 2u);
    expectVec3Near(out->normals[0], Ogre::Vector3(0, 0, 1));   // (0,1,0) -> (0,0,1)
    expectVec3Near(out->normals[1], Ogre::Vector3(0, -1, 0));  // (0,0,1) -> (0,-1,0)
}

// ---------------------------------------------------------------------------
// Z-up rotation applied to tangents/bitangents/normals inside the
// HasTangentsAndBitangents() block. The block reads mNormals directly, so
// normals must be supplied as well.
//   T=(0,1,0) -> (0,0,1)   B=(0,0,1) -> (0,-1,0)   N=(1,0,0) -> (1,0,0)
//   handedness = sign( cross(N,T) . B )
//     after rotation: cross((1,0,0),(0,0,1)) = (0,-1,0); dot (0,-1,0).(0,-1,0)=+1 -> +1
// ---------------------------------------------------------------------------
TEST_F(MeshProcessorZupTest, ZupRotationBakedIntoTangentSpace) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0)});
    mesh->mNormals     = new aiVector3D[1]{aiVector3D(1, 0, 0)};
    mesh->mTangents    = new aiVector3D[1]{aiVector3D(0, 1, 0)};
    mesh->mBitangents  = new aiVector3D[1]{aiVector3D(0, 0, 1)};

    MeshProcessorZupMock processor(skeleton, /*isZup=*/true);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    ASSERT_EQ(out->tangents.size(), 1u);
    ASSERT_EQ(out->bitangents.size(), 1u);

    // Tangent xyz rotated; w = handedness.
    expectVec3Near(Ogre::Vector3(out->tangents[0].x, out->tangents[0].y, out->tangents[0].z),
                   Ogre::Vector3(0, 0, 1));
    EXPECT_NEAR(out->tangents[0].w, 1.0f, 1e-5f);
    expectVec3Near(out->bitangents[0], Ogre::Vector3(0, -1, 0));
}

// ---------------------------------------------------------------------------
// Morph targets: anim->mVertices copied verbatim into MorphTargetData::positions
// (no Z-up bake when processor isZup=false). Positions must match the input.
// ---------------------------------------------------------------------------
TEST_F(MeshProcessorZupTest, MorphTargetPositionsCopiedVerbatimNoZup) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0), aiVector3D(1, 0, 0)});

    auto* anim = new aiAnimMesh();
    anim->mNumVertices = 2;
    anim->mVertices = new aiVector3D[2]{aiVector3D(0.25f, 0.5f, 0.75f), aiVector3D(2, 3, 4)};
    mesh->mNumAnimMeshes = 1;
    mesh->mAnimMeshes = new aiAnimMesh*[1]{anim};

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    ASSERT_EQ(out->morphTargets.size(), 1u);
    ASSERT_EQ(out->morphTargets[0].positions.size(), 2u);
    EXPECT_EQ(out->morphTargets[0].positions[0], Ogre::Vector3(0.25f, 0.5f, 0.75f));
    EXPECT_EQ(out->morphTargets[0].positions[1], Ogre::Vector3(2, 3, 4));
}

// Morph target positions DO get the Z-up bake when processor isZup=true.
TEST_F(MeshProcessorZupTest, MorphTargetPositionsRotatedWhenZup) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0)});

    auto* anim = new aiAnimMesh();
    anim->mNumVertices = 1;
    anim->mVertices = new aiVector3D[1]{aiVector3D(0, 1, 0)};
    mesh->mNumAnimMeshes = 1;
    mesh->mAnimMeshes = new aiAnimMesh*[1]{anim};

    MeshProcessorZupMock processor(skeleton, /*isZup=*/true);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    ASSERT_EQ(out->morphTargets.size(), 1u);
    ASSERT_EQ(out->morphTargets[0].positions.size(), 1u);
    expectVec3Near(out->morphTargets[0].positions[0], Ogre::Vector3(0, 0, 1));
}

// ---------------------------------------------------------------------------
// Morph-target name fallback: empty aiAnimMesh::mName -> "Shape_<index>".
// ---------------------------------------------------------------------------
TEST_F(MeshProcessorZupTest, MorphTargetNameFallbackWhenUnnamed) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0)});

    auto* anim0 = new aiAnimMesh();
    anim0->mNumVertices = 1;
    anim0->mVertices = new aiVector3D[1]{aiVector3D(0, 0, 0)};
    // mName left at default (length 0).
    auto* anim1 = new aiAnimMesh();
    anim1->mNumVertices = 1;
    anim1->mVertices = new aiVector3D[1]{aiVector3D(0, 0, 0)};

    mesh->mNumAnimMeshes = 2;
    mesh->mAnimMeshes = new aiAnimMesh*[2]{anim0, anim1};

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    ASSERT_EQ(out->morphTargets.size(), 2u);
    EXPECT_EQ(out->morphTargets[0].name, std::string("Shape_0"));
    EXPECT_EQ(out->morphTargets[1].name, std::string("Shape_1"));
}

// Named morph target keeps its name.
TEST_F(MeshProcessorZupTest, MorphTargetKeepsExplicitName) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0)});

    auto* anim = new aiAnimMesh();
    anim->mNumVertices = 1;
    anim->mVertices = new aiVector3D[1]{aiVector3D(0, 0, 0)};
    anim->mName = aiString("Smile");

    mesh->mNumAnimMeshes = 1;
    mesh->mAnimMeshes = new aiAnimMesh*[1]{anim};

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    ASSERT_EQ(out->morphTargets.size(), 1u);
    EXPECT_EQ(out->morphTargets[0].name, std::string("Smile"));
}

// ---------------------------------------------------------------------------
// Morph-target skip guard: anim->mNumVertices != mesh->mNumVertices leaves
// morphTargets empty (count mismatch is rejected).
// ---------------------------------------------------------------------------
TEST_F(MeshProcessorZupTest, MorphTargetSkippedOnVertexCountMismatch) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0), aiVector3D(1, 0, 0)});  // 2 verts

    auto* anim = new aiAnimMesh();
    anim->mNumVertices = 3;  // mismatch
    anim->mVertices = new aiVector3D[3]{aiVector3D(0, 0, 0), aiVector3D(0, 0, 0), aiVector3D(0, 0, 0)};
    mesh->mNumAnimMeshes = 1;
    mesh->mAnimMeshes = new aiAnimMesh*[1]{anim};

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    EXPECT_TRUE(out->morphTargets.empty());
}

// Morph-target skip guard: null mVertices leaves morphTargets empty.
TEST_F(MeshProcessorZupTest, MorphTargetSkippedOnNullVertices) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0)});

    auto* anim = new aiAnimMesh();
    anim->mNumVertices = 1;
    anim->mVertices = nullptr;  // null payload
    mesh->mNumAnimMeshes = 1;
    mesh->mAnimMeshes = new aiAnimMesh*[1]{anim};

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    EXPECT_TRUE(out->morphTargets.empty());
}

// No anim meshes at all -> morphTargets empty (default zeroed mNumAnimMeshes).
TEST_F(MeshProcessorZupTest, NoMorphTargetsLeavesMorphTargetsEmpty) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0)});
    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    EXPECT_TRUE(out->morphTargets.empty());
}

// ---------------------------------------------------------------------------
// HasNormals() == false leaves subMeshData->normals empty.
// (aiMesh default-zeroes mNormals, so HasNormals() returns false.)
// ---------------------------------------------------------------------------
TEST_F(MeshProcessorZupTest, NoNormalsLeavesNormalsEmpty) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0), aiVector3D(1, 0, 0)});
    ASSERT_FALSE(mesh->HasNormals());

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    EXPECT_TRUE(out->normals.empty());
    EXPECT_EQ(out->vertices.size(), 2u);  // vertices still processed
}

// ---------------------------------------------------------------------------
// HasTextureCoords(0) == false leaves texCoords empty.
// ---------------------------------------------------------------------------
TEST_F(MeshProcessorZupTest, NoTexCoordsLeavesTexCoordsEmpty) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0)});
    ASSERT_FALSE(mesh->HasTextureCoords(0));

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    EXPECT_TRUE(out->texCoords.empty());
}

// ---------------------------------------------------------------------------
// HasVertexColors(0) == false leaves colors empty.
// ---------------------------------------------------------------------------
TEST_F(MeshProcessorZupTest, NoVertexColorsLeavesColorsEmpty) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0)});
    ASSERT_FALSE(mesh->HasVertexColors(0));

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    EXPECT_TRUE(out->colors.empty());
}

// ---------------------------------------------------------------------------
// Bone-skip branch: a bone whose name is not in the skeleton contributes no
// boneAssignments. Here the skeleton only has "ZupBone"; the mesh references
// "AbsentBone", so the weight loop is skipped entirely.
// ---------------------------------------------------------------------------
TEST_F(MeshProcessorZupTest, MissingBoneSkippedNoAssignments) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0), aiVector3D(1, 0, 0)});

    mesh->mNumBones = 1;
    mesh->mBones = new aiBone*[1];
    mesh->mBones[0] = new aiBone();
    mesh->mBones[0]->mName = aiString("AbsentBone");  // not in skeleton
    mesh->mBones[0]->mNumWeights = 2;
    mesh->mBones[0]->mWeights = new aiVertexWeight[2];
    mesh->mBones[0]->mWeights[0] = aiVertexWeight(0, 1.0f);
    mesh->mBones[0]->mWeights[1] = aiVertexWeight(1, 1.0f);

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    EXPECT_TRUE(out->boneAssignments.empty());
}

// Control: a present bone DOES add assignments (the path the existing test covers,
// repeated here so the skip test has a positive counterpart in this file).
TEST_F(MeshProcessorZupTest, PresentBoneAddsAssignments) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0), aiVector3D(1, 0, 0)});

    mesh->mNumBones = 1;
    mesh->mBones = new aiBone*[1];
    mesh->mBones[0] = new aiBone();
    mesh->mBones[0]->mName = aiString("ZupBone");  // present in skeleton
    mesh->mBones[0]->mNumWeights = 2;
    mesh->mBones[0]->mWeights = new aiVertexWeight[2];
    mesh->mBones[0]->mWeights[0] = aiVertexWeight(0, 0.5f);
    mesh->mBones[0]->mWeights[1] = aiVertexWeight(1, 0.75f);

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    ASSERT_EQ(out->boneAssignments.size(), 2u);
    EXPECT_EQ(out->boneAssignments[0].vertexIndex, 0u);
    EXPECT_FLOAT_EQ(out->boneAssignments[0].weight, 0.5f);
    EXPECT_EQ(out->boneAssignments[1].vertexIndex, 1u);
    EXPECT_FLOAT_EQ(out->boneAssignments[1].weight, 0.75f);
}

// Null-skeleton guard: with no skeleton the bone loop is also skipped.
TEST_F(MeshProcessorZupTest, NullSkeletonSkipsBoneAssignments) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0)});
    mesh->mNumBones = 1;
    mesh->mBones = new aiBone*[1];
    mesh->mBones[0] = new aiBone();
    mesh->mBones[0]->mName = aiString("ZupBone");
    mesh->mBones[0]->mNumWeights = 1;
    mesh->mBones[0]->mWeights = new aiVertexWeight[1];
    mesh->mBones[0]->mWeights[0] = aiVertexWeight(0, 1.0f);

    MeshProcessorZupMock processor(Ogre::SkeletonPtr(), /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    EXPECT_TRUE(out->boneAssignments.empty());
}

// ---------------------------------------------------------------------------
// materialIndex passthrough: subMeshData->materialIndex == mesh->mMaterialIndex.
// ---------------------------------------------------------------------------
TEST_F(MeshProcessorZupTest, MaterialIndexPassthrough) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0)});
    mesh->mMaterialIndex = 7;

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    EXPECT_EQ(out->materialIndex, 7u);
}

// Indices are flattened from faces in order.
TEST_F(MeshProcessorZupTest, FaceIndicesFlattenedInOrder) {
    auto mesh = makeMesh({aiVector3D(0, 0, 0), aiVector3D(1, 0, 0), aiVector3D(0, 1, 0)});
    mesh->mNumFaces = 1;
    mesh->mFaces = new aiFace[1];
    mesh->mFaces[0].mNumIndices = 3;
    mesh->mFaces[0].mIndices = new unsigned int[3]{0, 1, 2};

    MeshProcessorZupMock processor(skeleton, /*isZup=*/false);
    SubMeshData* out = processor.run(mesh.get(), &scene);
    ASSERT_EQ(out->indices.size(), 3u);
    EXPECT_EQ(out->indices[0], 0u);
    EXPECT_EQ(out->indices[1], 1u);
    EXPECT_EQ(out->indices[2], 2u);
}

} // namespace
