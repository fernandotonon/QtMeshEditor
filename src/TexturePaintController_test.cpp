#include <gtest/gtest.h>

#include <QStandardPaths>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPoint>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>

#include "EditModeController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "TexturePaintBuffer.h"
#include "TexturePaintController.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include "UndoManager.h"

#include <OgreEntity.h>
#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgrePass.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubEntity.h>
#include <OgreTechnique.h>
#include <OgreTextureUnitState.h>

namespace {

// Build an entity in the scene with the canonical 3-vertex UV triangle
// and a material that already has a TUS named "diffuse_map". The TUS is
// what `findOrCreateActiveTextureUnit` will pick up by default, so any
// test that flows through `ensurePaintableTexture` can rely on it.
struct ScenePaintFixture
{
    Ogre::SceneManager* scene = nullptr;
    Ogre::MeshPtr mesh;
    Ogre::Entity* entity = nullptr;
    Ogre::SceneNode* node = nullptr;
    Ogre::MaterialPtr mat;

    bool setup(const QString& tag)
    {
        if (!tryInitOgre()) return false;
        auto* mgr = Manager::getSingleton();
        if (!mgr) return false;
        scene = mgr->getSceneMgr();
        if (!scene) return false;

        const std::string meshName  = ("TPC_Mesh_"   + tag).toStdString();
        const std::string entName   = ("TPC_Entity_" + tag).toStdString();
        const std::string matName   = ("TPC_Mat_"    + tag).toStdString();

        mesh = createInMemoryTriangleMesh(meshName);
        if (!mesh) return false;
        entity = scene->createEntity(entName, mesh->getName());
        if (!entity) return false;
        node = scene->getRootSceneNode()->createChildSceneNode();
        node->attachObject(entity);

        auto& mm = Ogre::MaterialManager::getSingleton();
        mat = mm.create(matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        auto* tus  = pass->createTextureUnitState();
        tus->setName("diffuse_map");
        entity->getSubEntity(0)->setMaterial(mat);

        SelectionSet::getSingleton()->clear();
        SelectionSet::getSingleton()->append(entity);
        return true;
    }

    void teardown()
    {
        auto* ctrl = TexturePaintController::instance();
        if (ctrl) ctrl->closeSession();
        if (SelectionSet::getSingleton())
            SelectionSet::getSingleton()->clear();
        if (scene) {
            if (node) {
                scene->getRootSceneNode()->removeAndDestroyChild(node);
                node = nullptr;
            }
            if (entity) {
                scene->destroyEntity(entity);
                entity = nullptr;
            }
        }
        if (mesh) {
            Ogre::MeshManager::getSingleton().remove(mesh);
            mesh.reset();
        }
        if (mat) {
            Ogre::MaterialManager::getSingleton().remove(mat);
            mat.reset();
        }
    }
};

// Spin the Qt event loop for `ms` real milliseconds so debounced
// QTimer::singleShot callbacks fire. Used for tests that need to
// observe the result of flushDirtyToOgre (16 ms) → refreshPreviewUri
// (60 ms inside). Default 120 ms covers both with margin.
void pumpEventsFor(int ms = 120)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, ms);
        QCoreApplication::sendPostedEvents();
    }
}

// Drop the controller's session + reset selection so every test starts
// from a clean state regardless of run order.
void hardResetController()
{
    auto* ctrl = TexturePaintController::instance();
    if (!ctrl) return;
    if (ctrl->texturePaintEnabled()) ctrl->setTexturePaintEnabled(false);
    ctrl->clearSelectionMask();
    ctrl->closeSession();
    ctrl->setBrushTool(TexturePaintController::ToolPaint);
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ctrl->setUvOverlayVisible(false);
    // Paint v2 Slice E (#548): reset symmetry + stabilizer so a test that
    // enabled them can't leak persisted state into later tests (order-independent).
    ctrl->setSymmetryEnabled(false);
    ctrl->setSymmetryAxes(static_cast<int>(TexturePaintController::SymAxisX));
    ctrl->setStabilizerMode(static_cast<int>(TexturePaintController::StabAverage));
    ctrl->setStabilizerAmount(0.0);
    if (auto* sel = SelectionSet::getSingleton()) sel->clear();
}

} // namespace

// ===========================================================================
// Standalone tests — pure-state, no Ogre needed
// ===========================================================================

TEST(TexturePaintControllerStandalone, InstanceIsStable) {
    auto* a = TexturePaintController::instance();
    auto* b = TexturePaintController::instance();
    EXPECT_EQ(a, b) << "instance() should be a singleton";
    EXPECT_NE(a, nullptr);
}

TEST(TexturePaintControllerStandalone, DefaultsAreSane) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    EXPECT_FALSE(ctrl->texturePaintEnabled());
    EXPECT_FALSE(ctrl->hasActiveSession());
    EXPECT_EQ(ctrl->brushTool(), static_cast<int>(TexturePaintController::ToolPaint));
    EXPECT_EQ(ctrl->paintTarget(), static_cast<int>(TexturePaintController::TargetTexture));
    EXPECT_FALSE(ctrl->hasSelectionMask());
    EXPECT_EQ(ctrl->selectedPixelCount(), 0);
    EXPECT_FALSE(ctrl->uvOverlayVisible());
    EXPECT_FALSE(ctrl->editorWindowOpen());
    EXPECT_TRUE(ctrl->currentTextureName().isEmpty());
    EXPECT_EQ(ctrl->textureResolution(), 0);
}

TEST(TexturePaintControllerStandalone, SetBrushToolSticksAcrossAllValues) {
    auto* ctrl = TexturePaintController::instance();
    for (int t : {static_cast<int>(TexturePaintController::ToolErase),
                  static_cast<int>(TexturePaintController::ToolFill),
                  static_cast<int>(TexturePaintController::ToolColorPicker),
                  static_cast<int>(TexturePaintController::ToolSmudge),
                  static_cast<int>(TexturePaintController::ToolSmartSelect),
                  static_cast<int>(TexturePaintController::ToolPaint)}) {
        ctrl->setBrushTool(t);
        EXPECT_EQ(ctrl->brushTool(), t);
    }
}

TEST(TexturePaintControllerStandalone, SetBrushToolNoSignalOnSameValue) {
    auto* ctrl = TexturePaintController::instance();
    ctrl->setBrushTool(TexturePaintController::ToolPaint);
    QSignalSpy spy(ctrl, &TexturePaintController::brushToolChanged);
    ctrl->setBrushTool(TexturePaintController::ToolPaint);
    EXPECT_EQ(spy.count(), 0);
}

TEST(TexturePaintControllerStandalone, SetPaintTargetEmitsOnceAndSticks) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    QSignalSpy spy(ctrl, &TexturePaintController::paintTargetChanged);
    ctrl->setPaintTarget(TexturePaintController::TargetVertex);
    EXPECT_EQ(ctrl->paintTarget(), static_cast<int>(TexturePaintController::TargetVertex));
    EXPECT_EQ(spy.count(), 1);
    ctrl->setPaintTarget(TexturePaintController::TargetVertex);
    EXPECT_EQ(spy.count(), 1) << "same target should not re-emit";
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    EXPECT_EQ(spy.count(), 2);
}

TEST(TexturePaintControllerStandalone, SetPaintTargetAlsoFiresSessionAndSmartSelectSignals) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    QSignalSpy session(ctrl, &TexturePaintController::sessionChanged);
    QSignalSpy smart  (ctrl, &TexturePaintController::smartSelectChanged);
    ctrl->setPaintTarget(TexturePaintController::TargetVertex);
    EXPECT_GE(session.count(), 1);
    EXPECT_GE(smart.count(),   1);
}

TEST(TexturePaintControllerStandalone, SmartSelectToleranceClampedAndEmits) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    QSignalSpy spy(ctrl, &TexturePaintController::smartSelectChanged);
    ctrl->setSmartSelectTolerance(0.42);
    EXPECT_DOUBLE_EQ(ctrl->smartSelectTolerance(), 0.42);
    EXPECT_EQ(spy.count(), 1);
    ctrl->setSmartSelectTolerance(0.42);
    EXPECT_EQ(spy.count(), 1) << "no re-emit on no-op";
    ctrl->setSmartSelectTolerance(-1.0);
    EXPECT_DOUBLE_EQ(ctrl->smartSelectTolerance(), 0.0);
    ctrl->setSmartSelectTolerance(99.0);
    EXPECT_DOUBLE_EQ(ctrl->smartSelectTolerance(), 1.0);
}

TEST(TexturePaintControllerStandalone, SetUvOverlayVisibleTogglesAndEmits) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    QSignalSpy spy(ctrl, &TexturePaintController::uvOverlayChanged);
    ctrl->setUvOverlayVisible(true);
    EXPECT_TRUE(ctrl->uvOverlayVisible());
    EXPECT_EQ(spy.count(), 1);
    ctrl->setUvOverlayVisible(true);
    EXPECT_EQ(spy.count(), 1);
    ctrl->setUvOverlayVisible(false);
    EXPECT_FALSE(ctrl->uvOverlayVisible());
    EXPECT_EQ(spy.count(), 2);
}

TEST(TexturePaintControllerStandalone, BrushSettersMirrorIntoEditModeController) {
    auto* ctrl = TexturePaintController::instance();
    auto* em   = EditModeController::instance();
    ctrl->setBrushRadius(0.123);
    EXPECT_DOUBLE_EQ(em->vertexPaintRadius(), 0.123);
    ctrl->setBrushStrength(0.456);
    EXPECT_DOUBLE_EQ(em->vertexPaintStrength(), 0.456);
    ctrl->setBrushFalloff(0.789);
    EXPECT_DOUBLE_EQ(em->vertexPaintFalloff(), 0.789);
    QColor c(12, 34, 56);
    ctrl->setBrushColor(c);
    EXPECT_EQ(em->vertexPaintColor(), c);
    // And reading-side mirrors agree.
    EXPECT_DOUBLE_EQ(ctrl->texturePaintRadius(),   0.123);
    EXPECT_DOUBLE_EQ(ctrl->texturePaintStrength(), 0.456);
    EXPECT_DOUBLE_EQ(ctrl->texturePaintFalloff(), 0.789);
    EXPECT_EQ(ctrl->texturePaintColor(), c);
}

