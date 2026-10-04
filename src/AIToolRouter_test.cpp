// Routing benchmark + unit tests for the BM25 + intent-lexicon tool router.
// The benchmark requests are the ones typed during the first agent sessions
// (2026-09-17) plus paraphrases; every `need_capabilities` round the trace
// log records is a routing miss and belongs in this table.
#include <gtest/gtest.h>

#include "AIToolRouter.h"

namespace {

QVector<AIToolRouter::ToolDoc> corpus()
{
    // A slice of the real MCP surface: names + (abridged) real descriptions.
    return {
        {"get_scene_info", "scene", "Get a summary of the current scene: all scene nodes (with names), entities (with materials), and material count."},
        {"get_mesh_info", "scene", "Get detailed information about loaded meshes: vertex/index counts, submeshes, materials, bounding box, skeleton data."},
        {"create_primitive", "scene", "Create a procedural 3D primitive (box, sphere, cylinder, cone, plane) and add it to the scene. name: node name"},
        {"transform_mesh", "scene", "Set the position, rotation, and/or scale of a named scene node. position [x,y,z] scale [x,y,z] rotation"},
        {"delete_entity", "scene", "Delete an entity from the scene. entity_name"},
        {"duplicate_entity", "scene", "Duplicate an entity/node in the scene, creating a clone with the same mesh, materials, and transform."},
        {"select_entity", "scene", "Select a scene node/entity by name so that tools which act on the selected mesh target it."},
        {"validate_mesh", "scene", "Validate the selected mesh for common issues: degenerate triangles, non-finite UV coordinates."},
        {"load_mesh", "scene_io", "Load a 3D mesh file (obj, fbx, glb, gltf, dae, stl) into the scene. path"},
        {"export_mesh", "scene_io", "Export a mesh entity to a file (glb, gltf, fbx, obj). output_path"},
        {"save_scene", "scene_io", "Save the whole scene to a glTF file. path"},
        {"list_files", "scene_io", "List files in a directory, optionally filtered by extension."},
        {"take_screenshot", "view", "Capture a screenshot of the 3D viewport to a PNG file."},
        {"camera_control", "view", "Control the 3D viewport camera. Set position, look-at target, zoom, or frame the selection."},
        {"create_material", "materials", "Create a new Ogre3D material with optional colors. Colors are [R,G,B] arrays (0.0-1.0): ambient, diffuse, specular, emissive, shininess."},
        {"apply_material", "materials", "Apply a material to a mesh entity in the scene. material, mesh"},
        {"modify_material", "materials", "Modify an existing material's properties: ambient, diffuse, specular, emissive colors, shininess, texture."},
        {"set_texture", "materials", "Bind a texture image to a material's texture unit."},
        {"create_light", "lighting", "Create a scene light (point, directional, spot) with colour and intensity."},
        {"set_hdr_environment", "lighting", "Set the HDR environment (IBL) used for PBR lighting and the skybox."},
        {"generate_pbr_maps", "textures_ai", "AI PBR map synthesis: predict normal, roughness and height maps from a diffuse/albedo texture."},
        {"upscale_texture", "textures_ai", "AI super-resolution (Real-ESRGAN 2x/4x) of a texture image."},
        {"generate_lods", "mesh_optimize", "Generate LOD (Level of Detail) levels for the selected mesh, reducing polygon count at distance."},
        {"decimate_mesh", "mesh_optimize", "Reduce the triangle count of a mesh by a reduction ratio (0..1) or to a target triangle count."},
        {"retopologize", "mesh_optimize", "Quad-dominant retopology of the selected mesh via triangle pairing."},
        {"auto_uv_unwrap", "uv", "Auto UV-unwrap the selected entity via xatlas into UV0."},
        {"auto_rig", "rigging", "Auto-rig the currently selected STATIC (unrigged) mesh by embedding a skeleton template (humanoid, biped, quadruped, generic, vehicle) or UniRig; optionally skin."},
        {"compute_skin_weights", "rigging", "Compute and apply skin weights for the currently selected mesh against its skeleton (skintokens, geodesic-voxel, inverse-distance)."},
        {"remove_skeleton", "rigging", "Remove the ENTIRE skeleton from the currently selected mesh."},
        {"segment_mesh", "segmentation", "AI part segmentation: per-part vertex/face labels (head, torso, arms, legs; vehicle body, wheels)."},
        {"split_mesh_by_segments", "segmentation", "Split the segmented mesh into one named submesh per part."},
        {"generate_mesh_from_image", "generation_3d", "Generate a NEW 3D mesh from a 2D image file (png/jpg) or from a text prompt (TripoSR/TripoSG/TRELLIS.2). image_path, prompt, output"},
        {"list_skeletal_animations", "animation", "List the skeletal animations of an entity."},
        {"play_animation", "animation", "Play or stop a skeletal animation on an entity."},
        {"add_keyframe", "animation", "Add a keyframe to a bone track at a time."},
        {"merge_animations", "animation", "Merge animations from other files into the entity's skeleton."},
        {"resample_animation", "animation", "Resample an animation to N keyframes."},
        {"simplify_animation", "animation", "Remove redundant keyframes from an animation."},
        {"bake_animation_fps", "animation", "Re-grid every track to a uniform FPS."},
        {"generate_isometric_sprites", "animation", "Render an 8-direction isometric sprite atlas (optionally animated) to PNG."},
        {"generate_motion", "motion_ai", "Text-to-motion: generate a skeletal animation from a text prompt (walk, run, jump, dance, wave, idle...) on a humanoid rig."},
        {"motion_in_between", "motion_ai", "AI in-betweening: fill the gap between keyframes with plausible poses."},
        {"set_morph_weight", "morph_pose", "Set a morph target (blend shape) weight on the selected entity."},
        {"add_node_animation_clip", "node_animation", "Create a node transform (TRS) animation clip on a scene node (spinning props, doors)."},
        {"paint_set_enabled", "paint", "Enter or leave texture-paint mode on the selected mesh."},
        {"capture_face_from_video", "mocap", "Facial performance capture from a video file to blendshape keyframes."},
        {"cloud_upload", "cloud", "Upload a project to QtMesh Cloud."},
    };
}

// topTool: the tool expected on top; "a|b" accepts either (both are correct
// first moves — e.g. a colour change may create or modify a material).
struct Case { const char* request; const char* mustInclude; const char* topTool; };

} // namespace

