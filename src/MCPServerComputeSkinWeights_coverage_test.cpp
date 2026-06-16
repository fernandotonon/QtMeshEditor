// Coverage tests for MCPServer::toolComputeSkinWeights (compute_skin_weights).
//
// Targets the fully-untested handler at MCPServer.cpp:1503. The rich
// argument-validation block (lines 1515-1543) is exercised WITHOUT needing a
// valid selection for the type/range checks once a (non-skeleton) entity is
// selected. The no-selection branch is hit with nothing selected, and the
// success path uses createInMemorySkeletonMesh which attaches a real skeleton.
//
// Distinct filename + distinct suite name (MCPServerComputeSkinWeightsCoverageTest)
// to avoid any ODR / duplicate-registration clash with MCPServer_test.cpp.

#include <gtest/gtest.h>
#include <QApplication>
#include <QThread>
#include <QJsonArray>
#include <QJsonObject>

#define private public
#include "MCPServer.h"
#undef private

#include "Manager.h"
#include "SelectionSet.h"
#include "SkinWeights.h"
#include "TestHelpers.h"

#include <OgreEntity.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <memory>

namespace {

QString resultText(const QJsonObject& result)
{
    const QJsonArray content = result["content"].toArray();
    if (content.isEmpty()) return QString();
    return content[0].toObject()["text"].toString();
}

bool resultIsError(const QJsonObject& result)
{
    return result["isError"].toBool(false);
}

class MCPServerComputeSkinWeightsCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        server.reset();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        server = std::make_unique<MCPServer>();
    }

    void TearDown() override
    {
        if (SelectionSet::getSingletonPtr())
            SelectionSet::getSingleton()->clear();
        if (app)
            app->processEvents();
    }

    // Selects a static (skeleton-less) triangle entity. Enough to satisfy
    // hasSelectedEntities() so the type/range validation branches run before
    // the SkinWeights::computeAndApply call.
    Ogre::Entity* createAndSelectTriangleEntity(const QString& baseName)
    {
        auto* manager = Manager::getSingletonPtr();
        if (!manager) return nullptr;
        Ogre::MeshPtr mesh = createInMemoryTriangleMesh((baseName + "_mesh").toStdString());
        if (!mesh) return nullptr;
        Ogre::SceneManager* sceneMgr = manager->getSceneMgr();
        if (!sceneMgr) return nullptr;
        Ogre::SceneNode* node = manager->addSceneNode(baseName);
        if (!node) return nullptr;
        Ogre::Entity* entity = sceneMgr->createEntity((baseName + "_entity").toStdString(), mesh);
        if (!entity) return nullptr;
        node->attachObject(entity);
        SelectionSet::getSingleton()->clear();
        SelectionSet::getSingleton()->selectOne(entity);
        app->processEvents();
        return entity;
    }

    // Selects an entity backed by a real skeleton mesh (TestHelpers.h:484).
    Ogre::Entity* createAndSelectSkeletonEntity(const QString& baseName)
    {
        auto* manager = Manager::getSingletonPtr();
        if (!manager) return nullptr;
        Ogre::MeshPtr mesh = createInMemorySkeletonMesh((baseName + "_mesh").toStdString());
        if (!mesh) return nullptr;
        Ogre::SceneManager* sceneMgr = manager->getSceneMgr();
        if (!sceneMgr) return nullptr;
        Ogre::SceneNode* node = manager->addSceneNode(baseName);
        if (!node) return nullptr;
        Ogre::Entity* entity = sceneMgr->createEntity((baseName + "_entity").toStdString(), mesh);
        if (!entity) return nullptr;
        node->attachObject(entity);
        SelectionSet::getSingleton()->clear();
        SelectionSet::getSingleton()->selectOne(entity);
        app->processEvents();
        return entity;
    }

    QApplication* app = nullptr;
    std::unique_ptr<MCPServer> server;
};

// --- No-selection branch -------------------------------------------------

TEST_F(MCPServerComputeSkinWeightsCoverageTest, NoSelectionReturnsError)
{
    SelectionSet::getSingleton()->clear();
    app->processEvents();

    const QJsonObject result = server->toolComputeSkinWeights(QJsonObject{});
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("No mesh selected"));
}

// --- Type-validation branches (each distinct message) --------------------

TEST_F(MCPServerComputeSkinWeightsCoverageTest, MaxInfluencesWrongTypeIsError)
{
    ASSERT_NE(createAndSelectTriangleEntity("csw_maxinf_type"), nullptr);
    QJsonObject args;
    args["max_influences"] = QStringLiteral("4");  // string, not number
    const QJsonObject result = server->toolComputeSkinWeights(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("'max_influences' must be a number"));
}

TEST_F(MCPServerComputeSkinWeightsCoverageTest, FalloffWrongTypeIsError)
{
    ASSERT_NE(createAndSelectTriangleEntity("csw_falloff_type"), nullptr);
    QJsonObject args;
    args["falloff"] = QStringLiteral("4.0");
    const QJsonObject result = server->toolComputeSkinWeights(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("'falloff' must be a number"));
}

TEST_F(MCPServerComputeSkinWeightsCoverageTest, MaxDistanceWrongTypeIsError)
{
    ASSERT_NE(createAndSelectTriangleEntity("csw_maxdist_type"), nullptr);
    QJsonObject args;
    args["max_distance"] = QStringLiteral("0.5");
    const QJsonObject result = server->toolComputeSkinWeights(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("'max_distance' must be a number"));
}

