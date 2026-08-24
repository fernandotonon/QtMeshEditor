#include "TexturePaintController.h"

#include "AppSettingsKeys.h"
#include "EditModeController.h"
#include "GamificationManager.h"
#include "EditableMesh.h"
#include "Manager.h"
#include "OgreWidget.h"
#include "PaintBufferImageProvider.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "SpaceCamera.h"
#include "UndoManager.h"
#include "VertexColorBaker.h"
#include "EmbeddedTextureCache.h"
#include "PropertiesPanelController.h"
#include "RTShaderHelper.h"
#include "NormalMapGenerator.h"
#include "TextureChannelPacker.h"
#include "MeshImporterExporter.h"
#include "ProjectionPainter.h"
#include "MultiViewTextureBaker.h"
#include "MeshDepthRenderer.h"
#include "PaintLayerStack.h"
#include "TransformOperator.h"

#include <OgreCamera.h>
#include <OgreEntity.h>
#include <OgreSceneNode.h>

#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QPainter>
#include <QPen>
#include <QLibraryInfo>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickWindow>
#include <QSettings>
#include <QSet>
#include <QTimer>
#include <QByteArray>
#include <QColorDialog>
#include <QDir>
#include <QDateTime>
#include <QStandardPaths>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QRandomGenerator>
#include <QUndoCommand>
#include <QUrl>
#include <QVariantMap>
#include <QWidget>
#include <QMessageBox>
#include <QtMath>

#include <OgreCamera.h>
#include <OgreEntity.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreImage.h>
#include <OgreManualObject.h>
#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgrePixelFormat.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubEntity.h>
#include <OgreTechnique.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <cstring>
#include <memory>
#include <set>

TexturePaintController* TexturePaintController::s_instance = nullptr;

namespace {

/// Undo command for one texture-paint stroke on a single layer.
class TexturePaintStrokeCommand : public QUndoCommand
{
public:
    TexturePaintStrokeCommand(TexturePaintController* controller,
                              int layerIndex,
                              std::vector<uint8_t> before,
                              std::vector<uint8_t> after,
                              int width,
                              int height,
                              std::string entityName,
                              int channel)
        : QUndoCommand(QStringLiteral("Texture paint"))
        , m_controller(controller)
        , m_layerIndex(layerIndex)
        , m_before(std::move(before))
        , m_after(std::move(after))
        , m_width(width)
        , m_height(height)
        , m_entityName(std::move(entityName))
        , m_channel(channel)
    {
    }

    void undo() override { apply(m_before); }
    void redo() override
    {
        if (m_skipFirstRedo) {
            m_skipFirstRedo = false;
            return;
        }
        apply(m_after);
    }

private:
    void apply(const std::vector<uint8_t>& pixels)
    {
        if (!m_controller) return;
        // Activate the (entity, channel) this stroke belongs to — undo works
        // even after the user switched channels or deselected the mesh.
        if (!m_controller->ensureUndoTarget(m_entityName, m_channel)) return;
        m_controller->applyLayerPixelSnapshot(m_layerIndex, pixels);
    }

    TexturePaintController* m_controller = nullptr;
    int m_layerIndex = 0;
    std::vector<uint8_t> m_before;
    std::vector<uint8_t> m_after;
    int m_width = 0;
    int m_height = 0;
    std::string m_entityName;
    int m_channel = 0;
    bool m_skipFirstRedo = true;
};

/// Undo command for structural layer-stack changes (#546).
class PaintLayerOpCommand : public QUndoCommand
{
public:
    PaintLayerOpCommand(TexturePaintController* controller,
                        QString label,
                        std::string entityName,
                        int channel,
                        PaintLayerStack::Snapshot before,
                        PaintLayerStack::Snapshot after)
        : QUndoCommand(std::move(label))
        , m_controller(controller)
        , m_entityName(std::move(entityName))
        , m_channel(channel)
        , m_before(std::move(before))
        , m_after(std::move(after))
    {}

    void undo() override { apply(m_before); }
    void redo() override
    {
        if (m_skipFirstRedo) { m_skipFirstRedo = false; return; }
        apply(m_after);
    }

private:
    void apply(const PaintLayerStack::Snapshot& snap)
    {
        if (!m_controller) return;
        if (!m_controller->ensureUndoTarget(m_entityName, m_channel)) return;
        m_controller->applyLayerStackSnapshot(snap);
    }

    TexturePaintController* m_controller = nullptr;
    std::string m_entityName;
    int m_channel = 0;
    PaintLayerStack::Snapshot m_before;
    PaintLayerStack::Snapshot m_after;
    bool m_skipFirstRedo = true;
};

/// Undo command for a one-shot selection-mask action (fill FG / fill BG
/// / delete). Same pre/post-snapshot model as TexturePaintStrokeCommand
/// — actions like "Delete selected" can affect thousands of pixels but
/// happen atomically, so storing a full pixel snapshot is the simplest
/// correct approach (matches Photoshop's "History snapshot").
class TexturePaintMaskActionCommand : public QUndoCommand
{
public:
    TexturePaintMaskActionCommand(TexturePaintController* controller,
                                  int layerIndex,
                                  std::vector<uint8_t> before,
                                  std::vector<uint8_t> after,
                                  int width,
                                  int height,
                                  std::string entityName,
                                  int channel,
                                  QString label)
        : QUndoCommand(label)
        , m_controller(controller)
        , m_layerIndex(layerIndex)
        , m_before(std::move(before))
        , m_after(std::move(after))
        , m_width(width)
        , m_height(height)
        , m_entityName(std::move(entityName))
        , m_channel(channel)
    {}

    void undo() override { apply(m_before); }
    void redo() override
    {
        if (m_skipFirstRedo) { m_skipFirstRedo = false; return; }
        apply(m_after);
    }

private:
    void apply(const std::vector<uint8_t>& pixels)
    {
        if (!m_controller) return;
        if (!m_controller->ensureUndoTarget(m_entityName, m_channel)) return;
        m_controller->applyLayerPixelSnapshot(m_layerIndex, pixels);
    }

    TexturePaintController* m_controller = nullptr;
    int m_layerIndex = 0;
    std::vector<uint8_t> m_before;
    std::vector<uint8_t> m_after;
    int m_width = 0;
    int m_height = 0;
    std::string m_entityName;
    int m_channel = 0;
    bool m_skipFirstRedo = true;
};

Ogre::TexturePtr findTextureAcrossGroups(const std::string& name)
{
    auto texPtr = Ogre::TextureManager::getSingleton().getByName(name);
    if (texPtr)
        return texPtr;
    auto it = Ogre::TextureManager::getSingleton().getResourceIterator();
    while (it.hasMoreElements()) {
        const Ogre::ResourcePtr r = it.getNext();
        if (r && r->getName() == name)
            return Ogre::static_pointer_cast<Ogre::Texture>(r);
    }
    return {};
}

bool copyQImageToPaintBuffer(TexturePaintBuffer& buffer, const QImage& source)
{
    QImage qimg = source;
    if (qimg.isNull())
        return false;
    if (qimg.format() != QImage::Format_RGBA8888)
        qimg = qimg.convertToFormat(QImage::Format_RGBA8888);
    const int w = qimg.width();
    const int h = qimg.height();
    if (w <= 0 || h <= 0)
        return false;
    buffer.resize(w, h);
    for (int y = 0; y < h; ++y) {
        std::memcpy(buffer.data().data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u,
                    qimg.constScanLine(y),
                    static_cast<size_t>(w) * 4u);
    }
    buffer.clearDirty();
    return true;
}

bool loadPaintBufferFromImageBytes(TexturePaintBuffer& buffer,
                                   const uint8_t* data,
                                   std::size_t size)
{
    QImage qimg;
    if (!qimg.loadFromData(data, static_cast<int>(size)))
        return false;
    return copyQImageToPaintBuffer(buffer, qimg);
}

bool loadPaintBufferFromDiskPath(TexturePaintBuffer& buffer, const QString& path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;
    return copyQImageToPaintBuffer(buffer, QImage(path));
}

bool isPlainUncompressedFormat(Ogre::PixelFormat fmt)
{
    return fmt == Ogre::PF_BYTE_RGBA
        || fmt == Ogre::PF_BYTE_RGB
        || fmt == Ogre::PF_BYTE_BGRA
        || fmt == Ogre::PF_BYTE_BGR
        || fmt == Ogre::PF_A8R8G8B8
        || fmt == Ogre::PF_R8G8B8A8
        || fmt == Ogre::PF_A8B8G8R8
        || fmt == Ogre::PF_X8R8G8B8
        || fmt == Ogre::PF_R8G8B8;
}

/// True when we can blit dirty rects straight into the model's bound GPU
/// texture without rebinding materials.
bool canWriteOriginalTextureInPlace(const Ogre::TexturePtr& tex,
                                    int bufferW,
                                    int bufferH)
{
    if (!tex || bufferW <= 0 || bufferH <= 0)
        return false;
    try {
        if (static_cast<int>(tex->getWidth()) != bufferW
            || static_cast<int>(tex->getHeight()) != bufferH)
            return false;
        if (!isPlainUncompressedFormat(tex->getFormat()))
            return false;
        if (!tex->getBuffer())
            return false;
    } catch (...) {
        return false;
    }
    return true;
}

/// Pixel payload copied on a worker thread, consumed on the main thread.
namespace {

void copyBufferRect(const TexturePaintBuffer& src, TexturePaintBuffer& dst,
                    const TexturePaintBuffer::DirtyRect& rect)
{
    const int W = src.width();
    if (W <= 0 || rect.empty()) return;
    auto& dstData = dst.data();
    const auto& srcData = src.data();
    for (int row = rect.y0; row < rect.y1; ++row) {
        const size_t off = (static_cast<size_t>(row) * static_cast<size_t>(W)
                            + static_cast<size_t>(rect.x0)) * 4u;
        const size_t bytes = static_cast<size_t>(rect.width()) * 4u;
        std::memcpy(dstData.data() + off, srcData.data() + off, bytes);
    }
}

} // namespace

struct GpuUploadPacket {
    std::vector<uint8_t> rgba;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    int bufferW = 0;
    int bufferH = 0;
    bool partial = false;
};

constexpr int kPaintUploadTilePx = 64;
constexpr int kStrokeDirectUploadMaxPx = 512 * 512;
constexpr int kStrokeTilesPerTick = 8;

// CPU-side sources first — same order as MaterialEditorQML::previewUrlFromOgreTexture.
// GPU readback (convertToImage / blitToMemory) is unreliable for imported FBX textures.
bool loadPaintBufferFromNonGpuSources(TexturePaintBuffer& buffer,
                                      const Ogre::TexturePtr& texPtr,
                                      const QString& texName)
{
    if (texName.isEmpty())
        return false;

    if (texPtr) {
        const QString origin = QString::fromStdString(texPtr->getOrigin());
        if (!origin.isEmpty() && loadPaintBufferFromDiskPath(buffer, origin))
            return true;

        const QString group = QString::fromStdString(texPtr->getGroup());
        if (!group.isEmpty()) {
            if (loadPaintBufferFromDiskPath(buffer, group + QLatin1Char('/') + texName))
                return true;
            if (!origin.isEmpty()
                && loadPaintBufferFromDiskPath(buffer, group + QLatin1Char('/') + origin)) {
                return true;
            }
        }
    }

    const std::vector<uint8_t> bytes =
        EmbeddedTextureCache::retrieve(texName.toStdString());
    if (!bytes.empty()
        && loadPaintBufferFromImageBytes(buffer, bytes.data(), bytes.size())) {
        return true;
    }

    const QString baseName = QFileInfo(texName).fileName();
    if (baseName != texName) {
        const std::vector<uint8_t> baseBytes =
            EmbeddedTextureCache::retrieve(baseName.toStdString());
        if (!baseBytes.empty()
            && loadPaintBufferFromImageBytes(buffer, baseBytes.data(), baseBytes.size())) {
            return true;
        }
    }

    return loadPaintBufferFromDiskPath(buffer, texName);
}

} // namespace

TexturePaintController* TexturePaintController::instance()
{
    if (!s_instance)
        s_instance = new TexturePaintController();
    return s_instance;
}

TexturePaintController* TexturePaintController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    auto* p = instance();
    QQmlEngine::setObjectOwnership(p, QQmlEngine::CppOwnership);
    return p;
}

void TexturePaintController::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

TexturePaintController::TexturePaintController(QObject* parent)
    : QObject(parent)
{
    // The paintChannels() picker model depends on both the active channel and
    // the live layer stack (its hasLayers badges). Re-notify the QML property
    // whenever either changes — done via signal-to-signal connections so every
    // existing layersChanged()/activeChannelChanged() emit site is covered
    // without threading an extra emit through all of them (#547 review).
    connect(this, &TexturePaintController::layersChanged,
            this, &TexturePaintController::paintChannelsChanged);
    connect(this, &TexturePaintController::activeChannelChanged,
            this, &TexturePaintController::paintChannelsChanged);

    // Mirror the toolbar brush settings — texture paint and vertex paint
    // share one source of truth so the user isn't juggling two sets of
    // controls. EditModeController owns the canonical values; we just
    // forward its change signal so the QML "Texture Paint" panel can
    // re-render the live brush preview.
    if (auto* em = EditModeController::instance()) {
        connect(em, &EditModeController::vertexPaintChanged,
                this, &TexturePaintController::texturePaintChanged);
        // FG/BG changes should refresh the FG/BG quick-ramp preview.
        connect(em, &EditModeController::vertexPaintChanged,
                this, [this]() {
                    // Only FG/BG ramps depend on the live FG/BG colours.
                    if (m_useFgBgRamp)
                        reloadActiveRamp();
                });
    }

    // Restore Paint v2 Slice A preferences.
    {
        QSettings s;
        // Solid is the product default. One-time migration resets installs
        // that picked up gradient during feature testing.
        if (!s.contains(AppSettingsKeys::paintColorSourceSolidDefaultApplied())) {
            m_colorSource = ColorSolid;
            s.setValue(AppSettingsKeys::paintColorSource(), static_cast<int>(ColorSolid));
            s.setValue(AppSettingsKeys::paintColorSourceSolidDefaultApplied(), true);
        } else {
            m_colorSource = static_cast<ColorSource>(
                s.value(AppSettingsKeys::paintColorSource(), static_cast<int>(ColorSolid)).toInt());
        }
        m_gradientMode = static_cast<GradientMode>(
            s.value(AppSettingsKeys::paintGradientMode(), static_cast<int>(GradientLinear)).toInt());
        m_activeRampName = s.value(AppSettingsKeys::paintGradientRampName(),
                                   QStringLiteral("Sunset")).toString();
        if (m_colorSource != ColorSolid && m_colorSource != ColorGradient)
            m_colorSource = ColorSolid;
        if (m_gradientMode < GradientLinear || m_gradientMode > GradientAngular)
            m_gradientMode = GradientLinear;
        reloadActiveRamp();
    }

    // Restore Paint v2 Slice E (#548) symmetry axis sub-preferences. Symmetry
    // itself is ALWAYS OFF at startup — it's a transient painting mode, not a
    // sticky preference, so it never persists across sessions (the enabled flag
    // is deliberately not restored). Local/World and the stabilizer no longer
    // have UI; they keep sane defaults (local space, stabilizer off).
    {
        QSettings s;
        m_symmetryEnabled = false;
        m_symmetrySpace = SymLocal;
        m_symmetryAxes = s.value(AppSettingsKeys::paintSymmetryAxes(),
                                 static_cast<int>(SymAxisX)).toInt()
                         & (SymAxisX | SymAxisY | SymAxisZ);
        if (m_symmetryAxes == SymAxisNone) {
            m_symmetryAxes = SymAxisX;   // persist the default so it round-trips
            s.setValue(AppSettingsKeys::paintSymmetryAxes(), m_symmetryAxes);
        }
        m_topologyMirror = s.value(AppSettingsKeys::paintTopologyMirror(), true).toBool();
        m_stabilizerMode = StabAverage;
        m_stabilizerAmount = 0.0;
    }

    {
        QSettings s;
        {
            auto t = static_cast<BrushFootprint::FootprintType>(
                s.value(AppSettingsKeys::paintFootprintType(), 0).toInt());
            if (t < BrushFootprint::FootprintType::Round
                || t > BrushFootprint::FootprintType::TilingSource)
                t = BrushFootprint::FootprintType::Round;
            m_footprintType = t;
        }
        m_activeStampName = s.value(AppSettingsKeys::paintActiveStampName(),
                                    QStringLiteral("Soft Circle")).toString();
        m_activeTilingName = s.value(AppSettingsKeys::paintActiveTilingName(),
                                     QStringLiteral("Wood")).toString();
        m_stampSettings.spacing = static_cast<float>(std::clamp(
            s.value(AppSettingsKeys::paintStampSpacing(), 0.35).toDouble(), 0.05, 2.0));
        m_stampSettings.scatter = static_cast<float>(std::clamp(
            s.value(AppSettingsKeys::paintStampScatter(), 0.0).toDouble(), 0.0, 1.0));
        m_stampSettings.sizeJitter = static_cast<float>(std::clamp(
            s.value(AppSettingsKeys::paintStampSizeJitter(), 0.0).toDouble(), 0.0, 1.0));
        m_stampSettings.opacityJitter = static_cast<float>(std::clamp(
            s.value(AppSettingsKeys::paintStampOpacityJitter(), 0.0).toDouble(), 0.0, 1.0));
        {
            auto r = static_cast<BrushFootprint::StampRotation>(
                s.value(AppSettingsKeys::paintStampRotation(), 0).toInt());
            if (r < BrushFootprint::StampRotation::None
                || r > BrushFootprint::StampRotation::RandomJitter)
                r = BrushFootprint::StampRotation::None;
            m_stampSettings.rotation = r;
        }
        m_stampSettings.fixedAngleDeg = static_cast<float>(
            s.value(AppSettingsKeys::paintStampFixedAngle(), 0.0).toDouble());
        m_tilingSettings.scale = static_cast<float>(std::max(
            s.value(AppSettingsKeys::paintTilingScale(), 1.0).toDouble(), 0.01));
        m_tilingSettings.rotationDeg = static_cast<float>(std::clamp(
            s.value(AppSettingsKeys::paintTilingRotation(), 0.0).toDouble(), 0.0, 360.0));
        m_tilingSettings.offsetU = static_cast<float>(std::clamp(
            s.value(AppSettingsKeys::paintTilingOffsetU(), 0.0).toDouble(), -1.0, 1.0));
        m_tilingSettings.offsetV = static_cast<float>(std::clamp(
            s.value(AppSettingsKeys::paintTilingOffsetV(), 0.0).toDouble(), -1.0, 1.0));
        reloadStampImage();
        reloadTilingImage();
        refreshStampPreviewUris();
    }

    // Refresh the texture slot list whenever the selection changes —
    // the user expects "select a mesh → see its textures" without any
    // explicit refresh action.
    if (auto* sel = SelectionSet::getSingleton()) {
        connect(sel, &SelectionSet::selectionChanged,
                this, &TexturePaintController::refreshSlots);
    }

    // Listen for scene-node destruction so we can drop dangling paint
    // session references before the source Entity goes away. Without
    // this, deleting a mesh while it had an active paint session or
    // a wand selection mask would crash on the next frame — the
    // mask-overlay clone holds a MeshPtr keyed off the dying entity,
    // and m_paintMeshEntity / m_sessionEntity remain pointed at
    // freed memory. Manager emits the signal BEFORE actually
    // destroying the node so the entities are still valid here.
    if (auto* mgr = Manager::getSingletonPtr()) {
        connect(mgr, &Manager::sceneNodeDestroyed, this,
            [this](Ogre::SceneNode* node) {
                if (!node) return;
                // If any attached object on the doomed node is the
                // session entity (or the overlay clone), tear down
                // the session now while the entity is still alive.
                bool touches = false;
                try {
                    const auto& objs = node->getAttachedObjects();
                    for (auto* o : objs) {
                        if (!o) continue;
                        if (o == static_cast<Ogre::MovableObject*>(m_paintMeshEntity)
                         || o == static_cast<Ogre::MovableObject*>(m_sessionEntity)
                         || o == static_cast<Ogre::MovableObject*>(m_maskOverlayEntity)) {
                            touches = true;
                            break;
                        }
                    }
                } catch (...) { touches = true; }
                if (touches) {
                    SentryReporter::addBreadcrumb("ui.action",
                        "Paint: scene node holding session entity destroyed — closing session");
                    try { closeSession(); } catch (...) {}
                }
            });
        connect(mgr, &Manager::sceneClearing, this, [this]() {
            // Hard scene reset — every entity is about to go away, so
            // unconditionally tear down the paint session.
            try { closeSession(); } catch (...) {}
        });
    }
}

TexturePaintController::~TexturePaintController()
{
    // Drop the manual objects on the scene before Ogre destructors
    // race us on shutdown. Skip the cleanup entirely if Ogre's
    // singletons are already gone — touching them post-teardown
    // segfaults at process exit.
    if (Ogre::Root::getSingletonPtr() == nullptr) {
        m_paintMesh.reset();
        m_ogreTexture.reset();
        m_originalTexture.reset();
        m_ringNode = nullptr;
        m_ringObj = nullptr;
        m_paintMeshEntity = nullptr;
        m_sessionEntity = nullptr;
        return;
    }
    try {
        closeSession();
    } catch (...) {
        // Best-effort shutdown.
    }
}

void TexturePaintController::setTexturePaintEnabled(bool enabled)
{
    if (m_paintEnabled == enabled) return;
    m_paintEnabled = enabled;
    if (enabled) {
        // Texture paint needs a private EditableMesh built from the
        // selected entity. NB: we deliberately don't call
        // EditModeController::enterEditMode() — that would flip the
        // workspace mode to EditMode, kick the user out of Material
        // Mode, and (via the visibility hooks) silently disable us.
        if (auto* e = activeEntity()) {
            ensureEditableMesh(e);
            // Pre-create the paint session for texture target so the
            // preview thumbnail populates immediately and the first
            // stroke doesn't have to do the heavy setup work.
            // (Skip for vertex target — no texture session needed.)
            if (m_target == TargetTexture && !hasActiveSession())
                ensurePaintableTexture(1024);
        }
        refreshSlots();
    }
    if (!enabled) {
        if (m_strokeActive) endStroke();
        // Disabling paint also tears down any active session so the
        // model's original textures snap back into the render.
        if (hasActiveSession()) closeSession();
    }
    SentryReporter::addBreadcrumb("ui.action",
        enabled ? "Texture paint: enabled" : "Texture paint: disabled");
    emit texturePaintChanged();
}

void TexturePaintController::setBrushTool(int tool)
{
    BrushTool t = static_cast<BrushTool>(tool);
    if (t == m_tool) return;
    m_tool = t;
    m_smudgeHavePrev = false;
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint: tool = %1").arg(tool));
    emit brushToolChanged();
}

// ---------------------------------------------------------------------------
// Paint v2 Slice E (#548) — symmetry + stabilizer setters
// ---------------------------------------------------------------------------

void TexturePaintController::setSymmetryEnabled(bool on)
{
    if (on == m_symmetryEnabled) return;
    m_symmetryEnabled = on;
    // Enabling with no axes set defaults to local X (the modelling convention).
    if (on && m_symmetryAxes == SymAxisNone) m_symmetryAxes = SymAxisX;
    // Deliberately NOT persisted — symmetry always starts OFF each session.
    SentryReporter::addBreadcrumb("paint.symmetry",
        QStringLiteral("enabled=%1 space=%2 axes=%3 topo=%4")
            .arg(m_symmetryEnabled).arg(static_cast<int>(m_symmetrySpace))
            .arg(m_symmetryAxes).arg(m_topologyMirror));
    invalidateSymmetryMaps();
    refreshSymmetryPlaneOverlay();
    emit symmetryChanged();
}

void TexturePaintController::setSymmetrySpace(int space)
{
    const SymmetrySpace s = (space == static_cast<int>(SymWorld)) ? SymWorld : SymLocal;
    if (s == m_symmetrySpace) return;
    m_symmetrySpace = s;
    QSettings().setValue(AppSettingsKeys::paintSymmetrySpace(), static_cast<int>(m_symmetrySpace));
    SentryReporter::addBreadcrumb("paint.symmetry",
        QStringLiteral("space=%1").arg(static_cast<int>(m_symmetrySpace)));
    invalidateSymmetryMaps();
    refreshSymmetryPlaneOverlay();
    emit symmetryChanged();
}

void TexturePaintController::setSymmetryAxes(int axes)
{
    axes &= (SymAxisX | SymAxisY | SymAxisZ);
    if (axes == m_symmetryAxes) return;
    m_symmetryAxes = axes;
    QSettings().setValue(AppSettingsKeys::paintSymmetryAxes(), m_symmetryAxes);
    SentryReporter::addBreadcrumb("paint.symmetry",
        QStringLiteral("axes=%1").arg(m_symmetryAxes));
    invalidateSymmetryMaps();   // per-axis maps must be rebuilt for the new set
    refreshSymmetryPlaneOverlay();
    emit symmetryChanged();
}

void TexturePaintController::setTopologyMirror(bool on)
{
    if (on == m_topologyMirror) return;
    m_topologyMirror = on;
    QSettings().setValue(AppSettingsKeys::paintTopologyMirror(), m_topologyMirror);
    SentryReporter::addBreadcrumb("paint.symmetry",
        QStringLiteral("topology=%1").arg(m_topologyMirror));
    invalidateSymmetryMaps();
    emit symmetryChanged();
}

void TexturePaintController::setStabilizerMode(int mode)
{
    const StabilizerMode m = (mode == static_cast<int>(StabTrail)) ? StabTrail : StabAverage;
    if (m == m_stabilizerMode) return;
    m_stabilizerMode = m;
    QSettings().setValue(AppSettingsKeys::paintStabilizerMode(), static_cast<int>(m_stabilizerMode));
    SentryReporter::addBreadcrumb("paint.stabilizer",
        QStringLiteral("mode=%1").arg(static_cast<int>(m_stabilizerMode)));
    emit stabilizerChanged();
}

void TexturePaintController::setStabilizerAmount(double amount)
{
    amount = std::clamp(amount, 0.0, 100.0);
    if (std::abs(amount - m_stabilizerAmount) < 1e-6) return;
    m_stabilizerAmount = amount;
    QSettings().setValue(AppSettingsKeys::paintStabilizerAmount(), m_stabilizerAmount);
    SentryReporter::addBreadcrumb("paint.stabilizer",
        QStringLiteral("amount=%1").arg(m_stabilizerAmount, 0, 'f', 0));
    emit stabilizerChanged();
}

// ---------------------------------------------------------------------------
// Paint v2 Slice F (#549) — projection / stencil painting
// ---------------------------------------------------------------------------

void TexturePaintController::setProjectionMode(int mode)
{
    const int m = (mode < 0 || mode > 2) ? 0 : mode;
    if (m == m_projectionMode) return;
    m_projectionMode = m;
    if (m_projectionMode != 2) { m_cameraLocked = false; m_haveLockedView = false; }
    SentryReporter::addBreadcrumb("paint.projection",
        QStringLiteral("mode=%1").arg(m_projectionMode));
    emit projectionChanged();
}

void TexturePaintController::setStencilImagePath(const QString& path)
{
    if (path == m_stencilImagePath) return;
    m_stencilImagePath = path;
    m_stencilImage = path.isEmpty() ? QImage() : QImage(path);
    if (!m_stencilImage.isNull())
        m_stencilImage = m_stencilImage.convertToFormat(QImage::Format_RGBA8888);
    // File NAME only — a full path embeds the OS account name (/Users/<name>/…).
    SentryReporter::addBreadcrumb("paint.projection",
        QStringLiteral("stencil=%1 (%2)").arg(QFileInfo(path).fileName())
            .arg(m_stencilImage.isNull() ? "load-failed" : "ok"));
    emit projectionChanged();
}

void TexturePaintController::setProjBackfaceCull(bool on)
{
    if (on == m_projBackfaceCull) return;
    m_projBackfaceCull = on;
    SentryReporter::addBreadcrumb("paint.projection",
        QStringLiteral("backfaceCull=%1").arg(on));
    emit projectionChanged();
}

void TexturePaintController::setProjUseOcclusion(bool on)
{
    if (on == m_projUseOcclusion) return;
    m_projUseOcclusion = on;
    SentryReporter::addBreadcrumb("paint.projection",
        QStringLiteral("useOcclusion=%1").arg(on));
    emit projectionChanged();
}

void TexturePaintController::setProjDepthLimit(double v)
{
    v = std::clamp(v, 0.0, 2.0);
    if (std::abs(v - m_projDepthLimit) < 1e-6) return;
    m_projDepthLimit = v;
    SentryReporter::addBreadcrumb("paint.projection",
        QStringLiteral("depthLimit=%1").arg(v, 0, 'f', 3));
    emit projectionChanged();
}

void TexturePaintController::ensureProjTris()
{
    if (m_haveProjTris) return;
    auto* entity = activeEntity();
    if (!entity) return;
    m_projTris = MultiViewTextureBaker::fromEntity(entity, nullptr);
    m_haveProjTris = !m_projTris.empty();
}

bool TexturePaintController::liveCameraView(OgreWidget* widget,
                                           ProjectionPainter::View& out) const
{
    if (!widget || !widget->getSpaceCamera()) return false;
    Ogre::Camera* cam = widget->getSpaceCamera()->getCamera();
    if (!cam) return false;
    out.viewProj = cam->getProjectionMatrixWithRSDepth() * cam->getViewMatrix();
    out.camDirection = cam->getRealDirection();
    out.camPosition = cam->getRealPosition();
    return true;
}

bool TexturePaintController::currentProjectionView(OgreWidget* widget,
                                                   ProjectionPainter::View& out) const
{
    if (m_projectionMode == 2 && m_haveLockedView) { out = m_lockedView; return true; }
    return liveCameraView(widget, out);
}

bool TexturePaintController::buildOcclusionForView(const ProjectionPainter::View& view,
                                                   ProjectionPainter::OcclusionMap& occ) const
{
    auto* entity = activeEntity();
    if (!entity) return false;
    MeshDepthRenderer::View dv;
    dv.dir = view.camDirection;
    // Any up not parallel to dir; the renderer re-frames the camera itself.
    dv.up = (std::abs(view.camDirection.dotProduct(Ogre::Vector3::UNIT_Y)) > 0.95f)
                ? Ogre::Vector3(0, 0, 1) : Ogre::Vector3::UNIT_Y;
    dv.name = "projection";
    QString err;
    MeshDepthRenderer::RenderResult r =
        MeshDepthRenderer::renderDepthMapView(entity, 512, dv, &err);
    if (r.depth.isNull()) return false;
    occ.depth = r.depth;
    occ.viewProj = r.projMatrix * r.viewMatrix;
    occ.camPosition = r.camPosition;
    occ.camDirection = r.camDirection;
    occ.depthNear = r.depthNear;
    occ.depthFar = r.depthFar;
    // Bias must exceed the 8-bit fog quantisation (1/255 of the range).
    occ.biasWorld = std::max((r.depthFar - r.depthNear) / 255.0f * 2.0f,
                             (r.depthFar - r.depthNear) * 3e-3f);
    return true;
}

ProjectionPainter::Options TexturePaintController::projectionOptions() const
{
    ProjectionPainter::Options opts;
    opts.resolution = m_buffer.width() > 0 ? m_buffer.width() : 1024;
    opts.backfaceCull = m_projBackfaceCull;
    opts.useOcclusion = m_projUseOcclusion;
    // depthLimit is a fraction of the bounds radius → world units.
    if (m_projDepthLimit > 0.0 && m_haveProjOcc) {
        const float range = m_projOcc.depthFar - m_projOcc.depthNear;
        opts.depthLimit = static_cast<float>(m_projDepthLimit) * range * 0.5f;
    }
    return opts;
}

int TexturePaintController::commitProjectedLayer(const TexturePaintBuffer& projected,
                                                 const QString& name)
{
    if (!hasActiveSession()) return -1;
    const auto before = m_layerStack.snapshot();
    const int idx = m_layerStack.addFromBuffer(projected, name,
                                               PaintLayerStack::LayerType::Generated);
    if (idx < 0) return -1;
    m_layerStack.setActiveIndex(idx);
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(name, before, m_layerStack.snapshot());
    invalidateLayerStrokeBaseline();
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
    return idx;
}

void TexturePaintController::snapProjectionCamera()
{
    auto* widget = m_pendingStrokeWidget ? m_pendingStrokeWidget
                       : (TransformOperator::getSingletonPtr() ? TransformOperator::getSingleton()->getActiveWidget() : nullptr);
    ProjectionPainter::View v;
    // Deliberately NOT currentProjectionView(): in locked mode that returns the
    // already-stored pose, so re-snapping after moving the viewport camera would
    // copy the stale pose back onto itself and never re-pin.
    if (!liveCameraView(widget, v)) {
        // No live camera (e.g. mode 2 requested before any paint) — keep the
        // stored one; otherwise bail.
        if (!m_haveLockedView) return;
        v = m_lockedView;
    }
    m_lockedView = v;
    m_haveLockedView = true;
    m_cameraLocked = true;
    m_haveProjOcc = buildOcclusionForView(v, m_projOcc);
    SentryReporter::addBreadcrumb("paint.projection.snap",
        QStringLiteral("occlusion=%1").arg(m_haveProjOcc));
    emit projectionChanged();
}

bool TexturePaintController::projectFromPhoto(const QString& path)
{
    if (!hasActiveSession()) {
        if (auto* e = activeEntity()) ensurePaintableTexture(1024);
        if (!hasActiveSession()) return false;
    }
    QImage src(path);
    if (src.isNull()) return false;
    src = src.convertToFormat(QImage::Format_RGBA8888);

    m_projTris.clear(); m_haveProjTris = false; ensureProjTris();
    if (!m_haveProjTris) return false;

    ProjectionPainter::View v;
    auto* widget = m_pendingStrokeWidget ? m_pendingStrokeWidget
                       : (TransformOperator::getSingletonPtr() ? TransformOperator::getSingleton()->getActiveWidget() : nullptr);
    if (m_haveLockedView) v = m_lockedView;
    else if (!currentProjectionView(widget, v)) return false;

    m_haveProjOcc = buildOcclusionForView(v, m_projOcc);
    ProjectionPainter::Options opts = projectionOptions();
    opts.useOcclusion = m_haveProjOcc;   // photo always occludes when we have a map

    TexturePaintBuffer scratch;
    scratch.resize(opts.resolution, opts.resolution);
    const auto rep = ProjectionPainter::project(
        m_projTris, v, src, scratch, opts, m_haveProjOcc ? &m_projOcc : nullptr);
    if (!rep.ok || rep.texelsWritten == 0) return false;

    SentryReporter::addBreadcrumb("paint.projection.photo",
        QStringLiteral("texels=%1 occluded=%2").arg(rep.texelsWritten).arg(rep.texelsOccluded));
    return commitProjectedLayer(scratch, QStringLiteral("Projected photo")) >= 0;
}

void TexturePaintController::chooseStencilImage()
{
    QApplication::processEvents();
    const QString path = QFileDialog::getOpenFileName(
        QApplication::activeWindow(),
        QStringLiteral("Choose stencil image"),
        QDir::currentPath(),
        QStringLiteral("Image files (*.png *.jpg *.jpeg *.tga *.bmp);;All files (*)"),
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons);
    if (!path.isEmpty()) setStencilImagePath(path);
}

bool TexturePaintController::chooseAndProjectPhoto()
{
    QApplication::processEvents();
    const QString path = QFileDialog::getOpenFileName(
        QApplication::activeWindow(),
        QStringLiteral("Project photo onto mesh"),
        QDir::currentPath(),
        QStringLiteral("Image files (*.png *.jpg *.jpeg *.tga *.bmp);;All files (*)"),
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons);
    if (path.isEmpty()) return false;
    return projectFromPhoto(path);
}

// ---------------------------------------------------------------------------
// Paint v2 Slice F (#549) — decal tool
// ---------------------------------------------------------------------------

