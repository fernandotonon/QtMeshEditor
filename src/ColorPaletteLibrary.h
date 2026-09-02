#ifndef COLOR_PALETTE_LIBRARY_H
#define COLOR_PALETTE_LIBRARY_H

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Paint v2 Slice H (#551) — colour swatches / palettes (Ogre-free).
 *
 * A palette is a named, ordered list of RGB swatches for curated reuse, plus a
 * "recent colours" ring the painter feeds as the user picks colours.
 *
 * Bundled palettes are defined in C++ (not shipped as data files) so they
 * cannot go missing from an install; custom palettes serialize as JSON into
 * `<AppData>/paint/palettes/`. This mirrors GradientRamp (#544) exactly — see
 * that file for the same directory/JSON/safe-stem conventions.
 *
 * Pure data. Qt is used only in the .cpp for JSON I/O and AppData paths, so the
 * colour maths stays unit-testable headlessly.
 */
namespace ColorPaletteLibrary {

/// 8-bit RGB swatch. Alpha is deliberately absent: a palette curates HUES, and
/// the paint alpha is a brush property the user sets independently — baking
/// alpha into swatches would silently override it on every pick.
struct Swatch {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const Swatch& o) const { return r == o.r && g == o.g && b == o.b; }
};

struct Palette {
    std::string name;
    std::vector<Swatch> swatches;

    bool isValid() const { return !name.empty() && !swatches.empty(); }
};

/// Parse "#rrggbb" (or "rrggbb"). Returns false on malformed input rather than
/// silently yielding black, so a corrupt file is a skipped entry, not a row of
/// invisible swatches.
bool swatchFromHex(const std::string& hex, Swatch& out);
/// Lower-case "#rrggbb".
std::string swatchToHex(const Swatch& s);

/// The six bundled CC0 palettes required by #551.
std::vector<Palette> bundledPalettes();
/// Look up a bundled palette by exact name; nullptr when unknown.
const Palette* findBundled(const std::string& name);
/// True when `name` matches a bundled palette (drives the rename/delete gate).
bool isBundled(const std::string& name);

/// JSON schema: `{ "name": "...", "swatches": ["#rrggbb", ...] }`.
std::string toJson(const Palette& p);
bool fromJson(const std::string& json, Palette& out);

/// `<AppData>/paint/palettes`, created on demand. Empty when unavailable.
std::string palettesDirectory();
/// Write a custom palette; returns the file path, or empty on failure.
std::string saveCustom(const Palette& p);
/// Every parseable custom palette, name-sorted. Unparseable files are skipped.
std::vector<Palette> loadCustomPalettes();
bool deleteCustom(const std::string& name);

/// Bundled + custom, with custom overriding a bundled palette of the same name
/// (the same precedence BrushAssetLibrary::resolvePath uses for stamps).
std::vector<Palette> allPalettes();

/// Filesystem-safe stem for a palette name.
std::string safeFileStem(const std::string& name);

/// Push `s` onto the front of `recent`, de-duplicating and capping at `maxCount`
/// (most-recent first). Pure list maths, exposed for unit tests.
void pushRecent(std::vector<Swatch>& recent, const Swatch& s, size_t maxCount = 12);

/// Extract up to `maxColours` representative swatches from an RGBA8 image
/// (row-major, 4 bytes/px) by uniform colour-cube quantisation, ordered most
/// frequent first. Fully transparent pixels are ignored — they carry no colour
/// and would otherwise dominate a mostly-empty texture.
std::vector<Swatch> extractFromImage(const uint8_t* rgba, int width, int height,
                                     int maxColours = 10);

} // namespace ColorPaletteLibrary

#endif // COLOR_PALETTE_LIBRARY_H