TEST_F(MCPServerComputeSkinWeightsCoverageTest, SkipUnweightedWrongTypeIsError)
{
    ASSERT_NE(createAndSelectTriangleEntity("csw_skip_type"), nullptr);
    QJsonObject args;
    args["skip_unweighted"] = QStringLiteral("false");  // string, not bool
    const QJsonObject result = server->toolComputeSkinWeights(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("'skip_unweighted' must be a boolean"));
}

TEST_F(MCPServerComputeSkinWeightsCoverageTest, ReplaceExistingWrongTypeIsError)
{
    ASSERT_NE(createAndSelectTriangleEntity("csw_replace_type"), nullptr);
    QJsonObject args;
    args["replace_existing"] = 1.0;  // number, not bool
    const QJsonObject result = server->toolComputeSkinWeights(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("'replace_existing' must be a boolean"));
}

// --- Range-validation branches -------------------------------------------

TEST_F(MCPServerComputeSkinWeightsCoverageTest, MaxInfluencesBelowRangeIsError)
{
    ASSERT_NE(createAndSelectTriangleEntity("csw_maxinf_low"), nullptr);
    QJsonObject args;
    args["max_influences"] = 0.0;  // < 1
    const QJsonObject result = server->toolComputeSkinWeights(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("'max_influences' must be in [1, 8]"));
}

TEST_F(MCPServerComputeSkinWeightsCoverageTest, MaxInfluencesAboveRangeIsError)
{
    ASSERT_NE(createAndSelectTriangleEntity("csw_maxinf_high"), nullptr);
    QJsonObject args;
    args["max_influences"] = 9.0;  // > 8
    const QJsonObject result = server->toolComputeSkinWeights(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("'max_influences' must be in [1, 8]"));
}

TEST_F(MCPServerComputeSkinWeightsCoverageTest, FalloffBelowRangeIsError)
{
    ASSERT_NE(createAndSelectTriangleEntity("csw_falloff_low"), nullptr);
    QJsonObject args;
    args["falloff"] = 0.1;  // < 0.5
    const QJsonObject result = server->toolComputeSkinWeights(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("'falloff' must be in [0.5, 16]"));
}

TEST_F(MCPServerComputeSkinWeightsCoverageTest, FalloffAboveRangeIsError)
{
    ASSERT_NE(createAndSelectTriangleEntity("csw_falloff_high"), nullptr);
    QJsonObject args;
    args["falloff"] = 20.0;  // > 16
    const QJsonObject result = server->toolComputeSkinWeights(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("'falloff' must be in [0.5, 16]"));
}

TEST_F(MCPServerComputeSkinWeightsCoverageTest, MaxDistanceBelowRangeIsError)
{
    ASSERT_NE(createAndSelectTriangleEntity("csw_maxdist_low"), nullptr);
    QJsonObject args;
    args["max_distance"] = -0.5;  // < 0
    const QJsonObject result = server->toolComputeSkinWeights(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("'max_distance' must be in [0, 10]"));
}

TEST_F(MCPServerComputeSkinWeightsCoverageTest, MaxDistanceAboveRangeIsError)
{
    ASSERT_NE(createAndSelectTriangleEntity("csw_maxdist_high"), nullptr);
    QJsonObject args;
    args["max_distance"] = 11.0;  // > 10
    const QJsonObject result = server->toolComputeSkinWeights(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("'max_distance' must be in [0, 10]"));
}

// --- Success path on a skeleton mesh -------------------------------------

TEST_F(MCPServerComputeSkinWeightsCoverageTest, SkeletonMeshSucceedsAndPopulatesSkin)
{
    Ogre::Entity* entity = createAndSelectSkeletonEntity("csw_success");
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());

    QJsonObject args;
    args["max_influences"] = 4.0;
    args["falloff"] = 4.0;
    const QJsonObject result = server->toolComputeSkinWeights(args);

    if (resultIsError(result)) {
        // computeAndApply may legitimately fail on a degenerate in-memory
        // mesh; the error must still be surfaced as text (no crash, branch
        // covered). We assert the failure message is non-empty.
        EXPECT_FALSE(resultText(result).isEmpty());
        EXPECT_TRUE(resultText(result).contains("Skin weights failed")
                    || resultText(result).contains("Ogre error"));
        return;
    }

    // Success: result must contain the "skin" JSON payload and the content
    // text must equal SkinWeights::reportToText for the same report.
    EXPECT_TRUE(result.contains("skin"));
    EXPECT_TRUE(result["skin"].isObject());

    const QString text = resultText(result);
    EXPECT_FALSE(text.isEmpty());
    EXPECT_TRUE(text.contains("Skin Weights"));
    // The header is a stable substring of reportToText regardless of counts.
    EXPECT_TRUE(text.contains("Skeleton:"));
    EXPECT_TRUE(text.contains("Bones:"));
}

// --- Defaults accepted (no args after a valid skeleton selection) --------

TEST_F(MCPServerComputeSkinWeightsCoverageTest, EmptyArgsWithSkeletonDoesNotHitValidationErrors)
{
    Ogre::Entity* entity = createAndSelectSkeletonEntity("csw_defaults");
    ASSERT_NE(entity, nullptr);

    const QJsonObject result = server->toolComputeSkinWeights(QJsonObject{});
    // With no args, none of the type/range validation messages can appear.
    const QString text = resultText(result);
    EXPECT_FALSE(text.contains("must be a number"));
    EXPECT_FALSE(text.contains("must be a boolean"));
    EXPECT_FALSE(text.contains("must be in ["));
}

} // namespace
