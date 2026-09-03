/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — BoneWeightOverlay runtime tests (Skel Slice D, issue #558)

These cover the parts that only misbehave in a LIVE session and that the
material/config-level tests in BoneWeightOverlay_test.cpp cannot reach:

 - the per-vertex dots are emitted by the update TIMER, not by the enable call,
   so a toggle-only assertion can pass while nothing ever renders;
 - draw ORDER between the mesh, heat map, dots and skeleton (sharing a render
   queue made the dots invisible even though they were emitted and attached);
 - the real AnimationWidget parenting (a parentless widget is a test artifact);
 - the paint-mode <-> heat-map binding, including that it does not recurse;
 - the reported crash from re-enabling paint after disabling the weights.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>
#include "AnimationWidget.h"
#include "BoneWeightOverlay.h"
#include "SkinWeightController.h"
#include "SelectionSet.h"
#include "Manager.h"
#include "TestHelpers.h"
#include <QApplication>
#include <QThread>
#include <OgreEntity.h>
#include <OgreBone.h>
#include <OgreSkeletonInstance.h>
#include <OgreSceneManager.h>
#include <OgreManualObject.h>
#include <OgreMaterialManager.h>
#include <OgreTechnique.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreRoot.h>
#include <OgreRenderSystem.h>
#include <QWidget>

class BoneWeightOverlayRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre());
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
    }
    void TearDown() override {
        SkinWeightController::instance()->setWeightPaintEnabled(false);
        if (SelectionSet::getSingletonPtr()) SelectionSet::getSingleton()->clear();
        Manager::kill();
    }
};

// Replicate the REAL sequence: select mesh, show weights, enter paint mode,
// then let the update timer tick — which is what actually emits the dots.
// The dots are emitted by the update TIMER, not by the enable call, so a test
// that only checks the toggle can pass while nothing ever renders. Drive the
// timer the way the live app does.
TEST_F(BoneWeightOverlayRuntimeTest, TimerTickActuallyEmitsDots) {
    auto* e = createAnimatedTestEntity("RtDots");
    ASSERT_NE(e, nullptr);

    AnimationWidget w(nullptr);
    SelectionSet::getSingleton()->selectOne(e);
    ASSERT_TRUE(w.toggleBoneWeights(e, true));
    auto* ov = w.getBoneWeightOverlay(e);
    ASSERT_NE(ov, nullptr);

    SkinWeightController::instance()->setWeightPaintEnabled(true);

    // Let the 0ms update timer fire, as it does in the live app.
    for (int i = 0; i < 5; ++i) { QApplication::processEvents(); QThread::msleep(20); }

    auto* sm = Manager::getSingleton()->getSceneMgr();
    const std::string n = sm->getName() + "/BoneWeightVertices/" + e->getName();
    EXPECT_TRUE(sm->hasManualObject(n));
}

// The REAL app parents AnimationWidget under MainWindow. Reproduce that exact
// topology: a parentless widget is only reachable via qobject_cast on the
// top-level list, so testing solely against one would hide a lookup that fails
// for the shipped parenting.
TEST_F(BoneWeightOverlayRuntimeTest, ParentedWidgetLikeTheRealApp) {
    auto* e = createAnimatedTestEntity("RtParented");
    ASSERT_NE(e, nullptr);

    QWidget host;                       // stands in for MainWindow
    auto* w = new AnimationWidget(&host);
    host.show();
    QApplication::processEvents();

    SelectionSet::getSingleton()->selectOne(e);
    ASSERT_TRUE(w->toggleBoneWeights(e, true));
    SkinWeightController::instance()->setWeightPaintEnabled(true);

    auto* ov = w->getBoneWeightOverlay(e);
    ASSERT_NE(ov, nullptr);

    for (int i = 0; i < 5; ++i) { QApplication::processEvents(); QThread::msleep(20); }

    auto* sm = Manager::getSingleton()->getSceneMgr();
    const std::string n = sm->getName() + "/BoneWeightVertices/" + e->getName();
    EXPECT_TRUE(ov->showVertices());
    EXPECT_TRUE(sm->hasManualObject(n));
}

