#include "GamificationTypes.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

namespace Gamification {

const QList<FeatureInfo>& featureCatalog()
{
    // Keys MUST match qtmesh-cloud's DISCOVERY_FEATURES (src/gamification.ts).
    static const QList<FeatureInfo> catalog = {
        {QStringLiteral("retopo"), QStringLiteral("Quad Retopology"),
         QStringLiteral("Convert triangle meshes into clean quad-dominant topology.")},
        {QStringLiteral("decimate_lod"), QStringLiteral("Decimation & LODs"),
         QStringLiteral("Reduce triangle counts and generate LOD chains for game-ready assets.")},
        {QStringLiteral("uv_unwrap"), QStringLiteral("UV Unwrap"),
         QStringLiteral("Auto-unwrap non-overlapping UVs, or use box/cylinder/sphere projection.")},
        {QStringLiteral("texture_paint"), QStringLiteral("Texture Paint"),
         QStringLiteral("Paint directly onto your model's textures in the viewport.")},
        {QStringLiteral("pbr_synth"), QStringLiteral("PBR Map Synthesis"),
         QStringLiteral("Generate normal, roughness and height maps from a single diffuse texture.")},
        {QStringLiteral("material_editor"), QStringLiteral("Material Editor"),
         QStringLiteral("Edit materials with live preview, one-click presets and PBR templates.")},
        {QStringLiteral("auto_rig"), QStringLiteral("Auto-Rig"),
         QStringLiteral("Embed a skeleton into a static mesh — template fit or guided markers.")},
        {QStringLiteral("skin_weights"), QStringLiteral("Auto Skin Weights"),
         QStringLiteral("Compute smooth-bind skin weights for a mesh + skeleton in one click.")},
        {QStringLiteral("animation_blend"), QStringLiteral("Animation Tools"),
         QStringLiteral("Blend, merge, resample and retarget skeletal animations.")},
        {QStringLiteral("morph"), QStringLiteral("Morph Targets"),
         QStringLiteral("Create and animate morph (blend shape) targets.")},
        {QStringLiteral("motion_inbetween"), QStringLiteral("AI In-Betweening"),
         QStringLiteral("Fill gaps between keyframes with smooth, plausible motion.")},
        {QStringLiteral("pose_library"), QStringLiteral("Pose Library"),
         QStringLiteral("Save and reuse skeleton poses across animations.")},
        {QStringLiteral("vat_bake"), QStringLiteral("VAT Baking"),
         QStringLiteral("Bake vertex animation textures for engine-side playback.")},
        {QStringLiteral("vertex_color_bake"), QStringLiteral("Vertex Color Bake"),
         QStringLiteral("Bake lighting or textures into per-vertex colors.")},
        {QStringLiteral("texture_atlas"), QStringLiteral("Texture Atlas"),
         QStringLiteral("Pack many textures into one atlas and remap mesh UVs onto it.")},
        {QStringLiteral("isometric_sprites"), QStringLiteral("Isometric Sprites"),
         QStringLiteral("Render 8-direction isometric sprite atlases from any model.")},
        {QStringLiteral("turntable"), QStringLiteral("Turntable Render"),
         QStringLiteral("Render turntable sprite sheets for previews and showcases.")},
        {QStringLiteral("ai_assist"), QStringLiteral("AI Assist"),
         QStringLiteral("Local AI helpers: segmentation, texture upscaling and more.")},
        {QStringLiteral("image_to_3d"), QStringLiteral("Image → 3D"),
         QStringLiteral("Reconstruct a textured 3D mesh from a single image.")},
        {QStringLiteral("stable_diffusion"), QStringLiteral("AI Texture Generation"),
         QStringLiteral("Generate mesh-aware textures from a text prompt.")},
        {QStringLiteral("batch_export"), QStringLiteral("Batch Export"),
         QStringLiteral("Convert and package many assets in one pass.")},
        {QStringLiteral("cli_scan"), QStringLiteral("CLI Asset Scan"),
         QStringLiteral("Lint asset folders locally or in CI with `qtmesh scan`.")},
        {QStringLiteral("mcp_server"), QStringLiteral("MCP Server"),
         QStringLiteral("Drive the editor from AI agents over the Model Context Protocol.")},
        {QStringLiteral("cloud_upload"), QStringLiteral("Cloud Upload"),
         QStringLiteral("Publish projects to QtMesh Cloud with scan reports and share links.")},
        {QStringLiteral("lighting"), QStringLiteral("Scene Lighting"),
         QStringLiteral("Light your scene with point/spot/directional lights or an HDR environment.")},
    };
    return catalog;
}

const FeatureInfo* featureInfo(const QString& key)
{
    for (const FeatureInfo& f : featureCatalog()) {
        if (f.key == key)
            return &f;
    }
    return nullptr;
}

bool isValidEventKey(const QString& key)
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9_]+$"));
    return !key.isEmpty() && key.size() <= 64 && re.match(key).hasMatch();
}

