#ifdef ENABLE_PS1_RIP

// In-core rip capture chain tests (#813/#814/#815/#817).
//
// Two layers:
//  - RipperHooksInCoreStreamTest drives RipperHooks directly with a scripted
//    stream (no core binary needed): record delivery, draw correlation,
//    provenance tiers, stale-record degradation, RAM-pass suppression.
//  - FakeRipCoreTest loads the test-only fake libretro core (exporting the
//    qtmesh rip ABI) through the real plugin + trampolines: handshake, armed
//    mirroring, ABI-mismatch refusal, QTMESH_PS1_RIP_INCORE=0, and the full
//    stream → CaptureBuffer chain. This is the zero-ROM CI keystone (#817).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>

#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/CaptureSnapshot.h"
#include "PS1/runtime/EmuCore.h"
#include "PS1/runtime/EmuCoreLoader.h"
#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/GteCapture.h"
#include "PS1/runtime/RipperHooks.h"
#include "PS1/runtime/libretro/qtmesh_rip_abi.h"

#include <atomic>
#include <cstring>

// ABI layout guards, mirrored from the fork (any drift is an ABI break).
static_assert(sizeof(qtmesh_rip_vertex_shadow) == 20, "rip ABI shadow layout drifted");
static_assert(sizeof(qtmesh_rip_gte_record) == 88, "rip ABI record layout drifted");
static_assert(QTMESH_RIP_ABI_VERSION == 1u, "bump host handling when the ABI changes");

namespace {

constexpr uint32_t kFlatTriOpcode = 0x20000000u;

qtmesh_rip_gte_record makeRecord(int16_t vx, int16_t vy, int16_t vz, uint32_t seq,
                                 uint32_t frame = 0)
{
    qtmesh_rip_gte_record rec{};
    rec.vx = vx;
    rec.vy = vy;
    rec.vz = vz;
    rec.rt[0] = 4096;
    rec.rt[4] = 4096;
    rec.rt[8] = 4096;
    rec.tr[2] = 2000;
    rec.h = 256;
    const float sz = static_cast<float>(vz) + 2000.0f;
    rec.sx = static_cast<float>(vx) * 256.0f / sz;
    rec.sy = static_cast<float>(vy) * 256.0f / sz;
    rec.sz = sz;
    rec.frame = frame;
    rec.seq = seq;
    return rec;
}

qtmesh_rip_vertex_shadow shadowForRecord(const qtmesh_rip_gte_record &rec)
{
    qtmesh_rip_vertex_shadow sh{};
    sh.sx = rec.sx;
    sh.sy = rec.sy;
    sh.w = rec.sz;
    sh.flags = QTMESH_RIP_SHADOW_XY_VALID | QTMESH_RIP_SHADOW_W_VALID
               | QTMESH_RIP_SHADOW_TAG_VALID;
    sh.gte_record = rec.seq % QTMESH_RIP_GTE_RING_ENTRIES;
    return sh;
}

uint32_t vertexWord(const qtmesh_rip_gte_record &rec)
{
    const int ix = 160 + static_cast<int>(rec.sx);
    const int iy = 120 + static_cast<int>(rec.sy);
    return (static_cast<uint32_t>(iy & 0xFFFF) << 16) | static_cast<uint32_t>(ix & 0xFFFF);
}

struct HooksFixture {
    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;

    HooksFixture()
    {
        hooks.setArmedFlag(&armed);
        hooks.setBuffer(&buffer);
    }

    // One scripted triangle in the fork's delivery order: draw during the
    // frame, records flushed at frame end, then on_frame_end resolution.
    void streamTriangle(uint32_t seqBase, uint32_t frame = 0)
    {
        const qtmesh_rip_gte_record recs[3] = {
            makeRecord(-100, -100, -100, seqBase + 0, frame),
            makeRecord(100, -100, -100, seqBase + 1, frame),
            makeRecord(100, 100, -100, seqBase + 2, frame),
        };
        const uint32_t words[4] = {kFlatTriOpcode | 0x808080u, vertexWord(recs[0]),
                                   vertexWord(recs[1]), vertexWord(recs[2])};
        const qtmesh_rip_vertex_shadow shadows[3] = {shadowForRecord(recs[0]),
                                                     shadowForRecord(recs[1]),
                                                     shadowForRecord(recs[2])};
        hooks.onGpuDrawTracked(words, 4, shadows, 3);
        hooks.onGteRecords(recs, 3);
        hooks.onCoreFrameEnd(frame);
    }
};

} // namespace

