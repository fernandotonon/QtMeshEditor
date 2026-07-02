#include <gtest/gtest.h>

#include "CLIPipeline.h"
#include "HDR/HdrMaterialScript.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <vector>

#include "HDR/HdrBundledLibrary.h"

namespace {

class ArgvBuilder {
public:
    explicit ArgvBuilder(const QStringList& parts)
    {
        m_storage.reserve(static_cast<size_t>(parts.size()));
        m_argv.reserve(static_cast<size_t>(parts.size() + 1));
        for (const QString& p : parts) {
            m_storage.push_back(p.toUtf8());
            m_argv.push_back(m_storage.back().data());
        }
        m_argv.push_back(nullptr);
        m_argc = static_cast<int>(parts.size());
    }

    int argc() const { return m_argc; }
    char** argv() { return m_argv.data(); }

private:
    std::vector<QByteArray> m_storage;
    std::vector<char*> m_argv;
    int m_argc = 0;
};

class CLIPipelineCmdMaterialHdrTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "Ogre plugins/codecs not available";
        createStandardOgreMaterials();
        if (auto* sel = SelectionSet::getSingletonPtr())
            sel->clear();
    }

    void TearDown() override
    {
        if (auto* sel = SelectionSet::getSingletonPtr())
            sel->clear();
    }

    QString copyRobotInto(QTemporaryDir& dir)
    {
        const QString fixture = testRobotMeshPath();
        if (fixture.isEmpty() || !QFile::exists(fixture))
            return QString();
        const QString dst = dir.filePath(QStringLiteral("robot.mesh"));
        QFile::remove(dst);
        if (!QFile::copy(fixture, dst))
            return QString();

        const QString skelFixture =
            QFileInfo(fixture).absolutePath() + QStringLiteral("/robot.skeleton");
        if (QFile::exists(skelFixture)) {
            const QString skelDst = dir.filePath(QStringLiteral("robot.skeleton"));
            QFile::remove(skelDst);
            QFile::copy(skelFixture, skelDst);
        }
        return dst;
    }
};

} // namespace

TEST_F(CLIPipelineCmdMaterialHdrTest, EnvIntensityOnlyWritesMaterialSidecar)
{
    QTemporaryDir src;
    ASSERT_TRUE(src.isValid());
    const QString mesh = copyRobotInto(src);
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture unavailable";

    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    const QString outMesh = out.filePath(QStringLiteral("hdr_out.mesh"));

    ArgvBuilder args({QStringLiteral("qtmesh"), QStringLiteral("material"), mesh,
                      QStringLiteral("--env-intensity"), QStringLiteral("1.5"),
                      QStringLiteral("-o"), outMesh});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 0);

    EXPECT_TRUE(QFile::exists(outMesh));
    const QString sidecar = out.filePath(QStringLiteral("hdr_out.material"));
    ASSERT_TRUE(QFile::exists(sidecar));
    EXPECT_GT(QFileInfo(sidecar).size(), 0);
    QFile sidecarFile(sidecar);
    ASSERT_TRUE(sidecarFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString matText = QString::fromUtf8(sidecarFile.readAll());
    EXPECT_TRUE(matText.contains(QStringLiteral("pbr_environment_intensity")))
        << matText.toStdString();
}

TEST_F(CLIPipelineCmdMaterialHdrTest, InvalidEnvTintReturnsUsageError)
{
    QTemporaryDir src;
    ASSERT_TRUE(src.isValid());
    const QString mesh = copyRobotInto(src);
    ASSERT_FALSE(mesh.isEmpty());

    ArgvBuilder args({QStringLiteral("qtmesh"), QStringLiteral("material"), mesh,
                      QStringLiteral("--env-tint"), QStringLiteral("not-a-color"),
                      QStringLiteral("-o"), src.filePath(QStringLiteral("out.mesh"))});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 2);
}

TEST_F(CLIPipelineCmdMaterialHdrTest, EnvIntensityOutOfRangeReturnsUsageError)
{
    QTemporaryDir src;
    ASSERT_TRUE(src.isValid());
    const QString mesh = copyRobotInto(src);
    ASSERT_FALSE(mesh.isEmpty());

    ArgvBuilder args({QStringLiteral("qtmesh"), QStringLiteral("material"), mesh,
                      QStringLiteral("--env-intensity"),
                      QString::number(HdrMaterialScript::kMaxEnvIntensity + 1.f),
                      QStringLiteral("-o"), src.filePath(QStringLiteral("out.mesh"))});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 2);
}

TEST_F(CLIPipelineCmdMaterialHdrTest, LoadEnvWritesHdrEnvSidecar)
{
    const QString envName = QStringLiteral("flat_grey.hdr");
    if (HdrBundledLibrary::resolveHdriPath(envName).isEmpty())
        GTEST_SKIP() << "flat_grey.hdr not available in this build tree";

    QTemporaryDir src;
    ASSERT_TRUE(src.isValid());
    const QString mesh = copyRobotInto(src);
    ASSERT_FALSE(mesh.isEmpty());

    QTemporaryDir out;
    ASSERT_TRUE(out.isValid());
    const QString outMesh = out.filePath(QStringLiteral("env_out.mesh"));

    ArgvBuilder args({QStringLiteral("qtmesh"), QStringLiteral("material"), mesh,
                      QStringLiteral("--env"), envName,
                      QStringLiteral("-o"), outMesh});
    EXPECT_EQ(CLIPipeline::cmdMaterial(args.argc(), args.argv()), 0);

    const QString envSidecar = out.filePath(QStringLiteral("env_out.hdr-env.json"));
    ASSERT_TRUE(QFile::exists(envSidecar));
    QFile envFile(envSidecar);
    ASSERT_TRUE(envFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString envJson = QString::fromUtf8(envFile.readAll());
    EXPECT_FALSE(envJson.isEmpty()) << envSidecar.toStdString();
    EXPECT_TRUE(envJson.contains(QStringLiteral("flat_grey"))
                || envJson.contains(QStringLiteral("environment")))
        << envJson.toStdString();
}