TEST(AIToolRouter, TokenizerStemsAndDropsStopwords)
{
    const QStringList t = AIToolRouter::tokenize("Rigging the Materials of auto_rig, please!");
    EXPECT_TRUE(t.contains("rig")) << t.join(",").toStdString();
    EXPECT_TRUE(t.contains("material"));
    EXPECT_TRUE(t.contains("auto"));
    EXPECT_FALSE(t.contains("rigging")) << "stemmed";
    EXPECT_FALSE(t.contains("the"));
    EXPECT_FALSE(t.contains("please"));
    EXPECT_EQ(AIToolRouter::tokenize("f22"), QStringList({"f22"}));
}

TEST(AIToolRouter, IntentLexiconTranslatesUserWordsIntoToolVocabulary)
{
    const QStringList green = AIToolRouter::expandIntent(AIToolRouter::tokenize("make it green"));
    EXPECT_TRUE(green.contains("material")) << green.join(",").toStdString();
    EXPECT_TRUE(green.contains(AIToolRouter::tokenize("diffuse").first())) << "terms are stored canonically stemmed";
    const QStringList create = AIToolRouter::expandIntent(AIToolRouter::tokenize("create a f22 raptor scene"));
    EXPECT_TRUE(create.contains(AIToolRouter::tokenize("generate").first()));
    EXPECT_TRUE(create.contains("prompt"));
    EXPECT_TRUE(AIToolRouter::expandIntent(AIToolRouter::tokenize("xyzzy")).isEmpty());
    // generic verbs expand weakly, specific words strongly
    const auto w = AIToolRouter::expandIntentWeighted(AIToolRouter::tokenize("make it green"));
    EXPECT_LT(w.value(AIToolRouter::tokenize("generate").first()), w.value("material")) << "'make' must not outweigh 'green'";
    // stems are canonical on both sides: dance/dancing agree
    EXPECT_EQ(AIToolRouter::tokenize("dancing"), AIToolRouter::tokenize("dance"));
    EXPECT_EQ(AIToolRouter::tokenize("creating images"), AIToolRouter::tokenize("create image"));
}

