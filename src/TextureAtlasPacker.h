#pragma once

#include <QImage>
#include <QList>
#include <QString>
#include <QStringList>

/// Phase 6 slice E: pack N input texture files into a single atlas image.
/// Sibling to TextureChannelPacker — same "pure-data, Ogre-free, callable
/// from CLI / MCP / GUI" shape — but operates at the file-list level
/// rather than the per-channel level. Common indie use case: consolidate
/// small per-prop textures into a single binding so renderers (and the
/// scan engine's draw-call analyzer) see one batch instead of many.
///
/// The packer is shelf-bin-pack: sort inputs by height descending, lay
/// them in rows left-to-right, open a new row when the current row
/// can't fit the next tile. No rotation, no MaxRects — deterministic,
/// readable, and "good enough" for the typical indie texture set
/// (10-100 props, mostly square POT). Each tile gets a configurable
/// pixel padding on every side so MIPs don't bleed across neighbours.
namespace TextureAtlasPacker {

struct AtlasSpec {
    QStringList sourcePaths;   // input image paths (PNG/TGA/JPG/BMP)
    int atlasWidth   = 2048;   // pixels; must be > 0 (no auto-sizing)
    int atlasHeight  = 2048;   // pixels; must be > 0
    int padding      = 2;      // pixels of empty space on every side of a tile
};

/// One tile's placement in the atlas, plus pre-computed UV ranges
/// (atlas-space). u0,v0 is the top-left corner; u1,v1 the bottom-right.
/// All values are valid for QImage's y-down convention.
struct AtlasTile {
    QString sourcePath;   // copied from spec
    int x = 0;            // pixel offsets in the atlas
    int y = 0;
    int width = 0;
    int height = 0;
    float u0 = 0.f;       // normalized UVs (x / atlasWidth, y / atlasHeight)
    float v0 = 0.f;
    float u1 = 0.f;
    float v1 = 0.f;
};

struct AtlasResult {
    bool ok = false;
    QString error;
    QImage image;             // RGBA8 composite; empty on failure
    QList<AtlasTile> tiles;   // one entry per successfully-placed input,
                              // in input order (not pack order)
    int usedWidth = 0;
    int usedHeight = 0;
};

/// Build the atlas image + tile manifest. Pure-data; safe to call without
/// Ogre. Returns a result with ok=false and a human-readable error string
/// if any input fails to load, exceeds the atlas size after padding, or
/// the inputs can't be fit in the requested atlas.
AtlasResult pack(const AtlasSpec& spec);

/// Convenience: pack and write the atlas to PNG/TGA/JPG at outPath. The
/// extension determines the format. Returns the same AtlasResult; check
/// `ok` for success.
AtlasResult packToFile(const AtlasSpec& spec, const QString& outPath);

/// Serialize an AtlasResult's tile manifest to a JSON string. Stable
/// schema:
///   { width, height, padding, tiles: [
///       { source, x, y, w, h, u0, v0, u1, v1 }, …
///   ] }
/// Downstream tooling (Inspector "Apply Atlas" action, qtmesh-cloud
/// asset pipeline integration, etc.) can ingest this directly.
QString manifestToJson(const AtlasResult& result, int padding);

} // namespace TextureAtlasPacker
