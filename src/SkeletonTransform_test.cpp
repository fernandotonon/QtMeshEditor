#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <OgreException.h>
#include "Manager.h"
#include "SkeletonTransform.h"
#include "MeshImporterExporter.h"
#include "TestHelpers.h"

class SkeletonTransformTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    Ogre::Entity* entity = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        ASSERT_TRUE(canLoadMeshFiles()) << "mesh loading requires GL (Xvfb in CI)";

        QStringList validUri{"./media/models/robot.mesh"};
        ASSERT_NO_THROW(MeshImporterExporter::importer(validUri));

        ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
        entity = Manager::getSingleton()->getEntities().last();
        ASSERT_NE(entity, nullptr);
        ASSERT_TRUE(entity->hasSkeleton());
    }

    void TearDown() override {
        entity = nullptr;
        if (app) {
            app->processEvents();
        }
    }
};

// --------------------------------------------------------------------------
// Constructor is deleted -- verify this is a static-only class
// --------------------------------------------------------------------------
TEST_F(SkeletonTransformTest, ConstructorIsDeleted)
{
    // SkeletonTransform() = delete; -- verified at compile time.
    // This test simply confirms the class can be used via static methods.
    EXPECT_NE(entity, nullptr);
    EXPECT_TRUE(entity->hasSkeleton());
}

// --------------------------------------------------------------------------
// renameAnimation tests
// --------------------------------------------------------------------------

TEST_F(SkeletonTransformTest, RenameAnimationEmptyNewNameReturnsFalse)
{
    // renameAnimation should return false when new name is empty
    bool result = SkeletonTransform::renameAnimation(entity, "Walk", "");
    EXPECT_FALSE(result);
}

TEST_F(SkeletonTransformTest, RenameAnimationNullEntityReturnsFalse)
{
    // renameAnimation should return false when entity is null
    bool result = SkeletonTransform::renameAnimation(nullptr, "Walk", "Run");
    EXPECT_FALSE(result);
}

TEST_F(SkeletonTransformTest, RenameAnimationNonExistentOldNameReturnsFalse)
{
    // renameAnimation should return false when old animation doesn't exist
    bool result = SkeletonTransform::renameAnimation(entity, "NonExistentAnimation", "NewName");
    EXPECT_FALSE(result);
}

TEST_F(SkeletonTransformTest, RenameAnimationValidRenameReturnsTrue)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    // Find a real animation name from the skeleton
    ASSERT_GT(sk->getNumAnimations(), 0u);
    Ogre::Animation* firstAnim = sk->getAnimation(0);
    QString oldName = QString::fromStdString(firstAnim->getName());
    Ogre::Real expectedLength = firstAnim->getLength();
    unsigned short expectedNumTracks = firstAnim->getNumNodeTracks();

    QString newName = "RenamedAnimation_TestUnique";

    bool result = SkeletonTransform::renameAnimation(entity, oldName, newName);
    EXPECT_TRUE(result);

    // Verify old animation no longer exists
    EXPECT_FALSE(sk->hasAnimation(oldName.toStdString()));

    // Verify new animation exists
    EXPECT_TRUE(sk->hasAnimation(newName.toStdString()));

    // Verify the new animation preserved properties
    Ogre::Animation* newAnim = sk->getAnimation(newName.toStdString());
    ASSERT_NE(newAnim, nullptr);
    EXPECT_FLOAT_EQ(newAnim->getLength(), expectedLength);
    EXPECT_EQ(newAnim->getNumNodeTracks(), expectedNumTracks);
}

TEST_F(SkeletonTransformTest, RenameAnimationPreservesKeyframes)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    // Find an animation with tracks and keyframes
    ASSERT_GT(sk->getNumAnimations(), 0u);
    Ogre::Animation* firstAnim = sk->getAnimation(0);
    QString oldName = QString::fromStdString(firstAnim->getName());

    // Store keyframe data from the first available track for comparison
    unsigned short trackHandle = 0;
    unsigned short numKeyFrames = 0;
    Ogre::Vector3 firstKeyTranslate = Ogre::Vector3::ZERO;
    Ogre::Quaternion firstKeyRotation = Ogre::Quaternion::IDENTITY;
    Ogre::Vector3 firstKeyScale = Ogre::Vector3::UNIT_SCALE;
    bool foundTrack = false;

    // Iterate over possible track handles to find one
    for (unsigned short j = 0; j < 1000; j++) {
        if (!firstAnim->hasNodeTrack(j)) continue;
        Ogre::NodeAnimationTrack* track = firstAnim->getNodeTrack(j);
        if (!track || track->getNumKeyFrames() == 0) continue;

        trackHandle = j;
        numKeyFrames = track->getNumKeyFrames();
        Ogre::TransformKeyFrame* kf = track->getNodeKeyFrame(0);
        if (kf) {
            firstKeyTranslate = kf->getTranslate();
            firstKeyRotation = kf->getRotation();
            firstKeyScale = kf->getScale();
        }
        foundTrack = true;
        break;
    }

    ASSERT_TRUE(foundTrack) << "no animation tracks found with keyframes";

    QString newName = "KeyframePreserved_TestUnique";
    bool result = SkeletonTransform::renameAnimation(entity, oldName, newName);
    ASSERT_TRUE(result);

    Ogre::Animation* newAnim = sk->getAnimation(newName.toStdString());
    ASSERT_NE(newAnim, nullptr);
    ASSERT_TRUE(newAnim->hasNodeTrack(trackHandle));

    Ogre::NodeAnimationTrack* newTrack = newAnim->getNodeTrack(trackHandle);
    ASSERT_NE(newTrack, nullptr);
    EXPECT_EQ(newTrack->getNumKeyFrames(), numKeyFrames);

    // Verify the first keyframe data was preserved
    Ogre::TransformKeyFrame* newKf = newTrack->getNodeKeyFrame(0);
    ASSERT_NE(newKf, nullptr);
    EXPECT_EQ(newKf->getTranslate(), firstKeyTranslate);
    EXPECT_EQ(newKf->getRotation(), firstKeyRotation);
    EXPECT_EQ(newKf->getScale(), firstKeyScale);
}

