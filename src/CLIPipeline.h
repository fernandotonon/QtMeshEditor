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
    static int cmdScan(int argc, char* argv[]);
    static int cmdMaterial(int argc, char* argv[]);
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

    /// Map file extension to MeshImporterExporter format string.
    static QString formatForExtension(const QString& path);
};

#endif // CLIPIPELINE_H