// Draw ORDER is what made the dots invisible the first time: they were emitted,
// attached and inside a valid bounding box (all verified), but shared render
// queue MAIN+1 with the translucent heat map, leaving the order between them
// undefined — and the alpha triangles frequently drew last, over the points.
// Pin the full stack so a future queue change cannot silently re-hide them.
TEST_F(BoneWeightOverlayRuntimeTest, OverlayDrawOrderIsMeshHeatMapDotsSkeleton) {
    auto* e = createAnimatedTestEntity("RtOrder");
    ASSERT_NE(e, nullptr);

    QWidget host;
    auto* w = new AnimationWidget(&host);
    host.show();
    QApplication::processEvents();

    SelectionSet::getSingleton()->selectOne(e);
    ASSERT_TRUE(w->toggleBoneWeights(e, true));
    SkinWeightController::instance()->setWeightPaintEnabled(true);
    for (int i = 0; i < 5; ++i) { QApplication::processEvents(); QThread::msleep(20); }

    auto* sm = Manager::getSingleton()->getSceneMgr();
    const std::string heatName = std::string("BoneWeightOverlay_") + e->getName();
    const std::string dotName  = sm->getName() + "/BoneWeightVertices/" + e->getName();
    ASSERT_TRUE(sm->hasManualObject(dotName)) << "dots must exist to be ordered";

    auto* dots = sm->getManualObject(dotName);
    EXPECT_GT(dots->getRenderQueueGroup(), Ogre::RENDER_QUEUE_MAIN)
        << "dots must draw after the opaque mesh or it covers them";

    // ASSERT, not if(): guessing this name wrong once made the comparison skip
    // silently while the test still reported green.
    ASSERT_TRUE(sm->hasManualObject(heatName)) << "heat map object name drifted";
    auto* heat = sm->getManualObject(heatName);
    EXPECT_GT(dots->getRenderQueueGroup(), heat->getRenderQueueGroup())
        << "dots must draw AFTER the translucent heat map, not tie with it";
}

// The dots must carry the HEAT-MAP colour with a dark halo behind them for
// contrast. Two failure modes matter: only one section emitted (no border), and
// the colour lookup silently falling back to white (dots not weight-coloured).
TEST_F(BoneWeightOverlayRuntimeTest, DotsAreHeatMapColouredWithAHaloSection) {
    auto* e = createAnimatedTestEntity("RtHalo");
    ASSERT_NE(e, nullptr);

    QWidget host;
    auto* w = new AnimationWidget(&host);
    host.show();
    QApplication::processEvents();

    SelectionSet::getSingleton()->selectOne(e);
    ASSERT_TRUE(w->toggleBoneWeights(e, true));
    // Select the bone that carries the weights; the overlay defaults to handle
    // 0, where NOTHING is connected — so the halo section would be empty and
    // skipped entirely, which is what this test previously mismeasured.
    w->getBoneWeightOverlay(e)->setSelectedBone(1);
    SkinWeightController::instance()->setWeightPaintEnabled(true);
    for (int i = 0; i < 5; ++i) { QApplication::processEvents(); QThread::msleep(20); }

    auto* sm = Manager::getSingleton()->getSceneMgr();
    auto* dots = sm->getManualObject(
        sm->getName() + "/BoneWeightVertices/" + e->getName());
    ASSERT_NE(dots, nullptr);

    // Bone 1 carries every vertex in this fixture, so there are no unconnected
    // verts and the plain section is skipped: halo + coloured fill only.
    ASSERT_EQ(dots->getNumSections(), 2u)
        << "all verts connected -> one halo section + one coloured fill";
    EXPECT_EQ(dots->getSection(0)->getMaterialName(),
              "BoneWeightOverlay/VertexHaloMaterial");
    EXPECT_EQ(dots->getSection(1)->getMaterialName(),
              "BoneWeightOverlay/VertexMaterial");

    auto& mm = Ogre::MaterialManager::getSingleton();
    auto halo = mm.getByName("BoneWeightOverlay/VertexHaloMaterial");
    auto fill = mm.getByName("BoneWeightOverlay/VertexMaterial");
    ASSERT_TRUE(halo);
    ASSERT_TRUE(fill);
    auto* hp = halo->getTechnique(0)->getPass(0);
    auto* fp = fill->getTechnique(0)->getPass(0);

    EXPECT_GT(hp->getPointSize(), fp->getPointSize())
        << "the halo must be LARGER or it forms no visible border";
    EXPECT_EQ(fp->getVertexColourTracking(), Ogre::TVC_DIFFUSE)
        << "the fill must take the per-vertex heat-map colour";
    EXPECT_EQ(hp->getVertexColourTracking(), Ogre::TVC_NONE)
        << "the halo must stay dark regardless of weight";
    EXPECT_LT(hp->getDiffuse().r, 0.2f) << "halo must be dark for contrast";
}

