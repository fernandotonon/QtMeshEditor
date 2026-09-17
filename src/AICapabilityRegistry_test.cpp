// Headless tests for the capability registry + constrained tool protocol
// (#1002 / #1003). Pure data — no Ogre, no LLM.
#include <gtest/gtest.h>

#include "AICapabilityRegistry.h"

#include <QJsonArray>
#include <QJsonObject>

namespace {

QJsonObject prop(const char* type, const char* desc, QJsonArray enumVals = {})
{
    QJsonObject p{{"type", type}, {"description", desc}};
    if (!enumVals.isEmpty()) p["enum"] = enumVals;
    return p;
}

QJsonObject tool(const char* name, const char* desc, QJsonObject props, QJsonArray required = {})
{
    QJsonObject schema{{"type", "object"}, {"properties", props}};
    if (!required.isEmpty()) schema["required"] = required;
    return {{"name", name}, {"description", desc}, {"inputSchema", schema}};
}

QJsonArray sampleTools()
{
    return {
        tool("get_scene_info", "Get information about the scene.", {}),
        tool("create_primitive", "Create a primitive mesh. Supported: box, sphere.",
             {{"type", prop("string", "Primitive type", {"box", "sphere", "cylinder"})}, {"name", prop("string", "Node name")}},
             {"type"}),
        tool("transform_mesh", "Move/rotate/scale a node.",
             {{"name", prop("string", "Node name")}, {"position", prop("array", "XYZ")}, {"scale", prop("array", "XYZ")}},
             {"name"}),
        tool("delete_entity", "Delete an entity.", {{"entity_name", prop("string", "Entity")}}, {"entity_name"}),
        tool("export_mesh", "Export.", {{"output_path", prop("string", "Path")}}, {"output_path"}),
        tool("auto_rig", "Auto-rig a static mesh.", {{"template", prop("string", "Template")}, {"skin", prop("boolean", "Also skin")}}),
        tool("decimate_mesh", "Decimate.", {{"entity_name", prop("string", "Entity")}, {"reduction", prop("number", "0..1")}, {"count", prop("integer", "n")}}),
        tool("ps1rip_status", "Ripper status.", {}),
        tool("my_vendor_thing", "Unmapped tool.", {}),
        tool("join_mesh_parts", "Join.", {{"entity_names", QJsonObject{{"type", "array"}, {"description", "names"}, {"items", QJsonObject{{"type", "string"}}}}}}),
    };
}

} // namespace

TEST(AICapabilityRegistry, GroupsToolsByCapabilityAndParksUnknownOnesInOther)
{
    AICapabilityRegistry reg(sampleTools());
    EXPECT_EQ(reg.toolCount(), 10);
    EXPECT_EQ(reg.capabilityOf("auto_rig"), "rigging");
    EXPECT_EQ(reg.capabilityOf("create_primitive"), "scene");
    EXPECT_EQ(reg.capabilityOf("export_mesh"), "scene_io");
    EXPECT_EQ(reg.capabilityOf("decimate_mesh"), "mesh_optimize");
    EXPECT_EQ(reg.capabilityOf("ps1rip_status"), "ps1") << "prefix rule";
    EXPECT_EQ(reg.capabilityOf("my_vendor_thing"), "other");
    EXPECT_TRUE(reg.capabilityOf("nope").isEmpty());
    // empty capabilities are not advertised
    const QStringList ids = reg.capabilityIds();
    EXPECT_TRUE(ids.contains("rigging"));
    EXPECT_FALSE(ids.contains("paint")) << "no paint tools in the sample";
    EXPECT_EQ(reg.toolsFor({"scene"}).size(), 4);
}

TEST(AICapabilityRegistry, PromptIndexIsCompactAndToolDocsComeFromTheSchema)
{
    AICapabilityRegistry reg(sampleTools());
    const QString index = reg.promptIndex();
    EXPECT_TRUE(index.contains("- rigging:"));
    EXPECT_FALSE(index.contains("auto_rig")) << "the index names capabilities, not tools";
    EXPECT_FALSE(index.contains("- paint:"));

    const QString doc = reg.toolDoc("create_primitive");
    EXPECT_TRUE(doc.startsWith("- create_primitive: Create a primitive mesh."));
    EXPECT_TRUE(doc.contains("type (one of box|sphere|cylinder, required)")) << doc.toStdString();
    EXPECT_TRUE(doc.contains("name (string): Node name"));

    const QString docs = reg.promptToolsFor({"rigging"});
    EXPECT_TRUE(docs.contains("- auto_rig:"));
    EXPECT_FALSE(docs.contains("create_primitive")) << "only the requested capability's tools";
}