bool TexturePaintController::beginDecal(const QString& imagePath)
{
    if (!hasActiveSession()) {
        if (activeEntity()) ensurePaintableTexture(1024);
        if (!hasActiveSession()) return false;
    }
    QImage img(imagePath);
    if (img.isNull()) return false;
    m_decal.begin(img);
    m_haveDecalDragPos = false;
    setBrushTool(ToolDecal);
    // File NAME only — a full path embeds the OS account name.
    SentryReporter::addBreadcrumb("paint.decal.begin", QFileInfo(imagePath).fileName());
    refreshDecalOverlay();
    emit projectionChanged();
    return true;
}

bool TexturePaintController::beginDecalInteractive()
{
    QApplication::processEvents();
    const QString path = QFileDialog::getOpenFileName(
        QApplication::activeWindow(),
        QStringLiteral("Choose decal image"),
        QDir::currentPath(),
        QStringLiteral("Image files (*.png *.jpg *.jpeg *.tga *.bmp);;All files (*)"),
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons);
    if (path.isEmpty()) return false;
    return beginDecal(path);
}

bool TexturePaintController::decalSessionActive() const { return m_decal.active(); }
int  TexturePaintController::decalState() const { return static_cast<int>(m_decal.state()); }

void TexturePaintController::placeDecalAt(OgreWidget* widget, const QPoint& screenPos)
{
    if (m_decal.state() != DecalSession::State::Placing) return;
    Ogre::Vector3 localPos, localNormal;
    if (!hitTestLocalPoint(widget, screenPos, localPos, localNormal)) return;
    auto* entity = m_paintMeshEntity;
    auto* node = entity ? entity->getParentSceneNode() : nullptr;
    if (!node) return;
    const Ogre::Affine3 world = node->_getFullTransform();
    const Ogre::Vector3 worldPos = world * localPos;
    // Normals need the INVERSE TRANSPOSE, not the linear block: under non-uniform
    // scale the linear map does not preserve perpendicularity, which would tilt
    // the decal plane off the real surface and skew backface/occlusion on commit.
    // Matches MultiViewTextureBaker::fromEntity's normalMat.
    const Ogre::Matrix3 normalMat = world.linear().Inverse().Transpose();
    Ogre::Vector3 worldN = normalMat * localNormal;
    if (worldN.isZeroLength()) worldN = Ogre::Vector3::UNIT_Z;
    worldN.normalise();
    Ogre::Vector3 camUp = Ogre::Vector3::UNIT_Y;
    if (widget && widget->getSpaceCamera() && widget->getSpaceCamera()->getCamera())
        camUp = widget->getSpaceCamera()->getCamera()->getRealUp();
    // Initial half-size ~ 15% of the mesh bounds radius.
    float halfSize = 0.5f;
    if (m_paintMesh) halfSize = m_paintMesh->calculateBounds().getHalfSize().length() * 0.15f;
    m_decal.place(worldPos, worldN, camUp, std::max(halfSize, 1e-3f));
    SentryReporter::addBreadcrumb("paint.decal.place",
        QStringLiteral("half=%1").arg(halfSize, 0, 'f', 3));
    refreshDecalOverlay();
    emit projectionChanged();
}

bool TexturePaintController::decalPlaneHit(OgreWidget* widget, const QPoint& screenPos,
                                           Ogre::Vector3& outWorld) const
{
    if (!widget || !widget->getSpaceCamera() || !widget->getSpaceCamera()->getCamera())
        return false;
    Ogre::Camera* cam = widget->getSpaceCamera()->getCamera();
    int vw = 0, vh = 0; widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return false;
    const float nx = static_cast<float>(screenPos.x()) / vw;
    const float ny = static_cast<float>(screenPos.y()) / vh;
    const Ogre::Ray ray = cam->getCameraToViewportRay(nx, ny);
    const Ogre::Plane plane(m_decal.rect().normal, m_decal.rect().center);
    const auto hit = ray.intersects(plane);
    if (!hit.first) return false;
    outWorld = ray.getPoint(hit.second);
    return true;
}

int TexturePaintController::decalHitTest(OgreWidget* widget, const QPoint& screenPos)
{
    if (m_decal.state() != DecalSession::State::Editing) return static_cast<int>(DecalSession::Handle::None);
    Ogre::Vector3 world;
    if (!decalPlaneHit(widget, screenPos, world))
        return static_cast<int>(DecalSession::Handle::None);
    const Ogre::Vector2 uv = m_decal.worldToRectUv(world);
    const DecalSession::Handle h = m_decal.hitTest(uv.x, uv.y);
    m_decalDragLastPos = screenPos;
    m_haveDecalDragPos = (h != DecalSession::Handle::None);
    return static_cast<int>(h);
}

void TexturePaintController::dragDecal(OgreWidget* widget, const QPoint& screenPos, int handle)
{
    if (m_decal.state() != DecalSession::State::Editing) return;
    Ogre::Vector3 world;
    if (!decalPlaneHit(widget, screenPos, world)) return;
    Ogre::Vector3 prevWorld;
    const bool havePrev = m_haveDecalDragPos
        && decalPlaneHit(widget, m_decalDragLastPos, prevWorld);
    const DecalSession::Handle h = static_cast<DecalSession::Handle>(handle);
    const DecalSession::Rect& r = m_decal.rect();

    if (h == DecalSession::Handle::Body) {
        if (havePrev) m_decal.translate(world - prevWorld);
    } else if (h == DecalSession::Handle::RotateCorner) {
        if (havePrev) {
            const Ogre::Vector3 a = (prevWorld - r.center);
            const Ogre::Vector3 b = (world - r.center);
            if (!a.isZeroLength() && !b.isZeroLength()) {
                Ogre::Vector3 an = a; an.normalise();
                Ogre::Vector3 bn = b; bn.normalise();
                float ang = std::atan2(an.crossProduct(bn).dotProduct(r.normal),
                                       an.dotProduct(bn));
                m_decal.rotate(ang);
            }
        }
    } else if (h == DecalSession::Handle::ScaleEdge) {
        // Scale so the dragged edge follows the cursor (uniform-ish via UV).
        const Ogre::Vector2 uv = m_decal.worldToRectUv(world);
        const float su = std::abs(uv.x) > 0.2f ? std::abs(uv.x) : 1.0f;
        const float sv = std::abs(uv.y) > 0.2f ? std::abs(uv.y) : 1.0f;
        // Only scale the axis being dragged (whichever |uv| is larger).
        if (std::abs(uv.x) >= std::abs(uv.y)) m_decal.scale(su, 1.0f);
        else m_decal.scale(1.0f, sv);
    }
    m_decalDragLastPos = screenPos;
    m_haveDecalDragPos = true;
    refreshDecalOverlay();
}

bool TexturePaintController::commitDecal()
{
    if (m_decal.state() != DecalSession::State::Editing) { cancelDecal(); return false; }
    if (!hasActiveSession()) { cancelDecal(); return false; }
    m_projTris.clear(); m_haveProjTris = false; ensureProjTris();
    if (!m_haveProjTris) { cancelDecal(); return false; }

    const auto ci = m_decal.buildCommit(/*softEdge*/0.15f);
    // Occlusion for the decal projection (front arc only).
    ProjectionPainter::OcclusionMap occ;
    const bool haveOcc = buildOcclusionForView(ci.view, occ);
    ProjectionPainter::Options opts;
    opts.resolution = m_buffer.width() > 0 ? m_buffer.width() : 1024;
    opts.backfaceCull = true;
    opts.useOcclusion = haveOcc;

    TexturePaintBuffer scratch;
    scratch.resize(opts.resolution, opts.resolution);
    const auto rep = ProjectionPainter::project(
        m_projTris, ci.view, ci.source, scratch, opts, haveOcc ? &occ : nullptr);
    const bool ok = rep.ok && rep.texelsWritten > 0
        && commitProjectedLayer(scratch, QStringLiteral("Decal")) >= 0;
    SentryReporter::addBreadcrumb("paint.decal.commit",
        QStringLiteral("texels=%1 ok=%2").arg(rep.texelsWritten).arg(ok));
    cancelDecal();
    return ok;
}

void TexturePaintController::cancelDecal()
{
    m_decal.cancel();
    m_haveDecalDragPos = false;
    refreshDecalOverlay();
    emit projectionChanged();
}

void TexturePaintController::setColorSource(int source)
{
    const ColorSource s = (source == static_cast<int>(ColorGradient))
                              ? ColorGradient
                              : ColorSolid;
    if (s == m_colorSource) return;
    m_colorSource = s;
    QSettings().setValue(AppSettingsKeys::paintColorSource(), static_cast<int>(m_colorSource));
    if (m_colorSource == ColorGradient) {
        SentryReporter::addBreadcrumb(
            "paint.brush.gradient",
            QStringLiteral("mode=%1 ramp=%2")
                .arg(static_cast<int>(m_gradientMode))
                .arg(m_useFgBgRamp ? QStringLiteral("FG/BG") : m_activeRampName));
    }
    reloadActiveRamp();
    emit gradientChanged();
}

void TexturePaintController::setGradientMode(int mode)
{
    GradientMode m = GradientLinear;
    if (mode == static_cast<int>(GradientRadial))
        m = GradientRadial;
    else if (mode == static_cast<int>(GradientAngular))
        m = GradientAngular;
    if (m == m_gradientMode) return;
    m_gradientMode = m;
    QSettings().setValue(AppSettingsKeys::paintGradientMode(), static_cast<int>(m_gradientMode));
    SentryReporter::addBreadcrumb(
        "paint.brush.gradient",
        QStringLiteral("mode=%1 ramp=%2")
            .arg(static_cast<int>(m_gradientMode))
            .arg(m_useFgBgRamp ? QStringLiteral("FG/BG") : m_activeRampName));
    emit gradientChanged();
}

void TexturePaintController::setActiveRampName(const QString& name)
{
    if (name.isEmpty() || name == m_activeRampName) return;
    m_activeRampName = name;
    m_useFgBgRamp = false;
    QSettings().setValue(AppSettingsKeys::paintGradientRampName(), m_activeRampName);
    reloadActiveRamp();
    SentryReporter::addBreadcrumb(
        "paint.brush.gradient",
        QStringLiteral("mode=%1 ramp=%2")
            .arg(static_cast<int>(m_gradientMode))
            .arg(m_activeRampName));
    emit gradientChanged();
}

void TexturePaintController::setUseFgBgRamp(bool on)
{
    if (on == m_useFgBgRamp) return;
    m_useFgBgRamp = on;
    reloadActiveRamp();
    SentryReporter::addBreadcrumb(
        "paint.brush.gradient",
        QStringLiteral("fg/bg ramp=%1").arg(on ? QStringLiteral("on") : QStringLiteral("off")));
    emit gradientChanged();
}

void TexturePaintController::setGradientStepped(bool on)
{
    if (on == m_gradientStepped) return;
    m_gradientStepped = on;
    m_activeRamp.interpolate = on ? GradientRamp::Interpolate::Stepped
                                  : GradientRamp::Interpolate::Linear;
    refreshRampPreviewUri();
    SentryReporter::addBreadcrumb(
        "paint.brush.gradient",
        QStringLiteral("stepped=%1").arg(on ? QStringLiteral("on") : QStringLiteral("off")));
    emit gradientChanged();
}

void TexturePaintController::setRampJitter(double j)
{
    j = std::clamp(j, 0.0, 1.0);
    if (std::abs(j - m_rampJitter) < 1e-6) return;
    m_rampJitter = j;
    SentryReporter::addBreadcrumb(
        "paint.brush.gradient",
        QStringLiteral("jitter=%1").arg(j, 0, 'f', 2));
    emit gradientChanged();
}

QStringList TexturePaintController::rampNames() const
{
    QStringList names;
    for (const auto& r : GradientRamp::bundledPresets())
        names << QString::fromStdString(r.name);
    for (const auto& r : GradientRamp::loadCustomRamps()) {
        const QString n = QString::fromStdString(r.name);
        if (!names.contains(n))
            names << n;
    }
    return names;
}

QVariantList TexturePaintController::activeRampStops() const
{
    QVariantList out;
    for (const auto& s : m_activeRamp.stops) {
        QVariantMap m;
        m.insert(QStringLiteral("t"), static_cast<double>(s.position));
        m.insert(QStringLiteral("r"), static_cast<double>(s.colour.r));
        m.insert(QStringLiteral("g"), static_cast<double>(s.colour.g));
        m.insert(QStringLiteral("b"), static_cast<double>(s.colour.b));
        m.insert(QStringLiteral("a"), static_cast<double>(s.colour.a));
        out.push_back(m);
    }
    return out;
}

const GradientRamp::Ramp* TexturePaintController::resolveActiveRamp() const
{
    if (m_useFgBgRamp)
        return &m_activeRamp;
    if (m_activeRamp.isValid() && m_activeRamp.name == m_activeRampName.toStdString())
        return &m_activeRamp;
    if (const auto* bundled = GradientRamp::findBundled(m_activeRampName.toStdString()))
        return bundled;
    return m_activeRamp.isValid() ? &m_activeRamp : nullptr;
}

void TexturePaintController::reloadActiveRamp()
{
    if (m_useFgBgRamp) {
        const QColor fg = texturePaintColor();
        const QColor bg = bgPaintColor();
        m_activeRamp = GradientRamp::fromFgBg(
            {static_cast<float>(fg.redF()), static_cast<float>(fg.greenF()),
             static_cast<float>(fg.blueF()), static_cast<float>(fg.alphaF())},
            {static_cast<float>(bg.redF()), static_cast<float>(bg.greenF()),
             static_cast<float>(bg.blueF()), static_cast<float>(bg.alphaF())});
        m_activeRamp.interpolate = m_gradientStepped
                                       ? GradientRamp::Interpolate::Stepped
                                       : GradientRamp::Interpolate::Linear;
        refreshRampPreviewUri();
        return;
    }

    bool found = false;
    for (const auto& r : GradientRamp::loadCustomRamps()) {
        if (r.name == m_activeRampName.toStdString()) {
            m_activeRamp = r;
            found = true;
            break;
        }
    }
    if (!found) {
        if (const auto* bundled = GradientRamp::findBundled(m_activeRampName.toStdString())) {
            m_activeRamp = *bundled;
            found = true;
        }
    }
    if (!found) {
        // Fall back to Sunset so the brush always has a usable ramp.
        if (const auto* sunset = GradientRamp::findBundled("Sunset")) {
            m_activeRamp = *sunset;
            m_activeRampName = QStringLiteral("Sunset");
        }
    }
    m_gradientStepped =
        m_activeRamp.interpolate == GradientRamp::Interpolate::Stepped;
    refreshRampPreviewUri();
}

void TexturePaintController::refreshRampPreviewUri()
{
    constexpr int W = 256;
    constexpr int H = 24;
    QImage img(W, H, QImage::Format_RGBA8888);
    if (!m_activeRamp.isValid()) {
        img.fill(Qt::black);
    } else {
        for (int x = 0; x < W; ++x) {
            const float t = (W == 1) ? 0.0f : static_cast<float>(x) / static_cast<float>(W - 1);
            const auto c = m_activeRamp.sample(t);
            const QRgb px = qRgba(
                static_cast<int>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f),
                static_cast<int>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f),
                static_cast<int>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f),
                static_cast<int>(std::clamp(c.a, 0.0f, 1.0f) * 255.0f + 0.5f));
            for (int y = 0; y < H; ++y)
                img.setPixel(x, y, px);
        }
    }
    QByteArray ba;
    QBuffer buf(&ba);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    m_rampPreviewUri = QStringLiteral("data:image/png;base64,") + ba.toBase64();
}

void TexturePaintController::noteStrokeSample(const Ogre::Vector2& uv, bool isStart)
{
    if (isStart || !m_strokeHavePrevUV) {
        m_strokePrevUV = uv;
        m_strokeHavePrevUV = true;
        m_strokePathLength = 0.0f;
        m_strokeDirSmoothed = Ogre::Vector2::ZERO;
        return;
    }
    const Ogre::Vector2 delta = uv - m_strokePrevUV;
    const float dist = delta.length();
    if (dist > 1e-8f) {
        // EMA-smoothed direction keeps linear sampling stable when the
        // stroke turns — the path length still advances by the true
        // step distance so the ramp doesn't stutter.
        const Ogre::Vector2 dir = delta / dist;
        if (m_strokeDirSmoothed.squaredLength() < 1e-12f)
            m_strokeDirSmoothed = dir;
        else
            m_strokeDirSmoothed = (m_strokeDirSmoothed * 0.75f + dir * 0.25f).normalisedCopy();
        m_strokePathLength += dist;
    }
    m_strokePrevUV = uv;
}

bool TexturePaintController::saveCustomRamp(const QString& name, const QVariantList& stops,
                                            bool stepped)
{
    if (name.trimmed().isEmpty() || stops.size() < 2)
        return false;
    GradientRamp::Ramp ramp;
    ramp.name = name.trimmed().toStdString();
    ramp.interpolate = stepped ? GradientRamp::Interpolate::Stepped
                               : GradientRamp::Interpolate::Linear;
    for (const QVariant& v : stops) {
        const QVariantMap m = v.toMap();
        GradientRamp::Stop s;
        s.position = static_cast<float>(m.value(QStringLiteral("t")).toDouble());
        s.colour.r = static_cast<float>(m.value(QStringLiteral("r")).toDouble());
        s.colour.g = static_cast<float>(m.value(QStringLiteral("g")).toDouble());
        s.colour.b = static_cast<float>(m.value(QStringLiteral("b")).toDouble());
        s.colour.a = static_cast<float>(m.value(QStringLiteral("a"), 1.0).toDouble());
        s.position = std::clamp(s.position, 0.0f, 1.0f);
        ramp.stops.push_back(s);
    }
    std::sort(ramp.stops.begin(), ramp.stops.end(),
              [](const GradientRamp::Stop& a, const GradientRamp::Stop& b) {
                  return a.position < b.position;
              });
    if (!ramp.isValid())
        return false;
    const std::string path = GradientRamp::saveCustom(ramp);
    if (path.empty())
        return false;
    m_activeRampName = QString::fromStdString(ramp.name);
    m_useFgBgRamp = false;
    m_gradientStepped = stepped;
    m_activeRamp = std::move(ramp);
    QSettings().setValue(AppSettingsKeys::paintGradientRampName(), m_activeRampName);
    refreshRampPreviewUri();
    SentryReporter::addBreadcrumb("paint.brush.gradient",
        QStringLiteral("saved custom ramp=%1").arg(m_activeRampName));
    emit gradientChanged();
    return true;
}

bool TexturePaintController::deleteCustomRamp(const QString& name)
{
    if (GradientRamp::findBundled(name.toStdString()))
        return false;
    if (!GradientRamp::deleteCustom(name.toStdString()))
        return false;
    if (m_activeRampName == name)
        setActiveRampName(QStringLiteral("Sunset"));
    else
        emit gradientChanged();
    return true;
}

void TexturePaintController::setActiveRampStops(const QVariantList& stops, bool stepped)
{
    if (stops.size() < 2)
        return;
    GradientRamp::Ramp ramp;
    ramp.name = m_useFgBgRamp ? "FG/BG" : m_activeRampName.toStdString();
    ramp.interpolate = stepped ? GradientRamp::Interpolate::Stepped
                               : GradientRamp::Interpolate::Linear;
    for (const QVariant& v : stops) {
        const QVariantMap m = v.toMap();
        GradientRamp::Stop s;
        s.position = std::clamp(static_cast<float>(m.value(QStringLiteral("t")).toDouble()), 0.0f, 1.0f);
        s.colour.r = std::clamp(static_cast<float>(m.value(QStringLiteral("r")).toDouble()), 0.0f, 1.0f);
        s.colour.g = std::clamp(static_cast<float>(m.value(QStringLiteral("g")).toDouble()), 0.0f, 1.0f);
        s.colour.b = std::clamp(static_cast<float>(m.value(QStringLiteral("b")).toDouble()), 0.0f, 1.0f);
        s.colour.a = std::clamp(static_cast<float>(m.value(QStringLiteral("a"), 1.0).toDouble()), 0.0f, 1.0f);
        ramp.stops.push_back(s);
    }
    std::sort(ramp.stops.begin(), ramp.stops.end(),
              [](const GradientRamp::Stop& a, const GradientRamp::Stop& b) {
                  return a.position < b.position;
              });
    if (!ramp.isValid())
        return;
    m_activeRamp = std::move(ramp);
    m_gradientStepped = stepped;
    refreshRampPreviewUri();
    emit gradientChanged();
}

bool TexturePaintController::sampleRampFromTexture(double u0, double v0,
                                                   double u1, double v1,
                                                   int numStops)
{
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0)
        return false;
    numStops = std::clamp(numStops, 2, 16);
    GradientRamp::Ramp ramp;
    ramp.name = "Eyedropper";
    ramp.interpolate = GradientRamp::Interpolate::Linear;
    for (int i = 0; i < numStops; ++i) {
        const float t = (numStops == 1)
                            ? 0.0f
                            : static_cast<float>(i) / static_cast<float>(numStops - 1);
        const float u = static_cast<float>(u0 + (u1 - u0) * static_cast<double>(t));
        const float v = static_cast<float>(v0 + (v1 - v0) * static_cast<double>(t));
        int x = 0, y = 0;
        m_buffer.uvToPixel(Ogre::Vector2(u, v), x, y);
        const auto c = m_buffer.pixel(x, y);
        ramp.stops.push_back({t, {c.r, c.g, c.b, c.a}});
    }
    if (!ramp.isValid())
        return false;
    m_activeRamp = std::move(ramp);
    m_activeRampName = QStringLiteral("Eyedropper");
    m_useFgBgRamp = false;
    m_colorSource = ColorGradient;
    refreshRampPreviewUri();
    SentryReporter::addBreadcrumb("paint.brush.gradient",
        QStringLiteral("mode=%1 ramp=Eyedropper (sampled %2 stops)")
            .arg(static_cast<int>(m_gradientMode))
            .arg(numStops));
    emit gradientChanged();
    return true;
}

void TexturePaintController::openRampEditor()
{
    if (m_rampEditorWindow) {
        if (auto* w = qobject_cast<QQuickWindow*>(m_rampEditorWindow)) {
            w->show();
            w->raise();
            w->requestActivate();
            return;
        }
        m_rampEditorWindow = nullptr;
    }
    auto* engine = new QQmlApplicationEngine(this);
    const QString appDir = QCoreApplication::applicationDirPath();
    engine->addImportPath(appDir + "/qml");
    engine->addImportPath(QStringLiteral("qrc:/"));
    engine->addImportPath(QLibraryInfo::path(QLibraryInfo::QmlImportsPath));
    qmlRegisterSingletonType<TexturePaintController>(
        "PropertiesPanel", 1, 0, "TexturePaintController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* {
            return TexturePaintController::qmlInstance(e, nullptr);
        });
    qmlRegisterSingletonType<PropertiesPanelController>(
        "PropertiesPanel", 1, 0, "PropertiesPanelController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* {
            return PropertiesPanelController::qmlInstance(e, nullptr);
        });
    connect(engine, &QQmlApplicationEngine::warnings, this,
            [](const QList<QQmlError>& warnings) {
                for (const QQmlError& err : warnings) {
                    SentryReporter::addBreadcrumb(
                        "ui.action",
                        QStringLiteral("Gradient ramp editor QML: %1").arg(err.toString()));
                }
            });
    auto handled = std::make_shared<bool>(false);
    connect(engine, &QQmlApplicationEngine::objectCreated, this,
        [this, engine, handled](QObject* obj, const QUrl&) {
            *handled = true;
            if (!obj) {
                SentryReporter::addBreadcrumb("ui.action",
                    "Gradient ramp editor: QML load failed");
                engine->deleteLater();
                return;
            }
            m_rampEditorWindow = obj;
            if (auto* w = qobject_cast<QQuickWindow*>(obj)) {
                w->setModality(Qt::ApplicationModal);
                QWindow* parentWindow = nullptr;
                if (auto* aw = QApplication::activeWindow())
                    parentWindow = aw->windowHandle();
                if (!parentWindow) {
                    for (QWidget* tw : QApplication::topLevelWidgets()) {
                        if (tw && tw->isVisible() && tw->windowHandle()) {
                            parentWindow = tw->windowHandle();
                            break;
                        }
                    }
                }
                if (parentWindow)
                    w->setTransientParent(parentWindow);
                connect(w, &QQuickWindow::closing, this,
                    [this, w, engine]() {
                        if (m_rampEditorWindow != w) return;
                        m_rampEditorWindow = nullptr;
                        emit rampEditorChanged();
                        engine->deleteLater();
                    }, Qt::DirectConnection);
                w->show();
                w->raise();
                w->requestActivate();
            }
            emit rampEditorChanged();
        }, Qt::DirectConnection);
    engine->load(QUrl(QStringLiteral("qrc:/PropertiesPanel/GradientRampEditor.qml")));
    if (!*handled)
        engine->deleteLater();
    SentryReporter::addBreadcrumb("paint.brush.gradient", "ramp editor opened");
}

void TexturePaintController::closeRampEditor()
{
    if (!m_rampEditorWindow) return;
    if (auto* w = qobject_cast<QQuickWindow*>(m_rampEditorWindow)) {
        w->close();
    } else {
        m_rampEditorWindow->deleteLater();
        m_rampEditorWindow = nullptr;
        emit rampEditorChanged();
    }
}

// --- Paint v2 Slice B (#545) — textured / stamp brushes ---

void TexturePaintController::setFootprintType(int type)
{
    auto t = static_cast<BrushFootprint::FootprintType>(type);
    if (t < BrushFootprint::FootprintType::Round
        || t > BrushFootprint::FootprintType::TilingSource)
        t = BrushFootprint::FootprintType::Round;
    if (t == m_footprintType)
        return;
    m_footprintType = t;
    QSettings().setValue(AppSettingsKeys::paintFootprintType(), static_cast<int>(t));
    SentryReporter::addBreadcrumb(
        "paint.brush.stamp",
        QStringLiteral("footprint=%1 stamp=%2 tiling=%3")
            .arg(static_cast<int>(t))
            .arg(m_activeStampName)
            .arg(m_activeTilingName));
    emit stampChanged();
}

void TexturePaintController::setActiveStampName(const QString& name)
{
    if (name == m_activeStampName)
        return;
    m_activeStampName = name;
    QSettings().setValue(AppSettingsKeys::paintActiveStampName(), m_activeStampName);
    reloadStampImage();
    m_stampCache = {};
    m_stampCachePixelSize = 0;
    refreshStampPreviewUris();
    SentryReporter::addBreadcrumb(
        "paint.brush.stamp",
        QStringLiteral("stamp=%1 mode=stamp").arg(m_activeStampName));
    emit stampChanged();
}

void TexturePaintController::setActiveTilingName(const QString& name)
{
    if (name == m_activeTilingName)
        return;
    m_activeTilingName = name;
    QSettings().setValue(AppSettingsKeys::paintActiveTilingName(), m_activeTilingName);
    reloadTilingImage();
    refreshStampPreviewUris();
    SentryReporter::addBreadcrumb(
        "paint.brush.stamp",
        QStringLiteral("tiling=%1 mode=tiling").arg(m_activeTilingName));
    emit stampChanged();
}

QStringList TexturePaintController::stampNames() const
{
    QStringList names;
    QSet<QString> seen;
    for (const auto& a : BrushAssetLibrary::listAssets(BrushAssetLibrary::AssetKind::Stamp)) {
        const QString name = QString::fromStdString(a.name);
        const QString key = name.toLower();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        names << name;
    }
    return names;
}

QStringList TexturePaintController::tilingNames() const
{
    QStringList names;
    for (const auto& a : BrushAssetLibrary::listAssets(BrushAssetLibrary::AssetKind::Tiling))
        names << QString::fromStdString(a.name);
    return names;
}

void TexturePaintController::setStampSpacing(double v)
{
    const double clamped = std::clamp(v, 0.05, 2.0);
    m_stampSettings.spacing = static_cast<float>(clamped);
    QSettings().setValue(AppSettingsKeys::paintStampSpacing(), clamped);
    emit stampChanged();
}

void TexturePaintController::setStampScatter(double v)
{
    const double clamped = std::clamp(v, 0.0, 1.0);
    m_stampSettings.scatter = static_cast<float>(clamped);
    QSettings().setValue(AppSettingsKeys::paintStampScatter(), clamped);
    emit stampChanged();
}

void TexturePaintController::setStampSizeJitter(double v)
{
    const double clamped = std::clamp(v, 0.0, 1.0);
    m_stampSettings.sizeJitter = static_cast<float>(clamped);
    QSettings().setValue(AppSettingsKeys::paintStampSizeJitter(), clamped);
    emit stampChanged();
}

void TexturePaintController::setStampOpacityJitter(double v)
{
    const double clamped = std::clamp(v, 0.0, 1.0);
    m_stampSettings.opacityJitter = static_cast<float>(clamped);
    QSettings().setValue(AppSettingsKeys::paintStampOpacityJitter(), clamped);
    emit stampChanged();
}

void TexturePaintController::setStampRotation(int mode)
{
    auto r = static_cast<BrushFootprint::StampRotation>(mode);
    if (r < BrushFootprint::StampRotation::None || r > BrushFootprint::StampRotation::RandomJitter)
        r = BrushFootprint::StampRotation::None;
    m_stampSettings.rotation = r;
    QSettings().setValue(AppSettingsKeys::paintStampRotation(), mode);
    emit stampChanged();
}

void TexturePaintController::setStampFixedAngle(double deg)
{
    m_stampSettings.fixedAngleDeg = static_cast<float>(deg);
    QSettings().setValue(AppSettingsKeys::paintStampFixedAngle(), deg);
    emit stampChanged();
}

void TexturePaintController::setTilingScale(double v)
{
    const double clamped = std::max(v, 0.01);
    m_tilingSettings.scale = static_cast<float>(clamped);
    QSettings().setValue(AppSettingsKeys::paintTilingScale(), clamped);
    emit stampChanged();
}

void TexturePaintController::setTilingRotation(double deg)
{
    const double clamped = std::clamp(deg, 0.0, 360.0);
    m_tilingSettings.rotationDeg = static_cast<float>(clamped);
    QSettings().setValue(AppSettingsKeys::paintTilingRotation(), clamped);
    emit stampChanged();
}

void TexturePaintController::setTilingOffsetU(double v)
{
    const double clamped = std::clamp(v, -1.0, 1.0);
    m_tilingSettings.offsetU = static_cast<float>(clamped);
    QSettings().setValue(AppSettingsKeys::paintTilingOffsetU(), clamped);
    emit stampChanged();
}

void TexturePaintController::setTilingOffsetV(double v)
{
    const double clamped = std::clamp(v, -1.0, 1.0);
    m_tilingSettings.offsetV = static_cast<float>(clamped);
    QSettings().setValue(AppSettingsKeys::paintTilingOffsetV(), clamped);
    emit stampChanged();
}

QString TexturePaintController::importStampAsset(const QString& filePath)
{
    const std::string stored = BrushAssetLibrary::importAsset(
        filePath.toStdString(), BrushAssetLibrary::AssetKind::Stamp);
    if (stored.empty())
        return {};
    const QString importedName = QString::fromUtf8(
        BrushAssetLibrary::safeFileStem(
            QFileInfo(QString::fromUtf8(stored.c_str())).completeBaseName().toStdString())
            .c_str());
    setActiveStampName(importedName);
    return QString::fromStdString(stored);
}

QString TexturePaintController::importTilingAsset(const QString& filePath)
{
    const std::string stored = BrushAssetLibrary::importAsset(
        filePath.toStdString(), BrushAssetLibrary::AssetKind::Tiling);
    if (stored.empty())
        return {};
    const QString importedName = QString::fromUtf8(
        BrushAssetLibrary::safeFileStem(
            QFileInfo(QString::fromUtf8(stored.c_str())).completeBaseName().toStdString())
            .c_str());
    setActiveTilingName(importedName);
    return QString::fromStdString(stored);
}

bool TexturePaintController::deleteCustomStamp(const QString& name)
{
    if (!BrushAssetLibrary::deleteCustom(name.toStdString(), BrushAssetLibrary::AssetKind::Stamp))
        return false;
    if (m_activeStampName == name)
        setActiveStampName(QStringLiteral("Soft Circle"));
    else
        emit stampChanged();
    return true;
}

bool TexturePaintController::deleteCustomTiling(const QString& name)
{
    if (!BrushAssetLibrary::deleteCustom(name.toStdString(), BrushAssetLibrary::AssetKind::Tiling))
        return false;
    if (m_activeTilingName == name)
        setActiveTilingName(QStringLiteral("Wood"));
    else
        emit stampChanged();
    return true;
}

bool TexturePaintController::renameCustomStamp(const QString& oldName, const QString& newName)
{
    if (oldName.trimmed().isEmpty() || newName.trimmed().isEmpty())
        return false;
    if (!BrushAssetLibrary::renameCustom(oldName.toStdString(), newName.toStdString(),
                                         BrushAssetLibrary::AssetKind::Stamp))
        return false;
    if (m_activeStampName == oldName)
        setActiveStampName(newName.trimmed());
    else
        emit stampChanged();
    return true;
}

bool TexturePaintController::renameCustomTiling(const QString& oldName, const QString& newName)
{
    if (oldName.trimmed().isEmpty() || newName.trimmed().isEmpty())
        return false;
    if (!BrushAssetLibrary::renameCustom(oldName.toStdString(), newName.toStdString(),
                                         BrushAssetLibrary::AssetKind::Tiling))
        return false;
    if (m_activeTilingName == oldName)
        setActiveTilingName(newName.trimmed());
    else
        emit stampChanged();
    return true;
}

bool TexturePaintController::isBundledStamp(const QString& name) const
{
    for (const auto& a : BrushAssetLibrary::listAssets(BrushAssetLibrary::AssetKind::Stamp)) {
        if (QString::compare(QString::fromStdString(a.name), name, Qt::CaseInsensitive) == 0)
            return a.bundled;
    }
    return true;
}

bool TexturePaintController::isBundledTiling(const QString& name) const
{
    for (const auto& a : BrushAssetLibrary::listAssets(BrushAssetLibrary::AssetKind::Tiling)) {
        if (QString::compare(QString::fromStdString(a.name), name, Qt::CaseInsensitive) == 0)
            return a.bundled;
    }
    return true;
}

QString TexturePaintController::stampThumbnailUri(const QString& name) const
{
    const std::string path = BrushAssetLibrary::resolvePath(
        name.toStdString(), BrushAssetLibrary::AssetKind::Stamp);
    if (path.empty())
        return {};
    return QString::fromStdString(BrushAssetLibrary::thumbnailDataUri(path));
}

QString TexturePaintController::tilingThumbnailUri(const QString& name) const
{
    const std::string path = BrushAssetLibrary::resolvePath(
        name.toStdString(), BrushAssetLibrary::AssetKind::Tiling);
    if (path.empty())
        return {};
    return QString::fromStdString(BrushAssetLibrary::thumbnailDataUri(path));
}

void TexturePaintController::reloadStampImage()
{
    const std::string path = BrushAssetLibrary::resolvePath(
        m_activeStampName.toStdString(), BrushAssetLibrary::AssetKind::Stamp);
    m_stampImage = BrushAssetLibrary::loadImage(path);
}

void TexturePaintController::reloadTilingImage()
{
    const std::string path = BrushAssetLibrary::resolvePath(
        m_activeTilingName.toStdString(), BrushAssetLibrary::AssetKind::Tiling);
    m_tilingImage = BrushAssetLibrary::loadImage(path);
}

void TexturePaintController::rebuildStampCache(float radiusUv)
{
    if (m_buffer.width() <= 0 || m_stampImage.empty())
        return;
    const int px = std::clamp(
        static_cast<int>(std::lround(radiusUv * static_cast<float>(m_buffer.width()) * 2.0f)),
        8, 256);
    if (px == m_stampCachePixelSize && !m_stampCache.empty())
        return;
    m_stampCache = BrushFootprint::rasterizeStamp(m_stampImage, px);
    m_stampCachePixelSize = px;
}

