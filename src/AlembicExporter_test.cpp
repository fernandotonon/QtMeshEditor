#include <gtest/gtest.h>

#include "AlembicExporter.h"
#include "AlembicImporter.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

// Without ENABLE_ALEMBIC the exporter must report itself unavailable and fail
// soft (no crash, a "rebuild with" message) — the contract every surface relies on.
#ifndef ENABLE_ALEMBIC

TEST(AlembicExporterStandalone, UnavailableWithoutFlag) {
    EXPECT_FALSE(AlembicExporter::available());
    AlembicExporter::Scene scene;
    auto wr = AlembicExporter::write("/nonexistent/out.abc", scene);
    EXPECT_FALSE(wr.ok);
    EXPECT_TRUE(wr.error.contains("ENABLE_ALEMBIC"));
    auto er = AlembicExporter::exportEntities({}, "/nonexistent/out.abc");
    EXPECT_FALSE(er.ok);
    EXPECT_TRUE(er.error.contains("ENABLE_ALEMBIC"));
}

#else  // ENABLE_ALEMBIC

#include "NodeAnimationManager.h"
#include "TestHelpers.h"

#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>

#include <OgreAnimationState.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

#include <cmath>

namespace {

using AlembicExporter::Scene;
using AlembicExporter::Shape;

// A unit quad (one 4-gon) whose Y goes 0 → 1 over `frames` frames.
Shape quadShape(int frames, const QString& name = QStringLiteral("quad"))
{
    Shape s;
    s.xformName = name + QStringLiteral("_xf");
    s.meshName = name;
    s.faceCounts = {4};
    s.faceIndices = {0, 1, 2, 3};
    s.uvs = {0, 0, 1, 0, 1, 1, 0, 1};
    for (int f = 0; f < frames; ++f) {
        const float y = frames > 1 ? static_cast<float>(f) / (frames - 1) : 0.0f;
        s.framePositions.push_back({0, y, 0, 1, y, 0, 1, y, 1, 0, y, 1});
    }
    return s;
}

std::array<double, 16> translation(double x, double y, double z)
{
    std::array<double, 16> m{};
    m[0] = m[5] = m[10] = m[15] = 1.0;
    m[12] = x; m[13] = y; m[14] = z;     // Imath layout: row 3 holds translation
    return m;
}

}  // namespace

TEST(AlembicExporterWrite, Available) {
    EXPECT_TRUE(AlembicExporter::available());
}

TEST(AlembicExporterWrite, AnimatedCacheRoundTripsThroughTheImporter) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.filePath("anim.abc");

    Scene scene;
    scene.fps = 24.0;
    scene.shapes.push_back(quadShape(5));
    auto wr = AlembicExporter::write(path, scene);
    ASSERT_TRUE(wr.ok) << wr.error.toStdString();
    EXPECT_EQ(wr.frameCount, 5);
    EXPECT_EQ(wr.shapeCount, 1);

    auto rr = AlembicImporter::readFrameSet(path);
    ASSERT_TRUE(rr.ok) << rr.error.toStdString();
    EXPECT_EQ(rr.frames.vertexCount, 4);
    ASSERT_EQ(rr.frames.frames.size(), 5u);
    EXPECT_EQ(rr.frames.fps, 24);
    for (int f = 0; f < 5; ++f) {
        const auto& p = rr.frames.frames[static_cast<size_t>(f)].positions;
        ASSERT_EQ(p.size(), 12u);
        for (int v = 0; v < 4; ++v)
            EXPECT_NEAR(p[static_cast<size_t>(v * 3 + 1)], f / 4.0f, 1e-6f);
        EXPECT_NEAR(rr.frames.frames[static_cast<size_t>(f)].time, f / 24.0f, 1e-5f);
    }

    auto info = AlembicImporter::readInfo(path);
    ASSERT_TRUE(info.ok) << info.error.toStdString();
    EXPECT_EQ(info.faceCount, 2);   // one quad = two triangles
}

