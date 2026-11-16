#ifndef SHAPE_GUIDE_H
#define SHAPE_GUIDE_H

#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <vector>

// Shape guide (blockout control) for the TRELLIS.2 / Pixal3D backends.
//
// A rough low-poly mesh — a preset blockout (house, car, bus, ...) or any mesh
// the user made — steers WHERE the generated object puts its volume, and can
// optionally be the mesh the generation's textures are baked onto, so the
// result keeps the user's own low-poly topology.
//
// The method is SpaceControl's (MIT, training-free): the guide is voxelised to
// the 64^3 sparse-structure grid, encoded with TRELLIS's sparse-structure VAE
// encoder (TRELLIS-1 `ss_enc_conv3d_16l8_fp16`, MIT, which TRELLIS.2 reuses),
// and the sparse-structure flow starts from that latent instead of pure noise
// (trellis-cli `--ss-control`, in our trellis.cpp fork; cmake/TrellisCpp.cmake),
// skipping the first `strength` fraction of its steps.
//
// Measured on this project (res 512, 12 SS steps): <= 0.33 the IMAGE leads
// and the guide fixes scale/placement (and suppresses a floor slab); 0.5-0.67
// gives incoherent hybrids when image and guide disagree; >= 0.67 hugs the
// guide but its flat faces stay flat. Hence the 0.33 default.
//
// Pure data apart from the ONNX encoder (ENABLE_ONNX) — no Ogre.
namespace ShapeGuide {

// A triangle mesh in the EDITOR frame (+Y up, front towards +Z, metres).
struct Mesh {
    std::vector<float> positions;   // xyz per vertex
    std::vector<uint32_t> indices;  // 3 per triangle
    bool empty() const { return indices.size() < 3 || positions.size() < 9; }
};

// ---- presets ---------------------------------------------------------------
// Strength from which the generation hugs the guide closely enough to bake
// onto the guide mesh itself (the output IS the guide, so below this the
// image's shape would be stretched over it). The GUI offers the bake from
// here, the CLI/MCP default to it when baking onto the guide without an
// explicit strength, and the predictor warns below it.
inline constexpr float kBakeOnGuideMinStrength = 0.6f;

// ---- TripoSG as the guide ----------------------------------------------------
// `--guide triposg`: the guide is not a file but TripoSG's geometry of the SAME
// image, so the result can keep TripoSG's sharper shape (with
// --bake-onto-guide) while TRELLIS.2 paints it. Measured on a car photo:
//  * the SDF grid must be finer than TripoSG's own default (256): at 256 the
//    surface is terraced and the bake rays (cast along the guide's normals)
//    land in the gaps between terraces and pick up dark interior colour =
//    black stripes across the paint. 384 removed them (~2.4x the CPU time).
//  * 0.8 strength keeps TRELLIS.2 on the guide's shape, so its colours land
//    where the guide's surface is.
inline const QString kTripoSGGuideId = QStringLiteral("triposg");
inline constexpr int kTripoSGGuideResolution = 384;
inline constexpr float kTripoSGGuideDefaultStrength = 0.8f;
// Triangle budget of the TripoSG guide when the caller sets none: it becomes
// the output mesh with --bake-onto-guide, and the dense 384 grid surface
// (~1M triangles) is far more than a texture bake or the 64^3 guide voxels need.
inline constexpr int kTripoSGGuideDefaultTriangles = 50000;
inline bool isTripoSGGuide(const QString& spec)
{
    return spec.trimmed().compare(kTripoSGGuideId, Qt::CaseInsensitive) == 0;
}
// TripoSG predictor output (+Y up, front towards -Z, unit box) -> the editor
// frame a guide is given in (front towards +Z: MeshGenBuilder's 180 degree turn
// about Y) at the size a TripoSG generation arrives with (MeshGenBuilder scales
// those x`scale`; a result baked onto the guide comes back at the guide's own
// size, so the guide carries that scale). In place.
void tripoSGToEditorFrame(std::vector<float>& positions, float scale);

struct PresetInfo {
    QString id;        // stable id: "house", "car", ...
    QString label;     // UI label
    QString category;  // "Buildings" | "Vehicles" | "Props" | "Characters"
    QString hint;      // one-line description / suggested prompt
};
const std::vector<PresetInfo>& presets();
// Empty mesh for an unknown id.
Mesh preset(const QString& id);
bool isPreset(const QString& id);

// Wavefront OBJ (v / f lines; n-gons fan-triangulated; texture/normal indices
// ignored). Writing keeps one `o` object.
bool readObj(const QString& path, Mesh& out, QString* error = nullptr);
bool writeObj(const QString& path, const Mesh& mesh, const QString& objectName = {},
              QString* error = nullptr);

// ---- grid frame --------------------------------------------------------------
// The TRELLIS latent grid is Z-up: editor (X, Y, Z) = grid (-gx, gz, gy), i.e.
// grid = (-X, Z, Y) — the inverse of the rotations the TRELLIS path applies
// to its output (MeshGenPredictor -90 X, MeshGenBuilder 180 Y).
// The guide is centred and scaled so its largest side spans `extent` of the
// [-0.5, 0.5] cube:  grid = scale * R(editor - centre).
struct GridPlacement {
    float scale = 1.0f;
    float centre[3] = {0.0f, 0.0f, 0.0f};   // editor frame
};
GridPlacement placementFor(const Mesh& editorMesh, float extent = 0.95f);
// Editor-frame mesh -> grid frame (positions only; winding preserved, R is
// proper).
Mesh toGrid(const Mesh& editorMesh, const GridPlacement& p);
// Rotation part only (grid = R(v)), and its inverse.
inline void editorToGridDir(const float e[3], float g[3]) { g[0] = -e[0]; g[1] = e[2]; g[2] = e[1]; }
inline void gridToEditorDir(const float g[3], float e[3]) { e[0] = -g[0]; e[1] = g[2]; e[2] = g[1]; }

// Surface occupancy at res^3 of a GRID-frame mesh ([-0.5,0.5] cube), index
// (x, y, z) -> x*res*res + y*res + z. Triangles are sampled densely enough
// that no voxel the surface crosses is skipped.
std::vector<uint8_t> voxelizeSurface(const Mesh& gridMesh, int res = 64);

// ---- encoder ---------------------------------------------------------------
bool encoderAvailable();          // false on a non-ONNX build
QString encoderModelPath();       // <ai_models>/trellis2/ss_encoder.onnx
// Downloads the encoder on first use (QTMESH_SHAPE_GUIDE_MODEL_BASE_URL /
// QSettings ai/shapeGuideModelBaseUrl; QTMESH_SHAPE_GUIDE_NO_DOWNLOAD blocks).
// Must be called on the main thread (ModelDownloader lives there).
// `cancelled` is polled during a download; returning true stops it.
QString ensureEncoderBlocking(QString* error = nullptr,
                              const std::function<bool()>& cancelled = {});
// occupancy (64^3, layout above) -> latent in the layout trellis-cli's SS
// sampler uses: float [4096][8], channel fastest, s = x*256 + y*16 + z.
bool encode(const std::vector<uint8_t>& occupancy64, const QString& modelFile,
            std::vector<float>& latent, QString* error = nullptr);
// Convenience: editor-frame guide -> latent file for `--ss-control`.
bool writeControlLatent(const Mesh& editorMesh, const QString& modelFile,
                        const QString& outPath, QString* error = nullptr);

} // namespace ShapeGuide

#endif // SHAPE_GUIDE_H