void TexturePaintController::refreshStampPreviewUris()
{
    const std::string stampPath = BrushAssetLibrary::resolvePath(
        m_activeStampName.toStdString(), BrushAssetLibrary::AssetKind::Stamp);
    const std::string tilingPath = BrushAssetLibrary::resolvePath(
        m_activeTilingName.toStdString(), BrushAssetLibrary::AssetKind::Tiling);
    m_stampPreviewUri = stampPath.empty()
        ? QString()
        : QString::fromStdString(BrushAssetLibrary::thumbnailDataUri(stampPath));
    m_tilingPreviewUri = tilingPath.empty()
        ? QString()
        : QString::fromStdString(BrushAssetLibrary::thumbnailDataUri(tilingPath));
}

float TexturePaintController::strokeDirectionRad() const
{
    if (m_strokeDirSmoothed.squaredLength() < 1e-12f)
        return 0.0f;
    return std::atan2(m_strokeDirSmoothed.y, m_strokeDirSmoothed.x);
}

TexturePaintBuffer::BrushShape TexturePaintController::currentBrushShape() const
{
    if (m_footprintType == BrushFootprint::FootprintType::Square) {
        return TexturePaintBuffer::BrushShape::Square;
    }
    auto* em = EditModeController::instance();
    if (em && em->vertexPaintShape() == EditModeController::ShapeSquare)
        return TexturePaintBuffer::BrushShape::Square;
    return TexturePaintBuffer::BrushShape::Round;
}

TexturePaintBuffer::ColorAtFn TexturePaintController::buildBrushColorAtFn(float strokeT) const
{
    const QColor c = texturePaintColor();
    const Ogre::ColourValue solid(c.redF(), c.greenF(), c.blueF(), c.alphaF());
    if (m_colorSource != ColorGradient) {
        return [solid](float, float) { return solid; };
    }
    const GradientRamp::Ramp* ramp = resolveActiveRamp();
    if (!ramp || !ramp->isValid()) {
        return [solid](float, float) { return solid; };
    }
    BrushEngine::SampleParams params;
    params.source = BrushEngine::ColorSource::Gradient;
    params.solid = {solid.r, solid.g, solid.b, solid.a};
    params.ramp = ramp;
    params.mode = static_cast<BrushEngine::GradientMode>(m_gradientMode);
    params.strokeT = strokeT;
    params.phaseJitter = m_strokePhaseJitter;
    if (m_gradientMode == GradientLinear)
        params.phaseJitter = 0.0f;
    const BrushEngine::SampleParams paramsCopy = params;
    return [paramsCopy](float dx, float dy) {
        BrushEngine::SampleParams local = paramsCopy;
        local.dx = dx;
        local.dy = dy;
        const auto s = BrushEngine::sampleColor(local);
        return Ogre::ColourValue(s.r, s.g, s.b, s.a);
    };
}

bool TexturePaintController::paintColorFootprintAtUV(const Ogre::Vector2& uv, float radiusUv,
                                                     float strength)
{
    noteStrokeSample(uv, m_strokeJustBegan);
    m_strokeJustBegan = false;

    // Paint v2 Slice F (#549): stencil / camera-locked projection brush. Instead
    // of a flat footprint, splat the brush footprint's texels through the camera,
    // each masked by the projected stencil image's alpha + occlusion. Painted
    // into the active layer → captured by the normal stroke undo.
    if (m_projectionMode == 1 || m_projectionMode == 2) {
        ensureProjTris();
        if (!m_haveProjTris) return false;
        ProjectionPainter::View v;
        auto* widget = m_pendingStrokeWidget ? m_pendingStrokeWidget
                       : (TransformOperator::getSingletonPtr()
                              ? TransformOperator::getSingleton()->getActiveWidget() : nullptr);
        if (!currentProjectionView(widget, v)) return false;
        ProjectionPainter::Options opts = projectionOptions();
        const ProjectionPainter::OcclusionMap* occ =
            (m_haveProjOcc && (opts.useOcclusion || opts.depthLimit > 0.0f)) ? &m_projOcc : nullptr;
        const QColor c = texturePaintColor();
        const Ogre::ColourValue brush(c.redF(), c.greenF(), c.blueF(), c.alphaF());
        const int n = ProjectionPainter::projectDab(
            m_projTris, v, m_stencilImage, uv, radiusUv, brush,
            strength * static_cast<float>(c.alphaF()), activePaintBuffer(), opts, occ);
        return n > 0;
    }


    const float falloff = static_cast<float>(texturePaintFalloff());
    const TexturePaintBuffer::BrushShape shape = currentBrushShape();
    const float wavelength = std::max(radiusUv * 4.0f, 0.05f);
    const float strokeT = BrushEngine::linearStrokeT(
        m_strokePathLength, wavelength, m_strokePhaseJitter);
    const auto colorAt = buildBrushColorAtFn(strokeT);

    if (m_footprintType == BrushFootprint::FootprintType::TilingSource) {
        if (m_tilingImage.empty())
            return false;
        const Ogre::Vector2 center = uv;
        const float radiusCopy = radiusUv;
        const BrushFootprint::ImageRgba& tilingRef = m_tilingImage;
        const BrushFootprint::TilingSettings& settingsRef = m_tilingSettings;
        return activePaintBuffer().paintBrush(
                   uv, radiusUv,
                   [colorAt, center, radiusCopy, &tilingRef, &settingsRef](float dx, float dy) {
                       float tu = 0.0f;
                       float tv = 0.0f;
                       BrushFootprint::brushOffsetToUv(
                           center.x, center.y, radiusCopy, dx, dy, tu, tv);
                       const auto tile = BrushFootprint::sampleTiling(
                           tilingRef, tu, tv, settingsRef);
                       const Ogre::ColourValue brush = colorAt(dx, dy);
                       return Ogre::ColourValue(
                           tile.r * brush.r,
                           tile.g * brush.g,
                           tile.b * brush.b,
                           tile.a * brush.a);
                   },
                   strength, falloff, shape, true)
            > 0;
    }

    if (m_footprintType == BrushFootprint::FootprintType::StampImage) {
        if (m_stampImage.empty())
            return false;
        rebuildStampCache(radiusUv);
        auto* rng = QRandomGenerator::global();
        const float r0 = static_cast<float>(rng->generateDouble());
        const float r1 = static_cast<float>(rng->generateDouble());
        const float r2 = static_cast<float>(rng->generateDouble());
        const float r3 = static_cast<float>(rng->generateDouble());
        float scatterU = 0.0f;
        float scatterV = 0.0f;
        BrushFootprint::applyScatter(radiusUv, m_stampSettings.scatter, r0, r1, scatterU, scatterV);
        const Ogre::Vector2 stampUv(uv.x + scatterU, uv.y + scatterV);
        const float stampRadius = BrushFootprint::jitteredRadius(
            radiusUv, m_stampSettings.sizeJitter, r2);
        const float stampStrength = BrushFootprint::jitteredStrength(
            strength, m_stampSettings.opacityJitter, r3);
        const float angle = BrushFootprint::stampRotationRad(
            m_stampSettings, strokeDirectionRad(), static_cast<float>(rng->generateDouble()));
        return activePaintBuffer().paintStamp(
                   stampUv, stampRadius, m_stampCache, angle, colorAt, stampStrength)
            > 0;
    }

    if (m_colorSource != ColorGradient) {
        const QColor qc = texturePaintColor();
        const Ogre::ColourValue paint(qc.redF(), qc.greenF(), qc.blueF(), qc.alphaF());
        return activePaintBuffer().paintBrush(uv, radiusUv, paint, strength, falloff, shape) > 0;
    }

    if (m_gradientMode == GradientLinear) {
        const auto sampled = colorAt(0.0f, 0.0f);
        return activePaintBuffer().paintBrush(uv, radiusUv, sampled, strength, falloff, shape) > 0;
    }
    return activePaintBuffer().paintBrush(uv, radiusUv, colorAt, strength, falloff, shape) > 0;
}

void TexturePaintController::setPaintTarget(int target)
{
    PaintTarget t = static_cast<PaintTarget>(target);
    if (t == m_target) return;

    // Abort any active stroke first — switching target mid-stroke
    // crashes because beginStroke captured one set of buffers and
    // updateStroke would write into the other.
    if (m_strokeActive) {
        try { endStroke(); } catch (...) {}
    }
    // Tear down the texture-paint session when leaving texture target.
    // The session owns a GPU texture, rebind state, and an EditableMesh
    // built for the texture-paint flow; leaving any of that in place
    // when target=Vertex was the source of the "switch crashes app"
    // bug — the next vertex stroke called into half-initialized state.
    if (m_target == TargetTexture && t == TargetVertex && hasActiveSession()) {
        try { closeSession(); } catch (...) {}
    }
    m_target = t;
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Paint target = %1").arg(target == TargetVertex ? "vertex" : "texture"));
    // Eagerly create the texture session when switching to texture
    // paint while the tool is already active — otherwise the first
    // stroke races session setup against GPU flush/rebind.
    if (m_target == TargetTexture && m_paintEnabled) {
        if (auto* entity = activeEntity()) {
            ensureEditableMesh(entity);
            if (!hasActiveSession()) {
                const int res = m_buffer.width() > 0 ? m_buffer.width() : 1024;
                ensurePaintableTexture(res);
            }
        }
    }
    emit paintTargetChanged();
    emit sessionChanged();
    emit smartSelectChanged();  // the mask UI is texture-only
}

void TexturePaintController::setActiveSlotIndex(int index)
{
    if (index < 0 || index >= m_slots.size()) return;
    if (m_activeSlot == index) return;
    // Preserve the user's current buffer resolution across slot
    // switches — without this, jumping slots always falls back to 1024
    // even after the user picked 2048.
    const int preservedRes = m_buffer.width() > 0 ? m_buffer.width() : 1024;
    m_activeSlot = index;
    // Switching slots resets the buffer to the new slot's texture.
    closeSession();
    ensurePaintableTexture(preservedRes);
    emit slotsChanged();
}

QColor TexturePaintController::texturePaintColor() const
{
    auto* em = EditModeController::instance();
    return em ? em->vertexPaintColor() : QColor(255, 0, 0);
}

double TexturePaintController::texturePaintRadius() const
{
    auto* em = EditModeController::instance();
    return em ? em->vertexPaintRadius() : 0.05;
}

double TexturePaintController::texturePaintRadiusUV() const
{
    // Same mapping the texture-paint dispatch uses (see applyBrushAt)
    // — divide the mesh-local radius by the mesh bounding-box half-
    // size. When no paint mesh is available yet (no session) the raw
    // mesh-local value is returned; the QML overlay treats anything
    // > 0.5 as "clamp visually".
    const double raw = texturePaintRadius();
    if (!m_paintMesh) return raw;
    const auto bbox = m_paintMesh->calculateBounds();
    if (!bbox.isFinite()) return raw;
    const float meshExtent = bbox.getSize().length() * 0.5f;
    if (meshExtent <= 0.0f) return raw;
    double uv = raw / static_cast<double>(meshExtent);
    if (uv < 0.005) uv = 0.005;
    if (uv > 1.0)  uv = 1.0;
    return uv;
}

float TexturePaintController::brushRadiusUV() const
{
    return static_cast<float>(texturePaintRadiusUV());
}

bool TexturePaintController::paintBrushAlongSegment(const Ogre::Vector2& from,
                                                    const Ogre::Vector2& to)
{
    const Ogre::Vector2 delta = to - from;
    const float dist = delta.length();
    if (dist < 1e-6f)
        return applyBrushAtUV(to);

    const float radius = brushRadiusUV();
    float spacing = std::max(radius * 0.35f, 0.002f);
    if (m_footprintType == BrushFootprint::FootprintType::StampImage) {
        spacing = BrushFootprint::stampSpacingUv(radius, m_stampSettings.spacing);
        const float pathBase = m_strokePathLength;
        const float projectedLen = pathBase + dist;
        if (projectedLen - m_lastStampDabPathLength < spacing) {
            noteStrokeSample(to, false);
            return false;
        }
        bool changed = false;
        float dabPath = m_lastStampDabPathLength + spacing;
        int dabCount = 0;
        while (dabPath <= projectedLen + 1e-6f && dabCount < 8) {
            const float t = std::clamp(
                (dabPath - pathBase) / std::max(dist, 1e-6f), 0.0f, 1.0f);
            const Ogre::Vector2 pt(from.x + delta.x * t, from.y + delta.y * t);
            if (applyBrushAtUV(pt))
                changed = true;
            dabPath += spacing;
            ++dabCount;
        }
        m_lastStampDabPathLength = dabPath - spacing;
        noteStrokeSample(to, false);
        return changed;
    }

    int steps = std::max(1, static_cast<int>(std::ceil(dist / spacing)));
    steps = std::min(steps, 8);

    bool changed = false;
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const Ogre::Vector2 pt(from.x + delta.x * t, from.y + delta.y * t);
        if (applyBrushAtUV(pt))
            changed = true;
    }
    return changed;
}

int TexturePaintController::brushShape() const
{
    auto* em = EditModeController::instance();
    return em ? em->vertexPaintShape() : 0;
}

QColor TexturePaintController::bgPaintColor() const
{
    auto* em = EditModeController::instance();
    return em ? em->vertexPaintBackgroundColor() : QColor(255, 255, 255);
}

double TexturePaintController::texturePaintStrength() const
{
    auto* em = EditModeController::instance();
    return em ? em->vertexPaintStrength() : 0.75;
}

double TexturePaintController::texturePaintFalloff() const
{
    auto* em = EditModeController::instance();
    return em ? em->vertexPaintFalloff() : 0.5;
}

Ogre::Entity* TexturePaintController::activeEntity() const
{
    // First selected entity in Material Mode = paint target. Edit Mode
    // is no longer involved — we keep our own EditableMesh so painting
    // works regardless of workspace mode.
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return nullptr;
    auto entities = sel->getResolvedEntities();
    return entities.isEmpty() ? nullptr : entities.first();
}

bool TexturePaintController::ensureEditableMesh(Ogre::Entity* entity)
{
    if (!entity) return false;
    if (m_paintMeshEntity == entity && m_paintMesh) return true;
    auto mesh = std::make_unique<EditableMesh>();
    if (!mesh->loadFromEntity(entity)) {
        m_paintMesh.reset();
        m_paintMeshEntity = nullptr;
        return false;
    }
    m_paintMesh = std::move(mesh);
    m_paintMeshEntity = entity;
    m_hitCache.valid = false;
    return true;
}

Ogre::TextureUnitState* TexturePaintController::findOrCreateActiveTextureUnit(Ogre::Entity* entity)
{
    if (!entity || entity->getNumSubEntities() == 0) return nullptr;

    // If we have a slot model, use the slot's recorded submesh +
    // texture name to find the matching TUS.
    if (m_activeSlot >= 0 && m_activeSlot < m_slots.size()) {
        auto m = m_slots.at(m_activeSlot).toMap();
        const int subIdx = m.value("submesh", -1).toInt();
        const std::string slotName = m.value("slot").toString().toStdString();
        if (subIdx >= 0 && subIdx < static_cast<int>(entity->getNumSubEntities())) {
            auto* subEnt = entity->getSubEntity(subIdx);
            if (subEnt) {
                Ogre::MaterialPtr mat = subEnt->getMaterial();
                if (mat && mat->getNumTechniques() > 0) {
                    auto* tech = mat->getTechnique(0);
                    if (tech && tech->getNumPasses() > 0) {
                        auto* pass = tech->getPass(0);
                        if (pass) {
                            for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
                                auto* tus = pass->getTextureUnitState(i);
                                if (tus->getName() == slotName)
                                    return tus;
                            }
                            // Slot named in model but not found on the pass
                            // (rare — model out of sync). Fall through to
                            // first TUS / create.
                        }
                    }
                }
            }
        }
    }

    // Fallback: first submesh, first material, first pass.
    auto* subEnt = entity->getSubEntity(0);
    if (!subEnt) return nullptr;
    Ogre::MaterialPtr mat = subEnt->getMaterial();
    if (!mat || mat->getNumTechniques() == 0) return nullptr;
    auto* tech = mat->getTechnique(0);
    if (!tech || tech->getNumPasses() == 0) return nullptr;
    auto* pass = tech->getPass(0);
    if (!pass) return nullptr;

    // Paint v2 Slice D: the active channel names the canonical slot we want
    // (albedo/normal_map/roughness/metallic/ao/emissive; Height→normal_map).
    // Find that named TUS; if the material doesn't have it yet, CREATE it so
    // the user can paint a channel the asset never shipped. When no channel
    // targets a slot (legacy path) fall back to albedo/diffuse_map.
    const std::string wantSlot = activeChannelSlotName();
    if (!wantSlot.empty()) {
        // BaseColor aliases: imported PBR materials name the diffuse texture
        // `diffuse_map` (or both albedo + diffuse_map). Reuse the existing one
        // instead of creating an empty `albedo` slot + a blank white session.
        const bool baseColor = (m_activeChannel == PaintChannelNS::Channel::BaseColor);
        for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
            auto* tus = pass->getTextureUnitState(i);
            const std::string& n = tus->getName();
            if (n == wantSlot) return tus;
            if (baseColor && n == "diffuse_map") return tus;
        }
        // Not present — create it named for the channel's slot. It starts with
        // no texture; ensurePaintableTexture fills it with a blank/loaded buffer
        // and bakeChannel wires it into the PBR material for IBL.
        auto* tus = pass->createTextureUnitState();
        tus->setName(wantSlot);
        SentryReporter::addBreadcrumb(
            "paint.channel",
            QStringLiteral("created slot %1").arg(QString::fromStdString(wantSlot)));
        return tus;
    }

    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        const std::string& n = tus->getName();
        if (n == "albedo" || n == "diffuse_map")
            return tus;
    }
    if (pass->getNumTextureUnitStates() > 0)
        return pass->getTextureUnitState(0);
    return pass->createTextureUnitState();
}

bool TexturePaintController::ensurePaintableTexture(int resolution)
{
    auto* entity = activeEntity();
    if (!entity) {
        m_sessionEntity = nullptr;
        emit sessionChanged();
        return false;
    }

    // Per-channel sessions belong to ONE entity. If the painted entity changed
    // (e.g. the user selected a different mesh), drop the stashed stacks so a
    // previously-used channel doesn't restore — or bake — the old entity's
    // pixels onto the new one (#547 review).
    if (m_channelSessionEntity && m_channelSessionEntity != entity) {
        m_channelSessions.clear();
        const bool channelReset = (m_activeChannel != PaintChannelNS::Channel::BaseColor);
        m_activeChannel = PaintChannelNS::Channel::BaseColor;
        SentryReporter::addBreadcrumb(
            "paint.channel",
            QStringLiteral("entity changed → discard channel sessions, reset to BaseColor"));
        // QML may still show the previous channel while painting now targets
        // BaseColor — notify so the picker selection follows the forced reset.
        if (channelReset) emit activeChannelChanged();
    }
    m_channelSessionEntity = entity;

    if (m_sessionEntity == entity && m_buffer.width() > 0 && !m_textureName.isEmpty()) {
        // Active session for this entity — make sure m_paintMesh is
        // valid (it could have been torn down by a previous
        // closeSession that fired after the user's last stroke).
        if (!ensureEditableMesh(entity)) {
            emit sessionChanged();
            return false;
        }
        return true;
    }

    // Reset any prior session first — closeSession() clears m_paintMesh,
    // so we have to rebuild the EditableMesh AFTER this call. Doing it
    // before would leave m_paintMesh null on return, breaking every
    // hit-test (findMeshPointForUV walks m_paintMesh's submeshes).
    closeSession();
    m_sessionEntity = entity;

    if (!ensureEditableMesh(entity)) {
        // No mesh data → can't UV-hit-test → can't paint.
        m_sessionEntity = nullptr;
        emit sessionChanged();
        return false;
    }

    auto* tu = findOrCreateActiveTextureUnit(entity);
    if (!tu) {
        emit sessionChanged();
        return false;
    }

    QString existingTex = QString::fromStdString(tu->getTextureName());
    // The Normal (and legacy Height) channel paints a fresh HEIGHT field that
    // targets the normal_map slot (which may already hold a tangent-space
    // normal). Do NOT seed the paint buffer from that texture — Sobel would
    // then read the existing normal's RGB as height and bake garbage, and the
    // painted layer would carry the base normal instead of just the sculpted
    // relief. Start blank, but REMEMBER the real slot texture (skipping any
    // transient QMEPaint_* paint texture) so bakeChannel can whiteout-blend the
    // detail onto it.
    m_channelBaseTextureName.clear();
    if (m_activeChannel == PaintChannelNS::Channel::Height
        || m_activeChannel == PaintChannelNS::Channel::Normal) {
        if (!existingTex.isEmpty()
            && !existingTex.startsWith(QStringLiteral("QMEPaint_")))
            m_channelBaseTextureName = existingTex;
        existingTex.clear();
    }
    bool loadedExisting = false;
    QString loadError;
    // Track the original texture handle (not just its name) so we
    // can paint directly into it via blitFromMemory and skip the
    // TUS rebind entirely. This is the most reliable path on macOS
    // Metal where "missing texture" fallback gives yellow and
    // TU_DYNAMIC manual textures sometimes don't get uploaded.
    Ogre::TexturePtr originalTex;
    if (!existingTex.isEmpty()) {
        try {
            originalTex = findTextureAcrossGroups(existingTex.toStdString());
        } catch (...) {}
    }
    m_originalTexture = originalTex;
    m_originalTextureName = existingTex;
    if (!existingTex.isEmpty()) {
        // Try multiple strategies because imported PBR textures can
        // come from inline FBX embeds (no disk file), legacy on-disk
        // files, or auto-generated render targets. Each strategy
        // succeeds for a different source.
        Ogre::TexturePtr existing = originalTex;
        if (!existing) {
            try {
                existing = findTextureAcrossGroups(existingTex.toStdString());
            } catch (...) {}
        }

        // 0. CPU-side: embedded FBX bytes, on-disk origin, resource-group path.
        if (loadPaintBufferFromNonGpuSources(m_buffer, existing, existingTex)) {
            loadedExisting = true;
            loadError.clear();
        }

        // 1. TextureManager → convertToImage (works when Ogre keeps
        //    pixels in an Image buffer beside the GPU upload).
        if (!loadedExisting && existing) {
            try {
                if (!existing->isLoaded()) existing->load();
                Ogre::Image img;
                existing->convertToImage(img);
                const int w = static_cast<int>(img.getWidth());
                const int h = static_cast<int>(img.getHeight());
                if (w > 0 && h > 0) {
                    m_buffer.resize(w, h);
                    Ogre::PixelBox srcBox = img.getPixelBox();
                    Ogre::PixelBox dstBox(w, h, 1, Ogre::PF_BYTE_RGBA,
                                          m_buffer.data().data());
                    Ogre::PixelUtil::bulkPixelConversion(srcBox, dstBox);
                    m_buffer.clearDirty();
                    loadedExisting = true;
                }
            } catch (const Ogre::Exception& e) {
                loadError = QString::fromStdString(e.getDescription());
            } catch (...) {
                loadError = QStringLiteral("convertToImage exception");
            }
        } else if (!loadedExisting && !existing) {
            loadError = QStringLiteral("texture not found in TextureManager");
        }

        // 2. Read from GPU directly via blitToMemory. Works when the
        //    texture is on the GPU but its source Image buffer was
        //    discarded post-upload (common for static textures).
        //    Reading in the texture's native format and letting Ogre
        //    convert is more reliable than asking for RGBA up front —
        //    some Metal/GL drivers refuse the format mismatch.
        if (!loadedExisting && existing) {
            try {
                if (!existing->isLoaded()) existing->load();
                const int w = static_cast<int>(existing->getWidth());
                const int h = static_cast<int>(existing->getHeight());
                auto pixbuf = existing->getBuffer();
                if (w > 0 && h > 0 && pixbuf) {
                    const Ogre::PixelFormat srcFmt = existing->getFormat();
                    SentryReporter::addBreadcrumb("ui.action",
                        QStringLiteral("Texture paint: blit native fmt=%1 size=%2x%3")
                            .arg(static_cast<int>(srcFmt)).arg(w).arg(h));
                    const size_t srcBytes = Ogre::PixelUtil::getMemorySize(w, h, 1, srcFmt);
                    std::vector<uint8_t> srcBuf(srcBytes);
                    Ogre::PixelBox srcPb(w, h, 1, srcFmt, srcBuf.data());
                    pixbuf->blitToMemory(srcPb);
                    m_buffer.resize(w, h);
                    Ogre::PixelBox dstPb(w, h, 1, Ogre::PF_BYTE_RGBA,
                                         m_buffer.data().data());
                    Ogre::PixelUtil::bulkPixelConversion(srcPb, dstPb);
                    m_buffer.clearDirty();
                    loadedExisting = true;
                    loadError.clear();
                }
            } catch (const Ogre::Exception& e) {
                loadError = QStringLiteral("blit-native: ") + QString::fromStdString(e.getDescription());
            } catch (...) {
                loadError = QStringLiteral("blit-native: unknown exception");
            }
        }

        // 3. Ogre::Image::load — works when the texture name is also
        //    a filename in a registered resource location.
        if (!loadedExisting) {
            try {
                Ogre::Image img;
                img.load(existingTex.toStdString(),
                         Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
                const int w = static_cast<int>(img.getWidth());
                const int h = static_cast<int>(img.getHeight());
                if (w > 0 && h > 0) {
                    m_buffer.resize(w, h);
                    Ogre::PixelBox srcBox = img.getPixelBox();
                    Ogre::PixelBox dstBox(w, h, 1, Ogre::PF_BYTE_RGBA,
                                          m_buffer.data().data());
                    Ogre::PixelUtil::bulkPixelConversion(srcBox, dstBox);
                    m_buffer.clearDirty();
                    loadedExisting = true;
                    loadError.clear();
                }
            } catch (const Ogre::Exception& e) {
                loadError = QStringLiteral("image-load: ") + QString::fromStdString(e.getDescription());
            } catch (...) {
                loadError = QStringLiteral("image-load: unknown");
            }
        }

        // 4. QImage as a raw file path. Handles textures whose name
        //    is a relative or absolute filesystem path (some assets
        //    keep their texture name == on-disk path).
        if (!loadedExisting) {
            try {
                QImage qimg(existingTex);
                if (!qimg.isNull()) {
                    qimg = qimg.convertToFormat(QImage::Format_RGBA8888);
                    const int w = qimg.width();
                    const int h = qimg.height();
                    m_buffer.resize(w, h);
                    for (int y = 0; y < h; ++y) {
                        std::memcpy(m_buffer.data().data() + static_cast<size_t>(y) * w * 4u,
                                    qimg.constScanLine(y),
                                    static_cast<size_t>(w) * 4u);
                    }
                    m_buffer.clearDirty();
                    loadedExisting = true;
                    loadError.clear();
                }
            } catch (...) {
                // All four failed.
            }
        }
    }

    if (!loadedExisting) {
        const int res = std::max(16, resolution);
        m_buffer.resize(res, res);
        // Start a source-less channel session TRANSPARENT, not opaque white.
        // Layer 0 is initFromFlatBuffer(m_buffer), so an opaque-white base
        // meant: (a) switching to an unpainted channel rebound a solid-white
        // texture onto the model's slot (washing the surface out / "losing"
        // the texture), and (b) a Bake on a barely-painted channel captured
        // that white base. Transparent composites to nothing, so an unpainted
        // channel leaves the model's real textures showing and bakes empty
        // (#547 bake-goes-white bug).
        m_buffer.clear(Ogre::ColourValue(0.f, 0.f, 0.f, 0.f));
        m_buffer.clearDirty();
        // CPU buffer size won't match the model's bound GPU texture. In-place
        // blit with mismatched sizes crashes some GL/Metal drivers (OOB
        // HardwarePixelBuffer writes).
        m_originalTexture.reset();
        m_useOriginalTexture = false;
        m_forceManualPaintTexture = true;
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: starting from blank %1×%1 (existing tex='%2', err='%3')")
                .arg(res).arg(existingTex).arg(loadError));
    } else {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: loaded existing %1×%2 from '%3'")
                .arg(m_buffer.width()).arg(m_buffer.height()).arg(existingTex));
    }

    static unsigned int s_unique = 0;
    QString hint = QStringLiteral("QMEPaint_%1_%2")
                       .arg(QString::fromStdString(entity->getName()))
                       .arg(++s_unique);
    // Rebind eagerly on session create. The deferred / lazy rebind
    // approach was racing with the mouse-move event stack and
    // crashing mid-stroke. Doing it at session-create time happens
    // BEFORE the first stroke fires, so there's no re-entrant render
    // hazard.
    // Create the GPU texture but DEFER the material rebind. Doing
    // mat->compile()/mat->reload() on the same call stack as the
    // mouse-press event raced with Ogre's render thread and
    // segfaulted mid-stroke. The rebind happens on the next
    // event-loop tick via the deferred-rebind path in
    // doFlushDirtyToOgre.
    if (!createOgreTextureFromBuffer(entity, hint, /*rebindToModel=*/false)) {
        m_buffer = TexturePaintBuffer();
        m_textureName.clear();
        m_sessionEntity = nullptr;
        emit sessionChanged();
        return false;
    }

    // Decide up-front whether the viewport can keep sampling the original
    // texture (in-place GPU writes) or must be rebound to our manual
    // paint texture. Rebind is scheduled here — before the first stroke —
    // so pixels uploaded during painting are visible on the model.
    if (!m_forceManualPaintTexture) {
        m_forceManualPaintTexture =
            !canWriteOriginalTextureInPlace(m_originalTexture,
                                            m_buffer.width(), m_buffer.height());
    }
    if (m_forceManualPaintTexture) {
        m_originalTexture.reset();
        // Do NOT rebind the manual paint texture onto the model here. On
        // session-create the buffer is either the model's own texture (loaded)
        // or blank/transparent — rebinding a blank texture the instant the user
        // switches channels swaps the model's real slot texture for an empty
        // one, which is exactly how navigating channels "lost" the texture
        // (#547). The rebind is deferred to the first real dirty upload in
        // doFlushDirtyToOgre() (which schedules it when m_boundSlots is empty),
        // so the model keeps its real textures until the user actually paints.
    }

    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint session: %1×%2 on %3 (existing tex: %4)")
            .arg(m_buffer.width()).arg(m_buffer.height())
            .arg(QString::fromStdString(entity->getName()))
            .arg(loadedExisting ? "yes" : "no"));

    // The selection mask is paired 1:1 with the paint buffer. Re-size
    // on every session so smartSelect's per-pixel indexing matches.
    m_mask.resize(m_buffer.width(), m_buffer.height());
    m_maskOverlayUri.clear();

    // Paint v2 Slice C (#546): wrap the flat texture in a layer stack.
    m_layerStack.initFromFlatBuffer(m_buffer);
    recomposeComposite(/*fullBuffer=*/true);
    m_layerStrokeBaseline = snapshotActiveLayerPixels();

    refreshPreviewUri();
    if (m_uvOverlayVisible) refreshUvOverlay();
    // Paint v2 Slice E (#548): a fresh session for a new entity invalidates the
    // topology maps; (re)build the symmetry plane overlay for this mesh.
    refreshSymmetryPlaneOverlay();
    emit sessionChanged();
    emit layersChanged();
    emit smartSelectChanged();
    return true;
}

bool TexturePaintController::createOgreTextureFromBuffer(Ogre::Entity* entity,
                                                          const QString& nameHint,
                                                          bool rebindToModel)
{
    if (!entity || m_buffer.width() <= 0 || m_buffer.height() <= 0) return false;
    auto* tu = findOrCreateActiveTextureUnit(entity);
    if (!tu) return false;

    // Snapshot the original texture name on the chosen TUS *before* we
    // overwrite anything. We use this to find every other TUS bound to
    // the same source texture (e.g. `albedo` + `diffuse_map` aliasing
    // on imported PBR materials), so all of them get rebound.
    const std::string originalTexName = tu->getTextureName();

    const std::string texName = nameHint.toStdString();
    const std::string group = Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;
    try {
        auto& tm = Ogre::TextureManager::getSingleton();
        if (auto existing = tm.getByName(texName, group))
            tm.remove(existing);
        m_ogreTexture = tm.createManual(
            texName, group, Ogre::TEX_TYPE_2D,
            m_buffer.width(), m_buffer.height(), 0,
            Ogre::PF_BYTE_RGBA,
            // Plain TU_DYNAMIC: write-only with no discard hint. The
            // _DISCARDABLE variant can drop initial content on some
            // drivers — bad for us since the initial blit IS the
            // user's "current pixels" baseline.
            Ogre::TU_DYNAMIC_WRITE_ONLY);
        if (!m_ogreTexture) return false;
        // Initial upload
        auto pixbuf = m_ogreTexture->getBuffer();
        if (!pixbuf) return false;
        Ogre::PixelBox pb(m_buffer.width(), m_buffer.height(), 1,
                          Ogre::PF_BYTE_RGBA, m_buffer.data().data());
        pixbuf->blitFromMemory(pb);
        m_textureName = QString::fromStdString(texName);

        if (rebindToModel)
            rebindEntityDiffuseToPaintTexture(entity);
        else
            SentryReporter::addBreadcrumb("ui.action",
                QStringLiteral("Texture paint: GPU texture created, deferred rebind"));
        return true;
    } catch (const Ogre::Exception&) {
        return false;
    } catch (...) {
        return false;
    }
}

void TexturePaintController::rebindEntityDiffuseToPaintTexture(Ogre::Entity* entity)
{
    if (!entity || m_textureName.isEmpty()) return;
    // Use the texture name captured at session-create time — the live
    // TUS may already point at a prior paint texture if a rebind failed.
    auto* tu = findOrCreateActiveTextureUnit(entity);
    const std::string originalTexName = !m_originalTextureName.isEmpty()
        ? m_originalTextureName.toStdString()
        : (tu ? tu->getTextureName() : std::string{});
    const std::string texName = m_textureName.toStdString();

    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint: rebind base = '%1' → '%2'")
            .arg(QString::fromStdString(originalTexName))
            .arg(QString::fromStdString(texName)));

    m_boundSlots.clear();
    std::set<Ogre::Material*> touched;
    const int chosenSubmesh =
        (m_activeSlot >= 0 && m_activeSlot < m_slots.size())
            ? m_slots.at(m_activeSlot).toMap().value("submesh", -1).toInt()
            : 0;
    for (unsigned int se = 0; se < entity->getNumSubEntities(); ++se) {
        auto* sub = entity->getSubEntity(se);
        if (!sub) continue;
        Ogre::MaterialPtr mat = sub->getMaterial();
        if (!mat) continue;
        bool changed = false;
        const auto& techs = mat->getTechniques();
        for (size_t tIdx = 0; tIdx < techs.size(); ++tIdx) {
            auto* tech = techs[tIdx];
            for (unsigned short pi = 0; pi < tech->getNumPasses(); ++pi) {
                auto* p = tech->getPass(pi);
                for (unsigned short ti = 0; ti < p->getNumTextureUnitStates(); ++ti) {
                    auto* tusN = p->getTextureUnitState(ti);
                    const std::string currentTex = tusN->getTextureName();
                    bool shouldBind = false;
                    if (!originalTexName.empty()) {
                        shouldBind = (currentTex == originalTexName);
                    } else if (static_cast<int>(se) == chosenSubmesh) {
                        shouldBind = (tusN == tu);
                    }
                    if (!shouldBind) continue;
                    BoundSlot s;
                    s.materialName = mat->getName();
                    s.techIdx = static_cast<unsigned short>(tIdx);
                    s.passIdx = pi;
                    s.tusIdx = ti;
                    s.originalTexture = currentTex;
                    m_boundSlots.push_back(s);
                    // Bind the TexturePtr directly. setTextureName
                    // does name resolution that can pick the wrong
                    // group on macOS/Metal. Direct binding never
                    // mis-resolves.
                    if (m_ogreTexture)
                        tusN->setTexture(m_ogreTexture);
                    else
                        tusN->setTextureName(texName);
                    changed = true;
                }
            }
        }
        if (changed) touched.insert(mat.get());
    }
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint: bound %1 TUSes").arg(m_boundSlots.size()));

    for (auto* mat : touched) {
        try { mat->compile(); mat->reload(); }
        catch (const Ogre::Exception&) {}
    }
}

