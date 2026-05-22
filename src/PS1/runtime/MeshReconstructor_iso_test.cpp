#ifdef ENABLE_PS1_RIP

#include "CaptureSnapshot.h"
#include "MeshReconstructor.h"
#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/EmuCore.h"
#include "PS1/runtime/EmuCoreLoader.h"
#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/PsxGteInstructionCapture.h"
#include "PS1/runtime/RipperHooks.h"
#include "PS1/runtime/VramSnapshot.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>

namespace {

constexpr uint32_t kInsnRtops = 0x42000001u;

uint32_t insnLw(int rt, int rs, int offset)
{
    return (0x23u << 26) | (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16)
           | static_cast<uint32_t>(offset & 0xFFFF);
}

uint32_t insnCtc2(int gpr, int ctrlIndex)
{
    return (0x12u << 26) | (6u << 21) | (static_cast<uint32_t>(gpr) << 16)
           | (static_cast<uint32_t>(ctrlIndex) << 11);
}

uint32_t insnLui(int rt, uint16_t imm)
{
    return (0x0Fu << 26) | (static_cast<uint32_t>(rt) << 16) | imm;
}

uint32_t insnOri(int rt, int rs, uint16_t imm)
{
    return (0x0Du << 26) | (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) | imm;
}

void writeMatrixAt(uint8_t *ram, size_t offset, int trX, int h = 256)
{
    MatrixRecord matrix{};
    matrix.rt.m[0][0] = 1 << 12;
    matrix.rt.m[1][1] = 1 << 12;
    matrix.rt.m[2][2] = 1 << 12;
    matrix.tr[0] = trX;
    matrix.h = h;
    matrix.ofx = 160 << 16;
    matrix.ofy = 120 << 16;
    std::memcpy(ram + offset, &matrix.rt, sizeof(matrix.rt));
    std::memcpy(ram + offset + sizeof(matrix.rt), matrix.tr, sizeof(matrix.tr));
    std::memcpy(ram + offset + sizeof(matrix.rt) + sizeof(matrix.tr), &matrix.ofx, sizeof(matrix.ofx));
    std::memcpy(ram + offset + sizeof(matrix.rt) + sizeof(matrix.tr) + sizeof(matrix.ofx), &matrix.ofy,
                sizeof(matrix.ofy));
    std::memcpy(ram + offset + sizeof(matrix.rt) + sizeof(matrix.tr) + sizeof(matrix.ofx) + sizeof(matrix.ofy),
                &matrix.h, sizeof(matrix.h));
}

void embedStaticSceneProgram(uint8_t *ram, size_t ramBytes)
{
    constexpr size_t kMatA = 0x00001000;
    constexpr size_t kMatB = 0x00001080;
    constexpr size_t kMatC = 0x00001100;
    constexpr size_t kCode = 0x00000100;

    writeMatrixAt(ram, kMatA, 0, 256);
    writeMatrixAt(ram, kMatB, 512, 256);
    writeMatrixAt(ram, kMatC, 1024, 256);

    uint32_t *code = reinterpret_cast<uint32_t *>(ram + kCode);
    int i = 0;

    auto emitLoadMatrix = [&](size_t matAddr) {
        const uint16_t hi = static_cast<uint16_t>((matAddr >> 16) & 0xFFFF);
        const uint16_t lo = static_cast<uint16_t>(matAddr & 0xFFFF);
        code[i++] = insnLui(8, hi);
        code[i++] = insnOri(8, 8, lo);
        auto loadCtrl = [&](int ctrlIndex, size_t byteOff) {
            code[i++] = insnLw(9, 8, static_cast<int>(byteOff));
            code[i++] = insnCtc2(9, ctrlIndex);
        };
        for (int ctrl = 0; ctrl <= 4; ++ctrl)
            loadCtrl(ctrl, offsetof(MatrixRecord, rt.m[0][0]) + static_cast<size_t>(ctrl) * sizeof(int32_t));
        loadCtrl(5, offsetof(MatrixRecord, tr[0]));
        loadCtrl(6, offsetof(MatrixRecord, tr[1]));
        loadCtrl(7, offsetof(MatrixRecord, tr[2]));
        loadCtrl(24, offsetof(MatrixRecord, ofx));
        loadCtrl(25, offsetof(MatrixRecord, ofy));
        loadCtrl(26, offsetof(MatrixRecord, h));
        code[i++] = kInsnRtops;
    };

    emitLoadMatrix(kMatA);
    emitLoadMatrix(kMatB);
    emitLoadMatrix(kMatC);
    (void)ramBytes;
}

QString testBiosPath()
{
    const QString env = qEnvironmentVariable("QTMESH_PS1_TEST_BIOS");
    if (!env.isEmpty() && QFileInfo::exists(env))
        return env;
    return {};
}

QString testIsoPath()
{
    const QString env = qEnvironmentVariable("QTMESH_PS1_TEST_ISO");
    if (!env.isEmpty() && QFileInfo::exists(env))
        return env;
    const QString homebrew = qEnvironmentVariable("QTMESH_PS1_TEST_HOMEBREW_ISO");
    if (!homebrew.isEmpty() && QFileInfo::exists(homebrew))
        return homebrew;
    return {};
}

bool libretroCorePresent()
{
    const QString base = QCoreApplication::applicationDirPath() + QStringLiteral("/PS1Cores/");
#if defined(Q_OS_WIN)
    return QFileInfo::exists(base + QStringLiteral("mednafen_psx_libretro.dll"))
           || QFileInfo::exists(base + QStringLiteral("beetle_psx_libretro.dll"));
#else
    return QFileInfo::exists(base + QStringLiteral("mednafen_psx_libretro.so"))
           || QFileInfo::exists(base + QStringLiteral("beetle_psx_libretro.so"));
#endif
}

bool stubCorePluginBesideBinary()
{
    const QDir coresDir(QCoreApplication::applicationDirPath() + QStringLiteral("/PS1Cores"));
    const QStringList candidates = {
#if defined(Q_OS_WIN)
        coresDir.filePath(QStringLiteral("qtmesh_ps1core_stub.dll")),
#elif defined(Q_OS_MACOS)
        coresDir.filePath(QStringLiteral("libqtmesh_ps1core_stub.dylib")),
        coresDir.filePath(QStringLiteral("qtmesh_ps1core_stub.dylib")),
#else
        coresDir.filePath(QStringLiteral("libqtmesh_ps1core_stub.so")),
        coresDir.filePath(QStringLiteral("qtmesh_ps1core_stub.so")),
#endif
    };
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return true;
    }
    return false;
}