// Config alone is not enough: the fill section could still emit WHITE for every
// dot if the per-section colour lookup misses (mSectionColours indexed by the
// wrong section, an empty cache, etc.). Read the actual vertex colours back out
// of the buffer and require them to match the heat-map ramp.
TEST_F(BoneWeightOverlayRuntimeTest, EmittedDotColoursMatchTheHeatMapRamp) {
    auto* e = createAnimatedTestEntity("RtColours");
    ASSERT_NE(e, nullptr);
    Ogre::MeshPtr mesh = e->getMesh();
    ASSERT_TRUE(mesh);

    // Give vertex 0 a distinctive mid weight so both "all white" (lookup not
    // wired) and "all red" (stale cache) fail; the fixture weights everything
    // 1.0 to bone handle 1.
    const auto saved = mesh->getBoneAssignments();
    mesh->clearBoneAssignments();
    for (const auto& kv : saved) {
        Ogre::VertexBoneAssignment vba = kv.second;
        if (vba.vertexIndex == 0 && vba.boneIndex == 1) vba.weight = 0.5f;
        mesh->addBoneAssignment(vba);
    }

    QWidget host;
    auto* w = new AnimationWidget(&host);
    host.show();
    QApplication::processEvents();

    SelectionSet::getSingleton()->selectOne(e);
    ASSERT_TRUE(w->toggleBoneWeights(e, true));
    // Point the overlay at the bone that actually carries weights. It defaults
    // to handle 0, where every weight is zero — leaving that unset made an
    // earlier version of this test read colours for the wrong bone entirely.
    w->getBoneWeightOverlay(e)->setSelectedBone(1);
    SkinWeightController::instance()->setWeightPaintEnabled(true);
    for (int i = 0; i < 5; ++i) { QApplication::processEvents(); QThread::msleep(20); }

    auto* sm = Manager::getSingleton()->getSceneMgr();
    auto* dots = sm->getManualObject(
        sm->getName() + "/BoneWeightVertices/" + e->getName());
    ASSERT_NE(dots, nullptr);
    ASSERT_EQ(dots->getNumSections(), 2u);

    // Section 1 is the coloured fill.
    Ogre::RenderOperation ro;
    dots->getSection(1)->getRenderOperation(ro);
    Ogre::VertexData* vd = ro.vertexData;
    ASSERT_NE(vd, nullptr);
    const auto* colElem =
        vd->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE);
    ASSERT_NE(colElem, nullptr) << "the fill section must carry vertex colours";

    // Compare against the SAME packing the writer used rather than hand-decoding
    // a byte order.
    const Ogre::VertexElementType colType = colElem->getType();
    auto pack = [colType](const Ogre::ColourValue& c) {
        return Ogre::VertexElement::convertColourValue(c, colType);
    };
    const Ogre::uint32 whitePacked = pack(Ogre::ColourValue::White);
    Ogre::ColourValue half = BoneWeightOverlay::weightToColor(0.5f);
    half.a = 1.0f;
    const Ogre::uint32 halfPacked = pack(half);

    auto vbuf = vd->vertexBufferBinding->getBuffer(colElem->getSource());
    auto* base = static_cast<unsigned char*>(
        vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

    bool sawNonWhite = false, sawHalfWeight = false;
    for (size_t i = 0; i < vd->vertexCount; ++i) {
        Ogre::uint32* word = nullptr;
        colElem->baseVertexPointerToElement(base + i * vbuf->getVertexSize(), &word);
        if (*word != whitePacked) sawNonWhite = true;
        if (*word == halfPacked)  sawHalfWeight = true;
    }
    vbuf->unlock();

    EXPECT_TRUE(sawNonWhite)
        << "every dot came out white — the heat-map colour lookup is not wired";
    EXPECT_TRUE(sawHalfWeight)
        << "no dot carries the 0.5-weight ramp colour set up above";
}

