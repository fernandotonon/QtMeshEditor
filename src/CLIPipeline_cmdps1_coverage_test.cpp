// Coverage tests for CLIPipeline::cmdPs1 — the headless PS1 runtime-ripper
// subcommand parser (#431).
//
// These exercise ONLY the argument-validation branches that return BEFORE the
// emulator boots (initOgreHeadless() + PS1RipManager::start()), so they need no
// libretro core, no BIOS/ISO, no display. Anything that actually boots a disc
// is integration-only and covered manually (see src/PS1/golden_captures.md).
//
// Build-flag aware:
//   * With -DENABLE_PS1_RIP=ON  — the parser runs; bad args return 2.
//   * Without it                — cmdPs1 is a stub returning 1 (rebuild message)
//                                 regardless of args.
//
// src/test_main.cpp owns the single QCoreApplication; err() writes to a static
// QTextStream over stderr and is safe headlessly.

#include <gtest/gtest.h>
#include <QByteArray>
#include <QList>
#include <QString>
#include <initializer_list>

#include "CLIPipeline.h"

namespace {

class Ps1Argv {
public:
    Ps1Argv(std::initializer_list<const char*> args)
    {
        for (auto* a : args)
            m_storage.push_back(QByteArray(a));
        for (auto& ba : m_storage)
            m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() const { return m_argc; }
    char** argv() { return m_argv.data(); }

private:
    QList<QByteArray> m_storage;
    QList<char*> m_argv;
    int m_argc = 0;
};

#ifdef ENABLE_PS1_RIP
constexpr int kUsage = 2;   // parser rejects bad args with the usage code
constexpr int kRuntime = 1; // file-not-found etc.
#else
constexpr int kUsage = 1;   // stub always returns the rebuild-message code
constexpr int kRuntime = 1;
#endif

} // namespace

// No action token -> usage error.
TEST(CLIPipeline_cmdPs1CoverageTest, NoActionReturnsUsage)
{
    Ps1Argv args({"qtmesh", "ps1"});
    EXPECT_EQ(kUsage, CLIPipeline::cmdPs1(args.argc(), args.argv()));
}

// Unknown action -> usage error.
TEST(CLIPipeline_cmdPs1CoverageTest, UnknownActionReturnsUsage)
{
    Ps1Argv args({"qtmesh", "ps1", "frobnicate"});
    EXPECT_EQ(kUsage, CLIPipeline::cmdPs1(args.argc(), args.argv()));
}

// capture with no ISO -> usage error.
TEST(CLIPipeline_cmdPs1CoverageTest, CaptureNoIsoReturnsUsage)
{
    Ps1Argv args({"qtmesh", "ps1", "capture", "--bios", "b.bin"});
    EXPECT_EQ(kUsage, CLIPipeline::cmdPs1(args.argc(), args.argv()));
}

// capture with an ISO but no --bios -> usage error.
TEST(CLIPipeline_cmdPs1CoverageTest, CaptureNoBiosReturnsUsage)
{
    Ps1Argv args({"qtmesh", "ps1", "capture", "game.cue"});
    EXPECT_EQ(kUsage, CLIPipeline::cmdPs1(args.argc(), args.argv()));
}

// Unknown option -> usage error (PS1 build) / stub (non-PS1).
TEST(CLIPipeline_cmdPs1CoverageTest, UnknownOptionReturnsUsage)
{
    Ps1Argv args({"qtmesh", "ps1", "capture", "game.cue", "--bios", "b.bin", "--bogus"});
    EXPECT_EQ(kUsage, CLIPipeline::cmdPs1(args.argc(), args.argv()));
}

// dump-vram without -o -> usage error (PS1 build). Uses nonexistent paths, but
// the -o check fires before the file-existence check on a PS1 build... actually
// the ISO existence check runs first, so a missing ISO yields the runtime code.
TEST(CLIPipeline_cmdPs1CoverageTest, CaptureMissingIsoFileReturnsRuntime)
{
    Ps1Argv args({"qtmesh", "ps1", "capture", "/no/such/game.cue",
                  "--bios", "/no/such/bios.bin", "-o", "/tmp/out.gltf"});
    EXPECT_EQ(kRuntime, CLIPipeline::cmdPs1(args.argc(), args.argv()));
}

// --help returns 0 on a PS1 build (prints usage), stub returns 1.
TEST(CLIPipeline_cmdPs1CoverageTest, HelpReturns)
{
    Ps1Argv args({"qtmesh", "ps1", "--help"});
#ifdef ENABLE_PS1_RIP
    EXPECT_EQ(0, CLIPipeline::cmdPs1(args.argc(), args.argv()));
#else
    EXPECT_EQ(1, CLIPipeline::cmdPs1(args.argc(), args.argv()));
#endif
}
