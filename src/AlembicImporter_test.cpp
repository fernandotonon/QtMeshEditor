#include <gtest/gtest.h>

#include "AlembicImporter.h"

#include <QDir>
#include <QFile>

// The decode path only exists in an ENABLE_ALEMBIC build. Without it, assert the
// feature reports unavailable + fails gracefully (no crash) — the contract the
// GUI/CLI rely on for the "rebuild with -DENABLE_ALEMBIC" message.
#ifndef ENABLE_ALEMBIC

TEST(AlembicImporterStandalone, UnavailableWithoutFlag) {
    EXPECT_FALSE(AlembicImporter::available());
    auto rr = AlembicImporter::readFrameSet("/nonexistent.abc");
    EXPECT_FALSE(rr.ok);
    EXPECT_FALSE(rr.error.isEmpty());
    QString err;
    EXPECT_EQ(AlembicImporter::importToScene("/nonexistent.abc", &err), nullptr);
    EXPECT_FALSE(err.isEmpty());
    // readInfo (B3) must also fail-soft without the flag.
    auto info = AlembicImporter::readInfo("/nonexistent.abc");
    EXPECT_FALSE(info.ok);
    EXPECT_FALSE(info.error.isEmpty());
}

#else  // ENABLE_ALEMBIC

#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include <cmath>
#include <vector>

namespace {

// Write a tiny 2-frame quad vertex cache to `path`: a unit quad whose 4 verts
// translate +Y over frame 1. Round-trips through the same reader the app uses.
void writeQuadCache(const std::string& path) {
    using namespace Alembic::AbcGeom;
    OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path);
    // 30fps uniform time sampling.
    const chrono_t dt = 1.0 / 30.0;
    TimeSampling tsamp(dt, 0.0);
    Alembic::Util::uint32_t tsIdx = archive.addTimeSampling(tsamp);

    OPolyMesh meshObj(OObject(archive, kTop), "quadCache", tsIdx);
    OPolyMeshSchema& schema = meshObj.getSchema();

    // 4 verts, one quad face (count=4).
    std::vector<Imath::V3f> p0 = {
        {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}};
    std::vector<Imath::V3f> p1 = {
        {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}};
    std::vector<Alembic::Util::int32_t> indices = {0, 1, 2, 3};
    std::vector<Alembic::Util::int32_t> counts = {4};

    // First sample carries topology. Build via named args (avoid the
    // most-vexing-parse: `Sample s0(TempA(), TempB(), ...)` is read as a
    // function declaration).
    Abc::V3fArraySample posSample0(p0);
    Abc::Int32ArraySample idxSample(indices);
    Abc::Int32ArraySample cntSample(counts);
    OPolyMeshSchema::Sample s0(posSample0, idxSample, cntSample);
    schema.set(s0);
    // Second sample: positions only.
    Abc::V3fArraySample posSample1(p1);
    OPolyMeshSchema::Sample s1;
    s1.setPositions(posSample1);
    schema.set(s1);
    // archive flushes on destruction (end of scope).
}

}  // namespace

class AlembicImporterTest : public ::testing::Test {
protected:
    QString abcPath;
    void SetUp() override {
        abcPath = QDir::temp().filePath("qtmesh_test_quad.abc");
        QFile::remove(abcPath);
        writeQuadCache(abcPath.toStdString());
    }
    void TearDown() override { QFile::remove(abcPath); }
};

TEST_F(AlembicImporterTest, Available) {
    EXPECT_TRUE(AlembicImporter::available());
}

TEST_F(AlembicImporterTest, ReadsTwoFrameQuadCache) {
    auto rr = AlembicImporter::readFrameSet(abcPath);
    ASSERT_TRUE(rr.ok) << rr.error.toStdString();
    EXPECT_EQ(rr.frames.vertexCount, 4);
    ASSERT_EQ(rr.frames.frames.size(), 2u);
    EXPECT_EQ(rr.frames.fps, 30);

    // Frame 0 = base quad at Y=0; frame 1 = same quad at Y=1.
    const auto& f0 = rr.frames.frames[0].positions;
    const auto& f1 = rr.frames.frames[1].positions;
    ASSERT_EQ(f0.size(), 12u);
    for (int v = 0; v < 4; ++v) {
        EXPECT_NEAR(f0[v * 3 + 1], 0.0f, 1e-5f);  // frame 0 Y == 0
        EXPECT_NEAR(f1[v * 3 + 1], 1.0f, 1e-5f);  // frame 1 Y == 1
    }
    // Times: frame 1 is 1/30 s after frame 0.
    EXPECT_NEAR(rr.frames.frames[0].time, 0.0f, 1e-5f);
    EXPECT_NEAR(rr.frames.frames[1].time, 1.0f / 30.0f, 1e-4f);
    // AABB spans Y 0..1.
    EXPECT_NEAR(rr.frames.aabb[1], 0.0f, 1e-5f);
    EXPECT_NEAR(rr.frames.aabb[4], 1.0f, 1e-5f);
}

TEST_F(AlembicImporterTest, MissingFileFailsGracefully) {
    auto rr = AlembicImporter::readFrameSet("/no/such/file.abc");
    EXPECT_FALSE(rr.ok);
    EXPECT_FALSE(rr.error.isEmpty());
}

// B3: readInfo returns the same metadata as a full decode but without reading
// every frame's positions. On the 2-frame quad it must match readFrameSet.
TEST_F(AlembicImporterTest, ReadInfoMatchesDecode) {
    auto info = AlembicImporter::readInfo(abcPath);
    ASSERT_TRUE(info.ok) << info.error.toStdString();
    EXPECT_EQ(info.frameCount, 2);
    EXPECT_EQ(info.vertexCount, 4);
    EXPECT_EQ(info.fps, 30);
    // One quad → 2 triangles.
    EXPECT_EQ(info.faceCount, 2);
    // 2 frames is well under the pose/stream threshold (32) → "poses".
    EXPECT_EQ(info.storage, QStringLiteral("poses"));
    // duration = (frameCount - 1) / fps for uniform sampling.
    EXPECT_NEAR(info.durationSec, 1.0f / 30.0f, 1e-4f);
}

// B3: maxFrames caps the decode and flags truncation (no silent cap).
TEST_F(AlembicImporterTest, MaxFramesTruncates) {
    auto rr = AlembicImporter::readFrameSet(abcPath, /*maxFrames=*/1);
    ASSERT_TRUE(rr.ok) << rr.error.toStdString();
    EXPECT_EQ(rr.frames.frames.size(), 1u);
    EXPECT_EQ(rr.totalFrames, 2);
    EXPECT_TRUE(rr.truncated);

    // maxFrames >= total (or 0) must not flag truncation.
    auto full = AlembicImporter::readFrameSet(abcPath, /*maxFrames=*/0);
    ASSERT_TRUE(full.ok);
    EXPECT_EQ(full.totalFrames, 2);
    EXPECT_FALSE(full.truncated);
}

#endif  // ENABLE_ALEMBIC
