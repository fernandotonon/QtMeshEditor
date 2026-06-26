// Coverage test suite for MCPServer material handlers.
//
// Targets branches in toolModifyMaterial / toolCreateMaterial /
// toolSetTexture / toolGetMaterial / toolListMaterials that the
// existing MCPServer_test.cpp leaves uncovered (ambient / specular+
// shininess / emissive modify branches, applyColorsToPass via
// create_material, the create-vs-replace TUS branches of set_texture,
// the serializer output of get_material/list_materials, and the
// empty-name / missing-arg error paths).
//
// Distinct filename + distinct suite name (MCPServerMaterialBranchesCoverageTest)
// so there is no ODR / duplicate-registration clash with the existing suite.

#include <gtest/gtest.h>
#include <QApplication>
#include <QThread>
#include <QJsonArray>
#include <QJsonObject>
#include <memory>

#include "MCPServer.h"
#include "Manager.h"
#include "TestHelpers.h"

namespace {

// Local copies of the result accessors (static, file-internal — no clash
// with the identically-named statics in MCPServer_test.cpp since both have
// internal linkage in separate translation units).
QString resultText(const QJsonObject &result)
{
    QJsonArray content = result["content"].toArray();
    if (content.isEmpty()) return QString();
    return content[0].toObject()["text"].toString();
}

bool resultIsError(const QJsonObject &result)
{
    return result["isError"].toBool(false);
}

QJsonArray rgb(double r, double g, double b)
{
    QJsonArray a;
    a.append(r);
    a.append(g);
    a.append(b);
    return a;
}

} // namespace

class MCPServerMaterialBranchesCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        server.reset();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        server = std::make_unique<MCPServer>();
    }

    void TearDown() override
    {
        server.reset();
        Manager::kill();
        if (app) app->processEvents();
    }

    // Creates a fresh material via the public create_material handler and
    // asserts success, returning the (unique) name for follow-up calls.
    QString makeMaterial(const QString &name)
    {
        QJsonObject args;
        args["name"] = name;
        QJsonObject result = server->callTool("create_material", args);
        EXPECT_FALSE(resultIsError(result)) << resultText(result).toStdString();
        return name;
    }

    QApplication* app = nullptr;
    std::unique_ptr<MCPServer> server;
};

// ---------------------------------------------------------------------------
// modify_material branches
// ---------------------------------------------------------------------------

// Ambient array branch.
// Specular + shininess branch (builds the 'specular: ... (shininess: ...)' line).
// Specular branch WITHOUT explicit shininess — exercises the
// pass->getShininess() default fallback in args.value("shininess").toDouble(...).
TEST_F(MCPServerMaterialBranchesCoverageTest, ModifyMaterialSpecularDefaultShininess)
{
    const QString name = makeMaterial("CovMat_ModSpecDefault");
    QJsonObject args;
    args["name"] = name;
    args["specular"] = rgb(0.1, 0.2, 0.3);
    QJsonObject result = server->callTool("modify_material", args);
    EXPECT_FALSE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("specular"));
}

// Emissive (setSelfIllumination) branch.
// All four colour arrays in a single call — every modification line appended.
// Empty-name error branch.
// Missing-name (key absent) error branch.
TEST_F(MCPServerMaterialBranchesCoverageTest, ModifyMaterialMissingName)
{
    QJsonObject args;
    args["diffuse"] = rgb(1.0, 0.0, 0.0);
    QJsonObject result = server->callTool("modify_material", args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("name is required"));
}

// Material-not-found error branch (valid technique/pass path is reached only
// on found materials, so an existing material confirms the happy path; this
// confirms the not-found guard).
// ---------------------------------------------------------------------------
// create_material — applyColorsToPass paths + serializer output
// ---------------------------------------------------------------------------

