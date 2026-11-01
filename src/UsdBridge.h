#ifndef USDBRIDGE_H
#define USDBRIDGE_H

#include <QString>
#include <QStringList>
#include <vector>

#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>

struct aiScene;

///
/// USD (.usd/.usda/.usdc/.usdz) interop — Anim epic Slice J (#527).
///
/// The bridge converts between USD (via LightUSD, `-DENABLE_USD=ON`) and an
/// `aiScene`, and plugs itself into Assimp as an importer and an exporter. That
/// is deliberate: every skeleton / skin / blend-shape / clip rule the editor
/// already has for glTF and FBX lives in the Assimp<->Ogre processors
/// (`src/Assimp/*`) and `MeshImporterExporter::buildAiScene`, so USD gets the
/// same behaviour — and the same fixes — without a parallel Ogre builder.
///
/// The conversion layers are Ogre-free (an `aiScene` in, an `aiScene` out), so
/// they are unit-tested with a synthetic scene and a USD file on disk.
///
/// A build without ENABLE_USD still compiles and links: `available()` is false,
/// the register functions are no-ops and read/write return a clear
/// "rebuild with -DENABLE_USD=ON" error.
///
namespace UsdBridge {

/// True when this build can read and write USD.
bool available();

/// ".usd" / ".usda" / ".usdc" / ".usdz" (case-insensitive extension check).
bool isUsdPath(const QString& path);

/// The extensions the bridge handles, without the dot.
QStringList extensions();

/// Message returned/logged when USD is requested in a build without it.
QString unavailableMessage();

/// Register the USD reader with an Assimp importer (no-op without ENABLE_USD).
/// Call before `ReadFile` on every importer that may see a USD path.
void registerImporter(Assimp::Importer& importer);

/// Register the `usda` / `usdc` / `usd` / `usdz` writers with an Assimp
/// exporter (no-op without ENABLE_USD).
void registerExporter(Assimp::Exporter& exporter);

/// A morph-weight clip to write next to the geometry. Assimp's aiScene has no
/// transport for morph-weight animation (the glTF path injects it after the
/// fact), so the caller hands it to the writer through `setWriteExtras`.
struct MorphWeightClip {
    std::string name;
    std::vector<std::string> shapes;   ///< blend-shape names, in column order
    std::vector<double> times;         ///< seconds
    std::vector<std::vector<float>> weights;  ///< [time][shape]
};

/// Extra data for the NEXT write on this thread (cleared by `clearWriteExtras`).
void setWriteExtras(const std::vector<MorphWeightClip>& morphClips);
void clearWriteExtras();

/// Read a USD file into a fresh aiScene (caller deletes). Null + `err` on
/// failure; `warn` receives non-fatal converter notes.
aiScene* readScene(const QString& path, QString* err, QString* warn = nullptr);

/// Write an aiScene to `path` (format picked from the extension). Returns
/// false and fills `err` on failure; a failed write never leaves a truncated
/// file behind.
bool writeScene(const QString& path, const aiScene* scene, QString* err,
                QString* warn = nullptr);

}  // namespace UsdBridge

#endif  // USDBRIDGE_H
