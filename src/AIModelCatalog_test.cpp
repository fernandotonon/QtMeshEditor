#include <gtest/gtest.h>
#include <QDir>
#include <QStandardPaths>
#include <QSettings>
#include <QVariant>
#include "AIModelCatalog.h"
#include "ImageTo3D/Trellis2Predictor.h"

// The TRELLIS.2 GGUF weights are downloadable from AI Model Settings
// (the "QtMeshEditor Models" tab) like every other on-demand model.
TEST(AIModelCatalogTest, CatalogListsTrellis2GgufWeights)
{
    const QVariantList models = AIModelCatalog::instance()->models();
    QVariantMap t2;
    for (const QVariant& v : models) {
        const QVariantMap m = v.toMap();
        if (m.value("id").toString() == QLatin1String("trellis2-gguf")) {
            t2 = m;
            break;
        }
    }
    ASSERT_FALSE(t2.isEmpty()) << "trellis2-gguf missing from the catalog";
    EXPECT_EQ(t2.value("feature").toString(), QStringLiteral("Image to 3D"));
    // Not ONNX-gated: trellis.cpp is an external runtime.
    EXPECT_TRUE(t2.value("available").toBool());
    const QVariantList files = t2.value("files").toList();
    ASSERT_EQ(files.size(), 7);   // the full 512-pipeline set
    // Files land in the directory Trellis2Predictor discovers as a fallback.
    for (const QVariant& fv : files) {
        const QString path = fv.toMap().value("path").toString();
        EXPECT_TRUE(path.contains(QLatin1String("ai_models/trellis2/")))
            << path.toStdString();
        EXPECT_TRUE(path.endsWith(QLatin1String(".gguf"))) << path.toStdString();
    }
}

TEST(AIModelCatalogTest, PredictorFindsCatalogDownloadedTrellis2Models)
{
    // With no explicit models dir and no trellis-cli sibling dir, the
    // predictor must fall back to the AI Model Settings download location.
    // Scoped isolation of every source trellisCliModelsDir() reads: the env
    // vars, the QSettings key (a developer machine may point it at a real
    // install), and QStandardPaths test mode.
    struct Scope {
        QVariant savedSetting;
        Scope() {
            QSettings st;
            savedSetting = st.value(QStringLiteral("ai/trellis2CliModels"));
            st.remove(QStringLiteral("ai/trellis2CliModels"));
            qputenv("QTMESH_TRELLIS2_CLI", "/nonexistent/trellis-cli-catalog-ut");
            qunsetenv("QTMESH_TRELLIS2_CLI_MODELS");
            QStandardPaths::setTestModeEnabled(true);
        }
        ~Scope() {
            QStandardPaths::setTestModeEnabled(false);
            qunsetenv("QTMESH_TRELLIS2_CLI");
            QSettings st;
            if (savedSetting.isValid())
                st.setValue(QStringLiteral("ai/trellis2CliModels"), savedSetting);
        }
    } scope;
    const QString catalogDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("ai_models/trellis2"));
    ASSERT_TRUE(QDir().mkpath(catalogDir));

    const QString resolved = Trellis2Predictor::trellisCliModelsDir();
    EXPECT_EQ(QDir(resolved).absolutePath(), QDir(catalogDir).absolutePath());

    QDir(catalogDir).removeRecursively();
}