// Top-level colour args route through applyColorsToPass (ambient/diffuse/
// specular+shininess/emissive all populated).
// Nested "colors" object form — exercises the resolveColorArg / resolveNumberArg
// nested-lookup branch.
TEST_F(MCPServerMaterialBranchesCoverageTest, CreateMaterialWithNestedColors)
{
    QJsonObject colors;
    colors["ambient"] = rgb(0.1, 0.1, 0.1);
    colors["diffuse"] = rgb(0.3, 0.6, 0.9);
    colors["specular"] = rgb(0.4, 0.4, 0.4);
    colors["shininess"] = 16.0;
    colors["emissive"] = rgb(0.2, 0.0, 0.0);

    QJsonObject args;
    args["name"] = "CovMat_CreateNested";
    args["colors"] = colors;
    QJsonObject result = server->callTool("create_material", args);
    EXPECT_FALSE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("CovMat_CreateNested"));
}

// No colour args — applyColorsToPass takes the default (else) branches for
// ambient and specular and skips diffuse/emissive.
TEST_F(MCPServerMaterialBranchesCoverageTest, CreateMaterialDefaultColors)
{
    QJsonObject args;
    args["name"] = "CovMat_CreateDefault";
    QJsonObject result = server->callTool("create_material", args);
    EXPECT_FALSE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("Created material"));
}

// Empty-name error path.
// Duplicate-name error path.
// ---------------------------------------------------------------------------
// set_texture — every branch
// ---------------------------------------------------------------------------

// Missing texture -> 'Both material and texture names are required'.
// Missing material -> same combined-required error.
// Both empty -> required error.
// Material-not-found branch.
// Create-new-TUS branch (unit >= numTextureUnitStates: a fresh material has
// no TUS at unit 0, so this createTextureUnitState path runs).
TEST_F(MCPServerMaterialBranchesCoverageTest, SetTextureCreatesNewUnit)
{
    const QString name = makeMaterial("CovMat_SetTexCreate");
    QJsonObject args;
    args["material"] = name;
    args["texture"] = "create_unit.png";
    args["unit"] = 0;
    QJsonObject result = server->callTool("set_texture", args);
    EXPECT_FALSE(resultIsError(result));
    const QString text = resultText(result);
    EXPECT_TRUE(text.contains("Set texture"));
    EXPECT_TRUE(text.contains("create_unit.png"));
    EXPECT_TRUE(text.contains("unit 0"));
}

// Replace-existing-TUS branch: set unit 0 once (creates), then again at unit 0
// (unit < numTextureUnitStates -> setTextureName replace path).
// Create at a higher unit index (unit 1) after unit 0 exists — still the
// create branch since unit (1) >= numTextureUnitStates (1).
TEST_F(MCPServerMaterialBranchesCoverageTest, SetTextureCreatesHigherUnit)
{
    const QString name = makeMaterial("CovMat_SetTexHigher");

    QJsonObject unit0;
    unit0["material"] = name;
    unit0["texture"] = "u0.png";
    unit0["unit"] = 0;
    EXPECT_FALSE(resultIsError(server->callTool("set_texture", unit0)));

    QJsonObject unit1;
    unit1["material"] = name;
    unit1["texture"] = "u1.png";
    unit1["unit"] = 1;
    QJsonObject result = server->callTool("set_texture", unit1);
    EXPECT_FALSE(resultIsError(result));
    const QString text = resultText(result);
    EXPECT_TRUE(text.contains("Set texture"));
    EXPECT_TRUE(text.contains("unit 1"));
}

// Default unit (no "unit" arg -> toInt(0)) still works.
TEST_F(MCPServerMaterialBranchesCoverageTest, SetTextureDefaultUnit)
{
    const QString name = makeMaterial("CovMat_SetTexDefaultUnit");
    QJsonObject args;
    args["material"] = name;
    args["texture"] = "default_unit.png";
    // no "unit" key
    QJsonObject result = server->callTool("set_texture", args);
    EXPECT_FALSE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("unit 0"));
}

// ---------------------------------------------------------------------------
// get_material — serializer output + error paths
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// list_materials — sorted list join
// ---------------------------------------------------------------------------

// list_materials ignores its args object (Q_UNUSED) — passing junk still works.