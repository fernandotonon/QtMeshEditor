#include <gtest/gtest.h>

#include "AssetScanController.h"
#include "AppSettingsKeys.h"
#include "PlatformProfile.h"
#include "ScanConfig.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

namespace {

void resetAssetScanControllerState()
{
    AssetScanController::kill();
    QSettings settings;
    settings.remove(AppSettingsKeys::validationPlatformProfileId());
    settings.remove(AppSettingsKeys::validationPlatformProfilePickerVersion());
    settings.sync();
}

} // namespace

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

TEST(AssetScanControllerTest, ParseScanJsonReport_InvalidJson_ReturnsFalse)
{
    QString error;
    EXPECT_FALSE(AssetScanController::parseScanJsonReport(QByteArray("{not json"), nullptr, nullptr,
                                                                    nullptr, nullptr, nullptr, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(AssetScanControllerTest, SanitizeProfileLabel_StripsValidationSuffixes)
{
    EXPECT_EQ(AssetScanController::sanitizeProfileLabelForTest(QStringLiteral("PlayStation 1 (validation)"),
                                                               QStringLiteral("ps1")),
              QStringLiteral("PlayStation 1"));
    EXPECT_EQ(AssetScanController::sanitizeProfileLabelForTest(
                  QStringLiteral("Modern Console (source validation)"), QStringLiteral("modern-console")),
              QStringLiteral("Modern Console"));
    EXPECT_EQ(AssetScanController::sanitizeProfileLabelForTest(QString(), QStringLiteral("vr")),
              QStringLiteral("vr"));
}

TEST(AssetScanControllerTest, SelectedProfileId_PersistsInQSettings)
{
    resetAssetScanControllerState();

    auto* controller = AssetScanController::instance();
    ASSERT_FALSE(controller->profileIds().isEmpty());

    const QString id = QStringLiteral("mobile-low");
    ASSERT_TRUE(controller->profileIds().contains(id));
    controller->setSelectedProfileId(id);
    EXPECT_EQ(controller->selectedProfileId(), id);

    QSettings().sync();
    const QSettings reloaded;
    EXPECT_EQ(reloaded.value(AppSettingsKeys::validationPlatformProfileId()).toString(), id);

    AssetScanController::kill();
}

TEST(AssetScanControllerTest, SetSelectedProfileId_RejectsUnknownProfile)
{
    resetAssetScanControllerState();

    auto* controller = AssetScanController::instance();
    const QString before = controller->selectedProfileId();
    controller->setSelectedProfileId(QStringLiteral("not-a-real-profile-id"));
    EXPECT_EQ(controller->selectedProfileId(), before);

    AssetScanController::kill();
}

TEST(AssetScanControllerTest, ProfileDescription_PopulatedForModernConsole)
{
    resetAssetScanControllerState();

    auto* controller = AssetScanController::instance();
    controller->setSelectedProfileId(QStringLiteral("modern-console"));
    EXPECT_FALSE(controller->profileDescription().isEmpty());

    AssetScanController::kill();
}

TEST(AssetScanControllerTest, UiProfileList_ExcludesExampleProfilesAndDefaultsModernConsole)
{
    resetAssetScanControllerState();

    auto* controller = AssetScanController::instance();
    const QStringList ids = controller->profileIds();
    ASSERT_FALSE(ids.isEmpty());
    EXPECT_FALSE(ids.contains(QStringLiteral("example-minimal")));
    EXPECT_FALSE(ids.contains(QStringLiteral("example-base")));
    EXPECT_FALSE(ids.contains(QStringLiteral("example-texture-inspect")));
    EXPECT_TRUE(ids.contains(QStringLiteral("modern-console")));
    EXPECT_EQ(controller->selectedProfileId(), QStringLiteral("modern-console"));
    EXPECT_EQ(controller->profileIds().first(), QStringLiteral("modern-console"));
    EXPECT_EQ(controller->profileLabels().first(), QStringLiteral("Modern Console"));
    EXPECT_FALSE(controller->profileLabels().first().contains(QStringLiteral("validation")));

    AssetScanController::kill();
}

TEST(AssetScanControllerTest, PickerVersionMigration_ResetsStaleSavedProfileToModernConsole)
{
    resetAssetScanControllerState();
    QSettings settings;
    settings.setValue(AppSettingsKeys::validationPlatformProfileId(), QStringLiteral("ps1"));
    settings.setValue(AppSettingsKeys::validationPlatformProfilePickerVersion(), 1);
    settings.sync();

    auto* controller = AssetScanController::instance();
    EXPECT_EQ(controller->selectedProfileId(), QStringLiteral("modern-console"));
    EXPECT_EQ(settings.value(AppSettingsKeys::validationPlatformProfilePickerVersion()).toInt(), 2);

    AssetScanController::kill();
}

TEST(AssetScanControllerTest, IngestScanReportJsonForTest_UpdatesSummaryProperties)
{
    resetAssetScanControllerState();

    auto* controller = AssetScanController::instance();
    const QByteArray json = R"({
        "summary": { "scanned": 3, "passed": 2, "warnings": 1, "errors": 0 },
        "assets": []
    })";

    controller->ingestScanReportJsonForTest(json);
    EXPECT_TRUE(controller->hasResults());
    EXPECT_EQ(controller->summaryScanned(), 3);
    EXPECT_EQ(controller->summaryPassed(), 2);
    EXPECT_EQ(controller->summaryWarnings(), 1);
    EXPECT_EQ(controller->summaryErrors(), 0);
    EXPECT_TRUE(controller->findings().isEmpty());

    AssetScanController::kill();
}

TEST(AssetScanControllerTest, ScanFolder_RejectsInvalidDirectory)
{
    resetAssetScanControllerState();

    auto* controller = AssetScanController::instance();
    QSignalSpy finishedSpy(controller, &AssetScanController::scanFinished);
    QSignalSpy errorSpy(controller, &AssetScanController::error);

    controller->scanFolder(QStringLiteral("/path/that/does/not/exist/for/qtmesh-scan"));
    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_FALSE(finishedSpy.at(0).at(0).toBool());
    EXPECT_GE(errorSpy.count(), 1);
    EXPECT_FALSE(controller->scanning());

    AssetScanController::kill();
}

TEST(AssetScanControllerTest, ResolveCliBinary_ReturnsExistingPath)
{
    const QString binary = AssetScanController::resolveCliBinaryForTest();
    EXPECT_FALSE(binary.isEmpty());
    EXPECT_TRUE(QFileInfo::exists(binary));
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

TEST(PlatformProfileScanSetupTest, BuildScanConfigWithPlatformProfile_EmptyId_ReturnsDefaults)
{
    const PlatformProfileScanSetup setup = buildScanConfigWithPlatformProfile(QString());
    EXPECT_TRUE(setup.ok);
    EXPECT_TRUE(setup.profileId.isEmpty());
    EXPECT_EQ(setup.config.maxVertexCount, ScanConfig::defaults().maxVertexCount);
}

TEST(PlatformProfileScanSetupTest, BuildScanConfigWithPlatformProfile_InvalidId_Fails)
{
    const PlatformProfileScanSetup setup =
        buildScanConfigWithPlatformProfile(QStringLiteral("definitely-not-a-profile"));
    EXPECT_FALSE(setup.ok);
    EXPECT_FALSE(setup.error.isEmpty());
}
