#include "PS1ExtractedAssetBrowser.h"

#include "SentryReporter.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMimeData>
#include <QPainter>
#include <QTabWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kThumbnailSize = 96;

QString mimeTypeForKindImpl(ExtractedAssetKind kind)
{
    switch (kind) {
    case ExtractedAssetKind::Mesh:
        return QStringLiteral("application/x-ps1rip-mesh");
    case ExtractedAssetKind::Texture:
        return QStringLiteral("application/x-ps1rip-texture");
    case ExtractedAssetKind::Material:
        return QStringLiteral("application/x-ps1rip-material");
    }
    return QStringLiteral("application/x-ps1rip-unknown");
}

QColor accentForMaterial(const QString &materialName)
{
    // Stable hue per material so the same mesh always shows the same
    // swatch across captures — handy for visual diffing when re-capturing
    // the same scene with different normalizer settings.
    uint h = 0;
    for (QChar ch : materialName)
        h = h * 31 + ch.unicode();
    const int hue = static_cast<int>(h % 360);
    return QColor::fromHsv(hue, 160, 200);
}

} // namespace

// --- Model ----------------------------------------------------------------

PS1ExtractedAssetModel::PS1ExtractedAssetModel(ExtractedAssetKind kind, QObject *parent)
    : QAbstractListModel(parent)
    , m_kind(kind)
{
}

void PS1ExtractedAssetModel::setTiles(QVector<ExtractedAssetTile> tiles)
{
    beginResetModel();
    m_tiles = std::move(tiles);
    endResetModel();
}

int PS1ExtractedAssetModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_tiles.size();
}

QVariant PS1ExtractedAssetModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tiles.size())
        return {};
    const ExtractedAssetTile &tile = m_tiles.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return tile.displayName;
    case Qt::DecorationRole:
        return tile.thumbnail.isNull()
                   ? QVariant{}
                   : QVariant(QPixmap::fromImage(tile.thumbnail));
    case Qt::ToolTipRole: {
        QString tt = tile.displayName;
        if (tile.triangleCount > 0)
            tt += QStringLiteral("\nTriangles: %1").arg(tile.triangleCount);
        if (tile.instanceCount > 0)
            tt += QStringLiteral("\nInstances: %1").arg(tile.instanceCount);
        if (tile.textured)
            tt += QStringLiteral("\nTextured");
        else if (tile.coloredOnly)
            tt += QStringLiteral("\nColored only");
        if (tile.instanced)
            tt += QStringLiteral("\nInstanced (drag to scene to add another copy)");
        return tt;
    }
    case Qt::UserRole:
        return tile.assetId;
    case Qt::UserRole + 1:
        return tile.meshIndex;
    default:
        return {};
    }
}

Qt::ItemFlags PS1ExtractedAssetModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QAbstractListModel::flags(index);
    if (index.isValid() && m_kind == ExtractedAssetKind::Mesh)
        f |= Qt::ItemIsDragEnabled;
    return f;
}

QStringList PS1ExtractedAssetModel::mimeTypes() const
{
    return {mimeTypeForKindImpl(m_kind)};
}

QMimeData *PS1ExtractedAssetModel::mimeData(const QModelIndexList &indexes) const
{
    if (indexes.isEmpty())
        return nullptr;
    auto *md = new QMimeData();
    QStringList ids;
    QStringList meshIndexes;
    for (const QModelIndex &idx : indexes) {
        if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_tiles.size())
            continue;
        const ExtractedAssetTile &tile = m_tiles.at(idx.row());
        ids << tile.assetId;
        meshIndexes << QString::number(tile.meshIndex);
    }
    md->setData(mimeTypeForKindImpl(m_kind), ids.join(QLatin1Char(';')).toUtf8());
    md->setData(QStringLiteral("application/x-ps1rip-meshindex"),
                meshIndexes.join(QLatin1Char(';')).toUtf8());
    // Also set a text/plain payload so non-Ogre drop targets (system
    // file managers, code editors) see a human-readable label rather
    // than an opaque blob.
    md->setText(ids.join(QLatin1Char(' ')));
    return md;
}

