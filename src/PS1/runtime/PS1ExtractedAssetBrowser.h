#ifndef PS1EXTRACTEDASSETBROWSER_H
#define PS1EXTRACTEDASSETBROWSER_H

#include <QAbstractListModel>
#include <QImage>
#include <QSortFilterProxyModel>
#include <QString>
#include <QWidget>

#include "PS1CapturedAssets.h"

class QLabel;
class QLineEdit;
class QListView;
class QTabWidget;
class QToolButton;

/**
 * Asset browser kinds (#426). The browser uses one model per kind so
 * tab switches don't have to invalidate a single filter — each tab keeps
 * its own selection/scroll state, just like Finder / Explorer.
 */
enum class ExtractedAssetKind {
    Mesh = 0,
    Texture,
    Material,
};

/**
 * One thumbnail tile shown in the Extracted Asset Browser (#426). Pure
 * data — no Ogre handles — so the model can be exercised from unit tests
 * without an Ogre/Qt-OpenGL session.
 */
struct ExtractedAssetTile
{
    ExtractedAssetKind kind = ExtractedAssetKind::Mesh;
    QString displayName;
    /** Stable identifier used for drag-and-drop MIME payloads. For
     *  meshes this is the Ogre mesh resource name (e.g. `ps1_unique_0_<captureId>`),
     *  for textures and materials it is the logical material name. */
    QString assetId;
    /** Original mesh / instance / material indexes into
     *  `CapturedAssetSet`. -1 when not applicable to the kind. */
    int meshIndex = -1;
    int instanceCount = 0;
    int triangleCount = 0;
    /** Whether the mesh is "textured" (at least one submesh has a texture
     *  page). Drives the filter chip. */
    bool textured = false;
    /** Whether the mesh / material has only vertex colors (no texture). */
    bool coloredOnly = false;
    /** Whether the mesh has more than one placed instance — used for the
     *  `instanced` filter chip. */
    bool instanced = false;
    /** Preview pixmap (texture image for Texture/Material tabs, generated
     *  thumbnail for Mesh tab). The Mesh tab uses a software-rendered
     *  schematic until we wire up offscreen Ogre rendering. */
    QImage thumbnail;
};

/**
 * Generic list model backing one tab of the Extracted Asset Browser.
 *
 * Rather than maintain three separate widget hierarchies, we share a
 * single `QListView` template per tab and swap the model + filter.
 * Drag-and-drop is implemented at the model level (`mimeData()`) so
 * `application/x-ps1rip-mesh` (etc.) payloads survive across re-parents.
 */
class PS1ExtractedAssetModel : public QAbstractListModel
{
    Q_OBJECT

public:
    explicit PS1ExtractedAssetModel(ExtractedAssetKind kind, QObject *parent = nullptr);

    void setTiles(QVector<ExtractedAssetTile> tiles);
    const QVector<ExtractedAssetTile> &tiles() const { return m_tiles; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;

    /** Stable MIME type identifiers shared with `PS1RipSessionWindow`'s
     *  drop handler. Mesh has its own type so we can validate drops only
     *  fire on the editor viewport. */
    static QString mimeTypeForKind(ExtractedAssetKind kind);

private:
    ExtractedAssetKind m_kind;
    QVector<ExtractedAssetTile> m_tiles;
};

/**
 * Filter proxy driving the per-tab search box + chip toggles
 * (textured / colored-only / instanced).
 */
class PS1ExtractedAssetFilter : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit PS1ExtractedAssetFilter(QObject *parent = nullptr);

    void setShowTextured(bool show);
    void setShowColoredOnly(bool show);
    void setShowInstanced(bool show);
    void setSearchText(const QString &text);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    bool m_showTextured = true;
    bool m_showColoredOnly = true;
    bool m_showInstanced = true;
    QString m_searchText;
};

/**
 * Extracted Asset Browser dock body (#426). Three QListViews in icon mode
 * (Meshes / Textures / Materials), each with its own model + filter proxy.
 * Drag-and-drop a mesh tile into the editor viewport → emits
 * `instantiateMesh(meshIndex, assetId)` so the session window can clone
 * the Ogre mesh and create a permanent SceneNode.
 *
 * Thumbnail generation:
 * - Textures: directly from `CapturedAssetSet::textureImages`.
 * - Materials: same image as textures (PS1 materials are mostly tpage +
 *   tint), with a small color swatch overlay for solid materials.
 * - Meshes: software-rendered icon (triangle count badge + colored swatch)
 *   until offscreen Ogre rendering lands as a follow-up. The icon API is
 *   the same so the future renderer is a drop-in replacement.
 */
class PS1ExtractedAssetBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit PS1ExtractedAssetBrowser(PS1CapturedAssets *store, QWidget *parent = nullptr);

    void refreshHeader();

    /** Test/UI accessors. */
    QListView *meshView() const { return m_meshView; }
    QListView *textureView() const { return m_textureView; }
    QListView *materialView() const { return m_materialView; }
    PS1ExtractedAssetModel *meshModel() const { return m_meshModel; }
    PS1ExtractedAssetModel *textureModel() const { return m_textureModel; }
    PS1ExtractedAssetModel *materialModel() const { return m_materialModel; }

signals:
    /** Fires on double-click on a mesh tile or on a successful drag-drop
     *  out to a drop target — the session window listens and clones the
     *  Ogre mesh into a fresh SceneNode (#426 acceptance criterion:
     *  "Drag-and-drop creates a permanent entity"). */
    void instantiateMesh(int meshIndex, const QString &assetId);

public:
    /** Pure-data helper used by both the live UI path and unit tests.
     *  Exposed publicly so tests can exercise tile construction without
     *  needing a live QWidget tree. */
    static QVector<ExtractedAssetTile>
    buildTilesForKind(const CapturedAssetSet &set, ExtractedAssetKind kind);

private slots:
    void onCaptureSetChanged();
    void onMeshActivated(const QModelIndex &index);

private:
    void rebuildModels();
    static QImage placeholderThumbnail(const QString &caption, const QColor &accent);

    PS1CapturedAssets *m_store = nullptr;
    QTabWidget *m_tabs = nullptr;
    QLabel *m_countsLabel = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QToolButton *m_chipTextured = nullptr;
    QToolButton *m_chipColored = nullptr;
    QToolButton *m_chipInstanced = nullptr;

    QListView *m_meshView = nullptr;
    QListView *m_textureView = nullptr;
    QListView *m_materialView = nullptr;
    PS1ExtractedAssetModel *m_meshModel = nullptr;
    PS1ExtractedAssetModel *m_textureModel = nullptr;
    PS1ExtractedAssetModel *m_materialModel = nullptr;
    PS1ExtractedAssetFilter *m_meshFilter = nullptr;
    PS1ExtractedAssetFilter *m_textureFilter = nullptr;
    PS1ExtractedAssetFilter *m_materialFilter = nullptr;
};

#endif // PS1EXTRACTEDASSETBROWSER_H