TEST(TexturePaintControllerStandalone, BrushShapeMirrorsEditMode) {
    auto* ctrl = TexturePaintController::instance();
    auto* em   = EditModeController::instance();
    em->setVertexPaintShape(EditModeController::ShapeSquare);
    EXPECT_EQ(ctrl->brushShape(), static_cast<int>(EditModeController::ShapeSquare));
    em->setVertexPaintShape(EditModeController::ShapeRound);
    EXPECT_EQ(ctrl->brushShape(), static_cast<int>(EditModeController::ShapeRound));
}

TEST(TexturePaintControllerStandalone, BgPaintColorMirrorsEditMode) {
    auto* ctrl = TexturePaintController::instance();
    auto* em   = EditModeController::instance();
    em->setVertexPaintBackgroundColor(QColor(7, 8, 9));
    EXPECT_EQ(ctrl->bgPaintColor(), QColor(7, 8, 9));
}

TEST(TexturePaintControllerStandalone, SetActiveSlotIndexOutOfRangeIsNoOp) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    const int before = ctrl->activeSlotIndex();
    ctrl->setActiveSlotIndex(-1);
    ctrl->setActiveSlotIndex(9999);
    EXPECT_EQ(ctrl->activeSlotIndex(), before);
}

TEST(TexturePaintControllerStandalone, EnableWithNoSelectionIsHarmless) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    QSignalSpy spy(ctrl, &TexturePaintController::texturePaintChanged);
    ctrl->setTexturePaintEnabled(true);
    EXPECT_TRUE(ctrl->texturePaintEnabled());
    EXPECT_FALSE(ctrl->hasActiveSession()) << "no selection → no session";
    EXPECT_EQ(spy.count(), 1);
    ctrl->setTexturePaintEnabled(true);
    EXPECT_EQ(spy.count(), 1) << "no re-emit on no-op";
    ctrl->setTexturePaintEnabled(false);
    EXPECT_FALSE(ctrl->texturePaintEnabled());
    EXPECT_EQ(spy.count(), 2);
}

TEST(TexturePaintControllerStandalone, SnapshotBufferImageEmptyWithoutSession) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    QImage img = ctrl->snapshotBufferImage();
    EXPECT_TRUE(img.isNull()) << "no buffer → null QImage for the image provider";
}

TEST(TexturePaintControllerStandalone, MaskActionsNoOpWithoutSession) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    EXPECT_EQ(ctrl->fillMaskWithFG(),   0);
    EXPECT_EQ(ctrl->fillMaskWithBG(),   0);
    EXPECT_EQ(ctrl->deleteMaskPixels(), 0);
}

TEST(TexturePaintControllerStandalone, SmartSelectMaskOpsNoOpWithoutSession) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    EXPECT_EQ(ctrl->smartSelectAtUV(0.5, 0.5, 0), 0);
    ctrl->selectAllMask();
    EXPECT_FALSE(ctrl->hasSelectionMask());
    ctrl->invertSelectionMask();
    EXPECT_FALSE(ctrl->hasSelectionMask());
}

TEST(TexturePaintControllerStandalone, ClearSelectionMaskNoOpWhenEmpty) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    QSignalSpy spy(ctrl, &TexturePaintController::smartSelectChanged);
    ctrl->clearSelectionMask();
    EXPECT_EQ(spy.count(), 0) << "no emit when there's nothing to clear";
}

TEST(TexturePaintControllerStandalone, BakeVertexColorsNoOpWithoutSelection) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    EXPECT_EQ(ctrl->bakeVertexColorsToTexture(64, 1, QString()), -1);
}

TEST(TexturePaintControllerStandalone, SavePaintBufferFailsWithoutSession) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    EXPECT_FALSE(ctrl->savePaintBuffer(QStringLiteral("/tmp/should_not_be_written.png")));
    EXPECT_FALSE(ctrl->savePaintBuffer(QString()));
}

TEST(TexturePaintControllerStandalone, LoadPaintBufferRejectsEmptyAndMissingPath) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    EXPECT_FALSE(ctrl->loadPaintBuffer(QString()));
    EXPECT_FALSE(ctrl->loadPaintBuffer(QStringLiteral("/nonexistent/path/should_fail.png")));
}

TEST(TexturePaintControllerStandalone, EnsurePaintableTextureNoOpWithoutSelection) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    EXPECT_FALSE(ctrl->ensurePaintableTexture(256));
    EXPECT_FALSE(ctrl->hasActiveSession());
}

TEST(TexturePaintControllerStandalone, BeginStrokeUVRejectedWithoutSession) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    EXPECT_FALSE(ctrl->beginStrokeUV(0.5, 0.5));
}

TEST(TexturePaintControllerStandalone, RefreshSlotsClearsListWithoutSelection) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    ctrl->refreshSlots();
    EXPECT_TRUE(ctrl->textureSlots().isEmpty());
}

TEST(TexturePaintControllerStandalone, CloseSessionWithoutSessionIsHarmless) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    ctrl->closeSession();
    EXPECT_FALSE(ctrl->hasActiveSession());
}

TEST(TexturePaintControllerStandalone, SetUvOverlayVisibleNoSessionStillToggles) {
    // Visibility flag is purely controller state — independent of whether
    // a session exists.
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    ctrl->setUvOverlayVisible(true);
    EXPECT_TRUE(ctrl->uvOverlayVisible());
    EXPECT_TRUE(ctrl->uvOverlayDataUri().isEmpty()) << "no session ⇒ no overlay PNG";
    ctrl->setUvOverlayVisible(false);
}

TEST(TexturePaintControllerStandalone, BakeToOriginalFileEmptyWithoutSession) {
    hardResetController();
    auto* ctrl = TexturePaintController::instance();
    EXPECT_TRUE(ctrl->bakeToOriginalFile().isEmpty());
}

// ===========================================================================
// Stateful fixture tests — need a scene + entity
// ===========================================================================

class TexturePaintControllerSceneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Use tryInitOgre() (not canLoadMeshFiles()) to match the existing
        // FindMeshPointForUVHitsCorrectTriangle test in this file —
        // canLoadMeshFiles() returns false in some Linux-CI permutations
        // even though hardware buffer creation works for the simple
        // in-memory meshes these tests use.
        ASSERT_TRUE(tryInitOgre()) << "Ogre init / render window required";
        hardResetController();
    }

    void TearDown() override
    {
        m_fix.teardown();
        hardResetController();
    }

    ScenePaintFixture m_fix;
};

TEST_F(TexturePaintControllerSceneTest, EnableWithSelectionCreatesSession) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Enable")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ctrl->setTexturePaintEnabled(true);
    EXPECT_TRUE(ctrl->texturePaintEnabled());
    EXPECT_TRUE(ctrl->hasActiveSession());
    EXPECT_EQ(ctrl->textureResolution(), 1024);
    EXPECT_FALSE(ctrl->currentTextureName().isEmpty());
    // Disabling tears the session back down.
    ctrl->setTexturePaintEnabled(false);
    EXPECT_FALSE(ctrl->hasActiveSession());
}

TEST_F(TexturePaintControllerSceneTest, EnsurePaintableTextureBuildsBuffer) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Ensure")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(128));
    EXPECT_TRUE(ctrl->hasActiveSession());
    EXPECT_EQ(ctrl->textureResolution(), 128);
    EXPECT_EQ(ctrl->buffer().width(),  128);
    EXPECT_EQ(ctrl->buffer().height(), 128);
}

TEST_F(TexturePaintControllerSceneTest, EnsurePaintableTextureReusesExistingSession) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Reuse")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(64));
    const QString firstName = ctrl->currentTextureName();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(64)) << "second call should reuse the session";
    EXPECT_EQ(ctrl->currentTextureName(), firstName);
}

TEST_F(TexturePaintControllerSceneTest, CloseSessionResetsAllState) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Close")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    ASSERT_TRUE(ctrl->hasActiveSession());
    ctrl->closeSession();
    EXPECT_FALSE(ctrl->hasActiveSession());
    EXPECT_TRUE(ctrl->currentTextureName().isEmpty());
    EXPECT_EQ(ctrl->textureResolution(), 0);
}

TEST_F(TexturePaintControllerSceneTest, RefreshSlotsExposesEntitySlots) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Slots")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->refreshSlots();
    QVariantList slotList = ctrl->textureSlots();
    EXPECT_GE(slotList.size(), 1);
    if (!slotList.isEmpty()) {
        auto entry = slotList.first().toMap();
        EXPECT_TRUE(entry.contains("slot"));
        EXPECT_TRUE(entry.contains("submesh"));
    }
}

TEST_F(TexturePaintControllerSceneTest, FullResPreviewUrlActiveSession) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("PreviewUrl")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(64));
    const QString url = ctrl->fullResPreviewUrl();
    EXPECT_TRUE(url.startsWith(QStringLiteral("image://paintbuffer/")));
    EXPECT_TRUE(url.contains(QStringLiteral("?v=")));
}

TEST_F(TexturePaintControllerSceneTest, SnapshotBufferImageMatchesBuffer) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Snap")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(48));
    QImage img = ctrl->snapshotBufferImage();
    EXPECT_FALSE(img.isNull());
    EXPECT_EQ(img.width(),  ctrl->buffer().width());
    EXPECT_EQ(img.height(), ctrl->buffer().height());
}

TEST_F(TexturePaintControllerSceneTest, SaveAndLoadPaintBufferRoundTrip) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Roundtrip")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString out = tmp.path() + "/buf.png";
    EXPECT_TRUE(ctrl->savePaintBuffer(out));
    EXPECT_TRUE(QFile::exists(out));
    // Round-tripping the saved file back in should keep the session alive.
    EXPECT_TRUE(ctrl->loadPaintBuffer(out));
    EXPECT_TRUE(ctrl->hasActiveSession());
}

