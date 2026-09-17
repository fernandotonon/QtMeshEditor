#include "AICapabilityRegistry.h"

#include <QFileInfo>
#include <array>
#include <cmath>
#include <vector>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace {

struct CapDef { const char* id; const char* title; const char* description; };

// Display order = the order the planner reads them in. Everyday scene work
// first, heavy AI last.
const std::vector<CapDef> kCapDefs = {
    {"scene",          "Scene & objects",       "inspect the scene, create primitives, move/scale/rotate, duplicate, delete, group, validate meshes"},
    {"scene_io",       "Files & import/export", "load meshes, export/save scenes and poses, list/search/read files"},
    {"view",           "Camera & screenshots",  "camera moves, screenshots of the viewport, debug overlays (skeleton, normals, weights, info)"},
    {"materials",      "Materials & textures",  "create/modify/apply materials and presets, list and bind textures"},
    {"lighting",       "Lighting & environment","scene lights, light rigs, HDR environment, tonemap"},
    {"textures_ai",    "AI texture tools",      "AI texture generation, PBR maps from a diffuse, upscaling, photo depth, inpainting, channel packing, atlases"},
    {"mesh_optimize",  "Mesh optimisation",     "LODs, decimation, retopology, vertex welding, vertex-cache/draw-call optimisation"},
    {"uv",             "UV mapping",            "auto-unwrap, UV info, projections, seams"},
    {"rigging",        "Rigging & skinning",    "auto-rig a static mesh (humanoid/biped/quadruped/generic/vehicle templates or UniRig), compute skin weights, ARKit face blendshapes, remove skeleton"},
    {"segmentation",   "Part segmentation",     "AI part segmentation, split/explode/join mesh parts"},
    {"generation_3d",  "Image/prompt → 3D",     "generate a 3D mesh from an image or a text prompt (TripoSR/TripoSG/TRELLIS.2)"},
    {"animation",      "Skeletal animation",    "list/play/edit animations and keyframes, merge/resample/simplify/trim/bake, isometric sprites, VAT"},
    {"motion_ai",      "AI motion",             "text-to-motion generation, in-betweening, arm-space, foot pinning"},
    {"morph_pose",     "Morph targets & poses", "morph target weights and keyframes, pose library"},
    {"node_animation", "Node animation",        "transform (TRS) animation clips on scene nodes"},
    {"paint",          "Texture painting",      "paint layers/channels/brushes, stencil projection, bake painted PBR sets"},
    {"mocap",          "Performance capture",   "face/body capture from video or webcam"},
    {"cloud",          "QtMesh Cloud",          "cloud login/status/limits, project list/upload/delete"},
    {"ps1",            "PS1 ripper",            "PlayStation runtime model extraction"},
    {"other",          "Other tools",           "tools without a category"},
};

