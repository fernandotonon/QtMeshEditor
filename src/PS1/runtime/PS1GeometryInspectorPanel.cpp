#include "PS1GeometryInspectorPanel.h"

#include "SentryReporter.h"

#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

PS1GeometryInspectorModel::PS1GeometryInspectorModel(PS1CapturedAssets *store, QObject *parent)
    : QAbstractTableModel(parent)
    , m_store(store)
{
    if (m_store) {
        connect(m_store, &PS1CapturedAssets::captureSetChanged, this,
                &PS1GeometryInspectorModel::onCaptureSetChanged);
        connect(m_store, &PS1CapturedAssets::rowChanged, this,
                &PS1GeometryInspectorModel::onRowChanged);
    }
}

int PS1GeometryInspectorModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_store)
        return 0;
    return m_store->captureSet().rows.size();
}

int PS1GeometryInspectorModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return ColCount;
}

CapturedAssetRow PS1GeometryInspectorModel::rowAt(int row) const
{
    if (!m_store)
        return {};
    const auto &rows = m_store->captureSet().rows;
    if (row < 0 || row >= rows.size())
        return {};
    return rows.at(row);
}

QString PS1GeometryInspectorModel::primKindLabel(PrimKind kind)
{
    switch (kind) {
    case PrimKind::MonoTri:
        return QStringLiteral("tri");
    case PrimKind::ShadedTri:
        return QStringLiteral("tri/shaded");
    case PrimKind::TexturedTri:
        return QStringLiteral("tri/tex");
    case PrimKind::MonoQuad:
        return QStringLiteral("quad");
    case PrimKind::ShadedQuad:
        return QStringLiteral("quad/shaded");
    case PrimKind::TexturedQuad:
        return QStringLiteral("quad/tex");
    case PrimKind::Sprite:
        return QStringLiteral("sprite");
    }
    return QStringLiteral("?");
}

QString PS1GeometryInspectorModel::provenanceLabel(CapturedAssetProvenance provenance)
{
    switch (provenance) {
    case CapturedAssetProvenance::Tracked:
        return QStringLiteral("tracked");
    case CapturedAssetProvenance::Depth:
        return QStringLiteral("depth");
    case CapturedAssetProvenance::Screen:
        return QStringLiteral("screen");
    }
    return QStringLiteral("?");
}

QVariant PS1GeometryInspectorModel::data(const QModelIndex &index, int role) const
{
    if (!m_store || !index.isValid())
        return {};
    const auto &rows = m_store->captureSet().rows;
    if (index.row() < 0 || index.row() >= rows.size())
        return {};
    const CapturedAssetRow &row = rows.at(index.row());

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColRow:
            return row.rowIndex;
        case ColFrame:
            return static_cast<qulonglong>(row.frameIndex);
        case ColType:
            return primKindLabel(row.kind);
        case ColVerts:
            return row.vertexCount;
        case ColTpage:
            return QStringLiteral("0x%1").arg(row.tpage, 4, 16, QLatin1Char('0'));
        case ColClut:
            return QStringLiteral("0x%1").arg(row.clut, 4, 16, QLatin1Char('0'));
        case ColMatrix:
            return row.matrixId == 0 ? QStringLiteral("—")
                                     : QStringLiteral("%1").arg(row.matrixId);
        case ColMaterial:
            return row.materialName;
        case ColTriangles:
            return row.triangleCount;
        case ColProvenance:
            return provenanceLabel(row.provenance);
        default:
            return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        if (row.discarded)
            return QColor(Qt::gray);
        if (row.hidden)
            return QColor(120, 120, 120);
    }
    if (role == Qt::ToolTipRole) {
        QString tt = tr("Row #%1 · %2 · %3 verts")
                         .arg(row.rowIndex)
                         .arg(primKindLabel(row.kind))
                         .arg(row.vertexCount);
        if (row.textured)
            tt += tr("\nTextured · tpage=0x%1 clut=0x%2")
                      .arg(row.tpage, 4, 16, QLatin1Char('0'))
                      .arg(row.clut, 4, 16, QLatin1Char('0'));
        if (row.uniqueMeshIndex >= 0)
            tt += tr("\nUnique mesh #%1, instance #%2")
                      .arg(row.uniqueMeshIndex)
                      .arg(row.instanceIndex);
        if (row.hidden)
            tt += tr("\n(hidden in viewport)");
        if (row.discarded)
            tt += tr("\n(discarded)");
        return tt;
    }
    return {};
}

QVariant PS1GeometryInspectorModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return QAbstractTableModel::headerData(section, orientation, role);
    switch (section) {
    case ColRow:
        return tr("#");
    case ColFrame:
        return tr("frame");
    case ColType:
        return tr("type");
    case ColVerts:
        return tr("verts");
    case ColTpage:
        return tr("tpage");
    case ColClut:
        return tr("clut");
    case ColMatrix:
        return tr("matrix#");
    case ColMaterial:
        return tr("material");
    case ColTriangles:
        return tr("triangles");
    case ColProvenance:
        return tr("src");
    default:
        return {};
    }
}