// Alembic stores faces clockwise, Ogre counter-clockwise. A round trip through
// our own importer cannot catch a winding bug (both sides would flip alike), so
// read the raw indices: Blender showed inward normals before this was pinned.
TEST(AlembicExporterWrite, FacesAreWrittenInAlembicWinding) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.filePath("wind.abc");

    Scene scene;
    scene.shapes.push_back(quadShape(1));   // Ogre-order corners 0,1,2,3
    ASSERT_TRUE(AlembicExporter::write(path, scene).ok);

    Alembic::AbcCoreFactory::IFactory factory;
    Alembic::Abc::IArchive archive = factory.getArchive(path.toStdString());
    Alembic::AbcGeom::IPolyMesh mesh(
        Alembic::Abc::IObject(Alembic::Abc::IObject(archive.getTop(), "quad_xf"), "quad"),
        Alembic::Abc::kWrapExisting);
    Alembic::AbcGeom::IPolyMeshSchema::Sample smp;
    mesh.getSchema().get(smp);
    auto idx = smp.getFaceIndices();
    ASSERT_EQ(idx->size(), 4u);
    const int32_t expected[4] = {3, 2, 1, 0};
    for (size_t i = 0; i < 4; ++i) EXPECT_EQ((*idx)[i], expected[i]);
}

TEST(AlembicExporterWrite, XformIsWrittenPerFrameInWorldSpace) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath("xf.abc");

    Shape s = quadShape(3);
    s.frameXforms = {translation(0, 0, 0), translation(1, 2, 3), translation(2, 4, 6)};
    Scene scene;
    scene.shapes.push_back(s);
    ASSERT_TRUE(AlembicExporter::write(path, scene).ok);

    Alembic::AbcCoreFactory::IFactory factory;
    Alembic::AbcGeom::IArchive ar = factory.getArchive(path.toStdString());
    ASSERT_TRUE(ar.valid());
    Alembic::AbcGeom::IXform xf(ar.getTop().getChild(0), Alembic::AbcGeom::kWrapExisting);
    ASSERT_TRUE(xf.valid());
    ASSERT_EQ(xf.getSchema().getNumSamples(), 3u);
    Alembic::AbcGeom::XformSample xs;
    xf.getSchema().get(xs, Alembic::AbcGeom::ISampleSelector(Alembic::Abc::index_t(2)));
    const Imath::M44d m = xs.getMatrix();
    EXPECT_DOUBLE_EQ(m[3][0], 2.0);
    EXPECT_DOUBLE_EQ(m[3][1], 4.0);
    EXPECT_DOUBLE_EQ(m[3][2], 6.0);
}

TEST(AlembicExporterWrite, StaticShapeInAnimatedSceneKeepsOneSample) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath("mixed.abc");

    Scene scene;
    scene.shapes.push_back(quadShape(4, "moving"));
    scene.shapes.push_back(quadShape(4, "still"));
    // Make "still" identical on every frame — it must collapse to one sample.
    auto& still = scene.shapes.back();
    for (auto& f : still.framePositions) f = still.framePositions.front();
    ASSERT_TRUE(AlembicExporter::write(path, scene).ok);

    Alembic::AbcCoreFactory::IFactory factory;
    Alembic::AbcGeom::IArchive ar = factory.getArchive(path.toStdString());
    ASSERT_EQ(ar.getTop().getNumChildren(), 2u);
    size_t counts[2] = {0, 0};
    for (size_t i = 0; i < 2; ++i) {
        Alembic::AbcGeom::IObject xf = ar.getTop().getChild(i);
        Alembic::AbcGeom::IPolyMesh pm(xf.getChild(0), Alembic::AbcGeom::kWrapExisting);
        ASSERT_TRUE(pm.valid());
        counts[xf.getName() == "moving_xf" ? 0 : 1] = pm.getSchema().getNumSamples();
    }
    EXPECT_EQ(counts[0], 4u);
    EXPECT_EQ(counts[1], 1u);
}