TEST(RipperHooksInCoreStreamTest, TrackedDrawResolvesToGteRecords)
{
    HooksFixture fx;
    fx.streamTriangle(0);

    ASSERT_EQ(fx.buffer.prims().size(), 1);
    const PrimRecord &prim = fx.buffer.prims()[0];
    EXPECT_EQ(prim.vertexCount, 3);
    ASSERT_EQ(fx.buffer.gteRecords().size(), 3);

    for (int v = 0; v < 3; ++v) {
        const PsxVertex &vert = prim.verts[v];
        EXPECT_EQ(vert.provenance, static_cast<uint8_t>(PsxVertexProvenance::GteTracked))
            << "vertex " << v;
        ASSERT_LT(vert.gteRecordIndex, static_cast<uint32_t>(fx.buffer.gteRecords().size()));
        const GteRecordEntry &rec = fx.buffer.gteRecords()[static_cast<int>(vert.gteRecordIndex)];
        // Precise screen coords round to the packet's integer XY (±1 px).
        EXPECT_NEAR(rec.sx + 160.0f, static_cast<float>(vert.x), 1.0f);
        EXPECT_NEAR(rec.sy + 120.0f, static_cast<float>(vert.y), 1.0f);
        EXPECT_GT(vert.viewW, 0.0f);
    }

    // The record's matrix landed in the matrix table and is a real rotation.
    ASSERT_GE(fx.buffer.matrices().size(), 1);
    bool anyOrthonormal = false;
    for (const MatrixRecord &m : fx.buffer.matrices())
        anyOrthonormal = anyOrthonormal || GteCapture::looksOrthonormalRotation(m);
    EXPECT_TRUE(anyOrthonormal)
        << "in-core GTE matrices are real rotations; the validator must accept them";
}

TEST(RipperHooksInCoreStreamTest, UnknownRingSlotDegradesToDepthOnly)
{
    HooksFixture fx;

    const qtmesh_rip_gte_record rec = makeRecord(-100, -100, -100, 0);
    qtmesh_rip_vertex_shadow sh = shadowForRecord(rec);
    sh.gte_record = 4242; // never delivered
    const uint32_t words[4] = {kFlatTriOpcode, vertexWord(rec), vertexWord(rec) + 1,
                               vertexWord(rec) + 2};
    const qtmesh_rip_vertex_shadow shadows[3] = {sh, sh, sh};
    fx.hooks.onGpuDrawTracked(words, 4, shadows, 3);
    fx.hooks.onGteRecords(&rec, 1); // ring slot 0, not 4242
    fx.hooks.onCoreFrameEnd(0);

    ASSERT_EQ(fx.buffer.prims().size(), 1);
    const PsxVertex &vert = fx.buffer.prims()[0].verts[0];
    EXPECT_EQ(vert.provenance, static_cast<uint8_t>(PsxVertexProvenance::DepthOnly));
    EXPECT_EQ(vert.gteRecordIndex, UINT32_MAX);
    EXPECT_GT(vert.viewW, 0.0f);
}

TEST(RipperHooksInCoreStreamTest, MutatedShadowCoordsDegradeToDepthOnly)
{
    HooksFixture fx;

    const qtmesh_rip_gte_record rec = makeRecord(-100, -100, -100, 0);
    qtmesh_rip_vertex_shadow sh = shadowForRecord(rec);
    sh.sx += 5.0f; // value was recombined after the transform (or slot reused)
    const uint32_t words[4] = {kFlatTriOpcode, vertexWord(rec), vertexWord(rec) + 1,
                               vertexWord(rec) + 2};
    const qtmesh_rip_vertex_shadow shadows[3] = {sh, sh, sh};
    fx.hooks.onGpuDrawTracked(words, 4, shadows, 3);
    fx.hooks.onGteRecords(&rec, 1);
    fx.hooks.onCoreFrameEnd(0);

    ASSERT_EQ(fx.buffer.prims().size(), 1);
    EXPECT_EQ(fx.buffer.prims()[0].verts[0].provenance,
              static_cast<uint8_t>(PsxVertexProvenance::DepthOnly));
}

