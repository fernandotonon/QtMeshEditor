/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — Paint v2 Slice I (#552): bake-up targets.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include "PaintBakeTargets.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace PaintBakeTargets {
namespace {

/// Rec.601 — the same weighting TextureChannelPacker samples with.
inline int luma601(const QRgb p)
{
    return qBound(0, static_cast<int>(0.299 * qRed(p) + 0.587 * qGreen(p)
                                      + 0.114 * qBlue(p) + 0.5), 255);
}

/// Scale to a square `size`, or pass through when size <= 0.
QImage fit(const QImage& src, int size)
{
    if (src.isNull() || size <= 0) return src;
    if (src.width() == size && src.height() == size) return src;
    return src.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

int largestEdge(const std::initializer_list<const QImage*> imgs)
{
    int e = 0;
    for (const QImage* i : imgs)
        if (i && !i->isNull()) e = std::max({e, i->width(), i->height()});
    return e;
}

} // namespace

const char* targetId(Target t)
{
    switch (t) {
        case Target::Generic: return "generic";
        case Target::Unity:   return "unity";
        case Target::Unreal:  return "unreal";
        case Target::Godot:   return "godot";
        case Target::GLTF:    return "gltf";
        default:              return "generic";
    }
}

const char* targetLabel(Target t)
{
    switch (t) {
        case Target::Generic: return "Generic PBR set";
        case Target::Unity:   return "Unity (Metallic+Smoothness)";
        case Target::Unreal:  return "Unreal (ORM)";
        case Target::Godot:   return "Godot (separate + .tres)";
        case Target::GLTF:    return "glTF 2.0 (metallic-roughness)";
        default:              return "Generic PBR set";
    }
}

bool targetFromId(const QString& id, Target& out)
{
    const QString k = id.trimmed().toLower();
    for (int i = 0; i < static_cast<int>(Target::Count); ++i) {
        const auto t = static_cast<Target>(i);
        if (k == QLatin1String(targetId(t))) { out = t; return true; }
    }
    return false;
}

QStringList targetIds()
{
    QStringList out;
    for (int i = 0; i < static_cast<int>(Target::Count); ++i)
        out << QString::fromLatin1(targetId(static_cast<Target>(i)));
    return out;
}

bool ChannelImages::empty() const
{
    return baseColor.isNull() && normal.isNull() && roughness.isNull()
        && metallic.isNull() && ao.isNull() && emissive.isNull();
}

QImage toGrayscale(const QImage& src)
{
    if (src.isNull()) return {};
    if (src.format() == QImage::Format_Grayscale8) return src;
    const QImage rgb = src.convertToFormat(QImage::Format_RGBA8888);
    QImage out(rgb.width(), rgb.height(), QImage::Format_Grayscale8);
    for (int y = 0; y < rgb.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        uchar* dst = out.scanLine(y);
        for (int x = 0; x < rgb.width(); ++x)
            dst[x] = static_cast<uchar>(luma601(line[x]));
    }
    return out;
}

QImage invertGrayscale(const QImage& src)
{
    if (src.isNull()) return {};
    QImage g = toGrayscale(src);
    for (int y = 0; y < g.height(); ++y) {
        uchar* line = g.scanLine(y);
        for (int x = 0; x < g.width(); ++x)
            line[x] = static_cast<uchar>(255 - line[x]);
    }
    return g;
}

QImage flipNormalGreen(const QImage& src)
{
    if (src.isNull()) return {};
    QImage out = src.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < out.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const QRgb p = line[x];
            line[x] = qRgba(qRed(p), 255 - qGreen(p), qBlue(p), qAlpha(p));
        }
    }
    return out;
}

QImage packRgb(const QImage& r, const QImage& g, const QImage& b,
               int size, int fallback)
{
    const int edge = size > 0 ? size : largestEdge({&r, &g, &b});
    if (edge <= 0) return {};

    const QImage gr = fit(toGrayscale(r), edge);
    const QImage gg = fit(toGrayscale(g), edge);
    const QImage gb = fit(toGrayscale(b), edge);
    const int fb = qBound(0, fallback, 255);

    QImage out(edge, edge, QImage::Format_RGB888);
    for (int y = 0; y < edge; ++y) {
        auto* dst = out.scanLine(y);
        const uchar* lr = (!gr.isNull() && y < gr.height()) ? gr.constScanLine(y) : nullptr;
        const uchar* lg = (!gg.isNull() && y < gg.height()) ? gg.constScanLine(y) : nullptr;
        const uchar* lb = (!gb.isNull() && y < gb.height()) ? gb.constScanLine(y) : nullptr;
        for (int x = 0; x < edge; ++x) {
            dst[x * 3 + 0] = static_cast<uchar>(lr && x < gr.width() ? lr[x] : fb);
            dst[x * 3 + 1] = static_cast<uchar>(lg && x < gg.width() ? lg[x] : fb);
            dst[x * 3 + 2] = static_cast<uchar>(lb && x < gb.width() ? lb[x] : fb);
        }
    }
    return out;
}

