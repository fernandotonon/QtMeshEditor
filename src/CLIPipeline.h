#ifndef CLIPIPELINE_H
#define CLIPIPELINE_H

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <Ogre.h>
#include <assimp/postprocess.h>

struct MeshInfo {
    QString file;
    unsigned int vertices = 0;
    unsigned int triangles = 0;
    unsigned int submeshes = 0;
    QStringList materials;
    QStringList textures;
    int upAxis = 1; // 1=Y-up (default/Mixamo), 2=Z-up (Unreal Engine)
    QString skeletonName;
    unsigned short boneCount = 0;
    QStringList bones;
    struct AnimInfo {
        QString name;
        float duration = 0.0f;
    };
    QList<AnimInfo> animations;
    Ogre::Vector3 bbMin = Ogre::Vector3::ZERO;
    Ogre::Vector3 bbMax = Ogre::Vector3::ZERO;

    // Per-submesh ACMR (Average Cache Miss Ratio, 32-entry post-T&L
    // cache, meshoptimizer convention). Useful for downstream tooling
    // to decide whether `qtmesh convert` would benefit from re-export
    // with the optimizer enabled. Empty list when the submesh's index
    // count is zero. Indexed by submesh index. Issue #399.
    struct SubmeshAcmr {
        int    submeshIndex   = 0;
        int    triangleCount  = 0;
        double acmr           = 0.0;
    };
    QList<SubmeshAcmr> submeshAcmr;
};

struct FixOptions {
    bool removeDegenerates = false;
    bool mergeMaterials = false;

    bool anySet() const {
        return removeDegenerates || mergeMaterials;
    }

    unsigned int toAssimpFlags() const {
        unsigned int flags = 0;
        if (removeDegenerates) flags |= aiProcess_FindDegenerates;
        if (mergeMaterials)    flags |= aiProcess_RemoveRedundantMaterials;
        return flags;
    }
};

class CLIPipeline {
public:
    CLIPipeline() = delete;

    /// Entry point: parse argv and dispatch to subcommand.
    /// Returns process exit code (0=success, 1=runtime error, 2=usage error).
    static int run(int argc, char* argv[]);

    /// Extract mesh info from a loaded Ogre Entity (pure data, no I/O).
    static MeshInfo extractMeshInfo(const Ogre::Entity* entity, const QString& fileName);

    /// Format MeshInfo as human-readable text.
    static QString formatMeshInfoText(const MeshInfo& info);

    /// Format MeshInfo as JSON string.
    static QString formatMeshInfoJson(const MeshInfo& info);

    static void printUsage();
    static void printVersion();

    /// Initialize Ogre in headless mode (hidden render window).
    /// Returns true on success.  Idempotent — safe to call after tryInitOgre().
    static bool initOgreHeadless();

