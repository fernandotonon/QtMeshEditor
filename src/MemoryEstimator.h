#ifndef MEMORYESTIMATOR_H
#define MEMORYESTIMATOR_H

#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonObject>

namespace Ogre {
    class Entity;
    class Mesh;
    class Texture;
}

// Per-mesh GPU memory breakdown, derived from vertex/index buffer sizes
// declared on the loaded Ogre mesh. Geometry only — texture VRAM is reported
// separately so callers can warn on each axis independently.
struct MeshMemoryEstimate {
    QString name;
    quint64 vertexBytes = 0;
    quint64 indexBytes = 0;
    unsigned int vertexCount = 0;
    unsigned int indexCount = 0;

    quint64 totalBytes() const { return vertexBytes + indexBytes; }
};

// Per-texture VRAM estimate: width * height * bytesPerPixel, plus a mip
// overhead factor (1.33x for full mip chain, 1.0x for base level only).
struct TextureMemoryEstimate {
    QString name;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int bytesPerPixel = 0;
    bool hasMips = false;
    quint64 bytes = 0;
};

struct SceneMemoryReport {
    QList<MeshMemoryEstimate> meshes;
    QList<TextureMemoryEstimate> textures;
    quint64 meshTotalBytes = 0;
    quint64 textureTotalBytes = 0;
    quint64 budgetBytes = 0;          // 0 = unlimited

    quint64 totalBytes() const { return meshTotalBytes + textureTotalBytes; }
    bool overBudget() const { return budgetBytes > 0 && totalBytes() > budgetBytes; }
};

// Pure-data memory estimator.  All methods are static and side-effect free.
// Designed so unit tests can exercise the byte math without an Ogre context.
class MemoryEstimator {
public:
    // ---- Primitive estimators (testable without Ogre) ----

    // Geometry bytes for a single submesh: vertexCount * vertexStride + indexCount * indexSize.
    static quint64 meshBytes(unsigned int vertexCount, unsigned int vertexStride,
                             unsigned int indexCount, unsigned int indexSize);

    // Texture VRAM bytes: width * height * bpp, optionally with mip overhead (4/3x).
    static quint64 textureBytes(unsigned int width, unsigned int height,
                                unsigned int bytesPerPixel, bool hasMips);

    // Format byte count as human-readable: "1.23 MB", "456 KB", "789 B".
    static QString formatBytes(quint64 bytes);

    // Parse "50MB", "1.5 GB", "2048KB" → bytes.  Returns 0 on parse error.
    static quint64 parseBudget(const QString& spec);

    // ---- Ogre-backed estimators ----

    // Walk every submesh on the entity and accumulate vertex/index bytes.
    static MeshMemoryEstimate estimateEntity(const Ogre::Entity* entity);

    // Walk Ogre::TextureManager and return one estimate per named texture.
    // Filters: textures not yet loaded (no width/height) are skipped so the
    // total reflects what is actually resident on the GPU right now.
    static QList<TextureMemoryEstimate> estimateAllTextures();

    // Build a scene-wide report by iterating Manager's scene nodes.
    // budgetBytes=0 disables the over-budget flag.
    static SceneMemoryReport estimateScene(quint64 budgetBytes = 0);

    // Serialize a SceneMemoryReport as JSON (used by CLI/MCP).
    static QJsonObject toJson(const SceneMemoryReport& report);

    // Serialize as human-readable text (CLI default output).
    static QString toText(const SceneMemoryReport& report);
};

#endif // MEMORYESTIMATOR_H