void PS1GeometryInspectorModel::onCaptureSetChanged()
{
    beginResetModel();
    endResetModel();
}

void PS1GeometryInspectorModel::onRowChanged(int rowIndex)
{
    if (rowIndex < 1)
        return;
    const int row = rowIndex - 1;
    emit dataChanged(index(row, 0), index(row, ColCount - 1));
}

// --- Filter proxy ----------------------------------------------------------

PS1GeometryInspectorFilter::PS1GeometryInspectorFilter(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setFilterCaseSensitivity(Qt::CaseInsensitive);
}

void PS1GeometryInspectorFilter::setShowTextured(bool show)
{
    if (m_showTextured == show)
        return;
    m_showTextured = show;
    invalidateFilter();
}

void PS1GeometryInspectorFilter::setShowColored(bool show)
{
    if (m_showColored == show)
        return;
    m_showColored = show;
    invalidateFilter();
}

void PS1GeometryInspectorFilter::setShowInstanced(bool show)
{
    if (m_showInstanced == show)
        return;
    m_showInstanced = show;
    invalidateFilter();
}

void PS1GeometryInspectorFilter::setHideDiscarded(bool hide)
{
    if (m_hideDiscarded == hide)
        return;
    m_hideDiscarded = hide;
    invalidateFilter();
}

void PS1GeometryInspectorFilter::setSearchText(const QString &text)
{
    if (m_searchText == text)
        return;
    m_searchText = text;
    invalidateFilter();
}

bool PS1GeometryInspectorFilter::filterAcceptsRow(int sourceRow,
                                                  const QModelIndex &sourceParent) const
{
    auto *src = qobject_cast<PS1GeometryInspectorModel *>(sourceModel());
    if (!src)
        return true;
    Q_UNUSED(sourceParent)
    const CapturedAssetRow row = src->rowAt(sourceRow);
    if (row.rowIndex == 0)
        return false;
    if (row.discarded && m_hideDiscarded)
        return false;
    if (row.textured && !m_showTextured)
        return false;
    if (!row.textured && row.colored && !m_showColored)
        return false;
    if (row.instanceIndex >= 0 && !m_showInstanced) {
        // "Instanced" chip is a positive filter: when it's off, hide every
        // prim that resolved to a non-zero matrix instance, regardless of
        // textured / colored state. The earlier `!row.textured && !row.colored`
        // qualifier neutered the chip because virtually every PS1 prim is
        // either textured or colored (#679 review feedback).
        return false;
    }
    if (!m_searchText.isEmpty()) {
        if (!row.materialName.contains(m_searchText, Qt::CaseInsensitive))
            return false;
    }
    return true;
}

// --- Panel ----------------------------------------------------------------

PS1GeometryInspectorPanel::PS1GeometryInspectorPanel(PS1CapturedAssets *store, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
{
    m_model = new PS1GeometryInspectorModel(store, this);
    m_filter = new PS1GeometryInspectorFilter(this);
    m_filter->setSourceModel(m_model);
    m_filter->setFilterKeyColumn(PS1GeometryInspectorModel::ColMaterial);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    m_countsLabel = new QLabel(this);
    m_countsLabel->setWordWrap(false);
    root->addWidget(m_countsLabel);

    auto *chipRow = new QHBoxLayout();
    chipRow->setSpacing(4);
    auto makeChip = [this](const QString &text, bool defaultOn) {
        auto *btn = new QToolButton(this);
        btn->setText(text);
        btn->setCheckable(true);
        btn->setChecked(defaultOn);
        btn->setAutoRaise(false);
        return btn;
    };
    m_chipTextured = makeChip(tr("Textured"), true);
    m_chipColored = makeChip(tr("Colored"), true);
    m_chipInstanced = makeChip(tr("Instanced"), true);
    m_chipVisible = makeChip(tr("Hide discarded"), false);
    chipRow->addWidget(m_chipTextured);
    chipRow->addWidget(m_chipColored);
    chipRow->addWidget(m_chipInstanced);
    chipRow->addWidget(m_chipVisible);
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search material…"));
    m_searchEdit->setClearButtonEnabled(true);
    chipRow->addWidget(m_searchEdit, /*stretch*/ 1);
    root->addLayout(chipRow);

    m_tableView = new QTableView(this);
    m_tableView->setModel(m_filter);
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tableView->setSortingEnabled(true);
    m_tableView->verticalHeader()->setVisible(false);
    m_tableView->horizontalHeader()->setStretchLastSection(true);
    m_tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tableView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tableView->setAlternatingRowColors(true);
    root->addWidget(m_tableView, /*stretch*/ 1);

    connect(m_tableView, &QTableView::activated, this, &PS1GeometryInspectorPanel::onActivated);
    connect(m_tableView, &QTableView::clicked, this, &PS1GeometryInspectorPanel::onActivated);
    connect(m_tableView, &QTableView::customContextMenuRequested, this,
            &PS1GeometryInspectorPanel::onCustomMenu);
    connect(m_chipTextured, &QToolButton::toggled, this, [this](bool on) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("ps1_rip_inspector_chip_textured=%1")
                                          .arg(on ? 1 : 0));
        m_filter->setShowTextured(on);
    });
    connect(m_chipColored, &QToolButton::toggled, this, [this](bool on) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("ps1_rip_inspector_chip_colored=%1")
                                          .arg(on ? 1 : 0));
        m_filter->setShowColored(on);
    });
    connect(m_chipInstanced, &QToolButton::toggled, this, [this](bool on) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("ps1_rip_inspector_chip_instanced=%1")
                                          .arg(on ? 1 : 0));
        m_filter->setShowInstanced(on);
    });
    connect(m_chipVisible, &QToolButton::toggled, this, [this](bool on) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("ps1_rip_inspector_chip_hide_discarded=%1")
                                          .arg(on ? 1 : 0));
        m_filter->setHideDiscarded(on);
    });
    connect(m_searchEdit, &QLineEdit::textChanged, this,
            [this](const QString &t) { m_filter->setSearchText(t); });

    if (m_store)
        connect(m_store, &PS1CapturedAssets::captureSetChanged, this,
                &PS1GeometryInspectorPanel::onCaptureSetChanged);

    refreshHeader();
}