void TexturePaintController::scheduleRebindToPaintTexture(Ogre::Entity* entity)
{
    if (!entity || m_textureName.isEmpty() || !m_ogreTexture)
        return;
    if (!m_boundSlots.empty())
        return;
    if (m_rebindScheduled)
        return;
    m_rebindScheduled = true;
    QTimer::singleShot(0, this, [this, entity]() {
        m_rebindScheduled = false;
        if (m_sessionEntity != entity || !m_boundSlots.empty())
            return;
        if (!m_ogreTexture || m_textureName.isEmpty())
            return;
        rebindEntityDiffuseToPaintTexture(entity);
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: viewport rebound (%1 TUSes)")
                .arg(m_boundSlots.size()));
    });
}

void TexturePaintController::recomposePaintBufferIfNeeded()
{
    if (m_layerStack.layerCount() > 0 && !m_layerStack.layerDirtyUnion().empty())
        recomposeComposite(/*fullBuffer=*/false);
}

void TexturePaintController::scheduleThrottledLiveGpuFlush()
{
    if (m_buffer.dirtyRect().empty()) return;

    if (!m_strokeLiveUploadStarted) {
        m_strokeLiveUploadStarted = true;
        QTimer::singleShot(0, this, [this]() { flushLiveStrokeToGpu(); });
        return;
    }

    if (m_strokeGpuFlushScheduled) {
        m_strokeGpuFlushPending = true;
        return;
    }
    m_strokeGpuFlushScheduled = true;
    QTimer::singleShot(16, this, [this]() {
        m_strokeGpuFlushScheduled = false;
        flushLiveStrokeToGpu();
        if (m_strokeGpuFlushPending) {
            m_strokeGpuFlushPending = false;
            scheduleThrottledLiveGpuFlush();
        }
    });
}

bool TexturePaintController::flushLiveStrokeToGpu()
{
    const auto dirty = m_buffer.dirtyRect();
    if (dirty.empty()) return true;

    if (m_paintMeshEntity && m_ogreTexture && m_boundSlots.empty())
        rebindEntityDiffuseToPaintTexture(m_paintMeshEntity);

    if (blitBufferRectToOgreTexture(dirty.x0, dirty.y0, dirty.x1, dirty.y1))
        m_buffer.clearDirty();
    return m_buffer.dirtyRect().empty();
}

void TexturePaintController::flushDirtyToOgre()
{
    recomposePaintBufferIfNeeded();
    if (m_buffer.dirtyRect().empty()) return;

    if (m_strokeActive) {
        schedulePreviewRefresh();
        // Preview-panel strokes are low-frequency — upload immediately so
        // the 2D thumbnail and 3D mesh stay in lockstep. Viewport drags
        // stay throttled (~60 Hz) to avoid flooding the GL driver.
        if (m_strokeFromUvPreview)
            flushLiveStrokeToGpu();
        else
            scheduleThrottledLiveGpuFlush();
        return;
    }

    schedulePreviewRefresh();

    const int debounceMs = 33;
    if (m_gpuFlushScheduled) return;
    m_gpuFlushScheduled = true;
    QTimer::singleShot(debounceMs, this, [this]() {
        m_gpuFlushScheduled = false;
        if (m_buffer.dirtyRect().empty()) return;
        doFlushDirtyToOgre();
    });
}

void TexturePaintController::cancelInFlightGpuUpload()
{
    if (!m_tiledUploadRunning && m_tiledUploadQueue.empty())
        return;
    m_tiledUploadRunning = false;
    m_tiledUploadQueue.clear();
    m_tiledUploadIndex = 0;
}

void TexturePaintController::scheduleStrokeGpuFlush()
{
    if (m_buffer.dirtyRect().empty()) return;

    auto kickUpload = [this]() {
        if (m_buffer.dirtyRect().empty()) return;
        if (m_tiledUploadRunning) {
            if (m_activeUploadGeneration != m_gpuUploadGeneration) {
                cancelInFlightGpuUpload();
            } else {
                m_strokeGpuFlushPending = true;
                return;
            }
        }
        m_strokeGpuFlushPending = false;
        startTiledGpuUpload(/*finishingStroke=*/false);
    };

    // First dab in a stroke: upload on the next event-loop tick so colour
    // appears on the model immediately (not after a debounce on release).
    if (!m_strokeLiveUploadStarted) {
        m_strokeLiveUploadStarted = true;
        QTimer::singleShot(0, this, kickUpload);
        return;
    }

    if (m_strokeGpuFlushScheduled) {
        m_strokeGpuFlushPending = true;
        return;
    }
    m_strokeGpuFlushScheduled = true;
    QTimer::singleShot(8, this, [this, kickUpload]() {
        m_strokeGpuFlushScheduled = false;
        kickUpload();
    });
}

Ogre::TexturePtr TexturePaintController::gpuUploadTargetTexture() const
{
    // When the viewport still samples the model's original diffuse, upload
    // there — otherwise strokes only show up after release (manual tex path).
    if (!m_forceManualPaintTexture && m_boundSlots.empty() && m_originalTexture)
        return m_originalTexture;
    return m_ogreTexture;
}

bool TexturePaintController::blitBufferRectToOgreTexture(int x0, int y0, int x1, int y1)
{
    Ogre::TexturePtr tex = gpuUploadTargetTexture();
    if (!tex || x1 <= x0 || y1 <= y0) return false;
    if (tex == m_ogreTexture && m_boundSlots.empty() && m_paintMeshEntity)
        scheduleRebindToPaintTexture(m_paintMeshEntity);

    const int W = m_buffer.width();
    const int rectW = x1 - x0;
    const int rectH = y1 - y0;
    std::vector<uint8_t> rgba(static_cast<size_t>(rectW) * static_cast<size_t>(rectH) * 4u);
    const auto& src = m_buffer.data();
    for (int row = 0; row < rectH; ++row) {
        const size_t srcOff = (static_cast<size_t>(y0 + row) * static_cast<size_t>(W)
                               + static_cast<size_t>(x0)) * 4u;
        const size_t dstOff = static_cast<size_t>(row) * static_cast<size_t>(rectW) * 4u;
        std::memcpy(rgba.data() + dstOff, src.data() + srcOff,
                    static_cast<size_t>(rectW) * 4u);
    }
    try {
        auto buf = tex->getBuffer();
        if (!buf) return false;
        const Ogre::PixelFormat dstFmt = tex->getFormat();
        if (dstFmt == Ogre::PF_BYTE_RGBA || tex == m_ogreTexture) {
            Ogre::PixelBox pb(rectW, rectH, 1, Ogre::PF_BYTE_RGBA, rgba.data());
            Ogre::Box dst(x0, y0, x1, y1);
            buf->blitFromMemory(pb, dst);
        } else {
            Ogre::PixelBox srcRgba(rectW, rectH, 1, Ogre::PF_BYTE_RGBA, rgba.data());
            const size_t dstBytes = Ogre::PixelUtil::getMemorySize(rectW, rectH, 1, dstFmt);
            std::vector<uint8_t> slice(dstBytes);
            Ogre::PixelBox dstPb(rectW, rectH, 1, dstFmt, slice.data());
            Ogre::PixelUtil::bulkPixelConversion(srcRgba, dstPb);
            Ogre::Box dst(x0, y0, x1, y1);
            buf->blitFromMemory(dstPb, dst);
        }
        return true;
    } catch (const Ogre::Exception&) {
        return false;
    }
}

void TexturePaintController::startTiledGpuUpload(bool finishingStroke)
{
    if (m_tiledUploadRunning) {
        if (m_activeUploadGeneration != m_gpuUploadGeneration)
            cancelInFlightGpuUpload();
        else
            return;
    }

    const auto dirty = m_buffer.dirtyRect();
    if (dirty.empty()) {
        m_uploadFinishingStroke = false;
        return;
    }

    m_uploadFinishingStroke = finishingStroke;
    m_activeUploadGeneration = m_gpuUploadGeneration;
    m_uploadEpochSnapshot = m_bufferDirtyEpoch;

    const int dirtyPx = dirty.width() * dirty.height();
    if (m_strokeActive && dirtyPx <= kStrokeDirectUploadMaxPx) {
        if (blitBufferRectToOgreTexture(dirty.x0, dirty.y0, dirty.x1, dirty.y1)
            && m_bufferDirtyEpoch == m_uploadEpochSnapshot) {
            m_buffer.clearDirty();
        }
        onTiledUploadPassComplete();
        return;
    }

    m_uploadPassDirty = dirty;
    m_tiledUploadQueue.clear();
    for (int ty = dirty.y0; ty < dirty.y1; ty += kPaintUploadTilePx) {
        for (int tx = dirty.x0; tx < dirty.x1; tx += kPaintUploadTilePx) {
            UploadTile t;
            t.x0 = tx;
            t.y0 = ty;
            t.x1 = std::min(dirty.x1, tx + kPaintUploadTilePx);
            t.y1 = std::min(dirty.y1, ty + kPaintUploadTilePx);
            m_tiledUploadQueue.push_back(t);
        }
    }
    m_tiledUploadIndex = 0;
    m_tiledUploadRunning = !m_tiledUploadQueue.empty();
    if (!m_tiledUploadRunning)
        return;
    if (gpuUploadTargetTexture() == m_ogreTexture && m_paintMeshEntity && m_boundSlots.empty()
        && m_ogreTexture) {
        rebindEntityDiffuseToPaintTexture(m_paintMeshEntity);
    }
    processNextTiledUploadTile();
}

void TexturePaintController::processNextTiledUploadTile()
{
    if (!m_tiledUploadRunning) return;
    if (m_activeUploadGeneration != m_gpuUploadGeneration) {
        m_tiledUploadRunning = false;
        m_tiledUploadQueue.clear();
        m_tiledUploadIndex = 0;
        return;
    }
    if (m_tiledUploadIndex >= static_cast<int>(m_tiledUploadQueue.size())) {
        onTiledUploadPassComplete();
        return;
    }

    const bool fastUpload = m_strokeActive || m_uploadFinishingStroke;
    const int batch = fastUpload ? kStrokeTilesPerTick : 1;
    for (int b = 0; b < batch && m_tiledUploadIndex < static_cast<int>(m_tiledUploadQueue.size());
         ++b) {
        const UploadTile& t = m_tiledUploadQueue[static_cast<size_t>(m_tiledUploadIndex++)];
        blitBufferRectToOgreTexture(t.x0, t.y0, t.x1, t.y1);
    }

    if (m_tiledUploadIndex >= static_cast<int>(m_tiledUploadQueue.size())) {
        onTiledUploadPassComplete();
        return;
    }

    const int delayMs = fastUpload ? 0 : 1;
    const uint64_t uploadGen = m_activeUploadGeneration;
    QTimer::singleShot(delayMs, this, [this, uploadGen]() {
        if (!m_tiledUploadRunning || uploadGen != m_activeUploadGeneration)
            return;
        processNextTiledUploadTile();
    });
}

void TexturePaintController::onTiledUploadPassComplete()
{
    m_tiledUploadRunning = false;
    m_tiledUploadQueue.clear();
    m_tiledUploadIndex = 0;

    if (m_activeUploadGeneration != m_gpuUploadGeneration) {
        if (m_strokeActive && !m_buffer.dirtyRect().empty())
            scheduleStrokeGpuFlush();
        else if (m_strokeGpuFlushPending && !m_buffer.dirtyRect().empty())
            scheduleStrokeGpuFlush();
        schedulePreviewRefresh();
        return;
    }

    if (m_bufferDirtyEpoch != m_uploadEpochSnapshot) {
        startTiledGpuUpload(m_uploadFinishingStroke);
        return;
    }

    m_buffer.clearDirty();
    m_uploadFinishingStroke = false;

    if (m_strokeGpuFlushPending && m_strokeActive && !m_buffer.dirtyRect().empty()) {
        m_strokeGpuFlushPending = false;
        scheduleStrokeGpuFlush();
    }

    schedulePreviewRefresh();
}

void TexturePaintController::commitStrokeUndo(std::vector<uint8_t> prePixels, int layerIndex)
{
    if (prePixels.empty()) return;

    auto after = snapshotActiveLayerPixels();
    UndoManager::getSingleton()->push(
        new TexturePaintStrokeCommand(
            this,
            layerIndex,
            std::move(prePixels),
            std::move(after),
            activePaintBuffer().width(),
            activePaintBuffer().height(),
            (m_sessionEntity ? m_sessionEntity->getName() : std::string()),
            static_cast<int>(m_activeChannel)));
    m_layerStrokeBaseline = snapshotActiveLayerPixels();
}

void TexturePaintController::resetStrokePaintState()
{
    m_strokeJustBegan = true;
    m_smudgeHavePrev = false;
    // Paint v2 Slice E (#548): reset per-subset mirror-segment continuity so a
    // new stroke doesn't fan a segment from the previous stroke's last mirror UV.
    std::fill(m_mirrorHavePrevUV.begin(), m_mirrorHavePrevUV.end(), false);
    m_strokeLiveUploadStarted = false;
    m_strokeGpuFlushPending = false;
    m_strokeGpuFlushScheduled = false;
    m_uploadFinishingStroke = false;
    m_wandStrokeActive = false;
    m_strokeHavePrevUV = false;
    m_strokePathLength = 0.0f;
    m_lastStampDabPathLength = 0.0f;
    m_strokeDirSmoothed = Ogre::Vector2::ZERO;
    m_strokeHaveHitScreen = false;
    m_strokeUvPerScreenX = 0.0f;
    m_strokeUvPerScreenY = 0.0f;
    m_strokeMadeChanges = false;
    m_strokePhaseJitter = (m_rampJitter > 0.0)
        ? static_cast<float>(QRandomGenerator::global()->generateDouble() * m_rampJitter)
        : 0.0f;
}

void TexturePaintController::invalidateLayerStrokeBaseline()
{
    m_layerStrokeBaseline.clear();
}

void TexturePaintController::scheduleEmbeddedTextureCacheUpdate()
{
    if (m_embeddedCacheUpdateScheduled) return;
    m_embeddedCacheUpdateScheduled = true;
    QTimer::singleShot(300, this, [this]() {
        m_embeddedCacheUpdateScheduled = false;
        if (m_strokeActive) {
            scheduleEmbeddedTextureCacheUpdate();
            return;
        }
        updateEmbeddedTextureCache();
    });
}

bool TexturePaintController::localPointFromHitCache(const Ogre::Vector2& uv,
                                                      Ogre::Vector3& outLocal,
                                                      Ogre::Vector3& outNormal) const
{
    if (!m_hitCache.valid || !m_paintMesh) return false;
    if (m_hitCache.submesh < 0 || m_hitCache.triangle < 0) return false;
    const auto& subs = m_paintMesh->subMeshes();
    if (m_hitCache.submesh >= static_cast<int>(subs.size())) return false;
    const auto& sub = subs[static_cast<size_t>(m_hitCache.submesh)];
    if (m_hitCache.triangle >= static_cast<int>(sub.triangles.size())) return false;
    const auto& tri = sub.triangles[static_cast<size_t>(m_hitCache.triangle)];
    const auto& v0 = sub.vertices[tri.indices[0]];
    const auto& v1 = sub.vertices[tri.indices[1]];
    const auto& v2 = sub.vertices[tri.indices[2]];
    if (!v0.hasUV || !v1.hasUV || !v2.hasUV) return false;

    const Ogre::Vector2 e1 = v1.uv - v0.uv;
    const Ogre::Vector2 e2 = v2.uv - v0.uv;
    const Ogre::Vector2 dp = uv - v0.uv;
    const float denom = e1.x * e2.y - e2.x * e1.y;
    if (std::abs(denom) < 1e-10f) return false;
    const float bu = (dp.x * e2.y - e2.x * dp.y) / denom;
    const float bv = (e1.x * dp.y - dp.x * e1.y) / denom;
    const float bw = 1.0f - bu - bv;
    const float eps = 1e-3f;
    if (bu < -eps || bv < -eps || bw < -eps) return false;
    outLocal = v0.position * bw + v1.position * bu + v2.position * bv;
    Ogre::Vector3 n = (v1.position - v0.position).crossProduct(v2.position - v0.position);
    if (!n.isZeroLength()) n.normalise();
    else n = Ogre::Vector3::UNIT_Y;
    outNormal = n;
    return true;
}

bool TexturePaintController::hitTestUVForStroke(const QPoint& screenPos,
                                                OgreWidget* widget,
                                                Ogre::Vector2& outUV)
{
    if (m_strokeHaveHitScreen) {
        const int dx = screenPos.x() - m_strokeLastHitScreen.x();
        const int dy = screenPos.y() - m_strokeLastHitScreen.y();
        const int dist2 = dx * dx + dy * dy;
        // Extrapolate for small moves — avoids walking every triangle.
        if (dist2 <= 32 * 32
            && (std::abs(m_strokeUvPerScreenX) > 1e-8f
                || std::abs(m_strokeUvPerScreenY) > 1e-8f)) {
            outUV.x = m_strokeLastHitUV.x + static_cast<float>(dx) * m_strokeUvPerScreenX;
            outUV.y = m_strokeLastHitUV.y + static_cast<float>(dy) * m_strokeUvPerScreenY;
            outUV.x = std::clamp(outUV.x, 0.0f, 1.0f);
            outUV.y = std::clamp(outUV.y, 0.0f, 1.0f);
            return true;
        }
    }

    if (!hitTestUV(screenPos, widget, outUV))
        return false;

    if (m_strokeHaveHitScreen) {
        const int dx = screenPos.x() - m_strokeLastHitScreen.x();
        const int dy = screenPos.y() - m_strokeLastHitScreen.y();
        if (dx != 0 || dy != 0) {
            m_strokeUvPerScreenX = (outUV.x - m_strokeLastHitUV.x) / static_cast<float>(dx);
            m_strokeUvPerScreenY = (outUV.y - m_strokeLastHitUV.y) / static_cast<float>(dy);
        }
    } else {
        m_strokeUvPerScreenX = 0.0f;
        m_strokeUvPerScreenY = 0.0f;
    }
    m_strokeLastHitScreen = screenPos;
    m_strokeLastHitUV = outUV;
    m_strokeHaveHitScreen = true;
    return true;
}

void TexturePaintController::processPendingStrokeUpdate()
{
    if (!m_strokeActive || !m_paintEnabled) return;
    OgreWidget* widget = m_pendingStrokeWidget;
    const QPoint screenPos = m_pendingStrokePos;

    if (m_tool == ToolSmartSelect && m_wandStrokeActive) {
        if (widget) {
            int vw = 0, vh = 0;
            widget->pixelSizeForCameraPicking(vw, vh);
            const int viewportW = vw > 0 ? vw : 800;
            const double dx = static_cast<double>(screenPos.x() - m_wandStartScreenPos.x());
            const double t = std::clamp(m_wandStartTolerance + dx / static_cast<double>(viewportW),
                                        0.0, 1.0);
            if (std::abs(t - m_smartSelectTolerance) > 1e-4) {
                m_smartSelectTolerance = t;
                emit smartSelectChanged();
                smartSelectAtUV(static_cast<double>(m_wandSeedUV.x),
                                static_cast<double>(m_wandSeedUV.y),
                                /*mode=*/0);
            }
        }
        return;
    }

    if (m_target == TargetVertex) {
        if (!m_paintMesh || !m_paintMeshEntity) return;
        Ogre::Vector3 localPos, localNormal;
        if (!hitTestLocalPoint(widget, screenPos, localPos, localNormal)) return;
        drawHoverRingAt(localPos, localNormal);
        const QColor c = texturePaintColor();
        const Ogre::ColourValue paint(c.redF(), c.greenF(), c.blueF(), c.alphaF());
        const auto* em = EditModeController::instance();
        const bool square = em && em->vertexPaintShape() == EditModeController::ShapeSquare;
        const bool changed = EditModeController::applyVertexColorBrush(
            *m_paintMesh, localPos,
            static_cast<float>(texturePaintRadius()),
            paint,
            static_cast<float>(texturePaintStrength()),
            static_cast<float>(texturePaintFalloff()),
            square);
        if (changed)
            m_paintMesh->commitVertexColorsToEntity(m_paintMeshEntity);
        return;
    }

    Ogre::Vector2 uv;
    if (!hitTestUVForStroke(screenPos, widget, uv))
        return;

    bool changed = false;
    const bool canSegment =
        m_strokeHavePrevUV
        && m_tool != ToolFill
        && m_tool != ToolColorPicker
        && m_tool != ToolSmartSelect;
    if (canSegment)
        changed = paintBrushAlongSegment(m_strokePrevUV, uv);
    else
        changed = applyBrushAtUV(uv);

    // Paint v2 Slice E (#548): mirror this dab across the enabled symmetry axes
    // (in the same buffer, inside the begin/end window → one undo step).
    // Excluded tools: ColorPicker/SmartSelect (not paint ops); Fill (the primary
    // consumes the single-stamp m_strokeJustBegan flag, so mirrored fills would
    // no-op — a correct per-target mirrored flood-fill is a follow-up); Smudge
    // (shares m_smudgePrev — a mirror dab would sample its delta from the distant
    // primary UV and corrupt the next primary dab; independent per-path history
    // is a follow-up). #548 review.
    if (m_symmetryEnabled && m_symmetryAxes != SymAxisNone
        && m_tool != ToolColorPicker && m_tool != ToolSmartSelect
        && m_tool != ToolFill && m_tool != ToolSmudge) {
        applyBrushSymmetryDabs(uv);
    }

    if (changed || m_strokeMadeChanges) {
        m_strokePrevUV = uv;
        m_strokeHavePrevUV = true;
        m_strokeMadeChanges = true;
        if (m_layerStack.layerCount() <= 0)
            ++m_bufferDirtyEpoch;
        flushDirtyToOgre();
    }
}

void TexturePaintController::processPendingStrokeUpdateUV()
{
    if (!m_strokeActive || !m_paintEnabled) return;
    const Ogre::Vector2 uv(static_cast<float>(m_pendingStrokeU),
                           static_cast<float>(m_pendingStrokeV));

    if (m_tool == ToolSmartSelect && m_wandStrokeActive) {
        const double pressU = static_cast<double>(m_wandStartScreenPos.x()) / 10000.0;
        const double du = m_pendingStrokeU - pressU;
        const double t = std::clamp(m_wandStartTolerance + du, 0.0, 1.0);
        if (std::abs(t - m_smartSelectTolerance) > 1e-4) {
            m_smartSelectTolerance = t;
            emit smartSelectChanged();
            smartSelectAtUV(static_cast<double>(m_wandSeedUV.x),
                            static_cast<double>(m_wandSeedUV.y),
                            /*mode=*/0);
        }
        return;
    }

    bool changed = false;
    const bool canSegment =
        m_strokeHavePrevUV
        && m_tool != ToolFill
        && m_tool != ToolColorPicker
        && m_tool != ToolSmartSelect;
    if (canSegment)
        changed = paintBrushAlongSegment(m_strokePrevUV, uv);
    else
        changed = applyBrushAtUV(uv);

    // Paint v2 Slice E (#548): mirror this dab across the enabled symmetry axes
    // (in the same buffer, inside the begin/end window → one undo step).
    // Excluded tools: ColorPicker/SmartSelect (not paint ops); Fill (the primary
    // consumes the single-stamp m_strokeJustBegan flag, so mirrored fills would
    // no-op — a correct per-target mirrored flood-fill is a follow-up); Smudge
    // (shares m_smudgePrev — a mirror dab would sample its delta from the distant
    // primary UV and corrupt the next primary dab; independent per-path history
    // is a follow-up). #548 review.
    if (m_symmetryEnabled && m_symmetryAxes != SymAxisNone
        && m_tool != ToolColorPicker && m_tool != ToolSmartSelect
        && m_tool != ToolFill && m_tool != ToolSmudge) {
        applyBrushSymmetryDabs(uv);
    }

    if (changed || m_strokeMadeChanges) {
        m_strokePrevUV = uv;
        m_strokeHavePrevUV = true;
        m_strokeMadeChanges = true;
        if (m_layerStack.layerCount() <= 0)
            ++m_bufferDirtyEpoch;
        flushDirtyToOgre();
    }
}

void TexturePaintController::doFlushDirtyToOgre(bool immediate)
{
    const auto& dirty = m_buffer.dirtyRect();
    if (dirty.empty()) return;

    // Preferred path: paint directly INTO the model's original
    // texture. No rebind needed — the existing material binding
    // continues to work, and the paint just modifies pixels in place.
    if (!m_forceManualPaintTexture && !m_originalTextureName.isEmpty()) {
        try {
            auto fresh = Ogre::TextureManager::getSingleton().getByName(
                m_originalTextureName.toStdString(),
                Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
            if (fresh) m_originalTexture = fresh;
        } catch (...) {}
    }
    // Skip the in-place path once we've rebound the model to the
    // manual paint texture — the original is no longer what the
    // renderer samples, so blitting to it would be invisible.
    // During strokes always use the manual paint texture path — in-place
    // blits into imported textures stall the GL driver even for partial
    // rects, and format conversion is worse still.
    const bool skipInPlaceDuringStroke = m_strokeActive;
    if (!m_forceManualPaintTexture && !skipInPlaceDuringStroke
        && m_boundSlots.empty() && m_originalTexture) {
        try {
            auto pixbuf = m_originalTexture->getBuffer();
            if (pixbuf) {
                const int W = m_buffer.width();
                const int rectW = dirty.width();
                const int rectH = dirty.height();
                const Ogre::PixelFormat dstFmt = m_originalTexture->getFormat();
                if (!isPlainUncompressedFormat(dstFmt)) {
                    SentryReporter::addBreadcrumb("ui.action",
                        QStringLiteral("Texture paint: in-place skipped — compressed format %1")
                            .arg(static_cast<int>(dstFmt)));
                    m_originalTexture.reset();
                    m_forceManualPaintTexture = true;
                } else {
                    std::vector<uint8_t> srcRow(static_cast<size_t>(rectW) * static_cast<size_t>(rectH) * 4u);
                    const auto& src = m_buffer.data();
                    for (int row = 0; row < rectH; ++row) {
                        const size_t srcOff = (static_cast<size_t>(dirty.y0 + row) * static_cast<size_t>(W)
                                               + static_cast<size_t>(dirty.x0)) * 4u;
                        const size_t dstOff = static_cast<size_t>(row) * static_cast<size_t>(rectW) * 4u;
                        std::memcpy(srcRow.data() + dstOff, src.data() + srcOff,
                                    static_cast<size_t>(rectW) * 4u);
                    }
                    Ogre::PixelBox srcRgba(rectW, rectH, 1, Ogre::PF_BYTE_RGBA, srcRow.data());
                    if (dstFmt == Ogre::PF_BYTE_RGBA) {
                        Ogre::Box dst(dirty.x0, dirty.y0, dirty.x1, dirty.y1);
                        pixbuf->blitFromMemory(srcRgba, dst);
                    } else {
                        const size_t dstBytes = Ogre::PixelUtil::getMemorySize(rectW, rectH, 1, dstFmt);
                        std::vector<uint8_t> slice(dstBytes);
                        Ogre::PixelBox dstPb(rectW, rectH, 1, dstFmt, slice.data());
                        Ogre::PixelUtil::bulkPixelConversion(srcRgba, dstPb);
                        Ogre::Box dst(dirty.x0, dirty.y0, dirty.x1, dirty.y1);
                        pixbuf->blitFromMemory(dstPb, dst);
                    }
                    m_useOriginalTexture = true;
                    if (!m_loggedInPlaceBlit) {
                        m_loggedInPlaceBlit = true;
                        SentryReporter::addBreadcrumb("ui.action",
                            QStringLiteral("Texture paint: in-place blit fmt=%1 size=%2x%3")
                                .arg(static_cast<int>(dstFmt))
                                .arg(m_originalTexture->getWidth())
                                .arg(m_originalTexture->getHeight()));
                    }
                    m_buffer.clearDirty();
                    schedulePreviewRefresh();
                    return;
                }
            }
        } catch (const Ogre::Exception& e) {
            SentryReporter::addBreadcrumb("ui.action",
                QStringLiteral("Texture paint: in-place blit FAILED → fallback to manual texture (%1)")
                    .arg(QString::fromStdString(e.getDescription())));
            m_originalTexture.reset();
            m_forceManualPaintTexture = true;
        } catch (...) {
            m_originalTexture.reset();
            m_forceManualPaintTexture = true;
        }
    }

    if (!m_ogreTexture) return;

    // Fallback path: upload into our manual paint texture. Rebind once
    // (scheduled at session create) so the viewport samples it.
    if (m_boundSlots.empty() && m_paintMeshEntity)
        scheduleRebindToPaintTexture(m_paintMeshEntity);

    GpuUploadPacket packet;
    packet.bufferW = m_buffer.width();
    packet.bufferH = m_buffer.height();
    packet.x0 = dirty.x0;
    packet.y0 = dirty.y0;
    packet.x1 = dirty.x1;
    packet.y1 = dirty.y1;
    const int rectW = dirty.width();
    const int rectH = dirty.height();
    packet.partial =
        rectW > 0 && rectH > 0
        && static_cast<size_t>(rectW) * static_cast<size_t>(rectH)
               < static_cast<size_t>(packet.bufferW) * static_cast<size_t>(packet.bufferH);
#if defined(Q_OS_MACOS)
    packet.partial = false;
#endif
    const size_t copyBytes =
        packet.partial
            ? static_cast<size_t>(rectW) * static_cast<size_t>(rectH) * 4u
            : static_cast<size_t>(packet.bufferW) * static_cast<size_t>(packet.bufferH) * 4u;
    packet.rgba.resize(copyBytes);
    const auto& src = m_buffer.data();
    if (packet.partial) {
        for (int row = 0; row < rectH; ++row) {
            const size_t srcOff = (static_cast<size_t>(dirty.y0 + row) * static_cast<size_t>(packet.bufferW)
                                   + static_cast<size_t>(dirty.x0)) * 4u;
            const size_t dstOff = static_cast<size_t>(row) * static_cast<size_t>(rectW) * 4u;
            std::memcpy(packet.rgba.data() + dstOff, src.data() + srcOff,
                        static_cast<size_t>(rectW) * 4u);
        }
    } else {
        std::memcpy(packet.rgba.data(), src.data(), copyBytes);
    }
    m_buffer.clearDirty();

    auto blitPacket = [this](GpuUploadPacket packet) {
        if (!m_ogreTexture) return;
        try {
            auto buf = m_ogreTexture->getBuffer();
            if (!buf) return;
            if (packet.partial) {
                const int rectW = packet.x1 - packet.x0;
                const int rectH = packet.y1 - packet.y0;
                Ogre::PixelBox pb(rectW, rectH, 1, Ogre::PF_BYTE_RGBA, packet.rgba.data());
                Ogre::Box dst(packet.x0, packet.y0, packet.x1, packet.y1);
                buf->blitFromMemory(pb, dst);
            } else {
                Ogre::PixelBox pb(packet.bufferW, packet.bufferH, 1,
                                  Ogre::PF_BYTE_RGBA, packet.rgba.data());
                buf->blitFromMemory(pb);
            }
        } catch (const Ogre::Exception& e) {
            SentryReporter::addBreadcrumb("ui.action",
                QStringLiteral("Texture paint: blit failed — %1")
                    .arg(QString::fromStdString(e.getDescription())));
        }
        schedulePreviewRefresh();
    };

    if (immediate) {
        blitPacket(std::move(packet));
        return;
    }

    // Yield one event-loop tick before the GPU blit so rapid mouse-move
    // events can be processed (the pixel copy above is already done).
    QTimer::singleShot(0, this, [blitPacket, packet = std::move(packet)]() mutable {
        blitPacket(std::move(packet));
    });
}

bool TexturePaintController::tryHitTestCachedTriangle(const Ogre::Vector3& localOrigin,
                                                      const Ogre::Vector3& localDir,
                                                      Ogre::Vector2& outUV) const
{
    if (!m_hitCache.valid || !m_paintMesh) return false;
    if (m_hitCache.submesh < 0 || m_hitCache.triangle < 0) return false;
    const auto& subs = m_paintMesh->subMeshes();
    if (m_hitCache.submesh >= static_cast<int>(subs.size())) return false;
    const auto& sub = subs[static_cast<size_t>(m_hitCache.submesh)];
    if (m_hitCache.triangle >= static_cast<int>(sub.triangles.size())) return false;
    const auto& tri = sub.triangles[static_cast<size_t>(m_hitCache.triangle)];
    const auto& v0 = sub.vertices[tri.indices[0]];
    const auto& v1 = sub.vertices[tri.indices[1]];
    const auto& v2 = sub.vertices[tri.indices[2]];
    if (!v0.hasUV || !v1.hasUV || !v2.hasUV) return false;

    const Ogre::Vector3 e1 = v1.position - v0.position;
    const Ogre::Vector3 e2 = v2.position - v0.position;
    const Ogre::Vector3 pvec = localDir.crossProduct(e2);
    const Ogre::Real det = e1.dotProduct(pvec);
    if (std::abs(det) < 1e-8f) return false;
    const Ogre::Real invDet = 1.0f / det;
    const Ogre::Vector3 tvec = localOrigin - v0.position;
    const Ogre::Real u = tvec.dotProduct(pvec) * invDet;
    if (u < 0.0f || u > 1.0f) return false;
    const Ogre::Vector3 qvec = tvec.crossProduct(e1);
    const Ogre::Real v = localDir.dotProduct(qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;
    const Ogre::Real tHit = e2.dotProduct(qvec) * invDet;
    if (tHit <= 0.0f) return false;

    const Ogre::Real w = 1.0f - u - v;
    outUV = v0.uv * w + v1.uv * u + v2.uv * v;
    return true;
}

bool TexturePaintController::hitTestUV(const QPoint& screenPos, OgreWidget* widget, Ogre::Vector2& outUV) const
{
    if (!m_paintMesh || !m_paintMeshEntity || !widget) return false;
    auto* mesh = m_paintMesh.get();
    auto* entity = m_paintMeshEntity;

    auto* spaceCam = widget->getSpaceCamera();
    auto* camera = spaceCam ? spaceCam->getCamera() : nullptr;
    if (!camera) return false;

    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return false;

    const Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / vw;
    const Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / vh;
    const Ogre::Ray ray = camera->getCameraToViewportRay(nx, ny);

    Ogre::SceneNode* node = entity->getParentSceneNode();
    Ogre::Affine3 worldToLocal = node ? node->_getFullTransform().inverse() : Ogre::Affine3::IDENTITY;
    Ogre::Vector3 localOrigin = worldToLocal * ray.getOrigin();
    Ogre::Vector3 localDir = worldToLocal.linear() * ray.getDirection();
    localDir.normalise();

    // Fast path: while dragging along the same surface patch, re-test
    // only the last hit triangle instead of walking the whole mesh.
    if (m_hitCache.valid) {
        const int dx = screenPos.x() - m_hitCache.screenPos.x();
        const int dy = screenPos.y() - m_hitCache.screenPos.y();
        if (dx * dx + dy * dy <= 24 * 24) {
            if (tryHitTestCachedTriangle(localOrigin, localDir, outUV)) {
                m_hitCache.screenPos = screenPos;
                return true;
            }
        }
    }

    // Walk every triangle and keep the closest hit.
    Ogre::Real bestT = std::numeric_limits<Ogre::Real>::infinity();
    Ogre::Vector2 bestUV(0, 0);
    bool found = false;
    int hitSubmesh = -1;
    int hitTriangle = -1;
    int subIdx = 0;
    for (const auto& sub : mesh->subMeshes()) {
        for (size_t ti = 0; ti < sub.triangles.size(); ++ti) {
            const auto& tri = sub.triangles[ti];
            const auto& v0 = sub.vertices[tri.indices[0]];
            const auto& v1 = sub.vertices[tri.indices[1]];
            const auto& v2 = sub.vertices[tri.indices[2]];
            // Möller–Trumbore for explicit barycentric coords.
            const Ogre::Vector3 e1 = v1.position - v0.position;
            const Ogre::Vector3 e2 = v2.position - v0.position;
            const Ogre::Vector3 pvec = localDir.crossProduct(e2);
            const Ogre::Real det = e1.dotProduct(pvec);
            if (std::abs(det) < 1e-8f) continue;
            const Ogre::Real invDet = 1.0f / det;
            const Ogre::Vector3 tvec = localOrigin - v0.position;
            const Ogre::Real u = tvec.dotProduct(pvec) * invDet;
            if (u < 0.0f || u > 1.0f) continue;
            const Ogre::Vector3 qvec = tvec.crossProduct(e1);
            const Ogre::Real v = localDir.dotProduct(qvec) * invDet;
            if (v < 0.0f || u + v > 1.0f) continue;
            const Ogre::Real tHit = e2.dotProduct(qvec) * invDet;
            if (tHit <= 0.0f || tHit >= bestT) continue;
            if (!v0.hasUV || !v1.hasUV || !v2.hasUV) continue;
            const Ogre::Real w = 1.0f - u - v;
            bestT = tHit;
            bestUV = v0.uv * w + v1.uv * u + v2.uv * v;
            hitSubmesh = subIdx;
            hitTriangle = static_cast<int>(ti);
            found = true;
        }
        ++subIdx;
    }
    if (!found) {
        m_hitCache.valid = false;
        return false;
    }
    m_hitCache.submesh = hitSubmesh;
    m_hitCache.triangle = hitTriangle;
    m_hitCache.screenPos = screenPos;
    m_hitCache.valid = true;
    outUV = bestUV;
    return true;
}

bool TexturePaintController::beginStroke(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_paintEnabled || m_strokeActive) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: beginStroke skipped (enabled=%1 active=%2)")
                .arg(m_paintEnabled).arg(m_strokeActive));
        return false;
    }
    if (m_target == TargetVertex) {
        // Vertex paint just needs the EditableMesh built — no GPU
        // texture, no rebind. Build it now if it isn't ready.
        if (auto* e = activeEntity())
            ensureEditableMesh(e);
        if (!m_paintMesh || !m_paintMeshEntity) {
            SentryReporter::addBreadcrumb("ui.action",
                "Vertex paint: beginStroke aborted — no mesh");
            return false;
        }
        m_paintMesh->ensureVertexColorBuffers(m_paintMeshEntity);
    } else {
        // Tear down a stale session if the selection moved to a
        // different entity since the session was created. Without
        // this, strokes hit-test against the new mesh's geometry but
        // write into the old mesh's texture/session state. Codex P1.
        auto* curEntity = activeEntity();
        if (hasActiveSession() && m_sessionEntity && curEntity
            && m_sessionEntity != curEntity) {
            SentryReporter::addBreadcrumb("ui.action",
                "Texture paint: selection changed — rebuilding session for new entity");
            closeSession();
        }
        if (!hasActiveSession()) {
            if (!ensurePaintableTexture(m_buffer.width() > 0 ? m_buffer.width() : 1024)) {
                SentryReporter::addBreadcrumb("ui.action",
                    "Texture paint: beginStroke aborted — no session could be created");
                return false;
            }
        }
    }
    if (m_tiledUploadRunning)
        cancelInFlightGpuUpload();
    ++m_gpuUploadGeneration;
    ++m_strokeUndoGeneration;
    m_strokeActive = true;
    m_strokeFromUvPreview = false;
    m_strokePreSnapshot.clear();
    resetStrokePaintState();
    m_wandStartScreenPos = screenPos;
    m_hitCache.valid = false;

    // Paint v2 Slice F (#549): a projection stroke caches the mesh's world
    // triangles once, and (mode 1, unlocked) refreshes the occlusion depth map
    // from the current camera at stroke start — never per dab.
    if (m_projectionMode == 1 || m_projectionMode == 2) {
        m_projTris.clear(); m_haveProjTris = false; ensureProjTris();
        if (m_projectionMode == 1 && (m_projUseOcclusion || m_projDepthLimit > 0.0)) {
            ProjectionPainter::View v;
            if (currentProjectionView(widget, v))
                m_haveProjOcc = buildOcclusionForView(v, m_projOcc);
        }
    }

    if (m_colorSource == ColorGradient) {
        SentryReporter::addBreadcrumb(
            "paint.brush.gradient",
            QStringLiteral("mode=%1 ramp=%2")
                .arg(static_cast<int>(m_gradientMode))
                .arg(m_useFgBgRamp ? QStringLiteral("FG/BG") : m_activeRampName));
    }
    if (m_footprintType == BrushFootprint::FootprintType::StampImage
        || m_footprintType == BrushFootprint::FootprintType::TilingSource) {
        SentryReporter::addBreadcrumb(
            "paint.brush.stamp",
            QStringLiteral("footprint=%1 stamp=%2 tiling=%3")
                .arg(static_cast<int>(m_footprintType))
                .arg(m_activeStampName)
                .arg(m_activeTilingName));
    }
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Paint stroke begin (target=%1 tool=%2 radius=%3 strength=%4 color=%5)")
            .arg(m_target == TargetVertex ? "vertex" : "texture")
            .arg(static_cast<int>(m_tool))
            .arg(texturePaintRadius(), 0, 'f', 3)
            .arg(texturePaintStrength(), 0, 'f', 3)
            .arg(texturePaintColor().name(QColor::HexRgb)));
    if (m_target == TargetTexture) {
        if (m_layerStrokeBaseline.empty())
            m_layerStrokeBaseline = snapshotActiveLayerPixels();
        m_strokePreSnapshot = std::move(m_layerStrokeBaseline);
    }
    if (m_target == TargetTexture && m_paintMeshEntity && m_ogreTexture
        && m_boundSlots.empty()) {
        rebindEntityDiffuseToPaintTexture(m_paintMeshEntity);
    }
    // Paint v2 Slice E (#548): seed the stabilizer with the press point so the
    // first dab is exact (no start-of-stroke lag), and log once per stroke.
    m_stabSamples.clear();
    m_stabHaveTrail = false;
    m_stabHaveLastRaw = false;
    m_stabTrailPos = QPointF(screenPos);
    if (m_stabilizerAmount > 0.0) {
        SentryReporter::addBreadcrumb("paint.stabilizer",
            QStringLiteral("mode=%1 amount=%2")
                .arg(static_cast<int>(m_stabilizerMode))
                .arg(m_stabilizerAmount, 0, 'f', 0));
    }
    m_pendingStrokeWidget = widget;
    m_pendingStrokePos = screenPos;   // first dab uses the exact press point
    processPendingStrokeUpdate();
    return true;
}

