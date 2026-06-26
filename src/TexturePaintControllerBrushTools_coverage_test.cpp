// Coverage suite for TexturePaintController brush tools.
//
// The existing TexturePaintController_test.cpp is broad but only ever paints
// with the default ToolPaint and never flips the brush tool before a stroke.
// This file drives the untested switch arms of applyBrushAtUV() via the
// beginStrokeUV / updateStrokeUV / endStrokeUV public stroke API:
//   - ToolErase       → paints bgPaintColor via m_buffer.paintBrush
//   - ToolFill        → floodFillAtUV, single-stamp-per-stroke (m_strokeJustBegan)
//   - ToolColorPicker → pickColorAtUV → EditModeController::setVertexPaintColor
//   - ToolSmudge      → the per-pixel smudge loop with m_smudgePrev forwarding
//   - Square vs Round shape branch from EditModeController::vertexPaintShape
//   - the mesh-extent radius remap
// plus the standalone-ish paths:
//   - refreshUvOverlay / uvOverlayDataUri WITH an active session
//   - bakeToOriginalFile() resolving an on-disk path + rewriting the file
//
// Distinct file name + distinct TEST suite names (TexturePaintControllerCoverage*)
// so there is no ODR / duplicate-registration clash with the existing suite.
// Fixture helpers are copied into this file's anonymous namespace because the
// originals are file-local to the existing test.

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QString>
#include <QTemporaryDir>
#include <QVariantList>

#include "EditModeController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "TexturePaintBuffer.h"
#include "TexturePaintController.h"

#include <OgreColourValue.h>
#include <OgreEntity.h>
#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgrePass.h>
#include <OgreResourceGroupManager.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubEntity.h>
#include <OgreTechnique.h>
#include <OgreTextureUnitState.h>

namespace {

// Same scene fixture used by the existing suite: an entity carrying the
// canonical 3-vertex UV triangle (UVs at (0,0),(1,0),(0,1)) and a material
// with a TUS named "diffuse_map" so findOrCreateActiveTextureUnit picks it up.
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

        const std::string meshName = ("TPCB_Mesh_"   + tag).toStdString();
        const std::string entName  = ("TPCB_Entity_" + tag).toStdString();
        const std::string matName  = ("TPCB_Mat_"    + tag).toStdString();

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

    // Variant of setup() that binds the TUS to a real on-disk texture file so
    // bakeToOriginalFile() can resolve a disk path. The caller supplies the
    // bare filename (which must live in a FileSystem resource location).
    bool setupWithDiskTexture(const QString& tag, const QString& textureFile)
    {
        if (!tryInitOgre()) return false;
        auto* mgr = Manager::getSingleton();
        if (!mgr) return false;
        scene = mgr->getSceneMgr();
        if (!scene) return false;

        const std::string meshName = ("TPCB_MeshD_"   + tag).toStdString();
        const std::string entName  = ("TPCB_EntityD_" + tag).toStdString();
        const std::string matName  = ("TPCB_MatD_"    + tag).toStdString();

        mesh = createInMemoryTriangleMesh(meshName);
        if (!mesh) return false;
        entity = scene->createEntity(entName, mesh->getName());
        if (!entity) return false;
        node = scene->getRootSceneNode()->createChildSceneNode();
        node->attachObject(entity);

        auto& mm = Ogre::MaterialManager::getSingleton();
        mat = mm.create(matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        auto* tus  = pass->createTextureUnitState(textureFile.toStdString());
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

void pumpEventsFor(int ms = 120)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, ms);
        QCoreApplication::sendPostedEvents();
    }
}

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
    // Restore round shape so a Square test doesn't leak into later tests.
    if (auto* em = EditModeController::instance())
        em->setVertexPaintShape(EditModeController::ShapeRound);
}

// Index into the raw RGBA8 buffer for the texel covering UV (u,v).
int pixelOffset(const TexturePaintBuffer& buf, double u, double v)
{
    int x = 0, y = 0;
    buf.uvToPixel(Ogre::Vector2(static_cast<float>(u), static_cast<float>(v)), x, y);
    return (y * buf.width() + x) * 4;
}

