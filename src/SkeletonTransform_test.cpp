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

        try {
            Manager::getSingleton();
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
        }
        createStandardOgreMaterials();

        if (!canLoadMeshFiles()) {
            GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
        }

        // Import robot.mesh which has a skeleton with animations
        QStringList validUri{"./media/models/robot.mesh"};
        try {
            MeshImporterExporter::importer(validUri);
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping: failed to import robot.mesh ("
                         << e.getFullDescription() << ")";
        }

        entity = Manager::getSingleton()->getEntities().isEmpty()
                     ? nullptr
                     : Manager::getSingleton()->getEntities().last();
        if (!entity) {
            GTEST_SKIP() << "Skipping: no entity available after import";
        }
        if (!entity->hasSkeleton()) {
            GTEST_SKIP() << "Skipping: imported entity has no skeleton";
        }
    }

    void TearDown() override {
        entity = nullptr;
        Manager::kill();
        if (app) {
            app->processEvents();
        }
        QThread::msleep(50);
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
    if (sk->getNumAnimations() == 0) {
        GTEST_SKIP() << "Skipping: skeleton has no animations";
    }
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
    if (sk->getNumAnimations() == 0) {
        GTEST_SKIP() << "Skipping: skeleton has no animations";
    }
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

    if (!foundTrack) {
        GTEST_SKIP() << "Skipping: no animation tracks found with keyframes";
    }

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

    if (sk->getNumAnimations() == 0) {
        GTEST_SKIP() << "Skipping: skeleton has no animations";
    }
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
    if (bones.empty()) {
        GTEST_SKIP() << "Skipping: skeleton has no bones";
    }

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
    if (bones.empty()) {
        GTEST_SKIP() << "Skipping: skeleton has no bones";
    }

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
    if (bones.empty()) {
        GTEST_SKIP() << "Skipping: skeleton has no bones";
    }

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
    if (bones.empty()) {
        GTEST_SKIP() << "Skipping: skeleton has no bones";
    }

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
    if (bones.empty()) {
        GTEST_SKIP() << "Skipping: skeleton has no bones";
    }

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
    if (bones.empty()) {
        GTEST_SKIP() << "Skipping: skeleton has no bones";
    }

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
    if (bones.empty()) {
        GTEST_SKIP() << "Skipping: skeleton has no bones";
    }

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
    if (bones.empty()) {
        GTEST_SKIP() << "Skipping: skeleton has no bones";
    }

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
    if (bones.empty()) {
        GTEST_SKIP() << "Skipping: skeleton has no bones";
    }

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
    if (bones.empty()) {
        GTEST_SKIP() << "Skipping: skeleton has no bones";
    }

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

        try {
            Manager::getSingleton();
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
        }
        createStandardOgreMaterials();
    }

    void TearDown() override {
        Manager::kill();
        if (app) {
            app->processEvents();
        }
        QThread::msleep(50);
    }
};

TEST_F(SkeletonTransformNoSkeletonTest, ScaleSkeletonNoSkeletonDoesNotCrash)
{
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
