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
TEST_F(MCPServerMaterialBranchesCoverageTest, ModifyMaterialAmbientBranch)
{
    const QString name = makeMaterial("CovMat_ModAmbient");
    QJsonObject args;
    args["name"] = name;
    args["ambient"] = rgb(0.3, 0.4, 0.5);
    QJsonObject result = server->callTool("modify_material", args);
    EXPECT_FALSE(resultIsError(result));
    const QString text = resultText(result);
    EXPECT_TRUE(text.contains("Modified material"));
    EXPECT_TRUE(text.contains("ambient"));
}

// Specular + shininess branch (builds the 'specular: ... (shininess: ...)' line).
TEST_F(MCPServerMaterialBranchesCoverageTest, ModifyMaterialSpecularShininessBranch)
{
    const QString name = makeMaterial("CovMat_ModSpecular");
    QJsonObject args;
    args["name"] = name;
    args["specular"] = rgb(0.6, 0.7, 0.8);
    args["shininess"] = 48.0;
    QJsonObject result = server->callTool("modify_material", args);
    EXPECT_FALSE(resultIsError(result));
    const QString text = resultText(result);
    EXPECT_TRUE(text.contains("specular"));
    EXPECT_TRUE(text.contains("shininess"));
    EXPECT_TRUE(text.contains("48"));
}

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
TEST_F(MCPServerMaterialBranchesCoverageTest, ModifyMaterialEmissiveBranch)
{
    const QString name = makeMaterial("CovMat_ModEmissive");
    QJsonObject args;
    args["name"] = name;
    args["emissive"] = rgb(0.9, 0.1, 0.2);
    QJsonObject result = server->callTool("modify_material", args);
    EXPECT_FALSE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("emissive"));
}

// All four colour arrays in a single call — every modification line appended.
TEST_F(MCPServerMaterialBranchesCoverageTest, ModifyMaterialAllChannels)
{
    const QString name = makeMaterial("CovMat_ModAll");
    QJsonObject args;
    args["name"] = name;
    args["ambient"] = rgb(0.1, 0.1, 0.1);
    args["diffuse"] = rgb(0.5, 0.5, 0.5);
    args["specular"] = rgb(0.2, 0.3, 0.4);
    args["shininess"] = 12.0;
    args["emissive"] = rgb(0.0, 0.0, 0.7);
    QJsonObject result = server->callTool("modify_material", args);
    EXPECT_FALSE(resultIsError(result));
    const QString text = resultText(result);
    EXPECT_TRUE(text.contains("ambient"));
    EXPECT_TRUE(text.contains("diffuse"));
    EXPECT_TRUE(text.contains("specular"));
    EXPECT_TRUE(text.contains("emissive"));
}

// Empty-name error branch.
TEST_F(MCPServerMaterialBranchesCoverageTest, ModifyMaterialEmptyName)
{
    QJsonObject args;
    args["name"] = "";
    args["diffuse"] = rgb(1.0, 0.0, 0.0);
    QJsonObject result = server->callTool("modify_material", args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("name is required"));
}

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
TEST_F(MCPServerMaterialBranchesCoverageTest, ModifyMaterialNotFound)
{
    QJsonObject args;
    args["name"] = "CovMat_DoesNotExist_Modify";
    args["diffuse"] = rgb(0.1, 0.2, 0.3);
    QJsonObject result = server->callTool("modify_material", args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("not found"));
}

// ---------------------------------------------------------------------------
// create_material — applyColorsToPass paths + serializer output
// ---------------------------------------------------------------------------

// Top-level colour args route through applyColorsToPass (ambient/diffuse/
// specular+shininess/emissive all populated).
TEST_F(MCPServerMaterialBranchesCoverageTest, CreateMaterialWithTopLevelColors)
{
    QJsonObject args;
    args["name"] = "CovMat_CreateTopLevel";
    args["ambient"] = rgb(0.2, 0.2, 0.2);
    args["diffuse"] = rgb(0.8, 0.4, 0.1);
    args["specular"] = rgb(0.5, 0.5, 0.5);
    args["shininess"] = 24.0;
    args["emissive"] = rgb(0.0, 0.1, 0.0);
    QJsonObject result = server->callTool("create_material", args);
    EXPECT_FALSE(resultIsError(result));
    const QString text = resultText(result);
    EXPECT_TRUE(text.contains("Created material"));
    EXPECT_TRUE(text.contains("CovMat_CreateTopLevel"));
    // Serializer queueForExport output — should look like a material script.
    EXPECT_TRUE(text.contains("material") || text.contains("technique") ||
                text.contains("pass"));
}

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
TEST_F(MCPServerMaterialBranchesCoverageTest, CreateMaterialEmptyName)
{
    QJsonObject args;
    args["name"] = "";
    QJsonObject result = server->callTool("create_material", args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("name is required"));
}

