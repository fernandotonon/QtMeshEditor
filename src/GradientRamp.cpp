#include "GradientRamp.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

namespace GradientRamp {
namespace {

Rgba lerp(const Rgba& a, const Rgba& b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return {
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t,
    };
}

Stop stopAt(float t, float r, float g, float b, float a = 1.0f)
{
    return {t, {r, g, b, a}};
}

void sortStops(std::vector<Stop>& stops)
{
    std::sort(stops.begin(), stops.end(),
              [](const Stop& a, const Stop& b) {
                  return a.position < b.position;
              });
}

} // namespace

Rgba Ramp::sample(float t) const
{
    if (stops.empty())
        return {};
    if (stops.size() == 1)
        return stops.front().colour;

    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= stops.front().position)
        return stops.front().colour;
    if (t >= stops.back().position)
        return stops.back().colour;

    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        const Stop& a = stops[i];
        const Stop& b = stops[i + 1];
        if (t > b.position)
            continue;
        if (interpolate == Interpolate::Stepped) {
            // Hold the left stop until the next stop's position; at the
            // exact boundary, take the right stop (falls through via continue).
            if (t < b.position)
                return a.colour;
            continue;
        }
        const float span = b.position - a.position;
        if (span <= 1e-8f)
            return b.colour;
        const float u = (t - a.position) / span;
        return lerp(a.colour, b.colour, u);
    }
    return stops.back().colour;
}

Ramp fromFgBg(const Rgba& fg, const Rgba& bg, const std::string& name)
{
    Ramp r;
    r.name = name;
    r.interpolate = Interpolate::Linear;
    r.stops = {stopAt(0.0f, fg.r, fg.g, fg.b, fg.a),
               stopAt(1.0f, bg.r, bg.g, bg.b, bg.a)};
    return r;
}

std::vector<Ramp> bundledPresets()
{
    std::vector<Ramp> out;
    out.reserve(6);

    {
        Ramp r;
        r.name = "Greyscale";
        r.stops = {stopAt(0.0f, 0, 0, 0), stopAt(1.0f, 1, 1, 1)};
        out.push_back(std::move(r));
    }
    {
        // Full hue wheel — red→yellow→green→cyan→blue→magenta→red.
        Ramp r;
        r.name = "Hue";
        r.stops = {
            stopAt(0.0f / 6.0f, 1, 0, 0),
            stopAt(1.0f / 6.0f, 1, 1, 0),
            stopAt(2.0f / 6.0f, 0, 1, 0),
            stopAt(3.0f / 6.0f, 0, 1, 1),
            stopAt(4.0f / 6.0f, 0, 0, 1),
            stopAt(5.0f / 6.0f, 1, 0, 1),
            stopAt(1.0f,        1, 0, 0),
        };
        out.push_back(std::move(r));
    }
    {
        Ramp r;
        r.name = "Gold to Rust";
        r.stops = {
            stopAt(0.0f, 0.95f, 0.78f, 0.25f),
            stopAt(0.45f, 0.85f, 0.45f, 0.12f),
            stopAt(1.0f, 0.55f, 0.18f, 0.08f),
        };
        out.push_back(std::move(r));
    }
    {
        Ramp r;
        r.name = "Sunset";
        r.stops = {
            stopAt(0.0f, 1.00f, 0.55f, 0.15f),
            stopAt(0.45f, 0.95f, 0.25f, 0.45f),
            stopAt(1.0f, 0.20f, 0.10f, 0.45f),
        };
        out.push_back(std::move(r));
    }
    {
        Ramp r;
        r.name = "Ocean";
        r.stops = {
            stopAt(0.0f, 0.02f, 0.12f, 0.35f),
            stopAt(0.50f, 0.05f, 0.45f, 0.55f),
            stopAt(1.0f, 0.25f, 0.85f, 0.90f),
        };
        out.push_back(std::move(r));
    }
    {
        Ramp r;
        r.name = "Skin Tones";
        r.stops = {
            stopAt(0.0f, 0.96f, 0.82f, 0.72f),
            stopAt(0.50f, 0.78f, 0.55f, 0.42f),
            stopAt(1.0f, 0.42f, 0.25f, 0.18f),
        };
        out.push_back(std::move(r));
    }
    return out;
}

const Ramp* findBundled(const std::string& name)
{
    // Stable addresses for the lifetime of the process — rebuilt once.
    static const std::vector<Ramp> kBundled = bundledPresets();
    for (const Ramp& r : kBundled) {
        if (r.name == name)
            return &r;
    }
    return nullptr;
}