    static int cmdInfo(int argc, char* argv[]);
    static int cmdFix(int argc, char* argv[]);
    static int cmdConvert(int argc, char* argv[]);
    static int cmdAnim(int argc, char* argv[]);
    static int cmdValidate(int argc, char* argv[]);
    static int cmdLod(int argc, char* argv[]);
    static int cmdPose(int argc, char* argv[]);
    /// Render a mesh turntable as PNG frame(s) or a horizontal sprite sheet (#294).
    static int cmdTurntable(int argc, char* argv[]);
    /// Render an 8-direction isometric sprite grid (rows = directions, cols = frames) (#724).
    static int cmdIsometric(int argc, char* argv[]);
    static int cmdScan(int argc, char* argv[]);
    static int cmdMaterial(int argc, char* argv[]);
    /// #403: depth-conditioned (ControlNet) mesh-aware texture generation,
    /// headless equivalent of the Material Editor's "Use selected mesh" path.
    /// Loads an SD base model, renders a front-view depth map, drives the
    /// async SDManager worker synchronously, binds the result as diffuse, and
    /// re-exports. No-op error (exit 1) when built without ENABLE_STABLE_DIFFUSION.
    static int cmdMaterialGenerateTexture(const QString& inputPath,
                                          QString outputPath,
                                          const QString& prompt,
                                          const QString& modelName,
                                          QString controlNetPath,
                                          double controlStrength,
                                          int width, int height);
    /// #404: ONNX PBR map synthesis from a diffuse texture. Produces
    /// normal/roughness/height PNGs next to the albedo; if a mesh is given,
    /// binds them into the slice-E canonical slots and re-exports. Exit 1 when
    /// built without ENABLE_ONNX.
    static int cmdMaterialGeneratePbr(const QString& albedoPath,
                                      const QString& meshPath,
                                      QString outputPath,
                                      int tileSize, bool wantNormal,
                                      bool wantRoughness, bool wantHeight);
    /// Slice G: pack 1-4 grayscale source images into a single RGBA
    /// output texture. Headless / scriptable equivalent of the GUI
    /// "Pack Channels…" dialog.
    static int cmdPackTextures(int argc, char* argv[]);
    /// Slice H: generate a tangent-space normal map from a height/bump
    /// source via Sobel filter. Headless equivalent of the GUI
    /// "Generate Normal Map…" dialog.
    static int cmdNormalFromHeight(int argc, char* argv[]);
    /// Phase 6 slice E: pack N input texture files into a single atlas
    /// image + JSON manifest of per-tile UV remaps. Pure-data; no Ogre.
    /// Reduces draw-call count by consolidating many small textures into
    /// one binding.
    static int cmdAtlas(int argc, char* argv[]);
    /// Phase 6 slice E2: apply a previously-packed atlas to a mesh.
    /// Reads the manifest JSON, scales+biases UV0 of every submesh whose
    /// diffuse texture matches a tile into the tile's sub-rect, and
    /// rebinds the diffuse TUS to the atlas texture. The output is a
    /// single-binding-friendly mesh.
    static int cmdAtlasApply(int argc, char* argv[]);
    /// Phase 6 slice A: estimate GPU memory & VRAM for a mesh file
    /// (per-submesh + per-texture, optional --json, optional --budget).
    static int cmdMemory(int argc, char* argv[]);
    /// Phase 6 slice B: analyze draw calls and surface merge opportunities
    /// (per-material grouping, optional --json).
    static int cmdAnalyze(int argc, char* argv[]);
    /// Phase 6 slice C: vertex-cache (Forsyth) optimization. With -o, write
    /// the reordered mesh; without -o, analyze only.
    static int cmdVertexCache(int argc, char* argv[]);
    /// Phase 6 slice D: single-pass mesh decimation. Reduce the base mesh
    /// to a target triangle count, vertex budget, or percent reduction.
    static int cmdDecimate(int argc, char* argv[]);
    /// Phase 6 slice G: batch optimize pipeline. Runs the slice A–E
    /// optimizations end-to-end on a single asset: vertex-cache reorder,
    /// optional decimation, optional animation simplify. Emits a
    /// per-stage before/after report (text or --json).
    static int cmdOptimize(int argc, char* argv[]);

    /// Phase 7 paint: bake EditableMesh vertex colors into a UV-space
    /// PNG texture with configurable resolution and seam dilation.
    /// Surfaces VertexColorBaker via the CLI for headless asset pipelines.
    static int cmdBakeVertexColors(int argc, char* argv[]);

    /// Bake a skeletal animation into a Vertex Animation Texture (VAT)
    /// + JSON sidecar. Surfaces `VATBaker::bake()` over the CLI for
    /// headless asset pipelines. Slice 1: engine-agnostic / RGBA8 /
    /// positions only.
    static int cmdVat(int argc, char* argv[]);

    /// xatlas-backed automatic UV unwrap. `qtmesh uv mesh.fbx
    /// --unwrap [--resolution N] [--padding P] [--channel C] -o
    /// out.fbx` writes non-overlapping UVs into the chosen channel
    /// (default 0) and exports. `--info [--json]` reports current
    /// UV channels without mutating. Issue #400.
    static int cmdUv(int argc, char* argv[]);

    /// Quad retopology via triangle pairing. Walks every interior edge
    /// whose two adjacent faces are triangles and scores the merge by
    /// coplanarity + quad shape + aspect ratio; takes the best pairs
    /// greedily. Output is committed via the n-gon binding so quads
    /// round-trip through the FBX / glTF exporter. Issue #401.
    static int cmdRetopo(int argc, char* argv[]);

    /// Compute skin weights for a mesh + skeleton via inverse-
    /// distance heuristic. Issue #402.
    static int cmdSkin(int argc, char* argv[]);

    /// List the morph targets / blend shapes on a mesh file. Slice A1
    /// surfaces a `--list` mode only; subsequent slices add `--set`,
    /// `--add`, `--delete` once the in-memory authoring path lands.
    static int cmdMorph(int argc, char* argv[]);

    /// Inspect node-animation clips on a scene file. Slice C-CLI
    /// initial surface: `--list [--json]` lists clips that have at
    /// least one NodeAnimationTrack (typically created via
    /// in-editor authoring or the C6 MCP tools and round-tripped
    /// through the scene exporter). Authoring on the CLI side
    /// (`--add-clip`, `--add-keyframe`, `-o out.gltf`) needs the
    /// glTF/FBX exporter round-trip from C5 first.
    static int cmdNodeAnim(int argc, char* argv[]);

    /// Map file extension to MeshImporterExporter format string.
    static QString formatForExtension(const QString& path);
};

#endif // CLIPIPELINE_H