Achievement Achievement::fromJson(const QJsonObject& o)
{
    Achievement a;
    a.key = o.value(QStringLiteral("key")).toString();
    a.category = o.value(QStringLiteral("category")).toString();
    a.title = o.value(QStringLiteral("title")).toString();
    a.description = o.value(QStringLiteral("description")).toString();
    a.icon = o.value(QStringLiteral("icon")).toString();
    a.tier = o.value(QStringLiteral("tier")).toString();
    a.xp = o.value(QStringLiteral("xp")).toInt();
    a.hidden = o.value(QStringLiteral("hidden")).toBool();
    a.earnedAt = static_cast<qint64>(o.value(QStringLiteral("earnedAt")).toDouble());
    return a;
}

QVariantMap Achievement::toVariantMap() const
{
    QVariantMap m;
    m.insert(QStringLiteral("key"), key);
    m.insert(QStringLiteral("category"), category);
    m.insert(QStringLiteral("title"), title);
    m.insert(QStringLiteral("description"), description);
    m.insert(QStringLiteral("icon"), icon);
    m.insert(QStringLiteral("tier"), tier);
    m.insert(QStringLiteral("xp"), xp);
    m.insert(QStringLiteral("hidden"), hidden);
    m.insert(QStringLiteral("earnedAt"), earnedAt);
    return m;
}

const QList<MilestoneInfo>& milestoneCatalog()
{
    // Mirrors qtmesh-cloud SEED_ACHIEVEMENTS counters + streak thresholds so
    // the editor can render "nearest unlockable" progress bars offline.
    static const QList<MilestoneInfo> catalog = {
        {QStringLiteral("discovery_10"), QStringLiteral("Explorer"),
         QStringLiteral("Try 10 different tools"),
         QStringLiteral("features_discovered"), 10, false},
        {QStringLiteral("discovery_all"), QStringLiteral("Cartographer"),
         QStringLiteral("Try every tool"),
         QStringLiteral("features_discovered"),
         static_cast<qint64>(featureCatalog().size()), true},
        {QStringLiteral("optimized_1m_tris"), QStringLiteral("Triangle Slayer"),
         QStringLiteral("Optimize 1,000,000 triangles total"),
         QStringLiteral("tris_optimized"), 1000000, false},
        {QStringLiteral("rigged_10_meshes"), QStringLiteral("Master Rigger"),
         QStringLiteral("Rig 10 meshes"),
         QStringLiteral("meshes_rigged"), 10, false},
        {QStringLiteral("fixed_100_issues"), QStringLiteral("Fixer"),
         QStringLiteral("Fix 100 scan issues"),
         QStringLiteral("issues_fixed"), 100, false},
        {QStringLiteral("streak_3"), QStringLiteral("Warming Up"),
         QStringLiteral("3-day scan streak"),
         QStringLiteral("current_streak"), 3, false},
        {QStringLiteral("streak_7"), QStringLiteral("Consistent"),
         QStringLiteral("7-day scan streak"),
         QStringLiteral("current_streak"), 7, false},
        {QStringLiteral("streak_30"), QStringLiteral("Committed"),
         QStringLiteral("30-day scan streak"),
         QStringLiteral("current_streak"), 30, false},
        {QStringLiteral("streak_100"), QStringLiteral("Relentless"),
         QStringLiteral("100-day scan streak"),
         QStringLiteral("current_streak"), 100, false},
    };
    return catalog;
}

QVariantMap NextUnlockable::toVariantMap() const
{
    QVariantMap m;
    m.insert(QStringLiteral("key"), key);
    m.insert(QStringLiteral("title"), title);
    m.insert(QStringLiteral("description"), description);
    m.insert(QStringLiteral("current"), current);
    m.insert(QStringLiteral("threshold"), threshold);
    m.insert(QStringLiteral("fraction"), fraction);
    return m;
}

StatsSnapshot StatsSnapshot::fromJson(const QJsonObject& root)
{
    StatsSnapshot s;
    if (root.isEmpty())
        return s;

    const QJsonObject stats = root.value(QStringLiteral("stats")).toObject();
    s.xp = static_cast<qint64>(stats.value(QStringLiteral("xp")).toDouble());
    s.level = qMax(1, stats.value(QStringLiteral("level")).toInt(1));
    s.currentStreak = stats.value(QStringLiteral("current_streak")).toInt();
    s.longestStreak = stats.value(QStringLiteral("longest_streak")).toInt();
    s.lastActiveDay = stats.value(QStringLiteral("last_active_day")).toString();

    const QJsonObject progress = root.value(QStringLiteral("progress")).toObject();
    s.levelFloorXp = static_cast<qint64>(progress.value(QStringLiteral("levelFloorXp")).toDouble());
    s.nextLevelXp = static_cast<qint64>(progress.value(QStringLiteral("nextLevelXp")).toDouble(100));
    s.intoLevel = static_cast<qint64>(progress.value(QStringLiteral("intoLevel")).toDouble());
    s.span = static_cast<qint64>(progress.value(QStringLiteral("span")).toDouble(100));
    s.fraction = progress.value(QStringLiteral("fraction")).toDouble();

    for (const QJsonValue& v : root.value(QStringLiteral("achievements")).toArray())
        s.achievements.append(Achievement::fromJson(v.toObject()));
    for (const QJsonValue& v : root.value(QStringLiteral("recentlyEarned")).toArray())
        s.recentlyEarned.append(Achievement::fromJson(v.toObject()));

    const QJsonObject counters = root.value(QStringLiteral("counters")).toObject();
    for (auto it = counters.constBegin(); it != counters.constEnd(); ++it)
        s.counters.insert(it.key(), static_cast<qint64>(it.value().toDouble()));

    for (const QJsonValue& v : root.value(QStringLiteral("featureUsage")).toArray()) {
        const QJsonObject row = v.toObject();
        const QString key = row.value(QStringLiteral("feature_key")).toString();
        if (!key.isEmpty())
            s.featureUseCounts.insert(key, row.value(QStringLiteral("use_count")).toInt(1));
    }

    s.valid = true;
    return s;
}