QHash<QString, QString> buildTaxonomy()
{
    QHash<QString, QString> t;
    auto add = [&](const char* cap, std::initializer_list<const char*> tools) {
        for (const char* n : tools) t.insert(QLatin1String(n), QLatin1String(cap));
    };
    add("scene", {"get_scene_info", "get_mesh_info", "select_entity", "create_primitive", "delete_entity", "duplicate_entity",
                  "transform_mesh", "transform_submesh", "group_nodes", "ungroup_node", "reparent_node",
                  "set_pivot_mode", "get_pivot_mode", "set_snap_settings", "get_snap_settings",
                  "validate_mesh", "get_memory_usage", "analyze_draw_calls"});
    add("scene_io", {"load_mesh", "export_mesh", "export_pose", "save_scene", "open_scene", "import_alembic",
                     "list_files", "search_files", "read_file"});
    add("view", {"take_screenshot", "get_camera_info", "camera_control", "toggle_skeleton_debug",
                 "toggle_bone_weights", "toggle_normals", "toggle_mesh_info"});
    add("materials", {"create_material", "modify_material", "get_material", "list_materials", "apply_material",
                      "list_material_presets", "apply_material_preset", "describe_material", "list_textures",
                      "set_texture"});
    add("lighting", {"create_light", "delete_light", "list_lights", "set_light_property", "apply_light_rig",
                     "set_hdr_environment", "get_hdr_environment", "set_tonemap", "set_env_intensity",
                     "set_env_tint"});
    add("textures_ai", {"generate_mesh_texture", "generate_pbr_maps", "upscale_texture", "photo_depth",
                        "inpaint_texture", "generate_normal_map", "pack_textures", "pack_atlas", "apply_atlas"});
    add("mesh_optimize", {"generate_lods", "generate_auto_lods", "remove_lods", "get_lod_info",
                          "optimize_vertex_cache", "retopologize", "decimate_mesh", "weld_vertices",
                          "optimize_mesh"});
    add("uv", {"auto_uv_unwrap", "uv_info", "uv_project", "uv_set_seams", "uv_unwrap_selection"});
    add("rigging", {"auto_rig", "compute_skin_weights", "set_skinning_display", "remove_skeleton",
                    "add_arkit_blendshapes"});
    add("segmentation", {"segment_mesh", "split_mesh_by_segments", "explode_mesh_parts", "join_mesh_parts"});
    add("generation_3d", {"generate_mesh_from_image"});
    add("animation", {"animate", "list_skeletal_animations", "get_animation_info", "set_animation_length",
                      "set_animation_time", "add_keyframe", "remove_keyframe", "play_animation",
                      "merge_animations", "resample_animation", "simplify_animation", "analyze_animation",
                      "bake_animation_fps", "trim_animation", "set_playback_speed", "set_loop_region",
                      "get_playback_state", "select_animation", "select_bone", "set_keyframe_value",
                      "move_bone_keyframe", "step_keyframe", "get_channel_values", "play_vertex_animation",
                      "bake_vat", "generate_isometric_sprites"});
    add("motion_ai", {"motion_in_between", "generate_motion", "adjust_arm_space", "pin_feet"});
    add("morph_pose", {"list_morph_targets", "set_morph_weight", "set_morph_weight_keyframe",
                       "clear_morph_weight_keyframe", "list_poses", "save_pose", "apply_pose", "delete_pose",
                       "mirror_pose", "save_pose_library", "load_pose_library", "apply_pose_masked"});
    add("node_animation", {"list_node_animations", "add_node_animation_clip", "set_node_keyframe",
                           "set_node_animation_playing", "delete_node_animation_clip", "move_node_keyframe",
                           "delete_node_keyframe", "get_node_animation"});
    add("mocap", {"capture_face_from_video", "capture_body_from_video", "list_capture_devices",
                  "start_live_capture", "set_capture_channels", "stop_live_capture"});
    return t;
}

QString capForPrefix(const QString& tool)
{
    if (tool.startsWith(QLatin1String("paint_")))  return QStringLiteral("paint");
    if (tool.startsWith(QLatin1String("cloud_")))  return QStringLiteral("cloud");
    if (tool.startsWith(QLatin1String("ps1rip_"))) return QStringLiteral("ps1");
    return {};
}