// Only vertices actually weighted to the selected bone get a halo. Vertices
// with no assignment to it are drawn once, flat, with NO halo — both to save
// the extra point and so a sea of haloed dots elsewhere does not compete with
// the bone being painted.
TEST_F(BoneWeightOverlayRuntimeTest, UnconnectedVerticesGetAPlainDotWithNoHalo) {
    auto* e = createAnimatedTestEntity("RtPlain");
    ASSERT_NE(e, nullptr);
    Ogre::MeshPtr mesh = e->getMesh();
    ASSERT_TRUE(mesh);

    // Strip vertex 2's assignment so it is UNCONNECTED to bone 1, leaving
    // vertices 0 and 1 connected. A mesh where every vertex is connected (the
    // fixture default) could not distinguish the two paths at all.
    const auto saved = mesh->getBoneAssignments();
    mesh->clearBoneAssignments();
    for (const auto& kv : saved) {
        if (kv.second.vertexIndex == 2) continue;
        mesh->addBoneAssignment(kv.second);
    }

    QWidget host;
    auto* w = new AnimationWidget(&host);
    host.show();
    QApplication::processEvents();

    SelectionSet::getSingleton()->selectOne(e);
    ASSERT_TRUE(w->toggleBoneWeights(e, true));
    w->getBoneWeightOverlay(e)->setSelectedBone(1);
    SkinWeightController::instance()->setWeightPaintEnabled(true);
    for (int i = 0; i < 5; ++i) { QApplication::processEvents(); QThread::msleep(20); }

    auto* sm = Manager::getSingleton()->getSceneMgr();
    auto* dots = sm->getManualObject(
        sm->getName() + "/BoneWeightVertices/" + e->getName());
    ASSERT_NE(dots, nullptr);

    ASSERT_EQ(dots->getNumSections(), 3u)
        << "plain + halo + fill once some verts are unconnected";
    EXPECT_EQ(dots->getSection(0)->getMaterialName(),
              "BoneWeightOverlay/VertexPlainMaterial")
        << "unconnected dots must draw FIRST, under the painted bone's dots";

    // One unconnected vertex, two connected.
    Ogre::RenderOperation plainOp, haloOp;
    dots->getSection(0)->getRenderOperation(plainOp);
    dots->getSection(1)->getRenderOperation(haloOp);
    EXPECT_EQ(plainOp.vertexData->vertexCount, 1u);
    EXPECT_EQ(haloOp.vertexData->vertexCount, 2u)
        << "the halo section must cover ONLY the connected vertices";

    auto& mm = Ogre::MaterialManager::getSingleton();
    auto plain = mm.getByName("BoneWeightOverlay/VertexPlainMaterial");
    ASSERT_TRUE(plain);
    auto* pp = plain->getTechnique(0)->getPass(0);
    EXPECT_EQ(pp->getVertexColourTracking(), Ogre::TVC_NONE)
        << "unconnected dots are a single flat colour";
    EXPECT_LT(pp->getPointSize(),
              mm.getByName("BoneWeightOverlay/VertexMaterial")
                ->getTechnique(0)->getPass(0)->getPointSize())
        << "unconnected dots stay smaller than the painted bone's dots";
}

