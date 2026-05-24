#ifdef ENABLE_PS1_RIP

#include "CaptureBuffer.h"
#include "CaptureSnapshot.h"
#include "EmuCore.h"
#include "EmuCoreLoader.h"
#include "MeshReconstructionStats.h"
#include "MeshReconstructor.h"
#include "MeshTopologyHash.h"
#include "PsxGoldenCapture.h"
#include "RipperHooks.h"

#include <QCoreApplication>
#include <QFileInfo>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {

MatrixRecord unitCubeMatrix()
{
    MatrixRecord m{};
    m.rt.m[0][0] = 1 << 12;
    m.rt.m[1][1] = 1 << 12;
    m.rt.m[2][2] = 1 << 12;
    m.ofx = 160 << 16;
    m.ofy = 120 << 16;
    m.h = 256;
    return m;
}

void appendScreenCubeWithMatrix(CaptureSnapshot &snap)
{
    snap.matrices.append(unitCubeMatrix());
    snap.cameraMatrixId = 0;

    constexpr int l = 48;
    constexpr int r = 208;
    constexpr int t = 48;
    constexpr int b = 168;
    constexpr int zNear = 4096;
    constexpr int zFar = 8192;

    auto tri = [&](int x0, int y0, int z0, int x1, int y1, int z1, int x2, int y2, int z2) {
        PrimRecord prim{};
        prim.kind = PrimKind::MonoTri;
        prim.vertexCount = 3;
        prim.matrixId = 0;
        prim.verts[0] = {x0, y0, z0, 200, 80, 80, 0, 0};
        prim.verts[1] = {x1, y1, z1, 200, 80, 80, 0, 0};
        prim.verts[2] = {x2, y2, z2, 200, 80, 80, 0, 0};
        snap.prims.append(prim);
    };

    tri(l, t, zNear, r, t, zNear, r, b, zNear);
    tri(l, t, zNear, r, b, zNear, l, b, zNear);
    tri(l, t, zFar, r, t, zFar, r, b, zFar);
    tri(l, t, zFar, r, b, zFar, l, b, zFar);
    tri(l, t, zNear, l, t, zFar, l, b, zFar);
    tri(l, t, zNear, l, b, zFar, l, b, zNear);
    tri(r, t, zNear, r, t, zFar, r, b, zFar);
    tri(r, t, zNear, r, b, zFar, r, b, zNear);
    tri(l, t, zNear, l, t, zFar, r, t, zFar);
    tri(l, t, zNear, r, t, zFar, r, t, zNear);
    tri(l, b, zNear, l, b, zFar, r, b, zFar);
    tri(l, b, zNear, r, b, zFar, r, b, zNear);
}

float meshMaxExtent(const ReconstructedMesh &mesh)
{
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
    float minZ = 0.0f;
    float maxZ = 0.0f;
    bool first = true;
    for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
        for (const ReconstructedVertex &v : sub.vertices) {
            if (first) {
                minX = maxX = v.px;
                minY = maxY = v.py;
                minZ = maxZ = v.pz;
                first = false;
            } else {
                minX = std::min(minX, v.px);
                maxX = std::max(maxX, v.px);
                minY = std::min(minY, v.py);
                maxY = std::max(maxY, v.py);
                minZ = std::min(minZ, v.pz);
                maxZ = std::max(maxZ, v.pz);
            }
        }
    }
    if (first)
        return 0.0f;
    const float ex = maxX - minX;
    const float ey = maxY - minY;
    const float ez = maxZ - minZ;
    return std::max({ex, ey, ez, 0.0f});
}

bool libretroCorePresent()
{
    const QString base = QCoreApplication::applicationDirPath() + QStringLiteral("/PS1Cores/");
    const auto anyExists = [&](const QStringList &names) {
        for (const QString &name : names) {
            if (QFileInfo(base + name).isFile())
                return true;
        }
        return false;
    };
#if defined(Q_OS_WIN)
    return anyExists({QStringLiteral("mednafen_psx_libretro.dll"),
                      QStringLiteral("beetle_psx_libretro.dll")});
#elif defined(Q_OS_MACOS)
    return anyExists({QStringLiteral("libmednafen_psx_libretro.dylib"),
                      QStringLiteral("mednafen_psx_libretro.dylib"),
                      QStringLiteral("libbeetle_psx_libretro.dylib"),
                      QStringLiteral("beetle_psx_libretro.dylib")});
#else
    return anyExists({QStringLiteral("libmednafen_psx_libretro.so"),
                      QStringLiteral("mednafen_psx_libretro.so"),
                      QStringLiteral("libbeetle_psx_libretro.so"),
                      QStringLiteral("beetle_psx_libretro.so")});
#endif
}

void assertGoldenReconstructionHealthy(const CaptureSnapshot &snapshot, const QString &sceneId)
{
    MeshReconstructionStats stats;
    const ReconstructedCaptureSet captureSet =
        MeshReconstructor::reconstructDeduped(snapshot, MeshDedupeMode::Loose, &stats);
    ASSERT_FALSE(captureSet.isEmpty()) << sceneId.toStdString();
    const ReconstructedMesh mesh = MeshReconstructor::reconstruct(snapshot);
    ASSERT_FALSE(mesh.isEmpty()) << sceneId.toStdString();
    EXPECT_GE(mesh.triangleCount, PsxGoldenCapture::kMinTrianglesEnvIntegration)
        << sceneId.toStdString();
    EXPECT_TRUE(stats.hasBounds()) << sceneId.toStdString();
    EXPECT_FALSE(stats.slabLike) << sceneId.toStdString();
    EXPECT_GT(meshMaxExtent(mesh), 0.1f) << sceneId.toStdString();
    EXPECT_GT(mesh.triangleCount, 0) << sceneId.toStdString();
}

} // namespace