TEST(RipperHooksInCoreStreamTest, XyOnlyShadowStaysNoneProvenance)
{
    HooksFixture fx;

    const qtmesh_rip_gte_record rec = makeRecord(-100, -100, -100, 0);
    qtmesh_rip_vertex_shadow sh = shadowForRecord(rec);
    sh.flags = QTMESH_RIP_SHADOW_XY_VALID; // no w, no tag
    sh.w = 0.0f;
    const uint32_t words[4] = {kFlatTriOpcode, vertexWord(rec), vertexWord(rec) + 1,
                               vertexWord(rec) + 2};
    const qtmesh_rip_vertex_shadow shadows[3] = {sh, sh, sh};
    fx.hooks.onGpuDrawTracked(words, 4, shadows, 3);
    fx.hooks.onCoreFrameEnd(0);

    ASSERT_EQ(fx.buffer.prims().size(), 1);
    const PsxVertex &vert = fx.buffer.prims()[0].verts[0];
    EXPECT_EQ(vert.provenance, static_cast<uint8_t>(PsxVertexProvenance::None));
    EXPECT_FLOAT_EQ(vert.preciseX, sh.sx);
}

TEST(RipperHooksInCoreStreamTest, DrawEnvWordsApplyInSubmissionOrder)
{
    HooksFixture fx;

    // E1 draw-mode word interleaved before the triangle: the flat prim must
    // pick up the mode's tpage/drawModeBits (submission-order association).
    const uint32_t e1 = 0xE10000AAu; // wire format: opcode in bits 24-31
    fx.hooks.onGpuDrawTracked(&e1, 1, nullptr, 0);
    fx.streamTriangle(0);

    ASSERT_EQ(fx.buffer.prims().size(), 1);
    // The ingest rotates wire words into the host parser convention (opcode
    // low byte, payload << 8) before decoding.
    EXPECT_EQ(fx.buffer.prims()[0].drawModeBits, (e1 << 8) | (e1 >> 24));
}

TEST(RipperHooksInCoreStreamTest, CrossFrameDedupeKeepsOneCopy)
{
    HooksFixture fx;
    fx.streamTriangle(0, /*frame=*/0);
    fx.streamTriangle(3, /*frame=*/1);
    // Same geometry drawn in two frames → one prim, records accumulate.
    EXPECT_EQ(fx.buffer.prims().size(), 1);
    EXPECT_EQ(fx.buffer.gteRecords().size(), 6);
}

TEST(RipperHooksInCoreStreamTest, InCoreStreamSuppressesRamGp0Passes)
{
    HooksFixture fx;
    fx.streamTriangle(0);
    ASSERT_EQ(fx.buffer.prims().size(), 1);
    ASSERT_TRUE(fx.hooks.inCoreStreamActiveThisFrame());

    // RAM containing a GP0 packet the linear scan WOULD ingest normally.
    QByteArray ram(64 * 1024, '\0');
    const uint32_t packet[4] = {kFlatTriOpcode | 0x404040u,
                                (100u << 16) | 50u, (110u << 16) | 90u, (150u << 16) | 60u};
    std::memcpy(ram.data() + 4096, packet, sizeof(packet));

    const Gp0CaptureStats stats = Gp0HookDispatch::captureFrameFromSystemRam(
        reinterpret_cast<const uint8_t *>(ram.constData()), static_cast<size_t>(ram.size()),
        &fx.hooks, /*scanGteRam=*/true, /*accumulate=*/true);

    EXPECT_TRUE(stats.inCoreStream);
    EXPECT_EQ(stats.ramOtPrims, 0);
    EXPECT_EQ(stats.ramLinearPrims, 0);
    EXPECT_EQ(stats.ramChainRootPrims, 0);
    EXPECT_EQ(fx.buffer.prims().size(), 1) << "RAM GP0 passes must be skipped";
    EXPECT_EQ(stats.primarySource, Gp0CaptureSource::InCoreHook);
}