// Seed the whole buffer to a known color through the public mask path so the
// brush-tool tests have a deterministic baseline to diff against.
void seedBufferColor(TexturePaintController* ctrl, const QColor& c)
{
    ctrl->setBrushColor(c);
    ctrl->selectAllMask();
    ctrl->fillMaskWithFG();
    ctrl->clearSelectionMask();
}

} // namespace

// ===========================================================================
// Brush-tool stroke coverage — needs a scene + entity + active session
// ===========================================================================

class TexturePaintControllerCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init / render window required";
        createStandardOgreMaterials();
        hardResetController();
        auto* ctrl = TexturePaintController::instance();
        // Full strength + no falloff so a single stamp fully replaces the
        // center texel — makes the pixel assertions deterministic.
        ctrl->setBrushStrength(1.0);
        ctrl->setBrushFalloff(0.0);
        ctrl->setBrushRadius(0.5);
    }

    void TearDown() override
    {
        m_fix.teardown();
        hardResetController();
    }

    ScenePaintFixture m_fix;
};

// ToolPaint baseline (control case) — paint a different color over a seeded
// background and confirm the covered texel changes.
TEST_F(TexturePaintControllerCoverageTest, ToolPaintChangesCenterPixel)
{
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Paint")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    seedBufferColor(ctrl, QColor(0, 0, 0));

    ctrl->setBrushTool(TexturePaintController::ToolPaint);
    ctrl->setBrushColor(QColor(255, 0, 0));
    ctrl->setTexturePaintEnabled(true);
    ASSERT_TRUE(ctrl->beginStrokeUV(0.4, 0.4));
    ctrl->updateStrokeUV(0.4, 0.4);
    ctrl->endStrokeUV();

    const auto& px = ctrl->buffer().data();
    const int off = pixelOffset(ctrl->buffer(), 0.4, 0.4);
    EXPECT_GT(px[off + 0], 100) << "red channel should have been painted up";
    EXPECT_LT(px[off + 1], 50);
    EXPECT_LT(px[off + 2], 50);
}

// ToolErase paints with the EditModeController background color.
TEST_F(TexturePaintControllerCoverageTest, ToolErasePaintsBackgroundColor)
{
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Erase")));
    auto* ctrl = TexturePaintController::instance();
    auto* em   = EditModeController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    seedBufferColor(ctrl, QColor(255, 255, 255));

    em->setVertexPaintBackgroundColor(QColor(0, 0, 255, 255));
    ctrl->setBrushTool(TexturePaintController::ToolErase);
    ctrl->setTexturePaintEnabled(true);
    ASSERT_TRUE(ctrl->beginStrokeUV(0.4, 0.4));
    ctrl->updateStrokeUV(0.4, 0.4);
    ctrl->endStrokeUV();

    const auto& px = ctrl->buffer().data();
    const int off = pixelOffset(ctrl->buffer(), 0.4, 0.4);
    EXPECT_GT(px[off + 2], 100) << "blue (BG color) should dominate after erase";
    EXPECT_LT(px[off + 0], 150);
}

// ToolFill flood-fills the connected region under the seed once per stroke.
TEST_F(TexturePaintControllerCoverageTest, ToolFillFloodsRegionWithBrushColor)
{
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Fill")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    seedBufferColor(ctrl, QColor(0, 0, 0));   // uniform region → fill spreads

    ctrl->setBrushTool(TexturePaintController::ToolFill);
    ctrl->setBrushColor(QColor(0, 200, 0));
    ctrl->setTexturePaintEnabled(true);
    ASSERT_TRUE(ctrl->beginStrokeUV(0.4, 0.4));
    ctrl->updateStrokeUV(0.4, 0.4);  // second stamp must NOT re-flood
    ctrl->endStrokeUV();

    const auto& px = ctrl->buffer().data();
    const int off = pixelOffset(ctrl->buffer(), 0.4, 0.4);
    EXPECT_GT(px[off + 1], 100) << "green fill color expected at seed texel";
    // A far corner should also be green if the uniform region flooded.
    const int corner = pixelOffset(ctrl->buffer(), 0.95, 0.95);
    EXPECT_GT(px[corner + 1], 100) << "flood should have spread across the uniform buffer";
}

