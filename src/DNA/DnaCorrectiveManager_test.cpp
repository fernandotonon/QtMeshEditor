// Epic #1095 phase 2: DnaCorrectiveManager on a live skeleton, driven by a FAKE
// rig so the scene-level behaviour (driver read-out, absolute corrective writes,
// toggle, preview pose, missing joints) is covered in every CI lane without a
// MetaHuman asset. The real RigLogic numbers are covered in DnaRigLogic_test.

#include <gtest/gtest.h>

#include "DnaCorrectiveManager.h"
#include "DnaRigLogic.h"

#include "../Manager.h"
#include "../TestHelpers.h"

#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>
#include <OgreSkeletonManager.h>

#include <cmath>

using namespace Dna;

namespace {

// Driver "Child"; corrective "Corr" (and a ghost that the skeleton lacks).
// Corrective translation y = 0.1 * (rotation angle of the driver about Z).
class FakeRig : public RigEvaluator
{
public:
    explicit FakeRig(bool withDrivers = true, bool withGhost = false)
        : m_drivers(withDrivers), m_ghost(withGhost)
    {
        m_driven = {0};
        if (withGhost) m_driven.push_back(1);
    }
    QString jointName(int j) const override { return j == 0 ? QStringLiteral("Corr") : QStringLiteral("Ghost"); }
    QStringList driverJoints() const override { return m_drivers ? QStringList{QStringLiteral("Child")} : QStringList(); }
    const std::vector<int>& drivenJoints() const override { return m_driven; }
    int rbfSolverCount() const override { return 1; }
    int rbfPoseCount() const override { return 2; }
    void resetControls() override { lastDriver = {1, 0, 0, 0}; ++resets; }
    bool setDriverRotation(const QString& joint, const std::array<double, 4>& q) override
    {
        if (joint != QLatin1String("Child")) return false;
        lastDriver = q;
        return true;
    }
    double calculate() override { ++calcs; return 0.001; }
    std::vector<JointPose> drivenPoses() const override
    {
        const double angle = 2.0 * std::atan2(std::abs(lastDriver[3]), lastDriver[0]);
        std::vector<JointPose> out;
        for (int j : m_driven) {
            JointPose p;
            p.joint = j;
            p.translation = {0.0, 0.1 * angle, 0.0};
            p.orientation = {1, 0, 0, 0};
            p.scale = {1, 1, 1};
            out.push_back(p);
        }
        return out;
    }

    std::array<double, 4> lastDriver{1, 0, 0, 0};
    int resets = 0, calcs = 0;

private:
    bool m_drivers, m_ghost;
    std::vector<int> m_driven;
};

std::array<double, 4> aboutZ(double degrees)
{
    const double h = degrees * M_PI / 360.0;
    return {std::cos(h), 0.0, 0.0, std::sin(h)};
}

class DnaCorrectiveSceneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        DnaCorrectiveManager::instance()->clear();
        DnaCorrectiveManager::instance()->setEnabled(true);
        const std::string name = "dna_corr_test_" + std::to_string(++counter);
        entityName = QString::fromStdString(name);
        // Root -> Child (driver) -> Corr (corrective), bound to a skinned triangle.
        Ogre::MeshPtr mesh = createInMemorySkeletonMesh(name);
        auto skel = Ogre::SkeletonManager::getSingleton().getByName(name + "_skel");
        ASSERT_TRUE(skel);
        Ogre::Bone* corr = skel->createBone("Corr", 2);
        corr->setPosition(Ogre::Vector3(0, 0.5f, 0));
        skel->getBone("Child")->addChild(corr);
        skel->setBindingPose();
        Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode(entityName);
        entity = Manager::getSingleton()->createEntity(node, mesh);
        ASSERT_NE(entity, nullptr);
        ASSERT_TRUE(entity->hasSkeleton());
    }
    void TearDown() override
    {
        DnaCorrectiveManager::instance()->clear();
        DnaCorrectiveManager::instance()->setEnabled(true);
        if (auto* m = Manager::getSingletonPtr())
            if (auto* scene = m->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
    }
    Ogre::Bone* bone(const char* n) { return entity->getSkeleton()->getBone(n); }

    static inline int counter = 0;
    QString entityName;
    Ogre::Entity* entity = nullptr;
};

}  // namespace