TEST(RipperHooksInCoreStreamTest, WithoutStreamRamPassesStillIngest)
{
    HooksFixture fx;

    QByteArray ram(64 * 1024, '\0');
    const uint32_t packet[4] = {kFlatTriOpcode | 0x404040u,
                                (100u << 16) | 50u, (110u << 16) | 90u, (150u << 16) | 60u};
    std::memcpy(ram.data() + 4096, packet, sizeof(packet));

    // accumulate=false: in accumulate mode the linear scan inserts the raw
    // dedupe key into the SAME live set onGpuPrim consults, and a synthetic
    // packet whose post-dispatch mutations are all no-ops (zero draw mode,
    // matrixId 0) collides with itself.
    const Gp0CaptureStats stats = Gp0HookDispatch::captureFrameFromSystemRam(
        reinterpret_cast<const uint8_t *>(ram.constData()), static_cast<size_t>(ram.size()),
        &fx.hooks, /*scanGteRam=*/false, /*accumulate=*/false);

    EXPECT_FALSE(stats.inCoreStream);
    EXPECT_GE(fx.buffer.prims().size(), 1)
        << "None-provenance world must be untouched when no in-core stream ran";
    EXPECT_NE(stats.primarySource, Gp0CaptureSource::InCoreHook);
    for (const PrimRecord &p : fx.buffer.prims())
        for (int v = 0; v < p.vertexCount; ++v)
            EXPECT_EQ(p.verts[v].provenance, static_cast<uint8_t>(PsxVertexProvenance::None));
}

TEST(RipperHooksInCoreStreamTest, SnapshotCarriesGteRecords)
{
    HooksFixture fx;
    fx.streamTriangle(0);

    const CaptureSnapshot snap = CaptureSnapshot::fromBuffer(fx.buffer);
    ASSERT_EQ(snap.gteRecords.size(), 3);
    ASSERT_EQ(snap.prims.size(), 1);
    const PsxVertex &vert = snap.prims[0].verts[0];
    ASSERT_LT(vert.gteRecordIndex, static_cast<uint32_t>(snap.gteRecords.size()));
    EXPECT_EQ(snap.gteRecords[static_cast<int>(vert.gteRecordIndex)].vx, -100);
}

// ---------------------------------------------------------------------------
// Fake-core end-to-end (#817 keystone): real plugin, real trampolines.
// ---------------------------------------------------------------------------

namespace {

QString fakeRipCorePath()
{
    const QDir binDir(QCoreApplication::applicationDirPath());
#if defined(Q_OS_WIN)
    const QString name = QStringLiteral("fake_rip_core.dll");
#elif defined(Q_OS_MACOS)
    const QString name = QStringLiteral("fake_rip_core.dylib");
#else
    const QString name = QStringLiteral("fake_rip_core.so");
#endif
    const QString path = binDir.filePath(name);
    return QFileInfo::exists(path) ? path : QString();
}

} // namespace

class FakeRipCoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_NE(QCoreApplication::instance(), nullptr);
        m_corePath = fakeRipCorePath();
        if (m_corePath.isEmpty())
            GTEST_SKIP() << "fake_rip_core test module not built beside UnitTests";
        qunsetenv("QTMESH_PS1_FORCE_STUB");
        qputenv("QTMESH_PS1_LIBRETRO_CORE", m_corePath.toUtf8());
    }

    void TearDown() override
    {
        qunsetenv("QTMESH_PS1_LIBRETRO_CORE");
        qunsetenv("QTMESH_FAKE_RIP_ABI_VERSION");
        qunsetenv("QTMESH_PS1_RIP_INCORE");
        qputenv("QTMESH_PS1_FORCE_STUB", "1");
    }

    std::unique_ptr<EmuCore> bootFakeCore(RipperHooks *hooks)
    {
        QString err;
        std::unique_ptr<EmuCore> core = EmuCoreLoader::loadCore(&err);
        if (!core) {
            ADD_FAILURE() << err.toStdString();
            return nullptr;
        }
        EXPECT_EQ(core->coreId(), QStringLiteral("libretro"));

        m_bios.reset(new QTemporaryFile(QDir::tempPath() + "/qtmesh_bios_XXXXXX.bin"));
        m_game.reset(new QTemporaryFile(QDir::tempPath() + "/qtmesh_game_XXXXXX.exe"));
        EXPECT_TRUE(m_bios->open());
        EXPECT_TRUE(m_game->open());
        m_bios->write(QByteArray(512 * 1024, '\0'));
        m_game->write("PS-X EXE");
        m_bios->close();
        m_game->close();

        if (!core->loadBios(m_bios->fileName()) || !core->loadIso(m_game->fileName())) {
            ADD_FAILURE() << "fake core BIOS/game load failed: "
                          << core->lastError().toStdString();
            return nullptr;
        }
        core->setHooks(hooks);
        QString bootErr;
        if (!core->boot(&bootErr)) {
            ADD_FAILURE() << bootErr.toStdString();
            return nullptr;
        }
        return core;
    }

    QString m_corePath;
    std::unique_ptr<QTemporaryFile> m_bios;
    std::unique_ptr<QTemporaryFile> m_game;
};