struct Keyword { const char* word; const char* cap; };
const std::vector<Keyword> kKeywords = {
    {"rig", "rigging"}, {"skeleton", "rigging"}, {"skin", "rigging"}, {"bone", "rigging"}, {"weights", "rigging"},
    {"blendshape", "rigging"}, {"arkit", "rigging"}, {"unirig", "rigging"},
    {"segment", "segmentation"}, {"parts", "segmentation"}, {"split", "segmentation"}, {"explode", "segmentation"},
    {"join", "segmentation"},
    {"anim", "animation"}, {"keyframe", "animation"}, {"play", "animation"}, {"clip", "animation"},
    {"sprite", "animation"}, {"isometric", "animation"},
    {"walk", "motion_ai"}, {"run", "motion_ai"}, {"dance", "motion_ai"}, {"idle", "motion_ai"}, {"motion", "motion_ai"},
    {"wave", "motion_ai"}, {"jump", "motion_ai"}, {"in-between", "motion_ai"}, {"inbetween", "motion_ai"},
    {"material", "materials"}, {"color", "materials"}, {"colour", "materials"}, {"shiny", "materials"},
    {"metal", "materials"}, {"texture", "materials"}, {"glossy", "materials"}, {"matte", "materials"},
    {"pbr", "textures_ai"}, {"upscale", "textures_ai"}, {"inpaint", "textures_ai"}, {"normal map", "textures_ai"},
    {"roughness", "textures_ai"}, {"atlas", "textures_ai"}, {"depth", "textures_ai"},
    {"light", "lighting"}, {"hdr", "lighting"}, {"environment", "lighting"}, {"shadow", "lighting"}, {"tonemap", "lighting"},
    {"lod", "mesh_optimize"}, {"decimate", "mesh_optimize"}, {"simplify", "mesh_optimize"}, {"retopo", "mesh_optimize"},
    {"optimi", "mesh_optimize"}, {"weld", "mesh_optimize"}, {"triangle", "mesh_optimize"}, {"polycount", "mesh_optimize"},
    {"uv", "uv"}, {"unwrap", "uv"}, {"seam", "uv"},
    {"generate", "generation_3d"}, {"image to 3d", "generation_3d"}, {"from image", "generation_3d"},
    {"from photo", "generation_3d"}, {"prompt", "generation_3d"}, {"create a 3d", "generation_3d"}, {"model of", "generation_3d"},
    {"load", "scene_io"}, {"import", "scene_io"}, {"open", "scene_io"}, {"export", "scene_io"}, {"save", "scene_io"},
    {"file", "scene_io"}, {".glb", "scene_io"}, {".fbx", "scene_io"}, {".obj", "scene_io"}, {"folder", "scene_io"},
    {"screenshot", "view"}, {"camera", "view"}, {"look at", "view"}, {"render", "view"}, {"show me", "view"},
    {"zoom", "view"}, {"normals", "view"},
    {"morph", "morph_pose"}, {"pose", "morph_pose"}, {"expression", "morph_pose"},
    {"paint", "paint"}, {"brush", "paint"}, {"layer", "paint"}, {"stencil", "paint"},
    {"mocap", "mocap"}, {"webcam", "mocap"}, {"video", "mocap"}, {"capture", "mocap"},
    {"cloud", "cloud"}, {"upload", "cloud"},
    {"ps1", "ps1"}, {"playstation", "ps1"},
    {"node anim", "node_animation"}, {"spin", "node_animation"}, {"rotate over time", "node_animation"},
    {"delete", "scene"}, {"remove", "scene"}, {"move", "scene"}, {"scale", "scene"}, {"rotate", "scene"},
    {"duplicate", "scene"}, {"copy", "scene"}, {"box", "scene"}, {"cube", "scene"}, {"sphere", "scene"},
    {"cylinder", "scene"}, {"plane", "scene"}, {"primitive", "scene"}, {"validate", "scene"}, {"check", "scene"},
    {"scene", "scene"}, {"select", "scene"}, {"bigger", "scene"}, {"smaller", "scene"}, {"larger", "scene"},
    {"twice", "scene"}, {"half", "scene"}, {"mesh", "scene"}, {"object", "scene"}, {"info", "scene"},
};

} // namespace

const QHash<QString, QString>& AICapabilityRegistry::taxonomy()
{
    static const QHash<QString, QString> t = buildTaxonomy();
    return t;
}

AICapabilityRegistry::AICapabilityRegistry(const QJsonArray& toolList)
{
    for (const CapDef& d : kCapDefs) {
        m_capIndex.insert(QLatin1String(d.id), static_cast<int>(m_capabilities.size()));
        m_capabilities.push_back({QLatin1String(d.id), QLatin1String(d.title), QLatin1String(d.description), {}});
    }
    for (const QJsonValue& v : toolList) {
        const QJsonObject t = v.toObject();
        const QString name = t["name"].toString();
        if (name.isEmpty()) continue;
        m_tools.insert(name, {name, t["description"].toString(), t["inputSchema"].toObject()});
        QString cap = taxonomy().value(name);
        if (cap.isEmpty()) cap = capForPrefix(name);
        if (cap.isEmpty()) cap = QStringLiteral("other");
        m_capabilities[m_capIndex.value(cap)].tools << name;
    }
}

QStringList AICapabilityRegistry::capabilityIds() const
{
    QStringList ids;
    for (const Capability& c : m_capabilities)
        if (!c.tools.isEmpty()) ids << c.id;
    return ids;
}