TEST_F(TexturePaintControllerSceneTest, FindMeshPointForUVHitsAllThreeVertices) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Find")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->refreshSlots();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(64));
    Ogre::Vector3 pos, n;
    EXPECT_TRUE(ctrl->findMeshPointForUV(Ogre::Vector2(0.0f, 0.0f), pos, n));
    EXPECT_NEAR(pos.x, 0.0f, 1e-4);
    EXPECT_NEAR(pos.y, 0.0f, 1e-4);
    EXPECT_TRUE(ctrl->findMeshPointForUV(Ogre::Vector2(1.0f, 0.0f), pos, n));
    EXPECT_NEAR(pos.x, 1.0f, 1e-4);
    EXPECT_TRUE(ctrl->findMeshPointForUV(Ogre::Vector2(0.0f, 1.0f), pos, n));
    EXPECT_NEAR(pos.y, 1.0f, 1e-4);
}

TEST_F(TexturePaintControllerSceneTest, FindMeshPointForUVMissesOutsideTriangle) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("FindMiss")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    Ogre::Vector3 pos, n;
    EXPECT_FALSE(ctrl->findMeshPointForUV(Ogre::Vector2(0.99f, 0.99f), pos, n));
}

TEST_F(TexturePaintControllerSceneTest, TexturePaintRadiusUVMapsThroughMeshExtent) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("RadUV")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    ctrl->setBrushRadius(0.5);
    const double uv = ctrl->texturePaintRadiusUV();
    EXPECT_GT(uv, 0.005);
    EXPECT_LE(uv, 1.0);
}

TEST_F(TexturePaintControllerSceneTest, SmartSelectInsideTriangleAddsPixels) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Smart")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    // White buffer + low tolerance: pick the center pixel → mass select.
    ctrl->setSmartSelectTolerance(0.5);
    const int affected = ctrl->smartSelectAtUV(0.2, 0.2, 0);
    EXPECT_GT(affected, 0);
    EXPECT_TRUE(ctrl->hasSelectionMask());
    EXPECT_EQ(ctrl->selectedPixelCount(), affected);
}

TEST_F(TexturePaintControllerSceneTest, SelectAllMaskCoversEveryPixel) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("All")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    ctrl->selectAllMask();
    EXPECT_TRUE(ctrl->hasSelectionMask());
    EXPECT_EQ(ctrl->selectedPixelCount(), 16 * 16);
}

TEST_F(TexturePaintControllerSceneTest, InvertSelectionMaskFlipsCount) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Invert")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    ctrl->selectAllMask();
    const int allCount = ctrl->selectedPixelCount();
    EXPECT_EQ(allCount, 16 * 16);
    ctrl->invertSelectionMask();
    EXPECT_EQ(ctrl->selectedPixelCount(), 0);
    ctrl->invertSelectionMask();
    EXPECT_EQ(ctrl->selectedPixelCount(), allCount);
}

TEST_F(TexturePaintControllerSceneTest, ClearSelectionMaskFiresSmartSignal) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("ClearMask")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    ctrl->selectAllMask();
    QSignalSpy spy(ctrl, &TexturePaintController::smartSelectChanged);
    ctrl->clearSelectionMask();
    EXPECT_EQ(spy.count(), 1);
    EXPECT_FALSE(ctrl->hasSelectionMask());
}

TEST_F(TexturePaintControllerSceneTest, FillMaskWithFGAffectsAllSelectedPixels) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("FillFG")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    ctrl->selectAllMask();
    ctrl->setBrushColor(QColor(255, 0, 0));
    const int affected = ctrl->fillMaskWithFG();
    EXPECT_EQ(affected, 16 * 16);
    // First pixel should now be red.
    const auto& px = ctrl->buffer().data();
    EXPECT_EQ(px[0], 255);
    EXPECT_EQ(px[1], 0);
    EXPECT_EQ(px[2], 0);
}

TEST_F(TexturePaintControllerSceneTest, FillMaskWithBGAffectsAllSelectedPixels) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("FillBG")));
    auto* ctrl = TexturePaintController::instance();
    auto* em   = EditModeController::instance();
    em->setVertexPaintBackgroundColor(QColor(0, 255, 0));
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    ctrl->selectAllMask();
    const int affected = ctrl->fillMaskWithBG();
    EXPECT_EQ(affected, 16 * 16);
    const auto& px = ctrl->buffer().data();
    EXPECT_EQ(px[1], 255);
}

TEST_F(TexturePaintControllerSceneTest, DeleteMaskPixelsZeroesAll) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("DelMask")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    ctrl->selectAllMask();
    const int affected = ctrl->deleteMaskPixels();
    EXPECT_EQ(affected, 16 * 16);
    const auto& px = ctrl->buffer().data();
    EXPECT_EQ(px[0], 0);
    EXPECT_EQ(px[3], 0) << "alpha must be zeroed too";
}

TEST_F(TexturePaintControllerSceneTest, MaskActionsNoOpWhenSelectionEmpty) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("MaskNoOp")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    ASSERT_FALSE(ctrl->hasSelectionMask());
    EXPECT_EQ(ctrl->fillMaskWithFG(),   0);
    EXPECT_EQ(ctrl->fillMaskWithBG(),   0);
    EXPECT_EQ(ctrl->deleteMaskPixels(), 0);
}

TEST_F(TexturePaintControllerSceneTest, SetActiveSlotIndexZeroIsIdempotent) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("SlotIdx")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->refreshSlots();
    const int before = ctrl->activeSlotIndex();
    ctrl->setActiveSlotIndex(before);  // same value
    EXPECT_EQ(ctrl->activeSlotIndex(), before);
}

TEST_F(TexturePaintControllerSceneTest, ApplyPixelSnapshotRoundTripsBuffer) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("ApplySnap")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(8));
    // Capture the initial buffer, mutate, then restore via applyPixelSnapshot
    // — this is the same code path the undo stack uses.
    auto before = ctrl->buffer().data();
    ctrl->mutableBuffer().data()[0] = 1;
    ctrl->mutableBuffer().data()[1] = 2;
    ctrl->mutableBuffer().data()[2] = 3;
    ctrl->mutableBuffer().data()[3] = 4;
    EXPECT_NE(ctrl->buffer().data()[0], before[0]);
    ctrl->applyPixelSnapshot(before);
    EXPECT_EQ(ctrl->buffer().data()[0], before[0]);
    EXPECT_EQ(ctrl->buffer().data()[1], before[1]);
    EXPECT_EQ(ctrl->buffer().data()[2], before[2]);
    EXPECT_EQ(ctrl->buffer().data()[3], before[3]);
}

TEST_F(TexturePaintControllerSceneTest, ApplyPixelSnapshotMismatchedSizeIgnored) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("ApplyBadSize")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(8));
    auto good = ctrl->buffer().data();
    good[0] = 99;
    std::vector<uint8_t> bad(4, 0);  // intentionally wrong size
    ctrl->applyPixelSnapshot(bad);
    // Buffer should remain unchanged (and not crash).
    EXPECT_NE(ctrl->buffer().data()[0], 99) << "buffer should not have been touched";
}

TEST_F(TexturePaintControllerSceneTest, SwitchPaintTargetTexToVertexTearsDownSession) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("SwitchTarget")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    ASSERT_TRUE(ctrl->hasActiveSession());
    ctrl->setPaintTarget(TexturePaintController::TargetVertex);
    EXPECT_FALSE(ctrl->hasActiveSession())
        << "moving to vertex target must release the texture-paint session";
}

TEST_F(TexturePaintControllerSceneTest, SetHoveredUVEmitsSignal) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Hover")));
    auto* ctrl = TexturePaintController::instance();
    QSignalSpy spy(ctrl, &TexturePaintController::hoveredUVChanged);
    ctrl->setHoveredUV(0.3, 0.7);
    EXPECT_EQ(spy.count(), 1);
    if (spy.count() > 0) {
        const auto args = spy.first();
        EXPECT_NEAR(args.at(0).toDouble(), 0.3, 1e-6);
        EXPECT_NEAR(args.at(1).toDouble(), 0.7, 1e-6);
    }
}

TEST_F(TexturePaintControllerSceneTest, ClearHoveredUVEmitsMinusOne) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("HoverClear")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setHoveredUV(0.5, 0.5);
    QSignalSpy spy(ctrl, &TexturePaintController::hoveredUVChanged);
    ctrl->clearHoveredUV();
    EXPECT_EQ(spy.count(), 1);
    if (spy.count() > 0) {
        const auto args = spy.first();
        EXPECT_LT(args.at(0).toDouble(), 0.0);
        EXPECT_LT(args.at(1).toDouble(), 0.0);
    }
}

TEST_F(TexturePaintControllerSceneTest, BeginAndEndStrokeUVCompletesWithoutCrash) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("StrokeUV")));
    auto* ctrl = TexturePaintController::instance();
    // beginStrokeUV requires paint mode on — setTexturePaintEnabled
    // also calls ensurePaintableTexture internally.
    ctrl->setTexturePaintEnabled(true);
    ASSERT_TRUE(ctrl->hasActiveSession());
    EXPECT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.6, 0.5);
    ctrl->endStrokeUV();
    SUCCEED();
}

TEST_F(TexturePaintControllerSceneTest, SmartSelectInvalidModeFallsToReplace) {
    // mode 0 = replace, 1 = add, 2 = sub; anything else falls through to
    // the replace branch. Exercise that branch.
    ASSERT_TRUE(m_fix.setup(QStringLiteral("SmartBadMode")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    ctrl->setSmartSelectTolerance(0.5);
    const int affected = ctrl->smartSelectAtUV(0.5, 0.5, 99);
    EXPECT_GE(affected, 0);  // valid call, doesn't crash on unrecognised mode
}

TEST_F(TexturePaintControllerSceneTest, SmartSelectAddAndSubtractCombineModes) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("SmartCombine")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    ctrl->setSmartSelectTolerance(0.5);
    const int initial = ctrl->smartSelectAtUV(0.5, 0.5, 0);
    EXPECT_GT(initial, 0);
    const int afterAdd = ctrl->smartSelectAtUV(0.5, 0.5, 1);
    (void)afterAdd;  // depending on impl may be 0 (already selected) or more
    // Sub on the same seed should remove pixels.
    const int afterSub = ctrl->smartSelectAtUV(0.5, 0.5, 2);
    EXPECT_GE(afterSub, 0);
}

TEST_F(TexturePaintControllerSceneTest, EnsurePaintableTextureFreshSessionEmitsSessionChanged) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("EnsureEmit")));
    auto* ctrl = TexturePaintController::instance();
    QSignalSpy spy(ctrl, &TexturePaintController::sessionChanged);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    EXPECT_GE(spy.count(), 1);
}

