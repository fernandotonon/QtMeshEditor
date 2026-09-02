#include "ColorPaletteLibrary.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <map>

namespace ColorPaletteLibrary {

namespace {

Swatch rgb(int r, int g, int b)
{
    Swatch s;
    s.r = static_cast<uint8_t>(std::clamp(r, 0, 255));
    s.g = static_cast<uint8_t>(std::clamp(g, 0, 255));
    s.b = static_cast<uint8_t>(std::clamp(b, 0, 255));
    return s;
}

int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/// Custom palettes get a hashed suffix so two names that sanitise to the same
/// stem ("Sky Blues" / "sky/blues") cannot overwrite each other.
std::string customFileStem(const std::string& name)
{
    const size_t h = std::hash<std::string>{}(name);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "_%08x", static_cast<unsigned>(h & 0xffffffffu));
    return safeFileStem(name) + buf;
}

} // namespace

bool swatchFromHex(const std::string& hex, Swatch& out)
{
    std::string h = hex;
    if (!h.empty() && h.front() == '#') h.erase(h.begin());
    if (h.size() != 6) return false;
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = hexDigit(h[static_cast<size_t>(i)]);
        if (v[i] < 0) return false;
    }
    out.r = static_cast<uint8_t>(v[0] * 16 + v[1]);
    out.g = static_cast<uint8_t>(v[2] * 16 + v[3]);
    out.b = static_cast<uint8_t>(v[4] * 16 + v[5]);
    return true;
}

std::string swatchToHex(const Swatch& s)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", s.r, s.g, s.b);
    return std::string(buf);
}

std::vector<Palette> bundledPalettes()
{
    std::vector<Palette> out;

    // All six are CC0 / factual colour values (Material Design's published
    // palette, common Pantone-classic hues, and observational skin/foliage/sky/
    // earth ranges) — no third-party asset files ship with them.
    out.push_back({"Material Design", {
        rgb(0xF4, 0x43, 0x36), rgb(0xE9, 0x1E, 0x63), rgb(0x9C, 0x27, 0xB0),
        rgb(0x67, 0x3A, 0xB7), rgb(0x3F, 0x51, 0xB5), rgb(0x21, 0x96, 0xF3),
        rgb(0x03, 0xA9, 0xF4), rgb(0x00, 0xBC, 0xD4), rgb(0x00, 0x96, 0x88),
        rgb(0x4C, 0xAF, 0x50), rgb(0x8B, 0xC3, 0x4A), rgb(0xCD, 0xDC, 0x39),
        rgb(0xFF, 0xEB, 0x3B), rgb(0xFF, 0xC1, 0x07), rgb(0xFF, 0x98, 0x00),
        rgb(0xFF, 0x57, 0x22), rgb(0x79, 0x55, 0x48), rgb(0x9E, 0x9E, 0x9E),
        rgb(0x60, 0x7D, 0x8B), rgb(0x21, 0x21, 0x21),
    }});

    out.push_back({"Pantone Classics", {
        rgb(0xC7, 0x40, 0x75), rgb(0x93, 0x9A, 0xC0), rgb(0xFF, 0x6F, 0x61),
        rgb(0x5F, 0x4B, 0x8B), rgb(0x88, 0xB0, 0x4B), rgb(0x0F, 0x4C, 0x81),
        rgb(0x93, 0x4F, 0x5E), rgb(0xF5, 0xDF, 0x4D), rgb(0x93, 0x93, 0x96),
        rgb(0xBB, 0x25, 0x28),
    }});

    out.push_back({"Skin Tones", {
        rgb(0xFF, 0xE0, 0xC9), rgb(0xF6, 0xC9, 0xA8), rgb(0xE8, 0xB0, 0x8D),
        rgb(0xD9, 0x99, 0x77), rgb(0xC1, 0x7F, 0x5E), rgb(0xA3, 0x66, 0x47),
        rgb(0x84, 0x51, 0x36), rgb(0x63, 0x3C, 0x28), rgb(0x45, 0x29, 0x1B),
        rgb(0x2B, 0x19, 0x10),
    }});

    out.push_back({"Foliage Greens", {
        rgb(0xE4, 0xF0, 0xC2), rgb(0xC4, 0xDE, 0x8E), rgb(0x9C, 0xC7, 0x5B),
        rgb(0x76, 0xA9, 0x3A), rgb(0x55, 0x8B, 0x2F), rgb(0x3E, 0x6E, 0x24),
        rgb(0x2C, 0x54, 0x1B), rgb(0x1D, 0x3D, 0x14), rgb(0x6B, 0x7F, 0x3A),
        rgb(0x8F, 0x9E, 0x4C),
    }});

    out.push_back({"Sky Blues", {
        rgb(0xEA, 0xF6, 0xFF), rgb(0xC7, 0xE7, 0xFB), rgb(0x9E, 0xD2, 0xF6),
        rgb(0x73, 0xB9, 0xEC), rgb(0x4A, 0x9D, 0xDE), rgb(0x2E, 0x7F, 0xC4),
        rgb(0x1F, 0x62, 0xA1), rgb(0x16, 0x47, 0x78), rgb(0xB5, 0xC7, 0xD8),
        rgb(0xF7, 0xC9, 0x9B),
    }});

    out.push_back({"Earth Tones", {
        rgb(0xE8, 0xD9, 0xC0), rgb(0xD3, 0xBC, 0x9A), rgb(0xBB, 0x9E, 0x77),
        rgb(0xA1, 0x82, 0x5C), rgb(0x84, 0x67, 0x45), rgb(0x68, 0x4F, 0x34),
        rgb(0x4D, 0x39, 0x25), rgb(0x8C, 0x6B, 0x52), rgb(0xA9, 0x7C, 0x50),
        rgb(0x6E, 0x5A, 0x47),
    }});

    return out;
}

