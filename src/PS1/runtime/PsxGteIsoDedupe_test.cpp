#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/EmuCore.h"
#include "PS1/runtime/EmuCoreLoader.h"
#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/PsxGteInstructionCapture.h"
#include "PS1/runtime/RipperHooks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <QSet>

#include <atomic>
#include <cstddef>
#include <cstring>

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
    constexpr size_t kCode = 0x00004000;

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

} // namespace

TEST(PsxGteIsoDedupeTest, StaticSceneCop2ProgramDedupesThreeDrawables)
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

    EXPECT_GE(buffer.matrices().size(), 3);
    EXPECT_LE(buffer.matrices().size(), 12);
    EXPECT_TRUE(buffer.hasCameraMatrix());

    QSet<uint64_t> hashes;
    for (const MatrixRecord &m : buffer.matrices())
        hashes.insert(m.hash);
    EXPECT_EQ(hashes.size(), buffer.matrices().size());
}

TEST(PsxGteIsoDedupeTest, RealIsoCaptureHasBoundedMatrixCount)
{
    if (!libretroCorePresent())
        GTEST_SKIP() << "libretro PS1 core not installed";

    const QString bios = testBiosPath();
    const QString iso = testIsoPath();
    if (bios.isEmpty() || iso.isEmpty())
        GTEST_SKIP() << "Set QTMESH_PS1_TEST_BIOS and QTMESH_PS1_TEST_ISO (or QTMESH_PS1_TEST_HOMEBREW_ISO)";

    qunsetenv("QTMESH_PS1_FORCE_STUB");

    QString err;
    std::unique_ptr<EmuCore> core = EmuCoreLoader::loadCore(&err);
    ASSERT_TRUE(core) << err.toStdString();
    if (core->coreId() == QStringLiteral("stub"))
        GTEST_SKIP() << "libretro core unavailable (stub loaded)";

    ASSERT_TRUE(core->loadBios(bios));
    ASSERT_TRUE(core->loadIso(iso));
    QString bootErr;
    ASSERT_TRUE(core->boot(&bootErr)) << bootErr.toStdString();

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);
    core->setHooks(&hooks);

    for (int frame = 0; frame < 240; ++frame)
        core->runFrame();

    core->ingestCaptureFrame();

    EXPECT_GE(buffer.matrices().size(), 1);
    EXPECT_LE(buffer.matrices().size(), 64) << "hash dedupe should keep per-drawable matrices bounded";
    EXPECT_TRUE(buffer.hasCameraMatrix());

    QSet<uint64_t> hashes;
    for (const MatrixRecord &m : buffer.matrices())
        hashes.insert(m.hash);
    EXPECT_EQ(hashes.size(), buffer.matrices().size());

    qputenv("QTMESH_PS1_FORCE_STUB", "1");
}

#endif // ENABLE_PS1_RIP