QString PS1ExtractedAssetModel::mimeTypeForKind(ExtractedAssetKind kind)
{
    return mimeTypeForKindImpl(kind);
}

// --- Filter ---------------------------------------------------------------

PS1ExtractedAssetFilter::PS1ExtractedAssetFilter(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setFilterCaseSensitivity(Qt::CaseInsensitive);
}

void PS1ExtractedAssetFilter::setShowTextured(bool show)
{
    if (m_showTextured == show)
        return;
    m_showTextured = show;
    invalidateFilter();
}

void PS1ExtractedAssetFilter::setShowColoredOnly(bool show)
{
    if (m_showColoredOnly == show)
        return;
    m_showColoredOnly = show;
    invalidateFilter();
}

void PS1ExtractedAssetFilter::setShowInstanced(bool show)
{
    if (m_showInstanced == show)
        return;
    m_showInstanced = show;
    invalidateFilter();
}

void PS1ExtractedAssetFilter::setSearchText(const QString &text)
{
    if (m_searchText == text)
        return;
    m_searchText = text;
    invalidateFilter();
}

bool PS1ExtractedAssetFilter::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    auto *src = qobject_cast<PS1ExtractedAssetModel *>(sourceModel());
    if (!src)
        return true;
    Q_UNUSED(sourceParent)
    if (sourceRow < 0 || sourceRow >= src->tiles().size())
        return false;
    const ExtractedAssetTile &tile = src->tiles().at(sourceRow);
    if (tile.textured && !m_showTextured)
        return false;
    if (tile.coloredOnly && !m_showColoredOnly)
        return false;
    if (tile.instanced && !m_showInstanced)
        return false;
    if (!m_searchText.isEmpty() && !tile.displayName.contains(m_searchText, Qt::CaseInsensitive))
        return false;
    return true;
}

// --- Browser --------------------------------------------------------------