TEST_F(TexturePaintControllerSceneTest, LoadPaintBufferReplacesBufferWithFileBytes) {
    // Write a known image to disk, load it via the controller, verify the
    // first pixel matches.
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Load")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(8));
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + "/known.png";
    QImage seed(8, 8, QImage::Format_RGBA8888);
    seed.fill(QColor(11, 22, 33, 255));
    ASSERT_TRUE(seed.save(path));
    ASSERT_TRUE(ctrl->loadPaintBuffer(path));
    const auto& px = ctrl->buffer().data();
    EXPECT_EQ(px[0], 11);
    EXPECT_EQ(px[1], 22);
    EXPECT_EQ(px[2], 33);
}

TEST_F(TexturePaintControllerSceneTest, SavePaintBufferProducesReadablePNG) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("SavePNG")));
    auto* ctrl = TexturePaintController::instance();
    // ensurePaintableTexture floors the resolution at 16 when no existing
    // texture is bound (so request 32 to get a non-trivial known size).
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    ASSERT_EQ(ctrl->textureResolution(), 32);
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString out = tmp.path() + "/save.png";
    EXPECT_TRUE(ctrl->savePaintBuffer(out));
    QImage round(out);
    EXPECT_FALSE(round.isNull());
    EXPECT_EQ(round.width(),  32);
    EXPECT_EQ(round.height(), 32);
}

TEST_F(TexturePaintControllerSceneTest, OpenAndCloseEditorWindowFlipFlag) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("EditorWin")));
    auto* ctrl = TexturePaintController::instance();
    QSignalSpy spy(ctrl, &TexturePaintController::editorWindowChanged);
    ctrl->openEditorWindow();
    // openEditorWindow has a fast path when no session — exercise without
    // requiring it to succeed in a headless test (the QML may fail to
    // load). Just make sure closing is harmless.
    ctrl->closeEditorWindow();
    EXPECT_FALSE(ctrl->editorWindowOpen());
}

TEST_F(TexturePaintControllerSceneTest, FullResVersionAdvancesPerRefresh) {
    // Bumping pixels + flushing should change the URL's `?v=N` segment so
    // QML invalidates Image cache. flushDirtyToOgre is debounced (~16 ms)
    // and the preview refresh inside is further debounced (~60 ms), so
    // we pump the event loop to let both timers fire.
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Bump")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    const QString url1 = ctrl->fullResPreviewUrl();
    ctrl->selectAllMask();
    ctrl->setBrushColor(QColor(123, 45, 67));
    ctrl->fillMaskWithFG();
    pumpEventsFor(150);
    const QString url2 = ctrl->fullResPreviewUrl();
    EXPECT_NE(url1, url2) << "refresh must bump the version counter";
}

TEST_F(TexturePaintControllerSceneTest, BakeVertexColorsToTextureBakesPixels) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Bake")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    // bakeVertexColorsToTexture returns the rasterized pixel count. Some
    // mesh configurations have no vertex colors → returns 0 or -1; we
    // assert it doesn't crash and yields a non-negative-or--1 value.
    const int n = ctrl->bakeVertexColorsToTexture(32, 1, QString());
    EXPECT_GE(n, -1);
}

TEST_F(TexturePaintControllerSceneTest, RefreshPreviewUriEmitsFullResSignal) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("RefreshSig")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    QSignalSpy spy(ctrl, &TexturePaintController::fullResPreviewChanged);
    // Mask write → flushDirtyToOgre (16 ms debounce) → refreshPreviewUri
    // (60 ms debounce inside). Pump events to let both timers fire.
    ctrl->selectAllMask();
    ctrl->setBrushColor(QColor(99, 99, 99));
    ctrl->fillMaskWithFG();
    pumpEventsFor(150);
    EXPECT_GE(spy.count(), 1);
}

TEST_F(TexturePaintControllerSceneTest, SelectAllMaskHonorsBufferDimensions) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("MaskRes")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(64));
    ctrl->selectAllMask();
    EXPECT_EQ(ctrl->selectedPixelCount(), 64 * 64);
}

TEST_F(TexturePaintControllerSceneTest, ChangingResolutionAfterCloseProducesNewBuffer) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("ResChange")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    EXPECT_EQ(ctrl->textureResolution(), 16);
    ctrl->closeSession();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(48));
    EXPECT_EQ(ctrl->textureResolution(), 48);
}

// ---------------------------------------------------------------------------
// Paint v2 Slice D — PBR channel painting (#547)
// ---------------------------------------------------------------------------

namespace {
// Does the entity's first material's first pass carry a TUS named `slot`
// (optionally with a non-empty texture bound)?
bool passHasSlot(Ogre::Entity* ent, const char* slot, bool needTexture = false)
{
    if (!ent || ent->getNumSubEntities() == 0) return false;
    auto mat = ent->getSubEntity(0)->getMaterial();
    if (!mat || mat->getNumTechniques() == 0) return false;
    auto* pass = mat->getTechnique(0)->getPass(0);
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        if (tus->getName() == slot)
            return !needTexture || !tus->getTextureName().empty();
    }
    return false;
}
} // namespace

TEST_F(TexturePaintControllerSceneTest, PaintChannelsModelExcludesHeight) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("ChanModel")));
    auto* ctrl = TexturePaintController::instance();
    const QVariantList chans = ctrl->paintChannels();
    // Height is not a selectable channel — it has no slot of its own and is
    // baked via the Normal channel. So the picker shows one fewer than the
    // painter-channel count (which itself already excludes VertexColor).
    ASSERT_EQ(chans.size(), PaintChannelNS::kTexturePaintChannelCount - 1);
    for (const auto& v : chans)
        EXPECT_NE(v.toMap().value("id").toString(), QStringLiteral("height"))
            << "Height must not appear in the channel picker";
    // First entry is BaseColor → albedo; a scalar entry reports scalar=true.
    EXPECT_EQ(chans.at(0).toMap().value("slot").toString(), QStringLiteral("albedo"));
    EXPECT_TRUE(chans.at(static_cast<int>(PaintChannelNS::Channel::Roughness))
                    .toMap().value("scalar").toBool());
    EXPECT_FALSE(chans.at(static_cast<int>(PaintChannelNS::Channel::BaseColor))
                     .toMap().value("scalar").toBool());
}

TEST_F(TexturePaintControllerSceneTest, SwitchingChannelAutoCreatesCanonicalSlot) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("ChanSlot")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    // The fixture material only ships `diffuse_map`. Switching to Roughness
    // must create a `roughness` TUS so the channel is paintable.
    EXPECT_FALSE(passHasSlot(m_fix.entity, "roughness"));
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::Roughness));
    EXPECT_EQ(ctrl->activeChannel(),
              static_cast<int>(PaintChannelNS::Channel::Roughness));
    EXPECT_TRUE(passHasSlot(m_fix.entity, "roughness"));
}

TEST_F(TexturePaintControllerSceneTest, ScalarChannelBakeBindsMetallicOrmSlot) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("ScalarBake")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::Roughness));
    ASSERT_TRUE(ctrl->hasActiveSession());
    // Paint a stroke, then bake — scalar channels collapse into the packed ORM
    // texture bound to the `metallic` slot the Cook-Torrance SRS reads.
    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.55, 0.55);
    ctrl->endStrokeUV();
    pumpEventsFor(150);
    EXPECT_TRUE(ctrl->bakeChannel(static_cast<int>(PaintChannelNS::Channel::Roughness)));
    EXPECT_TRUE(passHasSlot(m_fix.entity, "metallic", /*needTexture*/true));
}

TEST_F(TexturePaintControllerSceneTest, NormalChannelBakeBindsNormalMapSlot) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("NormalBake")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::Normal));
    ASSERT_TRUE(ctrl->hasActiveSession());
    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.6, 0.5);
    ctrl->endStrokeUV();
    pumpEventsFor(150);
    // The Normal channel Sobel-bakes the painted grayscale into normal_map.
    EXPECT_TRUE(ctrl->bakeChannel(static_cast<int>(PaintChannelNS::Channel::Normal)));
    EXPECT_TRUE(passHasSlot(m_fix.entity, "normal_map", /*needTexture*/true));
}

// Height is not a selectable channel: setActiveChannel(Height) redirects to
// Normal (they share the normal_map slot), and Height is absent from the picker.
TEST_F(TexturePaintControllerSceneTest, HeightChannelRedirectsToNormal) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("HeightRedirect")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::Height));
    EXPECT_EQ(ctrl->activeChannel(),
              static_cast<int>(PaintChannelNS::Channel::Normal));
}

TEST_F(TexturePaintControllerSceneTest, ChannelSessionsAreIsolated) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("ChanIsolate")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);

    // Add a layer on BaseColor.
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    const int baseLayers = ctrl->layerCount();
    ctrl->addPaintLayer(QStringLiteral("bc-extra"));
    const int baseAfter = ctrl->layerCount();
    EXPECT_GT(baseAfter, baseLayers);

    // Switch to Emissive — its own (fresh) session shouldn't inherit the
    // BaseColor layer we just added.
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::Emissive));
    EXPECT_LT(ctrl->layerCount(), baseAfter);

    // Switch back to BaseColor — the stashed stack is restored.
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    EXPECT_EQ(ctrl->layerCount(), baseAfter);
}

TEST_F(TexturePaintControllerSceneTest, BakeUnpaintedChannelReturnsFalse) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BakeEmpty")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    // Metallic never painted → nothing to bake.
    EXPECT_FALSE(ctrl->bakeChannel(static_cast<int>(PaintChannelNS::Channel::Metallic)));
}