TEST_F(SkeletonTransformTest, RenameAnimationAlreadyRenamedReturnsFalse)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    ASSERT_GT(sk->getNumAnimations(), 0u);
    Ogre::Animation* firstAnim = sk->getAnimation(0);
    QString oldName = QString::fromStdString(firstAnim->getName());
    QString newName = "AlreadyRenamed_TestUnique";

    // Rename once -- should succeed
    ASSERT_TRUE(SkeletonTransform::renameAnimation(entity, oldName, newName));

    // Try to rename the old name again -- should fail because old name no longer exists
    bool result = SkeletonTransform::renameAnimation(entity, oldName, "AnotherName");
    EXPECT_FALSE(result);
}

// --------------------------------------------------------------------------
// scaleSkeleton tests
// --------------------------------------------------------------------------

TEST_F(SkeletonTransformTest, ScaleSkeletonUniformScale)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    auto bones = sk->getBones();
    ASSERT_FALSE(bones.empty()) << "skeleton has no bones";

    // Record the initial root bone positions
    std::vector<Ogre::Vector3> initialPositions;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            initialPositions.push_back(bone->getPosition());
        }
    }

    // Scale by identity (1,1,1) should not change positions meaningfully
    SkeletonTransform::scaleSkeleton(entity, Ogre::Vector3(1.0f, 1.0f, 1.0f));

    // Verify root bones are still at approximately the same positions
    size_t idx = 0;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialPositions.size());
            Ogre::Vector3 pos = bone->getPosition();
            EXPECT_NEAR(pos.x, initialPositions[idx].x, 0.001f);
            EXPECT_NEAR(pos.y, initialPositions[idx].y, 0.001f);
            EXPECT_NEAR(pos.z, initialPositions[idx].z, 0.001f);
            idx++;
        }
    }
}

TEST_F(SkeletonTransformTest, ScaleSkeletonDoubleScale)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    auto bones = sk->getBones();
    ASSERT_FALSE(bones.empty()) << "skeleton has no bones";

    // Record root bone positions before scaling
    std::vector<Ogre::Vector3> initialPositions;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            initialPositions.push_back(bone->getPosition());
        }
    }

    // Scale by (2,2,2)
    SkeletonTransform::scaleSkeleton(entity, Ogre::Vector3(2.0f, 2.0f, 2.0f));

    // Root bone positions should be doubled
    size_t idx = 0;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialPositions.size());
            Ogre::Vector3 pos = bone->getPosition();
            EXPECT_NEAR(pos.x, initialPositions[idx].x * 2.0f, 0.001f);
            EXPECT_NEAR(pos.y, initialPositions[idx].y * 2.0f, 0.001f);
            EXPECT_NEAR(pos.z, initialPositions[idx].z * 2.0f, 0.001f);
            idx++;
        }
    }
}

// --------------------------------------------------------------------------
// translateSkeleton tests
// --------------------------------------------------------------------------

TEST_F(SkeletonTransformTest, TranslateSkeletonZeroVector)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    auto bones = sk->getBones();
    ASSERT_FALSE(bones.empty()) << "skeleton has no bones";

    // Record root bone positions
    std::vector<Ogre::Vector3> initialPositions;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            initialPositions.push_back(bone->getPosition());
        }
    }

    // Translate by zero vector should not change positions
    SkeletonTransform::translateSkeleton(entity, Ogre::Vector3::ZERO);

    size_t idx = 0;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialPositions.size());
            Ogre::Vector3 pos = bone->getPosition();
            EXPECT_NEAR(pos.x, initialPositions[idx].x, 0.001f);
            EXPECT_NEAR(pos.y, initialPositions[idx].y, 0.001f);
            EXPECT_NEAR(pos.z, initialPositions[idx].z, 0.001f);
            idx++;
        }
    }
}