void TexturePaintController::updateStroke(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_strokeActive || !m_paintEnabled) return;
    m_pendingStrokeWidget = widget;
    // Paint v2 Slice E (#548): smooth the raw cursor before hit-testing.
    // amount==0 is an exact passthrough (zero latency).
    const QPointF stab = stabilizeScreen(QPointF(screenPos));
    m_pendingStrokePos = QPoint(static_cast<int>(std::lround(stab.x())),
                                static_cast<int>(std::lround(stab.y())));
    processPendingStrokeUpdate();
}

bool TexturePaintController::applyBrushAtUV(const Ogre::Vector2& uv)
{
    if (m_buffer.width() <= 0) return false;
    if (m_layerStack.layerCount() > 0 && m_layerStack.activeLayer().locked)
        return false;
    const float radius = brushRadiusUV();
    const float strength = static_cast<float>(texturePaintStrength());
    const float falloff = static_cast<float>(texturePaintFalloff());
    // Shape is sourced from EditModeController (the canonical brush
    // settings owner). Default Round = circular falloff; Square is
    // an axis-aligned constant-strength rectangle for pixel-art
    // style hard edges.
    const auto* em = EditModeController::instance();
    const TexturePaintBuffer::BrushShape shape =
        (em && em->vertexPaintShape() == EditModeController::ShapeSquare)
            ? TexturePaintBuffer::BrushShape::Square
            : TexturePaintBuffer::BrushShape::Round;

    switch (m_tool) {
    case ToolPaint:
        return paintColorFootprintAtUV(uv, radius, strength);
    case ToolErase: {
        // Erase = paint with the user-chosen background color. The BG
        // color is part of the FG/BG color pair (Photoshop / GIMP /
        // Krita model). When BG is fully transparent (alpha 0) this
        // matches the old "erase to transparent" behaviour; otherwise
        // it lays down a solid replacement color — much more useful
        // for actually masking out parts of a texture.
        const QColor bg = bgPaintColor();
        const Ogre::ColourValue eraseTo(
            static_cast<float>(bg.redF()),
            static_cast<float>(bg.greenF()),
            static_cast<float>(bg.blueF()),
            static_cast<float>(bg.alphaF()));
        return activePaintBuffer().paintBrush(uv, radius, eraseTo, strength, falloff, shape) > 0;
    }
    case ToolFill: {
        // Fill is a single-stamp operation — apply once per stroke
        // start, not on every move. Most paint apps work this way.
        // Suppress repeats by only flooding when the stroke just began.
        if (!m_strokeJustBegan) return false;
        m_strokeJustBegan = false;
        return floodFillAtUV(uv);
    }
    case ToolColorPicker: {
        if (!m_strokeJustBegan) return false;
        m_strokeJustBegan = false;
        pickColorAtUV(uv);
        return false;
    }
    case ToolSmudge: {
        // Sample previous stamp's pixels and bias toward them.
        if (!m_smudgeHavePrev) {
            m_smudgePrev = uv;
            m_smudgeHavePrev = true;
            return false;
        }
        // Walk pixels in the brush footprint; for each, sample at
        // (uv + (prev - uv) * smudgeAmount) and lerp toward it.
        const int W = m_buffer.width();
        const int H = m_buffer.height();
        const float radiusXf = radius * W;
        const float radiusYf = radius * H;
        const float cxF = uv.x * W;
        const float cyF = uv.y * H;
        const int x0 = std::max(0, static_cast<int>(std::floor(cxF - radiusXf)));
        const int x1 = std::min(W, static_cast<int>(std::ceil (cxF + radiusXf)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cyF - radiusYf)));
        const int y1 = std::min(H, static_cast<int>(std::ceil (cyF + radiusYf)));
        if (x0 >= x1 || y0 >= y1) return false;
        const float dxUV = m_smudgePrev.x - uv.x;
        const float dyUV = m_smudgePrev.y - uv.y;
        const float p = 1.0f + falloff * 3.0f;
        const float invRx = 1.0f / std::max(radiusXf, 1e-6f);
        const float invRy = 1.0f / std::max(radiusYf, 1e-6f);
        bool changed = false;
        int touchedX0 = x1, touchedX1 = x0, touchedY0 = y1, touchedY1 = y0;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const float dx = (x + 0.5f - cxF) * invRx;
                const float dy = (y + 0.5f - cyF) * invRy;
                const float r2 = dx * dx + dy * dy;
                if (r2 >= 1.0f) continue;
                const float w = std::pow(1.0f - r2, p);
                const float blend = strength * w;
                if (blend <= 0.0f) continue;
                const Ogre::Vector2 sampleUV(
                    (x + 0.5f) / W + dxUV,
                    (y + 0.5f) / H + dyUV);
                int sx = 0, sy = 0;
                m_buffer.uvToPixel(sampleUV, sx, sy);
                const auto src = m_buffer.pixel(sx, sy);
                const auto dst = activePaintBuffer().pixel(x, y);
                if (src.a <= 0.0f) continue;
                const Ogre::ColourValue blended(
                    dst.r + (src.r - dst.r) * blend,
                    dst.g + (src.g - dst.g) * blend,
                    dst.b + (src.b - dst.b) * blend,
                    dst.a + (src.a - dst.a) * blend);
                activePaintBuffer().setPixel(x, y, blended);
                changed = true;
                touchedX0 = std::min(touchedX0, x);
                touchedY0 = std::min(touchedY0, y);
                touchedX1 = std::max(touchedX1, x + 1);
                touchedY1 = std::max(touchedY1, y + 1);
            }
        }
        m_smudgePrev = uv;
        return changed;
    }
    case ToolSmartSelect: {
        // Press = seed the selection at uv. Subsequent moves don't
        // re-seed — instead they nudge the tolerance and re-run the
        // select at the press seed. The on-move tolerance update
        // happens in updateStroke / updateStrokeUV because we need
        // the screen / UV delta to compute the scrub; this case
        // handles the press-time seed only.
        //
        // Don't clobber the user-configured tolerance — the default
        // lives in the member initializer in the header. Just
        // remember it as the wand-stroke baseline so drag deltas
        // are added relative to where the user started.
        if (!m_strokeJustBegan) return false;
        m_strokeJustBegan = false;
        m_wandStrokeActive = true;
        m_wandSeedUV = uv;
        m_wandStartTolerance = m_smartSelectTolerance;
        smartSelectAtUV(static_cast<double>(uv.x), static_cast<double>(uv.y), /*mode=*/0);
        return false;  // smart-select doesn't dirty pixels
    }
    case ToolDecal:
        // Paint v2 Slice F (#549): the decal tool paints nothing per-dab — the
        // world-anchored quad is placed/dragged via the DecalSession and only
        // rasterizes once, on commit. Explicit case so -Wswitch stays clean.
        return false;
    }
    return false;
}

bool TexturePaintController::floodFillAtUV(const Ogre::Vector2& uv)
{
    int sx = 0, sy = 0;
    activePaintBuffer().uvToPixel(uv, sx, sy);
    const QColor c = texturePaintColor();
    const Ogre::ColourValue fill(c.redF(), c.greenF(), c.blueF(), c.alphaF());
    return activePaintBuffer().floodFill(sx, sy, fill) > 0;
}

void TexturePaintController::pickColorAtUV(const Ogre::Vector2& uv)
{
    int x = 0, y = 0;
    m_buffer.uvToPixel(uv, x, y);
    const auto c = m_buffer.pixel(x, y);
    QColor qc;
    qc.setRgbF(c.r, c.g, c.b, 1.0f);
    if (auto* em = EditModeController::instance())
        em->setVertexPaintColor(qc);
}

void TexturePaintController::endStroke()
{
    if (!m_strokeActive) return;

    // Wand-drag stroke never dirties pixels, so the post-snapshot
    // diff below would be a no-op; just clear the wand flags and bail.
    if (m_wandStrokeActive) {
        m_wandStrokeActive = false;
        m_strokePreSnapshot.clear();
        m_strokeActive = false;
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Wand stroke end (final tolerance=%1)")
                .arg(m_smartSelectTolerance, 0, 'f', 3));
        return;
    }
    if (m_target == TargetVertex) {
        m_strokePreSnapshot.clear();
        m_strokeActive = false;
        SentryReporter::addBreadcrumb("ui.action", "Vertex paint stroke end");
        return;
    }

    // Paint v2 Slice E (#548): stabilizer catch-up. The smoothed cursor lags the
    // true cursor; before ending, drive the brush the rest of the way to the
    // real last position so the stroke terminates where the user released
    // (Krita behaviour). SYNCHRONOUS — still inside the begin/end window, so the
    // one deferred undo command below captures these final dabs too. Bypass the
    // stabilizer for this final dab (paint exactly at the true cursor).
    if (m_stabilizerAmount > 0.0 && m_stabHaveLastRaw && m_pendingStrokeWidget
        && !m_strokeFromUvPreview) {
        const double savedAmount = m_stabilizerAmount;
        m_stabilizerAmount = 0.0;                 // passthrough for the final dab
        m_pendingStrokePos = QPoint(static_cast<int>(std::lround(m_stabLastRaw.x())),
                                    static_cast<int>(std::lround(m_stabLastRaw.y())));
        processPendingStrokeUpdate();
        m_stabilizerAmount = savedAmount;
    }

    m_strokeActive = false;

    recomposePaintBufferIfNeeded();
    if (!flushLiveStrokeToGpu())
        doFlushDirtyToOgre(/*immediate=*/true);
    if (m_buffer.dirtyRect().empty())
        m_buffer.clearDirty();
    m_previewRefreshScheduled = false;
    refreshPreviewUri();
    if (m_layerStack.layerCount() > 0) {
        ++m_layerPreviewVersion;
        emit layersChanged();
    }

    std::vector<uint8_t> undoPre = std::move(m_strokePreSnapshot);
    const int undoLayer =
        m_layerStack.layerCount() > 0 ? m_layerStack.activeIndex() : 0;
    const bool strokeMadeChanges = m_strokeMadeChanges;
    m_strokeFromUvPreview = false;
    m_strokePreSnapshot.clear();
    const uint64_t undoGen = m_strokeUndoGeneration;
    QTimer::singleShot(0, this, [this, undoGen, undoLayer, strokeMadeChanges,
                                 pre = std::move(undoPre)]() mutable {
        if (undoGen != m_strokeUndoGeneration) return;
        if (strokeMadeChanges)
            commitStrokeUndo(std::move(pre), undoLayer);
        else
            m_layerStrokeBaseline = std::move(pre);
        scheduleEmbeddedTextureCacheUpdate();
    });
}

void TexturePaintController::updateEmbeddedTextureCache()
{
    if (m_target != TargetTexture) return;
    if (m_originalTextureName.isEmpty()) return;
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0) return;
    try {
        QImage img(const_cast<uchar*>(m_buffer.data().data()),
                   m_buffer.width(), m_buffer.height(),
                   m_buffer.width() * 4, QImage::Format_RGBA8888);
        QByteArray bytes;
        QBuffer qbuf(&bytes);
        qbuf.open(QIODevice::WriteOnly);
        if (img.save(&qbuf, "PNG")) {
            std::vector<uint8_t> v(bytes.begin(), bytes.end());
            EmbeddedTextureCache::store(
                m_originalTextureName.toStdString(), v);
        }
    } catch (...) {}
}

std::vector<uint8_t> TexturePaintController::snapshotPixels() const
{
    return m_buffer.data();
}

bool TexturePaintController::savePaintBuffer(const QString& path) const
{
    if (path.isEmpty()) return false;
    if (!hasActiveSession()) return false;
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    return m_buffer.save(path.toStdString());
}

bool TexturePaintController::loadPaintBuffer(const QString& path)
{
    if (path.isEmpty()) return false;
    if (!m_buffer.load(path.toStdString())) return false;
    m_layerStack.initFromFlatBuffer(m_buffer);
    recomposeComposite(/*fullBuffer=*/true);
    auto* entity = activeEntity();
    if (entity) {
        QFileInfo fi(path);
        QString hint = QStringLiteral("QMEPaintLoad_%1").arg(fi.completeBaseName());
        m_sessionEntity = entity;
        // Re-create the Ogre texture at the new resolution. The user
        // explicitly loaded a new image — rebind immediately so they
        // see it on the model.
        m_ogreTexture.reset();
        if (!createOgreTextureFromBuffer(entity, hint, /*rebindToModel=*/true)) return false;
    }
    // Refresh the 2D preview panel — the buffer just got rewritten
    // with the loaded image's pixels.
    refreshPreviewUri();
    emit sessionChanged();
    emit layersChanged();
    return true;
}

QString TexturePaintController::savePaintBufferInteractive()
{
    if (!hasActiveSession()) {
        SentryReporter::addBreadcrumb("ui.action",
            "Texture paint: save dialog skipped (no session)");
        return QString();
    }
    QApplication::processEvents();
    QWidget* parent = QApplication::activeWindow();
    const QString path = QFileDialog::getSaveFileName(
        parent,
        QStringLiteral("Save painted texture"),
        QDir::currentPath() + "/paint.png",
        QStringLiteral("PNG image (*.png);;JPEG image (*.jpg *.jpeg);;TGA image (*.tga);;BMP image (*.bmp)"),
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons);
    if (path.isEmpty()) return QString();
    if (!savePaintBuffer(path)) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: save failed (%1)").arg(path));
        return QString();
    }
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint: saved to %1").arg(path));
    return path;
}

void TexturePaintController::pickBrushColorInteractive()
{
    auto* em = EditModeController::instance();
    if (!em) return;
    // DontUseNativeDialog: native pickers deadlock / freeze against Ogre's
    // GL surface on Linux (and occasionally macOS). Same fix as the
    // background-color picker in MainWindow.
    QWidget* parent = QApplication::activeWindow();
    const QColor picked = QColorDialog::getColor(
        em->vertexPaintColor(), parent, QStringLiteral("Brush color"),
        QColorDialog::ShowAlphaChannel | QColorDialog::DontUseNativeDialog);
    if (picked.isValid())
        em->setVertexPaintColor(picked);
}

void TexturePaintController::setBrushRadius(double r)
{
    if (auto* em = EditModeController::instance()) em->setVertexPaintRadius(r);
}

void TexturePaintController::setBrushStrength(double s)
{
    if (auto* em = EditModeController::instance()) em->setVertexPaintStrength(s);
}

void TexturePaintController::setBrushFalloff(double f)
{
    if (auto* em = EditModeController::instance()) em->setVertexPaintFalloff(f);
}

void TexturePaintController::setBrushColor(const QColor& c)
{
    if (auto* em = EditModeController::instance()) em->setVertexPaintColor(c);
}

QString TexturePaintController::bakeToOriginalFile()
{
    if (m_originalTextureName.isEmpty()) return QString();
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0) return QString();

    // Resolve the on-disk path. The texture name might already be a
    // full filename; Ogre's resource manager has it indexed under
    // each registered FileSystem location.
    QString diskPath;
    auto& rgm = Ogre::ResourceGroupManager::getSingleton();
    const Ogre::String texName = m_originalTextureName.toStdString();
    // Try every group's listResourceLocations.
    try {
        for (const auto& grp : rgm.getResourceGroups()) {
            auto locs = rgm.listResourceLocations(grp);
            for (const auto& loc : *locs) {
                QDir d(QString::fromStdString(loc));
                const QString candidate = d.filePath(m_originalTextureName);
                if (QFileInfo(candidate).exists()) {
                    diskPath = candidate;
                    break;
                }
            }
            if (!diskPath.isEmpty()) break;
        }
    } catch (...) {}

    if (diskPath.isEmpty()) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Bake to original: no disk file for texture '%1' (embedded?)")
                .arg(m_originalTextureName));
        return QString();
    }
    if (!m_buffer.save(diskPath.toStdString())) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Bake to original: save FAILED at %1").arg(diskPath));
        return QString();
    }
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Bake to original: wrote %1").arg(diskPath));
    return diskPath;
}

QString TexturePaintController::loadPaintBufferInteractive()
{
    QApplication::processEvents();
    QWidget* parent = QApplication::activeWindow();
    const QString path = QFileDialog::getOpenFileName(
        parent,
        QStringLiteral("Load texture into paint buffer"),
        QDir::currentPath(),
        QStringLiteral("Image files (*.png *.jpg *.jpeg *.tga *.bmp);;All files (*)"),
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons);
    if (path.isEmpty()) return QString();
    if (!loadPaintBuffer(path)) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture paint: load failed (%1)").arg(path));
        return QString();
    }
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Texture paint: loaded %1").arg(path));
    return path;
}

int TexturePaintController::bakeVertexColorsToTexture(int resolution,
                                                      int dilation,
                                                      const QString& savePath)
{
    auto* entity = activeEntity();
    if (!entity) return -1;
    if (!ensureEditableMesh(entity)) return -1;
    const int res = resolution > 0 ? resolution
                                   : (m_buffer.width() > 0 ? m_buffer.width() : 1024);
    VertexColorBaker::Options opts;
    opts.resolution = res;
    opts.dilationPixels = std::max(0, dilation);
    const int painted = VertexColorBaker::bake(*m_paintMesh, m_buffer, opts);

    m_sessionEntity = entity;
    m_ogreTexture.reset();
    static unsigned int s_bakeUnique = 0;
    const QString hint = QStringLiteral("QMEBake_%1_%2")
                       .arg(QString::fromStdString(entity->getName()))
                       .arg(++s_bakeUnique);
    // Bake is an explicit user action — rebind immediately so the
    // baked result appears on the model.
    createOgreTextureFromBuffer(entity, hint, /*rebindToModel=*/true);

    if (!savePath.isEmpty())
        m_buffer.save(savePath.toStdString());

    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Vertex→Texture bake: %1×%1 (%2 pixels, dilation=%3)")
            .arg(res).arg(painted).arg(opts.dilationPixels));

    GamificationManager::noteOperation(
        QStringLiteral("vertex_color_bake"),
        {{QStringLiteral("texture_size"), res},
         {QStringLiteral("pixels_painted"), painted}});

    refreshPreviewUri();
    emit sessionChanged();
    return painted;
}

bool TexturePaintController::ensureUndoTarget(const std::string& entityName, int channel)
{
    if (entityName.empty()) return false;
    if (channel < 0 || channel >= PaintChannelNS::kTexturePaintChannelCount)
        return false;

    // Resolve the entity BY NAME (the command stores the name, never a raw
    // pointer — resolving avoids dereferencing a possibly-deleted entity). If
    // it's gone from the scene, the undo is a safe no-op.
    Ogre::Entity* entity = nullptr;
    if (auto* mgr = Manager::getSingletonPtr()) {
        if (auto* sm = mgr->getSceneMgr()) {
            try {
                if (sm->hasEntity(entityName)) entity = sm->getEntity(entityName);
            } catch (...) { entity = nullptr; }
        }
    }
    if (!entity) return false;

    // Make `entity` the paint target if it isn't already (undo of a stroke on a
    // mesh the user has since deselected still restores the right pixels).
    if (activeEntity() != entity) {
        if (auto* sel = SelectionSet::getSingleton()) {
            sel->clear();
            sel->append(entity);
        }
        // A different entity means fresh per-channel sessions.
        m_channelSessions.clear();
        m_channelSessionEntity = nullptr;
        closeSession();
    }

    // Switch to the command's channel (stashes/restores the right stack) and
    // make sure a live session exists on it.
    if (m_activeChannel != static_cast<PaintChannelNS::Channel>(channel))
        setActiveChannel(channel);
    if (!hasActiveSession())
        ensurePaintableTexture(m_buffer.width() > 0 ? m_buffer.width() : 1024);
    return hasActiveSession();
}

void TexturePaintController::applyPixelSnapshot(const std::vector<uint8_t>& pixels)
{
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0) return;
    if (pixels.size() != m_buffer.data().size()) return;
    std::memcpy(m_buffer.data().data(), pixels.data(), pixels.size());
    m_buffer.markDirty(0, 0, m_buffer.width(), m_buffer.height());
    flushDirtyToOgre();
    updateEmbeddedTextureCache();
}

void TexturePaintController::applyLayerPixelSnapshot(int layerIndex,
                                                     const std::vector<uint8_t>& pixels)
{
    if (m_layerStack.layerCount() <= 0) {
        applyPixelSnapshot(pixels);
        return;
    }
    if (layerIndex < 0 || layerIndex >= m_layerStack.layerCount()) return;
    auto& layerBuf = m_layerStack.layer(layerIndex).buffer;
    if (pixels.size() != layerBuf.data().size()) return;
    std::memcpy(layerBuf.data().data(), pixels.data(), pixels.size());
    layerBuf.markDirty(0, 0, layerBuf.width(), layerBuf.height());
    m_layerStrokeBaseline.clear();
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    updateEmbeddedTextureCache();
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
}

void TexturePaintController::applyLayerStackSnapshot(const PaintLayerStack::Snapshot& snap)
{
    m_layerStack.restore(snap);
    m_layerStrokeBaseline.clear();
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    updateEmbeddedTextureCache();
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
}

void TexturePaintController::closeSession()
{
    if (m_strokeActive) endStroke();

    // Restore every TUS we rebound. Must do this BEFORE removing the
    // paint texture from TextureManager, otherwise the next render
    // tries to sample a freed handle. Look the material up by name
    // each time so a destroyed/reloaded material doesn't dangle.
    std::set<std::string> toReload;
    for (const auto& s : m_boundSlots) {
        try {
            if (s.materialName.empty()) continue;
            auto matPtr = Ogre::MaterialManager::getSingleton().getByName(
                s.materialName);
            if (!matPtr) continue;
            const auto& techs = matPtr->getTechniques();
            if (s.techIdx >= techs.size()) continue;
            auto* tech = techs[s.techIdx];
            if (s.passIdx >= tech->getNumPasses()) continue;
            auto* p = tech->getPass(s.passIdx);
            if (s.tusIdx >= p->getNumTextureUnitStates()) continue;
            auto* tusN = p->getTextureUnitState(s.tusIdx);
            tusN->setTextureName(s.originalTexture);
            toReload.insert(s.materialName);
        } catch (...) {}
    }
    m_boundSlots.clear();
    for (const auto& mname : toReload) {
        try {
            auto matPtr = Ogre::MaterialManager::getSingleton().getByName(mname);
            if (matPtr) { matPtr->compile(); matPtr->reload(); }
        } catch (...) {}
    }

    if (m_ogreTexture) {
        try {
            Ogre::TextureManager::getSingleton().remove(m_ogreTexture);
        } catch (...) {}
        m_ogreTexture.reset();
    }
    // Source scene manager from the global singleton — going through
    // m_paintMeshEntity->_getManager() is unsafe if the entity was
    // cascade-destroyed (mesh removed while paint was active).
    {
        auto* mgr = Manager::getSingletonPtr();
        auto* sceneMgr = mgr ? mgr->getSceneMgr() : nullptr;
        if (sceneMgr) {
            try {
                if (m_ringNode) {
                    m_ringNode->detachAllObjects();
                    sceneMgr->getRootSceneNode()->removeChild(m_ringNode);
                    sceneMgr->destroySceneNode(m_ringNode);
                }
                if (m_ringObj)
                    sceneMgr->destroyManualObject(m_ringObj);
                // Paint v2 Slice E (#548): tear down the symmetry-plane overlay.
                if (m_symPlaneNode) {
                    m_symPlaneNode->detachAllObjects();
                    sceneMgr->getRootSceneNode()->removeChild(m_symPlaneNode);
                    sceneMgr->destroySceneNode(m_symPlaneNode);
                }
                if (m_symPlaneObj)
                    sceneMgr->destroyManualObject(m_symPlaneObj);
                // Paint v2 Slice F (#549): tear down the decal overlay.
                if (m_decalNode) {
                    m_decalNode->detachAllObjects();
                    sceneMgr->getRootSceneNode()->removeChild(m_decalNode);
                    sceneMgr->destroySceneNode(m_decalNode);
                }
                if (m_decalObj)
                    sceneMgr->destroyManualObject(m_decalObj);
            } catch (...) {}
        }
        m_ringNode = nullptr;
        m_ringObj = nullptr;
        m_symPlaneNode = nullptr;
        m_symPlaneObj = nullptr;
        m_decalNode = nullptr;
        m_decalObj = nullptr;
    }
    // Paint v2 Slice E (#548): the topology mirror maps are per-entity/per-mesh;
    // drop them so a rebuilt session (or a different entity) rebuilds fresh.
    invalidateSymmetryMaps();
    m_decal.cancel();
    m_haveDecalDragPos = false;   // else a stale drag anchor leaks into the next session
    m_paintMesh.reset();
    m_paintMeshEntity = nullptr;
    m_layerStack = PaintLayerStack();
    m_layerPreviewVersion = 0;
    m_buffer = TexturePaintBuffer();
    m_textureName.clear();
    m_originalTexture.reset();
    m_originalTextureName.clear();
    m_useOriginalTexture = false;
    m_forceManualPaintTexture = false;
    m_loggedInPlaceBlit = false;
    m_rebindScheduled = false;
    m_gpuFlushScheduled = false;
    m_strokeGpuFlushScheduled = false;
    m_tiledUploadRunning = false;
    m_tiledUploadQueue.clear();
    m_uploadFinishingStroke = false;
    m_gpuUploadGeneration = 0;
    m_activeUploadGeneration = 0;
    m_bufferDirtyEpoch = 0;
    m_uploadEpochSnapshot = 0;
    m_strokeUndoGeneration = 0;
    m_layerStrokeBaseline.clear();
    m_strokePreSnapshot.clear();
    m_sessionEntity = nullptr;
    if (!m_uvOverlayUri.isEmpty()) {
        m_uvOverlayUri.clear();
        emit uvOverlayChanged();
    }
    // Tear down smart-select state too — a stale mask sized for the
    // previous buffer would crash smartSelectAtUV. Also drop the on-
    // mesh wand overlay (it pointed at the old entity).
    m_mask = PaintSelectionMask();
    destroyMeshMaskOverlay();
    if (!m_maskOverlayUri.isEmpty()) {
        m_maskOverlayUri.clear();
        emit smartSelectChanged();
    }
    m_previewUri.clear();
    emit previewChanged();
    emit sessionChanged();
    // `m_decal.cancel()` above flipped decalSessionActive/decalState, whose NOTIFY
    // is projectionChanged — without this the panel keeps showing an active decal
    // session after the session closes (entity delete, channel switch, bake,
    // sceneClearing) until some unrelated projection change happens to fire.
    emit projectionChanged();
}

// ---------------------------------------------------------------------------
// UV-based stroke API (driven by the texture preview panel)
// ---------------------------------------------------------------------------

bool TexturePaintController::beginStrokeUV(double u, double v)
{
    if (!m_paintEnabled || m_strokeActive) return false;
    if (!hasActiveSession())
        if (!ensurePaintableTexture(1024)) return false;
    GamificationManager::noteFeature(QStringLiteral("texture_paint"));
    if (m_tiledUploadRunning)
        cancelInFlightGpuUpload();
    ++m_gpuUploadGeneration;
    ++m_strokeUndoGeneration;
    m_strokeActive = true;
    m_strokeFromUvPreview = true;
    m_strokePreSnapshot.clear();
    resetStrokePaintState();
    // Re-use the screen-pos field to stash press UV (u in pixel-ish
    // units). updateStrokeUV reads the delta from the current u.
    m_wandStartScreenPos = QPoint(static_cast<int>(u * 10000.0), 0);
    m_pendingStrokeWidget = nullptr;
    if (m_colorSource == ColorGradient) {
        SentryReporter::addBreadcrumb(
            "paint.brush.gradient",
            QStringLiteral("mode=%1 ramp=%2")
                .arg(static_cast<int>(m_gradientMode))
                .arg(m_useFgBgRamp ? QStringLiteral("FG/BG") : m_activeRampName));
    }
    emit hoveredUVChanged(u, v);
    if (m_layerStrokeBaseline.empty())
        m_layerStrokeBaseline = snapshotActiveLayerPixels();
    m_strokePreSnapshot = std::move(m_layerStrokeBaseline);
    m_pendingStrokeU = u;
    m_pendingStrokeV = v;
    processPendingStrokeUpdateUV();
    return true;
}

void TexturePaintController::updateStrokeUV(double u, double v)
{
    if (!m_strokeActive || !m_paintEnabled) return;
    m_pendingStrokeU = u;
    m_pendingStrokeV = v;
    processPendingStrokeUpdateUV();
}

void TexturePaintController::endStrokeUV()
{
    endStroke();
}

void TexturePaintController::setHoveredUV(double u, double v)
{
    emit hoveredUVChanged(u, v);
    // Reverse-lookup: find the 3D point on the mesh that maps to this
    // UV, then draw the brush ring there. Lets the user "scrub" the
    // texture panel and see the corresponding location on the model.
    if (!m_paintEnabled || !m_paintMesh || !m_paintMeshEntity) return;
    Ogre::Vector3 localPos, localNormal;
    if (findMeshPointForUV(Ogre::Vector2(u, v), localPos, localNormal))
        drawHoverRingAt(localPos, localNormal);
}

void TexturePaintController::clearHoveredUV()
{
    emit hoveredUVChanged(-1.0, -1.0);
}

// ---------------------------------------------------------------------------
// Texture slot enumeration
// ---------------------------------------------------------------------------

namespace {
// Paint v2 Slice D — map a canonical Ogre TUS name to the PBR channel it
// represents (or Channel::Count when it isn't a recognised PBR slot). Uses the
// same name conventions as RTShaderHelper's slot predicates.
PaintChannelNS::Channel paintChannelForSlotName(const std::string& n)
{
    using C = PaintChannelNS::Channel;
    if (n == "albedo" || n == "diffuse_map" || n == "Diffuse" || n == "BaseColor")
        return C::BaseColor;
    if (n == "normal_map" || n == "NormalMap" || n == "Bump" || n == "bump")
        return C::Normal;
    if (n == "roughness" || n == "Roughness") return C::Roughness;
    if (n == "metallic"  || n == "Metallic")  return C::Metallic;
    if (n == "ao" || n == "AO" || n == "occlusion") return C::AO;
    if (n == "emissive" || n == "Emissive")   return C::Emissive;
    return C::Count;
}
} // namespace