const AICapabilityRegistry::Capability* AICapabilityRegistry::capability(const QString& id) const
{
    const auto it = m_capIndex.constFind(id);
    if (it == m_capIndex.constEnd()) return nullptr;
    const Capability& c = m_capabilities[it.value()];
    return c.tools.isEmpty() ? nullptr : &c;
}

QString AICapabilityRegistry::capabilityOf(const QString& tool) const
{
    if (!m_tools.contains(tool)) return {};
    QString cap = taxonomy().value(tool);
    if (cap.isEmpty()) cap = capForPrefix(tool);
    return cap.isEmpty() ? QStringLiteral("other") : cap;
}

QJsonObject AICapabilityRegistry::schemaFor(const QString& tool) const
{
    return m_tools.value(tool).schema;
}

QStringList AICapabilityRegistry::toolsFor(const QStringList& capabilityIds) const
{
    QStringList out;
    for (const QString& id : capabilityIds) {
        const Capability* c = capability(id);
        if (!c) continue;
        for (const QString& t : c->tools) {
            if (!out.contains(t)) out << t;
        }
    }
    return out;
}

QString AICapabilityRegistry::promptIndex() const
{
    QString s;
    for (const Capability& c : m_capabilities) {
        if (c.tools.isEmpty()) continue;
        s += QStringLiteral("- %1: %2 (%3 tools)\n").arg(c.id, c.description).arg(c.tools.size());
    }
    return s;
}

QString AICapabilityRegistry::toolDoc(const QString& tool) const
{
    const auto it = m_tools.constFind(tool);
    if (it == m_tools.constEnd()) return {};
    const ToolInfo& info = it.value();
    QString desc = info.description.simplified();
    // One line: everything up to the first sentence end, capped.
    const qsizetype dot = desc.indexOf(QLatin1String(". "));
    if (dot > 40) desc = desc.left(dot + 1);
    if (desc.size() > 220) desc = desc.left(217) + QLatin1String("...");
    QString s = QStringLiteral("- %1: %2\n").arg(info.name, desc);
    const QJsonObject props = info.schema["properties"].toObject();
    QSet<QString> required;
    for (const QJsonValue& r : info.schema["required"].toArray()) required.insert(r.toString());
    for (auto p = props.begin(); p != props.end(); ++p) {
        const QJsonObject def = p.value().toObject();
        QString pd = def["description"].toString().simplified();
        if (pd.size() > 140) pd = pd.left(137) + QLatin1String("...");
        QString type = def["type"].toString();
        if (def.contains("enum")) {
            QStringList vals;
            for (const QJsonValue& e : def["enum"].toArray()) vals << e.toVariant().toString();
            type = QStringLiteral("one of %1").arg(vals.join('|'));
        }
        s += QStringLiteral("    %1 (%2%3): %4\n")
                 .arg(p.key(), type.isEmpty() ? QStringLiteral("any") : type,
                      required.contains(p.key()) ? QStringLiteral(", required") : QString(), pd);
    }
    return s;
}

QString AICapabilityRegistry::promptToolsFor(const QStringList& capabilityIds) const
{
    QString s;
    for (const QString& t : toolsFor(capabilityIds)) s += toolDoc(t);
    return s;
}

namespace {
// Whole-word-ish match for short keys, plain substring for phrases/extensions.
bool keywordHits(const QString& word, const QString& request)
{
    if (word.contains(' ') || word.startsWith('.')) return request.contains(word);
    static QHash<QString, QRegularExpression> cache;
    auto it = cache.find(word);
    if (it == cache.end())
        it = cache.insert(word, QRegularExpression(QStringLiteral("\\b%1").arg(QRegularExpression::escape(word))));
    return it->match(request).hasMatch();
}
} // namespace

