#ifndef GAME_READY_PRESETS_H
#define GAME_READY_PRESETS_H

#include <QString>
#include <QStringList>
#include <vector>

// Named game-ready budgets for image-to-3D generation (all backends:
// TRELLIS.2 / Pixal3D / TripoSR / TripoSG). ONE table feeds the Inspector's
// "Mesh" picker (MeshGenController::gameReadyPresets), the CLI
// (`qtmesh generate3d --game-preset <id>` / `--list-game-presets`) and the
// MCP `generate_mesh_from_image {game_preset}` argument, so the three
// surfaces cannot drift.
//
// A preset is a TRIANGLE budget plus, for platform presets, the upload
// limits that platform enforces:
//   * `targetTriangles`  — what the game-ready pass simplifies toward
//                          (0 = keep the generation's own density).
//   * `strictTriangles`  — the target is a HARD CEILING, not a suggestion.
//                          The ordinary pass accepts landing up to 2x above
//                          the budget when the error-capped simplify stops
//                          early (see Trellis2Bake::makeGameReady); a platform
//                          that refuses the upload past N triangles needs the
//                          count guaranteed, so the pass re-runs the
//                          topology-free simplifier until it fits.
//   * `maxTextureSize`   — the largest texture the platform accepts (0 = no
//                          cap). Callers clamp the bake size to it and say so.
//
// Roblox numbers (the reason the platform presets exist): a MeshPart is
// capped at 20,000 triangles and an accessory (rigid or layered clothing)
// at 4,000; uploaded textures are capped at 1024 px on a side.
namespace GameReady {

struct Preset {
    QString id;             // stable machine id: [a-z0-9-]+
    QString label;          // picker text
    int     targetTriangles = 0;
    bool    strictTriangles = false;
    int     maxTextureSize  = 0;
    QString note;           // one line for tooltips / CLI listing (may be empty)
};

// Every preset, in picker order (lightest budget first, "max" first of all).
const std::vector<Preset>& presets();

// The id the GUI preselects (Game Medium, 25k).
QString defaultId();

// Case-insensitive lookup by id; a few forgiving aliases ("roblox_meshpart",
// "roblox-mesh-part", "roblox", "accessory"). nullptr when unknown.
const Preset* find(const QString& id);

// Ids in table order, for usage text and the MCP schema enum.
QStringList ids();

// Index of `id` in presets() (-1 when unknown) — the picker's currentIndex.
int indexOf(const QString& id);

} // namespace GameReady

#endif // GAME_READY_PRESETS_H