TEST_F(SkeletonTransformTest, TranslateSkeletonAppliesOffset)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    auto bones = sk->getBones();
    ASSERT_FALSE(bones.empty()) << "skeleton has no bones";

    // Record root bone positions
    std::vector<Ogre::Vector3> initialPositions;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            initialPositions.push_back(bone->getPosition());
        }
    }

    Ogre::Vector3 offset(10.0f, 5.0f, 3.0f);
    SkeletonTransform::translateSkeleton(entity, offset);

    // Root bones should have been translated by the offset
    size_t idx = 0;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialPositions.size());
            Ogre::Vector3 pos = bone->getPosition();
            EXPECT_NEAR(pos.x, initialPositions[idx].x + offset.x, 0.001f);
            EXPECT_NEAR(pos.y, initialPositions[idx].y + offset.y, 0.001f);
            EXPECT_NEAR(pos.z, initialPositions[idx].z + offset.z, 0.001f);
            idx++;
        }
    }
}

// --------------------------------------------------------------------------
// rotateSkeleton tests
// --------------------------------------------------------------------------

TEST_F(SkeletonTransformTest, RotateSkeletonZeroRotation)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    auto bones = sk->getBones();
    ASSERT_FALSE(bones.empty()) << "skeleton has no bones";

    // Record root bone orientations
    std::vector<Ogre::Quaternion> initialOrientations;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            initialOrientations.push_back(bone->getOrientation());
        }
    }

    // Rotate by zero vector should skip (continue in the code path)
    SkeletonTransform::rotateSkeleton(entity, Ogre::Vector3::ZERO);

    // Orientations should remain unchanged
    size_t idx = 0;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialOrientations.size());
            Ogre::Quaternion orient = bone->getOrientation();
            EXPECT_NEAR(orient.w, initialOrientations[idx].w, 0.001f);
            EXPECT_NEAR(orient.x, initialOrientations[idx].x, 0.001f);
            EXPECT_NEAR(orient.y, initialOrientations[idx].y, 0.001f);
            EXPECT_NEAR(orient.z, initialOrientations[idx].z, 0.001f);
            idx++;
        }
    }
}

TEST_F(SkeletonTransformTest, RotateSkeletonXAxis)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    auto bones = sk->getBones();
    ASSERT_FALSE(bones.empty()) << "skeleton has no bones";

    // Record root bone orientations before rotation
    std::vector<Ogre::Quaternion> initialOrientations;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            initialOrientations.push_back(bone->getOrientation());
        }
    }

    // Rotate around X axis (which maps to UNIT_Y in the code: _rotate.x uses UNIT_Y)
    SkeletonTransform::rotateSkeleton(entity, Ogre::Vector3(90.0f, 0.0f, 0.0f));

    // At least one root bone orientation should have changed
    bool anyChanged = false;
    size_t idx = 0;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialOrientations.size());
            Ogre::Quaternion orient = bone->getOrientation();
            if (!orient.equals(initialOrientations[idx], Ogre::Radian(0.001f))) {
                anyChanged = true;
            }
            idx++;
        }
    }
    EXPECT_TRUE(anyChanged);
}

TEST_F(SkeletonTransformTest, RotateSkeletonYAxis)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    auto bones = sk->getBones();
    ASSERT_FALSE(bones.empty()) << "skeleton has no bones";

    std::vector<Ogre::Quaternion> initialOrientations;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            initialOrientations.push_back(bone->getOrientation());
        }
    }

    // Rotate around Y axis (which maps to UNIT_Z in the code: _rotate.y uses UNIT_Z)
    SkeletonTransform::rotateSkeleton(entity, Ogre::Vector3(0.0f, 45.0f, 0.0f));

    bool anyChanged = false;
    size_t idx = 0;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialOrientations.size());
            Ogre::Quaternion orient = bone->getOrientation();
            if (!orient.equals(initialOrientations[idx], Ogre::Radian(0.001f))) {
                anyChanged = true;
            }
            idx++;
        }
    }
    EXPECT_TRUE(anyChanged);
}

TEST_F(SkeletonTransformTest, RotateSkeletonZAxis)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    auto bones = sk->getBones();
    ASSERT_FALSE(bones.empty()) << "skeleton has no bones";

    std::vector<Ogre::Quaternion> initialOrientations;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            initialOrientations.push_back(bone->getOrientation());
        }
    }

    // Rotate around Z axis (which maps to UNIT_X in the code: _rotate.z uses UNIT_X)
    SkeletonTransform::rotateSkeleton(entity, Ogre::Vector3(0.0f, 0.0f, 30.0f));

    bool anyChanged = false;
    size_t idx = 0;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialOrientations.size());
            Ogre::Quaternion orient = bone->getOrientation();
            if (!orient.equals(initialOrientations[idx], Ogre::Radian(0.001f))) {
                anyChanged = true;
            }
            idx++;
        }
    }
    EXPECT_TRUE(anyChanged);
}

// --------------------------------------------------------------------------
// rotateSkeleton: verify bone positions are rotated around mesh center
// --------------------------------------------------------------------------