namespace {

void addIfPresent(std::vector<OutputTexture>& out, const QString& suffix,
                  const QImage& img, int size, bool srgb, const QString& note)
{
    if (img.isNull()) return;
    out.push_back({suffix, fit(img, size), srgb, note});
}

/// Godot .tres: the flags matter more than the file list. A colour texture read
/// as linear (or a data texture read as sRGB) is the classic "my material looks
/// washed out / too dark" import bug, so the flag is emitted per texture.
QString buildGodotResource(const std::vector<OutputTexture>& textures,
                           const QString& prefix)
{
    QString s;
    s += QStringLiteral("[gd_resource type=\"StandardMaterial3D\" format=3]\n\n");
    for (const auto& t : textures) {
        s += QStringLiteral("; %1%2.png  srgb=%3  (%4)\n")
                 .arg(prefix, t.suffix, t.srgb ? QStringLiteral("true")
                                               : QStringLiteral("false"), t.note);
    }
    s += QStringLiteral("\n[resource]\n");
    for (const auto& t : textures) {
        s += QStringLiteral("; %1 -> %2\n")
                 .arg(t.suffix, t.srgb ? QStringLiteral("sRGB")
                                       : QStringLiteral("linear/data"));
    }
    return s;
}

} // namespace

Result build(const ChannelImages& ch, const Options& opt)
{
    Result r;
    if (ch.empty()) {
        r.error = QStringLiteral("Nothing painted — no channel has data to bake.");
        return r;
    }
    if (opt.resolution < 0) {
        r.error = QStringLiteral("Resolution must be 0 (keep source) or positive.");
        return r;
    }

    const int size = opt.resolution;

    switch (opt.target) {
        case Target::Generic:
            addIfPresent(r.textures, "BaseColor", ch.baseColor, size, true,
                         "sRGB base colour");
            addIfPresent(r.textures, "Normal", ch.normal, size, false,
                         "tangent-space normal, OpenGL +Y up");
            addIfPresent(r.textures, "Roughness", toGrayscale(ch.roughness), size, false,
                         "linear roughness");
            addIfPresent(r.textures, "Metallic", toGrayscale(ch.metallic), size, false,
                         "linear metallic");
            addIfPresent(r.textures, "AO", toGrayscale(ch.ao), size, false,
                         "ambient occlusion");
            addIfPresent(r.textures, "Emissive", ch.emissive, size, true,
                         "sRGB emissive");
            break;

        case Target::Unity: {
            addIfPresent(r.textures, "BaseColor", ch.baseColor, size, true,
                         "sRGB albedo");
            // Unity samples tangent-space normals with +Y DOWN.
            if (!ch.normal.isNull())
                addIfPresent(r.textures, "Normal", flipNormalGreen(ch.normal), size,
                             false, "tangent-space normal, DirectX +Y down");
            // Metallic in R, SMOOTHNESS (1 - roughness) in A.
            if (!ch.metallic.isNull() || !ch.roughness.isNull()) {
                const int edge = size > 0 ? size
                                          : largestEdge({&ch.metallic, &ch.roughness});
                const QImage m = fit(toGrayscale(ch.metallic), edge);
                const QImage s = fit(invertGrayscale(ch.roughness), edge);
                QImage ms(edge, edge, QImage::Format_RGBA8888);
                for (int y = 0; y < edge; ++y) {
                    auto* dst = reinterpret_cast<QRgb*>(ms.scanLine(y));
                    const uchar* lm = (!m.isNull() && y < m.height()) ? m.constScanLine(y) : nullptr;
                    const uchar* ls = (!s.isNull() && y < s.height()) ? s.constScanLine(y) : nullptr;
                    for (int x = 0; x < edge; ++x) {
                        const int mv = (lm && x < m.width()) ? lm[x] : 0;
                        // No roughness painted => fully rough => smoothness 0.
                        const int sv = (ls && x < s.width()) ? ls[x] : 0;
                        dst[x] = qRgba(mv, 0, 0, sv);
                    }
                }
                r.textures.push_back({"MetallicSmoothness", ms, false,
                                      "R = metallic, A = smoothness (1 - roughness)"});
            }
            addIfPresent(r.textures, "Occlusion", toGrayscale(ch.ao), size, false,
                         "ambient occlusion (separate in Unity)");
            addIfPresent(r.textures, "Emissive", ch.emissive, size, true,
                         "sRGB emissive");
            break;
        }

        case Target::Unreal: {
            addIfPresent(r.textures, "BaseColor", ch.baseColor, size, true,
                         "sRGB base colour");
            addIfPresent(r.textures, "Normal", ch.normal, size, false,
                         "tangent-space normal, OpenGL +Y up");
            if (!ch.ao.isNull() || !ch.roughness.isNull() || !ch.metallic.isNull()) {
                // Unpainted occlusion must read as "no occlusion" (white), not
                // black — a 0 lane would darken the whole surface.
                const int edge = size > 0 ? size
                                          : largestEdge({&ch.ao, &ch.roughness, &ch.metallic});
                QImage aoImg = ch.ao;
                if (aoImg.isNull()) {
                    aoImg = QImage(edge, edge, QImage::Format_Grayscale8);
                    aoImg.fill(255);
                }
                r.textures.push_back({"ORM",
                                      packRgb(aoImg, ch.roughness, ch.metallic, edge, 0),
                                      false,
                                      "R = occlusion, G = roughness, B = metallic"});
            }
            addIfPresent(r.textures, "Emissive", ch.emissive, size, true,
                         "sRGB emissive");
            break;
        }

        case Target::Godot:
            addIfPresent(r.textures, "Albedo", ch.baseColor, size, true,
                         "sRGB albedo");
            addIfPresent(r.textures, "Normal", ch.normal, size, false,
                         "tangent-space normal, OpenGL +Y up");
            addIfPresent(r.textures, "Roughness", toGrayscale(ch.roughness), size, false,
                         "linear roughness");
            addIfPresent(r.textures, "Metallic", toGrayscale(ch.metallic), size, false,
                         "linear metallic");
            addIfPresent(r.textures, "AO", toGrayscale(ch.ao), size, false,
                         "ambient occlusion");
            addIfPresent(r.textures, "Emission", ch.emissive, size, true,
                         "sRGB emission");
            r.godotResource = buildGodotResource(r.textures, opt.namePrefix);
            break;

        case Target::GLTF: {
            addIfPresent(r.textures, "baseColor", ch.baseColor, size, true,
                         "sRGB base colour (glTF pbrMetallicRoughness.baseColorTexture)");
            addIfPresent(r.textures, "normal", ch.normal, size, false,
                         "tangent-space normal, OpenGL +Y up (glTF normalTexture)");
            if (!ch.roughness.isNull() || !ch.metallic.isNull()) {
                // glTF: G = roughness, B = metallic. R is free; occlusion may
                // share it, which is the common single-texture packing.
                const int edge = size > 0 ? size
                                          : largestEdge({&ch.roughness, &ch.metallic, &ch.ao});
                r.textures.push_back({"metallicRoughness",
                                      packRgb(ch.ao, ch.roughness, ch.metallic, edge, 255),
                                      false,
                                      "G = roughness, B = metallic (R = occlusion)"});
            }
            if (!ch.ao.isNull())
                addIfPresent(r.textures, "occlusion", toGrayscale(ch.ao), size, false,
                             "occlusion (glTF occlusionTexture reads R)");
            addIfPresent(r.textures, "emissive", ch.emissive, size, true,
                         "sRGB emissive (glTF emissiveTexture)");
            break;
        }

        default:
            r.error = QStringLiteral("Unknown bake target.");
            return r;
    }

    if (r.textures.empty()) {
        r.error = QStringLiteral("No output textures for this target — the painted "
                                 "channels do not feed it.");
        return r;
    }
    r.ok = true;
    return r;
}

