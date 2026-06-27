#ifndef UV_EDITOR_CONTROLLER_H
#define UV_EDITOR_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QRectF>
#include <QSet>
#include <QVariantList>
#include <QTimer>
#include <vector>
#include <unordered_map>

#include <Ogre.h>

class EditableMesh;
class HalfEdgeMesh;

namespace Ogre {
class Entity;
class VertexData;
}

/// QML-facing singleton for the UV editor panel (issues #459 / #460).
/// Extracts UV layouts, groups islands, supports UV-space component
/// selection (vertex / edge / face), with read-only island tint from Edit Mode.
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

    Q_PROPERTY(int selectionMode READ selectionMode WRITE setSelectionMode NOTIFY selectionModeChanged)
    Q_PROPERTY(int selectionRevision READ selectionRevision NOTIFY uvSelectionChanged)
    Q_PROPERTY(int selectedVertexCount READ selectedVertexCount NOTIFY uvSelectionChanged)
    Q_PROPERTY(int selectedEdgeCount READ selectedEdgeCount NOTIFY uvSelectionChanged)
    Q_PROPERTY(int selectedFaceCount READ selectedFaceCount NOTIFY uvSelectionChanged)

public:
    enum SelectionMode {
        VertexMode = 0,
        EdgeMode = 1,
        FaceMode = 2
    };
    Q_ENUM(SelectionMode)

    /// Pick / box-select modifier flags (match Qt::KeyboardModifier bits used from QML).
    enum SelectionModifier {
        NoModifier = 0,
        ShiftModifier = 0x02000000,
        ControlModifier = 0x04000000
    };
    Q_ENUM(SelectionModifier)

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

    int selectionMode() const { return static_cast<int>(m_selectionMode); }
    void setSelectionMode(int mode);

    int selectionRevision() const { return m_selectionRevision; }
    int selectedVertexCount() const { return static_cast<int>(m_selectedUvVerts.size()); }
    int selectedEdgeCount() const { return static_cast<int>(m_selectedUvEdges.size()); }
    int selectedFaceCount() const { return static_cast<int>(m_selectedUvFaces.size()); }

    /// Triangle draw payload for QML Canvas: list of
    /// { u0,v0,u1,v1,u2,v2, island, color } maps.
    Q_INVOKABLE QVariantList triangles() const { return m_triangles; }

    /// Highlight geometry for the active UV selection.
    Q_INVOKABLE QVariantList selectionVertices() const;
    Q_INVOKABLE QVariantList selectionEdges() const;
    Q_INVOKABLE QVariantList selectionFaces() const;

    /// Read-only island tint for the current 3D Edit Mode selection.
    Q_INVOKABLE QVariantList contextIslandFaces() const;

    Q_INVOKABLE void clearUvSelection();
    Q_INVOKABLE void pickAt(double u, double v, int modifiers, double pickRadiusUv);
    /// Box rules: vertex = fully enclosed; edge/face = any intersection/touch.
    Q_INVOKABLE void boxSelect(double uMin, double vMin, double uMax, double vMax, int modifiers);

    /// Re-read the active selection and rebuild cached draw data.
    Q_INVOKABLE void refresh();

    /// When false (UV Editor dock hidden), scene/selection changes are not
    /// rebuilt — avoids blocking the main thread during mesh import.
    Q_INVOKABLE void setPanelActive(bool active);
    bool panelActive() const { return m_panelActive; }

    /// Connected UV islands for a mesh snapshot (headless tests).
    /// Caller must populate `EditableMesh` UVs for the channel under test.
    static IslandResult computeIslandsFromEditableMesh(const EditableMesh& mesh);

signals:
    void uvChannelChanged();
    void showTextureBackgroundChanged();
    void meshDataChanged();
    void fitToViewRequested();
    void selectionModeChanged();
    void uvSelectionChanged();

private:
    struct UvVert {
        float u = 0.f;
        float v = 0.f;
        int meshGlobalVert = -1;
    };

    struct UvTri {
        float u[3]{};
        float v[3]{};
        int meshGlobalVert[3]{-1, -1, -1};
        int meshGlobalTri = -1;
        int island = 0;
        int uvVertId[3]{-1, -1, -1};
    };

    struct UvEdge {
        int v0 = -1;
        int v1 = -1;
        int meshGlobalV0 = -1;
        int meshGlobalV1 = -1;
    };

    explicit UVEditorController(QObject* parent = nullptr);
    ~UVEditorController() override = default;

    void connectSignals();
    void scheduleRefresh();
    void rebuildMeshCache();
    bool buildFromEntity(Ogre::Entity* entity, const QSet<int>& submeshFilter, int uvChannel);
    void applySelectionSet(const QSet<int>& verts, const QSet<int>& edges, const QSet<int>& faces,
                           int modifiers);
    void notifyUvSelectionChanged();
    void onEditSelectionChanged();
    void updateContextIslandsFromEdit();
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
    QString m_statusText;
    QString m_textureBackgroundSource;
    QRectF m_uvBounds;
    int m_islandCount = 0;
    QVariantList m_triangles;

    SelectionMode m_selectionMode = VertexMode;
    int m_selectionRevision = 0;
    QSet<int> m_selectedUvVerts;
    QSet<int> m_selectedUvEdges;
    QSet<int> m_selectedUvFaces;
    QSet<int> m_contextIslandIds;

    std::vector<UvVert> m_uvVerts;
    std::vector<UvTri> m_uvTris;
    std::vector<UvEdge> m_uvEdges;
    /// Per mesh-global-vertex list of triangle indices in m_uvTris (for fast context islands).
    std::vector<std::vector<int>> m_trisByGlobalVert;
    std::unordered_map<int, int> m_fiByGlobalTri;

    Ogre::Entity* m_activeEntity = nullptr;

    bool m_panelActive = false;
    bool m_refreshPending = false;
    QTimer* m_refreshTimer = nullptr;
};

#endif // UV_EDITOR_CONTROLLER_H