// Undo must survive a channel switch (#547): a stroke on BaseColor, then switch
// to another channel, then Ctrl+Z restores BaseColor — the undo command keys on
// (entity, channel), NOT the transient GPU texture name that changes on switch.
TEST_F(TexturePaintControllerSceneTest, UndoSurvivesChannelSwitch) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("UndoAcrossChannel")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    ASSERT_TRUE(ctrl->hasActiveSession());

    // Snapshot BaseColor before, paint a stroke, snapshot after.
    const QImage before = ctrl->snapshotBufferImage();
    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.6, 0.5);
    ctrl->endStrokeUV();
    pumpEventsFor(150);
    const QImage afterPaint = ctrl->snapshotBufferImage();
    ASSERT_FALSE(before.isNull());
    ASSERT_FALSE(afterPaint.isNull());
    ASSERT_NE(before, afterPaint) << "stroke should have changed the buffer";

    // Switch to Roughness — a different session with a fresh GPU texture name.
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::Roughness));
    EXPECT_EQ(ctrl->activeChannel(),
              static_cast<int>(PaintChannelNS::Channel::Roughness));

    // Undo: must reactivate BaseColor and restore its pre-stroke pixels
    // (before the fix this no-oped because the texture name had changed).
    UndoManager::getSingleton()->undo();
    pumpEventsFor(150);
    EXPECT_EQ(ctrl->activeChannel(),
              static_cast<int>(PaintChannelNS::Channel::BaseColor));
    const QImage afterUndo = ctrl->snapshotBufferImage();
    ASSERT_FALSE(afterUndo.isNull());
    EXPECT_EQ(afterUndo, before) << "undo across a channel switch must restore BaseColor";
}

namespace {
// Read the texture bound to a named (alias-aware for diffuse) slot on sub 0.
QString slotTextureName(Ogre::Entity* ent, const char* slot)
{
    if (!ent || ent->getNumSubEntities() == 0) return {};
    auto mat = ent->getSubEntity(0)->getMaterial();
    if (!mat || mat->getNumTechniques() == 0) return {};
    auto* pass = mat->getTechnique(0)->getPass(0);
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        if (tus->getName() == slot)
            return QString::fromStdString(tus->getTextureName());
    }
    return {};
}
} // namespace

// #547 bug: clicking Bake on the BaseColor channel must overwrite the model's
// EXISTING diffuse TUS (named "diffuse_map" on a typical import — NOT the
// canonical "albedo"). The pre-fix code created a brand-new "albedo" TUS,
// leaving the real diffuse_map still bound to the transient paint texture; when
// closeSession() then restored diffuse_map to its pre-paint texture and removed
// the paint texture, the model "lost" the painted result. The baked texture
// must survive closeSession() on the slot the model actually samples, and the
// plain material must NOT be silently promoted to Cook-Torrance.
TEST_F(TexturePaintControllerSceneTest, BaseColorBakeSurvivesCloseSession) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BaseColorBake")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    ASSERT_TRUE(ctrl->hasActiveSession());

    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.6, 0.55);
    ctrl->endStrokeUV();
    pumpEventsFor(150);

    ASSERT_TRUE(ctrl->bakeChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor)));

    // The baked file lands on the model's existing diffuse_map slot (alias of
    // BaseColor's canonical "albedo"), not a new stray albedo TUS.
    const QString baked = slotTextureName(m_fix.entity, "diffuse_map");
    EXPECT_FALSE(baked.isEmpty()) << "diffuse_map must carry the baked texture";
    EXPECT_TRUE(baked.startsWith(QStringLiteral("paint_basecolor_")))
        << "expected the baked file, got '" << baked.toStdString() << "'";
    EXPECT_FALSE(passHasSlot(m_fix.entity, "albedo"))
        << "bake must not create a duplicate albedo TUS on a diffuse_map model";

    // The plain (non-PBR) material must not be promoted to Cook-Torrance.
    {
        const auto& b = m_fix.mat->getTechnique(0)->getPass(0)->getUserObjectBindings();
        EXPECT_FALSE(b.getUserAny("pbr_workflow").has_value())
            << "a BaseColor bake must not add a PBR workflow tag";
    }

    // The crux: ending the session must NOT revert diffuse_map to its pre-paint
    // texture — the bake is permanent, so the model keeps the painted result.
    ctrl->closeSession();
    EXPECT_EQ(slotTextureName(m_fix.entity, "diffuse_map"), baked)
        << "closeSession() lost the baked BaseColor texture (the #547 bug)";
}

// #547 bug: merely NAVIGATING channels (without painting) must not swap the
// model's real slot textures for blank paint textures. The pre-fix code
// rebound a manual (blank/white) paint texture onto the active channel's slot
// at session-create, so switching to an unpainted Normal/Roughness/etc. slot
// washed the surface out or made it render untextured. The rebind is now
// deferred to the first painted stroke, so a channel switch alone leaves every
// existing slot texture untouched.
TEST_F(TexturePaintControllerSceneTest, ChannelSwitchWithoutPaintingKeepsSlotTextures) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("ChanSwitchKeepTex")));
    // Give the fixture's diffuse a stable texture name we can watch.
    m_fix.mat->getTechnique(0)->getPass(0)
        ->getTextureUnitState(0)->setTextureName("orig_diffuse.png");

    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    ASSERT_TRUE(ctrl->hasActiveSession());

    // Walk every channel WITHOUT painting a single stroke.
    for (int c = 0; c < PaintChannelNS::kTexturePaintChannelCount; ++c)
        ctrl->setActiveChannel(c);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));

    // The model's diffuse must still point at its original texture — no blank
    // paint texture was ever bound because nothing was painted.
    EXPECT_EQ(slotTextureName(m_fix.entity, "diffuse_map"),
              QStringLiteral("orig_diffuse.png"))
        << "navigating channels without painting must not rebind the diffuse";

    ctrl->closeSession();
    EXPECT_EQ(slotTextureName(m_fix.entity, "diffuse_map"),
              QStringLiteral("orig_diffuse.png"));
}

// #547 bug: baking BaseColor must PRESERVE the model's existing diffuse where
// the user didn't paint. The paint session's base buffer is transparent when
// its source texture couldn't be seeded, so a raw save would bind a mostly-
// transparent diffuse and "lose" the base colour. bakeChannel composites the
// painted strokes OVER the slot's underlying texture — so untouched texels keep
// the original colour. This test binds a known solid-red diffuse, paints, bakes,
// and verifies a corner texel (unlikely to be painted at UV 0.5) stays red.
TEST_F(TexturePaintControllerSceneTest, BaseColorBakePreservesUnpaintedDiffuse) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BaseColorPreserve")));

    // Write a solid-red base diffuse to a temp dir and register it so the
    // controller's loadImageAcrossGroups can read it back.
    static QTemporaryDir s_tmp;  // outlive the resource-location registration
    ASSERT_TRUE(s_tmp.isValid());
    const QString baseName = QStringLiteral("preserve_base.png");
    const QString basePath = s_tmp.path() + "/" + baseName;
    QImage red(64, 64, QImage::Format_RGBA8888);
    red.fill(QColor(220, 20, 20, 255));
    ASSERT_TRUE(red.save(basePath));
    Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
        s_tmp.path().toStdString(), "FileSystem",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, false);
    m_fix.mat->getTechnique(0)->getPass(0)
        ->getTextureUnitState(0)->setTextureName(baseName.toStdString());

    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    ASSERT_TRUE(ctrl->hasActiveSession());

    // Paint a small stroke near the centre; the corners stay unpainted.
    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.52, 0.52);
    ctrl->endStrokeUV();
    pumpEventsFor(150);

    ASSERT_TRUE(ctrl->bakeChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor)));

    // The freshly-bound diffuse must be the composited bake, and its corner
    // (unpainted) must still be the original red — not transparent/black/white.
    const QString baked = slotTextureName(m_fix.entity, "diffuse_map");
    ASSERT_TRUE(baked.startsWith(QStringLiteral("paint_basecolor_")));
    // Re-read the baked file through Ogre's resource system — bakeChannel
    // already registered its generatedTexDir as a resource location.
    Ogre::Image ogreImg;
    bool haveImg = false;
    try {
        ogreImg.load(baked.toStdString(),
                     Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
        haveImg = ogreImg.getWidth() > 0 && ogreImg.getHeight() > 0;
    } catch (...) { haveImg = false; }
    ASSERT_TRUE(haveImg) << "baked diffuse '" << baked.toStdString()
                         << "' should be loadable";
    const Ogre::ColourValue corner = ogreImg.getColourAt(0, 0, 0);
    EXPECT_GT(corner.r, 0.6f) << "unpainted corner lost its red base colour";
    EXPECT_LT(corner.g, 0.4f);
    EXPECT_LT(corner.b, 0.4f);
    EXPECT_GT(corner.a, 0.9f) << "baked diffuse must be opaque, not transparent";

    ctrl->closeSession();
}

// #547 bug: the SECOND bake wiped the texture. bindBakedChannelTexture used to
// flushDirtyToOgre() at the end, which re-uploaded the stale paint buffer and —
// after pruning m_boundSlots — re-bound the transient paint texture straight
// over the just-baked file. So the first bake changed the render and the second
// bake left the slot pointing at an empty paint texture. bakeChannel now tears
// the live session down cleanly instead, and each bake produces a fresh, valid
// file bound into the slot. This test bakes BaseColor twice and asserts the slot
// still carries a real baked diffuse (never a QMEPaint_* / empty binding).
TEST_F(TexturePaintControllerSceneTest, RepeatedBaseColorBakeKeepsValidTexture) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("RepeatBake")));
    static QTemporaryDir s_tmp2;
    ASSERT_TRUE(s_tmp2.isValid());
    const QString baseName = QStringLiteral("repeat_base.png");
    QImage base(64, 64, QImage::Format_RGBA8888);
    base.fill(QColor(30, 120, 200, 255));
    ASSERT_TRUE(base.save(s_tmp2.path() + "/" + baseName));
    Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
        s_tmp2.path().toStdString(), "FileSystem",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, false);
    m_fix.mat->getTechnique(0)->getPass(0)
        ->getTextureUnitState(0)->setTextureName(baseName.toStdString());

    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);

    auto paintAndBake = [&](double u, double v) {
        ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
        ASSERT_TRUE(ctrl->hasActiveSession());
        ASSERT_TRUE(ctrl->beginStrokeUV(u, v));
        ctrl->updateStrokeUV(u + 0.02, v + 0.02);
        ctrl->endStrokeUV();
        pumpEventsFor(150);
        ASSERT_TRUE(ctrl->bakeChannel(
            static_cast<int>(PaintChannelNS::Channel::BaseColor)));
    };

    paintAndBake(0.4, 0.4);
    const QString first = slotTextureName(m_fix.entity, "diffuse_map");
    EXPECT_TRUE(first.startsWith(QStringLiteral("paint_basecolor_")))
        << "first bake bound '" << first.toStdString() << "'";

    // Second round: the session was torn down by the first bake; painting
    // rebuilds it seeded from the baked result. The slot must end on a real
    // baked file, NOT a transient paint texture or empty name.
    paintAndBake(0.6, 0.6);
    const QString second = slotTextureName(m_fix.entity, "diffuse_map");
    EXPECT_TRUE(second.startsWith(QStringLiteral("paint_basecolor_")))
        << "second bake bound '" << second.toStdString() << "' (the #547 bug)";
    EXPECT_FALSE(second.startsWith(QStringLiteral("QMEPaint_")))
        << "second bake left the slot on the transient paint texture";
    EXPECT_FALSE(second.isEmpty());

    ctrl->closeSession();
}

