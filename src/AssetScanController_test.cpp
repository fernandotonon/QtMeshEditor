#include <gtest/gtest.h>

#include "AssetScanController.h"
#include "AppSettingsKeys.h"
#include "PlatformProfile.h"
#include "ScanConfig.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

TEST(AssetScanControllerTest, ParseScanJsonReport_ExtractsSummaryAndFindings)
{
    const QByteArray json = R"({
        "summary": { "scanned": 2, "passed": 1, "warnings": 1, "errors": 1 },
        "assets": [
            {
                "file": "a.fbx",
                "findings": [
                    { "rule": "max_triangle_count", "severity": "error", "message": "too many tris" },
                    { "rule": "max_acmr", "severity": "info", "message": "ok acmr" }
                ]
            },
            {
                "file": "b.fbx",
                "findings": [
                    { "rule": "max_file_size_mb", "severity": "warning", "message": "large file" }
                ]
            }
        ]
    })";

    int scanned = 0;
    int passed = 0;
    int warnings = 0;
    int errors = 0;
    QVariantList findings;
    QString error;
    ASSERT_TRUE(AssetScanController::parseScanJsonReport(json, &scanned, &passed, &warnings, &errors,
                                                          &findings, &error));
    EXPECT_EQ(scanned, 2);
    EXPECT_EQ(passed, 1);
    EXPECT_EQ(warnings, 1);
    EXPECT_EQ(errors, 1);
    ASSERT_EQ(findings.size(), 2);
    EXPECT_EQ(findings.at(0).toMap().value(QStringLiteral("severity")).toString(), QStringLiteral("error"));
    EXPECT_EQ(findings.at(1).toMap().value(QStringLiteral("severity")).toString(), QStringLiteral("warning"));
}

TEST(AssetScanControllerTest, SelectedProfileId_PersistsInQSettings)
{
    AssetScanController::kill();
    QSettings settings;
    settings.remove(AppSettingsKeys::validationPlatformProfileId());

    auto* controller = AssetScanController::instance();
    ASSERT_FALSE(controller->profileIds().isEmpty());

    const QString id = controller->profileIds().first();
    controller->setSelectedProfileId(id);
    EXPECT_EQ(controller->selectedProfileId(), id);
    EXPECT_EQ(settings.value(AppSettingsKeys::validationPlatformProfileId()).toString(), id);

    AssetScanController::kill();
}

TEST(PlatformProfileScanSetupTest, BuildScanConfigWithPlatformProfile_MatchesLoader)
{
    const QStringList ids = PlatformProfileLoader::listBuiltinIds();
    ASSERT_FALSE(ids.isEmpty());

    const QString id = ids.first();
    const PlatformProfileScanSetup setup = buildScanConfigWithPlatformProfile(id);
    ASSERT_TRUE(setup.ok) << setup.error.toStdString();
    EXPECT_EQ(setup.profileId, id);

    ScanConfig expected = ScanConfig::defaults();
    const PlatformProfileLoadResult loaded = PlatformProfileLoader::load(id);
    ASSERT_TRUE(loaded.ok);
    applyPlatformProfile(expected, loaded.profile);

    EXPECT_EQ(setup.config.maxTriangleCount, expected.maxTriangleCount);
    EXPECT_EQ(setup.config.allowedFormats, expected.allowedFormats);
}