std::string toJson(const Ramp& ramp)
{
    QJsonObject root;
    root.insert(QStringLiteral("name"), QString::fromStdString(ramp.name));
    root.insert(QStringLiteral("interpolate"),
                ramp.interpolate == Interpolate::Stepped
                    ? QStringLiteral("stepped")
                    : QStringLiteral("linear"));
    QJsonArray stops;
    for (const Stop& s : ramp.stops) {
        QJsonObject o;
        o.insert(QStringLiteral("t"), static_cast<double>(s.position));
        o.insert(QStringLiteral("r"), static_cast<double>(s.colour.r));
        o.insert(QStringLiteral("g"), static_cast<double>(s.colour.g));
        o.insert(QStringLiteral("b"), static_cast<double>(s.colour.b));
        o.insert(QStringLiteral("a"), static_cast<double>(s.colour.a));
        stops.append(o);
    }
    root.insert(QStringLiteral("stops"), stops);
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

bool fromJson(const std::string& json, Ramp& out)
{
    QJsonParseError err;
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(json), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const QJsonObject root = doc.object();
    Ramp ramp;
    ramp.name = root.value(QStringLiteral("name")).toString().toStdString();
    const QString interp =
        root.value(QStringLiteral("interpolate")).toString(QStringLiteral("linear"));
    ramp.interpolate = (interp == QStringLiteral("stepped"))
                           ? Interpolate::Stepped
                           : Interpolate::Linear;
    const QJsonArray stops = root.value(QStringLiteral("stops")).toArray();
    ramp.stops.reserve(static_cast<size_t>(stops.size()));
    for (const QJsonValue& v : stops) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        Stop s;
        s.position = static_cast<float>(o.value(QStringLiteral("t")).toDouble(0.0));
        s.colour.r = static_cast<float>(o.value(QStringLiteral("r")).toDouble(0.0));
        s.colour.g = static_cast<float>(o.value(QStringLiteral("g")).toDouble(0.0));
        s.colour.b = static_cast<float>(o.value(QStringLiteral("b")).toDouble(0.0));
        s.colour.a = static_cast<float>(o.value(QStringLiteral("a")).toDouble(1.0));
        s.position = std::clamp(s.position, 0.0f, 1.0f);
        s.colour.r = std::clamp(s.colour.r, 0.0f, 1.0f);
        s.colour.g = std::clamp(s.colour.g, 0.0f, 1.0f);
        s.colour.b = std::clamp(s.colour.b, 0.0f, 1.0f);
        s.colour.a = std::clamp(s.colour.a, 0.0f, 1.0f);
        ramp.stops.push_back(s);
    }
    sortStops(ramp.stops);
    if (!ramp.isValid())
        return false;
    out = std::move(ramp);
    return true;
}

std::string rampsDirectory()
{
    if (!QCoreApplication::instance())
        return {};
    const QString root =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty())
        return {};
    const QString dir = root + QStringLiteral("/paint/ramps");
    QDir().mkpath(dir);
    return dir.toStdString();
}

std::string safeFileStem(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                        || (c >= '0' && c <= '9') || c == '-' || c == '_'
                        || c == ' ';
        out.push_back(ok ? (c == ' ' ? '_' : c) : '_');
    }
    if (out.empty())
        out = "ramp";
    return out;
}

std::string customRampFileStem(const std::string& name)
{
    const std::string base = safeFileStem(name.empty() ? "custom" : name);
    const auto h = static_cast<uint32_t>(std::hash<std::string>{}(name));
    char suffix[10];
    std::snprintf(suffix, sizeof(suffix), "_%08x", h);
    return base + suffix;
}

std::string saveCustom(const Ramp& ramp)
{
    const std::string dir = rampsDirectory();
    if (dir.empty() || !ramp.isValid())
        return {};
    const std::string stem = customRampFileStem(ramp.name.empty() ? "custom" : ramp.name);
    const QString path =
        QString::fromStdString(dir) + QLatin1Char('/')
        + QString::fromStdString(stem) + QStringLiteral(".json");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    const std::string payload = toJson(ramp);
    if (f.write(payload.data(), static_cast<qint64>(payload.size()))
        != static_cast<qint64>(payload.size()))
        return {};
    return path.toStdString();
}

std::vector<Ramp> loadCustomRamps()
{
    std::vector<Ramp> out;
    const std::string dir = rampsDirectory();
    if (dir.empty())
        return out;
    const QDir qdir(QString::fromStdString(dir));
    const QStringList files =
        qdir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& name : files) {
        QFile f(qdir.filePath(name));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        Ramp ramp;
        if (fromJson(f.readAll().toStdString(), ramp))
            out.push_back(std::move(ramp));
    }
    return out;
}

bool deleteCustom(const std::string& name)
{
    const std::string dir = rampsDirectory();
    if (dir.empty() || name.empty())
        return false;
    const QString path =
        QString::fromStdString(dir) + QLatin1Char('/')
        + QString::fromStdString(customRampFileStem(name)) + QStringLiteral(".json");
    if (QFile::remove(path))
        return true;
    // Legacy stems (pre-hash) or hand-edited files: match by ramp.name in JSON.
    const QDir qdir(QString::fromStdString(dir));
    const QStringList files =
        qdir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& file : files) {
        QFile f(qdir.filePath(file));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        Ramp ramp;
        if (fromJson(f.readAll().toStdString(), ramp) && ramp.name == name)
            return QFile::remove(qdir.filePath(file));
    }
    return false;
}

} // namespace GradientRamp