PS1ExtractedAssetBrowser::PS1ExtractedAssetBrowser(PS1CapturedAssets *store, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    m_countsLabel = new QLabel(this);
    root->addWidget(m_countsLabel);

    auto *chipRow = new QHBoxLayout();
    chipRow->setSpacing(4);
    auto makeChip = [this](const QString &text) {
        auto *btn = new QToolButton(this);
        btn->setText(text);
        btn->setCheckable(true);
        btn->setChecked(true);
        return btn;
    };
    m_chipTextured = makeChip(tr("Textured"));
    m_chipColored = makeChip(tr("Colored only"));
    m_chipInstanced = makeChip(tr("Instanced"));
    chipRow->addWidget(m_chipTextured);
    chipRow->addWidget(m_chipColored);
    chipRow->addWidget(m_chipInstanced);
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search asset name…"));
    m_searchEdit->setClearButtonEnabled(true);
    chipRow->addWidget(m_searchEdit, /*stretch*/ 1);
    root->addLayout(chipRow);

    auto makeListView = [this]() {
        auto *view = new QListView(this);
        view->setViewMode(QListView::IconMode);
        view->setIconSize(QSize(kThumbnailSize, kThumbnailSize));
        view->setResizeMode(QListView::Adjust);
        view->setSpacing(8);
        view->setMovement(QListView::Static);
        view->setUniformItemSizes(true);
        view->setSelectionMode(QAbstractItemView::SingleSelection);
        view->setDragEnabled(true);
        view->setDragDropMode(QAbstractItemView::DragOnly);
        return view;
    };

    m_meshView = makeListView();
    m_textureView = makeListView();
    m_materialView = makeListView();

    m_meshModel = new PS1ExtractedAssetModel(ExtractedAssetKind::Mesh, this);
    m_textureModel = new PS1ExtractedAssetModel(ExtractedAssetKind::Texture, this);
    m_materialModel = new PS1ExtractedAssetModel(ExtractedAssetKind::Material, this);
    m_meshFilter = new PS1ExtractedAssetFilter(this);
    m_textureFilter = new PS1ExtractedAssetFilter(this);
    m_materialFilter = new PS1ExtractedAssetFilter(this);
    m_meshFilter->setSourceModel(m_meshModel);
    m_textureFilter->setSourceModel(m_textureModel);
    m_materialFilter->setSourceModel(m_materialModel);
    m_meshView->setModel(m_meshFilter);
    m_textureView->setModel(m_textureFilter);
    m_materialView->setModel(m_materialFilter);

    connect(m_meshView, &QListView::doubleClicked, this,
            &PS1ExtractedAssetBrowser::onMeshActivated);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(m_meshView, tr("Meshes"));
    m_tabs->addTab(m_textureView, tr("Textures"));
    m_tabs->addTab(m_materialView, tr("Materials"));
    root->addWidget(m_tabs, /*stretch*/ 1);

    auto applySearch = [this](const QString &t) {
        m_meshFilter->setSearchText(t);
        m_textureFilter->setSearchText(t);
        m_materialFilter->setSearchText(t);
    };
    connect(m_searchEdit, &QLineEdit::textChanged, this, applySearch);
    connect(m_chipTextured, &QToolButton::toggled, this, [this](bool on) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("ps1_rip_browser_chip_textured=%1")
                                          .arg(on ? 1 : 0));
        m_meshFilter->setShowTextured(on);
        m_textureFilter->setShowTextured(on);
        m_materialFilter->setShowTextured(on);
    });
    connect(m_chipColored, &QToolButton::toggled, this, [this](bool on) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("ps1_rip_browser_chip_colored=%1")
                                          .arg(on ? 1 : 0));
        m_meshFilter->setShowColoredOnly(on);
        m_textureFilter->setShowColoredOnly(on);
        m_materialFilter->setShowColoredOnly(on);
    });
    connect(m_chipInstanced, &QToolButton::toggled, this, [this](bool on) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("ps1_rip_browser_chip_instanced=%1")
                                          .arg(on ? 1 : 0));
        m_meshFilter->setShowInstanced(on);
        m_textureFilter->setShowInstanced(on);
        m_materialFilter->setShowInstanced(on);
    });

    if (m_store)
        connect(m_store, &PS1CapturedAssets::captureSetChanged, this,
                &PS1ExtractedAssetBrowser::onCaptureSetChanged);
    rebuildModels();
}

void PS1ExtractedAssetBrowser::refreshHeader()
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

void PS1ExtractedAssetBrowser::onCaptureSetChanged()
{
    rebuildModels();
    refreshHeader();
}

void PS1ExtractedAssetBrowser::onMeshActivated(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    const QModelIndex src = m_meshFilter->mapToSource(index);
    if (src.row() < 0 || src.row() >= m_meshModel->tiles().size())
        return;
    const ExtractedAssetTile &tile = m_meshModel->tiles().at(src.row());
    SentryReporter::addBreadcrumb(
        QStringLiteral("ps1.rip.inspector.promote"),
        QStringLiteral("source=browser-doubleclick asset=%1 mesh=%2")
            .arg(tile.assetId)
            .arg(tile.meshIndex));
    emit instantiateMesh(tile.meshIndex, tile.assetId);
}