void TexturePaintController::refreshSlots()
{
    // Refresh is pure metadata — it must not create a paint session
    // as a side effect. Otherwise toggling the brush button (which
    // calls refreshSlots) silently rebinds the model's diffuse TUS
    // and the user sees the model "go textureless". Sessions are
    // created lazily inside beginStroke() instead.
    //
    // But we DO eagerly build the EditableMesh for the selected
    // entity so hover/brush-ring queries work without an active
    // session. The EditableMesh is just a CPU mirror of mesh data;
    // it doesn't alter the model's render.
    if (m_paintEnabled) {
        if (auto* e = activeEntity())
            ensureEditableMesh(e);
    }

    QVariantList newSlots;
    auto* entity = activeEntity();
    if (entity) {
        for (unsigned int si = 0; si < entity->getNumSubEntities(); ++si) {
            auto* subEnt = entity->getSubEntity(si);
            if (!subEnt) continue;
            Ogre::MaterialPtr mat = subEnt->getMaterial();
            if (!mat || mat->getNumTechniques() == 0) continue;
            auto* tech = mat->getTechnique(0);
            if (!tech || tech->getNumPasses() == 0) continue;
            auto* pass = tech->getPass(0);
            if (!pass) continue;
            for (unsigned short ti = 0; ti < pass->getNumTextureUnitStates(); ++ti) {
                auto* tus = pass->getTextureUnitState(ti);
                const std::string& n = tus->getName();
                const std::string tex = tus->getTextureName();
                // Skip entirely-empty TUSes (no name AND no texture) —
                // they're usually placeholders.
                if (n.empty() && tex.empty()) continue;
                QVariantMap m;
                const QString labelN = QString::fromStdString(n.empty() ? "TUS " + std::to_string(ti) : n);
                m["label"] = QStringLiteral("sub %1 — %2").arg(si).arg(labelN);
                m["submesh"] = static_cast<int>(si);
                m["slot"] = QString::fromStdString(n);
                m["textureName"] = QString::fromStdString(tex);
                // Paint v2 Slice D: tag the slot with the PBR channel it maps
                // to (by canonical TUS name) so the channel router can find the
                // TUS for a given channel. -1 = not a recognised PBR channel.
                const auto slotChan = paintChannelForSlotName(n);
                m["channel"] = (slotChan == PaintChannelNS::Channel::Count)
                                   ? -1 : static_cast<int>(slotChan);
                newSlots.append(m);
            }
        }
    }
    if (newSlots == m_slots) return;
    m_slots = newSlots;
    if (m_activeSlot >= m_slots.size())
        m_activeSlot = m_slots.isEmpty() ? -1 : 0;
    emit slotsChanged();
}

// ---------------------------------------------------------------------------
// Paint v2 Slice D — PBR channel painting (#547)
// ---------------------------------------------------------------------------

std::string TexturePaintController::activeChannelSlotName() const
{
    // Height has no direct slot — it is painted as a heightmap and baked into
    // the normal_map slot, so it targets normal_map for its live session too.
    if (m_activeChannel == PaintChannelNS::Channel::Height)
        return "normal_map";
    return PaintChannelNS::slotName(m_activeChannel);
}

QVariantList TexturePaintController::paintChannels() const
{
    // Model for the 7-button channel picker (VertexColor excluded — it's the
    // Texture/Vertex paintTarget toggle). `hasLayers` lets QML mark channels
    // the user has already painted into.
    QVariantList out;
    for (int i = 0; i < PaintChannelNS::kTexturePaintChannelCount; ++i) {
        const auto c = static_cast<PaintChannelNS::Channel>(i);
        // Height is NOT offered as its own channel: it has no slot of its own
        // (it can only be Sobel-converted INTO normal_map, which is exactly
        // what the Normal channel already does), and a separate Height channel
        // just produced a second normal-map bake that fought the first. Paint
        // the Normal channel directly instead (#547).
        if (c == PaintChannelNS::Channel::Height) continue;
        QVariantMap m;
        m["id"] = QString::fromLatin1(PaintChannelNS::id(c));
        m["label"] = QString::fromLatin1(PaintChannelNS::label(c));
        m["slot"] = QString::fromLatin1(PaintChannelNS::slotName(c));
        m["scalar"] = PaintChannelNS::isScalar(c);
        // hasLayers: the ACTIVE channel's stack is the live m_layerStack (its
        // stashed copy is stale until the next channel switch); other channels
        // read their stashed session.
        bool hasLayers = false;
        if (c == m_activeChannel) {
            hasLayers = m_layerStack.layerCount() > 0;
        } else {
            auto it = m_channelSessions.constFind(i);
            hasLayers = (it != m_channelSessions.constEnd() && it->initialized
                         && it->layerStack.layerCount() > 0);
        }
        m["hasLayers"] = hasLayers;
        out.append(m);
    }
    return out;
}

void TexturePaintController::stashChannelSession(PaintChannelNS::Channel channel)
{
    ChannelSessionState& s = m_channelSessions[static_cast<int>(channel)];
    s.layerStack = m_layerStack;                 // deep copy (layers own buffers)
    s.layerStrokeBaseline = m_layerStrokeBaseline;
    s.initialized = (m_layerStack.layerCount() > 0);
}

bool TexturePaintController::restoreChannelSession(PaintChannelNS::Channel channel)
{
    auto it = m_channelSessions.find(static_cast<int>(channel));
    if (it == m_channelSessions.end() || !it->initialized)
        return false;
    m_layerStack = it->layerStack;
    m_layerStrokeBaseline = it->layerStrokeBaseline;
    return true;
}

void TexturePaintController::setActiveChannel(int channel)
{
    if (channel < 0 || channel >= PaintChannelNS::kTexturePaintChannelCount)
        return;
    const auto newChannel = static_cast<PaintChannelNS::Channel>(channel);
    // Height is not a selectable channel (it has no slot of its own — paint
    // Normal directly). Redirect any stray request to Normal.
    if (newChannel == PaintChannelNS::Channel::Height)
        return setActiveChannel(static_cast<int>(PaintChannelNS::Channel::Normal));
    // Selecting the already-active channel is normally a no-op — BUT a bake
    // tears the live session down while leaving m_activeChannel unchanged, so
    // re-selecting the same channel after a bake must REOPEN the session (else
    // the panel shows no active session and the next stroke can't start).
    if (newChannel == m_activeChannel) {
        if (!hasActiveSession() && activeEntity()) {
            const int res = m_buffer.width() > 0 ? m_buffer.width() : 1024;
            ensurePaintableTexture(res);
            restoreChannelSession(newChannel);
            emit sessionChanged();
            emit layersChanged();
        }
        return;
    }

    SentryReporter::addBreadcrumb(
        "paint.channel",
        QStringLiteral("switch to %1").arg(PaintChannelNS::id(newChannel)));

    // Preserve the working resolution across channel switches (same rationale
    // as setActiveSlotIndex).
    const int preservedRes = m_buffer.width() > 0 ? m_buffer.width() : 1024;

    // Stash the current channel's live layer stack, then close the live
    // session (tears down GPU texture handles + paint mesh).
    if (hasActiveSession() || m_layerStack.layerCount() > 0)
        stashChannelSession(m_activeChannel);
    closeSession();

    m_activeChannel = newChannel;

    // Point the slot router at this channel's canonical slot, if the model has
    // a matching slot; else -1 so findOrCreateActiveTextureUnit creates it.
    m_activeSlot = -1;
    for (int i = 0; i < m_slots.size(); ++i) {
        if (m_slots.at(i).toMap().value("channel", -1).toInt() == channel) {
            m_activeSlot = i;
            break;
        }
    }

    // Rebuild the live session for the new channel's texture, then restore any
    // previously-stashed layer stack for it (overwriting the freshly-loaded
    // single layer). If none was stashed, ensurePaintableTexture's stack stands.
    ensurePaintableTexture(preservedRes);
    if (restoreChannelSession(newChannel)) {
        // compositeTo() resizes m_buffer to the restored stack's own dimensions.
        // Same-entity sessions share preservedRes so this is normally a no-op,
        // but keep the selection mask paired 1:1 with the buffer regardless so
        // smartSelect's per-pixel indexing can never run past the buffer if a
        // stashed stack ever differed in size (#547 review — defensive).
        m_layerStack.compositeTo(m_buffer);
        if (m_mask.width() != m_buffer.width() || m_mask.height() != m_buffer.height())
            m_mask.resize(m_buffer.width(), m_buffer.height());
        m_buffer.markDirty(0, 0, m_buffer.width(), m_buffer.height());
        schedulePreviewRefresh();
        flushDirtyToOgre();
    }

    emit activeChannelChanged();
    emit layersChanged();
    emit slotsChanged();
}

namespace {
// Composite a channel's stashed (or live) layer stack into an RGBA8 QImage.
QImage compositeChannelToImage(const PaintLayerStack& stack)
{
    if (stack.empty() || stack.width() <= 0 || stack.height() <= 0)
        return {};
    std::vector<uint8_t> px;
    stack.compositeTo(px);
    const int w = stack.width(), h = stack.height();
    if (static_cast<int>(px.size()) < w * h * 4) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), px.data(), static_cast<size_t>(w) * h * 4);
    return img;
}

// Rec.601 luminance of an RGBA pixel row-major image → grayscale [0..255].
inline uint8_t luma601(QRgb p)
{
    return static_cast<uint8_t>(qBound(0, static_cast<int>(
        0.299 * qRed(p) + 0.587 * qGreen(p) + 0.114 * qBlue(p) + 0.5), 255));
}

// Directory for baked channel textures (shared with AI-generated maps).
QString generatedTexDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir = QDir(base).filePath("generated_textures");
    QDir().mkpath(dir);
    return dir;
}
} // namespace

bool TexturePaintController::bakeChannel(int channel)
{
    if (channel < 0 || channel >= PaintChannelNS::kTexturePaintChannelCount)
        return false;
    const auto ch = static_cast<PaintChannelNS::Channel>(channel);

    // Source the channel's layer stack: the live stack if it's the active
    // channel, else the stashed session.
    const PaintLayerStack* stack = nullptr;
    if (ch == m_activeChannel && m_layerStack.layerCount() > 0) {
        stack = &m_layerStack;
    } else {
        auto it = m_channelSessions.constFind(channel);
        if (it != m_channelSessions.constEnd() && it->initialized)
            stack = &it->layerStack;
    }
    if (!stack || stack->empty()) return false;

    QImage painted = compositeChannelToImage(*stack);
    if (painted.isNull()) return false;

    auto* entity = activeEntity();
    if (!entity) return false;

    SentryReporter::addBreadcrumb(
        "paint.channel", QStringLiteral("bake %1").arg(PaintChannelNS::id(ch)));

    const QString dir = generatedTexDir();
    const QString stamp = QString::number(QDateTime::currentMSecsSinceEpoch());
    QString outFile;      // file written to disk (basename bound into the slot)
    std::string slot;     // canonical TUS slot to bind

    if (PaintChannelNS::isColor(ch)) {
        // BaseColor / Emissive: composite the painted strokes OVER the slot's
        // existing texture (source-over), not straight into the slot. A paint
        // session whose base texture couldn't be loaded starts TRANSPARENT
        // (so an unpainted channel doesn't wash the model — see
        // ensurePaintableTexture); saving that composite raw would bind a
        // mostly-transparent diffuse and "lose" the base color (#547). Reading
        // the current slot texture back and painting on top keeps the original
        // colour everywhere the user didn't paint.
        slot = PaintChannelNS::slotName(ch);
        {
            // Resolve the slot's UNDERLYING texture to composite over. The live
            // TUS may currently point at the transient manual paint texture
            // (name "QMEPaint_*") — reading THAT back would give the painted
            // strokes over transparent, wiping the real base colour on bake. So
            // prefer the session's recorded original texture, and ignore any
            // QMEPaint_* name when falling back to the slot/alias lookup.
            auto isPaintTex = [](const QString& n) {
                return n.startsWith(QStringLiteral("QMEPaint_"));
            };
            QString cur;
            if (ch == m_activeChannel && !m_originalTextureName.isEmpty()
                && !isPaintTex(m_originalTextureName)) {
                cur = m_originalTextureName;
            }
            if (cur.isEmpty() || isPaintTex(cur)) {
                QString c = currentSlotTextureName(slot);
                if ((c.isEmpty() || isPaintTex(c))
                    && ch == PaintChannelNS::Channel::BaseColor) {
                    for (const char* alias : {"diffuse_map", "albedo", "Diffuse", "BaseColor"}) {
                        c = currentSlotTextureName(alias);
                        if (!c.isEmpty() && !isPaintTex(c)) break;
                    }
                }
                if (!c.isEmpty() && !isPaintTex(c)) cur = c;
            }
            QImage base = isPaintTex(cur) ? QImage() : loadImageAcrossGroups(cur);
            if (!base.isNull()) {
                base = base.convertToFormat(QImage::Format_RGBA8888)
                           .scaled(painted.size());
                QPainter p(&base);
                p.setCompositionMode(QPainter::CompositionMode_SourceOver);
                p.drawImage(0, 0, painted.convertToFormat(QImage::Format_RGBA8888));
                p.end();
                painted = base;
            }
            // No existing texture (a brand-new diffuse) → paint the strokes as
            // the whole texture. Flatten any transparency onto an opaque base
            // so a partially-painted new diffuse doesn't render see-through:
            // BaseColor is opaque by nature.
            else if (ch == PaintChannelNS::Channel::BaseColor) {
                QImage opaque(painted.size(), QImage::Format_RGBA8888);
                opaque.fill(Qt::white);
                QPainter p(&opaque);
                p.drawImage(0, 0, painted.convertToFormat(QImage::Format_RGBA8888));
                p.end();
                painted = opaque;
            }
        }
        outFile = QDir(dir).filePath(QStringLiteral("paint_%1_%2.png")
                      .arg(PaintChannelNS::id(ch), stamp));
        if (!painted.save(outFile, "PNG")) return false;
    } else if (ch == PaintChannelNS::Channel::Height
               || ch == PaintChannelNS::Channel::Normal) {
        // Normal channel: the painted grayscale is treated as a height field and
        // Sobel-converted to a tangent-space normal (NormalMapGenerator), then
        // combined with the existing normal (below). (Height is no longer a
        // selectable channel — setActiveChannel redirects it here — but the
        // branch still accepts it so a direct bakeChannel(Height) call works.)
        // Write a grayscale heightmap first.
        QImage height(painted.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < painted.height(); ++y)
            for (int x = 0; x < painted.width(); ++x)
                height.scanLine(y)[x] = luma601(painted.pixel(x, y));
        const QString heightFile = QDir(dir).filePath(
            QStringLiteral("paint_height_%1.png").arg(stamp));
        if (!height.save(heightFile, "PNG")) return false;

        NormalMapGenerator::GenSpec spec;
        spec.sourcePath = heightFile;
        spec.strength = 2.0f;
        NormalMapGenerator::GenResult nr = NormalMapGenerator::generate(spec);
        if (!nr.ok || nr.image.isNull()) return false;
        slot = "normal_map";
        QImage detail = nr.image.convertToFormat(QImage::Format_RGBA8888);

        // COMBINE the painted detail normal with the model's EXISTING normal map
        // rather than replacing it. Untouched texels produce a flat detail
        // normal (0,0,1 → RGB 128,128,255) which must leave the base normal
        // unchanged; painted texels add their relief on top. Without this a
        // Height/Normal bake wiped the original normal map everywhere the user
        // didn't paint (#547). Skip the transient QMEPaint_* paint texture (the
        // same trap the colour path guards against).
        {
            auto isPaintTex = [](const QString& n) {
                return n.startsWith(QStringLiteral("QMEPaint_"));
            };
            // Prefer m_channelBaseTextureName — the real normal_map texture the
            // slot held when the Normal session opened, captured before the
            // buffer was blanked (the Normal session is NOT seeded from it, so
            // m_originalTextureName is empty here). The live TUS may point at
            // the transient QMEPaint_* paint texture. Fall back to the slot and
            // its aliases.
            QString cur;
            if (ch == m_activeChannel && !m_channelBaseTextureName.isEmpty()
                && !isPaintTex(m_channelBaseTextureName)) {
                cur = m_channelBaseTextureName;
            }
            if (cur.isEmpty() || isPaintTex(cur)) {
                QString c = currentSlotTextureName("normal_map");
                if (c.isEmpty() || isPaintTex(c)) {
                    for (const char* alias : {"NormalMap", "Bump", "bump", "BumpMap", "height_map"}) {
                        const QString a = currentSlotTextureName(alias);
                        if (!a.isEmpty() && !isPaintTex(a)) { c = a; break; }
                    }
                }
                if (!c.isEmpty() && !isPaintTex(c)) cur = c;
            }
            QImage base = isPaintTex(cur) ? QImage() : loadImageAcrossGroups(cur);
            if (!base.isNull()) {
                base = base.convertToFormat(QImage::Format_RGBA8888)
                           .scaled(detail.size());
                // Whiteout / partial-derivative blend:
                //   n.xy = base.xy + detail.xy ; n.z = base.z * detail.z ; normalize
                for (int y = 0; y < detail.height(); ++y) {
                    uchar* d = detail.scanLine(y);
                    const uchar* b = base.constScanLine(y);
                    for (int x = 0; x < detail.width(); ++x) {
                        const int i = x * 4;
                        auto dec = [](uchar c) { return (c / 255.0f) * 2.0f - 1.0f; };
                        auto enc = [](float v) {
                            int c = static_cast<int>((v * 0.5f + 0.5f) * 255.0f + 0.5f);
                            return static_cast<uchar>(std::clamp(c, 0, 255));
                        };
                        float bx = dec(b[i]),   by = dec(b[i+1]),   bz = dec(b[i+2]);
                        float dx = dec(d[i]),   dy = dec(d[i+1]),   dz = dec(d[i+2]);
                        float nx = bx + dx, ny = by + dy, nz = bz * dz;
                        const float len = std::sqrt(nx*nx + ny*ny + nz*nz);
                        if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
                        else { nx = 0.f; ny = 0.f; nz = 1.f; }
                        d[i]   = enc(nx);
                        d[i+1] = enc(ny);
                        d[i+2] = enc(nz);
                        d[i+3] = 255;
                    }
                }
            }
        }
        outFile = QDir(dir).filePath(QStringLiteral("paint_normal_%1.png").arg(stamp));
        if (!detail.save(outFile, "PNG")) return false;
    } else {
        // Scalar (Roughness/Metallic/AO): collapse to luminance and write into
        // the packed ORM texture the Cook-Torrance SRS reads from the
        // `metallic` slot — .r = AO, .g = roughness, .b = metallic. Preserve
        // the other two lanes (start from the existing ORM if bound, else a
        // sensible unpainted default: AO=255 (no occlusion), roughness=255
        // (rough dielectric), metallic=0 (NON-metal — filling metallic with
        // 255 would turn a first roughness bake fully metallic).
        slot = "metallic";
        // Find an existing ORM/ metallic texture on the material to merge with.
        QImage orm(painted.size(), QImage::Format_RGBA8888);
        orm.fill(qRgba(/*AO*/255, /*rough*/255, /*metal*/0, 255));
        {
            // Read back the current metallic-slot texture if present.
            const QString cur = currentSlotTextureName("metallic");
            if (!cur.isEmpty()) {
                QImage existing = loadImageAcrossGroups(cur);
                if (!existing.isNull()) {
                    orm = existing.convertToFormat(QImage::Format_RGBA8888)
                              .scaled(painted.size());
                }
            }
        }
        const int lane = (ch == PaintChannelNS::Channel::AO) ? 0
                       : (ch == PaintChannelNS::Channel::Roughness) ? 1 : 2;
        for (int y = 0; y < painted.height(); ++y) {
            uchar* row = orm.scanLine(y);
            for (int x = 0; x < painted.width(); ++x) {
                row[x * 4 + lane] = luma601(painted.pixel(x, y));
                row[x * 4 + 3] = 255;
            }
        }
        outFile = QDir(dir).filePath(QStringLiteral("paint_orm_%1.png").arg(stamp));
        if (!orm.save(outFile, "PNG")) return false;
    }

    // Ensure the baked file is discoverable + bind it into every material on
    // the active entity that should carry this slot, then wire PBR + IBL.
    Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
        dir.toStdString(), "FileSystem",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, false);
    const std::string baseName = QFileInfo(outFile).fileName().toStdString();
    bindBakedChannelTexture(entity, slot, baseName, ch);
    return true;
}

QString TexturePaintController::currentSlotTextureName(const std::string& slot) const
{
    auto* entity = activeEntity();
    if (!entity || entity->getNumSubEntities() == 0) return {};
    for (unsigned int s = 0; s < entity->getNumSubEntities(); ++s) {
        auto* se = entity->getSubEntity(s);
        if (!se) continue;
        Ogre::MaterialPtr mat = se->getMaterial();
        if (!mat || mat->getNumTechniques() == 0) continue;
        auto* tech = mat->getTechnique(0);
        if (!tech || tech->getNumPasses() == 0) continue;
        auto* pass = tech->getPass(0);
        for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
            auto* tus = pass->getTextureUnitState(i);
            if (tus->getName() == slot && !tus->getTextureName().empty())
                return QString::fromStdString(tus->getTextureName());
        }
    }
    return {};
}

QImage TexturePaintController::loadImageAcrossGroups(const QString& textureName) const
{
    if (textureName.isEmpty()) return {};
    // Prefer a CPU-side read (handles embedded FBX bytes / on-disk origins /
    // resource-group paths), reusing the paint session's own loader.
    TexturePaintBuffer tmp;
    Ogre::TexturePtr tex;
    try { tex = findTextureAcrossGroups(textureName.toStdString()); } catch (...) {}
    if (loadPaintBufferFromNonGpuSources(tmp, tex, textureName)
        && tmp.width() > 0 && tmp.height() > 0) {
        QImage img(tmp.width(), tmp.height(), QImage::Format_RGBA8888);
        std::memcpy(img.bits(), tmp.data().data(),
                    static_cast<size_t>(tmp.width()) * tmp.height() * 4);
        return img;
    }

    // Fallback: the texture may be referenced only by resource-group NAME (a
    // FileSystem-registered file that was never loaded into TextureManager, or
    // an imported normal/diffuse whose GPU texture has no readable CPU buffer).
    // Ogre::Image::load resolves it through the resource system (AUTODETECT
    // group), which findTextureAcrossGroups + the non-GPU loader miss. Without
    // this, a bake's "combine with the existing texture" step silently found no
    // base and overwrote it (the #547 normal-map-replaced-not-combined bug).
    try {
        Ogre::Image ogreImg;
        ogreImg.load(textureName.toStdString(),
                     Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
        const int w = static_cast<int>(ogreImg.getWidth());
        const int h = static_cast<int>(ogreImg.getHeight());
        if (w > 0 && h > 0) {
            QImage img(w, h, QImage::Format_RGBA8888);
            Ogre::PixelBox src = ogreImg.getPixelBox();
            Ogre::PixelBox dst(w, h, 1, Ogre::PF_BYTE_RGBA, img.bits());
            Ogre::PixelUtil::bulkPixelConversion(src, dst);
            return img;
        }
    } catch (...) {}
    return {};
}

void TexturePaintController::bindBakedChannelTexture(
    Ogre::Entity* entity, const std::string& slot,
    const std::string& textureBaseName, PaintChannelNS::Channel channel)
{
    if (!entity || slot.empty() || textureBaseName.empty()) return;

    // The BaseColor bake targets the model's EXISTING diffuse TUS, which on a
    // typical import is named "diffuse_map" (or "Diffuse"/"BaseColor"), NOT the
    // canonical "albedo". Forcing a brand-new "albedo" TUS left the real
    // diffuse_map still bound to the transient paint texture (tracked in
    // m_boundSlots) — closeSession()/channel-switch then restored diffuse_map to
    // its pre-paint texture while the freshly-baked bytes sat on an unused
    // second slot, so the model "lost" the painted texture (#547 bug). Match the
    // diffuse slot by any albedo alias so a color bake overwrites the slot the
    // model actually samples.
    auto slotMatchesTus = [&](const std::string& tusName) {
        if (tusName == slot) return true;
        if (channel == PaintChannelNS::Channel::BaseColor) {
            // Alias-aware: diffuse_map / albedo / Diffuse / BaseColor.
            return tusName == "diffuse_map" || tusName == "albedo"
                || tusName == "Diffuse" || tusName == "BaseColor";
        }
        return false;
    };

    // Bind the baked texture into `slot` on every material used by the entity
    // (create the TUS if the material never had that slot), then wire PBR/FFP
    // so it renders live. We deliberately do NOT promote a plain (non-PBR)
    // material to Cook-Torrance here: completing the 6-slot metallic-roughness
    // layout (e.g. a scalar bake adding the `metallic` slot) would otherwise
    // flip applyPbrIfTagged on and darken the surface to near-black when no HDR
    // env is loaded — the other way the model appeared to "lose" its texture.
    // Only re-run applyPbrIfTagged for materials that were ALREADY PBR.
    std::set<std::string> wiredMats;
    for (unsigned int s = 0; s < entity->getNumSubEntities(); ++s) {
        auto* se = entity->getSubEntity(s);
        if (!se) continue;
        Ogre::MaterialPtr mat = se->getMaterial();
        if (!mat || wiredMats.count(mat->getName())) continue;
        wiredMats.insert(mat->getName());

        // "Already PBR" = the slice-E `pbr_workflow` user tag is present on the
        // first pass (set by imports / the Material Editor's Convert-to-PBR).
        // We only re-run Cook-Torrance for these; a plain material keeps its
        // FFP shading after a bake (see the comment above).
        bool wasPbr = false;
        if (mat->getNumTechniques() > 0
            && mat->getTechnique(0)->getNumPasses() > 0) {
            const auto& b = mat->getTechnique(0)->getPass(0)->getUserObjectBindings();
            const Ogre::Any& tag = b.getUserAny("pbr_workflow");
            if (tag.has_value()) {
                try { wasPbr = !Ogre::any_cast<Ogre::String>(tag).empty(); }
                catch (...) { wasPbr = false; }
            }
        }

        for (auto* tech : mat->getTechniques()) {
            for (unsigned short p = 0; p < tech->getNumPasses(); ++p) {
                auto* pass = tech->getPass(p);
                Ogre::TextureUnitState* tus = nullptr;
                for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i)
                    if (slotMatchesTus(pass->getTextureUnitState(i)->getName())) {
                        tus = pass->getTextureUnitState(i); break;
                    }
                if (!tus) { tus = pass->createTextureUnitState(); tus->setName(slot); }
                tus->setTextureName(textureBaseName);
            }
        }

        RTShaderHelper::wirePbrSlotsForFFP(mat.get());
        // Normal maps need tangents wired via the entity path (else unlit).
        if (channel == PaintChannelNS::Channel::Normal
            || channel == PaintChannelNS::Channel::Height) {
            MeshImporterExporter::applyNormalMapsToEntity(entity);
        }
        // Re-attach Cook-Torrance/IBL only for materials that were already PBR —
        // never silently convert a plain material's shading model on bake.
        if (wasPbr) RTShaderHelper::applyPbrIfTagged(mat);
        try { mat->compile(); } catch (...) {}
    }

    // A bake permanently rebinds the slot to the baked file. closeSession()
    // restores each m_boundSlots entry's recorded original texture — so if a
    // bound TUS IS the slot we just baked (alias-aware for BaseColor's
    // diffuse_map), drop that entry, otherwise the next closeSession()/
    // channel-switch would overwrite the bake with the pre-bake texture
    // (#547 review + bug). Resolve each bound slot's TUS name and prune matches.
    m_boundSlots.erase(
        std::remove_if(m_boundSlots.begin(), m_boundSlots.end(),
            [&](const BoundSlot& bs) {
                auto m = Ogre::MaterialManager::getSingleton().getByName(bs.materialName);
                if (!m || bs.techIdx >= m->getNumTechniques()) return false;
                auto* tech = m->getTechnique(bs.techIdx);
                if (!tech || bs.passIdx >= tech->getNumPasses()) return false;
                auto* pass = tech->getPass(bs.passIdx);
                if (!pass || bs.tusIdx >= pass->getNumTextureUnitStates()) return false;
                return slotMatchesTus(pass->getTextureUnitState(bs.tusIdx)->getName());
            }),
        m_boundSlots.end());

    // Re-attach the HDR IBL SRS so the freshly-bound channel renders under the
    // current environment.
    RTShaderHelper::refreshAllPbrMaterialsForHdr();

    // Do NOT flushDirtyToOgre() here. The bake has committed this channel to a
    // file and bound it into the slot; the live manual paint texture must no
    // longer touch that slot. flushDirtyToOgre() would re-upload the (now
    // stale) paint buffer and — because we just pruned m_boundSlots — its
    // deferred-rebind branch would re-bind the transient paint texture straight
    // back over the freshly-baked file, so the first bake changed the render
    // and a second bake wiped the texture entirely (#547). Instead, tear the
    // live session down cleanly: the model is left sampling the baked files,
    // and the next stroke lazily rebuilds a session seeded FROM the baked
    // result. Stash the just-baked channel's layer stack first so its layers
    // (and undo history) survive the teardown.
    stashChannelSession(m_activeChannel);
    m_buffer.clearDirty();
    closeSession();

    emit sessionChanged();
}

// ---------------------------------------------------------------------------
// Preview URI
// ---------------------------------------------------------------------------

void TexturePaintController::setUvOverlayVisible(bool on)
{
    if (m_uvOverlayVisible == on) return;
    m_uvOverlayVisible = on;
    if (on && m_uvOverlayUri.isEmpty()) refreshUvOverlay();
    emit uvOverlayChanged();
}

void TexturePaintController::refreshUvOverlay()
{
    if (!m_paintMesh || m_buffer.width() <= 0 || m_buffer.height() <= 0) {
        if (!m_uvOverlayUri.isEmpty()) {
            m_uvOverlayUri.clear();
            emit uvOverlayChanged();
        }
        return;
    }
    const int W = m_buffer.width();
    const int H = m_buffer.height();
    QImage img(W, H, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);
    QPen pen(QColor(255, 255, 255, 200));
    // Keep the wireframe a single pixel wide at any resolution.
    pen.setWidth(1);
    pen.setCosmetic(true);
    p.setPen(pen);

    auto toPx = [&](const Ogre::Vector2& uv) {
        return QPointF(uv.x * W, uv.y * H);
    };
    for (const auto& sub : m_paintMesh->subMeshes()) {
        for (const auto& tri : sub.triangles) {
            const auto& v0 = sub.vertices[tri.indices[0]];
            const auto& v1 = sub.vertices[tri.indices[1]];
            const auto& v2 = sub.vertices[tri.indices[2]];
            if (!v0.hasUV || !v1.hasUV || !v2.hasUV) continue;
            const QPointF p0 = toPx(v0.uv);
            const QPointF p1 = toPx(v1.uv);
            const QPointF p2 = toPx(v2.uv);
            p.drawLine(p0, p1);
            p.drawLine(p1, p2);
            p.drawLine(p2, p0);
        }
    }
    p.end();

    QByteArray bytes;
    QBuffer qbuf(&bytes);
    qbuf.open(QIODevice::WriteOnly);
    img.save(&qbuf, "PNG");
    const QString next = QStringLiteral("data:image/png;base64,")
                       + QString::fromLatin1(bytes.toBase64());
    if (next != m_uvOverlayUri) {
        m_uvOverlayUri = next;
        emit uvOverlayChanged();
    }
}

void TexturePaintController::refreshPreviewUri()
{
    // Both the Inspector thumbnail and the detached editor window now
    // consume `fullResPreviewUrl`, which is served by
    // PaintBufferImageProvider (a QImage view of the buffer — no PNG
    // encode, no base64). The legacy `previewDataUri` PNG path is
    // retained as a property for binary compatibility but no longer
    // populated; emitting `previewChanged` keeps any external observer
    // notified without paying the encode cost on the main thread.
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0) {
        if (!m_previewUri.isEmpty()) {
            m_previewUri.clear();
            emit previewChanged();
        }
    }
    ++m_fullResVersion;
    emit fullResPreviewChanged();
}

void TexturePaintController::schedulePreviewRefresh()
{
    if (m_previewRefreshScheduled) return;
    m_previewRefreshScheduled = true;
    const int delayMs = m_strokeActive
        ? (m_strokeFromUvPreview ? 16 : 33)
        : 60;
    QTimer::singleShot(delayMs, this, [this]() {
        m_previewRefreshScheduled = false;
        refreshPreviewUri();
        // Rebuilding the layer list thumbnails is expensive — skip while
        // the brush is down; endStroke emits layersChanged once on release.
        if (m_layerStack.layerCount() > 0 && !m_strokeActive) {
            ++m_layerPreviewVersion;
            emit layersChanged();
        }
    });
}

QString TexturePaintController::fullResPreviewUrl() const
{
    // The trailing `?v=N` invalidates QML's Image cache on each
    // refresh; the provider ignores it. Static `paintbuffer` host
    // is registered in main.cpp so any consumer can bind to this.
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0)
        return {};
    return QStringLiteral("image://paintbuffer/current?v=%1").arg(m_fullResVersion);
}

QImage TexturePaintController::snapshotBufferImage() const
{
    if (m_buffer.width() <= 0 || m_buffer.height() <= 0) return {};
    // Copy out — Qt may hold the QImage across thread boundaries
    // and the underlying m_buffer could mutate (or get freed)
    // between the provider call and the actual GPU upload.
    QImage view(const_cast<uchar*>(m_buffer.data().data()),
                m_buffer.width(), m_buffer.height(),
                m_buffer.width() * 4, QImage::Format_RGBA8888);
    return view.copy();
}

// ---------------------------------------------------------------------------
// 3D-mesh hover ring overlay
// ---------------------------------------------------------------------------

void TexturePaintController::updateMeshHover(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_paintEnabled) { clearMeshHover(); return; }
    // Build the EditableMesh on first hover if it isn't already
    // ready. This handles the "user selected the mesh AFTER enabling
    // paint" case so the brush ring appears as soon as the cursor
    // touches the mesh.
    if (!m_paintMesh || !m_paintMeshEntity) {
        if (auto* e = activeEntity())
            ensureEditableMesh(e);
    }
    Ogre::Vector2 uv;
    if (!hitTestUV(screenPos, widget, uv)) {
        clearMeshHover();
        return;
    }
    emit hoveredUVChanged(uv.x, uv.y);

    // Compute the 3D hit point in local space and a surface normal.
    // We piggyback on the same hit-test math: re-fire a ray and find
    // the closest triangle (same logic as hitTestUV; we keep it inline
    // here to also recover the world-space hit position and normal).
    auto* mesh = m_paintMesh.get();
    auto* entity = m_paintMeshEntity;
    if (!mesh || !entity) return;
    auto* spaceCam = widget->getSpaceCamera();
    auto* camera = spaceCam ? spaceCam->getCamera() : nullptr;
    if (!camera) return;
    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return;
    const Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / vw;
    const Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / vh;
    const Ogre::Ray ray = camera->getCameraToViewportRay(nx, ny);
    Ogre::SceneNode* node = entity->getParentSceneNode();
    Ogre::Affine3 worldToLocal = node ? node->_getFullTransform().inverse() : Ogre::Affine3::IDENTITY;
    Ogre::Vector3 localOrigin = worldToLocal * ray.getOrigin();
    Ogre::Vector3 localDir = worldToLocal.linear() * ray.getDirection();
    localDir.normalise();

    Ogre::Vector3 hitLocal(0,0,0);
    Ogre::Vector3 hitNormal(0,1,0);
    Ogre::Real bestT = std::numeric_limits<Ogre::Real>::infinity();
    for (const auto& sub : mesh->subMeshes()) {
        for (const auto& tri : sub.triangles) {
            const auto& v0 = sub.vertices[tri.indices[0]];
            const auto& v1 = sub.vertices[tri.indices[1]];
            const auto& v2 = sub.vertices[tri.indices[2]];
            const Ogre::Vector3 e1 = v1.position - v0.position;
            const Ogre::Vector3 e2 = v2.position - v0.position;
            const Ogre::Vector3 pvec = localDir.crossProduct(e2);
            const Ogre::Real det = e1.dotProduct(pvec);
            if (std::abs(det) < 1e-8f) continue;
            const Ogre::Real invDet = 1.0f / det;
            const Ogre::Vector3 tvec = localOrigin - v0.position;
            const Ogre::Real u = tvec.dotProduct(pvec) * invDet;
            if (u < 0.0f || u > 1.0f) continue;
            const Ogre::Vector3 qvec = tvec.crossProduct(e1);
            const Ogre::Real v = localDir.dotProduct(qvec) * invDet;
            if (v < 0.0f || u + v > 1.0f) continue;
            const Ogre::Real tHit = e2.dotProduct(qvec) * invDet;
            if (tHit <= 0.0f || tHit >= bestT) continue;
            bestT = tHit;
            hitLocal = localOrigin + localDir * tHit;
            hitNormal = e1.crossProduct(e2);
            if (!hitNormal.isZeroLength()) hitNormal.normalise();
        }
    }
    if (bestT == std::numeric_limits<Ogre::Real>::infinity()) {
        clearMeshHover();
        return;
    }
    drawHoverRingAt(hitLocal, hitNormal);
}