const Palette* findBundled(const std::string& name)
{
    // Function-local static: stable addresses for the returned pointer, built
    // once. Same pattern as GradientRamp::findBundled.
    static const std::vector<Palette> kBundled = bundledPalettes();
    for (const auto& p : kBundled)
        if (p.name == name) return &p;
    return nullptr;
}

bool isBundled(const std::string& name)
{
    return findBundled(name) != nullptr;
}

std::string toJson(const Palette& p)
{
    QJsonObject root;
    root["name"] = QString::fromStdString(p.name);
    QJsonArray arr;
    for (const auto& s : p.swatches)
        arr.append(QString::fromStdString(swatchToHex(s)));
    root["swatches"] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

bool fromJson(const std::string& json, Palette& out)
{
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(json), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject root = doc.object();

    Palette p;
    p.name = root.value("name").toString().toStdString();
    if (p.name.empty()) return false;
    const QJsonArray arr = root.value("swatches").toArray();
    for (const auto& v : arr) {
        Swatch s;
        // Skip malformed entries rather than rejecting the whole palette: one
        // bad hex string should not lose the user's other swatches.
        if (swatchFromHex(v.toString().toStdString(), s)) p.swatches.push_back(s);
    }
    if (p.swatches.empty()) return false;
    out = std::move(p);
    return true;
}

std::string palettesDirectory()
{
    // QStandardPaths needs an application instance for AppDataLocation.
    if (!QCoreApplication::instance()) return {};
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) return {};
    const QString dir = QDir(base).filePath(QStringLiteral("paint/palettes"));
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
    if (out.empty()) out = "palette";
    return out;
}

std::string saveCustom(const Palette& p)
{
    if (!p.isValid()) return {};
    const std::string dir = palettesDirectory();
    if (dir.empty()) return {};
    const QString path = QDir(QString::fromStdString(dir))
                             .filePath(QString::fromStdString(customFileStem(p.name))
                                       + QStringLiteral(".json"));
    // QSaveFile: temp file + rename on commit, so an interrupted or short
    // write preserves the previous palette instead of truncating it.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    const std::string json = toJson(p);
    if (f.write(json.data(), static_cast<qint64>(json.size()))
        != static_cast<qint64>(json.size())) {
        f.cancelWriting();
        return {};
    }
    if (!f.commit()) return {};
    return path.toStdString();
}

