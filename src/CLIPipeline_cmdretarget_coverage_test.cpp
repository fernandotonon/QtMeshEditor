// #523 — `qtmesh anim <src> --retarget <tgt>` argument gates. All return
// before initOgreHeadless(), so no GL is needed.

#include <gtest/gtest.h>

#include "CLIPipeline.h"

#include <QByteArray>
#include <QList>
#include <initializer_list>

namespace {
class RtArgv {
public:
    RtArgv(std::initializer_list<const char*> args)
    {
        for (auto* a : args) m_storage.push_back(QByteArray(a));
        for (auto& b : m_storage) m_argv.push_back(b.data());
    }
    int argc() const { return int(m_argv.size()); }
    char** argv() { return m_argv.data(); }
private:
    QList<QByteArray> m_storage;
    QList<char*> m_argv;
};
}  // namespace

TEST(CLIPipeline_cmdRetargetCoverageTest, RoutesFromAnim)
{
    // cmdAnim must hand --retarget to cmdAnimRetarget (missing target → 2)
    RtArgv a({"qtmesh", "anim", "src.fbx", "--retarget"});
    EXPECT_EQ(2, CLIPipeline::cmdAnim(a.argc(), a.argv()));
}

TEST(CLIPipeline_cmdRetargetCoverageTest, MissingOutputIsRejected)
{
    RtArgv a({"qtmesh", "anim", "src.fbx", "--retarget", "tgt.fbx"});
    EXPECT_EQ(2, CLIPipeline::cmdAnimRetarget(a.argc(), a.argv()));
}

TEST(CLIPipeline_cmdRetargetCoverageTest, BadOptionValuesAreRejected)
{
    RtArgv t({"qtmesh", "anim", "s.fbx", "--retarget", "t.fbx", "--translation", "some", "-o", "o.glb"});
    EXPECT_EQ(2, CLIPipeline::cmdAnimRetarget(t.argc(), t.argv()));
    RtArgv r({"qtmesh", "anim", "s.fbx", "--retarget", "t.fbx", "--source-rest", "x", "-o", "o.glb"});
    EXPECT_EQ(2, CLIPipeline::cmdAnimRetarget(r.argc(), r.argv()));
    RtArgv f({"qtmesh", "anim", "s.fbx", "--retarget", "t.fbx", "--fps", "0", "-o", "o.glb"});
    EXPECT_EQ(2, CLIPipeline::cmdAnimRetarget(f.argc(), f.argv()));
    RtArgv u({"qtmesh", "anim", "s.fbx", "--retarget", "t.fbx", "--frobnicate", "-o", "o.glb"});
    EXPECT_EQ(2, CLIPipeline::cmdAnimRetarget(u.argc(), u.argv()));
}

TEST(CLIPipeline_cmdRetargetCoverageTest, MissingFilesAreReported)
{
    RtArgv a({"qtmesh", "anim", "/nonexistent/s.fbx", "--retarget", "/nonexistent/t.fbx", "-o", "o.glb"});
    EXPECT_EQ(1, CLIPipeline::cmdAnimRetarget(a.argc(), a.argv()));
}