TEST(AIToolRouter, RoutingBenchmark)
{
    AIToolRouter router(corpus());
    // request → a capability the route MUST include, and the tool expected on top
    const Case cases[] = {
        {"create a f22 raptor scene",                          "generation_3d", "generate_mesh_from_image"},
        {"make me a dragon",                                   "generation_3d", "generate_mesh_from_image"},
        {"generate a 3d model from this photo",                "generation_3d", "generate_mesh_from_image"},
        {"make it green",                                      "materials",     "create_material|modify_material|apply_material"},
        {"change the material of luigi to red",                "materials",     nullptr},
        {"give the car a shiny metal look",                    "materials",     nullptr},
        {"rig the wolf as a quadruped and skin it",            "rigging",       "auto_rig"},
        {"compute skin weights",                               "rigging",       "compute_skin_weights"},
        {"remove the skeleton",                                "rigging",       "remove_skeleton"},
        {"make the box twice as large",                        "scene",         "transform_mesh"},
        {"move the crate 2 units to the left",                 "scene",         "transform_mesh"},
        {"delete the cube",                                    "scene",         "delete_entity"},
        {"duplicate the sphere",                               "scene",         "duplicate_entity"},
        {"what is in the scene?",                              "scene",         "get_scene_info"},
        {"how many triangles does the mesh have",              "scene",         "get_mesh_info"},
        {"export luigi as glb",                                "scene_io",      "export_mesh"},
        {"load the wolf obj",                                  "scene_io",      "load_mesh"},
        {"save the scene",                                     "scene_io",      "save_scene"},
        {"take a screenshot",                                  "view",          "take_screenshot"},
        {"show me the model from the front",                   "view",          nullptr},
        {"make it walk",                                       "motion_ai",     "generate_motion"},
        {"animate the character dancing",                      "motion_ai",     nullptr},
        {"reduce the polygon count by half",                   "mesh_optimize", nullptr},
        {"generate 3 lods",                                    "mesh_optimize", "generate_lods"},
        {"unwrap the uvs",                                     "uv",            "auto_uv_unwrap"},
        {"segment the body into parts",                        "segmentation",  nullptr},
        {"add a point light above the scene",                  "lighting",      "create_light"},
        {"upscale the texture 4x",                             "textures_ai",   "upscale_texture"},
        {"generate pbr maps from the albedo",                  "textures_ai",   "generate_pbr_maps"},
        // non-English requests are the LLM intent round's job (AgentFixture), not the lexical router's
    };
    int capHits = 0, toolHits = 0, toolCases = 0;
    QStringList misses;
    for (const Case& c : cases) {
        const auto r = router.route(QString::fromUtf8(c.request));
        const bool cap = r.capabilities.contains(QLatin1String(c.mustInclude));
        if (cap) ++capHits; else misses << QStringLiteral("%1 → %2 (wanted %3)").arg(c.request, r.capabilities.join("+"), c.mustInclude);
        if (c.topTool) {
            ++toolCases;
            const QStringList accepted = QString::fromLatin1(c.topTool).split('|');
            if (!r.scores.isEmpty() && accepted.contains(r.scores.first().name)) ++toolHits;
            else {
                QStringList top;
                for (int i = 0; i < std::min(3, int(r.scores.size())); ++i) top << QStringLiteral("%1=%2").arg(r.scores[i].name).arg(r.scores[i].score, 0, 'f', 2);
                misses << QStringLiteral("%1 → top tool %2 (wanted %3) [%4] +%5").arg(c.request, r.scores.isEmpty() ? "-" : r.scores.first().name, c.topTool, top.join(" "), r.expandedTerms.join(","));
            }
        }
    }
    const int total = static_cast<int>(sizeof(cases) / sizeof(cases[0]));
    EXPECT_EQ(capHits, total) << "capability misses:\n  " << misses.join("\n  ").toStdString();
    EXPECT_GE(toolHits * 100 / std::max(1, toolCases), 85) << "top-tool misses:\n  " << misses.join("\n  ").toStdString();
    RecordProperty("capability_hits", capHits);
    RecordProperty("top_tool_hits", toolHits);
}

TEST(AIToolRouter, SceneIsAlwaysKeptAndCapabilitiesAreCapped)
{
    AIToolRouter router(corpus());
    const auto r = router.route("rig it, skin it, animate it walking, export it, light it and unwrap the uvs", 3);
    EXPECT_EQ(r.capabilities.size(), 3);
    EXPECT_TRUE(r.capabilities.contains("scene")) << r.capabilities.join(",").toStdString();
    EXPECT_TRUE(router.route("").capabilities.contains("scene")) << "an empty request still routes to scene";
}

TEST(AIToolRouter, ShortlistPrunesLargeCapabilitiesToTheRelevantTools)
{
    AIToolRouter router(corpus());
    const auto r = router.route("resample the walk animation to 30 keyframes");
    // animation has 8 tools in this corpus (> keepAll=4) → only the scoring ones, best first
    const QStringList anim = router.shortlist(r, "animation", /*keepAll=*/4, /*maxTools=*/3);
    ASSERT_FALSE(anim.isEmpty());
    EXPECT_LE(anim.size(), 3);
    EXPECT_EQ(anim.first(), "resample_animation") << anim.join(",").toStdString();
    // a small capability is always shown whole
    EXPECT_EQ(router.shortlist(r, "uv", 4, 3), QStringList({"auto_uv_unwrap"}));
    // a large capability nothing matched is shown whole rather than hidden
    EXPECT_EQ(router.shortlist(router.route("xyzzy"), "animation", 4, 3).size(), 8);
}