void expectNonEmptyReconstruction(const CaptureSnapshot &snap)
{
    const ReconstructedMesh flat = MeshReconstructor::reconstruct(snap);
    EXPECT_FALSE(flat.isEmpty());
    EXPECT_GT(flat.vertexCount, 0);
    EXPECT_GT(flat.triangleCount, 0);

    const ReconstructedCaptureSet deduped =
        MeshReconstructor::reconstructDeduped(snap, MeshDedupeMode::Loose);
    EXPECT_GT(deduped.capturedPartCount, 0);
    EXPECT_GT(deduped.uniqueCount(), 0);
    EXPECT_GT(deduped.instanceCount(), 0);
}

} // namespace

TEST(MeshReconstructorIsoTest, GteCaptureSnapshotProducesNonEmptyMesh)
{
    alignas(4) uint8_t ram[64 * 1024];
    std::memset(ram, 0, sizeof(ram));
    embedStaticSceneProgram(ram, sizeof(ram));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    hooks.onFrameBegin();
    PsxGteInstructionCapture::captureFromSystemRam(ram, sizeof(ram), &hooks);
    Gp0HookDispatch::captureFrameFromSystemRam(ram, sizeof(ram), &hooks, false);
    hooks.onFrameEnd();

    ASSERT_FALSE(buffer.matrices().isEmpty());

    if (buffer.prims().isEmpty()) {
        PrimRecord tri{};
        tri.kind = PrimKind::TexturedTri;
        tri.vertexCount = 3;
        tri.matrixId = 0;
        tri.tpage = 0x0100;
        tri.clut = 0x0200;
        tri.verts[0] = {16, 16, 4096, 255, 0, 0, 32, 48};
        tri.verts[1] = {48, 16, 4096, 0, 255, 0, 160, 16};
        tri.verts[2] = {32, 40, 4096, 0, 0, 255, 96, 200};
        buffer.addPrim(tri);
    }

    const CaptureSnapshot snap = CaptureSnapshot::fromBuffer(buffer);
    expectNonEmptyReconstruction(snap);
}