TEST(MeshReconstructorGoldenTest, SlabMetricHeuristicDetectsThinExtent)
{
    MeshReconstructionStats stats;
    stats.totalVertices = 3;
    stats.boundsMinX = 0.0f;
    stats.boundsMaxX = 1.6f;
    stats.boundsMinY = 0.0f;
    stats.boundsMaxY = 1.2f;
    stats.boundsMinZ = 0.0f;
    stats.boundsMaxZ = 0.05f;
    stats.finalizeSlabMetric();
    EXPECT_TRUE(stats.slabLike);
}

TEST(MeshReconstructorGoldenTest, SlabMetricHeuristicPassesCubeExtent)
{
    MeshReconstructionStats stats;
    stats.totalVertices = 36;
    stats.boundsMinX = -1.0f;
    stats.boundsMaxX = 1.0f;
    stats.boundsMinY = -1.0f;
    stats.boundsMaxY = 1.0f;
    stats.boundsMinZ = -1.0f;
    stats.boundsMaxZ = 1.0f;
    stats.finalizeSlabMetric();
    EXPECT_FALSE(stats.slabLike);
}

TEST(MeshReconstructorGoldenTest, ScreenCubeReconstructionHasVolume)
{
    CaptureSnapshot snap;
    appendScreenCubeWithMatrix(snap);

    const ReconstructedMesh mesh = MeshReconstructor::reconstruct(snap);
    ASSERT_FALSE(mesh.isEmpty());
    EXPECT_GE(mesh.triangleCount, 8);
    EXPECT_GT(meshMaxExtent(mesh), 0.5f);
}

TEST(MeshReconstructorGoldenTest, GoldenSceneEnvResolution)
{
    EXPECT_TRUE(PsxGoldenCapture::isKnownSceneId(QStringLiteral("homebrew-static")));
    EXPECT_TRUE(PsxGoldenCapture::isKnownSceneId(QStringLiteral("retail-a")));
    EXPECT_FALSE(PsxGoldenCapture::isKnownSceneId(QStringLiteral("unknown-scene")));
    EXPECT_EQ(PsxGoldenCapture::isoEnvVarForScene(QStringLiteral("retail-a")),
              QStringLiteral("QTMESH_PS1_GOLDEN_RETAIL_A_ISO"));
}

TEST(MeshReconstructorGoldenTest, ConfiguredGoldenIsoReconstructsWithVolume)
{
    if (!libretroCorePresent())
        GTEST_SKIP() << "No libretro PS1 core in PS1Cores/";

    const QString bios = PsxGoldenCapture::biosPath();
    if (bios.isEmpty())
        GTEST_SKIP() << "Set QTMESH_PS1_TEST_BIOS to run golden ISO integration";

    QStringList scenes = PsxGoldenCapture::configuredSceneIds();
    const QString activeOnly = PsxGoldenCapture::activeSceneId();
    if (!activeOnly.isEmpty())
        scenes = {activeOnly};
    if (scenes.isEmpty())
        GTEST_SKIP() << "Set QTMESH_PS1_GOLDEN_*_ISO (or legacy homebrew/test ISO env)";

    qunsetenv("QTMESH_PS1_FORCE_STUB");

    QString err;
    std::unique_ptr<EmuCore> core = EmuCoreLoader::loadCore(&err);
    ASSERT_TRUE(core) << err.toStdString();
    if (core->coreId() == QStringLiteral("stub"))
        GTEST_SKIP() << "Libretro core unavailable (stub loaded)";

    for (const QString &sceneId : scenes) {
        const QString iso = PsxGoldenCapture::isoPathForScene(sceneId);
        ASSERT_FALSE(iso.isEmpty()) << sceneId.toStdString();

        ASSERT_TRUE(core->loadBios(bios)) << sceneId.toStdString();
        ASSERT_TRUE(core->loadIso(iso)) << sceneId.toStdString();
        QString bootErr;
        ASSERT_TRUE(core->boot(&bootErr)) << bootErr.toStdString() << " scene=" << sceneId.toStdString();

        std::atomic<bool> armed{true};
        CaptureBuffer buffer;
        RipperHooks hooks;
        hooks.setArmedFlag(&armed);
        hooks.setBuffer(&buffer);
        core->setHooks(&hooks);

        for (int frame = 0; frame < 240; ++frame)
            core->runFrame();

        core->ingestCaptureFrame();
        ASSERT_GT(buffer.prims().size(), 0) << sceneId.toStdString();

        CaptureSnapshot snapshot = CaptureSnapshot::fromBuffer(buffer, {});
        assertGoldenReconstructionHealthy(snapshot, sceneId);
    }

    qputenv("QTMESH_PS1_FORCE_STUB", "1");
}

#endif // ENABLE_PS1_RIP
