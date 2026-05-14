#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace Ogre { class Entity; class Mesh; class SubMesh; }

/// Phase 6 slice E2: the consumption side of TextureAtlasPacker.
///
/// Slice E produced an atlas image + a JSON manifest of per-tile UV ranges.
/// Slice E2 takes that manifest plus an Ogre entity and rewrites every
/// submesh that references a packed source texture so it points at the
/// atlas image and has its UV0 channel scaled/biased into the matching
/// tile's [u0..u1, v0..v1] sub-rectangle. The net result: a mesh that
/// renders the same scene with one bound texture instead of N.
///
/// Pure-data — does not depend on the editor singletons. The caller hands
/// in an Ogre::Entity and the parsed manifest. CLI / MCP / Inspector each
/// load the entity their own way and reuse this module.
///
/// **Tiling UVs are NOT preserved.** A submesh whose UVs go outside
/// [0..1] cannot be safely atlased — the sub-rect doesn't repeat. We
/// detect the case and report it in the per-submesh result, but the
/// caller decides whether to clamp, abort, or skip. The default behaviour
/// is *clamp* (matches every other game-engine atlas tool).
namespace ApplyAtlas {

/// One tile from the manifest. Mirrors TextureAtlasPacker::AtlasTile but
/// keeps this module independent of that header.
struct ManifestTile {
    QString sourcePath; // path as written by the packer
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    float u0 = 0.f;
    float v0 = 0.f;
    float u1 = 0.f;
    float v1 = 0.f;
};

struct Manifest {
    int width   = 0;
    int height  = 0;
    int padding = 0;
    QList<ManifestTile> tiles;
};

/// Parse a JSON manifest produced by `TextureAtlasPacker::manifestToJson`.
/// Returns ok=false and a human-readable error if the JSON is malformed
/// or any tile is missing required fields.
struct ParseResult {
    bool ok = false;
    QString error;
    Manifest manifest;
};
ParseResult parseManifestJson(const QByteArray& json);

/// How to match a submesh's diffuse texture name against manifest tiles.
enum class MatchMode {
    Basename,     // strip directory; compare filenames (default — robust)
    FullPath,     // exact string match
};

struct ApplyOptions {
    MatchMode matchMode = MatchMode::Basename;
    /// Filename of the atlas image to bind on matched submeshes' diffuse
    /// TUS. Just the leaf name — the texture must already be discoverable
    /// via Ogre's resource system.
    QString atlasTextureName;
    /// When true, UVs outside [0..1] are clamped before remapping. When
    /// false, out-of-range UVs cause that submesh to be skipped with a
    /// warning in the report. Default: true (safer; matches the typical
    /// atlas-tool contract).
    bool clampOutOfRangeUVs = true;
    /// Atlasing UV0 invalidates any other texture binding that samples
    /// UV0 (normal map, AO, emissive, etc.) because those textures
    /// still expect `[0..1]` per-submesh UVs, but the mesh now points
    /// into the diffuse atlas's sub-rect. When true (default) those
    /// extra texture units are stripped from the affected materials so
    /// lighting at least matches the diffuse. Power-users who pre-
    /// atlased their normal/AO maps with a matching layout can set
    /// this to false to keep the extras and remap them separately.
    bool stripNonDiffuseTextures = true;
};

struct SubmeshReport {
    int submeshIndex = -1;
    QString materialName;
    QString diffuseTextureName; // pre-apply
    QString matchedTileSource;  // empty when no tile matched
    bool uvsRewritten = false;
    bool materialUpdated = false;
    int verticesTouched = 0;
    int outOfRangeUVs = 0;  // count of UVs that were clamped (or skipped)
    int strippedExtraTextures = 0;  // normal/AO/etc TUSes removed when stripNonDiffuseTextures=true
    QString note;           // human-readable status / skip reason
};

struct ApplyReport {
    bool ok = false;
    QString error; // populated only when the overall call failed
    QList<SubmeshReport> submeshes;

    int submeshCount() const { return submeshes.size(); }
    int rewrittenCount() const;
    QJsonObject toJson() const;
};

/// Walk every submesh on `entity`, match its diffuse texture name against
/// `manifest.tiles`, and (for matches) rewrite UV0 in place to the tile's
/// sub-rectangle plus swap the submesh's diffuse TUS to `opts.atlasTextureName`.
///
/// Returns a per-submesh report. The call itself is "ok" if it reached
/// every submesh — even submeshes with no match are listed (with a note).
/// Crashing on a corrupt buffer is the only thing that flips ok=false.
ApplyReport applyToEntity(Ogre::Entity* entity,
                          const Manifest& manifest,
                          const ApplyOptions& opts);

} // namespace ApplyAtlas
