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
                              QString textureName)
        : QUndoCommand(QStringLiteral("Texture paint"))
        , m_controller(controller)
        , m_layerIndex(layerIndex)
        , m_before(std::move(before))
        , m_after(std::move(after))
        , m_width(width)
        , m_height(height)
        , m_textureName(std::move(textureName))
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
        if (m_controller->currentTextureName() != m_textureName) return;
        m_controller->applyLayerPixelSnapshot(m_layerIndex, pixels);
    }

    TexturePaintController* m_controller = nullptr;
    int m_layerIndex = 0;
    std::vector<uint8_t> m_before;
    std::vector<uint8_t> m_after;
    int m_width = 0;
    int m_height = 0;
    QString m_textureName;
    bool m_skipFirstRedo = true;
};

/// Undo command for structural layer-stack changes (#546).
class PaintLayerOpCommand : public QUndoCommand
{
public:
    PaintLayerOpCommand(TexturePaintController* controller,
                        QString label,
                        PaintLayerStack::Snapshot before,
                        PaintLayerStack::Snapshot after)
        : QUndoCommand(std::move(label))
        , m_controller(controller)
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
        m_controller->applyLayerStackSnapshot(snap);
    }

    TexturePaintController* m_controller = nullptr;
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
                                  QString textureName,
                                  QString label)
        : QUndoCommand(label)
        , m_controller(controller)
        , m_layerIndex(layerIndex)
        , m_before(std::move(before))
        , m_after(std::move(after))
        , m_width(width)
        , m_height(height)
        , m_textureName(std::move(textureName))
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
        if (m_controller->currentTextureName() != m_textureName) return;
        m_controller->applyLayerPixelSnapshot(m_layerIndex, pixels);
    }

    TexturePaintController* m_controller = nullptr;
    int m_layerIndex = 0;
    std::vector<uint8_t> m_before;
    std::vector<uint8_t> m_after;
    int m_width = 0;
    int m_height = 0;
    QString m_textureName;
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

    // Fallback: first submesh, first material, first pass, prefer
    // canonical diffuse slot names; otherwise first TUS or create.
    auto* subEnt = entity->getSubEntity(0);
    if (!subEnt) return nullptr;
    Ogre::MaterialPtr mat = subEnt->getMaterial();
    if (!mat || mat->getNumTechniques() == 0) return nullptr;
    auto* tech = mat->getTechnique(0);
    if (!tech || tech->getNumPasses() == 0) return nullptr;
    auto* pass = tech->getPass(0);
    if (!pass) return nullptr;
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
        m_buffer.clear(Ogre::ColourValue::White);
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
        scheduleRebindToPaintTexture(entity);
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

void TexturePaintController::flushLiveStrokeToGpu()
{
    const auto dirty = m_buffer.dirtyRect();
    if (dirty.empty()) return;

    if (m_paintMeshEntity && m_ogreTexture && m_boundSlots.empty())
        rebindEntityDiffuseToPaintTexture(m_paintMeshEntity);

    if (blitBufferRectToOgreTexture(dirty.x0, dirty.y0, dirty.x1, dirty.y1))
        m_buffer.clearDirty();
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
            m_textureName));
    m_layerStrokeBaseline = snapshotActiveLayerPixels();
}

