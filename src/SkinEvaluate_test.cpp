#include "SkinEvaluate.h"
#include "SkinWeights.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QJsonObject>

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>

#include <string>

#include "Manager.h"
#include "MeshImporterExporter.h"
#include "TestHelpers.h"

// Slice E (#819): `qtmesh skin --evaluate/--compare` backing code.
// Uses live skinned entities (Linux CI; macOS local runs stop at the
// test_main Ogre gate like every other Ogre-backed suite).

namespace {

std::string uniqueName(const char* prefix)
{
    static int counter = 0;
    return std::string(prefix) + "_se_" + std::to_string(counter++);
}

} // namespace

class SkinEvaluateTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(qobject_cast<QApplication*>(QCoreApplication::instance()),
                  nullptr);
        ASSERT_TRUE(tryInitOgre())
            << "Ogre init failed — invalid CI/runtime environment";
        ASSERT_TRUE(canLoadMeshFiles()) << "no GL context";
        createStandardOgreMaterials();
    }
};

TEST_F(SkinEvaluateTest, ExtractGuardsAndSucceeds)
{
    QString err;
    SkinEvaluate::EvalData data;
    EXPECT_FALSE(SkinEvaluate::extract(nullptr, data, &err));
    EXPECT_FALSE(err.isEmpty());

    // Skeleton-less entity fails cleanly.
    Ogre::MeshPtr mesh = createInMemoryTriangleMesh(uniqueName("static"));
    ASSERT_TRUE(mesh);
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* staticEnt =
        sceneMgr->createEntity(uniqueName("static_ent"), mesh);
    err.clear();
    EXPECT_FALSE(SkinEvaluate::extract(staticEnt, data, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("skeleton")));
    sceneMgr->destroyEntity(staticEnt);

    // A real skinned entity extracts: 3 shared verts, bone 1 carries
    // weight 1.0 on each (createAnimatedTestEntity's fixture).
    Ogre::Entity* ent = createAnimatedTestEntity(uniqueName("hero"));
    ASSERT_NE(ent, nullptr);
    ASSERT_TRUE(SkinEvaluate::extract(ent, data, &err))
        << err.toStdString();
    EXPECT_EQ(int(data.positions.size() / 3), 3);
    EXPECT_EQ(data.totalBones, 2);
    ASSERT_EQ(int(data.weights.size()), 3);
    for (const auto& vw : data.weights) {
        ASSERT_EQ(vw.count, 1);
        EXPECT_EQ(vw.boneIndices[0], 1);
        EXPECT_FLOAT_EQ(float(vw.weights[0]), 1.0f);
    }
    EXPECT_EQ(data.boneNames.size(), 2);
}

TEST_F(SkinEvaluateTest, EvaluateReportsCoreMetrics)
{
    Ogre::Entity* ent = createAnimatedTestEntity(uniqueName("metrics"));
    ASSERT_NE(ent, nullptr);

    QString err;
    const QJsonObject report = SkinEvaluate::evaluate(ent, 32, &err);
    ASSERT_FALSE(report.isEmpty()) << err.toStdString();
    EXPECT_EQ(report["vertices"].toInt(), 3);
    EXPECT_EQ(report["totalBones"].toInt(), 2);
    EXPECT_EQ(report["weightedVertices"].toInt(), 3);
    ASSERT_TRUE(report.contains("influenceHistogram"));
    const auto h = report["influenceHistogram"].toObject();
    EXPECT_EQ(h["maxInfluences"].toInt(), 1);
    // A lone triangle encloses no volume → the bleed metric is
    // skipped with a reason, never fails the evaluation.
    EXPECT_TRUE(report.contains("bleedFraction")
                || report.contains("bleedFractionSkipped"));

    // Text rendering carries the headline numbers.
    const QString txt = SkinEvaluate::reportToText(report);
    EXPECT_TRUE(txt.contains(QStringLiteral("Skin Evaluation")));
    EXPECT_TRUE(txt.contains(QStringLiteral("Vertices")));
}

TEST_F(SkinEvaluateTest, CompareIsNearZeroAgainstItself)
{
    // Comparing an entity against a second entity with identical
    // geometry + weights must produce ~zero difference — the
    // position-matching identity case.
    Ogre::Entity* a = createAnimatedTestEntity(uniqueName("cmp_a"));
    Ogre::Entity* b = createAnimatedTestEntity(uniqueName("cmp_b"));
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    QString err;
    const QJsonObject report = SkinEvaluate::compare(a, b, &err);
    ASSERT_FALSE(report.isEmpty()) << err.toStdString();
    EXPECT_EQ(int(report["verticesCompared"].toDouble()), 3);
    EXPECT_EQ(int(report["verticesUnmatched"].toDouble()), 0);
    EXPECT_NEAR(report["meanWeightL1Diff"].toDouble(), 0.0, 1e-9);
}

// Env-gated Mixamo-reference comparison (docs/SKINNING_QUALITY.md):
// set QTMESH_SKIN_OURS_FBX + QTMESH_SKIN_REF_FBX to run it; skipped
// otherwise so CI stays hermetic. (This suite has unconditional
// tests above, so the skip never leaves the suite empty.)
TEST_F(SkinEvaluateTest, CompareAgainstReference)
{
    const QByteArray ours = qgetenv("QTMESH_SKIN_OURS_FBX");
    const QByteArray ref  = qgetenv("QTMESH_SKIN_REF_FBX");
    if (ours.isEmpty() || ref.isEmpty())
        GTEST_SKIP() << "QTMESH_SKIN_OURS_FBX / QTMESH_SKIN_REF_FBX not set";

    auto entitiesNow = []() {
        QList<Ogre::Entity*> out;
        for (Ogre::Entity* e : Manager::getSingleton()->getEntities())
            if (e && e->getMovableType() == "Entity") out.push_back(e);
        return out;
    };
    const auto before = entitiesNow();
    MeshImporterExporter::importer({QString::fromLocal8Bit(ours)});
    Ogre::Entity* oursEnt = nullptr;
    for (Ogre::Entity* e : entitiesNow())
        if (!before.contains(e)) { oursEnt = e; break; }
    ASSERT_NE(oursEnt, nullptr) << "failed to load " << ours.constData();

    const auto mid = entitiesNow();
    MeshImporterExporter::importer({QString::fromLocal8Bit(ref)});
    Ogre::Entity* refEnt = nullptr;
    for (Ogre::Entity* e : entitiesNow())
        if (!mid.contains(e)) { refEnt = e; break; }
    ASSERT_NE(refEnt, nullptr) << "failed to load " << ref.constData();

    QString err;
    const QJsonObject report = SkinEvaluate::compare(oursEnt, refEnt, &err);
    ASSERT_FALSE(report.isEmpty()) << err.toStdString();
    ASSERT_GT(report["verticesCompared"].toDouble(), 0.0);
    // The #819 target for GVB vs the Mixamo reference.
    EXPECT_LE(report["meanWeightL1Diff"].toDouble(), 0.4)
        << "mean per-vertex weight L1 diff vs the reference exceeds "
           "the documented budget";
}
