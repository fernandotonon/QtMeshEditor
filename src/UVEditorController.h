#ifndef UV_EDITOR_CONTROLLER_H
#define UV_EDITOR_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QRectF>
#include <QSet>
#include <QVariantList>
#include <vector>

#include <Ogre.h>

class EditableMesh;
class HalfEdgeMesh;

namespace Ogre {
class Entity;
class VertexData;
}

/// QML-facing singleton for the read-only UV editor panel (issue #459).
/// Extracts UV layouts from the active mesh selection, groups triangles
/// into islands via HalfEdgeMesh adjacency, and exposes draw data to
/// UVEditorPanel.qml (Canvas2D, software rendering).
class UVEditorController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int uvChannel READ uvChannel WRITE setUvChannel NOTIFY uvChannelChanged)
    Q_PROPERTY(bool showTextureBackground READ showTextureBackground WRITE setShowTextureBackground
                   NOTIFY showTextureBackgroundChanged)
    Q_PROPERTY(int meshRevision READ meshRevision NOTIFY meshDataChanged)
    Q_PROPERTY(bool hasMesh READ hasMesh NOTIFY meshDataChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY meshDataChanged)
    Q_PROPERTY(QString textureBackgroundSource READ textureBackgroundSource NOTIFY meshDataChanged)
    Q_PROPERTY(QRectF uvBounds READ uvBounds NOTIFY meshDataChanged)
    Q_PROPERTY(int islandCount READ islandCount NOTIFY meshDataChanged)

public:
    struct IslandResult {
        int islandCount = 0;
        std::vector<int> faceIslandIds;
    };

    static UVEditorController* instance();
    static UVEditorController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    int uvChannel() const { return m_uvChannel; }
    void setUvChannel(int channel);

    bool showTextureBackground() const { return m_showTextureBackground; }
    void setShowTextureBackground(bool on);

    int meshRevision() const { return m_meshRevision; }
    bool hasMesh() const { return m_hasMesh; }
    QString statusText() const { return m_statusText; }
    QString textureBackgroundSource() const { return m_textureBackgroundSource; }
    QRectF uvBounds() const { return m_uvBounds; }
    int islandCount() const { return m_islandCount; }

    /// Triangle draw payload for QML Canvas: list of
    /// { u0,v0,u1,v1,u2,v2, island, color } maps.
    Q_INVOKABLE QVariantList triangles() const { return m_triangles; }

    /// Re-read the active selection and rebuild cached draw data.
    Q_INVOKABLE void refresh();

    /// Activate/deactivate the controller, driven by the UV Editor dock's
    /// visibility. When INACTIVE, selection/entity-created/mesh-change signals
    /// only mark the cache dirty instead of rebuilding it — so importing a
    /// multi-submesh/multi-entity model doesn't pay the (expensive) per-entity
    /// UV read + island computation while nobody's looking at the panel. The
    /// pending rebuild runs lazily on the next setActive(true). No-op if the
    /// state is unchanged.
    void setActive(bool active);

    /// Connected UV islands for a mesh snapshot (headless tests).
    /// Caller must populate `EditableMesh` UVs for the channel under test.
    static IslandResult computeIslandsFromEditableMesh(const EditableMesh& mesh);

signals:
    void uvChannelChanged();
    void showTextureBackgroundChanged();
    void meshDataChanged();
    void fitToViewRequested();

private:
    explicit UVEditorController(QObject* parent = nullptr);
    ~UVEditorController() override = default;

    void connectSignals();
    // Gated signal-driven refresh (selection / entityCreated / mesh edits):
    // rebuilds only when active, else marks dirty. (refresh() is the explicit,
    // always-rebuild entry point.)
    void onSourceChanged();
    void rebuildMeshCache();
    bool buildFromEntity(Ogre::Entity* entity, const QSet<int>& submeshFilter, int uvChannel);
    static IslandResult computeIslandsFromHalfEdgeMesh(const HalfEdgeMesh& hem);
    static bool readUvChannel(const Ogre::VertexData* vertexData, int channel,
                              std::vector<Ogre::Vector2>& outUvs);
    static void applyUvChannel(EditableMesh& mesh, Ogre::Entity* entity, int channel,
                               const QSet<int>& submeshFilter);
    static QString resolveDiffuseTextureSource(Ogre::Entity* entity, int submeshIndex);
    static QString colorForIsland(int islandId);

    static UVEditorController* s_instance;

    int m_uvChannel = 0;
    bool m_showTextureBackground = true;
    int m_meshRevision = 0;
    bool m_hasMesh = false;
    // Gating: only rebuild the (expensive) UV cache when the panel is visible.
    // m_active mirrors the dock's visibility; m_dirty records that a rebuild is
    // owed once it becomes active again.
    bool m_active = false;
    bool m_dirty = true;
    QString m_statusText;
    QString m_textureBackgroundSource;
    QRectF m_uvBounds;
    int m_islandCount = 0;
    QVariantList m_triangles;
};

#endif // UV_EDITOR_CONTROLLER_H
