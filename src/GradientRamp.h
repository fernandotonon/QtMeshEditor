#ifndef GRADIENT_RAMP_H
#define GRADIENT_RAMP_H

#include <string>
#include <vector>

/**
 * @brief Paint v2 Slice A (#544) — gradient ramp data model (Ogre-free).
 *
 * An ordered list of colour stops on [0,1] with linear or stepped
 * interpolation. Bundled CC0 presets live here; custom ramps serialize
 * as JSON into `<AppData>/paint/ramps/`.
 *
 * Pure data. Qt is used only in the .cpp for JSON I/O and AppData paths;
 * the sample() path has no Qt dependency so unit tests stay headless.
 */
namespace GradientRamp {

struct Rgba {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct Stop {
    float position = 0.0f; ///< ∈ [0,1]
    Rgba colour;
};

enum class Interpolate {
    Linear = 0,  ///< Bilinear lerp between neighbouring stops.
    Stepped = 1, ///< Hold the left stop's colour until the next stop.
};

struct Ramp {
    std::string name;
    std::vector<Stop> stops;
    Interpolate interpolate = Interpolate::Linear;

    /// Sample the ramp at t ∈ [0,1] (clamped). Empty ramps return black.
    Rgba sample(float t) const;

    /// True when at least two stops are present (a usable ramp).
    bool isValid() const { return stops.size() >= 2; }
};

/// Build a two-stop FG→BG ramp (Photoshop / Krita quick mode).
Ramp fromFgBg(const Rgba& fg, const Rgba& bg,
              const std::string& name = "FG/BG");

/// The six bundled CC0 presets required by #544.
std::vector<Ramp> bundledPresets();

/// Look up a bundled preset by exact name. Returns nullptr if unknown.
const Ramp* findBundled(const std::string& name);

/// JSON schema: `{ "name", "interpolate": "linear"|"stepped",
/// "stops": [ { "t", "r", "g", "b", "a" } ] }`.
std::string toJson(const Ramp& ramp);
bool fromJson(const std::string& json, Ramp& out);

/// `<AppData>/paint/ramps/` — created on demand. Empty when AppData is
/// unavailable (headless tests without QCoreApplication).
std::string rampsDirectory();

/// Save `ramp` as `<rampsDirectory()>/<safeName>.json`. Returns the
/// written path, or empty on failure.
std::string saveCustom(const Ramp& ramp);

/// Load every `*.json` in the custom ramps directory.
std::vector<Ramp> loadCustomRamps();

/// Delete a custom ramp file by name. Returns true if a file was removed.
bool deleteCustom(const std::string& name);

/// Sanitize a ramp name into a filesystem-safe stem (no path seps).
std::string safeFileStem(const std::string& name);

} // namespace GradientRamp

#endif // GRADIENT_RAMP_H