TEST(AlembicExporterWrite, RejectsBadInputAndLeavesNoFile) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath("bad.abc");
    auto expectFail = [&](Scene scene, const char* needle) {
        auto wr = AlembicExporter::write(path, scene);
        EXPECT_FALSE(wr.ok);
        EXPECT_TRUE(wr.error.contains(needle)) << wr.error.toStdString();
        EXPECT_FALSE(QFile::exists(path));
        EXPECT_FALSE(QFile::exists(path + ".qtmesh_tmp"));
    };

    expectFail(Scene{}, "nothing to export");

    Scene s;
    s.shapes.push_back(quadShape(2));
    s.fps = 0.0;
    expectFail(s, "fps");

    s = Scene{};
    s.shapes.push_back(quadShape(2));
    s.shapes[0].faceIndices[3] = 9;   // outside the 4 vertices
    expectFail(s, "outside");

    s = Scene{};
    s.shapes.push_back(quadShape(3));
    s.shapes[0].framePositions[2].resize(9);   // vertex count changes
    expectFail(s, "fixed topology");

    s = Scene{};
    s.shapes.push_back(quadShape(2));
    s.shapes[0].uvs.resize(6);
    expectFail(s, "UV");

    s = Scene{};
    s.shapes.push_back(quadShape(2));
    s.shapes[0].faceCounts = {3};   // counts no longer sum to the indices
    expectFail(s, "face counts");
}

TEST(AlembicExporterWrite, FailedWriteKeepsAnExistingFile) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath("keep.abc");
    Scene good;
    good.shapes.push_back(quadShape(2));
    ASSERT_TRUE(AlembicExporter::write(path, good).ok);
    const qint64 size = QFileInfo(path).size();

    Scene bad;
    EXPECT_FALSE(AlembicExporter::write(path, bad).ok);
    EXPECT_EQ(QFileInfo(path).size(), size);
}

// ---------------------------------------------------------------------------
// Ogre adapter
// ---------------------------------------------------------------------------

class AlembicExporterSceneTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(tryInitOgre()) << "Ogre init required"; }
    void TearDown() override {
        if (auto* mgr = Manager::getSingletonPtr()) {
            if (auto* scene = mgr->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
        }
    }
};

TEST_F(AlembicExporterSceneTest, BakesSkinnedClipIntoVertexCache) {
    auto* entity = createAnimatedTestEntity("ABC_Skinned");
    ASSERT_NE(entity, nullptr);
    QTemporaryDir tmp;
    const QString path = tmp.filePath("skinned.abc");

    AlembicExporter::Options o;
    o.fps = 10.0;
    o.animationName = QStringLiteral("TestAnim");   // 1 s → 11 samples inclusive
    auto r = AlembicExporter::exportEntities({entity}, path, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.animated);
    EXPECT_EQ(r.frameCount, 11);
    EXPECT_EQ(r.shapeCount, 1);
    EXPECT_EQ(r.vertexCount, 3);
    EXPECT_EQ(r.animationsUsed, QStringList{QStringLiteral("TestAnim")});

    auto rr = AlembicImporter::readFrameSet(path);
    ASSERT_TRUE(rr.ok) << rr.error.toStdString();
    ASSERT_EQ(rr.frames.frames.size(), 11u);
    EXPECT_EQ(rr.frames.fps, 10);
    // Vertex 0 sits on the Child bone's Y rotation axis: only the +0.5 X
    // translation at the clip midpoint moves it. Frame 5 is t = 0.5 s.
    const auto& mid = rr.frames.frames[5].positions;
    const auto& first = rr.frames.frames[0].positions;
    EXPECT_NEAR(first[0], 0.0f, 1e-4f);
    EXPECT_NEAR(mid[0], 0.5f, 1e-3f);
    EXPECT_NEAR(mid[1], 0.0f, 1e-3f);
    // The clip returns to rest on the last frame.
    EXPECT_NEAR(rr.frames.frames[10].positions[0], 0.0f, 1e-3f);
}

TEST_F(AlembicExporterSceneTest, RestoresAnimationStateAfterBake) {
    auto* entity = createAnimatedTestEntity("ABC_Restore");
    auto* state = entity->getAnimationState("TestAnim");
    state->setEnabled(false);
    state->setLoop(true);
    state->setTimePosition(0.25f);

    QTemporaryDir tmp;
    AlembicExporter::Options o;
    o.fps = 10.0;
    o.animationName = QStringLiteral("TestAnim");
    ASSERT_TRUE(AlembicExporter::exportEntities({entity}, tmp.filePath("a.abc"), o).ok);

    EXPECT_FALSE(state->getEnabled());
    EXPECT_TRUE(state->getLoop());
    EXPECT_NEAR(state->getTimePosition(), 0.25f, 1e-6f);
}