// The fix for "cannot subtract once a vertex reaches 1.0" needs a recipient
// bone, and the controller supplies the ACTIVE bone's PARENT. Verify that
// end-to-end against a real skeleton: the fixture is Root(0) -> Child(1), so
// painting Child must nominate Root. A pure-data test cannot cover this — it is
// the skeleton walk that decides.
TEST_F(BoneWeightOverlayRuntimeTest, PaintingAFullyWeightedVertexCanBeSubtracted) {
    auto* e = createAnimatedTestEntity("RtSubtract");
    ASSERT_NE(e, nullptr);
    Ogre::MeshPtr mesh = e->getMesh();
    ASSERT_TRUE(mesh);

    // Fixture weights every vertex 1.0 to Child (handle 1) — precisely the
    // "sole influence at full weight" state that used to be unpaintable.
    for (const auto& kv : mesh->getBoneAssignments()) {
        EXPECT_EQ(kv.second.boneIndex, 1);
        EXPECT_NEAR(kv.second.weight, 1.0f, 1e-6f);
    }

    QWidget host;
    auto* w = new AnimationWidget(&host);
    host.show();
    QApplication::processEvents();
    SelectionSet::getSingleton()->selectOne(e);
    ASSERT_TRUE(w->toggleBoneWeights(e, true));

    // Mark Child selected so the controller treats it as the active bone.
    auto* skel = e->getSkeleton();
    ASSERT_NE(skel, nullptr);
    skel->getBone("Child")->getUserObjectBindings().setUserAny("selected", true);

    auto* c = SkinWeightController::instance();
    c->setWeightPaintEnabled(true);
    ASSERT_TRUE(c->weightPaintEnabled());

    // Drop vertex 0's Child weight numerically — the same write path the brush
    // uses, without needing a viewport ray.
    ASSERT_TRUE(c->setVertexWeight(0, QStringLiteral("Child"), 0.4))
        << "a fully-weighted vertex must accept a lower weight; status="
        << c->status().toStdString();

    const QStringList after = c->vertexWeights(0);
    ASSERT_FALSE(after.isEmpty());
    bool childReduced = false, rootAbsorbed = false;
    for (const QString& row : after) {
        if (row.startsWith("Child=") && row.mid(6).toDouble() < 0.99) childReduced = true;
        if (row.startsWith("Root=")  && row.mid(5).toDouble() > 0.01) rootAbsorbed = true;
    }
    EXPECT_TRUE(childReduced)
        << "Child stayed pinned at full weight: " << after.join(", ").toStdString();
    EXPECT_TRUE(rootAbsorbed)
        << "the parent bone should absorb the freed weight: "
        << after.join(", ").toStdString();
}

// --- weight paint and the heat map are bound together --------------------

// Painting without the heat map gives no feedback, so entering paint mode must
// turn the overlay on by itself.
TEST_F(BoneWeightOverlayRuntimeTest, EnablingPaintTurnsTheHeatMapOn) {
    auto* e = createAnimatedTestEntity("RtBindOn");
    ASSERT_NE(e, nullptr);

    QWidget host;
    auto* w = new AnimationWidget(&host);
    host.show();
    QApplication::processEvents();

    SelectionSet::getSingleton()->selectOne(e);
    ASSERT_FALSE(w->isBoneWeightsShown(e)) << "precondition: overlay off";

    SkinWeightController::instance()->setWeightPaintEnabled(true);
    EXPECT_TRUE(w->isBoneWeightsShown(e))
        << "entering paint mode must show the heat map";
    EXPECT_TRUE(SkinWeightController::instance()->weightPaintEnabled());
}

