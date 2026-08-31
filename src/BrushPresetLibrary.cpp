#include "BrushPresetLibrary.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>

namespace BrushPresetLibrary {

namespace {

// Mirrors of the controller enums, named here only so the bundled table reads
// clearly. These MUST stay numerically in step with the real enums — see the
// Preset doc comment.
enum Tool      { ToolPaint = 0, ToolErase = 1, ToolSmudge = 4 };
enum Footprint { FpRound = 0, FpSquare = 1, FpStamp = 2, FpTiling = 3 };
enum Rotation  { RotNone = 0, RotFixed = 1, RotStroke = 2, RotRandom = 3 };

/// Hashed suffix so two names that sanitise to the same stem cannot collide.
std::string customFileStem(const std::string& name)
{
    const size_t h = std::hash<std::string>{}(name);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "_%08x", static_cast<unsigned>(h & 0xffffffffu));
    return safeFileStem(name) + buf;
}

} // namespace

std::vector<Preset> bundledPresets()
{
    std::vector<Preset> out;

    auto add = [&out](const char* name, const char* note) -> Preset& {
        Preset p;
        p.name = name;
        p.note = note;
        out.push_back(std::move(p));
        return out.back();
    };

    {   // 1
        Preset& p = add("Soft Round", "General-purpose soft-edged round brush.");
        p.footprint = FpRound; p.radius = 0.06; p.strength = 0.7; p.falloff = 0.8;
    }
    {   // 2
        Preset& p = add("Hard Round", "Crisp round brush with almost no falloff.");
        p.footprint = FpRound; p.radius = 0.04; p.strength = 1.0; p.falloff = 0.05;
    }
    {   // 3
        Preset& p = add("Pencil Sketch", "Small, hard, lightly scattered — line work.");
        p.footprint = FpStamp; p.stamp = "Charcoal";
        p.radius = 0.015; p.strength = 0.85; p.falloff = 0.2;
        p.spacing = 0.12; p.scatter = 0.05; p.sizeJitter = 0.15;
        p.stampRotation = RotRandom;
    }
    {   // 4
        Preset& p = add("Spray Paint", "Wide, sparse spatter with heavy jitter.");
        p.footprint = FpStamp; p.stamp = "Spatter";
        p.radius = 0.12; p.strength = 0.35; p.falloff = 0.9;
        p.spacing = 0.25; p.scatter = 0.8; p.sizeJitter = 0.5; p.opacityJitter = 0.6;
        p.stampRotation = RotRandom;
    }
    {   // 5
        Preset& p = add("Foliage Cluster", "Scattered leaf stamps for vegetation.");
        p.footprint = FpStamp; p.stamp = "Foliage Cluster";
        p.radius = 0.1; p.strength = 0.9; p.falloff = 0.3;
        p.spacing = 0.6; p.scatter = 0.65; p.sizeJitter = 0.45; p.opacityJitter = 0.25;
        p.stampRotation = RotRandom;
    }
    {   // 6
        Preset& p = add("Edge Wear", "Thin scratchy strokes for worn edges.");
        p.footprint = FpStamp; p.stamp = "Scratch Lines";
        p.radius = 0.05; p.strength = 0.6; p.falloff = 0.4;
        p.spacing = 0.18; p.scatter = 0.2; p.opacityJitter = 0.4;
        p.stampRotation = RotStroke;
    }
    {   // 7
        Preset& p = add("Scratched Metal", "Long directional scratches.");
        p.footprint = FpStamp; p.stamp = "Scratch Lines";
        p.radius = 0.08; p.strength = 0.8; p.falloff = 0.15;
        p.spacing = 0.1; p.scatter = 0.1; p.sizeJitter = 0.3;
        p.stampRotation = RotStroke;
    }
    {   // 8
        Preset& p = add("Wet Brush", "Broad, soft, low-opacity build-up.");
        p.footprint = FpRound; p.radius = 0.1; p.strength = 0.25; p.falloff = 0.95;
        p.spacing = 0.08;
    }
    {   // 9
        Preset& p = add("Smudge Soft", "Gentle smear with a wide soft tip.");
        p.tool = ToolSmudge; p.footprint = FpRound;
        p.radius = 0.09; p.strength = 0.35; p.falloff = 0.9;
    }
    {   // 10
        Preset& p = add("Smudge Hard", "Tight, strong smear for pushing detail.");
        p.tool = ToolSmudge; p.footprint = FpRound;
        p.radius = 0.04; p.strength = 0.8; p.falloff = 0.2;
    }
    {   // 11
        Preset& p = add("Stencil Hard", "Hard-edged square footprint for masks.");
        p.footprint = FpSquare; p.shape = 1;
        p.radius = 0.06; p.strength = 1.0; p.falloff = 0.0;
    }
    {   // 12
        Preset& p = add("Stencil Soft", "Square footprint with a feathered edge.");
        p.footprint = FpSquare; p.shape = 1;
        p.radius = 0.07; p.strength = 0.75; p.falloff = 0.7;
    }
    {   // 13
        Preset& p = add("Eraser Soft", "Soft eraser for fading paint away.");
        p.tool = ToolErase; p.footprint = FpRound;
        p.radius = 0.08; p.strength = 0.5; p.falloff = 0.85;
    }
    {   // 14
        Preset& p = add("Eraser Hard", "Crisp eraser for clean cut-outs.");
        p.tool = ToolErase; p.footprint = FpRound;
        p.radius = 0.05; p.strength = 1.0; p.falloff = 0.05;
    }
    {   // 15
        Preset& p = add("Cavity Dirt", "Grimy speckle for recesses and seams.");
        p.footprint = FpStamp; p.stamp = "Spatter";
        p.radius = 0.07; p.strength = 0.45; p.falloff = 0.6;
        p.spacing = 0.2; p.scatter = 0.5; p.sizeJitter = 0.4; p.opacityJitter = 0.5;
        p.stampRotation = RotRandom;
    }

    return out;
}

