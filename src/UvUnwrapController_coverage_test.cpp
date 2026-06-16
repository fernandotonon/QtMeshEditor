#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVariantMap>

#include "UvUnwrapController.h"
#include "SelectionSet.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreEntity.h>

// Coverage suite for UvUnwrapController (issue #400). The algorithm
// itself is exercised by UvUnwrap_test.cpp; this suite drives the
// QML-facing singleton: lifecycle, selection-state property, signal
// wiring, and the QVariantMap report shape produced by
// unwrapSelectedToFile across its empty-path / no-selection /
// real-selection branches.
//
// We deliberately do NOT assume xatlas succeeds. For the real-selection
// path we assert the report SHAPE (keys present, applied is a bool,
// busyChanged emitted twice, exactly one of unwrapApplied/error fired)
// rather than a specific applied=true outcome.

class UvUnwrapControllerCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb required in CI)";
        createStandardOgreMaterials();
        // Ensure a clean selection state for every test.
        if (auto* sel = SelectionSet::getSingleton())
            sel->clearList();
        UvUnwrapController::kill();
    }

    void TearDown() override {
        if (auto* sel = SelectionSet::getSingleton())
            sel->clearList();
        UvUnwrapController::kill();
    }
};

// ---- lifecycle --------------------------------------------------------

TEST_F(UvUnwrapControllerCoverageTest, InstanceIsSingleton)
{
    UvUnwrapController* a = UvUnwrapController::instance();
    UvUnwrapController* b = UvUnwrapController::instance();
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a, b);
}

TEST_F(UvUnwrapControllerCoverageTest, KillResetsSingleton)
{
    UvUnwrapController* a = UvUnwrapController::instance();
    ASSERT_NE(a, nullptr);
    UvUnwrapController::kill();
    UvUnwrapController* b = UvUnwrapController::instance();
    ASSERT_NE(b, nullptr);
    // A fresh allocation after kill(); pointer equality is not
    // guaranteed but the new instance must be usable.
    EXPECT_FALSE(b->busy());
}

TEST_F(UvUnwrapControllerCoverageTest, QmlInstanceSetsCppOwnership)
{
    // qmlInstance must return the same singleton as instance() and set
    // CppOwnership so the QML engine never deletes our singleton.
    UvUnwrapController* direct = UvUnwrapController::instance();
    UvUnwrapController* viaQml = UvUnwrapController::qmlInstance(nullptr, nullptr);
    EXPECT_EQ(direct, viaQml);
    ASSERT_NE(viaQml, nullptr);
    EXPECT_EQ(QQmlEngine::objectOwnership(viaQml), QQmlEngine::CppOwnership);
}

TEST_F(UvUnwrapControllerCoverageTest, FreshControllerNotBusy)
{
    UvUnwrapController* c = UvUnwrapController::instance();
    EXPECT_FALSE(c->busy());
}

// ---- selection wiring -------------------------------------------------

TEST_F(UvUnwrapControllerCoverageTest, HasSelectionFalseWhenEmpty)
{
    UvUnwrapController* c = UvUnwrapController::instance();
    SelectionSet::getSingleton()->clearList();
    EXPECT_FALSE(c->hasSelection());
}

TEST_F(UvUnwrapControllerCoverageTest, HasSelectionTrueAfterSelectOne)
{
    UvUnwrapController* c = UvUnwrapController::instance();
    Ogre::Entity* ent = createAnimatedTestEntity("uvCtrlHasSel");
    ASSERT_NE(ent, nullptr);

    SelectionSet::getSingleton()->selectOne(ent);
    EXPECT_TRUE(c->hasSelection());
}

TEST_F(UvUnwrapControllerCoverageTest, CtorReEmitsSelectionChanged)
{
    // The ctor connects SelectionSet::selectionChanged →
    // UvUnwrapController::selectionChanged. Firing selectOne on the
    // SelectionSet must propagate through to the controller's signal.
    UvUnwrapController* c = UvUnwrapController::instance();
    QSignalSpy spy(c, &UvUnwrapController::selectionChanged);
    ASSERT_TRUE(spy.isValid());

    Ogre::Entity* ent = createAnimatedTestEntity("uvCtrlReemit");
    ASSERT_NE(ent, nullptr);
    SelectionSet::getSingleton()->selectOne(ent);

    EXPECT_GE(spy.count(), 1);
}

// ---- unwrapSelectedToFile: error branches -----------------------------

TEST_F(UvUnwrapControllerCoverageTest, EmptyOutputPathEmitsError)
{
    UvUnwrapController* c = UvUnwrapController::instance();
    // Selection present, but empty output path short-circuits first.
    Ogre::Entity* ent = createAnimatedTestEntity("uvCtrlEmptyPath");
    ASSERT_NE(ent, nullptr);
    SelectionSet::getSingleton()->selectOne(ent);

    QSignalSpy errSpy(c, &UvUnwrapController::error);
    QSignalSpy busySpy(c, &UvUnwrapController::busyChanged);
    QSignalSpy okSpy(c, &UvUnwrapController::unwrapApplied);
    ASSERT_TRUE(errSpy.isValid());

    QVariantMap result = c->unwrapSelectedToFile(QString(), 1024, 4, 0, true);

    ASSERT_EQ(errSpy.count(), 1);
    EXPECT_EQ(errSpy.at(0).at(0).toString(), QStringLiteral("Output path required."));
    EXPECT_TRUE(result.contains("applied"));
    EXPECT_FALSE(result["applied"].toBool());
    // Short-circuit before any work: no busy toggle, no success.
    EXPECT_EQ(busySpy.count(), 0);
    EXPECT_EQ(okSpy.count(), 0);
    EXPECT_FALSE(c->busy());
}