// And the inverse: hiding the heat map must leave paint mode, or the user is
// editing weights blind.
TEST_F(BoneWeightOverlayRuntimeTest, DisablingTheHeatMapLeavesPaintMode) {
    auto* e = createAnimatedTestEntity("RtBindOff");
    ASSERT_NE(e, nullptr);

    QWidget host;
    auto* w = new AnimationWidget(&host);
    host.show();
    QApplication::processEvents();

    SelectionSet::getSingleton()->selectOne(e);
    SkinWeightController::instance()->setWeightPaintEnabled(true);
    ASSERT_TRUE(w->isBoneWeightsShown(e));
    ASSERT_TRUE(SkinWeightController::instance()->weightPaintEnabled());

    w->toggleBoneWeights(e, false);
    EXPECT_FALSE(SkinWeightController::instance()->weightPaintEnabled())
        << "hiding the heat map must exit paint mode";
    EXPECT_FALSE(w->isBoneWeightsShown(e));
}

// The two toggles now call INTO each other, so an unguarded pair would recurse.
// Drive both directions repeatedly: this deadlocks or overflows the stack if
// the mutual calls are not broken.
TEST_F(BoneWeightOverlayRuntimeTest, BindingTheTogglesDoesNotRecurse) {
    auto* e = createAnimatedTestEntity("RtBindCycle");
    ASSERT_NE(e, nullptr);

    QWidget host;
    auto* w = new AnimationWidget(&host);
    host.show();
    QApplication::processEvents();
    SelectionSet::getSingleton()->selectOne(e);

    auto* c = SkinWeightController::instance();
    for (int i = 0; i < 5; ++i) {
        c->setWeightPaintEnabled(true);
        EXPECT_TRUE(w->isBoneWeightsShown(e));
        w->toggleBoneWeights(e, false);
        EXPECT_FALSE(c->weightPaintEnabled());
    }

    // Toggling the overlay on by itself must NOT drag paint mode along: the
    // heat map is useful on its own for inspecting weights.
    w->toggleBoneWeights(e, true);
    EXPECT_TRUE(w->isBoneWeightsShown(e));
    EXPECT_FALSE(c->weightPaintEnabled())
        << "showing the heat map alone must not enter paint mode";
}

// USER-REPORTED CRASH: enable paint AFTER having disabled weights.
//
// Disabling weights destroys the BoneWeightOverlay and (by the new binding)
// exits paint mode. Re-entering paint must then rebuild the overlay from
// scratch — with no stale pointer to the destroyed one.
TEST_F(BoneWeightOverlayRuntimeTest, ReEnablingPaintAfterDisablingWeightsDoesNotCrash) {
    auto* e = createAnimatedTestEntity("RtReEnable");
    ASSERT_NE(e, nullptr);

    QWidget host;
    auto* w = new AnimationWidget(&host);
    host.show();
    QApplication::processEvents();
    SelectionSet::getSingleton()->selectOne(e);

    auto* c = SkinWeightController::instance();

    // 1. paint on (also shows the heat map)
    c->setWeightPaintEnabled(true);
    ASSERT_TRUE(w->isBoneWeightsShown(e));
    for (int i = 0; i < 3; ++i) { QApplication::processEvents(); QThread::msleep(20); }

    // 2. user disables the weights overlay -> paint mode exits, overlay deleted
    w->toggleBoneWeights(e, false);
    ASSERT_FALSE(c->weightPaintEnabled());
    ASSERT_FALSE(w->isBoneWeightsShown(e));
    for (int i = 0; i < 3; ++i) { QApplication::processEvents(); QThread::msleep(20); }

    // 3. user enables paint again — this is where it crashed
    c->setWeightPaintEnabled(true);
    for (int i = 0; i < 5; ++i) { QApplication::processEvents(); QThread::msleep(20); }

    EXPECT_TRUE(c->weightPaintEnabled());
    EXPECT_TRUE(w->isBoneWeightsShown(e)) << "the overlay must be rebuilt";
    SUCCEED();
}
