#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QJsonObject>
#include "QtMeshCloudClient.h"

TEST(QtMeshCloudClientValidate, AcceptsMinimalValid)
{
    QJsonObject o;
    o["version"] = 1;
    o["scan"] = QJsonObject{};
    o["rules"] = QJsonObject{};
    EXPECT_TRUE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, RejectsMissingRules)
{
    QJsonObject o;
    o["version"] = 1;
    o["scan"] = QJsonObject{};
    EXPECT_FALSE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, RejectsMissingVersion)
{
    QJsonObject o;
    o["scan"] = QJsonObject{};
    o["rules"] = QJsonObject{};
    EXPECT_FALSE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, RejectsStringVersion)
{
    QJsonObject o;
    o["version"] = QStringLiteral("foo");
    o["scan"] = QJsonObject{};
    o["rules"] = QJsonObject{};
    EXPECT_FALSE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, RejectsBoolVersion)
{
    QJsonObject o;
    o["version"] = true;
    o["scan"] = QJsonObject{};
    o["rules"] = QJsonObject{};
    EXPECT_FALSE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, RejectsNonObjectScan)
{
    QJsonObject o;
    o["version"] = 1;
    o["scan"] = 42;       // not an object
    o["rules"] = QJsonObject{};
    EXPECT_FALSE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, RejectsNonObjectRules)
{
    QJsonObject o;
    o["version"] = 1;
    o["scan"] = QJsonObject{};
    o["rules"] = QStringLiteral("invalid");
    EXPECT_FALSE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientValidate, AcceptsNumericVersionFloat)
{
    QJsonObject o;
    o["version"] = 1.5;    // doubles are allowed (JSON numbers)
    o["scan"] = QJsonObject{};
    o["rules"] = QJsonObject{};
    EXPECT_TRUE(QtMeshCloudClient::validateCloudConfigJson(o));
}

TEST(QtMeshCloudClientApiBaseUrl, DefaultUrlWhenEnvUnset)
{
    qunsetenv("QTMESH_API_BASE");
    EXPECT_EQ(QtMeshCloudClient::apiBaseUrl(),
              QStringLiteral("https://api.qtmesh.dev"));
}

TEST(QtMeshCloudClientApiBaseUrl, EnvOverride)
{
    qputenv("QTMESH_API_BASE", "https://my-host.example.com");
    EXPECT_EQ(QtMeshCloudClient::apiBaseUrl(),
              QStringLiteral("https://my-host.example.com"));
    qunsetenv("QTMESH_API_BASE");
}

TEST(QtMeshCloudClientApiBaseUrl, TrailingSlashesStripped)
{
    qputenv("QTMESH_API_BASE", "https://my-host.example.com///");
    EXPECT_EQ(QtMeshCloudClient::apiBaseUrl(),
              QStringLiteral("https://my-host.example.com"));
    qunsetenv("QTMESH_API_BASE");
}

TEST(QtMeshCloudClientApiBaseUrl, WhitespaceTrimmed)
{
    qputenv("QTMESH_API_BASE", "  https://api.test.com/  ");
    EXPECT_EQ(QtMeshCloudClient::apiBaseUrl(),
              QStringLiteral("https://api.test.com"));
    qunsetenv("QTMESH_API_BASE");
}

TEST(QtMeshCloudClientFetchRules, MissingTokenReturnsErrorImmediately)
{
    auto result = QtMeshCloudClient::fetchRules(QString(), /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("missing bearer token", Qt::CaseInsensitive));
}

TEST(QtMeshCloudClientUploadScan, MissingTokenReturnsErrorImmediately)
{
    QJsonObject empty;
    auto result = QtMeshCloudClient::uploadScanReport(QString(), empty, /*timeoutMs=*/100);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorString.contains("missing bearer token", Qt::CaseInsensitive));
}
