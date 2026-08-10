#ifndef BRUSH_ASSET_LIBRARY_H
#define BRUSH_ASSET_LIBRARY_H

#include "BrushFootprint.h"

#include <string>
#include <vector>

/**
 * @brief Paint v2 Slice B (#545) — bundled + custom stamp / tiling assets.
 *
 * Bundled CC0 PNGs ship under `media/paint/{stamps,tilings}/` (copied next
 * to the binary). User imports land in `<AppData>/paint/{stamps,tilings}/`.
 */
namespace BrushAssetLibrary {

enum class AssetKind { Stamp = 0, Tiling = 1 };

struct AssetInfo {
    std::string name;
    AssetKind kind = AssetKind::Stamp;
    bool bundled = false;
    std::string path; ///< resolved absolute path when known
};

/// The six bundled stamp names required by #545.
std::vector<std::string> bundledStampNames();

/// The four bundled tiling names required by #545.
std::vector<std::string> bundledTilingNames();

std::string stampsDirectory();
std::string tilingsDirectory();

/// Resolve a stamp/tiling by name (custom overrides bundled).
std::string resolvePath(const std::string& name, AssetKind kind);

/// Load PNG/TGA/JPEG/BMP into RGBA8. Returns empty on failure.
BrushFootprint::ImageRgba loadImage(const std::string& path);

/// List bundled + custom assets of one kind.
std::vector<AssetInfo> listAssets(AssetKind kind);

/// Import a user PNG into the custom directory. Returns the stored path.
std::string importAsset(const std::string& sourcePath, AssetKind kind,
                        const std::string& desiredName = {});

bool deleteCustom(const std::string& name, AssetKind kind);

bool renameCustom(const std::string& oldName, const std::string& newName,
                  AssetKind kind);

std::string safeFileStem(const std::string& name);

/// Thumbnail as `data:image/png;base64,...` for QML / Qt widgets.
std::string thumbnailDataUri(const std::string& path, int maxSize = 48);

} // namespace BrushAssetLibrary

#endif // BRUSH_ASSET_LIBRARY_H