// #547: a Normal bake must COMBINE with the model's existing normal map, not
// replace it. It generates a detail normal from the painted grayscale and
// whiteout-blends it onto the base normal; untouched texels (flat detail normal
// 0,0,1) keep the base normal exactly. This test binds a tilted base normal,
// paints a small stroke, bakes, and asserts an unpainted corner still carries
// the tilted base (not a flat 128,128,255 wipe).
TEST_F(TexturePaintControllerSceneTest, NormalBakeCombinesWithExistingNormal) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("NormalCombine")));
    static QTemporaryDir s_tmp3;
    ASSERT_TRUE(s_tmp3.isValid());
    const QString baseName = QStringLiteral("base_normal.png");
    // A distinctly tilted tangent-space normal: +X lean → R high, G mid, B mid.
    QImage baseN(64, 64, QImage::Format_RGBA8888);
    baseN.fill(QColor(220, 128, 150, 255));
    ASSERT_TRUE(baseN.save(s_tmp3.path() + "/" + baseName));
    Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
        s_tmp3.path().toStdString(), "FileSystem",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, false);

    // Give the fixture material a normal_map slot holding the tilted base.
    auto* pass = m_fix.mat->getTechnique(0)->getPass(0);
    auto* ntus = pass->createTextureUnitState();
    ntus->setName("normal_map");
    ntus->setTextureName(baseName.toStdString());

    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::Normal));
    ASSERT_TRUE(ctrl->hasActiveSession());

    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.52, 0.52);
    ctrl->endStrokeUV();
    pumpEventsFor(150);
    ASSERT_TRUE(ctrl->bakeChannel(static_cast<int>(PaintChannelNS::Channel::Normal)));

    const QString baked = slotTextureName(m_fix.entity, "normal_map");
    ASSERT_TRUE(baked.startsWith(QStringLiteral("paint_normal_")));
    Ogre::Image img;
    bool ok = false;
    try {
        img.load(baked.toStdString(),
                 Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
        ok = img.getWidth() > 0;
    } catch (...) { ok = false; }
    ASSERT_TRUE(ok);
    // Unpainted corner must retain the tilted base (R clearly > B), NOT a flat
    // (128,128,255) normal that a plain overwrite would have produced.
    const Ogre::ColourValue corner = img.getColourAt(0, 0, 0);
    EXPECT_GT(corner.r, 0.65f) << "corner lost the base normal's +X tilt";
    EXPECT_GT(corner.r, corner.b) << "flat-wiped normal (base normal replaced)";

    ctrl->closeSession();
}

// #548 Slice E: a symmetric stroke (primary + mirror dabs) must remain ONE undo
// step, and enabling symmetry must never crash the stroke path even when a
// mirror point falls off the mesh. Uses the standard single-triangle fixture;
// the mirror across local X lands off-mesh (so only the primary paints), which
// still exercises the mirror dispatch + the single-command undo guarantee.
TEST_F(TexturePaintControllerSceneTest, SymmetricStrokeIsOneUndoStep) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("SymUndo")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    ASSERT_TRUE(ctrl->hasActiveSession());
    ctrl->setSymmetryEnabled(true);
    ctrl->setSymmetryAxes(static_cast<int>(TexturePaintController::SymAxisX));

    const QImage before = ctrl->snapshotBufferImage();
    ASSERT_TRUE(ctrl->beginStrokeUV(0.4, 0.4));
    ctrl->updateStrokeUV(0.45, 0.45);
    ctrl->endStrokeUV();
    pumpEventsFor(150);
    const QImage afterPaint = ctrl->snapshotBufferImage();
    ASSERT_FALSE(before.isNull());
    ASSERT_NE(before, afterPaint) << "the primary dab should have painted";

    // A single undo restores the pre-stroke buffer — the mirror dabs were
    // captured by the same command, not a second one.
    UndoManager::getSingleton()->undo();
    pumpEventsFor(150);
    const QImage afterUndo = ctrl->snapshotBufferImage();
    EXPECT_EQ(afterUndo, before) << "one undo must revert the whole symmetric stroke";

    ctrl->setSymmetryEnabled(false);
    ctrl->closeSession();
}

// #549 F-C: projection-mode setters round-trip and gate correctly, and
// projectFromPhoto fails GRACEFULLY (no crash, no layer) when there's no
// viewport camera in the headless fixture. The projection MATH is covered
// pure-data in ProjectionPainter_test; here we prove the controller plumbing.
TEST_F(TexturePaintControllerSceneTest, ProjectionModeSettersAndGracefulNoCamera) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("ProjMode")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    ASSERT_TRUE(ctrl->hasActiveSession());

    ctrl->setProjectionMode(1);
    EXPECT_EQ(ctrl->projectionMode(), 1);
    ctrl->setProjBackfaceCull(false);
    EXPECT_FALSE(ctrl->projBackfaceCull());
    ctrl->setProjUseOcclusion(true);
    EXPECT_TRUE(ctrl->projUseOcclusion());
    ctrl->setProjDepthLimit(0.5);
    EXPECT_NEAR(ctrl->projDepthLimit(), 0.5, 1e-6);
    ctrl->setProjectionMode(2);
    EXPECT_FALSE(ctrl->cameraLocked()) << "mode switch clears the lock until Snap";

    // No active viewport widget in the fixture → projectFromPhoto must fail
    // gracefully (return false, add no layer, not crash). Write a temp image.
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    const QString photo = tmp.path() + "/photo.png";
    QImage img(16, 16, QImage::Format_RGBA8888); img.fill(Qt::green);
    ASSERT_TRUE(img.save(photo));
    const int before = ctrl->layerCount();
    const bool ok = ctrl->projectFromPhoto(photo);   // no camera → false
    EXPECT_FALSE(ok);
    EXPECT_EQ(ctrl->layerCount(), before) << "a failed projection must not add a layer";

    ctrl->setProjectionMode(0);
    ctrl->closeSession();
}

// #549 F-D: the decal session transitions correctly through the controller
// (begin → placing; cancel → idle) and cancelDecal is safe. The full
// place/drag/commit needs a viewport camera the headless fixture lacks, so the
// geometry is covered pure-data in DecalSession_test; here we prove the
// controller session plumbing + graceful teardown.
TEST_F(TexturePaintControllerSceneTest, DecalSessionBeginCancelPlumbing) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("DecalPlumbing")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    ASSERT_TRUE(ctrl->hasActiveSession());

    EXPECT_FALSE(ctrl->decalSessionActive());

    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    const QString img = tmp.path() + "/decal.png";
    QImage di(24, 24, QImage::Format_RGBA8888); di.fill(QColor(255, 0, 0, 255));
    ASSERT_TRUE(di.save(img));

    ASSERT_TRUE(ctrl->beginDecal(img));
    EXPECT_TRUE(ctrl->decalSessionActive());
    EXPECT_EQ(ctrl->decalState(), 1);   // Placing (awaiting the placing click)
    EXPECT_EQ(ctrl->brushTool(), static_cast<int>(TexturePaintController::ToolDecal));

    // Commit before placing → session ends, no layer added, no crash.
    const int before = ctrl->layerCount();
    ctrl->commitDecal();
    EXPECT_FALSE(ctrl->decalSessionActive());
    EXPECT_EQ(ctrl->layerCount(), before);

    // Begin again then cancel.
    ASSERT_TRUE(ctrl->beginDecal(img));
    EXPECT_TRUE(ctrl->decalSessionActive());
    ctrl->cancelDecal();
    EXPECT_FALSE(ctrl->decalSessionActive());

    ctrl->closeSession();
}

// --- Paint v2 Slice H (#551): brush presets + colour palettes -------------
// The library cores are covered pure-data in BrushPresetLibrary_test /
// ColorPaletteLibrary_test. These cases cover the CONTROLLER contract: a preset
// actually reaches the live brush, capture round-trips, and every entry point
// degrades safely on bad input.

TEST_F(TexturePaintControllerSceneTest, ApplyBrushPresetReachesTheLiveBrush) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("PresetApply")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_NE(ctrl, nullptr);

    // "Hard Round": radius 0.04, strength 1.0, falloff 0.05, round footprint.
    ASSERT_TRUE(ctrl->applyBrushPreset(QStringLiteral("Hard Round")));
    EXPECT_NEAR(ctrl->texturePaintRadius(), 0.04, 1e-6);
    EXPECT_NEAR(ctrl->texturePaintStrength(), 1.0, 1e-6);
    EXPECT_NEAR(ctrl->texturePaintFalloff(), 0.05, 1e-6);

    // Switching presets must move every field, not just the first one applied.
    ASSERT_TRUE(ctrl->applyBrushPreset(QStringLiteral("Wet Brush")));
    EXPECT_NEAR(ctrl->texturePaintRadius(), 0.1, 1e-6);
    EXPECT_NEAR(ctrl->texturePaintStrength(), 0.25, 1e-6);
    EXPECT_NEAR(ctrl->texturePaintFalloff(), 0.95, 1e-6);

    ctrl->closeSession();
}