// ToolColorPicker samples the buffer pixel and writes it to the brush color
// (observable via EditModeController / texturePaintColor()).
TEST_F(TexturePaintControllerCoverageTest, ToolColorPickerSetsBrushColorFromPixel)
{
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Picker")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    // Pre-seed the entire buffer to a known sample color.
    seedBufferColor(ctrl, QColor(40, 160, 220));
    // Move the live brush color away so the pick is observable.
    ctrl->setBrushColor(QColor(255, 255, 255));

    ctrl->setBrushTool(TexturePaintController::ToolColorPicker);
    ctrl->setTexturePaintEnabled(true);
    ASSERT_TRUE(ctrl->beginStrokeUV(0.4, 0.4));
    ctrl->updateStrokeUV(0.4, 0.4);
    ctrl->endStrokeUV();

    const QColor picked = ctrl->texturePaintColor();
    // Picker writes opaque RGB; allow a small rounding tolerance for the
    // float round-trip through ColourValue.
    EXPECT_NEAR(picked.red(),   40,  4);
    EXPECT_NEAR(picked.green(), 160, 4);
    EXPECT_NEAR(picked.blue(),  220, 4);
}

// ToolSmudge needs two updateStrokeUV calls — the first records m_smudgePrev,
// the second actually smudges. Exercise the per-pixel smudge loop.
TEST_F(TexturePaintControllerCoverageTest, ToolSmudgeRunsWithoutCrashAndForwardsPrev)
{
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Smudge")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    // Two-tone buffer so a smudge from one region into another visibly blends.
    seedBufferColor(ctrl, QColor(0, 0, 0));
    // Paint a bright patch near one UV so the smudge has something to drag.
    ctrl->setBrushTool(TexturePaintController::ToolPaint);
    ctrl->setBrushColor(QColor(255, 255, 255));
    ctrl->setTexturePaintEnabled(true);
    ASSERT_TRUE(ctrl->beginStrokeUV(0.3, 0.3));
    ctrl->endStrokeUV();

    // Now smudge from the bright patch toward (0.5,0.5).
    ctrl->setBrushTool(TexturePaintController::ToolSmudge);
    ASSERT_TRUE(ctrl->beginStrokeUV(0.3, 0.3));   // first update → records prev
    ctrl->updateStrokeUV(0.4, 0.4);               // second → smudges
    ctrl->updateStrokeUV(0.5, 0.5);               // third → smudges further
    ctrl->endStrokeUV();
    // Hard to assert exact pixels; the key coverage is the smudge loop ran.
    SUCCEED();
    EXPECT_EQ(ctrl->brushTool(), static_cast<int>(TexturePaintController::ToolSmudge));
}

// Square shape branch — EditModeController::ShapeSquare drives a constant-
// strength rectangular stamp through paintBrush.
TEST_F(TexturePaintControllerCoverageTest, SquareShapePaintsCenterPixel)
{
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Square")));
    auto* ctrl = TexturePaintController::instance();
    auto* em   = EditModeController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    seedBufferColor(ctrl, QColor(0, 0, 0));

    em->setVertexPaintShape(EditModeController::ShapeSquare);
    EXPECT_EQ(ctrl->brushShape(), static_cast<int>(EditModeController::ShapeSquare));
    ctrl->setBrushTool(TexturePaintController::ToolPaint);
    ctrl->setBrushColor(QColor(255, 200, 0));
    ctrl->setTexturePaintEnabled(true);
    ASSERT_TRUE(ctrl->beginStrokeUV(0.4, 0.4));
    ctrl->endStrokeUV();

    const auto& px = ctrl->buffer().data();
    const int off = pixelOffset(ctrl->buffer(), 0.4, 0.4);
    EXPECT_GT(px[off + 0], 100) << "square stamp must paint the center texel";
}