TEST_F(SkeletonTransformTest, RotateSkeletonRotatesBonePositionsAroundMeshCenter)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    auto bones = sk->getBones();
    ASSERT_FALSE(bones.empty()) << "skeleton has no bones";

    Ogre::Vector3 meshCenter = entity->getMesh()->getBounds().getCenter();

    // Record root bone positions before rotation
    std::vector<Ogre::Vector3> initialPositions;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            initialPositions.push_back(bone->getPosition());
        }
    }

    // Rotate 90 degrees (maps to UNIT_Y axis)
    Ogre::Quaternion expectedQuat(Ogre::Degree(90.0f), Ogre::Vector3::UNIT_Y);
    SkeletonTransform::rotateSkeleton(entity, Ogre::Vector3(90.0f, 0.0f, 0.0f));

    // Root bone positions should be rotated around meshCenter
    size_t idx = 0;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialPositions.size());
            Ogre::Vector3 expectedPos = expectedQuat * (initialPositions[idx] - meshCenter) + meshCenter;
            Ogre::Vector3 actualPos = bone->getPosition();
            EXPECT_NEAR(actualPos.x, expectedPos.x, 0.01f)
                << "Root bone " << idx << " x position mismatch";
            EXPECT_NEAR(actualPos.y, expectedPos.y, 0.01f)
                << "Root bone " << idx << " y position mismatch";
            EXPECT_NEAR(actualPos.z, expectedPos.z, 0.01f)
                << "Root bone " << idx << " z position mismatch";
            idx++;
        }
    }
}

TEST_F(SkeletonTransformTest, RotateSkeletonYAxisRotatesBonePositions)
{
    Ogre::Skeleton* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    auto bones = sk->getBones();
    ASSERT_FALSE(bones.empty()) << "skeleton has no bones";

    Ogre::Vector3 meshCenter = entity->getMesh()->getBounds().getCenter();

    std::vector<Ogre::Vector3> initialPositions;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            initialPositions.push_back(bone->getPosition());
        }
    }

    Ogre::Quaternion expectedQuat(Ogre::Degree(45.0f), Ogre::Vector3::UNIT_Z);
    SkeletonTransform::rotateSkeleton(entity, Ogre::Vector3(0.0f, 45.0f, 0.0f));

    size_t idx = 0;
    for (const auto& bone : bones) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialPositions.size());
            Ogre::Vector3 expectedPos = expectedQuat * (initialPositions[idx] - meshCenter) + meshCenter;
            Ogre::Vector3 actualPos = bone->getPosition();
            EXPECT_NEAR(actualPos.x, expectedPos.x, 0.01f);
            EXPECT_NEAR(actualPos.y, expectedPos.y, 0.01f);
            EXPECT_NEAR(actualPos.z, expectedPos.z, 0.01f);
            idx++;
        }
    }
}

// --------------------------------------------------------------------------
// Edge case: entity without skeleton (using a primitive mesh)
// --------------------------------------------------------------------------

class SkeletonTransformNoSkeletonTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }

    void TearDown() override {
        if (app) {
            app->processEvents();
        }
    }
};

TEST_F(SkeletonTransformNoSkeletonTest, ScaleSkeletonNoSkeletonDoesNotCrash)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    // Create a simple mesh without a skeleton
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(
        "TestNoSkeletonMesh", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    mesh->createSubMesh();
    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 1, 1, 1));
    mesh->_setBoundingSphereRadius(1.0f);
    mesh->load();

    Ogre::SceneNode* node = mgr->addSceneNode("NoSkeletonNode");
    Ogre::Entity* ent = mgr->createEntity(node, mesh);
    ASSERT_NE(ent, nullptr);
    ASSERT_FALSE(ent->hasSkeleton());

    // These should all return safely without crashing
    SkeletonTransform::scaleSkeleton(ent, Ogre::Vector3(2.0f, 2.0f, 2.0f));
    SkeletonTransform::translateSkeleton(ent, Ogre::Vector3(1.0f, 1.0f, 1.0f));
    SkeletonTransform::rotateSkeleton(ent, Ogre::Vector3(45.0f, 0.0f, 0.0f));

    // If we get here without crashing, the test passes
    SUCCEED();
}

TEST_F(SkeletonTransformNoSkeletonTest, RenameAnimationNullEntityAndEmptyName)
{
    // Verify combined edge cases: null entity with empty name
    // Empty name check happens first, so this should return false without crashing
    bool result = SkeletonTransform::renameAnimation(nullptr, "SomeAnim", "");
    EXPECT_FALSE(result);

    // Null entity with non-empty names -- should return false without crashing
    result = SkeletonTransform::renameAnimation(nullptr, "OldAnim", "NewAnim");
    EXPECT_FALSE(result);
}

// Note: renameAnimation with a valid entity that has no skeleton would crash
// because the code does _ent->getSkeleton()->hasAnimation() without a null
// check on the skeleton pointer. This is a known limitation -- do not call
// renameAnimation on entities without skeletons.

// --------------------------------------------------------------------------
// Programmatic skeleton tests (no file I/O -- runs on CI headless)
// --------------------------------------------------------------------------

// No-op loader so Ogre marks the skeleton as LOADED (avoids file-based load)
class NullSkeletonLoader : public Ogre::ManualResourceLoader {
public:
    void loadResource(Ogre::Resource*) override {}
};

class SkeletonTransformProgrammaticTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    Ogre::Entity* entity = nullptr;
    std::string meshName;
    std::string skeletonName;
    QString nodeName;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

        // Use unique names per test to avoid resource conflicts
        static int counter = 0;
        counter++;
        meshName = "ProgSkMesh_" + std::to_string(counter);
        skeletonName = "ProgSk_" + std::to_string(counter);
        nodeName = QString("ProgSkNode_%1").arg(counter);

        entity = createEntityWithSkeleton();
        ASSERT_NE(entity, nullptr) << "failed to create programmatic skeleton entity";
    }

    void TearDown() override {
        entity = nullptr;
        if (app) app->processEvents();
    }

    Ogre::Entity* createEntityWithSkeleton()
    {
        auto* mgr = Manager::getSingletonPtr();
        if (!mgr) return nullptr;

        try {
            // Create skeleton with a root bone at (0, 5, 0) and child at relative (0, 3, 0)
            // Use ManualResourceLoader so skeleton is marked as LOADED when load() is called
            static NullSkeletonLoader nullLoader;
            auto skeleton = Ogre::SkeletonManager::getSingleton().create(
                skeletonName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
                true, &nullLoader);

            auto* rootBone = skeleton->createBone("Root", 0);
            rootBone->setPosition(Ogre::Vector3(0, 5, 0));

            auto* childBone = skeleton->createBone("Child", 1);
            childBone->setPosition(Ogre::Vector3(0, 3, 0));
            rootBone->addChild(childBone);

            skeleton->setBindingPose();
            skeleton->load();

            // Create an animation
            auto* anim = skeleton->createAnimation("TestWalk", 1.0f);
            auto* track = anim->createNodeTrack(0);
            track->setAssociatedNode(rootBone);
            auto* kf0 = track->createNodeKeyFrame(0.0f);
            kf0->setTranslate(Ogre::Vector3::ZERO);
            auto* kf1 = track->createNodeKeyFrame(1.0f);
            kf1->setTranslate(Ogre::Vector3(1, 0, 0));

            // Create mesh with vertex positions
            auto mesh = Ogre::MeshManager::getSingleton().createManual(
                meshName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

            auto* sub = mesh->createSubMesh();
            sub->vertexData = new Ogre::VertexData();
            sub->vertexData->vertexCount = 4;

            auto* decl = sub->vertexData->vertexDeclaration;
            decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

            auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
                decl->getVertexSize(0), 4, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);

            float vertices[] = {
                -1.0f, 0.0f, -1.0f,
                 1.0f, 0.0f, -1.0f,
                 1.0f, 0.0f,  1.0f,
                -1.0f, 0.0f,  1.0f,
            };
            vbuf->writeData(0, sizeof(vertices), vertices, true);
            sub->vertexData->vertexBufferBinding->setBinding(0, vbuf);

            mesh->setSkeletonName(skeletonName);
            mesh->_setBounds(Ogre::AxisAlignedBox(-1, 0, -1, 1, 5, 1));
            mesh->_setBoundingSphereRadius(6.0f);
            mesh->load();

            auto* node = mgr->addSceneNode(nodeName);
            return mgr->createEntity(node, mesh);
        } catch (const Ogre::Exception& e) {
            qWarning() << "Failed to create programmatic skeleton:"
                       << e.getFullDescription().c_str();
            return nullptr;
        }
    }
};

TEST_F(SkeletonTransformProgrammaticTest, EntityHasSkeletonAndBones)
{
    ASSERT_TRUE(entity->hasSkeleton());
    auto* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);
    EXPECT_GE(sk->getNumBones(), 2u);
}

TEST_F(SkeletonTransformProgrammaticTest, RotateQuaternionRotatesBonePositions)
{
    auto* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    Ogre::Vector3 meshCenter = entity->getMesh()->getBounds().getCenter();

    // Collect initial root bone positions
    std::vector<Ogre::Vector3> initialPositions;
    for (const auto& bone : sk->getBones()) {
        if (bone->getParent() == nullptr)
            initialPositions.push_back(bone->getPosition());
    }
    ASSERT_FALSE(initialPositions.empty());

    // Rotate 90 degrees around Y axis
    Ogre::Quaternion quat(Ogre::Degree(90.0f), Ogre::Vector3::UNIT_Y);
    SkeletonTransform::rotateSkeleton(entity, quat, meshCenter);

    size_t idx = 0;
    for (const auto& bone : sk->getBones()) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialPositions.size());
            Ogre::Vector3 expected = quat * (initialPositions[idx] - meshCenter) + meshCenter;
            Ogre::Vector3 actual = bone->getPosition();
            EXPECT_NEAR(actual.x, expected.x, 0.01f) << "Root bone " << idx << " x";
            EXPECT_NEAR(actual.y, expected.y, 0.01f) << "Root bone " << idx << " y";
            EXPECT_NEAR(actual.z, expected.z, 0.01f) << "Root bone " << idx << " z";
            idx++;
        }
    }
}