QVector<ExtractedAssetTile>
PS1ExtractedAssetBrowser::buildTilesForKind(const CapturedAssetSet &set, ExtractedAssetKind kind)
{
    QVector<ExtractedAssetTile> tiles;
    switch (kind) {
    case ExtractedAssetKind::Mesh: {
        QHash<int, int> instancesPerMesh;
        for (const ReconstructedInstance &inst : set.instances)
            instancesPerMesh[inst.uniqueMeshIndex]++;
        for (int i = 0; i < set.uniqueMeshes.size(); ++i) {
            const ReconstructedMesh &mesh = set.uniqueMeshes[i];
            ExtractedAssetTile tile;
            tile.kind = kind;
            tile.meshIndex = i;
            tile.assetId = mesh.meshName + QLatin1Char('_') + set.captureId;
            tile.triangleCount = mesh.triangleCount;
            tile.instanceCount = instancesPerMesh.value(i, 0);
            tile.instanced = tile.instanceCount > 1;
            bool anyTextured = false;
            QString firstMaterial;
            for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
                if (firstMaterial.isEmpty())
                    firstMaterial = sub.materialName;
                if (sub.materialName.startsWith(QStringLiteral("PS1Rip_tpage_")))
                    anyTextured = true;
            }
            tile.textured = anyTextured;
            tile.coloredOnly = !anyTextured;
            tile.displayName = tr("Mesh %1 · %2 tris · %3 inst")
                                   .arg(i)
                                   .arg(mesh.triangleCount)
                                   .arg(tile.instanceCount);
            tile.thumbnail = placeholderThumbnail(QString::number(i),
                                                  accentForMaterial(firstMaterial));
            tiles.push_back(tile);
        }
        break;
    }
    case ExtractedAssetKind::Texture: {
        for (auto it = set.textureImages.constBegin(); it != set.textureImages.constEnd(); ++it) {
            ExtractedAssetTile tile;
            tile.kind = kind;
            tile.assetId = it.key();
            tile.displayName = it.key();
            tile.textured = true;
            tile.thumbnail = it.value().scaled(kThumbnailSize, kThumbnailSize,
                                               Qt::KeepAspectRatio, Qt::SmoothTransformation);
            tiles.push_back(tile);
        }
        break;
    }
    case ExtractedAssetKind::Material: {
        // Materials are the union of texture-bearing material names and
        // the synthetic "PS1Rip_color" name used for solid-color prims.
        QSet<QString> seen;
        for (const ReconstructedMesh &mesh : set.uniqueMeshes) {
            for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
                if (seen.contains(sub.materialName))
                    continue;
                seen.insert(sub.materialName);
                ExtractedAssetTile tile;
                tile.kind = kind;
                tile.assetId = sub.materialName;
                tile.displayName = sub.materialName;
                tile.textured = sub.materialName.startsWith(QStringLiteral("PS1Rip_tpage_"));
                tile.coloredOnly = !tile.textured;
                if (tile.textured && set.textureImages.contains(sub.materialName)) {
                    tile.thumbnail = set.textureImages.value(sub.materialName).scaled(
                        kThumbnailSize, kThumbnailSize, Qt::KeepAspectRatio,
                        Qt::SmoothTransformation);
                } else {
                    tile.thumbnail = placeholderThumbnail(
                        tile.textured ? QStringLiteral("T") : QStringLiteral("◆"),
                        accentForMaterial(sub.materialName));
                }
                tiles.push_back(tile);
            }
        }
        break;
    }
    }
    return tiles;
}

QImage PS1ExtractedAssetBrowser::placeholderThumbnail(const QString &caption, const QColor &accent)
{
    QImage img(kThumbnailSize, kThumbnailSize, QImage::Format_ARGB32);
    img.fill(QColor(40, 40, 40));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    QLinearGradient grad(0, 0, 0, kThumbnailSize);
    grad.setColorAt(0.0, accent);
    grad.setColorAt(1.0, accent.darker(180));
    p.setBrush(grad);
    p.setPen(QPen(accent.lighter(150), 2));
    p.drawRoundedRect(QRect(6, 6, kThumbnailSize - 12, kThumbnailSize - 12), 8, 8);
    p.setPen(Qt::white);
    QFont f = p.font();
    f.setPointSize(18);
    f.setBold(true);
    p.setFont(f);
    p.drawText(img.rect(), Qt::AlignCenter, caption);
    return img;
}

void PS1ExtractedAssetBrowser::rebuildModels()
{
    if (!m_store) {
        m_meshModel->setTiles({});
        m_textureModel->setTiles({});
        m_materialModel->setTiles({});
        return;
    }
    const CapturedAssetSet &set = m_store->captureSet();
    m_meshModel->setTiles(buildTilesForKind(set, ExtractedAssetKind::Mesh));
    m_textureModel->setTiles(buildTilesForKind(set, ExtractedAssetKind::Texture));
    m_materialModel->setTiles(buildTilesForKind(set, ExtractedAssetKind::Material));
}