void TexturePaintController::resetStrokePaintState()
{
    m_strokeJustBegan = true;
    m_smudgeHavePrev = false;
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

    if (changed) {
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

    if (changed) {
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
    m_pendingStrokeWidget = widget;
    m_pendingStrokePos = screenPos;
    processPendingStrokeUpdate();
    return true;
}

void TexturePaintController::updateStroke(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_strokeActive || !m_paintEnabled) return;
    m_pendingStrokeWidget = widget;
    m_pendingStrokePos = screenPos;
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

    m_strokeActive = false;

    recomposePaintBufferIfNeeded();
    flushLiveStrokeToGpu();
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
            } catch (...) {}
        }
        m_ringNode = nullptr;
        m_ringObj = nullptr;
    }
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
                                                Ogre::Vector3& outNormal) const
{
    if (!m_paintMesh) return false;
    // Walk every triangle, find the one whose UV-space contains `uv`
    // (barycentric test on UV triangle), then interpolate the 3D
    // position with those same barycentrics.
    for (const auto& sub : m_paintMesh->subMeshes()) {
        for (const auto& tri : sub.triangles) {
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
            return true;
        }
    }
    return false;
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
        layerBuf.width(), layerBuf.height(), m_textureName,
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
        layerBuf.width(), layerBuf.height(), m_textureName,
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
        layerBuf.width(), layerBuf.height(), m_textureName,
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
        copyBufferRect(m_layerStack.layer(0).buffer, m_buffer, dirty);
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
        new PaintLayerOpCommand(this, label, std::move(before), std::move(after)));
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
    return {
        QStringLiteral("Normal"),
        QStringLiteral("Multiply"),
        QStringLiteral("Screen"),
        QStringLiteral("Overlay"),
        QStringLiteral("Add"),
        QStringLiteral("Subtract"),
        QStringLiteral("Soft Light"),
        QStringLiteral("Hue"),
    };
}

int TexturePaintController::addPaintLayer(const QString& name)
{
    if (!hasActiveSession()) return -1;
    const auto before = m_layerStack.snapshot();
    const int idx = m_layerStack.addEmpty(name);
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Add layer"), before, m_layerStack.snapshot());
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.add"),
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
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.delete"),
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
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.duplicate"),
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
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.reorder"), QStringLiteral("Moved layer up"));
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
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.reorder"), QStringLiteral("Moved layer down"));
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
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.rename"),
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
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.merge"),
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
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.flatten"), QStringLiteral("Flattened layer stack"));
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
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.visibility"),
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
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.lock"),
        locked ? QStringLiteral("Locked layer") : QStringLiteral("Unlocked layer"));
    ++m_layerPreviewVersion;
    emit layersChanged();
}

void TexturePaintController::setPaintLayerOpacity(int index, double opacity)
{
    if (index < 0 || index >= m_layerStack.layerCount()) return;
    const auto before = m_layerStack.snapshot();
    m_layerStack.setOpacity(index, static_cast<float>(opacity));
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Layer opacity"), before, m_layerStack.snapshot());
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.opacity"),
        QStringLiteral("Opacity=%1").arg(opacity, 0, 'f', 2));
    ++m_layerPreviewVersion;
    emit layersChanged();
    emit fullResPreviewChanged();
}

void TexturePaintController::setPaintLayerBlendMode(int index, int mode)
{
    if (index < 0 || index >= m_layerStack.layerCount()) return;
    const auto before = m_layerStack.snapshot();
    m_layerStack.setBlendMode(index, static_cast<PaintLayerBlend::Mode>(mode));
    recomposeComposite(/*fullBuffer=*/true);
    flushDirtyToOgre();
    pushLayerOpUndo(QStringLiteral("Layer blend mode"), before, m_layerStack.snapshot());
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.blend"),
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
    SentryReporter::addBreadcrumb(QStringLiteral("paint.layer.solo"),
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

    return QMessageBox::warning(parent, tr("Flatten Texture Layers?"), msg,
                                QMessageBox::Ok | QMessageBox::Cancel,
                                QMessageBox::Ok)
           == QMessageBox::Ok;
}

void TexturePaintController::flushPaintTextureForExport(Ogre::Entity* entity)
{
    if (!entity || entity != m_sessionEntity || !hasActiveSession())
        return;
    if (m_layerStack.layerCount() > 0)
        recomposeComposite(/*fullBuffer=*/true);
    if (!m_buffer.dirtyRect().empty() || m_layerStack.layerCount() > 0) {
        m_buffer.markDirty(0, 0, m_buffer.width(), m_buffer.height());
        flushDirtyToOgre();
    }
    updateEmbeddedTextureCache();
    SentryReporter::addBreadcrumb(
        QStringLiteral("paint.layer.export"),
        QStringLiteral("Flushed %1-layer composite before export")
            .arg(m_layerStack.layerCount()));
}
