#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include "commands/SkeletonResolver.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreSkeletonInstance.h>
#include <OgreEntity.h>

class SkeletonResolverTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre());
        createStandardOgreMaterials();
        ASSERT_TRUE(canLoadMeshFiles());
    }
    void TearDown() override {
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(20);
    }
    QApplication* app = nullptr;
};

TEST_F(SkeletonResolverTest, ResolveReturnsSkeletonForLiveEntity) {
    Ogre::Entity* entity = createAnimatedTestEntity("SR_Live");
    ASSERT_NE(entity, nullptr);

    Ogre::SkeletonInstance* resolved = SkeletonResolver::resolve(entity->getName());
    EXPECT_EQ(resolved, entity->getSkeleton());
}

TEST_F(SkeletonResolverTest, ResolveReturnsNullForUnknownEntity) {
    EXPECT_EQ(SkeletonResolver::resolve("does_not_exist"), nullptr);
}

TEST_F(SkeletonResolverTest, ResolveReturnsNullForEmptyName) {
    EXPECT_EQ(SkeletonResolver::resolve(""), nullptr);
}

TEST_F(SkeletonResolverTest, EntityNameRoundTripsThroughResolver) {
    Ogre::Entity* entity = createAnimatedTestEntity("SR_RoundTrip");
    ASSERT_NE(entity, nullptr);

    const std::string name = SkeletonResolver::entityNameForSkeleton(entity->getSkeleton());
    EXPECT_EQ(name, entity->getName());

    EXPECT_EQ(SkeletonResolver::resolve(name), entity->getSkeleton());
}

TEST_F(SkeletonResolverTest, EntityNameForNullSkeletonIsEmpty) {
    EXPECT_TRUE(SkeletonResolver::entityNameForSkeleton(nullptr).empty());
}