TEST_F(FakeRipCoreTest, HandshakeStreamsTrackedCube)
{
    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    std::unique_ptr<EmuCore> core = bootFakeCore(&hooks);
    ASSERT_TRUE(core);
    EXPECT_TRUE(core->inCoreHooksActive());

    for (int i = 0; i < 3; ++i)
        core->runFrame();

    // 12 cube triangles, deduped across the 3 frames.
    ASSERT_EQ(buffer.prims().size(), 12);
    int tracked = 0;
    int total = 0;
    for (const PrimRecord &p : buffer.prims()) {
        for (int v = 0; v < p.vertexCount; ++v) {
            ++total;
            if (p.verts[v].provenance == static_cast<uint8_t>(PsxVertexProvenance::GteTracked))
                ++tracked;
        }
    }
    EXPECT_EQ(tracked, total) << "every scripted vertex must resolve to its GTE record";

    // Records arrive every armed frame; matrices dedupe to the cube's one.
    EXPECT_GE(buffer.gteRecords().size(), 36);
    bool anyOrthonormal = false;
    for (const MatrixRecord &m : buffer.matrices())
        anyOrthonormal = anyOrthonormal || GteCapture::looksOrthonormalRotation(m);
    EXPECT_TRUE(anyOrthonormal);

    // Tracked model-space coords: exact cube corners via the record table.
    const CaptureSnapshot snap = CaptureSnapshot::fromBuffer(buffer);
    const PsxVertex &vert = snap.prims[0].verts[0];
    ASSERT_LT(vert.gteRecordIndex, static_cast<uint32_t>(snap.gteRecords.size()));
    const GteRecordEntry &rec = snap.gteRecords[static_cast<int>(vert.gteRecordIndex)];
    EXPECT_EQ(std::abs(static_cast<int>(rec.vx)), 100);
    EXPECT_EQ(std::abs(static_cast<int>(rec.vy)), 100);
    EXPECT_EQ(std::abs(static_cast<int>(rec.vz)), 100);

    const Gp0CaptureStats &stats = hooks.lastCaptureStats();
    EXPECT_TRUE(stats.inCoreStream);
    EXPECT_EQ(stats.primarySource, Gp0CaptureSource::InCoreHook);
}

TEST_F(FakeRipCoreTest, AbiMismatchRefusedRunsStock)
{
    qputenv("QTMESH_FAKE_RIP_ABI_VERSION", "99");

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    std::unique_ptr<EmuCore> core = bootFakeCore(&hooks);
    ASSERT_TRUE(core);
    EXPECT_FALSE(core->inCoreHooksActive()) << "version mismatch must refuse registration";

    for (int i = 0; i < 3; ++i)
        core->runFrame();

    EXPECT_TRUE(core->framebuffer().isValid()) << "the game must still run";
    EXPECT_TRUE(buffer.gteRecords().isEmpty());
}

TEST_F(FakeRipCoreTest, InCoreEnvKillSwitchSkipsRegistration)
{
    qputenv("QTMESH_PS1_RIP_INCORE", "0");

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    std::unique_ptr<EmuCore> core = bootFakeCore(&hooks);
    ASSERT_TRUE(core);
    EXPECT_FALSE(core->inCoreHooksActive());

    for (int i = 0; i < 3; ++i)
        core->runFrame();

    EXPECT_TRUE(buffer.gteRecords().isEmpty());
    EXPECT_TRUE(buffer.prims().isEmpty());
}

TEST_F(FakeRipCoreTest, DisarmedCoreDeliversNothing)
{
    std::atomic<bool> armed{false};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    std::unique_ptr<EmuCore> core = bootFakeCore(&hooks);
    ASSERT_TRUE(core);
    EXPECT_TRUE(core->inCoreHooksActive());

    for (int i = 0; i < 3; ++i)
        core->runFrame();
    EXPECT_TRUE(buffer.prims().isEmpty());
    EXPECT_TRUE(buffer.gteRecords().isEmpty());

    // Arm mid-session: the mirror must engage on the next frame.
    armed.store(true);
    for (int i = 0; i < 2; ++i)
        core->runFrame();
    EXPECT_EQ(buffer.prims().size(), 12);
}

#endif // ENABLE_PS1_RIP