TEST_F(AlembicExporterSceneTest, FrameRangeSelectsTheSampledWindow) {
    auto* entity = createAnimatedTestEntity("ABC_Range");
    QTemporaryDir tmp;
    const QString path = tmp.filePath("range.abc");

    AlembicExporter::Options o;
    o.fps = 10.0;
    o.animationName = QStringLiteral("TestAnim");
    o.startTime = 0.2;
    o.endTime = 0.6;   // 0.4 s → 5 samples
    auto r = AlembicExporter::exportEntities({entity}, path, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.frameCount, 5);
}

TEST_F(AlembicExporterSceneTest, UnknownAnimationAndBadRangeAreErrors) {
    auto* entity = createAnimatedTestEntity("ABC_Errors");
    QTemporaryDir tmp;
    const QString path = tmp.filePath("e.abc");

    AlembicExporter::Options o;
    o.animationName = QStringLiteral("NoSuchClip");
    auto r = AlembicExporter::exportEntities({entity}, path, o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("NoSuchClip"));
    EXPECT_FALSE(QFile::exists(path));

    o = AlembicExporter::Options();
    o.animationName = QStringLiteral("TestAnim");
    o.startTime = 0.8;
    o.endTime = 0.2;
    r = AlembicExporter::exportEntities({entity}, path, o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("endTime"));

    EXPECT_FALSE(AlembicExporter::exportEntities({}, path).ok);
}

TEST_F(AlembicExporterSceneTest, NodeAnimationBecomesAnXformTrack) {
    auto* entity = createAnimatedTestEntity("ABC_Node");
    auto* nodeAnim = NodeAnimationManager::instance();
    ASSERT_TRUE(nodeAnim->createClip(QStringLiteral("Slide"), 1.0));
    ASSERT_TRUE(nodeAnim->addKeyframe(QStringLiteral("Slide"), QStringLiteral("ABC_Node"), 0.0,
                                      Ogre::Vector3(0, 0, 0), Ogre::Quaternion::IDENTITY,
                                      Ogre::Vector3::UNIT_SCALE));
    ASSERT_TRUE(nodeAnim->addKeyframe(QStringLiteral("Slide"), QStringLiteral("ABC_Node"), 1.0,
                                      Ogre::Vector3(10, 0, 0), Ogre::Quaternion::IDENTITY,
                                      Ogre::Vector3::UNIT_SCALE));
    auto* node = entity->getParentSceneNode();
    const Ogre::Vector3 restPos = node->getPosition();

    QTemporaryDir tmp;
    const QString path = tmp.filePath("node.abc");
    AlembicExporter::Options o;
    o.fps = 4.0;
    auto r = AlembicExporter::exportEntities({entity}, path, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.animated);
    EXPECT_EQ(r.frameCount, 5);

    Alembic::AbcCoreFactory::IFactory factory;
    Alembic::AbcGeom::IArchive ar = factory.getArchive(path.toStdString());
    Alembic::AbcGeom::IXform xf(ar.getTop().getChild(0), Alembic::AbcGeom::kWrapExisting);
    ASSERT_EQ(xf.getSchema().getNumSamples(), 5u);
    Alembic::AbcGeom::XformSample xs;
    xf.getSchema().get(xs, Alembic::AbcGeom::ISampleSelector(Alembic::Abc::index_t(4)));
    EXPECT_NEAR(xs.getMatrix()[3][0], 10.0, 1e-3);   // end of the slide
    xf.getSchema().get(xs, Alembic::AbcGeom::ISampleSelector(Alembic::Abc::index_t(2)));
    EXPECT_NEAR(xs.getMatrix()[3][0], 5.0, 1e-3);    // halfway

    // The scene's own node transform is put back.
    EXPECT_NEAR(node->getPosition().x, restPos.x, 1e-5f);
    nodeAnim->deleteClip(QStringLiteral("Slide"));
}

#endif  // ENABLE_ALEMBIC