const Preset* findBundled(const std::string& name)
{
    // Function-local static so the returned pointer stays valid. Same pattern
    // as GradientRamp::findBundled.
    static const std::vector<Preset> kBundled = bundledPresets();
    for (const auto& p : kBundled)
        if (p.name == name) return &p;
    return nullptr;
}

bool isBundled(const std::string& name)
{
    return findBundled(name) != nullptr;
}

std::string toJson(const Preset& p)
{
    QJsonObject o;
    o["name"] = QString::fromStdString(p.name);
    o["tool"] = p.tool;
    o["radius"] = p.radius;
    o["strength"] = p.strength;
    o["falloff"] = p.falloff;
    o["shape"] = p.shape;
    o["channel"] = p.channel;
    o["footprint"] = p.footprint;
    o["stamp"] = QString::fromStdString(p.stamp);
    o["tiling"] = QString::fromStdString(p.tiling);
    o["spacing"] = p.spacing;
    o["scatter"] = p.scatter;
    o["sizeJitter"] = p.sizeJitter;
    o["opacityJitter"] = p.opacityJitter;
    o["stampRotation"] = p.stampRotation;
    o["stampAngleDeg"] = p.stampAngleDeg;
    o["colorSource"] = p.colorSource;
    o["gradientMode"] = p.gradientMode;
    o["rampName"] = QString::fromStdString(p.rampName);
    o["note"] = QString::fromStdString(p.note);
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

bool fromJson(const std::string& json, Preset& out)
{
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(json), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject o = doc.object();

    Preset p;   // struct defaults are the fallback for every missing field, so
                // a preset written by an older build still loads cleanly.
    p.name = o.value("name").toString().toStdString();
    if (p.name.empty()) return false;

    p.tool          = o.value("tool").toInt(p.tool);
    p.radius        = o.value("radius").toDouble(p.radius);
    p.strength      = o.value("strength").toDouble(p.strength);
    p.falloff       = o.value("falloff").toDouble(p.falloff);
    p.shape         = o.value("shape").toInt(p.shape);
    p.channel       = o.value("channel").toInt(p.channel);
    p.footprint     = o.value("footprint").toInt(p.footprint);
    p.stamp         = o.value("stamp").toString().toStdString();
    p.tiling        = o.value("tiling").toString().toStdString();
    p.spacing       = o.value("spacing").toDouble(p.spacing);
    p.scatter       = o.value("scatter").toDouble(p.scatter);
    p.sizeJitter    = o.value("sizeJitter").toDouble(p.sizeJitter);
    p.opacityJitter = o.value("opacityJitter").toDouble(p.opacityJitter);
    p.stampRotation = o.value("stampRotation").toInt(p.stampRotation);
    p.stampAngleDeg = o.value("stampAngleDeg").toDouble(p.stampAngleDeg);
    p.colorSource   = o.value("colorSource").toInt(p.colorSource);
    p.gradientMode  = o.value("gradientMode").toInt(p.gradientMode);
    p.rampName      = o.value("rampName").toString().toStdString();
    p.note          = o.value("note").toString().toStdString();

    out = std::move(p);
    return true;
}

std::string presetsDirectory()
{
    if (!QCoreApplication::instance()) return {};   // AppDataLocation needs one
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) return {};
    const QString dir = QDir(base).filePath(QStringLiteral("paint/presets"));
    QDir().mkpath(dir);
    return dir.toStdString();
}

