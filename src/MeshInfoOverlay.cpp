#include "MeshInfoOverlay.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "EditorViewport.h"
#include "OgreWidget.h"
#include "CLIPipeline.h"
#include "MemoryEstimator.h"
#include "mainwindow.h"

#include <QEvent>
#include <QLocale>
#include <QSet>

MeshInfoOverlay::MeshInfoOverlay(QMainWindow* parent)
    : QObject(parent)
    , mMainWindow(parent)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &MeshInfoOverlay::refresh);
    connect(Manager::getSingleton(), &Manager::entityCreated,
            this, &MeshInfoOverlay::refresh);
    connect(Manager::getSingleton(), &Manager::sceneNodeDestroyed,
            this, &MeshInfoOverlay::refresh);

    // Track main window moves/resizes to reposition the floating overlay
    mMainWindow->installEventFilter(this);
}

MeshInfoOverlay::~MeshInfoOverlay()
{
    delete mLabel;
}

bool MeshInfoOverlay::eventFilter(QObject* obj, QEvent* event)
{
    auto type = event->type();

    // Hide the overlay when the active viewport is hidden or destroyed
    if (obj == mActiveWidget &&
        (type == QEvent::Hide || type == QEvent::Close || type == QEvent::Destroy)) {
        if (mLabel)
            mLabel->hide();
        if (type != QEvent::Hide)
            mActiveWidget = nullptr;
        return QObject::eventFilter(obj, event);
    }

    if (type == QEvent::Move || type == QEvent::Resize) {
        if (mVisible && mLabel && mActiveWidget)
            repositionLabel();
    }
    return QObject::eventFilter(obj, event);
}

void MeshInfoOverlay::setActiveWidget(OgreWidget* widget)
{
    if (mActiveWidget == widget)
        return;

    // Stop watching old widget for move/resize
    if (mActiveWidget)
        mActiveWidget->removeEventFilter(this);

    mActiveWidget = widget;

    // Watch new widget for move/resize (dock splitter drags, etc.)
    if (mActiveWidget)
        mActiveWidget->installEventFilter(this);

    refresh();
}

QList<Ogre::Entity*> MeshInfoOverlay::collectEntities(bool& isSelection) const
{
    QList<Ogre::Entity*> entities;
    isSelection = false;

    auto* sel = SelectionSet::getSingleton();
    if (!sel->isEmpty()) {
        entities = sel->getResolvedEntities();
        isSelection = true;
    } else {
        // Cannot use Manager::getEntities() — it static_casts all attached
        // objects to Entity* without type checking (crashes on Lights, etc.).
        for (Ogre::SceneNode* node : Manager::getSingleton()->getSceneNodes()) {
            if (!node) continue;
            for (int i = 0; i < static_cast<int>(node->numAttachedObjects()); ++i) {
                Ogre::MovableObject* obj = node->getAttachedObject(i);
                if (obj && obj->getMovableType() == "Entity")
                    entities.append(static_cast<Ogre::Entity*>(obj));
            }
        }
    }
    return entities;
}