QStringList AICapabilityRegistry::routeByKeywords(const QString& request) const
{
    const QString r = request.toLower();
    QHash<QString, int> score;
    for (const Keyword& k : kKeywords) {
        const QString w = QLatin1String(k.word);
        if (keywordHits(w, r)) score[QLatin1String(k.cap)] += (w.size() >= 6 ? 2 : 1);
    }
    QStringList caps;
    for (const Capability& c : m_capabilities)
        if (!c.tools.isEmpty() && score.value(c.id) > 0) caps << c.id;
    std::stable_sort(caps.begin(), caps.end(), [&](const QString& a, const QString& b) {
        return score.value(a) > score.value(b);
    });
    if (caps.size() > 4) caps = caps.mid(0, 4);
    if (!caps.contains(QStringLiteral("scene")) && capability(QStringLiteral("scene")))
        caps << QStringLiteral("scene");   // inspection tools are always useful
    return caps;
}

namespace {

// Each coercer returns false with `why` set when the value cannot become the
// schema type; on success `v` holds the (possibly coerced) value and a
// warning is appended when a coercion happened.
bool coerceString(QJsonValue& v, const QString& key, QStringList* warnings, QString* why)
{
    if (v.isString() || v.isNull()) return true;
    if (!v.isDouble() && !v.isBool()) { *why = QStringLiteral("expected a string"); return false; }
    v = v.toVariant().toString();
    if (warnings) *warnings << QStringLiteral("'%1' coerced to string").arg(key);
    return true;
}

bool coerceNumber(QJsonValue& v, const QString& key, bool integer, QStringList* warnings, QString* why)
{
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().trimmed().toDouble(&ok);
        if (!ok) { *why = QStringLiteral("expected a number, got '%1'").arg(v.toString()); return false; }
        v = d;
        if (warnings) *warnings << QStringLiteral("'%1' coerced to number").arg(key);
    } else if (!v.isDouble()) {
        *why = QStringLiteral("expected a number");
        return false;
    }
    if (!integer) return true;
    const double number = v.toDouble();
    constexpr double kMaxExclusive = 9223372036854775808.0;   // 2^63
    const bool integral = std::isfinite(number) && std::trunc(number) == number
                       && number >= -kMaxExclusive && number < kMaxExclusive;
    if (!integral) { *why = QStringLiteral("expected an integer"); return false; }
    return true;
}

// `items.type` of an array schema (string/number/integer/boolean): every
// element must match (review finding — a wrong-typed element used to pass).
bool checkArrayItems(const QJsonValue& v, const QJsonObject& def, QString* why)
{
    const QString itemType = def["items"].toObject()["type"].toString();
    if (itemType.isEmpty()) return true;
    const QJsonArray arr = v.toArray();
    for (qsizetype i = 0; i < arr.size(); ++i) {
        const QJsonValue e = arr.at(i);
        bool ok = true;
        if (itemType == QLatin1String("string")) ok = e.isString();
        else if (itemType == QLatin1String("number") || itemType == QLatin1String("integer")) ok = e.isDouble();
        else if (itemType == QLatin1String("boolean")) ok = e.isBool();
        else if (itemType == QLatin1String("object")) ok = e.isObject();
        if (!ok) { *why = QStringLiteral("item %1: expected %2").arg(i + 1).arg(itemType); return false; }
    }
    return true;
}

bool coerceBoolean(QJsonValue& v, const QString& key, QStringList* warnings, QString* why)
{
    if (v.isBool()) return true;
    if (v.isDouble()) { v = (v.toDouble() != 0.0); return true; }
    if (!v.isString()) { *why = QStringLiteral("expected a boolean"); return false; }
    static const QStringList yes = {"true", "yes", "1"};
    static const QStringList no  = {"false", "no", "0"};
    const QString s = v.toString().trimmed().toLower();
    if (yes.contains(s)) v = true;
    else if (no.contains(s)) v = false;
    else { *why = QStringLiteral("expected true/false"); return false; }
    if (warnings) *warnings << QStringLiteral("'%1' coerced to boolean").arg(key);
    return true;
}

