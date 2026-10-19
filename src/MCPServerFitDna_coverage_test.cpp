// MCP fit_dna_body (epic #1095, phase 4): argument validation. The fit itself is covered by the DnaFit
// suites and the CLI; this pins the tool's refusals, which must match the CLI's.

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#define private public
#include "MCPServer.h"
#undef private

namespace {
QString textOf(const QJsonObject& r)
{
    const QJsonArray c = r["content"].toArray();
    return c.isEmpty() ? QString() : c[0].toObject()["text"].toString();
}
QString touch(QTemporaryDir& d, const char* name)
{
    const QString p = d.filePath(name);
    QFile f(p);
    f.open(QIODevice::WriteOnly);
    f.write("x");
    return p;
}
}  // namespace

TEST(MCPServerFitDnaCoverageTest, RequiresBothPaths)
{
    MCPServer s;
    const QJsonObject r = s.toolFitDnaBody({});
    EXPECT_TRUE(r["isError"].toBool());
    EXPECT_TRUE(textOf(r).contains("dna_path")) << textOf(r).toStdString();
    EXPECT_TRUE(s.toolFitDnaBody({{"dna_path", "/x.dna"}})["isError"].toBool());
}

TEST(MCPServerFitDnaCoverageTest, MissingFilesAreReported)
{
    MCPServer s;
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QString a = touch(d, "a.dna");
    EXPECT_TRUE(textOf(s.toolFitDnaBody({{"dna_path", d.filePath("nope.dna")}, {"target_path", a}})).contains("not found"));
    EXPECT_TRUE(textOf(s.toolFitDnaBody({{"dna_path", a}, {"target_path", d.filePath("nope.glb")}})).contains("not found"));
}

TEST(MCPServerFitDnaCoverageTest, UnknownModeIsRefused)
{
    MCPServer s;
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QJsonObject r = s.toolFitDnaBody({{"dna_path", touch(d, "a.dna")}, {"target_path", touch(d, "b.dna")}, {"mode", "magic"}});
    EXPECT_TRUE(r["isError"].toBool());
    EXPECT_TRUE(textOf(r).contains("mode"));
}

TEST(MCPServerFitDnaCoverageTest, RegistrationModesAreRefusedForADnaTargetLikeTheCli)
{
    MCPServer s;
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QString a = touch(d, "a.dna"), b = touch(d, "b.dna");
    for (const char* mode : {"joints", "surface"}) {
        const QJsonObject r = s.toolFitDnaBody({{"dna_path", a}, {"target_path", b}, {"mode", mode}});
        EXPECT_TRUE(r["isError"].toBool()) << mode;
        EXPECT_TRUE(textOf(r).contains("own skeleton")) << textOf(r).toStdString();
    }
}

TEST(MCPServerFitDnaCoverageTest, AlignLimbsFalseNeedsJointsMode)
{
    MCPServer s;
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QJsonObject r = s.toolFitDnaBody({{"dna_path", touch(d, "a.dna")}, {"target_path", touch(d, "b.glb")}, {"align_limbs", false}});
    EXPECT_TRUE(r["isError"].toBool());
    EXPECT_TRUE(textOf(r).contains("align_limbs")) << textOf(r).toStdString();
}