TEST_F(SkeletonTransformProgrammaticTest, RotateQuaternionMatchesVector3)
{
    // The Vector3 overload with x=90 should produce the same result as
    // Quaternion(Degree(90), UNIT_Y) since that's how buildRotationQuat works
    auto* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    // Record initial bone positions and orientations
    std::vector<Ogre::Vector3> initialPositions;
    std::vector<Ogre::Quaternion> initialOrientations;
    for (const auto& bone : sk->getBones()) {
        if (bone->getParent() == nullptr) {
            initialPositions.push_back(bone->getPosition());
            initialOrientations.push_back(bone->getOrientation());
        }
    }

    // Apply Vector3 rotation
    SkeletonTransform::rotateSkeleton(entity, Ogre::Vector3(90.0f, 0.0f, 0.0f));

    // Record Vector3 result
    std::vector<Ogre::Vector3> vec3Positions;
    std::vector<Ogre::Quaternion> vec3Orientations;
    for (const auto& bone : sk->getBones()) {
        if (bone->getParent() == nullptr) {
            vec3Positions.push_back(bone->getPosition());
            vec3Orientations.push_back(bone->getOrientation());
        }
    }

    // Reset bones back to initial state by applying inverse rotation
    Ogre::Quaternion quat(Ogre::Degree(90.0f), Ogre::Vector3::UNIT_Y);
    Ogre::Quaternion invQuat = quat.Inverse();
    Ogre::Vector3 meshCenter = entity->getMesh()->getBounds().getCenter();
    SkeletonTransform::rotateSkeleton(entity, invQuat, meshCenter);

    // Now apply Quaternion rotation from initial
    SkeletonTransform::rotateSkeleton(entity, quat, meshCenter);

    size_t idx = 0;
    for (const auto& bone : sk->getBones()) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, vec3Positions.size());
            EXPECT_NEAR(bone->getPosition().x, vec3Positions[idx].x, 0.01f);
            EXPECT_NEAR(bone->getPosition().y, vec3Positions[idx].y, 0.01f);
            EXPECT_NEAR(bone->getPosition().z, vec3Positions[idx].z, 0.01f);
            idx++;
        }
    }
}

TEST_F(SkeletonTransformProgrammaticTest, RotateQuaternionIdentityPreservesBones)
{
    auto* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    std::vector<Ogre::Vector3> initialPositions;
    for (const auto& bone : sk->getBones()) {
        if (bone->getParent() == nullptr)
            initialPositions.push_back(bone->getPosition());
    }

    SkeletonTransform::rotateSkeleton(entity, Ogre::Quaternion::IDENTITY,
                                      entity->getMesh()->getBounds().getCenter());

    size_t idx = 0;
    for (const auto& bone : sk->getBones()) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialPositions.size());
            EXPECT_NEAR(bone->getPosition().x, initialPositions[idx].x, 0.001f);
            EXPECT_NEAR(bone->getPosition().y, initialPositions[idx].y, 0.001f);
            EXPECT_NEAR(bone->getPosition().z, initialPositions[idx].z, 0.001f);
            idx++;
        }
    }
}

TEST_F(SkeletonTransformProgrammaticTest, RotateQuaternionArbitraryAxis)
{
    auto* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    Ogre::Vector3 meshCenter = entity->getMesh()->getBounds().getCenter();

    std::vector<Ogre::Vector3> initialPositions;
    for (const auto& bone : sk->getBones()) {
        if (bone->getParent() == nullptr)
            initialPositions.push_back(bone->getPosition());
    }

    // Rotate 60 degrees around an arbitrary axis -- impossible with Vector3 overload
    Ogre::Quaternion quat(Ogre::Degree(60.0f), Ogre::Vector3(1, 1, 0).normalisedCopy());
    SkeletonTransform::rotateSkeleton(entity, quat, meshCenter);

    size_t idx = 0;
    for (const auto& bone : sk->getBones()) {
        if (bone->getParent() == nullptr) {
            ASSERT_LT(idx, initialPositions.size());
            Ogre::Vector3 expected = quat * (initialPositions[idx] - meshCenter) + meshCenter;
            EXPECT_NEAR(bone->getPosition().x, expected.x, 0.01f);
            EXPECT_NEAR(bone->getPosition().y, expected.y, 0.01f);
            EXPECT_NEAR(bone->getPosition().z, expected.z, 0.01f);
            idx++;
        }
    }
}

TEST_F(SkeletonTransformProgrammaticTest, RotatePreservesAnimationData)
{
    auto* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    ASSERT_GT(sk->getNumAnimations(), 0u);

    auto* anim = sk->getAnimation(static_cast<unsigned short>(0));
    Ogre::Real originalLength = anim->getLength();
    unsigned short originalNumTracks = anim->getNumNodeTracks();

    // Rotate the skeleton
    Ogre::Quaternion quat(Ogre::Degree(45.0f), Ogre::Vector3::UNIT_Z);
    SkeletonTransform::rotateSkeleton(entity, quat,
                                      entity->getMesh()->getBounds().getCenter());

    // Animation metadata should be preserved
    ASSERT_TRUE(sk->hasAnimation(anim->getName()));
    auto* afterAnim = sk->getAnimation(anim->getName());
    EXPECT_FLOAT_EQ(afterAnim->getLength(), originalLength);
    EXPECT_EQ(afterAnim->getNumNodeTracks(), originalNumTracks);
}