void TexturePaintController::clearMeshHover()
{
    if (m_ringObj) m_ringObj->clear();
    emit hoveredUVChanged(-1.0, -1.0);
}

bool TexturePaintController::hitTestLocalPoint(OgreWidget* widget, const QPoint& screenPos,
                                                Ogre::Vector3& outLocal, Ogre::Vector3& outNormal) const
{
    if (!m_paintMesh || !m_paintMeshEntity || !widget) return false;
    auto* spaceCam = widget->getSpaceCamera();
    auto* camera = spaceCam ? spaceCam->getCamera() : nullptr;
    if (!camera) return false;
    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return false;
    const Ogre::Real nx = static_cast<Ogre::Real>(screenPos.x()) / vw;
    const Ogre::Real ny = static_cast<Ogre::Real>(screenPos.y()) / vh;
    const Ogre::Ray ray = camera->getCameraToViewportRay(nx, ny);
    Ogre::SceneNode* node = m_paintMeshEntity->getParentSceneNode();
    Ogre::Affine3 worldToLocal = node ? node->_getFullTransform().inverse() : Ogre::Affine3::IDENTITY;
    Ogre::Vector3 localOrigin = worldToLocal * ray.getOrigin();
    Ogre::Vector3 localDir = worldToLocal.linear() * ray.getDirection();
    localDir.normalise();
    Ogre::Real bestT = std::numeric_limits<Ogre::Real>::infinity();
    bool found = false;
    for (const auto& sub : m_paintMesh->subMeshes()) {
        for (const auto& tri : sub.triangles) {
            const auto& v0 = sub.vertices[tri.indices[0]];
            const auto& v1 = sub.vertices[tri.indices[1]];
            const auto& v2 = sub.vertices[tri.indices[2]];
            const Ogre::Vector3 e1 = v1.position - v0.position;
            const Ogre::Vector3 e2 = v2.position - v0.position;
            const Ogre::Vector3 pvec = localDir.crossProduct(e2);
            const Ogre::Real det = e1.dotProduct(pvec);
            if (std::abs(det) < 1e-8f) continue;
            const Ogre::Real invDet = 1.0f / det;
            const Ogre::Vector3 tvec = localOrigin - v0.position;
            const Ogre::Real u = tvec.dotProduct(pvec) * invDet;
            if (u < 0.0f || u > 1.0f) continue;
            const Ogre::Vector3 qvec = tvec.crossProduct(e1);
            const Ogre::Real v = localDir.dotProduct(qvec) * invDet;
            if (v < 0.0f || u + v > 1.0f) continue;
            const Ogre::Real tHit = e2.dotProduct(qvec) * invDet;
            if (tHit <= 0.0f || tHit >= bestT) continue;
            bestT = tHit;
            outLocal = localOrigin + localDir * tHit;
            Ogre::Vector3 n = e1.crossProduct(e2);
            if (!n.isZeroLength()) n.normalise();
            else n = Ogre::Vector3::UNIT_Y;
            outNormal = n;
            found = true;
        }
    }
    return found;
}

bool TexturePaintController::wouldStrokeHit(OgreWidget* widget,
                                            const QPoint& screenPos)
{
    if (!widget) return false;
    // Lazily build the paint mesh — beginStroke would do it anyway,
    // and without this the very first click on the model is treated
    // as a miss (m_paintMesh is null until the first stroke). That
    // bug masked the click-outside-clears flow because the seed
    // press never reached beginStroke.
    if (!m_paintMesh || !m_paintMeshEntity) {
        if (auto* e = activeEntity())
            ensureEditableMesh(e);
        if (!m_paintMesh || !m_paintMeshEntity) return false;
    }
    Ogre::Vector2 uv;
    return hitTestUV(screenPos, widget, uv);
}

bool TexturePaintController::findMeshPointForUV(const Ogre::Vector2& uv,
                                                Ogre::Vector3& outLocal,
                                                Ogre::Vector3& outNormal,
                                                int* outSubmesh,
                                                int* outTriangle) const
{
    if (!m_paintMesh) return false;
    // Walk every triangle, find the one whose UV-space contains `uv`
    // (barycentric test on UV triangle), then interpolate the 3D
    // position with those same barycentrics.
    const auto& subs = m_paintMesh->subMeshes();
    for (size_t s = 0; s < subs.size(); ++s) {
        const auto& sub = subs[s];
        for (size_t t = 0; t < sub.triangles.size(); ++t) {
            const auto& tri = sub.triangles[t];
            const auto& v0 = sub.vertices[tri.indices[0]];
            const auto& v1 = sub.vertices[tri.indices[1]];
            const auto& v2 = sub.vertices[tri.indices[2]];
            if (!v0.hasUV || !v1.hasUV || !v2.hasUV) continue;
            // Barycentric coords for `uv` in the UV triangle (v0.uv, v1.uv, v2.uv).
            const Ogre::Vector2 e1 = v1.uv - v0.uv;
            const Ogre::Vector2 e2 = v2.uv - v0.uv;
            const Ogre::Vector2 dp = uv  - v0.uv;
            const float denom = e1.x * e2.y - e2.x * e1.y;
            if (std::abs(denom) < 1e-10f) continue;
            const float u = (dp.x * e2.y - e2.x * dp.y) / denom;
            const float v = (e1.x * dp.y - dp.x * e1.y) / denom;
            const float w = 1.0f - u - v;
            const float eps = 1e-4f;
            if (u < -eps || v < -eps || w < -eps) continue;
            outLocal = v0.position * w + v1.position * u + v2.position * v;
            Ogre::Vector3 n = (v1.position - v0.position)
                .crossProduct(v2.position - v0.position);
            if (!n.isZeroLength()) n.normalise();
            else n = Ogre::Vector3::UNIT_Y;
            outNormal = n;
            if (outSubmesh) *outSubmesh = static_cast<int>(s);
            if (outTriangle) *outTriangle = static_cast<int>(t);
            return true;
        }
    }
    return false;
}

// ===========================================================================
// Paint v2 Slice E (#548) — symmetry mirror + line stabilizer
// ===========================================================================

bool TexturePaintController::uvForLocalPoint(const Ogre::Vector3& local,
                                             Ogre::Vector2& outUV,
                                             int* outSubmesh, int* outTriangle,
                                             float* outBary) const
{
    if (!m_paintMesh) return false;
    // calculateBounds() is O(V); call it once (not thrice) — this runs per
    // mirror dab on the stroke path (CodeRabbit perf).
    const Ogre::Vector3 boundsSize = m_paintMesh->calculateBounds().getSize();
    const float maxExtent = std::max({boundsSize.x, boundsSize.y, boundsSize.z, 1e-4f});
    // Accept a triangle whose plane the point is within `planeTol` of; among all
    // such (and their in-triangle projections) keep the closest 3D distance.
    const float planeTol = maxExtent * 5e-2f;   // generous — mirror point rarely on-surface
    float bestDist = std::numeric_limits<float>::max();
    bool found = false;
    const auto& subs = m_paintMesh->subMeshes();
    for (size_t s = 0; s < subs.size(); ++s) {
        const auto& sub = subs[s];
        for (size_t t = 0; t < sub.triangles.size(); ++t) {
            const auto& tri = sub.triangles[t];
            const auto& v0 = sub.vertices[tri.indices[0]];
            const auto& v1 = sub.vertices[tri.indices[1]];
            const auto& v2 = sub.vertices[tri.indices[2]];
            if (!v0.hasUV || !v1.hasUV || !v2.hasUV) continue;
            const Ogre::Vector3 e1 = v1.position - v0.position;
            const Ogre::Vector3 e2 = v2.position - v0.position;
            Ogre::Vector3 nrm = e1.crossProduct(e2);
            const float area2 = nrm.length();
            if (area2 < 1e-12f) continue;
            nrm /= area2;
            const float planeDist = std::abs((local - v0.position).dotProduct(nrm));
            if (planeDist > planeTol) continue;
            // Barycentric of the projection of `local` onto the triangle plane.
            const Ogre::Vector3 p = local - nrm * (local - v0.position).dotProduct(nrm);
            const Ogre::Vector3 dp = p - v0.position;
            const float d00 = e1.dotProduct(e1), d01 = e1.dotProduct(e2);
            const float d11 = e2.dotProduct(e2);
            const float d20 = dp.dotProduct(e1), d21 = dp.dotProduct(e2);
            const float denom = d00 * d11 - d01 * d01;
            if (std::abs(denom) < 1e-12f) continue;
            const float bu = (d11 * d20 - d01 * d21) / denom;   // weight of v1
            const float bv = (d00 * d21 - d01 * d20) / denom;   // weight of v2
            const float bw = 1.0f - bu - bv;                    // weight of v0
            const float eps = 1e-3f;
            if (bu < -eps || bv < -eps || bw < -eps) continue;
            const float dist = (p - local).length() + planeDist;
            if (dist < bestDist) {
                bestDist = dist;
                outUV = v0.uv * bw + v1.uv * bu + v2.uv * bv;
                if (outSubmesh) *outSubmesh = static_cast<int>(s);
                if (outTriangle) *outTriangle = static_cast<int>(t);
                if (outBary) { outBary[0] = bw; outBary[1] = bu; outBary[2] = bv; }
                found = true;
            }
        }
    }
    return found;
}

Ogre::Vector3 TexturePaintController::reflectLocal(const Ogre::Vector3& p,
                                                   int axisBit,
                                                   const Ogre::Vector3& pivot)
{
    Ogre::Vector3 r = p;
    if (axisBit & SymAxisX) r.x = 2.0f * pivot.x - p.x;
    if (axisBit & SymAxisY) r.y = 2.0f * pivot.y - p.y;
    if (axisBit & SymAxisZ) r.z = 2.0f * pivot.z - p.z;
    return r;
}

std::vector<Ogre::Vector3>
TexturePaintController::mirrorLocalPoints(const Ogre::Vector3& primaryLocal) const
{
    std::vector<Ogre::Vector3> out;
    if (!m_symmetryEnabled || m_symmetryAxes == SymAxisNone) return out;

    // For WORLD space, reflect about the plane through the entity's derived
    // origin: local → world, reflect world coord, world → local.
    Ogre::Matrix4 toWorld = Ogre::Matrix4::IDENTITY;
    Ogre::Matrix4 toLocal = Ogre::Matrix4::IDENTITY;
    Ogre::Vector3 worldPivot = m_symmetryPivotLocal;
    // World space needs the entity's transform; if it (or its node) is missing
    // fall back to LOCAL so we never reflect through an uninitialized matrix
    // (which would send dabs to arbitrary UVs) — CodeRabbit.
    bool world = (m_symmetrySpace == SymWorld);
    if (world) {
        auto* node = m_paintMeshEntity ? m_paintMeshEntity->getParentSceneNode() : nullptr;
        if (node) {
            toWorld = node->_getFullTransform();
            toLocal = toWorld.inverse();
            worldPivot = node->_getDerivedPosition();
        } else {
            world = false;   // fall back to local reflection about the mesh pivot
        }
    }

    const Ogre::Vector3 basis = world
        ? (toWorld * primaryLocal) : primaryLocal;
    const Ogre::Vector3& pivot = world ? worldPivot : m_symmetryPivotLocal;

    // Every nonzero subset of the enabled axis bits → one mirror image.
    for (int subset = 1; subset <= (SymAxisX | SymAxisY | SymAxisZ); ++subset) {
        if ((subset & m_symmetryAxes) != subset) continue;   // only enabled bits
        Ogre::Vector3 m = reflectLocal(basis, subset, pivot);
        if (world) m = toLocal * m;
        out.push_back(m);
    }
    return out;
}

bool TexturePaintController::mirrorUvForLocalPoint(const Ogre::Vector3& mirrorLocal,
                                                   int axisSubset,
                                                   const Ogre::Vector2& primaryUV,
                                                   Ogre::Vector2& outUV)
{
    (void)primaryUV;
    // Topology-aware path: only for a single-axis subset (composed maps handle
    // combos by reflecting sequentially — here we look up the single-axis map
    // matching the subset when it is one bit).
    const bool singleAxis =
        axisSubset == SymAxisX || axisSubset == SymAxisY || axisSubset == SymAxisZ;
    // The topology map is built across a MESH-LOCAL axis. In WORLD space the
    // reflection plane is world-aligned (and differs from any local axis on a
    // rotated entity), so the local-axis map would return the wrong-side UV —
    // use the geometric resolver in world mode. (#548 review.)
    if (m_symmetrySpace == SymLocal
        && m_topologyMirror && singleAxis && m_paintMesh && m_hitCache.valid
        && m_hitCache.submesh >= 0 && m_hitCache.triangle >= 0) {
        // Lazily build (and entity-guard) the per-axis map.
        auto* entity = activeEntity();
        if (entity && m_symmetryMapEntity != entity) {
            m_symmetryMaps.clear();
            m_symmetryMapEntity = entity;
        }
        auto it = m_symmetryMaps.find(axisSubset);
        if (it == m_symmetryMaps.end()) {
            SymmetryMirrorMap map;
            const float diag = m_paintMesh->calculateBounds().getSize().length();
            const float weld = std::max(diag * 1e-3f, 1e-5f);
            const bool ok = map.build(*m_paintMesh, axisSubset,
                                      m_symmetryPivotLocal, weld);
            SentryReporter::addBreadcrumb("paint.symmetry",
                QStringLiteral("topology map axis=%1 built=%2 coverage=%3")
                    .arg(axisSubset).arg(ok).arg(map.coverage(), 0, 'f', 2));
            it = m_symmetryMaps.emplace(axisSubset, std::move(map)).first;
        }
        const SymmetryMirrorMap& map = it->second;
        if (map.valid()) {
            const auto& subs = m_paintMesh->subMeshes();
            const auto& sub = subs[static_cast<size_t>(m_hitCache.submesh)];
            const auto& tri = sub.triangles[static_cast<size_t>(m_hitCache.triangle)];
            // Recover the primary dab's barycentric weights on the cached
            // triangle from the mirror local point's PRE-image is not needed —
            // we need the PRIMARY dab's barycentrics. Recompute them from the
            // cached triangle + the current primary hit via uvForLocalPoint on
            // the primary local point isn't available here; instead compute the
            // primary barycentric directly from the cached triangle geometry.
            // The primary local point is the reflection pre-image; but we only
            // hold mirrorLocal. So derive the primary bary from the hit cache's
            // stored UV path: use the corner indices and let mirrorDab permute.
            const int corner[3] = { static_cast<int>(tri.indices[0]),
                                    static_cast<int>(tri.indices[1]),
                                    static_cast<int>(tri.indices[2]) };
            // Primary barycentric: from the primary local point. We reflect the
            // mirror point back to get the primary local point, then compute
            // bary on the cached triangle.
            const Ogre::Vector3 primaryLocal =
                reflectLocal(mirrorLocal, axisSubset, m_symmetryPivotLocal);
            const auto& p0 = sub.vertices[tri.indices[0]].position;
            const auto& p1 = sub.vertices[tri.indices[1]].position;
            const auto& p2 = sub.vertices[tri.indices[2]].position;
            const Ogre::Vector3 e1 = p1 - p0, e2 = p2 - p0, dpv = primaryLocal - p0;
            const float d00 = e1.dotProduct(e1), d01 = e1.dotProduct(e2);
            const float d11 = e2.dotProduct(e2);
            const float d20 = dpv.dotProduct(e1), d21 = dpv.dotProduct(e2);
            const float denom = d00 * d11 - d01 * d01;
            if (std::abs(denom) > 1e-12f) {
                const float bu = (d11 * d20 - d01 * d21) / denom;
                const float bv = (d00 * d21 - d01 * d20) / denom;
                const float bary[3] = { 1.0f - bu - bv, bu, bv };
                int oSub = -1, oTri = -1; float oBary[3];
                if (map.mirrorDab(m_hitCache.submesh, corner, bary, oSub, oTri, oBary)) {
                    const auto& msub = subs[static_cast<size_t>(oSub)];
                    const auto& mtri = msub.triangles[static_cast<size_t>(oTri)];
                    outUV = msub.vertices[mtri.indices[0]].uv * oBary[0]
                          + msub.vertices[mtri.indices[1]].uv * oBary[1]
                          + msub.vertices[mtri.indices[2]].uv * oBary[2];
                    return true;
                }
            }
        }
    }
    // Geometric fallback.
    return uvForLocalPoint(mirrorLocal, outUV);
}

void TexturePaintController::applyBrushSymmetryDabs(const Ogre::Vector2& primaryUV)
{
    if (!m_symmetryEnabled || m_symmetryAxes == SymAxisNone) return;
    Ogre::Vector3 primaryLocal, primaryNormal;
    // Prefer the screen-raycast hit cache; fall back to the reverse UV lookup so
    // the 2D-panel (UV) paint path mirrors too. The fallback also seeds the hit
    // cache so the topology-aware path can key off the primary triangle.
    if (!localPointFromHitCache(primaryUV, primaryLocal, primaryNormal)) {
        int hs = -1, ht = -1;
        if (!findMeshPointForUV(primaryUV, primaryLocal, primaryNormal, &hs, &ht))
            return;
        m_hitCache.submesh = hs;
        m_hitCache.triangle = ht;
        m_hitCache.valid = true;
    }

    // mirrorLocalPoints returns one mesh-LOCAL mirror point per enabled
    // axis-subset, in ascending-subset order (X, Y, XY, Z, ...). We track the
    // matching subset bitmask so the topology map for that axis can be used.
    const std::vector<Ogre::Vector3> pts = mirrorLocalPoints(primaryLocal);
    const int nSubsets = static_cast<int>(pts.size());
    // Per-subset previous mirror UV, so mirror strokes fan a segment (E-B) and
    // don't gap on fast moves — mirroring the primary path's segment behaviour.
    if (static_cast<int>(m_mirrorPrevUV.size()) != nSubsets) {
        m_mirrorPrevUV.assign(nSubsets, Ogre::Vector2::ZERO);
        m_mirrorHavePrevUV.assign(nSubsets, false);
    }
    const bool canSegment = m_tool != ToolFill && m_tool != ToolColorPicker
                            && m_tool != ToolSmartSelect;

    // The mirror dabs go through paintBrushAlongSegment/applyBrushAtUV, which
    // advance the PRIMARY stroke-path state (m_strokePathLength / dir / prevUV /
    // stamp-dab length) from the mirror UV — those UVs are far from the primary,
    // so without isolation the gradient-along-stroke colour, stamp spacing and
    // direction-following stamp rotation of the *primary* path would corrupt
    // (CodeRabbit). Snapshot that state, let the mirror dabs run, then restore —
    // each mirror subset keeps its own continuity via m_mirrorPrevUV instead.
    const float          savedPathLen   = m_strokePathLength;
    const float          savedStampLen  = m_lastStampDabPathLength;
    const Ogre::Vector2  savedDir       = m_strokeDirSmoothed;
    const Ogre::Vector2  savedPrevUV    = m_strokePrevUV;
    const bool           savedHavePrev  = m_strokeHavePrevUV;

    int idx = 0;
    for (int subset = 1; subset <= (SymAxisX | SymAxisY | SymAxisZ); ++subset) {
        if ((subset & m_symmetryAxes) != subset) continue;
        if (idx >= nSubsets) break;
        Ogre::Vector2 mUV;
        if (mirrorUvForLocalPoint(pts[static_cast<size_t>(idx)], subset, primaryUV, mUV)) {
            bool ch = false;
            if (canSegment && m_mirrorHavePrevUV[static_cast<size_t>(idx)])
                ch = paintBrushAlongSegment(m_mirrorPrevUV[static_cast<size_t>(idx)], mUV);
            else
                ch = applyBrushAtUV(mUV);
            if (ch) m_strokeMadeChanges = true;
            m_mirrorPrevUV[static_cast<size_t>(idx)] = mUV;
            m_mirrorHavePrevUV[static_cast<size_t>(idx)] = true;
        }
        ++idx;
    }

    // Restore the primary stroke-path state the mirror dabs perturbed.
    m_strokePathLength        = savedPathLen;
    m_lastStampDabPathLength  = savedStampLen;
    m_strokeDirSmoothed       = savedDir;
    m_strokePrevUV            = savedPrevUV;
    m_strokeHavePrevUV        = savedHavePrev;
}

void TexturePaintController::invalidateSymmetryMaps()
{
    m_symmetryMaps.clear();
    m_symmetryMapEntity = nullptr;
}

int TexturePaintController::stabilizerWindow(double amount)
{
    const double a = std::clamp(amount, 0.0, 100.0) / 100.0;
    const int Nmax = 24;
    return 1 + static_cast<int>(std::lround(a * (Nmax - 1)));
}

QPointF TexturePaintController::stabilizeAveragePoint(const std::deque<QPointF>& samples,
                                                      int window)
{
    if (samples.empty()) return QPointF();
    const int n = static_cast<int>(samples.size());
    const int start = std::max(0, n - std::max(1, window));
    double wx = 0.0, wy = 0.0, wsum = 0.0;
    for (int i = start; i < n; ++i) {
        const double w = static_cast<double>(i - start + 1);   // newest heaviest
        wx += samples[static_cast<size_t>(i)].x() * w;
        wy += samples[static_cast<size_t>(i)].y() * w;
        wsum += w;
    }
    return (wsum > 0.0) ? QPointF(wx / wsum, wy / wsum) : samples.back();
}

QPointF TexturePaintController::stabilizeTrailPoint(const QPointF& trail,
                                                    const QPointF& raw, double lag)
{
    const QPointF d = raw - trail;
    const double dist = std::hypot(d.x(), d.y());
    if (dist > lag && dist > 1e-6)
        return trail + d * ((dist - lag) / dist);
    return trail;
}

QPointF TexturePaintController::stabilizeScreen(const QPointF& raw)
{
    m_stabLastRaw = raw;
    m_stabHaveLastRaw = true;
    if (m_stabilizerAmount <= 0.0) {          // passthrough — zero latency
        m_stabSamples.clear();
        m_stabHaveTrail = false;
        return raw;
    }
    if (m_stabilizerMode == StabTrail) {
        const double L = (m_stabilizerAmount / 100.0) * 60.0;   // Lmax = 60 px
        if (!m_stabHaveTrail) { m_stabTrailPos = raw; m_stabHaveTrail = true; }
        m_stabTrailPos = stabilizeTrailPoint(m_stabTrailPos, raw, L);
        return m_stabTrailPos;
    }
    const int N = stabilizerWindow(m_stabilizerAmount);
    m_stabSamples.push_back(raw);
    while (static_cast<int>(m_stabSamples.size()) > N) m_stabSamples.pop_front();
    return stabilizeAveragePoint(m_stabSamples, N);
}

void TexturePaintController::refreshSymmetryPlaneOverlay()
{
    auto* entity = m_paintMeshEntity;
    auto* sceneMgr = (entity ? entity->_getManager() : nullptr);
    if (!sceneMgr) sceneMgr = Manager::getSingletonPtr()
                       ? Manager::getSingletonPtr()->getSceneMgr() : nullptr;
    if (!sceneMgr) return;

    const bool show = m_symmetryEnabled && m_symmetryAxes != SymAxisNone
                      && hasActiveSession() && m_paintMesh;

    if (!show) {
        if (m_symPlaneObj) m_symPlaneObj->clear();
        if (m_symPlaneNode) m_symPlaneNode->setVisible(false);
        return;
    }

    static const char* kMat = "TexturePaint/SymPlane";
    auto& matMgr = Ogre::MaterialManager::getSingleton();
    if (!matMgr.getByName(kMat)) {
        auto mat = matMgr.create(kMat, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        pass->setDepthWriteEnabled(false);
        pass->setCullingMode(Ogre::CULL_NONE);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
    }
    if (!m_symPlaneNode)
        m_symPlaneNode = sceneMgr->getRootSceneNode()->createChildSceneNode();
    if (!m_symPlaneObj) {
        m_symPlaneObj = sceneMgr->createManualObject("TexturePaint_SymPlane");
        m_symPlaneObj->setDynamic(true);
        m_symPlaneObj->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);
        m_symPlaneNode->attachObject(m_symPlaneObj);
    }
    // Local symmetry mirrors about the mesh-local axes, so the plane inherits
    // the entity's full transform (renders in the mesh frame). World symmetry
    // mirrors about WORLD-aligned axes through the entity's derived origin, so
    // the overlay must be world-aligned — position at the derived origin but
    // with identity orientation/scale — else the guide shows a different plane
    // than the one that controls painting on a rotated model. (#548 review.)
    if (auto* node = entity ? entity->getParentSceneNode() : nullptr) {
        m_symPlaneNode->setPosition(node->_getDerivedPosition());
        if (m_symmetrySpace == SymWorld) {
            m_symPlaneNode->setOrientation(Ogre::Quaternion::IDENTITY);
            m_symPlaneNode->setScale(Ogre::Vector3::UNIT_SCALE);
        } else {
            m_symPlaneNode->setOrientation(node->_getDerivedOrientation());
            m_symPlaneNode->setScale(node->_getDerivedScale());
        }
    }
    const Ogre::AxisAlignedBox bb = m_paintMesh->calculateBounds();
    const Ogre::Vector3 ext = bb.getSize() * 0.6f + Ogre::Vector3(1e-3f, 1e-3f, 1e-3f);
    const Ogre::Vector3 c = m_symmetryPivotLocal;

    m_symPlaneObj->clear();
    m_symPlaneObj->begin(kMat, Ogre::RenderOperation::OT_TRIANGLE_LIST);
    auto quad = [&](const Ogre::Vector3& a, const Ogre::Vector3& b,
                    const Ogre::Vector3& d, const Ogre::Vector3& e,
                    const Ogre::ColourValue& col) {
        const int base = static_cast<int>(m_symPlaneObj->getCurrentVertexCount());
        for (const auto& v : {a, b, d, e}) { m_symPlaneObj->position(v); m_symPlaneObj->colour(col); }
        m_symPlaneObj->triangle(base, base + 1, base + 2);
        m_symPlaneObj->triangle(base, base + 2, base + 3);
    };
    const float alpha = 0.12f;
    if (m_symmetryAxes & SymAxisX)   // plane x=c.x, spans Y/Z
        quad({c.x, c.y - ext.y, c.z - ext.z}, {c.x, c.y + ext.y, c.z - ext.z},
             {c.x, c.y + ext.y, c.z + ext.z}, {c.x, c.y - ext.y, c.z + ext.z},
             Ogre::ColourValue(1, 0.2f, 0.2f, alpha));
    if (m_symmetryAxes & SymAxisY)   // plane y=c.y, spans X/Z
        quad({c.x - ext.x, c.y, c.z - ext.z}, {c.x + ext.x, c.y, c.z - ext.z},
             {c.x + ext.x, c.y, c.z + ext.z}, {c.x - ext.x, c.y, c.z + ext.z},
             Ogre::ColourValue(0.2f, 1, 0.2f, alpha));
    if (m_symmetryAxes & SymAxisZ)   // plane z=c.z, spans X/Y
        quad({c.x - ext.x, c.y - ext.y, c.z}, {c.x + ext.x, c.y - ext.y, c.z},
             {c.x + ext.x, c.y + ext.y, c.z}, {c.x - ext.x, c.y + ext.y, c.z},
             Ogre::ColourValue(0.3f, 0.3f, 1, alpha));
    m_symPlaneObj->end();
    m_symPlaneNode->setVisible(true);
}

void TexturePaintController::refreshDecalOverlay()
{
    auto* entity = m_paintMeshEntity;
    auto* sceneMgr = entity ? entity->_getManager() : nullptr;
    if (!sceneMgr) sceneMgr = Manager::getSingletonPtr()
                       ? Manager::getSingletonPtr()->getSceneMgr() : nullptr;
    if (!sceneMgr) return;

    const bool show = m_decal.state() == DecalSession::State::Editing;
    if (!show) {
        if (m_decalObj) m_decalObj->clear();
        if (m_decalNode) m_decalNode->setVisible(false);
        return;
    }

    static const char* kMat = "TexturePaint/DecalRect";
    auto& matMgr = Ogre::MaterialManager::getSingleton();
    if (!matMgr.getByName(kMat)) {
        auto mat = matMgr.create(kMat, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        pass->setDepthWriteEnabled(false);
        pass->setDepthCheckEnabled(false);         // draw on top so handles are grabbable
        pass->setCullingMode(Ogre::CULL_NONE);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
    }
    if (!m_decalNode)
        m_decalNode = sceneMgr->getRootSceneNode()->createChildSceneNode();
    if (!m_decalObj) {
        m_decalObj = sceneMgr->createManualObject("TexturePaint_DecalRect");
        m_decalObj->setDynamic(true);
        m_decalObj->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);
        m_decalNode->attachObject(m_decalObj);
    }
    // The overlay is in WORLD space (the rect stores world corners).
    m_decalNode->setPosition(Ogre::Vector3::ZERO);
    m_decalNode->setOrientation(Ogre::Quaternion::IDENTITY);
    m_decalNode->setScale(Ogre::Vector3::UNIT_SCALE);

    Ogre::Vector3 c[4];
    m_decal.corners(c);
    const Ogre::ColourValue line(1.0f, 0.9f, 0.2f, 0.9f);   // yellow outline
    const Ogre::ColourValue fill(1.0f, 0.9f, 0.2f, 0.12f);
    const Ogre::ColourValue hCol(0.2f, 0.8f, 1.0f, 0.95f);  // cyan handles

    m_decalObj->clear();
    // Translucent body (two tris).
    m_decalObj->begin(kMat, Ogre::RenderOperation::OT_TRIANGLE_LIST);
    for (int k : {0, 1, 2, 0, 2, 3}) { m_decalObj->position(c[k]); m_decalObj->colour(fill); }
    m_decalObj->end();
    // Outline (line strip).
    m_decalObj->begin(kMat, Ogre::RenderOperation::OT_LINE_STRIP);
    for (int k : {0, 1, 2, 3, 0}) { m_decalObj->position(c[k]); m_decalObj->colour(line); }
    m_decalObj->end();
    // Handle squares at the 4 corners (rotate) + 4 edge midpoints (scale).
    const Ogre::Vector3 hu = m_decal.rect().tangentU;
    const Ogre::Vector3 hv = m_decal.rect().tangentV;
    const float hs = 0.06f * (hu.length() + hv.length());   // handle half-size
    auto drawHandle = [&](const Ogre::Vector3& ctr) {
        const Ogre::Vector3 du = (hu.length() > 1e-6f ? hu.normalisedCopy() : Ogre::Vector3::UNIT_X) * hs;
        const Ogre::Vector3 dv = (hv.length() > 1e-6f ? hv.normalisedCopy() : Ogre::Vector3::UNIT_Y) * hs;
        const Ogre::Vector3 q0 = ctr - du - dv, q1 = ctr + du - dv,
                            q2 = ctr + du + dv, q3 = ctr - du + dv;
        m_decalObj->begin(kMat, Ogre::RenderOperation::OT_TRIANGLE_LIST);
        for (const auto& v : {q0, q1, q2, q0, q2, q3}) { m_decalObj->position(v); m_decalObj->colour(hCol); }
        m_decalObj->end();
    };
    for (int k = 0; k < 4; ++k) drawHandle(c[k]);                       // corners
    drawHandle(m_decal.rect().center + hu);                             // +U edge
    drawHandle(m_decal.rect().center - hu);                             // -U edge
    drawHandle(m_decal.rect().center + hv);                             // +V edge
    drawHandle(m_decal.rect().center - hv);                             // -V edge
    m_decalNode->setVisible(true);
}

void TexturePaintController::drawHoverRingAt(const Ogre::Vector3& localPos,
                                              const Ogre::Vector3& localNormal)
{
    auto* entity = m_paintMeshEntity;
    if (!entity) return;
    auto* sceneMgr = entity->_getManager();
    if (!sceneMgr) return;

    // Ensure the hover-line material exists. It's normally created
    // when entering Edit Mode (EditModeController::createOverlayMaterials);
    // we may need to draw the ring before the user has ever entered
    // Edit Mode, so set it up here too.
    static const char* kMatName = "TexturePaint/HoverRing";
    auto& matMgr = Ogre::MaterialManager::getSingleton();
    if (!matMgr.getByName(kMatName)) {
        auto mat = matMgr.create(kMatName,
            Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        pass->setDepthCheckEnabled(false);
        pass->setDepthWriteEnabled(false);
        pass->setLineWidth(2.0f);
    }
    if (!m_ringNode) m_ringNode = sceneMgr->getRootSceneNode()->createChildSceneNode();
    if (!m_ringObj) {
        m_ringObj = sceneMgr->createManualObject("TexturePaint_HoverRing");
        m_ringObj->setDynamic(true);
        m_ringObj->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
        m_ringNode->attachObject(m_ringObj);
    }
    auto* node = entity->getParentSceneNode();
    if (node) {
        m_ringNode->setPosition(node->_getDerivedPosition());
        m_ringNode->setOrientation(node->_getDerivedOrientation());
        m_ringNode->setScale(node->_getDerivedScale());
    }
    m_ringObj->clear();

    Ogre::Vector3 normal = localNormal;
    if (normal.squaredLength() < 1e-6f) normal = Ogre::Vector3::UNIT_Y;
    Ogre::Vector3 tangent = normal.perpendicular();
    tangent.normalise();
    Ogre::Vector3 bitangent = normal.crossProduct(tangent);
    bitangent.normalise();

    const QColor c = texturePaintColor();
    const Ogre::ColourValue ringCol(c.redF(), c.greenF(), c.blueF(), 0.95f);
    // The brush radius is in local mesh units (shared with vertex paint).
    // For the circular overlay we narrow it slightly (0.8x) so the ring
    // visually matches the painted footprint — a softly-falloff brush
    // doesn't quite touch the ring edge. The square overlay represents
    // the AABB cube footprint exactly, so it uses the full radius.
    const Ogre::Vector3 center = localPos + normal * 0.001f;

    const auto* em = EditModeController::instance();
    const bool square = em && em->vertexPaintShape() == EditModeController::ShapeSquare;
    if (square) {
        const float radius = static_cast<float>(texturePaintRadius());
        // Four corners of the local tangent-plane square, drawn as a
        // closed line strip. Tangent + bitangent are perpendicular by
        // construction so the corners are axis-aligned in the surface
        // frame. NB: applyVertexColorBrush uses an AABB in mesh-local
        // X/Y/Z space, not the surface frame — this overlay is a
        // close visual approximation that stays simple, not an exact
        // projection of that cube. Users want a hint of "square
        // brush", which this delivers.
        const Ogre::Vector3 p0 = center + ( tangent + bitangent) * radius;
        const Ogre::Vector3 p1 = center + (-tangent + bitangent) * radius;
        const Ogre::Vector3 p2 = center + (-tangent - bitangent) * radius;
        const Ogre::Vector3 p3 = center + ( tangent - bitangent) * radius;
        m_ringObj->begin(kMatName, Ogre::RenderOperation::OT_LINE_STRIP);
        m_ringObj->position(p0); m_ringObj->colour(ringCol);
        m_ringObj->position(p1); m_ringObj->colour(ringCol);
        m_ringObj->position(p2); m_ringObj->colour(ringCol);
        m_ringObj->position(p3); m_ringObj->colour(ringCol);
        m_ringObj->position(p0); m_ringObj->colour(ringCol);
        m_ringObj->end();
    } else {
        const float radius = static_cast<float>(texturePaintRadius()) * 0.8f;
        constexpr int kSegments = 64;
        m_ringObj->begin(kMatName, Ogre::RenderOperation::OT_LINE_STRIP);
        for (int i = 0; i <= kSegments; ++i) {
            const float t = static_cast<float>(i) / kSegments;
            const float a = Ogre::Math::TWO_PI * t;
            const Ogre::Vector3 p = center
                + (tangent * Ogre::Math::Cos(a) + bitangent * Ogre::Math::Sin(a)) * radius;
            m_ringObj->position(p);
            m_ringObj->colour(ringCol);
        }
        m_ringObj->end();
    }
}

// ---------------------------------------------------------------------------
// Smart-select / selection-mask API
// ---------------------------------------------------------------------------

void TexturePaintController::setSmartSelectTolerance(double t)
{
    const double clamped = std::clamp(t, 0.0, 1.0);
    if (m_smartSelectTolerance == clamped) return;
    m_smartSelectTolerance = clamped;
    emit smartSelectChanged();
}

bool TexturePaintController::hasSelectionMask() const
{
    return !m_mask.isEmpty();
}

int TexturePaintController::selectedPixelCount() const
{
    return m_mask.selectedCount();
}

void TexturePaintController::clearSelectionMask()
{
    if (m_mask.isEmpty()) return;
    m_mask.clear();
    m_maskOverlayUri.clear();
    destroyMeshMaskOverlay();
    SentryReporter::addBreadcrumb("ui.action", "Smart select: cleared");
    emit smartSelectChanged();
}

void TexturePaintController::selectAllMask()
{
    if (!hasActiveSession()) return;
    if (m_mask.width() != m_buffer.width() || m_mask.height() != m_buffer.height())
        m_mask.resize(m_buffer.width(), m_buffer.height());
    m_mask.selectAll();
    SentryReporter::addBreadcrumb("ui.action", "Smart select: select all");
    scheduleMaskOverlayRefresh();
    emit smartSelectChanged();
}

void TexturePaintController::invertSelectionMask()
{
    if (!hasActiveSession()) return;
    if (m_mask.width() != m_buffer.width() || m_mask.height() != m_buffer.height())
        m_mask.resize(m_buffer.width(), m_buffer.height());
    m_mask.invert();
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Smart select: invert (%1 px now)").arg(m_mask.selectedCount()));
    scheduleMaskOverlayRefresh();
    emit smartSelectChanged();
}

int TexturePaintController::smartSelectAtUV(double u, double v, int mode)
{
    if (!hasActiveSession()) return 0;
    if (m_mask.width() != m_buffer.width() || m_mask.height() != m_buffer.height())
        m_mask.resize(m_buffer.width(), m_buffer.height());

    int sx = 0, sy = 0;
    m_buffer.uvToPixel(Ogre::Vector2(static_cast<float>(u), static_cast<float>(v)),
                       sx, sy);
    const auto cmode = (mode == 1) ? PaintSelectionMask::CombineMode::Add
                     : (mode == 2) ? PaintSelectionMask::CombineMode::Sub
                                   : PaintSelectionMask::CombineMode::Replace;
    const int affected = m_mask.smartSelect(m_buffer, sx, sy,
                                            static_cast<float>(m_smartSelectTolerance),
                                            cmode);
    if (affected > 0) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Smart select: %1 px (mode=%2, tol=%3)")
                .arg(affected).arg(mode).arg(m_smartSelectTolerance, 0, 'f', 2));
        scheduleMaskOverlayRefresh();
        emit smartSelectChanged();
    }
    return affected;
}