std::vector<Palette> loadCustomPalettes()
{
    std::vector<Palette> out;
    const std::string dir = palettesDirectory();
    if (dir.empty()) return out;
    QDir d(QString::fromStdString(dir));
    const QStringList files =
        d.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& fn : files) {
        QFile f(d.filePath(fn));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray data = f.readAll();
        f.close();
        Palette p;
        if (fromJson(data.toStdString(), p)) out.push_back(std::move(p));
    }
    return out;
}

bool deleteCustom(const std::string& name)
{
    const std::string dir = palettesDirectory();
    if (dir.empty()) return false;
    QDir d(QString::fromStdString(dir));

    // Preferred: the hashed stem this library writes.
    const QString direct = d.filePath(QString::fromStdString(customFileStem(name))
                                      + QStringLiteral(".json"));
    if (QFile::exists(direct)) return QFile::remove(direct);

    // Fall back to matching the embedded name, so hand-authored or legacy files
    // (written before the hashed stem) are still deletable from the UI.
    const QStringList files =
        d.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& fn : files) {
        QFile f(d.filePath(fn));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray data = f.readAll();
        f.close();
        Palette p;
        if (fromJson(data.toStdString(), p) && p.name == name)
            return QFile::remove(d.filePath(fn));
    }
    return false;
}

std::vector<Palette> allPalettes()
{
    std::vector<Palette> out = bundledPalettes();
    for (auto& custom : loadCustomPalettes()) {
        // Custom overrides a bundled palette of the same name — the same
        // precedence BrushAssetLibrary::resolvePath applies to stamps, so a user
        // can tweak a bundled palette without losing the ability to reset.
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const Palette& p) { return p.name == custom.name; });
        if (it != out.end()) *it = std::move(custom);
        else out.push_back(std::move(custom));
    }
    return out;
}

void pushRecent(std::vector<Swatch>& recent, const Swatch& s, size_t maxCount)
{
    if (maxCount == 0) { recent.clear(); return; }
    // Remove an existing copy first so re-picking a colour promotes it to the
    // front instead of filling the ring with duplicates.
    recent.erase(std::remove(recent.begin(), recent.end(), s), recent.end());
    recent.insert(recent.begin(), s);
    if (recent.size() > maxCount) recent.resize(maxCount);
}

std::vector<Swatch> extractFromImage(const uint8_t* rgba, int width, int height,
                                     int maxColours)
{
    std::vector<Swatch> out;
    if (!rgba || width <= 0 || height <= 0 || maxColours <= 0) return out;

    // Quantise into a coarse colour cube and count occupancy. 5 bits/channel
    // (32 levels) groups near-identical shades — a photographic texture has
    // thousands of unique RGB values, so counting exact colours would return
    // ten imperceptibly different swatches.
    constexpr int kShift = 3;                 // 8 -> 5 bits
    std::map<uint32_t, uint32_t> counts;
    std::map<uint32_t, std::array<uint64_t, 3>> sums;
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* px = rgba + i * 4;
        if (px[3] == 0) continue;             // fully transparent carries no colour
        const uint32_t key = (uint32_t(px[0] >> kShift) << 10)
                           | (uint32_t(px[1] >> kShift) << 5)
                           | (uint32_t(px[2] >> kShift));
        ++counts[key];
        auto& acc = sums[key];
        acc[0] += px[0]; acc[1] += px[1]; acc[2] += px[2];
    }
    if (counts.empty()) return out;

    std::vector<std::pair<uint32_t, uint32_t>> ordered(counts.begin(), counts.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) {
                  // Most frequent first; tie-break on the key so the result is
                  // deterministic rather than dependent on map iteration.
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });

    const size_t take = std::min<size_t>(ordered.size(), static_cast<size_t>(maxColours));
    out.reserve(take);
    for (size_t i = 0; i < take; ++i) {
        const uint32_t key = ordered[i].first;
        const uint32_t c = ordered[i].second;
        const auto& acc = sums[key];
        // Average the real pixels in the bucket rather than the bucket centre,
        // so the swatch is a colour that actually occurs in the image.
        out.push_back(rgb(static_cast<int>(acc[0] / c),
                          static_cast<int>(acc[1] / c),
                          static_cast<int>(acc[2] / c)));
    }
    return out;
}

} // namespace ColorPaletteLibrary