TEST(AICapabilityRegistry, KeywordRoutingPicksRelevantCapabilitiesAndAlwaysKeepsScene)
{
    AICapabilityRegistry reg(sampleTools());
    const QStringList rig = reg.routeByKeywords("rig and skin the wolf then export it as glb");
    EXPECT_TRUE(rig.contains("rigging"));
    EXPECT_TRUE(rig.contains("scene_io"));
    EXPECT_TRUE(rig.contains("scene"));
    EXPECT_LE(rig.size(), 5);

    const QStringList simple = reg.routeByKeywords("make the box twice as large");
    EXPECT_EQ(simple.first(), "scene");
    EXPECT_FALSE(simple.contains("rigging"));

    EXPECT_TRUE(reg.routeByKeywords("hello").contains("scene")) << "never empty";
}

TEST(AICapabilityRegistry, ValidateArgumentsEnforcesRequiredTypesAndEnums)
{
    AICapabilityRegistry reg(sampleTools());
    QJsonObject out; QString err; QStringList warn;

    EXPECT_FALSE(reg.validateArguments("create_primitive", {}, &out, &err));
    EXPECT_TRUE(err.contains("missing required argument 'type'")) << err.toStdString();

    EXPECT_FALSE(reg.validateArguments("create_primitive", {{"type", "pyramid"}}, &out, &err));
    EXPECT_TRUE(err.contains("not one of box|sphere|cylinder")) << err.toStdString();

    EXPECT_TRUE(reg.validateArguments("create_primitive", {{"type", "box"}, {"name", 7}}, &out, &err, &warn));
    EXPECT_EQ(out["name"].toString(), "7") << "number → string coercion";
    EXPECT_FALSE(warn.isEmpty());

    EXPECT_FALSE(reg.validateArguments("nonexistent", {}, &out, &err));
    EXPECT_TRUE(err.contains("unknown tool"));
}

TEST(AICapabilityRegistry, ValidateArgumentsCoercesChattyModelOutput)
{
    AICapabilityRegistry reg(sampleTools());
    QJsonObject out; QString err; QStringList warn;

    // "2, 2, 2" and "[1,0,0]" strings become arrays; "0.5" becomes a number; "yes" a bool
    ASSERT_TRUE(reg.validateArguments("transform_mesh", {{"name", "Cube"}, {"scale", "2, 2, 2"}, {"position", "[1,0,0]"}}, &out, &err, &warn)) << err.toStdString();
    EXPECT_EQ(out["scale"].toArray().size(), 3);
    EXPECT_DOUBLE_EQ(out["position"].toArray().at(0).toDouble(), 1.0);

    ASSERT_TRUE(reg.validateArguments("decimate_mesh", {{"reduction", "0.5"}, {"count", 3}}, &out, &err, &warn)) << err.toStdString();
    EXPECT_DOUBLE_EQ(out["reduction"].toDouble(), 0.5);
    EXPECT_FALSE(reg.validateArguments("decimate_mesh", {{"count", 2.5}}, &out, &err));
    EXPECT_TRUE(err.contains("expected an integer"));
    EXPECT_FALSE(reg.validateArguments("decimate_mesh", {{"reduction", "half"}}, &out, &err));

    // integers: finite, integral and inside qint64 — never a UB conversion
    EXPECT_FALSE(reg.validateArguments("decimate_mesh", {{"count", 1e100}}, &out, &err));
    EXPECT_TRUE(err.contains("expected an integer")) << err.toStdString();
    EXPECT_FALSE(reg.validateArguments("decimate_mesh", {{"count", "inf"}}, &out, &err));

    ASSERT_TRUE(reg.validateArguments("auto_rig", {{"skin", "yes"}}, &out, &err, &warn));
    EXPECT_TRUE(out["skin"].toBool());
    EXPECT_FALSE(reg.validateArguments("auto_rig", {{"skin", "maybe"}}, &out, &err));

    // unknown properties pass through with a warning (tools may accept extras)
    warn.clear();
    ASSERT_TRUE(reg.validateArguments("get_scene_info", {{"verbose", true}}, &out, &err, &warn));
    EXPECT_TRUE(out.contains("verbose"));
    EXPECT_EQ(warn.size(), 1);
}

TEST(AICapabilityRegistry, ArrayItemsAreCheckedAgainstTheItemsSchema)
{
    AICapabilityRegistry reg(sampleTools());
    QJsonObject out; QString err;
    ASSERT_TRUE(reg.validateArguments("join_mesh_parts", {{"entity_names", QJsonArray{"A", "B"}}}, &out, &err)) << err.toStdString();
    EXPECT_FALSE(reg.validateArguments("join_mesh_parts", {{"entity_names", QJsonArray{"A", 3}}}, &out, &err));
    EXPECT_TRUE(err.contains("item 2: expected string")) << err.toStdString();
    // arrays without an items schema are still accepted as-is
    ASSERT_TRUE(reg.validateArguments("transform_mesh", {{"name", "Cube"}, {"scale", QJsonArray{1, "x"}}}, &out, &err));
}