// Duplicate-name error path.
TEST_F(MCPServerMaterialBranchesCoverageTest, CreateMaterialDuplicate)
{
    const QString name = makeMaterial("CovMat_CreateDup");
    QJsonObject args;
    args["name"] = name;
    QJsonObject result = server->callTool("create_material", args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("already exists"));
}

// ---------------------------------------------------------------------------
// set_texture — every branch
// ---------------------------------------------------------------------------

// Missing texture -> 'Both material and texture names are required'.
TEST_F(MCPServerMaterialBranchesCoverageTest, SetTextureMissingTexture)
{
    QJsonObject args;
    args["material"] = "CovMat_SetTexMissingTex";
    // no texture
    QJsonObject result = server->callTool("set_texture", args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("required"));
}

// Missing material -> same combined-required error.
TEST_F(MCPServerMaterialBranchesCoverageTest, SetTextureMissingMaterial)
{
    QJsonObject args;
    args["texture"] = "some_texture.png";
    // no material
    QJsonObject result = server->callTool("set_texture", args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("required"));
}

// Both empty -> required error.
TEST_F(MCPServerMaterialBranchesCoverageTest, SetTextureBothEmpty)
{
    QJsonObject args;
    args["material"] = "";
    args["texture"] = "";
    QJsonObject result = server->callTool("set_texture", args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("required"));
}

// Material-not-found branch.
TEST_F(MCPServerMaterialBranchesCoverageTest, SetTextureMaterialNotFound)
{
    QJsonObject args;
    args["material"] = "CovMat_SetTexNoSuchMaterial";
    args["texture"] = "tex.png";
    QJsonObject result = server->callTool("set_texture", args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("not found"));
}

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
TEST_F(MCPServerMaterialBranchesCoverageTest, SetTextureReplacesExistingUnit)
{
    const QString name = makeMaterial("CovMat_SetTexReplace");

    QJsonObject first;
    first["material"] = name;
    first["texture"] = "first.png";
    first["unit"] = 0;
    QJsonObject firstResult = server->callTool("set_texture", first);
    EXPECT_FALSE(resultIsError(firstResult));

    QJsonObject second;
    second["material"] = name;
    second["texture"] = "second.png";
    second["unit"] = 0;
    QJsonObject secondResult = server->callTool("set_texture", second);
    EXPECT_FALSE(resultIsError(secondResult));
    const QString text = resultText(secondResult);
    EXPECT_TRUE(text.contains("Set texture"));
    EXPECT_TRUE(text.contains("second.png"));
}

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

TEST_F(MCPServerMaterialBranchesCoverageTest, GetMaterialScript)
{
    const QString name = makeMaterial("CovMat_Get");
    QJsonObject args;
    args["name"] = name;
    QJsonObject result = server->callTool("get_material", args);
    EXPECT_FALSE(resultIsError(result));
    const QString text = resultText(result);
    EXPECT_TRUE(text.contains("CovMat_Get"));
    EXPECT_TRUE(text.contains("script"));
}

TEST_F(MCPServerMaterialBranchesCoverageTest, GetMaterialEmptyName)
{
    QJsonObject args;
    args["name"] = "";
    QJsonObject result = server->callTool("get_material", args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("name is required"));
}

TEST_F(MCPServerMaterialBranchesCoverageTest, GetMaterialNotFound)
{
    QJsonObject args;
    args["name"] = "CovMat_GetNoSuchMaterial";
    QJsonObject result = server->callTool("get_material", args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("not found"));
}

// ---------------------------------------------------------------------------
// list_materials — sorted list join
// ---------------------------------------------------------------------------

TEST_F(MCPServerMaterialBranchesCoverageTest, ListMaterialsContainsCreated)
{
    makeMaterial("CovMat_ListAardvark");
    makeMaterial("CovMat_ListZebra");

    QJsonObject result = server->callTool("list_materials", QJsonObject());
    EXPECT_FALSE(resultIsError(result));
    const QString text = resultText(result);
    EXPECT_TRUE(text.contains("Available materials"));
    EXPECT_TRUE(text.contains("CovMat_ListAardvark"));
    EXPECT_TRUE(text.contains("CovMat_ListZebra"));

    // Sorted output: Aardvark must appear before Zebra in the joined list.
    const int posA = text.indexOf("CovMat_ListAardvark");
    const int posZ = text.indexOf("CovMat_ListZebra");
    EXPECT_GE(posA, 0);
    EXPECT_GE(posZ, 0);
    EXPECT_LT(posA, posZ);
}

// list_materials ignores its args object (Q_UNUSED) — passing junk still works.
TEST_F(MCPServerMaterialBranchesCoverageTest, ListMaterialsIgnoresArgs)
{
    QJsonObject junk;
    junk["bogus"] = "value";
    QJsonObject result = server->callTool("list_materials", junk);
    EXPECT_FALSE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("Available materials"));
}
