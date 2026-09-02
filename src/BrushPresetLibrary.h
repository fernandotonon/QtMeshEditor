#ifndef BRUSH_PRESET_LIBRARY_H
#define BRUSH_PRESET_LIBRARY_H

#include <string>
#include <vector>

/**
 * @brief Paint v2 Slice H (#551) — brush preset library (Ogre-free).
 *
 * A preset is a full snapshot of the brush configuration: tool, footprint,
 * stamp, size/strength/falloff, colour source, and the stamp dynamics
 * (spacing / scatter / jitter / rotation). Applying one restores the whole
 * brush in a single click.
 *
 * Bundled presets are defined in C++ (not shipped as data files) so they cannot
 * go missing from an install; custom presets serialize as JSON into
 * `<AppData>/paint/presets/`. Same conventions as GradientRamp (#544) and
 * ColorPaletteLibrary.
 *
 * Pure data — Qt appears only in the .cpp for JSON I/O and AppData paths. The
 * enum values below intentionally mirror the controller enums numerically; see
 * `Preset` for why they are stored as ints rather than including the headers.
 */
namespace BrushPresetLibrary {

/**
 * A captured brush configuration.
 *
 * Enum-valued fields are plain ints holding the corresponding controller enum
 * value (TexturePaintController::BrushTool, BrushFootprint::FootprintType /
 * StampRotation, TexturePaintController::ColorSource / GradientMode,
 * PaintChannelNS::Channel, EditModeController::BrushShape). They are ints so
 * this stays a pure-data header with no dependency on the Ogre/Qt-heavy
 * controllers — the apply layer does the conversion. They are also what gets
 * written to JSON, so DO NOT renumber the controller enums without a format
 * version bump.
 */
struct Preset {
    std::string name;

    // --- core brush ---
    int    tool = 0;              ///< BrushTool (0 = Paint)
    double radius = 0.05;         ///< mesh-local units
    double strength = 1.0;        ///< 0..1
    double falloff = 0.5;         ///< 0..1
    int    shape = 0;             ///< BrushShape (0 = Round, 1 = Square)
    int    channel = 0;           ///< PaintChannelNS::Channel

    // --- footprint ---
    int         footprint = 0;    ///< FootprintType (0 Round/1 Square/2 Stamp/3 Tiling)
    std::string stamp;            ///< stamp asset name; empty = none
    std::string tiling;           ///< tiling asset name; empty = none

    // --- stamp dynamics ---
    double spacing = 0.35;        ///< 0.05..2.0
    double scatter = 0.0;         ///< 0..1
    double sizeJitter = 0.0;      ///< 0..1
    double opacityJitter = 0.0;   ///< 0..1
    int    stampRotation = 0;     ///< StampRotation (0 None/1 Fixed/2 Stroke/3 Random)
    double stampAngleDeg = 0.0;

    // --- colour source ---
    int         colorSource = 0;  ///< ColorSource (0 Solid, 1 Gradient)
    int         gradientMode = 0; ///< GradientMode (0 Linear, 1 Radial, 2 Angular)
    /// True = use the FG/BG two-stop ramp instead of `rampName`. Stored
    /// explicitly rather than inferred from an empty rampName, so applying a
    /// preset cannot fall back to whichever named ramp was previously active.
    bool        useFgBgRamp = false;
    std::string rampName;         ///< gradient ramp name; ignored when useFgBgRamp

    /// Optional one-line description shown as a tooltip.
    std::string note;

    bool isValid() const { return !name.empty(); }
};

/// The 15 bundled presets required by #551.
std::vector<Preset> bundledPresets();
/// Look up a bundled preset by exact name; nullptr when unknown.
const Preset* findBundled(const std::string& name);
/// True when `name` matches a bundled preset (gates rename/delete in the UI).
bool isBundled(const std::string& name);

/// JSON round-trip. Unknown/missing fields fall back to the struct defaults, so
/// a preset written by an older build still loads.
std::string toJson(const Preset& p);
bool fromJson(const std::string& json, Preset& out);

/// `<AppData>/paint/presets`, created on demand. Empty when unavailable.
std::string presetsDirectory();
std::string saveCustom(const Preset& p);
std::vector<Preset> loadCustomPresets();
bool deleteCustom(const std::string& name);

/// Bundled + custom, with custom overriding a bundled preset of the same name.
std::vector<Preset> allPresets();
/// Look up across bundled + custom (custom wins).
bool findPreset(const std::string& name, Preset& out);

/// Filesystem-safe stem for a preset name.
std::string safeFileStem(const std::string& name);

/// Export/import a preset to/from an arbitrary path (the issue's JSON
/// import/export). Returns false on I/O or parse failure.
bool exportToFile(const Preset& p, const std::string& path);
bool importFromFile(const std::string& path, Preset& out);

} // namespace BrushPresetLibrary

#endif // BRUSH_PRESET_LIBRARY_H