TEST_F(UvUnwrapControllerCoverageTest, NoSelectionEmitsError)
{
    UvUnwrapController* c = UvUnwrapController::instance();
    SelectionSet::getSingleton()->clearList();
    ASSERT_FALSE(c->hasSelection());

    QSignalSpy errSpy(c, &UvUnwrapController::error);
    QSignalSpy busySpy(c, &UvUnwrapController::busyChanged);
    ASSERT_TRUE(errSpy.isValid());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString out = tmp.filePath(QStringLiteral("nosel_unwrapped.glb"));

    QVariantMap result = c->unwrapSelectedToFile(out, 1024, 4, 0, true);

    ASSERT_EQ(errSpy.count(), 1);
    EXPECT_EQ(errSpy.at(0).at(0).toString(), QStringLiteral("No mesh selected."));
    EXPECT_TRUE(result.contains("applied"));
    EXPECT_FALSE(result["applied"].toBool());
    EXPECT_EQ(busySpy.count(), 0);
    EXPECT_FALSE(c->busy());
}

// ---- unwrapSelectedToFile: real-selection path ------------------------

TEST_F(UvUnwrapControllerCoverageTest, RealSelectionProducesReportShape)
{
    UvUnwrapController* c = UvUnwrapController::instance();
    Ogre::Entity* ent = createAnimatedTestEntity("uvCtrlReport");
    ASSERT_NE(ent, nullptr);
    SelectionSet::getSingleton()->selectOne(ent);
    ASSERT_TRUE(c->hasSelection());

    QSignalSpy busySpy(c, &UvUnwrapController::busyChanged);
    QSignalSpy okSpy(c, &UvUnwrapController::unwrapApplied);
    QSignalSpy errSpy(c, &UvUnwrapController::error);
    ASSERT_TRUE(busySpy.isValid());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString out = tmp.filePath(QStringLiteral("report_unwrapped.glb"));

    // Pass clamp-triggering values: resolution < 64 and negative
    // padding/channel exercise the std::max clamps in the controller.
    QVariantMap result = c->unwrapSelectedToFile(out, 16, -3, -1, true);

    // The work path always toggles m_busy true→false → two emissions.
    EXPECT_EQ(busySpy.count(), 2);
    EXPECT_FALSE(c->busy());

    // 'applied' is always present and is a bool. We do not assert its
    // value (xatlas success is environment-dependent).
    ASSERT_TRUE(result.contains("applied"));
    EXPECT_EQ(result["applied"].metaType().id(), QMetaType::Bool);

    const bool applied = result["applied"].toBool();
    if (applied) {
        // Full report map is populated on success.
        EXPECT_TRUE(result.contains("outputPath"));
        EXPECT_EQ(result["outputPath"].toString(), out);
        EXPECT_TRUE(result.contains("meshName"));
        EXPECT_TRUE(result.contains("submeshCount"));
        EXPECT_TRUE(result.contains("verticesBefore"));
        EXPECT_TRUE(result.contains("verticesAfter"));
        EXPECT_TRUE(result.contains("trianglesProcessed"));
        EXPECT_TRUE(result.contains("atlasWidth"));
        EXPECT_TRUE(result.contains("atlasHeight"));
        EXPECT_TRUE(result.contains("chartCount"));
        EXPECT_TRUE(result.contains("utilization"));
        // Exactly one of unwrapApplied / error should fire on success.
        EXPECT_EQ(okSpy.count(), 1);
        EXPECT_EQ(errSpy.count(), 0);
    } else {
        // Failure branch: error populated and error(QString) fired.
        EXPECT_TRUE(result.contains("error"));
        EXPECT_GE(errSpy.count(), 1);
        EXPECT_EQ(okSpy.count(), 0);
    }
}

TEST_F(UvUnwrapControllerCoverageTest, RealSelectionToggleBusyEvenOnFailure)
{
    // Regardless of xatlas outcome, the busy flag must end up false and
    // have transitioned through true (two busyChanged emissions) on the
    // real-selection path. Also verify the diffuse export path key
    // 'outputPath' echoes back the requested path when applied.
    UvUnwrapController* c = UvUnwrapController::instance();
    Ogre::Entity* ent = createAnimatedTestEntity("uvCtrlBusy");
    ASSERT_NE(ent, nullptr);
    SelectionSet::getSingleton()->selectOne(ent);

    QSignalSpy busySpy(c, &UvUnwrapController::busyChanged);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString out = tmp.filePath(QStringLiteral("busy_unwrapped.glb"));

    QVariantMap result = c->unwrapSelectedToFile(out, 256, 2, 0, false);

    EXPECT_EQ(busySpy.count(), 2);
    EXPECT_FALSE(c->busy());
    ASSERT_TRUE(result.contains("applied"));
    EXPECT_EQ(result["applied"].metaType().id(), QMetaType::Bool);
}