QString sidecarJson(const Options& opt, const QStringList& inputChannels,
                    const std::vector<OutputTexture>& textures,
                    const QString& meshName)
{
    QJsonObject root;
    root["schema"] = QStringLiteral("qtmesh-paint-bake-v1");
    root["target"] = QString::fromLatin1(targetId(opt.target));
    root["targetLabel"] = QString::fromLatin1(targetLabel(opt.target));
    root["resolution"] = opt.resolution;   // 0 = kept each channel's own size
    if (!opt.namePrefix.isEmpty()) root["namePrefix"] = opt.namePrefix;
    if (!meshName.isEmpty()) root["mesh"] = meshName;

    QJsonArray ins;
    for (const QString& c : inputChannels) ins.append(c);
    root["inputChannels"] = ins;

    QJsonArray outs;
    for (const auto& t : textures) {
        QJsonObject o;
        o["suffix"] = t.suffix;
        o["file"] = opt.namePrefix.isEmpty()
                        ? (t.suffix + QStringLiteral(".png"))
                        : (opt.namePrefix + QStringLiteral("_") + t.suffix
                           + QStringLiteral(".png"));
        o["width"] = t.image.width();
        o["height"] = t.image.height();
        o["colorSpace"] = t.srgb ? QStringLiteral("sRGB") : QStringLiteral("linear");
        if (!t.note.isEmpty()) o["note"] = t.note;
        outs.append(o);
    }
    root["outputs"] = outs;

    // Recorded so the omission reads as a decision, not a missing feature.
    root["heightOmitted"] =
        QStringLiteral("Height is not a paintable channel (see #547): it shares the "
                       "Normal session and has no standalone consumer.");

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

} // namespace PaintBakeTargets