// A colour NAME where an [R,G,B] array is expected — small models write
// "diffuse": "red" — becomes the array, with a warning.
bool colourNameToRgb(const QString& name, QJsonArray* out)
{
    static const QHash<QString, QJsonArray> colours = {
        {"red", {1.0, 0.0, 0.0}}, {"green", {0.0, 1.0, 0.0}}, {"blue", {0.0, 0.0, 1.0}},
        {"white", {1.0, 1.0, 1.0}}, {"black", {0.0, 0.0, 0.0}}, {"yellow", {1.0, 1.0, 0.0}},
        {"orange", {1.0, 0.5, 0.0}}, {"purple", {0.5, 0.0, 0.5}}, {"magenta", {1.0, 0.0, 1.0}},
        {"cyan", {0.0, 1.0, 1.0}}, {"gray", {0.5, 0.5, 0.5}}, {"grey", {0.5, 0.5, 0.5}},
        {"brown", {0.4, 0.25, 0.1}}, {"pink", {1.0, 0.6, 0.8}}, {"gold", {1.0, 0.84, 0.0}},
        {"silver", {0.75, 0.75, 0.75}},
    };
    const auto it = colours.constFind(name.trimmed().toLower());
    if (it == colours.constEnd()) return false;
    *out = it.value();
    return true;
}

bool coerceArray(QJsonValue& v, const QString& key, QStringList* warnings, QString* why)
{
    if (v.isArray()) return true;
    if (!v.isString()) { *why = QStringLiteral("expected an array"); return false; }
    QJsonArray rgb;
    if (colourNameToRgb(v.toString(), &rgb)) {
        v = rgb;
        if (warnings) *warnings << QStringLiteral("'%1': colour name '%2' → RGB").arg(key, v.toArray().isEmpty() ? QString() : v.toArray().first().toString());
        return true;
    }
    // "[1, 2, 3]" or "1,2,3" from a chatty model
    QString s = v.toString().trimmed();
    if (!s.startsWith('[')) s = '[' + s + ']';
    const QJsonDocument d = QJsonDocument::fromJson(s.toUtf8());
    if (!d.isArray()) { *why = QStringLiteral("expected an array"); return false; }
    v = d.array();
    if (warnings) *warnings << QStringLiteral("'%1' parsed into an array").arg(key);
    return true;
}

bool coerceToType(QJsonValue& v, const QString& key, const QString& type, QStringList* warnings, QString* why)
{
    if (type == QLatin1String("string"))  return coerceString(v, key, warnings, why);
    if (type == QLatin1String("number"))  return coerceNumber(v, key, false, warnings, why);
    if (type == QLatin1String("integer")) return coerceNumber(v, key, true, warnings, why);
    if (type == QLatin1String("boolean")) return coerceBoolean(v, key, warnings, why);
    if (type == QLatin1String("array"))   return coerceArray(v, key, warnings, why);
    if (type == QLatin1String("object") && !v.isObject()) { *why = QStringLiteral("expected an object"); return false; }
    return true;   // untyped or object
}

// camelCase → snake_case ("materialName" → "material_name").
QString snakeCase(const QString& key)
{
    QString out;
    for (const QChar c : key) {
        if (c.isUpper()) { out += '_'; out += c.toLower(); }
        else out += c;
    }
    return out;
}

// Small models rename parameters freely ("material_name" for "material",
// "entity" for "mesh"). Before rejecting a call for a MISSING required key,
// move a recognisable alias onto the schema's name — the MCP handlers accept
// several of these spellings anyway, so the validator must not be stricter
// than the tool. Only keys that are NOT themselves schema properties move.
QJsonObject normaliseAliases(const QJsonObject& props, const QJsonObject& args, QStringList* warnings)
{
    static const QHash<QString, QStringList> synonyms = {
        {"material",    {"material_name", "mat", "material_id"}},
        {"mesh",        {"mesh_name", "entity", "entity_name", "node", "node_name", "object", "target"}},
        {"entity_name", {"entity", "mesh", "mesh_name", "node", "node_name", "object", "name"}},
        {"name",        {"entity_name", "node_name", "mesh_name", "entity", "mesh", "object", "material_name"}},
        {"path",        {"file", "file_path", "filepath", "output", "output_path"}},
        {"output_path", {"output", "out", "path", "file", "file_path", "destination"}},
        {"template",    {"skeleton", "skeleton_type", "rig_template"}},
        {"type",        {"primitive", "primitive_type", "kind", "shape"}},
    };
    QJsonObject out;
    // pass 1: snake_case every key that is not a property as spelled
    for (auto a = args.begin(); a != args.end(); ++a) {
        QString key = a.key();
        if (!props.contains(key)) {
            const QString snake = snakeCase(key);
            if (snake != key) {
                if (warnings && props.contains(snake)) *warnings << QStringLiteral("'%1' read as '%2'").arg(key, snake);
                key = snake;
            }
        }
        out[key] = a.value();
    }
    // pass 2: fill missing schema keys from aliases the model used instead
    for (auto p = props.begin(); p != props.end(); ++p) {
        const QString want = p.key();
        if (out.contains(want)) continue;
        for (const QString& alias : synonyms.value(want)) {
            if (!out.contains(alias) || props.contains(alias)) continue;
            out[want] = out.take(alias);
            if (warnings) *warnings << QStringLiteral("'%1' read as '%2'").arg(alias, want);
            break;
        }
    }
    return out;
}