namespace {

// Apply `apply` to every pixel in `mask`. Returns count of affected
// pixels. Caller owns the before/after snapshots for undo.
template <typename F>
int applyToMaskedPixels(TexturePaintBuffer& buf, const PaintSelectionMask& mask, F&& apply)
{
    if (mask.isEmpty()) return 0;
    const int W = buf.width();
    const int H = buf.height();
    if (mask.width() != W || mask.height() != H) return 0;
    const auto& maskData = mask.data();
    auto& px = buf.data();
    const auto& bb = mask.bbox();
    int affected = 0;
    for (int y = bb.y0; y < bb.y1; ++y) {
        for (int x = bb.x0; x < bb.x1; ++x) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
            if (!maskData[i]) continue;
            const size_t off = i * 4u;
            apply(px[off + 0], px[off + 1], px[off + 2], px[off + 3]);
            ++affected;
        }
    }
    if (affected > 0)
        buf.markDirty(bb.x0, bb.y0, bb.x1, bb.y1);
    return affected;
}

} // namespace

int TexturePaintController::fillMaskWithFG()
{
    if (!hasActiveSession() || !hasSelectionMask()) return 0;
    auto& layerBuf = activePaintBuffer();
    auto before = layerBuf.data();
    const QColor c = texturePaintColor();
    const uint8_t fr = static_cast<uint8_t>(std::lround(c.redF()   * 255.0));
    const uint8_t fg = static_cast<uint8_t>(std::lround(c.greenF() * 255.0));
    const uint8_t fb = static_cast<uint8_t>(std::lround(c.blueF()  * 255.0));
    const uint8_t fa = static_cast<uint8_t>(std::lround(c.alphaF() * 255.0));
    const int affected = applyToMaskedPixels(layerBuf, m_mask,
        [&](uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
            r = fr; g = fg; b = fb; a = fa;
        });
    if (affected <= 0) return 0;
    const int layerIdx = m_layerStack.layerCount() > 0 ? m_layerStack.activeIndex() : 0;
    UndoManager::getSingleton()->push(new TexturePaintMaskActionCommand(
        this, layerIdx, std::move(before), layerBuf.data(),
        layerBuf.width(), layerBuf.height(),
        (m_sessionEntity ? m_sessionEntity->getName() : std::string()),
        static_cast<int>(m_activeChannel),
        QStringLiteral("Fill selection (FG)")));
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Smart select: filled %1 px with FG %2")
            .arg(affected).arg(c.name(QColor::HexRgb)));
    flushDirtyToOgre();
    updateEmbeddedTextureCache();
    return affected;
}

int TexturePaintController::fillMaskWithBG()
{
    if (!hasActiveSession() || !hasSelectionMask()) return 0;
    auto& layerBuf = activePaintBuffer();
    auto before = layerBuf.data();
    const QColor c = bgPaintColor();
    const uint8_t fr = static_cast<uint8_t>(std::lround(c.redF()   * 255.0));
    const uint8_t fg = static_cast<uint8_t>(std::lround(c.greenF() * 255.0));
    const uint8_t fb = static_cast<uint8_t>(std::lround(c.blueF()  * 255.0));
    const uint8_t fa = static_cast<uint8_t>(std::lround(c.alphaF() * 255.0));
    const int affected = applyToMaskedPixels(layerBuf, m_mask,
        [&](uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
            r = fr; g = fg; b = fb; a = fa;
        });
    if (affected <= 0) return 0;
    const int layerIdx = m_layerStack.layerCount() > 0 ? m_layerStack.activeIndex() : 0;
    UndoManager::getSingleton()->push(new TexturePaintMaskActionCommand(
        this, layerIdx, std::move(before), layerBuf.data(),
        layerBuf.width(), layerBuf.height(),
        (m_sessionEntity ? m_sessionEntity->getName() : std::string()),
        static_cast<int>(m_activeChannel),
        QStringLiteral("Fill selection (BG)")));
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Smart select: filled %1 px with BG %2")
            .arg(affected).arg(c.name(QColor::HexArgb)));
    flushDirtyToOgre();
    updateEmbeddedTextureCache();
    return affected;
}

int TexturePaintController::deleteMaskPixels()
{
    if (!hasActiveSession() || !hasSelectionMask()) return 0;
    auto& layerBuf = activePaintBuffer();
    auto before = layerBuf.data();
    const int affected = applyToMaskedPixels(layerBuf, m_mask,
        [](uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
            r = 0; g = 0; b = 0; a = 0;
        });
    if (affected <= 0) return 0;
    const int layerIdx = m_layerStack.layerCount() > 0 ? m_layerStack.activeIndex() : 0;
    UndoManager::getSingleton()->push(new TexturePaintMaskActionCommand(
        this, layerIdx, std::move(before), layerBuf.data(),
        layerBuf.width(), layerBuf.height(),
        (m_sessionEntity ? m_sessionEntity->getName() : std::string()),
        static_cast<int>(m_activeChannel),
        QStringLiteral("Delete selection")));
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("Smart select: deleted %1 px").arg(affected));
    flushDirtyToOgre();
    updateEmbeddedTextureCache();
    return affected;
}

void TexturePaintController::scheduleMaskOverlayRefresh()
{
    if (m_maskOverlayRefreshScheduled) return;
    m_maskOverlayRefreshScheduled = true;
    QTimer::singleShot(60, this, [this]() {
        m_maskOverlayRefreshScheduled = false;
        refreshMaskOverlay();
    });
}

void TexturePaintController::refreshMaskOverlay()
{
    // Keep the on-mesh 3D overlay in sync with the mask. Doing it here
    // (the debounced path) covers smartSelect, selectAll, and invert
    // without each caller having to remember to refresh both layers.
    refreshMeshMaskOverlay();

    const int W = m_mask.width();
    const int H = m_mask.height();
    if (W <= 0 || H <= 0 || m_mask.isEmpty()) {
        if (!m_maskOverlayUri.isEmpty()) {
            m_maskOverlayUri.clear();
            emit smartSelectChanged();
        }
        return;
    }
    // Render the mask as a high-contrast translucent overlay: yellow tint
    // inside, black 1px outline at the boundary (any selected pixel with
    // an unselected 4-neighbor). This is the "marching ants" mock —
    // animation comes from the QML side if we add it later.
    const int previewW = std::min(W, 512);
    const int previewH = std::min(H, 512);
    const float sx = static_cast<float>(W) / previewW;
    const float sy = static_cast<float>(H) / previewH;
    // ARGB32 (not RGBA8888) so qRgba's 0xAARRGGBB packing maps to the
    // image's bytes correctly. With RGBA8888 the channels are
    // re-ordered to R,G,B,A in memory and the values written via qRgba
    // come out swizzled (yellow → light blue).
    QImage img(previewW, previewH, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    const auto& d = m_mask.data();
    auto sel = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= W || y >= H) return false;
        return d[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] != 0;
    };
    for (int py = 0; py < previewH; ++py) {
        const int y = std::min(H - 1, static_cast<int>(py * sy));
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(py));
        for (int px = 0; px < previewW; ++px) {
            const int x = std::min(W - 1, static_cast<int>(px * sx));
            const bool inside = sel(x, y);
            if (!inside) continue;
            const bool boundary = !sel(x - 1, y) || !sel(x + 1, y)
                               || !sel(x, y - 1) || !sel(x, y + 1);
            line[px] = boundary ? qRgba(0, 0, 0, 220) : qRgba(255, 240, 0, 80);
        }
    }
    QByteArray bytes;
    QBuffer qbuf(&bytes);
    qbuf.open(QIODevice::WriteOnly);
    if (!img.save(&qbuf, "PNG")) return;
    m_maskOverlayUri = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
    emit smartSelectChanged();
}

// ---------------------------------------------------------------------------
// Detached texture editor window
// ---------------------------------------------------------------------------

void TexturePaintController::openEditorWindow()
{
    if (m_editorWindow) {
        // Already open — raise / show.
        if (auto* w = qobject_cast<QQuickWindow*>(m_editorWindow)) {
            w->show();
            w->raise();
            w->requestActivate();
        }
        return;
    }
    // Use a QQmlApplicationEngine so the loaded Window registers as a
    // top-level (matches MaterialEditor's pattern). We also need to
    // re-register the PropertiesPanel singleton in this new engine so
    // the imported singleton resolves at QML load time — main.cpp's
    // registrations are per-engine in Qt 6.
    auto* engine = new QQmlApplicationEngine(this);
    const QString appDir = QCoreApplication::applicationDirPath();
    engine->addImportPath(appDir + "/qml");
    engine->addImportPath(QLibraryInfo::path(QLibraryInfo::QmlImportsPath));

    // Register the paintbuffer image provider so the editor window's
    // <Image source="image://paintbuffer/current?v=…"/> can fetch
    // the live buffer without a PNG round-trip. Engine takes
    // ownership of the provider via QQmlEngine::addImageProvider.
    engine->addImageProvider(QStringLiteral("paintbuffer"),
                             new PaintBufferImageProvider());

    qmlRegisterSingletonType<TexturePaintController>(
        "PropertiesPanel", 1, 0, "TexturePaintController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* {
            return TexturePaintController::qmlInstance(e, nullptr);
        });

    bool handled = false;
    connect(engine, &QQmlApplicationEngine::objectCreated, this,
        [this, engine, &handled](QObject* obj, const QUrl&) {
            handled = true;
            if (!obj) {
                SentryReporter::addBreadcrumb("ui.action",
                    QStringLiteral("Texture editor window: QML load failed"));
                engine->deleteLater();
                return;
            }
            m_editorWindow = obj;
            if (auto* w = qobject_cast<QQuickWindow*>(obj)) {
                connect(w, &QQuickWindow::visibleChanged, this,
                    [this, w, engine](bool vis) {
                        if (vis || m_editorWindow != w) return;
                        m_editorWindow = nullptr;
                        emit editorWindowChanged();
                        engine->deleteLater();
                    });
                w->show();
                w->raise();
                w->requestActivate();
            }
            emit editorWindowChanged();
        }, Qt::DirectConnection);

    engine->load(QUrl(QStringLiteral("qrc:/PropertiesPanel/TextureEditorWindow.qml")));
    if (!handled) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("Texture editor window: load() returned without objectCreated firing"));
    }
    SentryReporter::addBreadcrumb("ui.action", "Texture editor window opened");
}

void TexturePaintController::closeEditorWindow()
{
    if (!m_editorWindow) return;
    if (auto* w = qobject_cast<QQuickWindow*>(m_editorWindow)) {
        w->close();
        // visibleChanged handler will null m_editorWindow + emit.
    } else {
        m_editorWindow->deleteLater();
        m_editorWindow = nullptr;
        emit editorWindowChanged();
    }
}

// ---------------------------------------------------------------------------
// On-mesh wand-selection overlay
// ---------------------------------------------------------------------------

void TexturePaintController::refreshMeshMaskOverlay()
{
    if (!m_paintMeshEntity || m_mask.isEmpty()) {
        destroyMeshMaskOverlay();
        return;
    }
    auto* entity = m_paintMeshEntity;
    auto* sceneMgr = entity->_getManager();
    auto* parentNode = entity->getParentSceneNode();
    if (!sceneMgr || !parentNode) return;

    // (1) Build / refresh the overlay GPU texture from the mask data.
    //     Match the 2D preview's marching-ants palette so the 3D and
    //     2D views read as the same selection: yellow tint inside
    //     (255,240,0 α≈80), black outline (0,0,0 α≈220) on any
    //     selected pixel adjacent to an unselected one. Boundary
    //     test is the same 4-neighbor as refreshMaskOverlay.
    const int W = m_mask.width();
    const int H = m_mask.height();
    if (W <= 0 || H <= 0) {
        destroyMeshMaskOverlay();
        return;
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(W) * static_cast<size_t>(H) * 4u, 0);
    const auto& d = m_mask.data();
    auto sel = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= W || y >= H) return false;
        return d[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] != 0;
    };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
            if (!d[i]) continue;
            const bool boundary = !sel(x - 1, y) || !sel(x + 1, y)
                               || !sel(x, y - 1) || !sel(x, y + 1);
            const size_t off = i * 4u;
            if (boundary) {
                rgba[off + 0] = 0;
                rgba[off + 1] = 0;
                rgba[off + 2] = 0;
                rgba[off + 3] = 220;
            } else {
                rgba[off + 0] = 255;
                rgba[off + 1] = 240;
                rgba[off + 2] = 0;
                rgba[off + 3] = 80;
            }
        }
    }
    const std::string texName = "QMEPaintMaskOverlay_"
        + std::to_string(reinterpret_cast<uintptr_t>(this));
    auto& texMgr = Ogre::TextureManager::getSingleton();
    if (m_maskOverlayTex && (static_cast<int>(m_maskOverlayTex->getWidth()) != W
                          || static_cast<int>(m_maskOverlayTex->getHeight()) != H)) {
        // Size changed — drop and recreate.
        try { texMgr.remove(m_maskOverlayTex); } catch (...) {}
        m_maskOverlayTex.reset();
    }
    if (!m_maskOverlayTex) {
        m_maskOverlayTex = texMgr.createManual(
            texName,
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
            Ogre::TEX_TYPE_2D, W, H, 0,
            Ogre::PF_BYTE_RGBA,
            Ogre::TU_DYNAMIC_WRITE_ONLY);
    }
    try {
        auto buf = m_maskOverlayTex->getBuffer();
        if (buf) {
            Ogre::PixelBox pb(W, H, 1, Ogre::PF_BYTE_RGBA, rgba.data());
            buf->blitFromMemory(pb);
        }
    } catch (...) {}

    // (2) Build / fetch the unlit transparent material that samples
    //     the overlay texture. One material per controller — reused
    //     across refreshes.
    if (m_maskOverlayMatName.empty()) {
        m_maskOverlayMatName = "QMEPaintMaskOverlay_Mat_"
            + std::to_string(reinterpret_cast<uintptr_t>(this));
    }
    auto& matMgr = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr mat = matMgr.getByName(m_maskOverlayMatName);
    if (!mat) {
        mat = matMgr.create(m_maskOverlayMatName,
            Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* tech = mat->getTechnique(0);
        auto* pass = tech->getPass(0);
        pass->setLightingEnabled(false);
        pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        pass->setDepthWriteEnabled(false);
        pass->setDepthCheckEnabled(true);
        pass->setDepthBias(1.0f, 1.0f);   // tiny z-pull-in so we sit on top
        pass->setCullingMode(Ogre::CULL_NONE);
        auto* tus = pass->createTextureUnitState(m_maskOverlayTex->getName());
        tus->setTextureFiltering(Ogre::TFO_NONE);
    } else {
        // Re-point the existing TUS at the (possibly resized) texture.
        try {
            auto* pass = mat->getTechnique(0)->getPass(0);
            if (pass->getNumTextureUnitStates() > 0) {
                pass->getTextureUnitState(0)->setTextureName(m_maskOverlayTex->getName());
            }
        } catch (...) {}
    }

    // (3) Create / refresh the duplicate Entity that draws the mesh
    //     with the overlay material. Sharing the source MeshPtr means
    //     the verts / UVs / animation match the base mesh for free.
    if (!m_maskOverlayEntity) {
        try {
            const std::string entName = "QMEPaintMaskOverlay_Ent_"
                + std::to_string(reinterpret_cast<uintptr_t>(this));
            m_maskOverlayEntity = sceneMgr->createEntity(entName, entity->getMesh()->getName());
            m_maskOverlayEntity->setMaterialName(m_maskOverlayMatName);
            m_maskOverlayEntity->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);
            m_maskOverlayEntity->setCastShadows(false);
            m_maskOverlayEntity->setQueryFlags(0);
        } catch (const Ogre::Exception& e) {
            SentryReporter::addBreadcrumb("ui.action",
                QStringLiteral("Mask overlay: createEntity failed: %1")
                    .arg(QString::fromStdString(e.getDescription())));
            return;
        }
    } else {
        // Ensure every submesh also samples the overlay material — Ogre
        // creates one sub-entity per submesh on createEntity and they
        // each get the source material unless we override.
        for (unsigned i = 0; i < m_maskOverlayEntity->getNumSubEntities(); ++i)
            m_maskOverlayEntity->getSubEntity(i)->setMaterialName(m_maskOverlayMatName);
    }
    if (!m_maskOverlayNode) {
        m_maskOverlayNode = parentNode->createChildSceneNode();
        m_maskOverlayNode->attachObject(m_maskOverlayEntity);
    }
}

void TexturePaintController::destroyMeshMaskOverlay()
{
    // Source scene manager via the global Manager — touching
    // m_maskOverlayEntity->_getManager() is unsafe when the entity
    // was already cascade-destroyed (e.g. user removed the mesh
    // while a wand selection was up). Manager's sceneNodeDestroyed
    // signal handler nulls our pointers preemptively, so by the time
    // we reach here the cleanup may be a partial no-op — guard each
    // step independently.
    auto* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr ? mgr->getSceneMgr() : nullptr;

    if (m_maskOverlayNode && sceneMgr) {
        try {
            m_maskOverlayNode->detachAllObjects();
            auto* parent = m_maskOverlayNode->getParentSceneNode();
            if (parent) parent->removeChild(m_maskOverlayNode);
            sceneMgr->destroySceneNode(m_maskOverlayNode);
        } catch (...) {}
    }
    m_maskOverlayNode = nullptr;

    if (m_maskOverlayEntity && sceneMgr) {
        try {
            // Only destroy if Ogre still thinks it owns this entity
            // by name — otherwise it was already cascade-destroyed.
            const std::string& n = m_maskOverlayEntity->getName();
            if (sceneMgr->hasEntity(n))
                sceneMgr->destroyEntity(m_maskOverlayEntity);
        } catch (...) {}
    }
    m_maskOverlayEntity = nullptr;

    if (m_maskOverlayTex) {
        try { Ogre::TextureManager::getSingleton().remove(m_maskOverlayTex); } catch (...) {}
        m_maskOverlayTex.reset();
    }
}

TexturePaintBuffer& TexturePaintController::activePaintBuffer()
{
    if (m_layerStack.layerCount() > 0)
        return m_layerStack.activeLayer().buffer;
    return m_buffer;
}

const TexturePaintBuffer& TexturePaintController::activePaintBuffer() const
{
    if (m_layerStack.layerCount() > 0)
        return m_layerStack.activeLayer().buffer;
    return m_buffer;
}

void TexturePaintController::recomposeComposite(bool fullBuffer)
{
    if (m_layerStack.layerCount() <= 0) return;

    const int w = m_layerStack.width();
    const int h = m_layerStack.height();
    if (w <= 0 || h <= 0) return;
    if (m_buffer.width() != w || m_buffer.height() != h)
        m_buffer.resize(w, h);

    TexturePaintBuffer::DirtyRect dirty = fullBuffer
        ? TexturePaintBuffer::DirtyRect{0, 0, w, h}
        : m_layerStack.layerDirtyUnion();
    if (dirty.empty()) return;

    if (m_layerStack.layerCount() == 1 && !fullBuffer) {
        const auto& L = m_layerStack.layer(0);
        const bool trivialLayer =
            L.visible
            && L.opacity >= 1.f - 1e-4f
            && L.blendMode == PaintLayerBlend::Mode::Normal
            && L.maskAlpha.empty()
            && !L.locked;
        if (trivialLayer)
            copyBufferRect(L.buffer, m_buffer, dirty);
        else
            m_layerStack.compositeRegionTo(m_buffer.data().data(),
                                           dirty.x0, dirty.y0, dirty.x1, dirty.y1);
    } else if (fullBuffer) {
        std::vector<uint8_t> pixels;
        m_layerStack.compositeTo(pixels);
        if (pixels.empty()) return;
        std::memcpy(m_buffer.data().data(), pixels.data(), pixels.size());
    } else {
        m_layerStack.compositeRegionTo(m_buffer.data().data(),
                                       dirty.x0, dirty.y0, dirty.x1, dirty.y1);
    }

    m_buffer.markDirty(dirty.x0, dirty.y0, dirty.x1, dirty.y1);
    ++m_bufferDirtyEpoch;
    for (int i = 0; i < m_layerStack.layerCount(); ++i)
        m_layerStack.layer(i).buffer.clearDirty();
}

std::vector<uint8_t> TexturePaintController::snapshotActiveLayerPixels() const
{
    return activePaintBuffer().data();
}

void TexturePaintController::pushLayerOpUndo(const QString& label,
                                             PaintLayerStack::Snapshot before,
                                             PaintLayerStack::Snapshot after)
{
    UndoManager::getSingleton()->push(
        new PaintLayerOpCommand(this, label,
            (m_sessionEntity ? m_sessionEntity->getName() : std::string()),
            static_cast<int>(m_activeChannel),
                                std::move(before), std::move(after)));
}

int TexturePaintController::layerCount() const
{
    return m_layerStack.layerCount();
}

int TexturePaintController::activeLayerIndex() const
{
    return m_layerStack.activeIndex();
}

void TexturePaintController::setActiveLayerIndex(int index)
{
    if (index < 0 || index >= m_layerStack.layerCount()) return;
    if (m_layerStack.activeIndex() == index) return;
    m_layerStack.setActiveIndex(index);
    invalidateLayerStrokeBaseline();
    ++m_layerPreviewVersion;
    emit layersChanged();
}

QVariantList TexturePaintController::paintLayers() const
{
    QVariantList out;
    for (int i = 0; i < m_layerStack.layerCount(); ++i) {
        const auto& L = m_layerStack.layer(i);
        QVariantMap row;
        row.insert(QStringLiteral("index"), i);
        row.insert(QStringLiteral("name"), L.name);
        row.insert(QStringLiteral("type"), static_cast<int>(L.type));
        row.insert(QStringLiteral("blendMode"), static_cast<int>(L.blendMode));
        row.insert(QStringLiteral("opacity"), static_cast<double>(L.opacity));
        row.insert(QStringLiteral("visible"), L.visible);
        row.insert(QStringLiteral("locked"), L.locked);
        row.insert(QStringLiteral("active"), i == m_layerStack.activeIndex());
        row.insert(QStringLiteral("solo"), m_layerStack.soloIndex() == i);
        row.insert(QStringLiteral("thumbnailUrl"), layerPreviewUrl(i));
        out.append(row);
    }
    return out;
}

QStringList TexturePaintController::blendModeNames() const
{
    QStringList names;
    for (int m = 0; m <= static_cast<int>(PaintLayerBlend::Mode::Hue); ++m)
        names << QString::fromLatin1(
            PaintLayerBlend::modeName(static_cast<PaintLayerBlend::Mode>(m)));
    return names;
}

int TexturePaintController::addPaintLayer(const QString& name)
{
    if (!hasActiveSession()) return -1;
    const auto before = m_layerStack.snapshot();
    const int idx = m_layerStack.addEmpty(name);
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Add layer"), before, m_layerStack.snapshot());
    invalidateLayerStrokeBaseline();
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Added layer '%1'").arg(m_layerStack.layer(idx).name));
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
    return idx;
}

void TexturePaintController::deletePaintLayer(int index)
{
    if (index < 0 || index >= m_layerStack.layerCount()) return;
    if (m_layerStack.layerCount() <= 1) return;
    const auto before = m_layerStack.snapshot();
    const QString removed = m_layerStack.layer(index).name;
    m_layerStack.removeLayer(index);
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Delete layer"), before, m_layerStack.snapshot());
    invalidateLayerStrokeBaseline();
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Deleted layer '%1'").arg(removed));
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
}

int TexturePaintController::duplicatePaintLayer(int index)
{
    if (index < 0 || index >= m_layerStack.layerCount()) return -1;
    const auto before = m_layerStack.snapshot();
    const int idx = m_layerStack.duplicateLayer(index);
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Duplicate layer"), before, m_layerStack.snapshot());
    invalidateLayerStrokeBaseline();
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Duplicated layer '%1'").arg(m_layerStack.layer(idx).name));
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
    return idx;
}

void TexturePaintController::movePaintLayerUp(int index)
{
    if (index <= 0 || index >= m_layerStack.layerCount()) return;
    const auto before = m_layerStack.snapshot();
    m_layerStack.moveLayer(index, index - 1);
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Move layer"), before, m_layerStack.snapshot());
    invalidateLayerStrokeBaseline();
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("Moved layer up"));
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
}

void TexturePaintController::movePaintLayerDown(int index)
{
    if (index < 0 || index >= m_layerStack.layerCount() - 1) return;
    const auto before = m_layerStack.snapshot();
    m_layerStack.moveLayer(index, index + 1);
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Move layer"), before, m_layerStack.snapshot());
    invalidateLayerStrokeBaseline();
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("Moved layer down"));
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
}

void TexturePaintController::renamePaintLayer(int index, const QString& name)
{
    if (index < 0 || index >= m_layerStack.layerCount() || name.isEmpty()) return;
    const auto before = m_layerStack.snapshot();
    m_layerStack.renameLayer(index, name);
    pushLayerOpUndo(QStringLiteral("Rename layer"), before, m_layerStack.snapshot());
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Renamed layer to '%1'").arg(name));
    ++m_layerPreviewVersion;
    emit layersChanged();
}

void TexturePaintController::mergePaintLayerDown(int index)
{
    if (index <= 0 || index >= m_layerStack.layerCount()) return;
    const auto before = m_layerStack.snapshot();
    m_layerStack.mergeDown(index);
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Merge down"), before, m_layerStack.snapshot());
    invalidateLayerStrokeBaseline();
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Merged layer down at index %1").arg(index));
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
}

void TexturePaintController::flattenPaintLayers()
{
    if (m_layerStack.layerCount() <= 1) return;
    const auto before = m_layerStack.snapshot();
    m_layerStack.flattenAll();
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Flatten layers"), before, m_layerStack.snapshot());
    invalidateLayerStrokeBaseline();
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("Flattened layer stack"));
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
}

void TexturePaintController::setPaintLayerVisible(int index, bool visible)
{
    if (index < 0 || index >= m_layerStack.layerCount()) return;
    if (m_layerStack.layer(index).visible == visible) return;
    const auto before = m_layerStack.snapshot();
    m_layerStack.setVisible(index, visible);
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Layer visibility"), before, m_layerStack.snapshot());
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        visible ? QStringLiteral("Show layer") : QStringLiteral("Hide layer"));
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
}

void TexturePaintController::setPaintLayerLocked(int index, bool locked)
{
    if (index < 0 || index >= m_layerStack.layerCount()) return;
    if (m_layerStack.layer(index).locked == locked) return;
    m_layerStack.setLocked(index, locked);
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        locked ? QStringLiteral("Locked layer") : QStringLiteral("Unlocked layer"));
    ++m_layerPreviewVersion;
    emit layersChanged();
}

void TexturePaintController::setPaintLayerOpacity(int index, double opacity)
{
    if (index < 0 || index >= m_layerStack.layerCount()) return;
    const float clamped = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    if (std::abs(m_layerStack.layer(index).opacity - clamped) < 1e-5f)
        return;

    PaintLayerStack::Snapshot before;
    if (!m_layerOpacityDragging)
        before = m_layerStack.snapshot();
    m_layerStack.setOpacity(index, clamped);
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    if (m_layerOpacityDragging) {
        m_layerOpacityDragChanged = true;
    } else {
        pushLayerOpUndo(QStringLiteral("Layer opacity"), before, m_layerStack.snapshot());
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
            QStringLiteral("Opacity=%1").arg(clamped, 0, 'f', 2));
    }
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
}

void TexturePaintController::beginPaintLayerOpacityDrag()
{
    if (!hasActiveSession() || m_layerStack.layerCount() <= 0) return;
    m_layerOpacityDragBefore = m_layerStack.snapshot();
    m_layerOpacityDragging = true;
    m_layerOpacityDragChanged = false;
}

void TexturePaintController::endPaintLayerOpacityDrag()
{
    if (!m_layerOpacityDragging) return;
    m_layerOpacityDragging = false;
    if (m_layerOpacityDragChanged) {
        const auto after = m_layerStack.snapshot();
        pushLayerOpUndo(QStringLiteral("Layer opacity"), m_layerOpacityDragBefore, after);
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("Opacity drag"));
    }
    m_layerOpacityDragBefore = {};
    m_layerOpacityDragChanged = false;
}

void TexturePaintController::setPaintLayerBlendMode(int index, int mode)
{
    if (index < 0 || index >= m_layerStack.layerCount()) return;
    const auto before = m_layerStack.snapshot();
    m_layerStack.setBlendMode(index, static_cast<PaintLayerBlend::Mode>(mode));
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Layer blend mode"), before, m_layerStack.snapshot());
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        PaintLayerBlend::modeName(static_cast<PaintLayerBlend::Mode>(mode)));
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
}

void TexturePaintController::setPaintLayerSolo(int index, bool solo)
{
    if (index < 0 || index >= m_layerStack.layerCount()) return;
    const auto before = m_layerStack.snapshot();
    if (solo)
        m_layerStack.setSolo(index, true);
    else
        m_layerStack.clearSolo();
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Layer solo"), before, m_layerStack.snapshot());
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        solo ? QStringLiteral("Solo on") : QStringLiteral("Solo off"));
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
}

QString TexturePaintController::layerPreviewUrl(int index) const
{
    if (index < 0 || index >= m_layerStack.layerCount()) return {};
    return QStringLiteral("image://paintbuffer/layer/%1?v=%2")
        .arg(index)
        .arg(m_layerPreviewVersion);
}

QImage TexturePaintController::snapshotLayerImage(int index) const
{
    if (index < 0 || index >= m_layerStack.layerCount()) return {};
    const auto& buf = m_layerStack.layer(index).buffer;
    if (buf.width() <= 0 || buf.height() <= 0) return {};
    QImage view(const_cast<uchar*>(buf.data().data()),
                buf.width(), buf.height(),
                buf.width() * 4, QImage::Format_RGBA8888);
    return view.copy();
}

bool TexturePaintController::confirmFlattenLayersForExport(QWidget* parent) const
{
    if (layerCount() <= 1 || !hasActiveSession() || !m_sessionEntity)
        return true;

    const auto* sel = SelectionSet::getSingleton();
    if (!sel) return true;

    auto exportsPaintSession = [this](const Ogre::Entity* entity) {
        return entity && entity == m_sessionEntity;
    };

    bool includesPaintSession = false;
    if (sel->hasEntities()) {
        for (Ogre::Entity* entity : sel->getEntitiesSelectionList()) {
            if (exportsPaintSession(entity)) {
                includesPaintSession = true;
                break;
            }
        }
    } else if (sel->hasNodes()) {
        auto* sceneMgr = Manager::getSingletonPtr()
                             ? Manager::getSingleton()->getSceneMgr()
                             : nullptr;
        if (sceneMgr) {
            for (Ogre::SceneNode* node : sel->getNodesSelectionList()) {
                if (!sceneMgr->hasEntity(node->getName())) continue;
                if (exportsPaintSession(sceneMgr->getEntity(node->getName()))) {
                    includesPaintSession = true;
                    break;
                }
            }
        }
    }

    if (!includesPaintSession)
        return true;

    if (!parent)
        return true;

    const QString msg = tr(
        "This mesh has %1 texture paint layers.\n\n"
        "FBX, glTF, OBJ, and other export formats store a single flat "
        "texture — the merged composite of all visible layers will be "
        "written. Separate layer data is not preserved in the exported file.\n\n"
        "Continue export?")
                            .arg(layerCount());

    const auto choice = QMessageBox::warning(parent, tr("Flatten Texture Layers?"), msg,
                                QMessageBox::Ok | QMessageBox::Cancel,
                                QMessageBox::Ok);
    SentryReporter::addBreadcrumb(
        QStringLiteral("ui.action"),
        choice == QMessageBox::Ok
            ? QStringLiteral("Export flatten layers confirmed (%1 layers)").arg(layerCount())
            : QStringLiteral("Export flatten layers cancelled (%1 layers)").arg(layerCount()));
    return choice == QMessageBox::Ok;
}

void TexturePaintController::flushPaintTextureForExport(Ogre::Entity* entity)
{
    if (!entity || entity != m_sessionEntity || !hasActiveSession())
        return;
    if (m_layerStack.layerCount() > 0)
        recomposeComposite(/*fullBuffer=*/true);
    if (m_buffer.width() > 0 && m_buffer.height() > 0)
        m_buffer.markDirty(0, 0, m_buffer.width(), m_buffer.height());
    doFlushDirtyToOgre(/*immediate=*/true);
    updateEmbeddedTextureCache();
    SentryReporter::addBreadcrumb(
        QStringLiteral("file.export"),
        QStringLiteral("Flushed %1-layer composite before export")
            .arg(m_layerStack.layerCount()));
}