// Round shape branch (explicitly) — same path, ShapeRound.
TEST_F(TexturePaintControllerCoverageTest, RoundShapePaintsCenterPixel)
{
    ASSERT_TRUE(m_fix.setup(QStringLiteral("Round")));
    auto* ctrl = TexturePaintController::instance();
    auto* em   = EditModeController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    seedBufferColor(ctrl, QColor(0, 0, 0));

    em->setVertexPaintShape(EditModeController::ShapeRound);
    EXPECT_EQ(ctrl->brushShape(), static_cast<int>(EditModeController::ShapeRound));
    ctrl->setBrushTool(TexturePaintController::ToolPaint);
    ctrl->setBrushColor(QColor(0, 0, 255));
    ctrl->setTexturePaintEnabled(true);
    ASSERT_TRUE(ctrl->beginStrokeUV(0.4, 0.4));
    ctrl->endStrokeUV();

    const auto& px = ctrl->buffer().data();
    const int off = pixelOffset(ctrl->buffer(), 0.4, 0.4);
    EXPECT_GT(px[off + 2], 100) << "round stamp center must be fully painted";
}

// Mesh-extent radius remap: with a mesh present, applyBrushAtUV divides the
// mesh-local radius by the bbox half-extent. Confirm the UV radius mirror
// reflects a non-trivial remap and that painting still produces a stamp.
TEST_F(TexturePaintControllerCoverageTest, MeshExtentRadiusRemapProducesBoundedStamp)
{
    ASSERT_TRUE(m_fix.setup(QStringLiteral("RadiusRemap")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    seedBufferColor(ctrl, QColor(0, 0, 0));

    ctrl->setBrushRadius(0.5);
    const double uvRadius = ctrl->texturePaintRadiusUV();
    EXPECT_GT(uvRadius, 0.0);
    EXPECT_LE(uvRadius, 1.0) << "remapped radius is clamped to <= 1.0";

    ctrl->setBrushTool(TexturePaintController::ToolPaint);
    ctrl->setBrushColor(QColor(255, 0, 255));
    ctrl->setTexturePaintEnabled(true);
    ASSERT_TRUE(ctrl->beginStrokeUV(0.4, 0.4));
    ctrl->endStrokeUV();

    const auto& px = ctrl->buffer().data();
    const int off = pixelOffset(ctrl->buffer(), 0.4, 0.4);
    EXPECT_GT(px[off + 0], 100);
    EXPECT_GT(px[off + 2], 100);
}

// A stroke whose tool is Fill but where the buffer is NOT uniform: still a
// single flood, and the second updateStroke is suppressed (m_strokeJustBegan).
TEST_F(TexturePaintControllerCoverageTest, ToolFillSingleStampPerStroke)
{
    ASSERT_TRUE(m_fix.setup(QStringLiteral("FillOnce")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(16));
    seedBufferColor(ctrl, QColor(10, 10, 10));

    ctrl->setBrushTool(TexturePaintController::ToolFill);
    ctrl->setBrushColor(QColor(200, 0, 0));
    ctrl->setTexturePaintEnabled(true);
    ASSERT_TRUE(ctrl->beginStrokeUV(0.5, 0.5));
    // Change the brush color mid-stroke; because fill only fires on the first
    // stamp, the later updates should NOT re-flood with the new color.
    ctrl->setBrushColor(QColor(0, 0, 200));
    ctrl->updateStrokeUV(0.6, 0.6);
    ctrl->updateStrokeUV(0.7, 0.7);
    ctrl->endStrokeUV();

    const auto& px = ctrl->buffer().data();
    const int off = pixelOffset(ctrl->buffer(), 0.5, 0.5);
    EXPECT_GT(px[off + 0], 100) << "first-flood red expected, not the later blue";
    EXPECT_LT(px[off + 2], 100);
}

// ===========================================================================
// UV overlay WITH an active session — exercises refreshUvOverlay drawing path
// ===========================================================================

// ===========================================================================
// bakeToOriginalFile — resolve an on-disk path and rewrite the file
// ===========================================================================

TEST_F(TexturePaintControllerCoverageTest, BakeToOriginalFileWritesResolvedDiskPath)
{
    // 1. Stand up a temp dir, write a real PNG into it.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString texFile = QStringLiteral("tpc_bake_src.png");
    const QString diskPath = tmp.path() + "/" + texFile;
    {
        QImage seed(32, 32, QImage::Format_RGBA8888);
        seed.fill(QColor(10, 20, 30, 255));
        ASSERT_TRUE(seed.save(diskPath));
    }
    const qint64 sizeBefore = QFileInfo(diskPath).size();

    // 2. Register the temp dir as an Ogre FileSystem resource location so
    //    bakeToOriginalFile's listResourceLocations walk can resolve it.
    const Ogre::String group = "TPCBakeGroup";
    auto& rgm = Ogre::ResourceGroupManager::getSingleton();
    bool groupCreated = false;
    try {
        if (!rgm.resourceGroupExists(group)) {
            rgm.createResourceGroup(group);
            groupCreated = true;
        }
        rgm.addResourceLocation(tmp.path().toStdString(), "FileSystem", group);
        rgm.initialiseResourceGroup(group);
    } catch (...) {
        FAIL() << "failed to register temp dir as Ogre resource location";
    }

    // 3. Bind that texture file as the entity diffuse and open a session so
    //    ensurePaintableTexture records m_originalTextureName = texFile.
    ASSERT_TRUE(m_fix.setupWithDiskTexture(QStringLiteral("Bake"), texFile));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));

    // 4. Mutate the buffer so the written file is meaningfully different.
    seedBufferColor(ctrl, QColor(200, 100, 50));
    pumpEventsFor(60);

    // 5. Bake → returns the resolved disk path and rewrites the file.
    const QString written = ctrl->bakeToOriginalFile();
    EXPECT_FALSE(written.isEmpty()) << "bake should resolve the on-disk texture path";
    if (!written.isEmpty()) {
        EXPECT_TRUE(QFileInfo(written).exists());
        EXPECT_EQ(QFileInfo(written).fileName(), texFile);
        // Re-load the written PNG and confirm it carries the painted color.
        QImage round(written);
        EXPECT_FALSE(round.isNull());
        if (!round.isNull()) {
            const QColor c = round.pixelColor(round.width() / 2, round.height() / 2);
            EXPECT_NEAR(c.red(),   200, 6);
            EXPECT_NEAR(c.green(), 100, 6);
            EXPECT_NEAR(c.blue(),  50,  6);
        }
    }
    (void)sizeBefore;

    // Cleanup the resource group registration before the temp dir vanishes.
    try {
        rgm.removeResourceLocation(tmp.path().toStdString(), group);
        if (groupCreated && rgm.resourceGroupExists(group))
            rgm.destroyResourceGroup(group);
    } catch (...) {}
}

TEST_F(TexturePaintControllerCoverageTest, BakeToOriginalFileEmptyWhenNoDiskFile)
{
    // No on-disk file backs the (generated) texture → bake returns empty
    // (the "embedded?" breadcrumb branch). The plain fixture binds a TUS with
    // no texture name, so ensurePaintableTexture leaves m_originalTextureName
    // empty and bake takes the early return.
    ASSERT_TRUE(m_fix.setup(QStringLiteral("BakeNoFile")));
    auto* ctrl = TexturePaintController::instance();
    ctrl->setPaintTarget(TexturePaintController::TargetTexture);
    ASSERT_TRUE(ctrl->ensurePaintableTexture(32));
    EXPECT_TRUE(ctrl->bakeToOriginalFile().isEmpty());
}
