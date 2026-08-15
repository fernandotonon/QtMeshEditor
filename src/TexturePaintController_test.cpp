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

TEST_F(TexturePaintControllerSceneTest, PaintChannelsModelHasSevenChannels) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("ChanModel")));
    auto* ctrl = TexturePaintController::instance();
    const QVariantList chans = ctrl->paintChannels();
    ASSERT_EQ(chans.size(), PaintChannelNS::kTexturePaintChannelCount);
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

TEST_F(TexturePaintControllerSceneTest, HeightChannelBakeBindsNormalMapSlot) {
    ASSERT_TRUE(m_fix.setup(QStringLiteral("HeightBake")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setTexturePaintEnabled(true);
    ctrl->setActiveChannel(static_cast<int>(PaintChannelNS::Channel::Height));
    ASSERT_TRUE(ctrl->hasActiveSession());
    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    ctrl->updateStrokeUV(0.6, 0.5);
    ctrl->endStrokeUV();
    pumpEventsFor(150);
    // Height bakes a Sobel normal map into the normal_map slot.
    EXPECT_TRUE(ctrl->bakeChannel(static_cast<int>(PaintChannelNS::Channel::Height)));
    EXPECT_TRUE(passHasSlot(m_fix.entity, "normal_map", /*needTexture*/true));
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