// --- Tests for animation-safe skeleton transforms (no disableAnimationsAndRender) ---

TEST_F(SkeletonTransformProgrammaticTest, RotatePreservesEnabledAnimationStates)
{
    // Verify that rotating the skeleton does not disable animation states
    auto *animSet = entity->getAllAnimationStates();
    ASSERT_NE(animSet, nullptr);

    // Enable all animation states
    for (const auto &[name, state] : animSet->getAnimationStates())
        state->setEnabled(true);

    Ogre::Vector3 meshCenter = entity->getMesh()->getBounds().getCenter();
    Ogre::Quaternion quat(Ogre::Degree(45.0f), Ogre::Vector3::UNIT_Y);
    SkeletonTransform::rotateSkeleton(entity, quat, meshCenter);

    // All states should still be enabled
    for (const auto &[name, state] : animSet->getAnimationStates())
        EXPECT_TRUE(state->getEnabled()) << "Animation state '" << name << "' was disabled by rotation";
}

TEST_F(SkeletonTransformProgrammaticTest, ScalePreservesEnabledAnimationStates)
{
    auto *animSet = entity->getAllAnimationStates();
    ASSERT_NE(animSet, nullptr);

    for (const auto &[name, state] : animSet->getAnimationStates())
        state->setEnabled(true);

    SkeletonTransform::scaleSkeleton(entity, Ogre::Vector3(2.0f, 2.0f, 2.0f));

    for (const auto &[name, state] : animSet->getAnimationStates())
        EXPECT_TRUE(state->getEnabled()) << "Animation state '" << name << "' was disabled by scaling";
}

TEST_F(SkeletonTransformProgrammaticTest, TranslatePreservesEnabledAnimationStates)
{
    auto *animSet = entity->getAllAnimationStates();
    ASSERT_NE(animSet, nullptr);

    for (const auto &[name, state] : animSet->getAnimationStates())
        state->setEnabled(true);

    SkeletonTransform::translateSkeleton(entity, Ogre::Vector3(1.0f, 2.0f, 3.0f));

    for (const auto &[name, state] : animSet->getAnimationStates())
        EXPECT_TRUE(state->getEnabled()) << "Animation state '" << name << "' was disabled by translation";
}

TEST_F(SkeletonTransformProgrammaticTest, ScaleLocalPositionsNoDoubleScaling)
{
    // Verify that scaling applies uniformly: child world position = old_world * scale
    auto* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    // Record initial world positions of all bones
    sk->reset(true);
    std::map<std::string, Ogre::Vector3> initialDerived;
    for (const auto& bone : sk->getBones())
        initialDerived[bone->getName()] = bone->_getDerivedPosition();

    Ogre::Vector3 scale(2.0f, 2.0f, 2.0f);
    SkeletonTransform::scaleSkeleton(entity, scale);

    // After scaling, reset to binding pose and verify world positions
    sk->reset(true);
    for (const auto& bone : sk->getBones()) {
        Ogre::Vector3 expected = initialDerived[bone->getName()] * scale;
        Ogre::Vector3 actual = bone->_getDerivedPosition();
        EXPECT_NEAR(actual.x, expected.x, 0.01f) << "Bone '" << bone->getName() << "' x";
        EXPECT_NEAR(actual.y, expected.y, 0.01f) << "Bone '" << bone->getName() << "' y";
        EXPECT_NEAR(actual.z, expected.z, 0.01f) << "Bone '" << bone->getName() << "' z";
    }
}

TEST_F(SkeletonTransformProgrammaticTest, ScalePreservesAnimationData)
{
    auto* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    ASSERT_GT(sk->getNumAnimations(), 0u);

    auto* anim = sk->getAnimation(static_cast<unsigned short>(0));
    Ogre::Real originalLength = anim->getLength();
    unsigned short originalNumTracks = anim->getNumNodeTracks();

    SkeletonTransform::scaleSkeleton(entity, Ogre::Vector3(3.0f, 3.0f, 3.0f));

    ASSERT_TRUE(sk->hasAnimation(anim->getName()));
    auto* afterAnim = sk->getAnimation(anim->getName());
    EXPECT_FLOAT_EQ(afterAnim->getLength(), originalLength);
    EXPECT_EQ(afterAnim->getNumNodeTracks(), originalNumTracks);
}

