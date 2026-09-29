#include "GameReadyPresets.h"

namespace GameReady {

const std::vector<Preset>& presets()
{
    // Kept in C++ rather than data files so a preset cannot go missing from
    // an install (the GradientRamp::bundledPresets precedent).
    //
    // 500 is the lowest HONEST prop tier: meshoptimizer locks UV and material
    // border edges, so a request for 100 and one for 500 both bottom out
    // around the same count on a given asset; an entry promising 100 would
    // name a number the simplifier cannot deliver.
    static const std::vector<Preset> table = {
        { QStringLiteral("max"),
          QStringLiteral("Maximum detail (auto cap)"), 0, false, 0,
          QStringLiteral("Keep the generation's own density (dense TRELLIS "
                         "sources are still capped at ~150-300k for the bake).") },
        { QStringLiteral("prop-minimal"),
          QStringLiteral("Prop Minimal (~500 tris)"), 500, false, 0,
          QStringLiteral("Flat-faced props: a mat, a crate, a pizza box.") },
        { QStringLiteral("prop-tiny"),
          QStringLiteral("Prop Tiny (~1k tris)"), 1000, false, 0, {} },
        { QStringLiteral("roblox-accessory"),
          QStringLiteral("Roblox Accessory (≤4k tris, 1024 px)"), 4000, true, 1024,
          QStringLiteral("Roblox accessory / layered-clothing upload limit: "
                         "4,000 triangles, textures up to 1024 px. The "
                         "triangle count is enforced as a hard ceiling.") },
        { QStringLiteral("prop-low"),
          QStringLiteral("Prop Low (~5k tris)"), 5000, false, 0, {} },
        { QStringLiteral("low"),
          QStringLiteral("Game Low (~10k tris)"), 10000, false, 0, {} },
        { QStringLiteral("roblox-meshpart"),
          QStringLiteral("Roblox MeshPart (≤20k tris, 1024 px)"), 20000, true, 1024,
          QStringLiteral("Roblox MeshPart upload limit: 20,000 triangles, "
                         "textures up to 1024 px. The triangle count is "
                         "enforced as a hard ceiling.") },
        { QStringLiteral("medium"),
          QStringLiteral("Game Medium (~25k tris)"), 25000, false, 0,
          QStringLiteral("The default: a clean bake on organic subjects.") },
        { QStringLiteral("high"),
          QStringLiteral("Game High (~50k tris)"), 50000, false, 0, {} },
    };
    return table;
}

QString defaultId()
{
    return QStringLiteral("medium");
}

const Preset* find(const QString& rawId)
{
    QString id = rawId.trimmed().toLower();
    id.replace(QLatin1Char('_'), QLatin1Char('-'));
    id.replace(QLatin1Char(' '), QLatin1Char('-'));
    // Forgiving spellings for the platform presets — the ids people type from
    // memory rather than copy from the listing.
    if (id == QLatin1String("roblox") || id == QLatin1String("roblox-mesh-part")
        || id == QLatin1String("roblox-mesh") || id == QLatin1String("meshpart"))
        id = QStringLiteral("roblox-meshpart");
    else if (id == QLatin1String("accessory") || id == QLatin1String("roblox-acc")
             || id == QLatin1String("roblox-clothing"))
        id = QStringLiteral("roblox-accessory");
    else if (id == QLatin1String("original") || id == QLatin1String("maximum")
             || id == QLatin1String("none"))
        id = QStringLiteral("max");
    for (const Preset& p : presets())
        if (p.id == id)
            return &p;
    return nullptr;
}

QStringList ids()
{
    QStringList out;
    for (const Preset& p : presets())
        out << p.id;
    return out;
}

int indexOf(const QString& id)
{
    const Preset* p = find(id);
    if (!p) return -1;
    const auto& all = presets();
    for (size_t i = 0; i < all.size(); ++i)
        if (&all[i] == p)
            return static_cast<int>(i);
    return -1;
}

} // namespace GameReady