QString MeshInfoOverlay::formatStats(const QList<Ogre::Entity*>& entities, bool isSelection)
{
    // Filter out null entries so counts and headers are accurate
    QList<Ogre::Entity*> valid;
    for (Ogre::Entity* e : entities) {
        if (e) valid.append(e);
    }

    if (valid.isEmpty())
        return QStringLiteral("No meshes");

    QLocale locale;
    unsigned int totalVerts = 0;
    unsigned int totalTris = 0;
    unsigned int totalSubmeshes = 0;
    unsigned short totalBones = 0;
    int totalAnims = 0;
    quint64 totalGpuBytes = 0;
    QSet<QString> seenMeshes;
    QSet<QString> materialSet;

    for (Ogre::Entity* entity : valid) {
        MeshInfo info = CLIPipeline::extractMeshInfo(entity, QString());
        totalVerts += info.vertices;
        totalTris += info.triangles;
        totalSubmeshes += info.submeshes;
        totalBones += info.boneCount;
        totalAnims += info.animations.size();
        for (const QString& mat : info.materials)
            materialSet.insert(mat);

        // De-duplicate GPU bytes by mesh name so we count each unique mesh
        // resident once, not per-instance.
        MeshMemoryEstimate mem = MemoryEstimator::estimateEntity(entity);
        if (!mem.name.isEmpty() && !seenMeshes.contains(mem.name)) {
            seenMeshes.insert(mem.name);
            totalGpuBytes += mem.totalBytes();
        }
    }

    QString header;
    if (valid.size() == 1) {
        const Ogre::MeshPtr& mesh = valid.first()->getMesh();
        if (mesh)
            header = QString::fromStdString(mesh->getName());
        else
            header = QStringLiteral("Unknown mesh");
    } else if (isSelection) {
        header = QString("Selected (%1 meshes)").arg(valid.size());
    } else {
        header = QString("Scene (%1 meshes)").arg(valid.size());
    }

    QString text = header + "\n\n";
    text += QString("Verts: %1  Tris: %2\n")
                .arg(locale.toString(totalVerts))
                .arg(locale.toString(totalTris));
    text += QString("Submeshes: %1  Materials: %2")
                .arg(totalSubmeshes)
                .arg(materialSet.size());

    if (totalBones > 0 || totalAnims > 0) {
        text += QString("\nBones: %1  Anims: %2")
                    .arg(totalBones)
                    .arg(totalAnims);
    }

    if (totalGpuBytes > 0) {
        text += QString("\nGPU: %1").arg(MemoryEstimator::formatBytes(totalGpuBytes));
    }

    return text;
}

// LCOV_EXCL_START — QLabel overlay requires parent OgreWidget + display
void MeshInfoOverlay::ensureLabel()
{
    if (mLabel)
        return;

    // Create as a top-level frameless transparent window.  This gives the
    // label its own compositor surface, completely independent of Ogre's
    // direct-to-native rendering in OgreWidget (WA_PaintOnScreen with
    // null paintEngine).  Child widgets of OgreWidget leave ghost text
    // because Qt cannot clear old pixels on a surface it doesn't own.
    mLabel = new QLabel(mMainWindow);
    mLabel->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint
                           | Qt::WindowTransparentForInput);
    mLabel->setAttribute(Qt::WA_TranslucentBackground);
    mLabel->setAttribute(Qt::WA_ShowWithoutActivating);
    mLabel->setStyleSheet(
        "background-color: rgba(0, 0, 0, 100);"
        "color: white;"
        "font-family: monospace;"
        "font-size: 11px;"
        "padding: 6px 10px;"
        "border-radius: 4px;"
    );
    mLabel->hide();
}

void MeshInfoOverlay::repositionLabel()
{
    if (!mLabel || !mActiveWidget)
        return;
    QPoint globalPos = mActiveWidget->mapToGlobal(QPoint(8, 8));
    mLabel->move(globalPos);
}

void MeshInfoOverlay::setVisible(bool visible)
{
    mVisible = visible;
    emit visibilityChanged(visible);
    if (visible) {
        // If no active widget yet, use the first viewport
        if (!mActiveWidget) {
            auto viewports = mMainWindow->findChildren<EditorViewport*>();
            if (!viewports.isEmpty())
                setActiveWidget(viewports.first()->getOgreWidget());
        }
        refresh();
    } else if (mLabel) {
        mLabel->hide();
    }
}

void MeshInfoOverlay::refresh()
{
    if (!mVisible || !mActiveWidget) {
        if (mLabel) mLabel->hide();
        return;
    }

    ensureLabel();

    bool isSelection = false;
    auto entities = collectEntities(isSelection);
    QString text = formatStats(entities, isSelection);

    mLabel->setText(text);
    mLabel->adjustSize();
    repositionLabel();
    mLabel->show();
}
// LCOV_EXCL_STOP