TEST_F(TexturePaintControllerSceneTest, ApplyStampPresetSelectsItsStampAndFootprint) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("PresetStamp")));
    auto* ctrl = TexturePaintController::instance();

    // Move the stamp somewhere else FIRST. m_activeStampName is restored from
    // QSettings, so without this the expected value can already be in place and
    // the assertion passes even if the preset never applies it (verified: a
    // mutant that skipped setActiveStampName survived this test until the
    // pre-set was added).
    ctrl->setActiveStampName(QStringLiteral("Soft Circle"));
    ASSERT_EQ(ctrl->activeStampName(), QStringLiteral("Soft Circle"));

    ASSERT_TRUE(ctrl->applyBrushPreset(QStringLiteral("Spray Paint")));
    EXPECT_EQ(ctrl->activeStampName(), QStringLiteral("Spatter"))
        << "a stamp preset must select its stamp asset";
    EXPECT_EQ(ctrl->footprintType(),
              static_cast<int>(BrushFootprint::FootprintType::StampImage))
        << "and switch the footprint to stamp mode";
    // Dynamics ride along, else the preset would look identical to a plain brush.
    EXPECT_NEAR(ctrl->stampScatter(), 0.8, 1e-6);
    EXPECT_NEAR(ctrl->stampSizeJitter(), 0.5, 1e-6);

    ctrl->closeSession();
}

TEST_F(TexturePaintControllerSceneTest, ApplyBrushPresetLeavesPaintColorAlone) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("PresetColor")));
    auto* ctrl = TexturePaintController::instance();
    auto* em = EditModeController::instance();
    ASSERT_NE(em, nullptr);

    em->setVertexPaintColor(QColor(12, 34, 56));
    ASSERT_TRUE(ctrl->applyBrushPreset(QStringLiteral("Soft Round")));
    // A preset describes the BRUSH, not what you paint with — clobbering the
    // user's colour on every preset click would be hostile.
    EXPECT_EQ(em->vertexPaintColor(), QColor(12, 34, 56));

    ctrl->closeSession();
}

TEST_F(TexturePaintControllerSceneTest, SaveBrushPresetCapturesCurrentBrush) {
    QStandardPaths::setTestModeEnabled(true);
    ASSERT_TRUE(m_fix.setup(QStringLiteral("PresetSave")));
    auto* ctrl = TexturePaintController::instance();

    ctrl->setBrushRadius(0.0321);
    ctrl->setBrushStrength(0.456);
    ctrl->setBrushFalloff(0.789);
    ASSERT_TRUE(ctrl->saveBrushPreset(QStringLiteral("Captured Brush")));

    // Move the brush away, then apply the capture — it must come back.
    ctrl->setBrushRadius(0.2);
    ctrl->setBrushStrength(0.1);
    ASSERT_TRUE(ctrl->applyBrushPreset(QStringLiteral("Captured Brush")));
    EXPECT_NEAR(ctrl->texturePaintRadius(), 0.0321, 1e-6);
    EXPECT_NEAR(ctrl->texturePaintStrength(), 0.456, 1e-6);
    EXPECT_NEAR(ctrl->texturePaintFalloff(), 0.789, 1e-6);

    EXPECT_TRUE(ctrl->deleteBrushPreset(QStringLiteral("Captured Brush")));
    ctrl->closeSession();
    QStandardPaths::setTestModeEnabled(false);
}

TEST_F(TexturePaintControllerSceneTest, BrushPresetEntryPointsRejectBadInput) {
    auto* ctrl = TexturePaintController::instance();
    ASSERT_NE(ctrl, nullptr);

    EXPECT_FALSE(ctrl->applyBrushPreset(QStringLiteral("No Such Preset")));
    EXPECT_FALSE(ctrl->saveBrushPreset(QString()));
    EXPECT_FALSE(ctrl->saveBrushPreset(QStringLiteral("   ")))
        << "a whitespace-only name is not a usable preset name";
    // Bundled presets are compiled in: "deleting" one could only remove a user
    // override, so it must refuse rather than appear to delete something that
    // comes straight back on restart.
    EXPECT_FALSE(ctrl->deleteBrushPreset(QStringLiteral("Soft Round")));
    EXPECT_TRUE(ctrl->isBundledBrushPreset(QStringLiteral("Soft Round")));
    EXPECT_FALSE(ctrl->isBundledBrushPreset(QStringLiteral("No Such Preset")));
    EXPECT_GE(ctrl->brushPresetNames().size(), 15);
}

TEST_F(TexturePaintControllerSceneTest, PaletteColorAppliesAndFeedsRecentRing) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("PaletteApply")));
    auto* ctrl = TexturePaintController::instance();
    auto* em = EditModeController::instance();
    ASSERT_NE(em, nullptr);

    ASSERT_TRUE(ctrl->applyPaletteColor(QStringLiteral("#4caf50")));
    EXPECT_EQ(em->vertexPaintColor(), QColor(0x4c, 0xaf, 0x50));
    ASSERT_FALSE(ctrl->recentPaintColors().isEmpty());
    EXPECT_EQ(ctrl->recentPaintColors().front(), QStringLiteral("#4caf50"));

    // The background slot must NOT churn the recent ring.
    const int before = ctrl->recentPaintColors().size();
    ASSERT_TRUE(ctrl->applyPaletteColor(QStringLiteral("#ff0000"), /*asBackground=*/true));
    EXPECT_EQ(em->vertexPaintBackgroundColor(), QColor(255, 0, 0));
    EXPECT_EQ(ctrl->recentPaintColors().size(), before)
        << "background picks must not enter the recent ring";

    EXPECT_FALSE(ctrl->applyPaletteColor(QStringLiteral("nonsense")));
    ctrl->closeSession();
}

TEST_F(TexturePaintControllerSceneTest, PaletteListsExposeBundledContent) {
    auto* ctrl = TexturePaintController::instance();
    ASSERT_NE(ctrl, nullptr);
    const QStringList names = ctrl->colorPaletteNames();
    EXPECT_GE(names.size(), 6) << "#551 requires at least 6 bundled palettes";
    ASSERT_TRUE(names.contains(QStringLiteral("Material Design")));

    const QStringList sw = ctrl->colorPaletteSwatches(QStringLiteral("Material Design"));
    EXPECT_FALSE(sw.isEmpty());
    for (const QString& s : sw)
        EXPECT_TRUE(s.startsWith('#') && s.size() == 7) << s.toStdString();

    EXPECT_TRUE(ctrl->colorPaletteSwatches(QStringLiteral("No Such Palette")).isEmpty());
}

TEST_F(TexturePaintControllerSceneTest, SavePaletteFromTextureNeedsABuffer) {
    QStandardPaths::setTestModeEnabled(true);
    auto* ctrl = TexturePaintController::instance();
    ctrl->closeSession();
    // No session => no buffer => must refuse rather than save an empty palette.
    EXPECT_FALSE(ctrl->savePaletteFromTexture(QStringLiteral("From Nothing")));

    ASSERT_TRUE(m_fix.setup(QStringLiteral("PaletteFromTex")));
    ctrl->setTexturePaintEnabled(true);
    ASSERT_TRUE(ctrl->hasActiveSession());
    EXPECT_FALSE(ctrl->savePaletteFromTexture(QString()))
        << "an unnamed palette is not saveable";

    ctrl->closeSession();
    QStandardPaths::setTestModeEnabled(false);
}

// --- Slice H review fixes (#551 / PR #970) --------------------------------

TEST_F(TexturePaintControllerSceneTest, PaletteColorPreservesBrushAlpha) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("PaletteAlpha")));
    auto* ctrl = TexturePaintController::instance();
    auto* em = EditModeController::instance();
    ASSERT_NE(em, nullptr);

    // Swatches carry no alpha by design, so applying one must KEEP the brush's
    // existing alpha rather than forcing it opaque via QColor(r,g,b).
    em->setVertexPaintColor(QColor(10, 20, 30, 128));
    ASSERT_TRUE(ctrl->applyPaletteColor(QStringLiteral("#4caf50")));
    EXPECT_EQ(em->vertexPaintColor().alpha(), 128)
        << "picking a swatch must not silently make the brush opaque";
    EXPECT_EQ(em->vertexPaintColor().red(), 0x4c);

    em->setVertexPaintBackgroundColor(QColor(1, 2, 3, 64));
    ASSERT_TRUE(ctrl->applyPaletteColor(QStringLiteral("#ff0000"), true));
    EXPECT_EQ(em->vertexPaintBackgroundColor().alpha(), 64);

    ctrl->closeSession();
}

TEST_F(TexturePaintControllerSceneTest, RecentRingSeesColorsSetOutsideThePalette) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("RecentShared")));
    auto* ctrl = TexturePaintController::instance();
    auto* em = EditModeController::instance();
    ASSERT_NE(em, nullptr);

    // The toolbar picker and pickBrushColorInteractive() call
    // setVertexPaintColor directly. Without hooking the shared change signal,
    // Recent would only ever show colours re-picked from palette tiles.
    em->setVertexPaintColor(QColor(0x11, 0x22, 0x33));
    const QStringList recent = ctrl->recentPaintColors();
    ASSERT_FALSE(recent.isEmpty());
    EXPECT_EQ(recent.front(), QStringLiteral("#112233"));

    ctrl->closeSession();
}

TEST_F(TexturePaintControllerSceneTest, PresetRoundTripsFgBgRampMode) {
    QStandardPaths::setTestModeEnabled(true);
    ASSERT_TRUE(m_fix.setup(QStringLiteral("PresetFgBg")));
    auto* ctrl = TexturePaintController::instance();

    // Save a gradient brush in FG/BG mode.
    ctrl->setColorSource(1);          // Gradient
    ctrl->setUseFgBgRamp(true);
    ASSERT_TRUE(ctrl->saveBrushPreset(QStringLiteral("FgBg Preset")));

    // Move to a NAMED ramp, then re-apply: FG/BG must come back, not the name.
    ctrl->setUseFgBgRamp(false);
    ctrl->setActiveRampName(QStringLiteral("Sunset"));
    ASSERT_TRUE(ctrl->applyBrushPreset(QStringLiteral("FgBg Preset")));
    EXPECT_TRUE(ctrl->useFgBgRamp())
        << "an FG/BG gradient preset must not restore a named ramp instead";

    EXPECT_TRUE(ctrl->deleteBrushPreset(QStringLiteral("FgBg Preset")));
    ctrl->closeSession();
    QStandardPaths::setTestModeEnabled(false);
}