// ---- pure ------------------------------------------------------------------

TEST(DnaCorrectiveMath, LocalDeltaIsNeutralInverseTimesCurrent)
{
    const auto id = DnaCorrectiveManager::localDelta({1, 0, 0, 0}, {1, 0, 0, 0});
    EXPECT_NEAR(id[0], 1.0, 1e-6);
    const auto neutral = aboutZ(30);
    const auto current = aboutZ(120);          // neutral * delta(90)
    const auto d = DnaCorrectiveManager::localDelta(neutral, current);
    const auto want = aboutZ(90);
    for (int i = 0; i < 4; ++i) EXPECT_NEAR(d[size_t(i)], want[size_t(i)], 1e-5);
}

TEST(DnaCorrectiveManagerBasics, RefusesNullAndDriverlessRigs)
{
    auto* m = DnaCorrectiveManager::instance();
    EXPECT_FALSE(m->attach("x", nullptr));
    EXPECT_FALSE(m->attach("x", std::make_shared<FakeRig>(/*withDrivers=*/false)));
    EXPECT_EQ(m->characterCount(), 0);
    EXPECT_EQ(m->summary(), "No DNA character");
    EXPECT_TRUE(m->statsMap().value("characters").toInt() == 0);
}

// ---- scene ---------------------------------------------------------------

TEST_F(DnaCorrectiveSceneTest, DriverRotationIsReadRelativeToNeutral)
{
    auto rig = std::make_shared<FakeRig>();
    auto* m = DnaCorrectiveManager::instance();
    ASSERT_TRUE(m->attach(entityName, rig));
    ASSERT_TRUE(m->setPoseOverride(entityName, "Child", aboutZ(90)));
    ASSERT_TRUE(m->evaluateNow());
    EXPECT_NEAR(rig->lastDriver[0], std::cos(M_PI / 4), 1e-4);
    EXPECT_NEAR(rig->lastDriver[3], std::sin(M_PI / 4), 1e-4);
    EXPECT_GE(rig->calcs, 1);
}

TEST_F(DnaCorrectiveSceneTest, CorrectiveBoneIsWrittenAsAnAbsoluteLocalTransform)
{
    auto* m = DnaCorrectiveManager::instance();
    ASSERT_TRUE(m->attach(entityName, std::make_shared<FakeRig>()));
    // At rest the fake rig yields y = 0 — absolute, NOT neutral + 0.
    ASSERT_TRUE(m->evaluateNow());
    EXPECT_NEAR(bone("Corr")->getPosition().y, 0.0f, 1e-5f);
    ASSERT_TRUE(m->setPoseOverride(entityName, "Child", aboutZ(90)));
    ASSERT_TRUE(m->evaluateNow());
    EXPECT_NEAR(bone("Corr")->getPosition().y, 0.1 * (M_PI / 2), 1e-3);
    // Evaluating again must not accumulate (the skeleton is re-posed first).
    ASSERT_TRUE(m->evaluateNow());
    EXPECT_NEAR(bone("Corr")->getPosition().y, 0.1 * (M_PI / 2), 1e-3);
    // The driver itself carries neutral * delta.
    EXPECT_NEAR(std::abs(bone("Child")->getOrientation().Dot(
                    bone("Child")->getInitialOrientation() * Ogre::Quaternion(Ogre::Radian(M_PI / 2), Ogre::Vector3::UNIT_Z))),
                1.0f, 1e-4f);
}

TEST_F(DnaCorrectiveSceneTest, DisablingRestoresPlainSkinningButKeepsThePreviewPose)
{
    auto* m = DnaCorrectiveManager::instance();
    ASSERT_TRUE(m->attach(entityName, std::make_shared<FakeRig>()));
    ASSERT_TRUE(m->setPoseOverride(entityName, "Child", aboutZ(90)));
    ASSERT_TRUE(m->evaluateNow());
    ASSERT_GT(bone("Corr")->getPosition().y, 0.1f);

    m->setEnabled(false);
    EXPECT_FALSE(m->enabled());
    // Corrective bone back at its neutral (initial) position; the driver stays bent.
    EXPECT_NEAR(bone("Corr")->getPosition().y, 0.5f, 1e-5f);
    EXPECT_LT(std::abs(bone("Child")->getOrientation().Dot(Ogre::Quaternion::IDENTITY)), 0.99f);

    m->setEnabled(true);
    EXPECT_GT(bone("Corr")->getPosition().y, 0.1f);
    EXPECT_NEAR(bone("Corr")->getPosition().y, 0.1 * (M_PI / 2), 1e-3);
}

