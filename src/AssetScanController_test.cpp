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

TEST(AssetScanControllerTest, UiProfileList_ExcludesExampleProfilesAndDefaultsModernConsole)
{
    AssetScanController::kill();
    QSettings settings;
    settings.remove(AppSettingsKeys::validationPlatformProfileId());

    auto* controller = AssetScanController::instance();
    const QStringList ids = controller->profileIds();
    ASSERT_FALSE(ids.isEmpty());
    EXPECT_FALSE(ids.contains(QStringLiteral("example-minimal")));
    EXPECT_FALSE(ids.contains(QStringLiteral("example-base")));
    EXPECT_FALSE(ids.contains(QStringLiteral("example-texture-inspect")));
    EXPECT_TRUE(ids.contains(QStringLiteral("modern-console")));
    EXPECT_EQ(controller->selectedProfileId(), QStringLiteral("modern-console"));
    EXPECT_EQ(controller->profileLabels().first(), QStringLiteral("Modern Console"));
    EXPECT_FALSE(controller->profileLabels().first().contains(QStringLiteral("validation")));

    AssetScanController::kill();
}

TEST(AssetScanControllerTest, PickerVersionMigration_ResetsStaleSavedProfileToModernConsole)
{
    AssetScanController::kill();
    QSettings settings;
    settings.setValue(AppSettingsKeys::validationPlatformProfileId(), QStringLiteral("ps1"));
    settings.setValue(AppSettingsKeys::validationPlatformProfilePickerVersion(), 1);

    auto* controller = AssetScanController::instance();
    EXPECT_EQ(controller->selectedProfileId(), QStringLiteral("modern-console"));
    EXPECT_EQ(settings.value(AppSettingsKeys::validationPlatformProfilePickerVersion()).toInt(), 2);

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