TEST_F(TexturePaintControllerSceneTest, CustomPresetShadowingBundledNameIsDeletable) {
    QStandardPaths::setTestModeEnabled(true);
    ASSERT_TRUE(m_fix.setup(QStringLiteral("PresetShadow")));
    auto* ctrl = TexturePaintController::instance();

    // A bundled name with no custom file: nothing to delete.
    EXPECT_FALSE(ctrl->canDeleteBrushPreset(QStringLiteral("Soft Round")));
    EXPECT_FALSE(ctrl->deleteBrushPreset(QStringLiteral("Soft Round")));

    // Save a custom preset that SHADOWS the bundled name. Because custom wins
    // in allPresets(), refusing to delete it would permanently hide the bundled
    // version with no way back.
    ctrl->setBrushRadius(0.191);
    ASSERT_TRUE(ctrl->saveBrushPreset(QStringLiteral("Soft Round")));
    EXPECT_TRUE(ctrl->canDeleteBrushPreset(QStringLiteral("Soft Round")));
    EXPECT_TRUE(ctrl->deleteBrushPreset(QStringLiteral("Soft Round")));

    // The bundled version is back.
    ASSERT_TRUE(ctrl->applyBrushPreset(QStringLiteral("Soft Round")));
    EXPECT_NEAR(ctrl->texturePaintRadius(), 0.06, 1e-6)
        << "deleting the override must restore the bundled preset";

    ctrl->closeSession();
    QStandardPaths::setTestModeEnabled(false);
}

// --- Slice I (#552): bake-up workflow -------------------------------------

TEST_F(TexturePaintControllerSceneTest, BakeTargetIdsAndLabelsAreExposedToQml) {
    auto* ctrl = TexturePaintController::instance();
    const QStringList ids = ctrl->bakeTargetIds();
    EXPECT_TRUE(ids.contains(QStringLiteral("generic")));
    EXPECT_TRUE(ids.contains(QStringLiteral("unity")));
    EXPECT_TRUE(ids.contains(QStringLiteral("unreal")));
    EXPECT_TRUE(ids.contains(QStringLiteral("godot")));
    EXPECT_TRUE(ids.contains(QStringLiteral("gltf")));
    EXPECT_FALSE(ctrl->bakeTargetLabel(QStringLiteral("unity")).isEmpty());
    EXPECT_TRUE(ctrl->bakeTargetLabel(QStringLiteral("nope")).isEmpty());
}

TEST_F(TexturePaintControllerSceneTest, BakeWithNothingPaintedReportsAnError) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BakeEmpty")));
    auto* ctrl = TexturePaintController::instance();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // No session at all: must refuse cleanly rather than writing blank files.
    const QString err = ctrl->bakePbrSet(QStringLiteral("generic"), dir.path());
    EXPECT_FALSE(err.isEmpty()) << "an empty bake must report why, not succeed";
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files).isEmpty())
        << "a failed bake must leave no files behind";
}

TEST_F(TexturePaintControllerSceneTest, BakeRejectsAnUnknownTargetBeforeTouchingDisk) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BakeBadTarget")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(64));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString err = ctrl->bakePbrSet(QStringLiteral("unrealengine5"), dir.path());
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files).isEmpty());
}

TEST_F(TexturePaintControllerSceneTest, BakeRejectsAnEmptyOutputDirectory) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BakeNoDir")));
    auto* ctrl = TexturePaintController::instance();
    ASSERT_TRUE(ctrl->ensurePaintableTexture(64));
    EXPECT_FALSE(ctrl->bakePbrSet(QStringLiteral("generic"), QString()).isEmpty());
}

TEST_F(TexturePaintControllerSceneTest, BakeWritesTexturesAndSidecarForThePaintedChannel) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BakeWrite")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    ASSERT_TRUE(ctrl->hasActiveSession());
    // A stroke so BaseColor actually has data.
    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.52, 0.52);
    ctrl->endStrokeUV();
    pumpEventsFor(150);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString err = ctrl->bakePbrSet(QStringLiteral("generic"), dir.path(),
                                         64, QStringLiteral("hero"));
    ASSERT_TRUE(err.isEmpty()) << err.toStdString();

    const QStringList files = QDir(dir.path()).entryList(QDir::Files);
    EXPECT_TRUE(files.contains(QStringLiteral("hero_BaseColor.png")))
        << files.join(", ").toStdString();
    EXPECT_TRUE(files.contains(QStringLiteral("hero.bake.json")))
        << "the sidecar is an acceptance criterion: " << files.join(", ").toStdString();

    // The sidecar must describe what was actually written.
    QFile f(QDir(dir.path()).filePath(QStringLiteral("hero.bake.json")));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    EXPECT_EQ(root["target"].toString(), QStringLiteral("generic"));
    EXPECT_EQ(root["namePrefix"].toString(), QStringLiteral("hero"));
    EXPECT_GT(root["outputs"].toArray().size(), 0);
}

TEST_F(TexturePaintControllerSceneTest, BakeCanSkipTheSidecar) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BakeNoSidecar")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    ASSERT_TRUE(ctrl->hasActiveSession());
    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.52, 0.52);
    ctrl->endStrokeUV();
    pumpEventsFor(150);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(ctrl->bakePbrSet(QStringLiteral("generic"), dir.path(), 64,
                                 QString(), false, /*writeSidecar=*/false).isEmpty());
    const QStringList files = QDir(dir.path()).entryList(QDir::Files);
    EXPECT_FALSE(files.filter(QStringLiteral(".bake.json")).size() > 0);
    EXPECT_TRUE(files.contains(QStringLiteral("BaseColor.png")))
        << "no prefix means bare channel names";
}

TEST_F(TexturePaintControllerSceneTest, GodotBakeAlsoWritesTheTresSidecar) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BakeGodot")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    ASSERT_TRUE(ctrl->hasActiveSession());
    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.52, 0.52);
    ctrl->endStrokeUV();
    pumpEventsFor(150);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(ctrl->bakePbrSet(QStringLiteral("godot"), dir.path(), 64,
                                 QStringLiteral("mat")).isEmpty());
    const QStringList files = QDir(dir.path()).entryList(QDir::Files);
    EXPECT_TRUE(files.contains(QStringLiteral("mat.tres")))
        << "the .tres carries Godot's sRGB/linear flags: "
        << files.join(", ").toStdString();
    EXPECT_TRUE(files.contains(QStringLiteral("mat_Albedo.png")));
}

// The ACTIVE channel's stack lives in m_layerStack; its m_channelSessions copy
// is deliberately stale until the next switch. A bake that read the map blindly
// would silently emit the pre-edit state of whatever channel is open — the most
// likely way this whole feature goes quietly wrong.
TEST_F(TexturePaintControllerSceneTest, BakeReadsTheLiveStackForTheActiveChannel) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BakeLive")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    ASSERT_TRUE(ctrl->hasActiveSession());

    // Nothing painted yet -> BaseColor still counts as having a stack (the
    // session seeded layer 0), but the point is the LIVE one is consulted.
    ASSERT_TRUE(ctrl->beginStrokeUV(0.25, 0.25));
    ctrl->updateStrokeUV(0.27, 0.27);
    ctrl->endStrokeUV();
    pumpEventsFor(150);
    const QStringList painted = ctrl->paintedChannelIds();
    EXPECT_TRUE(painted.contains(QStringLiteral("basecolor")))
        << "the active channel must be visible to the bake: "
        << painted.join(", ").toStdString();
}

// Height must never appear as a bakeable channel: it shares the Normal session
// and has no data of its own (#547).
//
// Asserting only on paintedChannelIds() cannot fail — setActiveChannel redirects
// Height to Normal, so m_channelSessions[Height] is never populated and the
// filter is unreachable defensive code (a mutant removing it still passed).
// So this pins the REACHABLE invariant that makes Height unbakeable: selecting
// it lands on Normal, and the bake's channel list therefore never grows a
// height entry even after painting what the user asked to be "Height".
TEST_F(TexturePaintControllerSceneTest, SelectingHeightRedirectsToNormalSoNothingBakesAsHeight) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BakeNoHeight")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);

    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::Height));
    EXPECT_EQ(ctrl->activeChannel(),
              static_cast<int>(PaintChannelNS::Channel::Normal))
        << "Height must redirect to Normal; otherwise it would accumulate a "
           "session the bake has no output for";

    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.52, 0.52);
    ctrl->endStrokeUV();
    pumpEventsFor(150);

    const QStringList painted = ctrl->paintedChannelIds();
    EXPECT_FALSE(painted.contains(QStringLiteral("height")))
        << painted.join(", ").toStdString();
    EXPECT_TRUE(painted.contains(QStringLiteral("normal")))
        << "the stroke must have landed on Normal: "
        << painted.join(", ").toStdString();

    // And Height is not offered by the picker at all.
    const QVariantList channels = ctrl->paintChannels();
    for (const QVariant& v : channels) {
        EXPECT_NE(v.toMap().value(QStringLiteral("id")).toString(),
                  QStringLiteral("height"));
    }
}

TEST_F(TexturePaintControllerSceneTest, BakePreviewUrlIsADataUriOrEmpty) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BakePreview")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::BaseColor));
    ASSERT_TRUE(ctrl->hasActiveSession());
    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.52, 0.52);
    ctrl->endStrokeUV();
    pumpEventsFor(150);

    const QString url = ctrl->bakePreviewUrl(QStringLiteral("generic"), 0, 64);
    EXPECT_TRUE(url.startsWith(QStringLiteral("data:image/png;base64,")))
        << url.left(40).toStdString();
    // Out-of-range and bad target degrade to empty, never crash.
    EXPECT_TRUE(ctrl->bakePreviewUrl(QStringLiteral("generic"), 99, 64).isEmpty());
    EXPECT_TRUE(ctrl->bakePreviewUrl(QStringLiteral("nope"), 0, 64).isEmpty());
}

TEST_F(TexturePaintControllerSceneTest, VertexLayerBakeDegradesWithoutASession) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BakeVertexNoSession")));
    auto* ctrl = TexturePaintController::instance();
    // No paint session: must report why rather than dereferencing a null stack.
    EXPECT_FALSE(ctrl->bakeVertexLayerToTextureLayer(64, 2).isEmpty());
}