TEST_F(DnaCorrectiveSceneTest, ClearingOverridesReturnsTheCharacterToRest)
{
    auto* m = DnaCorrectiveManager::instance();
    ASSERT_TRUE(m->attach(entityName, std::make_shared<FakeRig>()));
    ASSERT_TRUE(m->setPoseOverride(entityName, "Child", aboutZ(90)));
    ASSERT_TRUE(m->evaluateNow());
    m->clearPoseOverrides(entityName);
    EXPECT_NEAR(bone("Corr")->getPosition().y, 0.0f, 1e-5f);   // rig output at rest
    EXPECT_NEAR(std::abs(bone("Child")->getOrientation().Dot(Ogre::Quaternion::IDENTITY)), 1.0f, 1e-5f);
}

TEST_F(DnaCorrectiveSceneTest, MissingCorrectiveJointsAreCountedNotFatal)
{
    auto* m = DnaCorrectiveManager::instance();
    ASSERT_TRUE(m->attach(entityName, std::make_shared<FakeRig>(true, /*withGhost=*/true)));
    ASSERT_TRUE(m->evaluateNow());
    const auto st = m->statsFor(entityName);
    EXPECT_EQ(st.drivers, 1);
    EXPECT_EQ(st.driven, 1);
    EXPECT_EQ(st.missingDriven, 1);
    EXPECT_TRUE(m->summary().contains("not in skeleton"));
    EXPECT_EQ(m->statsMap(entityName).value("correctiveJointsMissing").toInt(), 1);
}

TEST_F(DnaCorrectiveSceneTest, UnknownBoneOverrideIsRejected)
{
    auto* m = DnaCorrectiveManager::instance();
    ASSERT_TRUE(m->attach(entityName, std::make_shared<FakeRig>()));
    EXPECT_FALSE(m->setPoseOverride(entityName, "NoSuchBone", aboutZ(10)));
    EXPECT_FALSE(m->setPoseOverride("not_an_entity", "Child", aboutZ(10)));
}

TEST_F(DnaCorrectiveSceneTest, ADestroyedEntityIsDroppedNotDereferenced)
{
    auto* m = DnaCorrectiveManager::instance();
    ASSERT_TRUE(m->attach(entityName, std::make_shared<FakeRig>()));
    Manager::getSingleton()->getSceneMgr()->destroyAllEntities();
    entity = nullptr;
    EXPECT_FALSE(m->evaluateNow());
    EXPECT_EQ(m->characterCount(), 0);
}

TEST_F(DnaCorrectiveSceneTest, ADestroyedEntityIsDroppedEvenWhileCorrectivesAreOff)
{
    auto* m = DnaCorrectiveManager::instance();
    ASSERT_TRUE(m->attach(entityName, std::make_shared<FakeRig>()));
    m->setEnabled(false);
    Manager::getSingleton()->getSceneMgr()->destroyAllEntities();
    entity = nullptr;
    m->evaluateNow();
    EXPECT_EQ(m->characterCount(), 0);
}

TEST_F(DnaCorrectiveSceneTest, SkinnedPositionsFollowTheBentDriver)
{
    auto* m = DnaCorrectiveManager::instance();
    ASSERT_TRUE(m->attach(entityName, std::make_shared<FakeRig>()));
    ASSERT_TRUE(m->evaluateNow());
    const std::vector<float> rest = DnaCorrectiveManager::skinnedPositions(entity);
    ASSERT_EQ(rest.size(), 9u);
    ASSERT_TRUE(m->setPoseOverride(entityName, "Child", aboutZ(90)));
    ASSERT_TRUE(m->evaluateNow());
    const std::vector<float> bent = DnaCorrectiveManager::skinnedPositions(entity);
    ASSERT_EQ(bent.size(), 9u);
    double moved = 0;
    for (size_t i = 0; i < 9; ++i) moved += std::abs(bent[i] - rest[i]);
    EXPECT_GT(moved, 0.1);
}
