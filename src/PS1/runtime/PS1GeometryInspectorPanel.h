#ifndef PS1GEOMETRYINSPECTORPANEL_H
#define PS1GEOMETRYINSPECTORPANEL_H

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QWidget>

#include "PS1CapturedAssets.h"

class QLabel;
class QLineEdit;
class QToolButton;
class QTableView;

/**
 * Qt table model exposing `PS1CapturedAssets` rows to the inspector
 * (#426). Columns match the issue spec:
 *
 *   `#`, `frame`, `type` (tri/quad/sprite), `verts`, `tpage`, `clut`,
 *   `matrix#`, `material`, `triangles`.
 *
 * The model holds no draw-call data of its own — it forwards reads
 * to `PS1CapturedAssets::captureSet().rows` so a row change in the
 * store (e.g. `setRowHidden`) only needs a single `dataChanged()`
 * signal rather than a model reset. Resets only fire on
 * `captureSetChanged()`.
 */
class PS1GeometryInspectorModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        ColRow = 0,
        ColFrame,
        ColType,
        ColVerts,
        ColTpage,
        ColClut,
        ColMatrix,
        ColMaterial,
        ColTriangles,
        ColProvenance,
        ColCount
    };

    explicit PS1GeometryInspectorModel(PS1CapturedAssets *store, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    /** Convenience for views that want the underlying row data
     *  for a given table row (used by the context menu, drag-drop
     *  source, etc.). Returns an invalid row when the row is out
     *  of range so callers can guard with `rowIndex > 0`. */
    CapturedAssetRow rowAt(int row) const;

    static QString primKindLabel(PrimKind kind);
    /** "tracked" / "depth" / "screen" — the #816 reconstruction-tier chip. */
    static QString provenanceLabel(CapturedAssetProvenance provenance);

private slots:
    void onCaptureSetChanged();
    void onRowChanged(int rowIndex);

private:
    PS1CapturedAssets *m_store = nullptr;
};

/**
 * Filter proxy that drives the inspector's filter chips
 * (Textured / Colored-only / Visible-only) plus a free-text
 * search across the Material column (#426). Filter state is held
 * on the proxy so the view can call `invalidateFilter()` when
 * the user toggles a chip without rebuilding the source model.
 */
class PS1GeometryInspectorFilter : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit PS1GeometryInspectorFilter(QObject *parent = nullptr);

    void setShowTextured(bool show);
    void setShowColored(bool show);
    void setShowInstanced(bool show);
    void setHideDiscarded(bool hide);
    void setSearchText(const QString &text);

    bool showTextured() const { return m_showTextured; }
    bool showColored() const { return m_showColored; }
    bool showInstanced() const { return m_showInstanced; }
    bool hideDiscarded() const { return m_hideDiscarded; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    bool m_showTextured = true;
    bool m_showColored = true;
    bool m_showInstanced = true;
    bool m_hideDiscarded = false;
    QString m_searchText;
};

/**
 * Geometry Inspector dock body (#426). Holds the table + filter chips
 * + counts header. Sits inside `PS1RipSessionWindow` as a `QDockWidget`.
 *
 * Click semantics:
 * - Single click → emits `highlightRow(rowIndex)` so the session
 *   window can drive the editor `SelectionSet` to the matching
 *   SubEntity.
 * - Right click → context menu with Hide/Show submesh, Promote to
 *   permanent entity, Discard. Each action emits a typed signal so
 *   the session window owns the actual scene mutation.
 *
 * The panel is dumb about scene state: it only reads from
 * `PS1CapturedAssets` and emits intent signals. This keeps the
 * Qt UI testable in isolation from Ogre.
 */
class PS1GeometryInspectorPanel : public QWidget
{
    Q_OBJECT

public:
    explicit PS1GeometryInspectorPanel(PS1CapturedAssets *store, QWidget *parent = nullptr);

    /** Force a refresh of the counts header. Called from the session
     *  window after it observes `captureSetChanged()` via the store. */
    void refreshHeader();

    /** Test/UI accessors. */
    QTableView *tableView() const { return m_tableView; }
    PS1GeometryInspectorFilter *filter() const { return m_filter; }
    PS1GeometryInspectorModel *model() const { return m_model; }

signals:
    /** Fires on row activation. `rowIndex` is 1-based (matches the
     *  `#` column). */
    void highlightRow(int rowIndex);
    /** Right-click menu actions. */
    void hideSubmeshRequested(int rowIndex);
    void showSubmeshRequested(int rowIndex);
    void promoteRowRequested(int rowIndex);
    void discardRowRequested(int rowIndex);

private slots:
    void onActivated(const QModelIndex &index);
    void onCustomMenu(const QPoint &pos);
    void onCaptureSetChanged();

private:
    /** Maps the proxy row at `viewPoint` to the underlying
     *  CapturedAssetRow's 1-based index. Returns 0 when no row
     *  is under the point. */
    int rowIndexAtView(const QPoint &viewPoint) const;

    PS1CapturedAssets *m_store = nullptr;
    PS1GeometryInspectorModel *m_model = nullptr;
    PS1GeometryInspectorFilter *m_filter = nullptr;
    QTableView *m_tableView = nullptr;
    QLabel *m_countsLabel = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QToolButton *m_chipTextured = nullptr;
    QToolButton *m_chipColored = nullptr;
    QToolButton *m_chipInstanced = nullptr;
    QToolButton *m_chipVisible = nullptr;
};

#endif // PS1GEOMETRYINSPECTORPANEL_H