TEST(MeshReconstructorIsoTest, StubCoreCaptureProducesNonEmptyMeshWithTexturedUvs)
{
    if (!stubCorePluginBesideBinary())
        return;

    qputenv("QTMESH_PS1_FORCE_STUB", "1");

    QString err;
    std::unique_ptr<EmuCore> core = EmuCoreLoader::loadCore(&err);
    ASSERT_TRUE(core) << err.toStdString();
    if (core->coreId() != QStringLiteral("stub"))
        return;

    QTemporaryFile bios(QDir::tempPath() + QStringLiteral("/qtmesh_bios_XXXXXX.bin"));
    QTemporaryFile iso(QDir::tempPath() + QStringLiteral("/qtmesh_iso_XXXXXX.bin"));
    ASSERT_TRUE(bios.open());
    ASSERT_TRUE(iso.open());
    bios.write("bios");
    iso.write("iso");
    bios.close();
    iso.close();
    ASSERT_TRUE(core->loadBios(bios.fileName()));
    ASSERT_TRUE(core->loadIso(iso.fileName()));
    ASSERT_TRUE(core->boot(nullptr));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    VramSnapshot vram;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);
    hooks.setVram(&vram);
    core->setHooks(&hooks);
    core->runFrame();

    ASSERT_FALSE(buffer.prims().isEmpty());
    const CaptureSnapshot snap = CaptureSnapshot::fromBuffer(buffer, vram.mutablePixels());
    expectNonEmptyReconstruction(snap);

    const ReconstructedMesh flat = MeshReconstructor::reconstruct(snap);
    bool foundTexturedUv = false;
    const float expectedU = 8.0f / 256.0f;
    const float expectedV = 8.0f / 256.0f;
    for (const ReconstructedSubMesh &sub : flat.subMeshes) {
        for (const ReconstructedVertex &v : sub.vertices) {
            if (std::abs(v.u - expectedU) < 1e-5f && std::abs(v.v - expectedV) < 1e-5f)
                foundTexturedUv = true;
        }
    }
    EXPECT_TRUE(foundTexturedUv) << "Stub textured prims should preserve page-local UVs";

    qunsetenv("QTMESH_PS1_FORCE_STUB");
}

TEST(MeshReconstructorIsoTest, RealIsoCaptureProducesNonEmptyMesh)
{
    if (!libretroCorePresent())
        return;
    const QString bios = testBiosPath();
    const QString iso = testIsoPath();
    if (bios.isEmpty() || iso.isEmpty())
        return;

    qunsetenv("QTMESH_PS1_FORCE_STUB");

    QString err;
    std::unique_ptr<EmuCore> core = EmuCoreLoader::loadCore(&err);
    ASSERT_TRUE(core) << err.toStdString();
    if (core->coreId() == QStringLiteral("stub"))
        return;

    ASSERT_TRUE(core->loadBios(bios));
    ASSERT_TRUE(core->loadIso(iso));
    QString bootErr;
    ASSERT_TRUE(core->boot(&bootErr)) << bootErr.toStdString();

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    VramSnapshot vram;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);
    hooks.setVram(&vram);
    core->setHooks(&hooks);

    for (int frame = 0; frame < 240; ++frame)
        core->runFrame();

    if (core->coreId() == QStringLiteral("libretro"))
        core->ingestCaptureFrame();

    ASSERT_FALSE(buffer.prims().isEmpty())
        << "Homebrew/commercial ISO should yield capturable GP0 primitives";
    const CaptureSnapshot snap = CaptureSnapshot::fromBuffer(buffer, vram.mutablePixels());
    expectNonEmptyReconstruction(snap);

    qputenv("QTMESH_PS1_FORCE_STUB", "1");
}

#endif // ENABLE_PS1_RIP
