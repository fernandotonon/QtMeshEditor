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

namespace {

void clearSceneNodes()
{
    if (!Manager::getSingletonPtr())
        return;
    auto nodes = Manager::getSingleton()->getSceneNodes();
    for (auto* node : nodes) {
        Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
        Manager::getSingleton()->destroySceneNode(node);
    }
}

} // namespace

class PartOpsMeshMaterialCoverageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        ASSERT_TRUE(CLIPipeline::initOgreHeadless());
        clearSceneNodes();
    }
    void TearDown() override { clearSceneNodes(); }
};

TEST_F(PartOpsMeshMaterialCoverageTest, ReadSubMeshesPrefersEffectiveSubEntityMaterial)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // Real materials must EXIST, else Ogre's SubEntity resolves an unknown name
    // to the fallback (BaseWhite) and getMaterialName() reports that — so create
    // every material we reference up front.
    auto& mm = Ogre::MaterialManager::getSingleton();
    for (const char* n : {"BaseMatA", "BaseMatB", "RuntimeOverride0"}) {
        if (!mm.getByName(n))
            mm.create(n, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }

    // Two-submesh mesh (shared-vert sub0 + local-vert sub1). Give the SubMeshes
    // distinct base materials so we can prove the override wins per-submesh.
    const std::string meshName = "partops_mat_test_mesh";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);
    Ogre::MeshPtr mesh = createInMemoryMeshSharedVertsPlusLocalSubmesh(meshName);
    ASSERT_EQ(mesh->getNumSubMeshes(), 2u);
    mesh->getSubMesh(0)->setMaterialName("BaseMatA");
    mesh->getSubMesh(1)->setMaterialName("BaseMatB");
    // Leave submesh 1 on its base material to prove non-overridden submeshes keep it.

    Ogre::SceneNode* node = mgr->addSceneNode("partops_mat_test_node");
    Ogre::Entity* e = mgr->createEntity(node, mesh);
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(e->getNumSubEntities(), 2u);

    // Override ONLY subentity 0's material at runtime.
    e->getSubEntity(0)->setMaterialName("RuntimeOverride0");

    std::vector<EditableSubMesh> subs;
    ASSERT_TRUE(PartOpsMesh::readSubMeshes(e, subs));
    ASSERT_EQ(subs.size(), 2u);

    // Submesh 0 reflects the runtime override; submesh 1 keeps its base material.
    EXPECT_EQ(subs[0].materialName, "RuntimeOverride0");
    EXPECT_EQ(subs[1].materialName, "BaseMatB");
}