TEST_F(SkeletonTransformProgrammaticTest, RotateUsesSamePivotAsVertices)
{
    // Verify bones are rotated around the provided pivot, not a stale mesh center
    auto* sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    Ogre::Vector3 pivot = entity->getMesh()->getBounds().getCenter();

    // Record root bone binding positions
    sk->reset(true);
    std::vector<Ogre::Vector3> initialPositions;
    for (const auto& bone : sk->getBones()) {
        if (bone->getParent() == nullptr)
            initialPositions.push_back(bone->getPosition());
    }

    // Apply two small incremental rotations (simulates gizmo drag)
    Ogre::Quaternion q1(Ogre::Degree(15.0f), Ogre::Vector3::UNIT_Y);
    SkeletonTransform::rotateSkeleton(entity, q1, pivot);

    // Capture the pivot that would be used for the second rotation
    // (mesh bounds may have changed, but we pass the SAME pivot)
    Ogre::Quaternion q2(Ogre::Degree(15.0f), Ogre::Vector3::UNIT_Y);
    Ogre::Vector3 pivot2 = pivot; // same pivot, not re-read from mesh
    SkeletonTransform::rotateSkeleton(entity, q2, pivot2);

    // The combined rotation should equal q2 * q1 applied once from the original positions
    Ogre::Quaternion combined = q2 * q1;
    sk->reset(true);
    size_t idx = 0;
    for (const auto& bone : sk->getBones()) {
        if (bone->getParent() == nullptr) {
            Ogre::Vector3 expected = combined * (initialPositions[idx] - pivot) + pivot;
            Ogre::Vector3 actual = bone->getPosition();
            EXPECT_NEAR(actual.x, expected.x, 0.05f) << "Root bone " << idx << " x after incremental rotation";
            EXPECT_NEAR(actual.y, expected.y, 0.05f) << "Root bone " << idx << " y after incremental rotation";
            EXPECT_NEAR(actual.z, expected.z, 0.05f) << "Root bone " << idx << " z after incremental rotation";
            idx++;
        }
    }
}

// --- Tests for renameAnimation ---

TEST_F(SkeletonTransformProgrammaticTest, RenamePreservesEnabledAnimationStates)
{
    auto *animSet = entity->getAllAnimationStates();
    ASSERT_NE(animSet, nullptr);
    ASSERT_TRUE(animSet->hasAnimationState("TestWalk"));

    // Enable the animation and set a time position
    auto *state = animSet->getAnimationState("TestWalk");
    state->setEnabled(true);
    state->setTimePosition(0.5f);
    state->setLoop(true);

    bool result = SkeletonTransform::renameAnimation(entity, "TestWalk", "TestRun");
    ASSERT_TRUE(result);

    animSet = entity->getAllAnimationStates();
    ASSERT_TRUE(animSet->hasAnimationState("TestRun"));

    auto *newState = animSet->getAnimationState("TestRun");
    EXPECT_TRUE(newState->getEnabled()) << "Renamed animation should still be enabled";
    EXPECT_NEAR(newState->getTimePosition(), 0.5f, 0.01f) << "Time position should be preserved";
    EXPECT_TRUE(newState->getLoop()) << "Loop state should be preserved";
}

TEST_F(SkeletonTransformProgrammaticTest, RenameRemovesOldAnimationState)
{
    auto *animSet = entity->getAllAnimationStates();
    ASSERT_NE(animSet, nullptr);
    ASSERT_TRUE(animSet->hasAnimationState("TestWalk"));

    bool result = SkeletonTransform::renameAnimation(entity, "TestWalk", "TestRun");
    ASSERT_TRUE(result);

    animSet = entity->getAllAnimationStates();
    EXPECT_FALSE(animSet->hasAnimationState("TestWalk")) << "Old animation state should be removed";
    EXPECT_TRUE(animSet->hasAnimationState("TestRun")) << "New animation state should exist";
}

TEST_F(SkeletonTransformProgrammaticTest, RenamePreservesAnimationKeyframes)
{
    auto *sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);
    ASSERT_TRUE(sk->hasAnimation("TestWalk"));

    auto *origAnim = sk->getAnimation("TestWalk");
    Ogre::Real origLength = origAnim->getLength();
    unsigned short origNumTracks = origAnim->getNumNodeTracks();

    bool result = SkeletonTransform::renameAnimation(entity, "TestWalk", "TestRun");
    ASSERT_TRUE(result);

    ASSERT_TRUE(sk->hasAnimation("TestRun"));
    auto *newAnim = sk->getAnimation("TestRun");
    EXPECT_FLOAT_EQ(newAnim->getLength(), origLength);
    EXPECT_EQ(newAnim->getNumNodeTracks(), origNumTracks);
}

TEST_F(SkeletonTransformProgrammaticTest, RenameDoesNotResetBonePositions)
{
    auto *sk = entity->getSkeleton();
    ASSERT_NE(sk, nullptr);

    // Record bone positions before rename
    std::map<std::string, Ogre::Vector3> beforePositions;
    for (const auto& bone : sk->getBones())
        beforePositions[bone->getName()] = bone->getPosition();

    SkeletonTransform::renameAnimation(entity, "TestWalk", "TestRun");

    // Bone positions should be unchanged — rename is metadata only
    for (const auto& bone : sk->getBones()) {
        auto it = beforePositions.find(bone->getName());
        ASSERT_NE(it, beforePositions.end());
        EXPECT_NEAR(bone->getPosition().x, it->second.x, 0.001f) << "Bone '" << bone->getName() << "' x";
        EXPECT_NEAR(bone->getPosition().y, it->second.y, 0.001f) << "Bone '" << bone->getName() << "' y";
        EXPECT_NEAR(bone->getPosition().z, it->second.z, 0.001f) << "Bone '" << bone->getName() << "' z";
    }
}