bool matchEnum(QJsonValue& v, const QJsonObject& def, QString* why)
{
    if (!def.contains("enum")) return true;
    QStringList vals;
    for (const QJsonValue& e : def["enum"].toArray()) {
        if (e.toVariant().toString() == v.toVariant().toString()) { v = e; return true; }
        vals << e.toVariant().toString();
    }
    *why = QStringLiteral("'%1' is not one of %2").arg(v.toVariant().toString(), vals.join('|'));
    return false;
}

} // namespace

bool AICapabilityRegistry::validateArguments(const QString& tool, const QJsonObject& rawArgs,
                                             QJsonObject* out, QString* error,
                                             QStringList* warnings) const
{
    const auto it = m_tools.constFind(tool);
    if (it == m_tools.constEnd()) {
        if (error) *error = QStringLiteral("unknown tool '%1'").arg(tool);
        return false;
    }
    const QJsonObject schema = it->schema;
    const QJsonObject props = schema["properties"].toObject();
    const QJsonObject args = normaliseAliases(props, rawArgs, warnings);

    for (const QJsonValue& r : schema["required"].toArray()) {
        const QString key = r.toString();
        if (args.contains(key) && !args[key].isNull()) continue;
        if (error) {
            QStringList passed = args.keys();
            *error = QStringLiteral("missing required argument '%1' (you passed: %2)")
                         .arg(key, passed.isEmpty() ? QStringLiteral("nothing") : passed.join(", "));
        }
        return false;
    }

    QJsonObject result;
    for (auto a = args.begin(); a != args.end(); ++a) {
        const QString key = a.key();
        QJsonValue v = a.value();
        if (!props.contains(key)) {
            if (warnings) *warnings << QStringLiteral("unknown argument '%1' passed through").arg(key);
            result[key] = v;
            continue;
        }
        const QJsonObject def = props[key].toObject();
        QString why;
        const bool valid = coerceToType(v, key, def["type"].toString(), warnings, &why)
                        && matchEnum(v, def, &why)
                        && (!v.isArray() || checkArrayItems(v, def, &why));
        if (!valid) {
            if (error) *error = QStringLiteral("argument '%1': %2").arg(key, why);
            return false;
        }
        result[key] = v;
    }
    if (out) *out = result;
    return true;
}