TEST(AICapabilityRegistry, DestructiveReasonNamesDeletesAndOverwritesOnly)
{
    auto exists = [](const QString& p) { return p == "/tmp/existing.glb"; };
    EXPECT_EQ(AICapabilityRegistry::destructiveReason("delete_entity", {{"entity_name", "Cube"}}, exists), "deletes 'Cube'");
    EXPECT_TRUE(AICapabilityRegistry::destructiveReason("remove_skeleton", {{"entity_name", "Wolf"}}, exists).contains("deletes 'Wolf'"));
    EXPECT_TRUE(AICapabilityRegistry::destructiveReason("cloud_delete_project", {{"project_id", "p1"}}, exists).contains("not undoable"));
    EXPECT_TRUE(AICapabilityRegistry::destructiveReason("decimate_mesh", {{"entity_name", "Wolf"}}, exists).contains("rewrites the geometry"));
    EXPECT_EQ(AICapabilityRegistry::destructiveReason("export_mesh", {{"output_path", "/tmp/existing.glb"}}, exists), "overwrites existing file /tmp/existing.glb");
    EXPECT_TRUE(AICapabilityRegistry::destructiveReason("export_mesh", {{"output_path", "/tmp/new.glb"}}, exists).isEmpty()) << "a new file is not destructive";
    // any tool with an output-shaped key is covered, not an allowlist
    EXPECT_EQ(AICapabilityRegistry::destructiveReason("generate_mesh_from_image", {{"image_path", "/tmp/existing.glb"}, {"output", "/tmp/existing.glb"}}, exists), "overwrites existing file /tmp/existing.glb");
    EXPECT_TRUE(AICapabilityRegistry::destructiveReason("generate_mesh_from_image", {{"image_path", "/tmp/existing.glb"}, {"output", "/tmp/new.glb"}}, exists).isEmpty()) << "an INPUT path that exists is not an overwrite";
    EXPECT_TRUE(AICapabilityRegistry::destructiveReason("read_file", {{"path", "/tmp/existing.glb"}}, exists).isEmpty()) << "read-only tools never overwrite";
    EXPECT_TRUE(AICapabilityRegistry::destructiveReason("cloud_upload", {{"file_path", "/tmp/existing.glb"}}, exists).isEmpty()) << "ambiguous key on a non-writer is an input";
    EXPECT_EQ(AICapabilityRegistry::destructiveReason("save_scene", {{"path", "/tmp/existing.glb"}}, exists), "overwrites existing file /tmp/existing.glb");
    EXPECT_TRUE(AICapabilityRegistry::destructiveReason("create_primitive", {{"type", "box"}}, exists).isEmpty());
    EXPECT_TRUE(AICapabilityRegistry::destructiveReason("get_scene_info", {}, exists).isEmpty());
}

TEST(AICapabilityRegistry, ReadOnlyToolsNeverOpenTheUndoGroup)
{
    EXPECT_TRUE(AICapabilityRegistry::isReadOnly("get_scene_info"));
    EXPECT_TRUE(AICapabilityRegistry::isReadOnly("list_materials"));
    EXPECT_TRUE(AICapabilityRegistry::isReadOnly("take_screenshot"));
    EXPECT_TRUE(AICapabilityRegistry::isReadOnly("validate_mesh"));
    EXPECT_FALSE(AICapabilityRegistry::isReadOnly("create_primitive"));
    EXPECT_FALSE(AICapabilityRegistry::isReadOnly("auto_rig"));
    EXPECT_FALSE(AICapabilityRegistry::isReadOnly("delete_entity"));
}

TEST(AICapabilityRegistry, TaxonomyOnlyNamesRealCapabilities)
{
    // Every mapped capability id must exist in the display table, else the
    // registry would index out of range when such a tool shows up.
    AICapabilityRegistry reg(sampleTools());
    const auto& tax = AICapabilityRegistry::taxonomy();
    static const QStringList known = {"scene", "scene_io", "view", "materials", "lighting", "textures_ai",
                                      "mesh_optimize", "uv", "rigging", "segmentation", "generation_3d",
                                      "animation", "motion_ai", "morph_pose", "node_animation", "paint",
                                      "mocap", "cloud", "ps1", "other"};
    for (auto it = tax.begin(); it != tax.end(); ++it)
        EXPECT_TRUE(known.contains(it.value())) << it.key().toStdString() << " -> " << it.value().toStdString();
    EXPECT_GT(tax.size(), 120) << "the taxonomy should cover the bulk of the MCP surface";
}
