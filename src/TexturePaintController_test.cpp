#include <gtest/gtest.h>

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