namespace {

// Tools that delete data or rewrite geometry, with the argument naming the target.
QString deleteReason(const QString& tool, const QJsonObject& args)
{
    static const QHash<QString, QString> deletes = {
        {"delete_entity", "entity_name"}, {"delete_light", "name"}, {"remove_skeleton", "entity_name"},
        {"remove_lods", "entity_name"}, {"delete_pose", "name"}, {"delete_node_animation_clip", "clip"},
        {"delete_node_keyframe", "clip"}, {"remove_keyframe", "animation"}, {"clear_morph_weight_keyframe", "name"},
        {"cloud_delete_project", "project_id"}, {"ps1rip_clear", ""}, {"paint_delete_layer", "index"},
        {"paint_flatten", ""}, {"ungroup_node", "name"}, {"split_mesh_by_segments", "entity_name"},
        {"explode_mesh_parts", "entity_name"}, {"join_mesh_parts", ""}, {"decimate_mesh", "entity_name"},
        {"retopologize", "entity_name"}, {"weld_vertices", "entity_name"},
    };
    const auto it = deletes.constFind(tool);
    if (it == deletes.constEnd()) return {};
    QString target;
    if (!it.value().isEmpty()) target = args[it.value()].toVariant().toString();
    static const std::array<const char*, 5> kTargetKeys = {"entity_name", "name", "mesh", "node", "clip"};
    for (const char* k : kTargetKeys) {
        if (!target.isEmpty()) break;
        if (args.contains(QLatin1String(k))) target = args[QLatin1String(k)].toVariant().toString();
    }
    const bool isDelete = tool.startsWith(QLatin1String("delete")) || tool.startsWith(QLatin1String("remove"))
                       || tool.startsWith(QLatin1String("clear"));
    QString r;
    if (target.isEmpty())
        r = QStringLiteral("%1 %2").arg(tool, isDelete ? QStringLiteral("deletes data") : QStringLiteral("rewrites geometry"));
    else
        r = QStringLiteral("%1 '%2'").arg(isDelete ? QStringLiteral("deletes") : QStringLiteral("rewrites the geometry of"), target);
    const bool irreversible = tool.startsWith(QLatin1String("cloud_")) || tool.startsWith(QLatin1String("ps1rip_"));
    if (irreversible) r += QStringLiteral(" (not undoable)");
    return r;
}

// Overwrites: ANY non-read-only tool whose arguments name an OUTPUT path
// that already exists. Keys are matched by shape (output*, out, dest*,
// *_output, export_path, save_path, target_path) rather than by a
// per-tool allowlist, so a new exporting tool is covered the day it is
// added (review finding: generate_mesh_from_image's `output` was missed
// by the old list). The ambiguous keys `path`/`file`/`file_path` count
// only for tools that are exporters/bakers by name — read_file and
// cloud_upload take them as INPUTS.
QString overwriteReason(const QString& tool, const QJsonObject& args,
                        const std::function<bool(const QString&)>& fileExists)
{
    if (!fileExists) return {};
    static const QRegularExpression outputKey(
        R"(^(?:output(?:_?(?:path|file|dir|mesh|image|texture))?|out|dest(?:ination)?(?:_?path)?|export_path|save_path|target_path|[a-z_]+_output)$)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression writerTool(R"(^(?:export_|save_|paint_bake|pack_|generate_|upscale_|inpaint_|photo_depth|bake_))");
    static const QStringList ambiguousKeys = {"path", "file", "file_path"};
    const bool writer = writerTool.match(tool).hasMatch();
    for (auto it = args.begin(); it != args.end(); ++it) {
        const bool byShape = outputKey.match(it.key()).hasMatch();
        const bool ambiguous = writer && ambiguousKeys.contains(it.key());
        if (!byShape && !ambiguous) continue;
        const QString p = it.value().toVariant().toString();
        if (!p.isEmpty() && fileExists(p)) return QStringLiteral("overwrites existing file %1").arg(p);
    }
    return {};
}

} // namespace

QString AICapabilityRegistry::destructiveReason(const QString& tool, const QJsonObject& args,
                                                const std::function<bool(const QString&)>& fileExists)
{
    const QString del = deleteReason(tool, args);
    if (!del.isEmpty()) return del;
    if (isReadOnly(tool)) return {};
    return overwriteReason(tool, args, fileExists);
}

QString AICapabilityRegistry::destructiveReason(const QString& tool, const QJsonObject& args)
{
    return destructiveReason(tool, args, [](const QString& p) { return QFileInfo::exists(p); });
}

bool AICapabilityRegistry::isReadOnly(const QString& tool)
{
    static const QSet<QString> readOnly = {
        "take_screenshot", "validate_mesh", "uv_info", "read_file", "search_files", "list_files",
        "analyze_animation", "analyze_draw_calls", "get_memory_usage", "describe_material",
        "cloud_status", "cloud_limits", "cloud_list_projects", "ps1rip_status", "ps1rip_stats",
        "list_capture_devices", "camera_control", "get_camera_info", "select_entity",
    };
    return tool.startsWith(QLatin1String("get_")) || tool.startsWith(QLatin1String("list_"))
        || tool.startsWith(QLatin1String("toggle_")) || readOnly.contains(tool);
}