QJsonObject StatsSnapshot::toJson() const
{
    QJsonObject stats;
    stats.insert(QStringLiteral("xp"), static_cast<double>(xp));
    stats.insert(QStringLiteral("level"), level);
    stats.insert(QStringLiteral("current_streak"), currentStreak);
    stats.insert(QStringLiteral("longest_streak"), longestStreak);
    stats.insert(QStringLiteral("last_active_day"), lastActiveDay);

    QJsonObject progress;
    progress.insert(QStringLiteral("levelFloorXp"), static_cast<double>(levelFloorXp));
    progress.insert(QStringLiteral("nextLevelXp"), static_cast<double>(nextLevelXp));
    progress.insert(QStringLiteral("intoLevel"), static_cast<double>(intoLevel));
    progress.insert(QStringLiteral("span"), static_cast<double>(span));
    progress.insert(QStringLiteral("fraction"), fraction);

    auto achievementJson = [](const Achievement& a) {
        QJsonObject o;
        o.insert(QStringLiteral("key"), a.key);
        o.insert(QStringLiteral("category"), a.category);
        o.insert(QStringLiteral("title"), a.title);
        o.insert(QStringLiteral("description"), a.description);
        o.insert(QStringLiteral("icon"), a.icon);
        o.insert(QStringLiteral("tier"), a.tier);
        o.insert(QStringLiteral("xp"), a.xp);
        o.insert(QStringLiteral("hidden"), a.hidden);
        o.insert(QStringLiteral("earnedAt"), static_cast<double>(a.earnedAt));
        return o;
    };

    QJsonArray achievementsArr;
    for (const Achievement& a : achievements)
        achievementsArr.append(achievementJson(a));
    QJsonArray recentArr;
    for (const Achievement& a : recentlyEarned)
        recentArr.append(achievementJson(a));

    QJsonObject countersObj;
    for (auto it = counters.constBegin(); it != counters.constEnd(); ++it)
        countersObj.insert(it.key(), static_cast<double>(it.value()));

    QJsonArray usageArr;
    for (auto it = featureUseCounts.constBegin(); it != featureUseCounts.constEnd(); ++it) {
        QJsonObject row;
        row.insert(QStringLiteral("feature_key"), it.key());
        row.insert(QStringLiteral("use_count"), it.value());
        usageArr.append(row);
    }

    QJsonObject root;
    root.insert(QStringLiteral("stats"), stats);
    root.insert(QStringLiteral("progress"), progress);
    root.insert(QStringLiteral("achievements"), achievementsArr);
    root.insert(QStringLiteral("recentlyEarned"), recentArr);
    root.insert(QStringLiteral("counters"), countersObj);
    root.insert(QStringLiteral("featureUsage"), usageArr);
    return root;
}

bool StatsSnapshot::hasEarned(const QString& achievementKey) const
{
    return std::any_of(achievements.cbegin(), achievements.cend(),
                       [&](const Achievement& a) { return a.key == achievementKey; });
}

QList<NextUnlockable> StatsSnapshot::nextUnlockables(int maxCount) const
{
    QList<NextUnlockable> out;
    for (const MilestoneInfo& m : milestoneCatalog()) {
        if (m.hidden || hasEarned(m.key))
            continue;
        NextUnlockable u;
        u.key = m.key;
        u.title = m.title;
        u.description = m.description;
        u.threshold = m.threshold;
        if (m.counter == QStringLiteral("current_streak"))
            u.current = currentStreak;
        else
            u.current = counters.value(m.counter, 0);
        u.current = qBound<qint64>(0, u.current, m.threshold);
        u.fraction = m.threshold > 0 ? static_cast<double>(u.current) / m.threshold : 0.0;
        out.append(u);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const NextUnlockable& a, const NextUnlockable& b) {
                         return a.fraction > b.fraction;
                     });
    if (maxCount >= 0 && out.size() > maxCount)
        out = out.mid(0, maxCount);
    return out;
}

QStringList StatsSnapshot::unusedFeatureKeys() const
{
    QStringList out;
    for (const FeatureInfo& f : featureCatalog()) {
        if (!featureUseCounts.contains(f.key))
            out.append(f.key);
    }
    return out;
}

}  // namespace Gamification
