/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — Paint v2 Slice I (#552): bake-up targets.

Turns a set of composited per-channel images into the textures a game engine
actually consumes. Pure data: QImage in, QImage out, plus a sidecar JSON. No
Ogre, no paint session, no GPU — so every engine channel rule is unit-testable
without a scene.

Why not just call TextureChannelPacker: its ChannelSource (and
NormalMapGenerator::GenSpec) take a FILE PATH, but a freshly composited channel
lives in memory. Routing through them would mean writing each channel to a temp
PNG and reading it straight back. The luminance rule here is deliberately the
same Rec.601 weighting TextureChannelPacker uses, so the two agree on any
grayscale conversion.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#ifndef PAINT_BAKE_TARGETS_H
#define PAINT_BAKE_TARGETS_H

#include <QImage>
#include <QMap>
#include <QString>
#include <QStringList>

#include <vector>

namespace PaintBakeTargets {

/// Engine channel layout to emit.
enum class Target {
    Generic = 0,  ///< one texture per channel, as painted (normal = OpenGL +Y up)
    Unity,        ///< metallic+smoothness RGBA, AO separate, normal DirectX +Y down
    Unreal,       ///< ORM packed RGB (occlusion / roughness / metallic)
    Godot,        ///< separate textures + .tres sidecar carrying sRGB/linear flags
    GLTF,         ///< glTF 2.0 metallic-roughness (G = roughness, B = metallic)
    Count
};

const char* targetId(Target t);
const char* targetLabel(Target t);
/// Parse a CLI/MCP target id ("unity", "unreal", "godot", "gltf", "generic").
/// Returns false on an unknown id rather than silently defaulting — a typo that
/// silently produced Generic output would look like the pack simply failed to
/// apply.
bool targetFromId(const QString& id, Target& out);
QStringList targetIds();

/// The painted channels this bake consumes.
///
/// NB there is deliberately no Height entry. Slice D (#547) removed Height as a
/// paintable channel — it shares the Normal session and has no buffer of its
/// own, and no parallax/displacement shader consumes a standalone height map. It
/// is omitted rather than emitted as a duplicate of the normal map's grayscale
/// source, which would look like a deliverable and be read by nothing. See
/// docs/PAINT_V2_SLICE_I_DESIGN.md.
struct ChannelImages {
    QImage baseColor;   ///< sRGB, may carry alpha
    QImage normal;      ///< tangent-space, OpenGL +Y up (as painted)
    QImage roughness;   ///< linear grayscale
    QImage metallic;    ///< linear grayscale
    QImage ao;          ///< linear grayscale
    QImage emissive;    ///< sRGB

    /// True when nothing was painted at all — the caller should report this
    /// rather than writing a directory of blank textures.
    bool empty() const;
};

struct Options {
    Target target = Target::Generic;
    /// Output edge length; 0 keeps each channel's own resolution.
    int resolution = 0;
    /// Prefix for every written file, e.g. "hero" -> "hero_BaseColor.png".
    QString namePrefix;
};

/// One texture to write.
struct OutputTexture {
    QString suffix;      ///< file stem suffix, e.g. "BaseColor" or "ORM"
    QImage image;
    bool srgb = false;   ///< colour data (vs linear/data) — drives Godot's flags
    QString note;        ///< human-readable description for the sidecar
};

struct Result {
    bool ok = false;
    QString error;
    std::vector<OutputTexture> textures;
    /// Godot only: the .tres text. Empty for other targets.
    QString godotResource;
};

/// Build the output set for `target`. Does not touch the filesystem; the caller
/// writes `Result::textures` and the sidecar.
Result build(const ChannelImages& channels, const Options& options);

/// Rec.601 luminance of `src` as an 8-bit grayscale image. Matches
/// TextureChannelPacker's sampling so a grayscale conversion is identical
/// whichever path produced it.
QImage toGrayscale(const QImage& src);

/// Invert an 8-bit grayscale image. Roughness -> smoothness (Unity) and back.
QImage invertGrayscale(const QImage& src);

/// Flip a tangent-space normal map's GREEN channel: OpenGL (+Y up) <-> DirectX
/// (+Y down). Unity expects +Y down; getting this backwards inverts every
/// surface's lighting, so it is unit-tested against hand-computed values.
QImage flipNormalGreen(const QImage& src);

/// Pack up to three grayscale images into one RGB image's R/G/B lanes.
/// A null image contributes `fallback` on that lane. Output size is the largest
/// input (or `size` when non-zero).
QImage packRgb(const QImage& r, const QImage& g, const QImage& b,
               int size = 0, int fallback = 0);

/// Sidecar JSON describing the bake, for reproducibility.
/// `inputChannels` are the channel ids that had painted data.
QString sidecarJson(const Options& options,
                    const QStringList& inputChannels,
                    const std::vector<OutputTexture>& textures,
                    const QString& meshName);

} // namespace PaintBakeTargets

#endif // PAINT_BAKE_TARGETS_H
