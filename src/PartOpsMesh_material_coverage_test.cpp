// GL-gated coverage for PartOpsMesh::readSubMeshes' effective-material override
// (#862 review): a material assigned at runtime via Material Mode lives on the
// Entity's SubEntity, not the SubMesh. readSubMeshes must prefer the SubEntity's
// effective material so split/explode/join keep what the user sees. Needs a real
// Ogre scene (Xvfb/GL in CI) — the readSubMeshes path reads a live Entity.

#include <gtest/gtest.h>

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreSubEntity.h>
#include <OgreMeshManager.h>
#include <OgreMaterialManager.h>

#include "PartOpsMesh.h"
#include "Manager.h"
#include "CLIPipeline.h"
#include "TestHelpers.h"

#include <QString>

// Unique-per-test resource names so the fixture never touches another test's
// state in the shared process (CodeRabbit): cleanup destroys ONLY these.
namespace {
const std::string kNodeName = "partops_mat_test_node";
const std::string kMeshName = "partops_mat_test_mesh";
const char* const kMatA = "partops_mat_BaseA";
const char* const kMatB = "partops_mat_BaseB";
const char* const kMatOverride = "partops_mat_Override0";
} // namespace

class PartOpsMeshMaterialCoverageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        ASSERT_TRUE(CLIPipeline::initOgreHeadless());
    }
    // Destroy ONLY the resources this fixture created — never the whole scene.
    void TearDown() override
    {
        auto* mgr = Manager::getSingletonPtr();
        if (mgr && mgr->hasSceneNode(QString::fromStdString(kNodeName))) {
            if (Ogre::SceneNode* n = mgr->getSceneNode(QString::fromStdString(kNodeName))) {
                mgr->destroyAllAttachedMovableObjects(n);
                mgr->destroySceneNode(n);
            }
        }
        if (auto m = Ogre::MeshManager::getSingleton().getByName(kMeshName))
            Ogre::MeshManager::getSingleton().remove(m);
        auto& mm = Ogre::MaterialManager::getSingleton();
        for (const char* n : {kMatA, kMatB, kMatOverride})
            if (auto mat = mm.getByName(n))
                mm.remove(mat);
    }
};

TEST_F(PartOpsMeshMaterialCoverageTest, ReadSubMeshesPrefersEffectiveSubEntityMaterial)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // Real materials must EXIST, else Ogre's SubEntity resolves an unknown name
    // to the fallback (BaseWhite) and getMaterialName() reports that — so create
    // every material we reference up front (unique names, cleaned up in TearDown).
    auto& mm = Ogre::MaterialManager::getSingleton();
    for (const char* n : {kMatA, kMatB, kMatOverride}) {
        if (!mm.getByName(n))
            mm.create(n, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }

    // Two-submesh mesh (shared-vert sub0 + local-vert sub1). Give the SubMeshes
    // distinct base materials so we can prove the override wins per-submesh.
    Ogre::MeshPtr mesh = createInMemoryMeshSharedVertsPlusLocalSubmesh(kMeshName);
    ASSERT_EQ(mesh->getNumSubMeshes(), 2u);
    mesh->getSubMesh(0)->setMaterialName(kMatA);
    mesh->getSubMesh(1)->setMaterialName(kMatB);
    // Leave submesh 1 on its base material to prove non-overridden submeshes keep it.

    Ogre::SceneNode* node = mgr->addSceneNode(QString::fromStdString(kNodeName));
    Ogre::Entity* e = mgr->createEntity(node, mesh);
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(e->getNumSubEntities(), 2u);

    // Override ONLY subentity 0's material at runtime.
    e->getSubEntity(0)->setMaterialName(kMatOverride);

    std::vector<EditableSubMesh> subs;
    ASSERT_TRUE(PartOpsMesh::readSubMeshes(e, subs));
    ASSERT_EQ(subs.size(), 2u);

    // Submesh 0 reflects the runtime override; submesh 1 keeps its base material.
    EXPECT_EQ(subs[0].materialName, kMatOverride);
    EXPECT_EQ(subs[1].materialName, kMatB);
}