std::string safeFileStem(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
            out.push_back(c);
        else if (c == ' ')
            out.push_back('_');
    }
    if (out.empty()) out = "preset";
    return out;
}

std::string saveCustom(const Preset& p)
{
    if (!p.isValid()) return {};
    const std::string dir = presetsDirectory();
    if (dir.empty()) return {};
    const QString path = QDir(QString::fromStdString(dir))
                             .filePath(QString::fromStdString(customFileStem(p.name))
                                       + QStringLiteral(".json"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    const std::string json = toJson(p);
    if (f.write(json.data(), static_cast<qint64>(json.size()))
        != static_cast<qint64>(json.size())) {
        f.close();
        QFile::remove(path);        // never leave a truncated preset behind
        return {};
    }
    f.close();
    return path.toStdString();
}

std::vector<Preset> loadCustomPresets()
{
    std::vector<Preset> out;
    const std::string dir = presetsDirectory();
    if (dir.empty()) return out;
    QDir d(QString::fromStdString(dir));
    const QStringList files =
        d.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& fn : files) {
        QFile f(d.filePath(fn));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray data = f.readAll();
        f.close();
        Preset p;
        if (fromJson(data.toStdString(), p)) out.push_back(std::move(p));
    }
    return out;
}

bool deleteCustom(const std::string& name)
{
    const std::string dir = presetsDirectory();
    if (dir.empty()) return false;
    QDir d(QString::fromStdString(dir));

    const QString direct = d.filePath(QString::fromStdString(customFileStem(name))
                                      + QStringLiteral(".json"));
    if (QFile::exists(direct)) return QFile::remove(direct);

    // Fall back to matching the embedded name so imported or hand-authored
    // files (whose stem we did not choose) are still deletable from the UI.
    const QStringList files =
        d.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& fn : files) {
        QFile f(d.filePath(fn));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray data = f.readAll();
        f.close();
        Preset p;
        if (fromJson(data.toStdString(), p) && p.name == name)
            return QFile::remove(d.filePath(fn));
    }
    return false;
}

std::vector<Preset> allPresets()
{
    std::vector<Preset> out = bundledPresets();
    for (auto& custom : loadCustomPresets()) {
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const Preset& p) { return p.name == custom.name; });
        if (it != out.end()) *it = std::move(custom);   // custom wins
        else out.push_back(std::move(custom));
    }
    return out;
}

bool findPreset(const std::string& name, Preset& out)
{
    for (const auto& p : allPresets()) {
        if (p.name == name) { out = p; return true; }
    }
    return false;
}

bool exportToFile(const Preset& p, const std::string& path)
{
    if (!p.isValid() || path.empty()) return false;
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const std::string json = toJson(p);
    const bool ok = f.write(json.data(), static_cast<qint64>(json.size()))
                    == static_cast<qint64>(json.size());
    f.close();
    if (!ok) QFile::remove(QString::fromStdString(path));
    return ok;
}

bool importFromFile(const std::string& path, Preset& out)
{
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = f.readAll();
    f.close();
    return fromJson(data.toStdString(), out);
}

} // namespace BrushPresetLibrary