void PS1GeometryInspectorPanel::refreshHeader()
{
    if (!m_countsLabel)
        return;
    if (!m_store || !m_store->hasCapture()) {
        m_countsLabel->setText(tr("No capture yet — Capture Frame or Capture Scene to populate."));
        return;
    }
    const CapturedAssetSet &set = m_store->captureSet();
    m_countsLabel->setText(
        tr("Captured: %1 prims · %2 unique meshes · %3 textures")
            .arg(QLocale().toString(set.totalPrims))
            .arg(QLocale().toString(set.uniqueMeshes.size()))
            .arg(QLocale().toString(set.uniqueTextureIds.size())));
}

void PS1GeometryInspectorPanel::onCaptureSetChanged()
{
    refreshHeader();
}

void PS1GeometryInspectorPanel::onActivated(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    const QModelIndex src = m_filter->mapToSource(index);
    const CapturedAssetRow row = m_model->rowAt(src.row());
    if (row.rowIndex == 0)
        return;
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("ps1_rip_inspector_highlight=%1").arg(row.rowIndex));
    emit highlightRow(row.rowIndex);
}

int PS1GeometryInspectorPanel::rowIndexAtView(const QPoint &viewPoint) const
{
    if (!m_tableView)
        return 0;
    const QModelIndex idx = m_tableView->indexAt(viewPoint);
    if (!idx.isValid())
        return 0;
    const QModelIndex src = m_filter->mapToSource(idx);
    const CapturedAssetRow row = m_model->rowAt(src.row());
    return row.rowIndex;
}

void PS1GeometryInspectorPanel::onCustomMenu(const QPoint &pos)
{
    const int rowIndex = rowIndexAtView(pos);
    if (rowIndex == 0)
        return;
    const CapturedAssetRow row = m_store->captureSet().rows.at(rowIndex - 1);

    QMenu menu(this);
    auto *toggleVis = menu.addAction(row.hidden ? tr("Show this submesh")
                                                : tr("Hide this submesh"));
    auto *promote = menu.addAction(tr("Promote to permanent entity"));
    auto *discard = menu.addAction(row.discarded ? tr("Restore (un-discard)")
                                                 : tr("Discard"));

    QAction *chosen = menu.exec(m_tableView->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == toggleVis) {
        if (row.hidden) {
            SentryReporter::addBreadcrumb(
                QStringLiteral("ui.action"),
                QStringLiteral("ps1_rip_inspector_show=%1").arg(rowIndex));
            emit showSubmeshRequested(rowIndex);
        } else {
            SentryReporter::addBreadcrumb(
                QStringLiteral("ui.action"),
                QStringLiteral("ps1_rip_inspector_hide=%1").arg(rowIndex));
            emit hideSubmeshRequested(rowIndex);
        }
    } else if (chosen == promote) {
        SentryReporter::addBreadcrumb(
            QStringLiteral("ps1.rip.inspector.promote"),
            QStringLiteral("row=%1 mesh=%2 material=%3")
                .arg(rowIndex)
                .arg(row.uniqueMeshIndex)
                .arg(row.materialName));
        emit promoteRowRequested(rowIndex);
    } else if (chosen == discard) {
        SentryReporter::addBreadcrumb(
            QStringLiteral("ui.action"),
            QStringLiteral("ps1_rip_inspector_discard=%1 set=%2")
                .arg(rowIndex)
                .arg(row.discarded ? 0 : 1));
        emit discardRowRequested(rowIndex);
    }
}
