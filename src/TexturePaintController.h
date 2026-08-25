#ifndef TEXTUREPAINTCONTROLLER_H
#define TEXTUREPAINTCONTROLLER_H

#include "PaintSelectionMask.h"
#include "TexturePaintBuffer.h"
#include "BrushEngine.h"
#include "BrushAssetLibrary.h"
#include "BrushFootprint.h"
#include "GradientRamp.h"
#include "PaintLayerStack.h"
#include "PaintChannel.h"
#include "SymmetryMirrorMap.h"
#include "ProjectionPainter.h"
#include "DecalSession.h"

#include <QColor>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QMap>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <OgreTexture.h>
#include <OgreVector.h>

#include <deque>
#include <memory>
#include <cstdint>
#include <unordered_map>
#include <vector>

class EditableMesh;
class OgreWidget;

namespace Ogre {
class Entity;
class Pass;
class TextureUnitState;
} // namespace Ogre

/**
 * @brief QML_SINGLETON managing texture painting on the active edit-mode entity.
 *
 * Mirrors the EditModeController vertex-paint flow: enable a mode, hook
 * into the existing TransformOperator mouse pipeline, hit-test a ray
 * against the EditableMesh, interpolate UV from the hit triangle, and
 * paint into a CPU-side TexturePaintBuffer. Each stroke uploads only
 * the dirty rect to a live Ogre::Texture so the viewport sees changes
 * with no full re-upload.
 *
 * Save / load round-trips the buffer to PNG (or any QImage-supported
 * format). Bake builds the same buffer from EditableMesh vertex colors
 * and is exposed as a Q_INVOKABLE so the QML "Bake" button hits the
 * same code path as the CLI.
 *
 * The controller does **not** own the Ogre::Texture once it has been
 * created — Ogre's TextureManager retains ownership, and the buffer
 * is a CPU-side mirror. Closing edit mode flushes the buffer to the
 * texture one last time and resets the session.
 */
class TexturePaintController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool texturePaintEnabled READ texturePaintEnabled WRITE setTexturePaintEnabled NOTIFY texturePaintChanged)
    // Brush params are mirrored from EditModeController so the toolbar brush
    // popup is the single source of truth — read-only here, used by QML for
    // live preview text in the Texture Paint section.
    Q_PROPERTY(QColor texturePaintColor READ texturePaintColor NOTIFY texturePaintChanged)
    Q_PROPERTY(double texturePaintRadius READ texturePaintRadius NOTIFY texturePaintChanged)
    /// Brush radius mapped into UV space (0..1). Same calculation
    /// the texture-paint dispatch uses to convert the mesh-local
    /// radius via `bbox half-size`. Drives the 2D thumbnail brush
    /// outline. Emits texturePaintChanged on radius / session change.
    Q_PROPERTY(double texturePaintRadiusUV READ texturePaintRadiusUV NOTIFY texturePaintChanged)
    /// Brush shape: 0 = Round, 1 = Square. Mirror of
    /// EditModeController::vertexPaintShape so QML doesn't need both
    /// singletons to draw a brush outline.
    Q_PROPERTY(int brushShape READ brushShape NOTIFY texturePaintChanged)
    Q_PROPERTY(double texturePaintStrength READ texturePaintStrength NOTIFY texturePaintChanged)
    Q_PROPERTY(double texturePaintFalloff READ texturePaintFalloff NOTIFY texturePaintChanged)
    Q_PROPERTY(int textureResolution READ textureResolution NOTIFY sessionChanged)
    Q_PROPERTY(QString currentTextureName READ currentTextureName NOTIFY sessionChanged)
    Q_PROPERTY(bool hasActiveSession READ hasActiveSession NOTIFY sessionChanged)

    // Brush tool — paint / erase / fill / picker.
    Q_PROPERTY(int brushTool READ brushTool WRITE setBrushTool NOTIFY brushToolChanged)

    // Paint v2 Slice A (#544) — gradient ramp brushes.
    Q_PROPERTY(int colorSource READ colorSource WRITE setColorSource NOTIFY gradientChanged)
    Q_PROPERTY(int gradientMode READ gradientMode WRITE setGradientMode NOTIFY gradientChanged)
    Q_PROPERTY(QString activeRampName READ activeRampName WRITE setActiveRampName NOTIFY gradientChanged)
    Q_PROPERTY(bool useFgBgRamp READ useFgBgRamp WRITE setUseFgBgRamp NOTIFY gradientChanged)
    Q_PROPERTY(bool gradientStepped READ gradientStepped WRITE setGradientStepped NOTIFY gradientChanged)
    Q_PROPERTY(double rampJitter READ rampJitter WRITE setRampJitter NOTIFY gradientChanged)
    Q_PROPERTY(QStringList rampNames READ rampNames NOTIFY gradientChanged)
    Q_PROPERTY(QString rampPreviewDataUri READ rampPreviewDataUri NOTIFY gradientChanged)
    Q_PROPERTY(QVariantList activeRampStops READ activeRampStops NOTIFY gradientChanged)

    // Paint target — texture or vertex.
    Q_PROPERTY(int paintTarget READ paintTarget WRITE setPaintTarget NOTIFY paintTargetChanged)

    // Live preview of the current paint buffer as a data URI, so the QML
    // preview panel can render it via Image { source: ... }. Emitted on
    // every dirty-rect flush so the preview stays in sync with strokes.
    Q_PROPERTY(QString previewDataUri READ previewDataUri NOTIFY previewChanged)
    /// Full-resolution preview URL for the detached Texture Editor
    /// window. Returns an `image://paintbuffer/N` URL, where `N`
    /// monotonically increases on every refresh so QML's Image
    /// element invalidates its cache and pulls a fresh copy from
    /// the QQuickImageProvider registered under the `paintbuffer`
    /// scheme. The provider hands back a `QImage` view of
    /// `m_buffer` directly — no PNG encode, no base64, no string
    /// copy of the megabyte buffer per stroke. Roughly 100x cheaper
    /// than the data-URI path on 2048² textures.
    Q_PROPERTY(QString fullResPreviewUrl READ fullResPreviewUrl NOTIFY fullResPreviewChanged)

    // Texture slots on the currently-selected entity. Each entry is a
    // map: { label, submesh, slot, textureName }. QML reads this to
    // populate the slot picker.
    Q_PROPERTY(QVariantList textureSlots READ textureSlots NOTIFY slotsChanged)
    Q_PROPERTY(int activeSlotIndex READ activeSlotIndex WRITE setActiveSlotIndex NOTIFY slotsChanged)

    // Paint v2 Slice E (#548) — symmetry. WRITE-backed (assign directly from QML).
    Q_PROPERTY(bool symmetryEnabled READ symmetryEnabled WRITE setSymmetryEnabled NOTIFY symmetryChanged)
    Q_PROPERTY(int  symmetrySpace   READ symmetrySpace   WRITE setSymmetrySpace   NOTIFY symmetryChanged)
    Q_PROPERTY(int  symmetryAxes    READ symmetryAxes    WRITE setSymmetryAxes    NOTIFY symmetryChanged)
    Q_PROPERTY(bool topologyMirror  READ topologyMirror  WRITE setTopologyMirror  NOTIFY symmetryChanged)
    // Paint v2 Slice E (#548) — line stabilizer.
    Q_PROPERTY(int    stabilizerMode   READ stabilizerMode   WRITE setStabilizerMode   NOTIFY stabilizerChanged)
    Q_PROPERTY(double stabilizerAmount READ stabilizerAmount WRITE setStabilizerAmount NOTIFY stabilizerChanged)

    // Paint v2 Slice F (#549) — projection / stencil painting. WRITE-backed.
    Q_PROPERTY(int    projectionMode    READ projectionMode    WRITE setProjectionMode    NOTIFY projectionChanged) // 0 off / 1 stencil-brush / 2 camera-locked
    Q_PROPERTY(QString stencilImagePath READ stencilImagePath  WRITE setStencilImagePath  NOTIFY projectionChanged)
    Q_PROPERTY(bool   projBackfaceCull  READ projBackfaceCull  WRITE setProjBackfaceCull  NOTIFY projectionChanged)
    Q_PROPERTY(bool   projUseOcclusion  READ projUseOcclusion  WRITE setProjUseOcclusion  NOTIFY projectionChanged)
    Q_PROPERTY(double projDepthLimit    READ projDepthLimit    WRITE setProjDepthLimit    NOTIFY projectionChanged) // fraction of bounds radius; 0 = off
    Q_PROPERTY(bool   cameraLocked      READ cameraLocked      NOTIFY projectionChanged)  // read-only status
    // Paint v2 Slice F (#549) — decal tool (read-only status for the panel).
    Q_PROPERTY(bool   decalSessionActive READ decalSessionActive NOTIFY projectionChanged)
    Q_PROPERTY(int    decalState         READ decalState         NOTIFY projectionChanged)

public:
    enum BrushTool {
        ToolPaint       = 0,  ///< Lerp pixels toward brush color.
        ToolErase       = 1,  ///< Paint with BG color (was: paint transparent).
        ToolFill        = 2,  ///< Flood-fill connected pixels under cursor.
        ToolColorPicker = 3,  ///< Sample color at hit UV into the brush color.
        ToolSmudge      = 4,  ///< Drag pixels in the brush direction.
        ToolSmartSelect = 5,  ///< Fuzzy / magic-wand region select by color.
        ToolDecal       = 6,  ///< Paint v2 Slice F (#549): place an image decal.
    };
    Q_ENUM(BrushTool)

    /// What the brush paints into.
    enum PaintTarget {
        TargetTexture = 0,  ///< Paint into the BaseColor texture (default).
        TargetVertex  = 1,  ///< Paint vertex colors (formerly Edit Mode's vertex paint).
    };
    Q_ENUM(PaintTarget)

    /// Paint v2 Slice A — brush colour source (mirrors BrushEngine::ColorSource).
    enum ColorSource {
        ColorSolid = 0,
        ColorGradient = 1,
    };
    Q_ENUM(ColorSource)

    /// Paint v2 Slice A — gradient mapping mode (mirrors BrushEngine::GradientMode).
    enum GradientMode {
        GradientLinear = 0,
        GradientRadial = 1,
        GradientAngular = 2,
    };
    Q_ENUM(GradientMode)

    /// Paint v2 Slice E (#548) — the space the symmetry mirror plane lives in.
    enum SymmetrySpace {
        SymLocal = 0,  ///< Reflect about the mesh's local origin plane (default).
        SymWorld = 1,  ///< Reflect about the world plane through the entity origin.
    };
    Q_ENUM(SymmetrySpace)

    /// Paint v2 Slice E (#548) — enabled mirror axes as a bitmask (OR-combinable
    /// for multi-axis, e.g. X|Y mirrors to 4 locations, X|Y|Z to 8).
    enum SymmetryAxis {
        SymAxisNone = 0,
        SymAxisX = 1,
        SymAxisY = 2,
        SymAxisZ = 4,
    };
    Q_DECLARE_FLAGS(SymmetryAxes, SymmetryAxis)
    Q_FLAG(SymmetryAxes)
    Q_ENUM(SymmetryAxis)

    /// Paint v2 Slice E (#548) — line-stabilizer smoothing mode.
    enum StabilizerMode {
        StabAverage = 0,  ///< Cursor is a weighted moving average of recent samples.
        StabTrail   = 1,  ///< Brush lags a fixed distance behind the cursor (Krita/PS).
    };
    Q_ENUM(StabilizerMode)

    static TexturePaintController* instance();
    static TexturePaintController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// @name Paint-mode toggle (mirrors vertex-paint toggle on EditMode)
    /// @{
    bool texturePaintEnabled() const { return m_paintEnabled; }
    void setTexturePaintEnabled(bool enabled);
    /// @}

    /// @name Brush parameters (read-only mirror of EditModeController)
    /// @{
    QColor texturePaintColor() const;
    /// Secondary "background" color. Used by:
    ///   - ToolErase (replaces pixels with BG instead of transparent)
    ///   - mask "fill with BG" action
    QColor bgPaintColor() const;
    double texturePaintRadius() const;
    /// Same brush radius mapped into UV space. Mirrors the conversion
    /// done by the texture-paint dispatch (mesh-local / bbox half-
    /// size). Returns the raw radius when no mesh is available.
    double texturePaintRadiusUV() const;
    /// Current brush shape (int form of EditModeController::BrushShape).
    /// 0 = Round, 1 = Square.
    int brushShape() const;
    double texturePaintStrength() const;
    double texturePaintFalloff() const;
    /// @}

    /// @name Session state
    /// @{
    int textureResolution() const { return m_buffer.width(); }
    QString currentTextureName() const { return m_textureName; }
    bool hasActiveSession() const { return m_buffer.width() > 0 && !m_textureName.isEmpty(); }
    /// @}

    /// @name Brush tool
    /// @{
    int brushTool() const { return static_cast<int>(m_tool); }
    void setBrushTool(int tool);
    /// @}

    /// @name Paint v2 Slice A — gradient ramp brushes
    /// @{
    int colorSource() const { return static_cast<int>(m_colorSource); }
    void setColorSource(int source);
    int gradientMode() const { return static_cast<int>(m_gradientMode); }
    void setGradientMode(int mode);
    QString activeRampName() const { return m_activeRampName; }
    void setActiveRampName(const QString& name);
    bool useFgBgRamp() const { return m_useFgBgRamp; }
    void setUseFgBgRamp(bool on);
    bool gradientStepped() const { return m_gradientStepped; }
    void setGradientStepped(bool on);
    double rampJitter() const { return m_rampJitter; }
    void setRampJitter(double j);
    QStringList rampNames() const;
    QString rampPreviewDataUri() const { return m_rampPreviewUri; }
    QVariantList activeRampStops() const;

    /// Persist the currently-edited stops as a named custom ramp.
    Q_INVOKABLE bool saveCustomRamp(const QString& name, const QVariantList& stops,
                                    bool stepped = false);
    /// Delete a custom ramp by name (bundled presets are not removable).
    Q_INVOKABLE bool deleteCustomRamp(const QString& name);
    /// Replace the active ramp's stops in-memory (editor live preview).
    Q_INVOKABLE void setActiveRampStops(const QVariantList& stops, bool stepped = false);
    /// Seed a new ramp by sampling N colours along a UV line on the buffer.
    Q_INVOKABLE bool sampleRampFromTexture(double u0, double v0,
                                           double u1, double v1,
                                           int numStops = 5);
    /// Open the gradient ramp editor window.
    Q_INVOKABLE void openRampEditor();
    Q_INVOKABLE void closeRampEditor();
    Q_PROPERTY(bool rampEditorOpen READ rampEditorOpen NOTIFY rampEditorChanged)
    bool rampEditorOpen() const { return m_rampEditorWindow != nullptr; }
    /// @}

    /// @name Paint v2 Slice B — textured / stamp brushes (#545)
    /// @{
    Q_PROPERTY(int footprintType READ footprintType WRITE setFootprintType NOTIFY stampChanged)
    Q_PROPERTY(QString activeStampName READ activeStampName WRITE setActiveStampName NOTIFY stampChanged)
    Q_PROPERTY(QString activeTilingName READ activeTilingName WRITE setActiveTilingName NOTIFY stampChanged)
    Q_PROPERTY(QStringList stampNames READ stampNames NOTIFY stampChanged)
    Q_PROPERTY(QStringList tilingNames READ tilingNames NOTIFY stampChanged)
    Q_PROPERTY(QString activeStampPreviewUri READ activeStampPreviewUri NOTIFY stampChanged)
    Q_PROPERTY(QString activeTilingPreviewUri READ activeTilingPreviewUri NOTIFY stampChanged)
    Q_PROPERTY(double stampSpacing READ stampSpacing WRITE setStampSpacing NOTIFY stampChanged)
    Q_PROPERTY(double stampScatter READ stampScatter WRITE setStampScatter NOTIFY stampChanged)
    Q_PROPERTY(double stampSizeJitter READ stampSizeJitter WRITE setStampSizeJitter NOTIFY stampChanged)
    Q_PROPERTY(double stampOpacityJitter READ stampOpacityJitter WRITE setStampOpacityJitter NOTIFY stampChanged)
    Q_PROPERTY(int stampRotation READ stampRotation WRITE setStampRotation NOTIFY stampChanged)
    Q_PROPERTY(double stampFixedAngle READ stampFixedAngle WRITE setStampFixedAngle NOTIFY stampChanged)
    Q_PROPERTY(double tilingScale READ tilingScale WRITE setTilingScale NOTIFY stampChanged)
    Q_PROPERTY(double tilingRotation READ tilingRotation WRITE setTilingRotation NOTIFY stampChanged)
    Q_PROPERTY(double tilingOffsetU READ tilingOffsetU WRITE setTilingOffsetU NOTIFY stampChanged)
    Q_PROPERTY(double tilingOffsetV READ tilingOffsetV WRITE setTilingOffsetV NOTIFY stampChanged)

    int footprintType() const { return static_cast<int>(m_footprintType); }
    void setFootprintType(int type);
    QString activeStampName() const { return m_activeStampName; }
    void setActiveStampName(const QString& name);
    QString activeTilingName() const { return m_activeTilingName; }
    void setActiveTilingName(const QString& name);
    QStringList stampNames() const;
    QStringList tilingNames() const;
    QString activeStampPreviewUri() const { return m_stampPreviewUri; }
    QString activeTilingPreviewUri() const { return m_tilingPreviewUri; }
    double stampSpacing() const { return m_stampSettings.spacing; }
    void setStampSpacing(double v);
    double stampScatter() const { return m_stampSettings.scatter; }
    void setStampScatter(double v);
    double stampSizeJitter() const { return m_stampSettings.sizeJitter; }
    void setStampSizeJitter(double v);
    double stampOpacityJitter() const { return m_stampSettings.opacityJitter; }
    void setStampOpacityJitter(double v);
    int stampRotation() const { return static_cast<int>(m_stampSettings.rotation); }
    void setStampRotation(int mode);
    double stampFixedAngle() const { return m_stampSettings.fixedAngleDeg; }
    void setStampFixedAngle(double deg);
    double tilingScale() const { return m_tilingSettings.scale; }
    void setTilingScale(double v);
    double tilingRotation() const { return m_tilingSettings.rotationDeg; }
    void setTilingRotation(double deg);
    double tilingOffsetU() const { return m_tilingSettings.offsetU; }
    void setTilingOffsetU(double v);
    double tilingOffsetV() const { return m_tilingSettings.offsetV; }
    void setTilingOffsetV(double v);

    Q_INVOKABLE QString importStampAsset(const QString& filePath);
    Q_INVOKABLE QString importTilingAsset(const QString& filePath);
    Q_INVOKABLE bool deleteCustomStamp(const QString& name);
    Q_INVOKABLE bool deleteCustomTiling(const QString& name);
    Q_INVOKABLE bool renameCustomStamp(const QString& oldName, const QString& newName);
    Q_INVOKABLE bool renameCustomTiling(const QString& oldName, const QString& newName);
    Q_INVOKABLE bool isBundledStamp(const QString& name) const;
    Q_INVOKABLE bool isBundledTiling(const QString& name) const;
    Q_INVOKABLE QString stampThumbnailUri(const QString& name) const;
    Q_INVOKABLE QString tilingThumbnailUri(const QString& name) const;
    /// @}

    /// @name Paint v2 Slice C — layer stack (#546)
    /// @{
    Q_PROPERTY(int layerCount READ layerCount NOTIFY layersChanged)
    Q_PROPERTY(int activeLayerIndex READ activeLayerIndex WRITE setActiveLayerIndex NOTIFY layersChanged)
    Q_PROPERTY(QVariantList paintLayers READ paintLayers NOTIFY layersChanged)
    Q_PROPERTY(QStringList blendModeNames READ blendModeNames CONSTANT)

    int layerCount() const;
    int activeLayerIndex() const;
    void setActiveLayerIndex(int index);
    QVariantList paintLayers() const;
    QStringList blendModeNames() const;

    Q_INVOKABLE int addPaintLayer(const QString& name = QString());
    Q_INVOKABLE void deletePaintLayer(int index);
    Q_INVOKABLE int duplicatePaintLayer(int index);
    Q_INVOKABLE void movePaintLayerUp(int index);
    Q_INVOKABLE void movePaintLayerDown(int index);
    Q_INVOKABLE void renamePaintLayer(int index, const QString& name);
    Q_INVOKABLE void mergePaintLayerDown(int index);
    Q_INVOKABLE void flattenPaintLayers();
    Q_INVOKABLE void setPaintLayerVisible(int index, bool visible);
    Q_INVOKABLE void setPaintLayerLocked(int index, bool locked);
    Q_INVOKABLE void setPaintLayerOpacity(int index, double opacity);
    Q_INVOKABLE void beginPaintLayerOpacityDrag();
    Q_INVOKABLE void endPaintLayerOpacityDrag();
    Q_INVOKABLE void setPaintLayerBlendMode(int index, int mode);
    Q_INVOKABLE void setPaintLayerSolo(int index, bool solo);
    Q_INVOKABLE QString layerPreviewUrl(int index) const;
    /// If the current selection includes a multi-layer paint session, ask
    /// whether to continue (export stores the flattened composite only).
    /// Returns false when the user cancels.
    bool confirmFlattenLayersForExport(QWidget* parent) const;
    /// Recompose visible layers and push the composite into the live texture
    /// + embedded cache so mesh export sees painted pixels.
    void flushPaintTextureForExport(Ogre::Entity* entity);
    /// @}

    /// @name Paint target (texture or vertex colors)
    /// @{
    int paintTarget() const { return static_cast<int>(m_target); }
    void setPaintTarget(int target);
    /// @}

    /// @name Texture slot enumeration (selection-driven)
    /// @{
    QVariantList textureSlots() const { return m_slots; }
    int activeSlotIndex() const { return m_activeSlot; }
    void setActiveSlotIndex(int index);
    /// Recompute the texture slot list from the current selection.
    Q_INVOKABLE void refreshSlots();
    /// @}

    /// @name Paint v2 Slice E — symmetry + line stabilizer (#548)
    /// @{
    bool symmetryEnabled() const { return m_symmetryEnabled; }
    void setSymmetryEnabled(bool on);
    int  symmetrySpace() const { return static_cast<int>(m_symmetrySpace); }
    void setSymmetrySpace(int space);
    int  symmetryAxes() const { return m_symmetryAxes; }
    void setSymmetryAxes(int axes);
    bool topologyMirror() const { return m_topologyMirror; }
    void setTopologyMirror(bool on);

    int  stabilizerMode() const { return static_cast<int>(m_stabilizerMode); }
    void setStabilizerMode(int mode);
    double stabilizerAmount() const { return m_stabilizerAmount; }
    void setStabilizerAmount(double amount);

    // Pure math (no member state) — exposed for unit testing the stabilizer.
    /// Window size for the weighted moving average at `amount` (0..100).
    static int stabilizerWindow(double amount);
    /// Newest-heaviest weighted average of the last `window` samples.
    static QPointF stabilizeAveragePoint(const std::deque<QPointF>& samples,
                                         int window);
    /// Step a lagging trail position toward `raw`, keeping distance `lag` px.
    static QPointF stabilizeTrailPoint(const QPointF& trail, const QPointF& raw,
                                       double lag);
    /// @}

    /// @name Paint v2 Slice F — projection / stencil painting (#549)
    /// @{
    int  projectionMode() const { return m_projectionMode; }
    void setProjectionMode(int mode);
    QString stencilImagePath() const { return m_stencilImagePath; }
    void setStencilImagePath(const QString& path);
    bool projBackfaceCull() const { return m_projBackfaceCull; }
    void setProjBackfaceCull(bool on);
    bool projUseOcclusion() const { return m_projUseOcclusion; }
    void setProjUseOcclusion(bool on);
    double projDepthLimit() const { return m_projDepthLimit; }
    void setProjDepthLimit(double v);
    bool cameraLocked() const { return m_cameraLocked; }

    /// Capture the live viewport camera (+ occlusion depth map) as the locked
    /// projection frame for camera-locked mode. No-op without an active widget.
    Q_INVOKABLE void snapProjectionCamera();
    /// One-shot: load `path`, project it through the current camera onto the
    /// mesh, and commit the result as a NEW layer (one undo step).
    Q_INVOKABLE bool projectFromPhoto(const QString& path);
    /// Open a file dialog to pick the stencil image (sets stencilImagePath).
    Q_INVOKABLE void chooseStencilImage();
    /// Open a file dialog to pick a photo, then projectFromPhoto() it.
    Q_INVOKABLE bool chooseAndProjectPhoto();
    /// @}

    /// @name Paint v2 Slice F — decal tool (#549)
    /// @{
    /// Open a file dialog, begin a decal session with that image, and switch to
    /// the Decal tool (the next viewport click places the rectangle).
    Q_INVOKABLE bool beginDecalInteractive();
    /// Begin a decal session with an already-loaded image path (no dialog).
    Q_INVOKABLE bool beginDecal(const QString& imagePath);
    bool decalSessionActive() const;
    int  decalState() const;   ///< DecalSession::State as int (0 idle/1 placing/2 editing)
    /// Place the decal rectangle at the surface point under `screenPos`.
    void placeDecalAt(OgreWidget* widget, const QPoint& screenPos);
    /// Classify the handle a viewport click at `screenPos` grabbed (int cast of
    /// DecalSession::Handle); also records the drag anchor.
    int  decalHitTest(OgreWidget* widget, const QPoint& screenPos);
    /// Drag the active decal handle to `screenPos` (translate/rotate/scale).
    void dragDecal(OgreWidget* widget, const QPoint& screenPos, int handle);
    /// Commit the decal onto the mesh as a NEW layer (one undo step). ESC/Enter
    /// path calls cancelDecal/commitDecal from mainwindow.
    Q_INVOKABLE bool commitDecal();
    Q_INVOKABLE void cancelDecal();
    /// @}

    /// @name Paint v2 Slice D — PBR channel painting (#547)
    /// @{
    /// Active PBR channel the brush paints into. Values are
    /// PaintChannelNS::Channel casts (0=BaseColor … 6=Height); VertexColor is
    /// not selected here (it's the Texture/Vertex `paintTarget` toggle).
    Q_PROPERTY(int activeChannel READ activeChannel WRITE setActiveChannel NOTIFY activeChannelChanged)
    /// Channel picker model: [{ id, label, slot, scalar, hasLayers }].
    /// Notified by paintChannelsChanged, which fires on channel switch AND on
    /// layer changes — `hasLayers` tracks the active channel's LIVE stack, which
    /// changes without a channel switch, so a layers-only NOTIFY would leave the
    /// picker's badges stale.
    Q_PROPERTY(QVariantList paintChannels READ paintChannels NOTIFY paintChannelsChanged)
    int activeChannel() const { return static_cast<int>(m_activeChannel); }
    void setActiveChannel(int channel);
    QVariantList paintChannels() const;
    /// Collapse/convert the given channel's painted buffer into its real PBR
    /// slot on the live material (scalar→ORM luminance, height→normal via
    /// Sobel) and re-wire the material for IBL. Returns false if the channel
    /// has no painted session. Exposed for the "Bake channel" button + tests.
    Q_INVOKABLE bool bakeChannel(int channel);
    /// @}

    /// Preview data URI (PNG, base64) regenerated on every dirty flush.
    QString previewDataUri() const { return m_previewUri; }
    /// Full-resolution preview URL — see the property doc.
    QString fullResPreviewUrl() const;
    /// Snapshot the current paint buffer as a QImage. Used by the
    /// `paintbuffer` QQuickImageProvider to hand QML a fresh copy
    /// on every request (no PNG encode, no base64).
    QImage snapshotBufferImage() const;
    /// Snapshot one layer's pixel buffer (for layer thumbnails).
    QImage snapshotLayerImage(int index) const;

    /// PNG data URI of the UV wireframe (white triangles on transparent
    /// background) at the current texture resolution. Lets the QML
    /// preview panel overlay UV islands on the texture so the user can
    /// see where their strokes are going relative to the unwrap.
    Q_PROPERTY(QString uvOverlayDataUri READ uvOverlayDataUri NOTIFY uvOverlayChanged)
    QString uvOverlayDataUri() const { return m_uvOverlayUri; }

    Q_PROPERTY(bool uvOverlayVisible READ uvOverlayVisible WRITE setUvOverlayVisible NOTIFY uvOverlayChanged)
    bool uvOverlayVisible() const { return m_uvOverlayVisible; }
    void setUvOverlayVisible(bool on);

    /// Paint via a UV coordinate directly (driven by the texture preview
    /// panel's mouse area). The preview ↔ 3D mesh hover indicator is
    /// driven by emitting hoveredUVChanged at the same time.
    Q_INVOKABLE bool beginStrokeUV(double u, double v);
    Q_INVOKABLE void updateStrokeUV(double u, double v);
    Q_INVOKABLE void endStrokeUV();
    /// Update the "hovered UV" without painting. Drives the brush ring
    /// overlay on the 3D mesh from the 2D preview panel.
    Q_INVOKABLE void setHoveredUV(double u, double v);
    Q_INVOKABLE void clearHoveredUV();

    /// Hover update from the 3D viewport — does a hit-test, emits the
    /// hovered UV signal, and draws the brush-ring overlay at the hit
    /// point in mesh-local space.
    void updateMeshHover(OgreWidget* widget, const QPoint& screenPos);
    void clearMeshHover();

    /// @name Stroke API (called from TransformOperator mouse pipeline)
    /// @{
    bool beginStroke(OgreWidget* widget, const QPoint& screenPos);
    void updateStroke(OgreWidget* widget, const QPoint& screenPos);
    void endStroke();
    /// @}

    /**
     * @brief Create or attach a paintable texture for the active edit entity.
     *
     * If the entity's first submesh material has an existing diffuse
     * texture, that texture is loaded into the paint buffer. Otherwise
     * a blank `resolution`x`resolution` opaque-white buffer is created
     * and bound under a generated texture name (`QMEPaint_<unique>`).
     *
     * @return true if a paint session is now active.
     */
    Q_INVOKABLE bool ensurePaintableTexture(int resolution = 1024);

    /**
     * @brief Save the current paint buffer to disk.
     *
     * @param path  Absolute path. Extension determines format (PNG by default).
     * @return true on success.
     */
    Q_INVOKABLE bool savePaintBuffer(const QString& path) const;

    /// Show a native save dialog and write the paint buffer to the chosen path.
    /// Returns the path on success or empty on cancel/failure.
    Q_INVOKABLE QString savePaintBufferInteractive();

    /**
     * @brief Load an image into the paint buffer (replaces contents) and
     * binds it as the active texture on the entity's first submesh.
     */
    Q_INVOKABLE bool loadPaintBuffer(const QString& path);

    /// Show a native open dialog and load the chosen image. Returns the
    /// path on success or empty on cancel/failure.
    Q_INVOKABLE QString loadPaintBufferInteractive();

    /// Write the current paint buffer **into the original texture's
    /// on-disk file** so exports pick up the painted pixels. Returns
    /// the disk path that was written, or empty if no source file was
    /// found (e.g. embedded texture).
    Q_INVOKABLE QString bakeToOriginalFile();

    /// Show a native color picker for the (shared) brush color. Writes
    /// back to EditModeController on accept.
    Q_INVOKABLE void pickBrushColorInteractive();

    /// Setters that mirror through to EditModeController so the toolbar
    /// brush popup and the Material-mode Paint Brush panel both stay
    /// in sync.
    Q_INVOKABLE void setBrushRadius(double r);
    Q_INVOKABLE void setBrushStrength(double s);
    Q_INVOKABLE void setBrushFalloff(double f);
    Q_INVOKABLE void setBrushColor(const QColor& c);

    /**
     * @brief Bake the active EditableMesh's vertex colors into the texture buffer.
     *
     * @param resolution  Square output resolution (defaults to current
     *                    buffer size if non-zero, else 1024).
     * @param dilation    Edge dilation in pixels after rasterization.
     * @param savePath    Optional path — if non-empty, the result is also
     *                    written to disk.
     * @return Pixel count rasterized (before dilation), or -1 on failure.
     */
    Q_INVOKABLE int bakeVertexColorsToTexture(int resolution = 0,
                                              int dilation = 4,
                                              const QString& savePath = QString());

    /// @brief End the session — release the paint buffer.
    Q_INVOKABLE void closeSession();

    /// @name Selection mask (smart-select / magic-wand)
    /// @{
    /// Smart-select tolerance, [0..1]. Mirrors GIMP's "Threshold" slider.
    /// 0 = exact color only; 1 = the whole texture.
    Q_PROPERTY(double smartSelectTolerance READ smartSelectTolerance
                       WRITE setSmartSelectTolerance NOTIFY smartSelectChanged)
    double smartSelectTolerance() const { return m_smartSelectTolerance; }
    void setSmartSelectTolerance(double t);

    /// True iff the user has selected at least one pixel.
    Q_PROPERTY(bool hasSelectionMask READ hasSelectionMask NOTIFY smartSelectChanged)
    bool hasSelectionMask() const;

    /// Number of pixels currently in the selection.
    Q_PROPERTY(int selectedPixelCount READ selectedPixelCount NOTIFY smartSelectChanged)
    int selectedPixelCount() const;

    /// PNG data URI of the selection mask (marching-ants–style outline
    /// rendered as a transparent overlay). Empty when no selection.
    Q_PROPERTY(QString maskOverlayDataUri READ maskOverlayDataUri NOTIFY smartSelectChanged)
    QString maskOverlayDataUri() const { return m_maskOverlayUri; }

    /// Clear the entire selection.
    Q_INVOKABLE void clearSelectionMask();
    /// Select every pixel.
    Q_INVOKABLE void selectAllMask();
    /// Invert the current selection.
    Q_INVOKABLE void invertSelectionMask();

    /// Smart-select via a UV coordinate. `mode` is 0=replace, 1=add, 2=subtract.
    /// Returns number of pixels added/removed.
    Q_INVOKABLE int smartSelectAtUV(double u, double v, int mode = 0);

    /// Open the detached texture editor window (or raise it if it's
    /// already open). The window shares the same paint pipeline as the
    /// inline 2D preview, so strokes done in it update the 3D viewport
    /// in real time and vice versa.
    Q_INVOKABLE void openEditorWindow();
    Q_INVOKABLE void closeEditorWindow();
    Q_PROPERTY(bool editorWindowOpen READ editorWindowOpen NOTIFY editorWindowChanged)
    bool editorWindowOpen() const { return m_editorWindow != nullptr; }

    /// Apply an action to the current selection. No-op when the mask is empty.
    /// Each action pushes a single undo command (TexturePaintMaskActionCommand).
    Q_INVOKABLE int fillMaskWithFG();
    Q_INVOKABLE int fillMaskWithBG();
    /// Delete = set selected pixels to fully transparent black (0,0,0,0).
    Q_INVOKABLE int deleteMaskPixels();
    /// @}

    /// Walk every UV-mapped triangle and return the local-space
    /// position + normal at `uv` (the first triangle that covers it
    /// in UV space). Used by the 2D-panel → 3D-mesh hover lookup;
    /// also exposed publicly so unit tests can verify reverse-UV
    /// math against an in-memory mesh.
    bool findMeshPointForUV(const Ogre::Vector2& uv,
                            Ogre::Vector3& outLocal,
                            Ogre::Vector3& outNormal,
                            int* outSubmesh = nullptr,
                            int* outTriangle = nullptr) const;

    // --- Paint v2 Slice E symmetry helpers (#548) ---
    /// Inverse of findMeshPointForUV: nearest triangle to a mesh-LOCAL point →
    /// interpolated UV (and optionally the winning submesh/triangle/barycentric).
    /// Returns false if no triangle lies within tolerance of `local`.
    bool uvForLocalPoint(const Ogre::Vector3& local,
                         Ogre::Vector2& outUV,
                         int* outSubmesh = nullptr,
                         int* outTriangle = nullptr,
                         float* outBary = nullptr /* [3] */) const;
    /// Reflect a mesh-LOCAL point across the given single axis bit about `pivot`.
    static Ogre::Vector3 reflectLocal(const Ogre::Vector3& p, int axisBit,
                                      const Ogre::Vector3& pivot);
    /// All mirror images of a primary LOCAL point for the currently-enabled
    /// axes: 1 point for a single axis, 3 for X|Y, 7 for X|Y|Z (every nonzero
    /// axis-subset). Empty when symmetry is disabled / no axes set.
    std::vector<Ogre::Vector3> mirrorLocalPoints(const Ogre::Vector3& primaryLocal) const;
    /// Resolve a mirror LOCAL point to a UV — topology-aware when enabled and a
    /// correspondence exists for `axisSubset`, else geometric (uvForLocalPoint).
    bool mirrorUvForLocalPoint(const Ogre::Vector3& mirrorLocal, int axisSubset,
                               const Ogre::Vector2& primaryUV,
                               Ogre::Vector2& outUV);
    /// Paint the primary dab's mirror images for the enabled symmetry. Called
    /// from the stroke-update paths AFTER the primary dab; all dabs land in the
    /// same buffer inside one begin/end window → captured by one undo command.
    void applyBrushSymmetryDabs(const Ogre::Vector2& primaryUV);
    /// Rebuild/clear the faint translucent symmetry-plane overlay.
    void refreshSymmetryPlaneOverlay();
    /// Drop the cached topology mirror maps (entity/mesh changed or axes toggled).
    void invalidateSymmetryMaps();

    // --- Paint v2 Slice E stabilizer helpers (#548) ---
    /// Smooth a raw screen position per the active stabilizer mode/amount.
    /// amount==0 is an exact passthrough (zero latency). Static-friendly math
    /// lives in stabilizeSamples for unit testing.
    QPointF stabilizeScreen(const QPointF& raw);

    /// Quick "would beginStroke hit the mesh at screenPos?" probe.
    /// Public wrapper around hitTestUV used by TransformOperator to
    /// decide whether a click landed on the mesh or empty space (for
    /// "click outside clears wand selection" behaviour). Lazily
    /// builds the EditableMesh / paint session if the user is making
    /// the first click on the model (otherwise the first hit would
    /// silently miss because m_paintMesh is still null at the time
    /// the TransformOperator press handler queries us). Not const
    /// for that reason.
    bool wouldStrokeHit(OgreWidget* widget, const QPoint& screenPos);

    /// Read-only access for tests.
    const TexturePaintBuffer& buffer() const { return m_buffer; }
    TexturePaintBuffer& mutableBuffer() { return m_buffer; }

    /// Internal: replace the composite buffer and re-upload. Legacy undo path.
    void applyPixelSnapshot(const std::vector<uint8_t>& pixels);
    /// Undo/redo: restore one layer's pixels then recompose.
    void applyLayerPixelSnapshot(int layerIndex, const std::vector<uint8_t>& pixels);
    /// Undo/redo: restore full layer stack state.
    void applyLayerStackSnapshot(const PaintLayerStack::Snapshot& snap);

    /// Undo/redo target resolution (#547): make the live session the one that
    /// owns `entity`'s `channel` before an undo command applies its snapshot.
    /// Reselects the entity + switches channel if needed. Returns false when
    /// the entity is gone (command becomes a safe no-op) — this replaces the
    /// old, brittle guard that keyed undo validity to the transient GPU texture
    /// name (which changes on every channel/session switch, so undo silently
    /// no-oped after a channel change).
    bool ensureUndoTarget(const std::string& entityName, int channel);

signals:
    void texturePaintChanged();
    void sessionChanged();
    void brushToolChanged();
    void paintTargetChanged();
    void slotsChanged();
    void previewChanged();
    void fullResPreviewChanged();
    void uvOverlayChanged();
    void smartSelectChanged();
    void editorWindowChanged();
    void gradientChanged();
    void rampEditorChanged();
    void stampChanged();
    void layersChanged();
    void activeChannelChanged();
    /// Fires whenever the paintChannels() picker model may have changed —
    /// on channel switch AND on layer add/remove/paint (so hasLayers badges
    /// stay live without a channel switch).
    void paintChannelsChanged();
    /// Paint v2 Slice E (#548): symmetry / stabilizer settings changed.
    void symmetryChanged();
    void stabilizerChanged();
    /// Paint v2 Slice F (#549): projection mode / stencil / lock state changed.
    void projectionChanged();
    /// Emitted when the mouse hovers over a UV-mapped triangle (from
    /// the 3D mesh or from the 2D texture preview panel). u,v in [0..1];
    /// (-1, -1) means "no hover".
    void hoveredUVChanged(double u, double v);

private:
    explicit TexturePaintController(QObject* parent = nullptr);
    ~TexturePaintController() override;

    /// Currently-selected entity (or Edit Mode's active entity if Edit
    /// Mode happens to be on). Painting no longer requires Edit Mode —
    /// we keep our own private EditableMesh built from this entity.
    Ogre::Entity* activeEntity() const;

    /// Ensure the private EditableMesh is built for the active entity.
    /// Called on session creation and whenever the selection changes.
    bool ensureEditableMesh(Ogre::Entity* entity);

    /// Find the texture unit corresponding to the currently-active slot,
    /// creating one if no diffuse-like TUS exists on the selected submesh.
    Ogre::TextureUnitState* findOrCreateActiveTextureUnit(Ogre::Entity* entity);

    /// Hit-test screen position against the private editable mesh and
    /// recover the barycentric-interpolated UV at the hit point. Returns
    /// false on miss.
    bool hitTestUV(const QPoint& screenPos, OgreWidget* widget, Ogre::Vector2& outUV) const;
    /// Ray-test only the cached triangle (see m_hitCache). Used while
    /// dragging along a continuous surface patch.
    bool tryHitTestCachedTriangle(const Ogre::Vector3& localOrigin,
                                  const Ogre::Vector3& localDir,
                                  Ogre::Vector2& outUV) const;
    /// Stroke-only hit test: extrapolates UV from recent screen deltas
    /// to skip full mesh raycasts on most mouse-move events.
    bool hitTestUVForStroke(const QPoint& screenPos, OgreWidget* widget,
                            Ogre::Vector2& outUV);
    void processPendingStrokeUpdate();
    void processPendingStrokeUpdateUV();

    /// Same hit-test but returns the local-space position and normal
    /// at the hit (for vertex paint, which works in 3D space).
    bool hitTestLocalPoint(OgreWidget* widget, const QPoint& screenPos,
                           Ogre::Vector3& outLocal, Ogre::Vector3& outNormal) const;

    /// Allocate a new manual Ogre::Texture with current buffer
    /// dimensions. When `rebindToModel` is true, also walk the
    /// entity's materials and rebind diffuse TUSes pointing at the
    /// original texture to the paint texture. When false (the default
    /// for session create), the GPU texture is allocated but the
    /// model's render is left alone until the first stroke
    /// committedly modifies pixels.
    bool createOgreTextureFromBuffer(Ogre::Entity* entity,
                                     const QString& nameHint,
                                     bool rebindToModel = false);

    /// Rebind diffuse TUSes pointing at the original-slot texture to
    /// the paint texture. Records the original bindings in
    /// `m_boundSlots` so closeSession() can restore them. Called
    /// either eagerly (rebindToModel=true on session create) or
    /// lazily (on the first stroke flush, so toggling the brush is
    /// non-destructive).
    void rebindEntityDiffuseToPaintTexture(Ogre::Entity* entity);

    /// Upload buffer.dirtyRect() into the live Ogre texture and clear it.
    /// This is the public entry point and **debounces** to ~16 ms — the
    /// actual GPU work happens in doFlushDirtyToOgre on a timer.
    void flushDirtyToOgre();
    /// The synchronous GPU upload. Called from the debounce timer.
    void doFlushDirtyToOgre(bool immediate = false);
    /// Upload one dirty rect as 64×64 tiles, one tile per event-loop
    /// tick, so GL blits never stall the UI for long.
    void scheduleStrokeGpuFlush();
    /// At most ~60 GPU blits/sec while the brush is down (CPU paint is immediate).
    void scheduleThrottledLiveGpuFlush();
    /// Synchronous GPU blit of the current dirty rect during live strokes.
    bool flushLiveStrokeToGpu();
    /// Rebuild composite CPU buffer from dirty layers (no GPU).
    void recomposePaintBufferIfNeeded();
    /// Stop an in-progress tiled upload (e.g. prior stroke still draining).
    void cancelInFlightGpuUpload();
    void startTiledGpuUpload(bool finishingStroke);
    void processNextTiledUploadTile();
    void onTiledUploadPassComplete();
    /// Texture the viewport samples — original (in-place) or manual paint tex.
    Ogre::TexturePtr gpuUploadTargetTexture() const;
    bool blitBufferRectToOgreTexture(int x0, int y0, int x1, int y1);
    void commitStrokeUndo(std::vector<uint8_t> prePixels, int layerIndex);
    /// Shared per-stroke reset — must stay in sync for viewport + UV preview paths.
    void resetStrokePaintState();
    void scheduleEmbeddedTextureCacheUpdate();
    void invalidateLayerStrokeBaseline();
    /// 3D brush ring during strokes — uses the cached hit triangle only.
    bool localPointFromHitCache(const Ogre::Vector2& uv,
                                Ogre::Vector3& outLocal,
                                Ogre::Vector3& outNormal) const;
    /// Point the model's diffuse TUSes at `m_ogreTexture` on the next
    /// event-loop tick (mat compile/reload must not run mid-stroke).
    void scheduleRebindToPaintTexture(Ogre::Entity* entity);

    /// Regenerate `m_previewUri` from the buffer (PNG, base64). Emits
    /// previewChanged when the URI actually changed.
    void refreshPreviewUri();
    /// Debounced Inspector / editor-window preview refresh (reads CPU buffer).
    void schedulePreviewRefresh();

    /// Regenerate `m_uvOverlayUri` by drawing every UV-mapped triangle
    /// outline at the current texture resolution into a transparent PNG.
    void refreshUvOverlay();

    /// One-time deep-copy snapshot of pixel buffer for undo.
    std::vector<uint8_t> snapshotPixels() const;

    /// Apply the brush stamp at a UV coord using the current tool.
    /// Returns true if any pixel changed.
    bool applyBrushAtUV(const Ogre::Vector2& uv);

    /// Brush radius mapped into UV space (matches applyBrushAtUV).
    float brushRadiusUV() const;

    /// Stamp along a UV segment so fast cursor moves don't leave gaps.
    /// Returns true if any dab modified pixels.
    bool paintBrushAlongSegment(const Ogre::Vector2& from, const Ogre::Vector2& to);

    /// Resolve the active GradientRamp (FG/BG quick mode, custom, or bundled).
    const GradientRamp::Ramp* resolveActiveRamp() const;
    /// Rebuild `m_activeRamp` / preview URI after name or stop edits.
    void reloadActiveRamp();
    void refreshRampPreviewUri();
    /// Update stroke path-length tracking used by linear gradients.
    void noteStrokeSample(const Ogre::Vector2& uv, bool isStart);
    float strokeDirectionRad() const;
    TexturePaintBuffer::BrushShape currentBrushShape() const;
    TexturePaintBuffer::ColorAtFn buildBrushColorAtFn(float strokeT) const;
    bool paintColorFootprintAtUV(const Ogre::Vector2& uv, float radiusUv,
                                 float strength);
    void reloadStampImage();
    void reloadTilingImage();
    void rebuildStampCache(float radiusUv);
    void refreshStampPreviewUris();

    /// CPU buffer the brush paints into (active layer).
    TexturePaintBuffer& activePaintBuffer();
    const TexturePaintBuffer& activePaintBuffer() const;
    /// Rebuild `m_buffer` from visible layers and mark dirty.
    /// @p fullBuffer forces a full-stack composite (layer add/delete/undo).
    void recomposeComposite(bool fullBuffer = false);
    void pushLayerOpUndo(const QString& label,
                         PaintLayerStack::Snapshot before,
                         PaintLayerStack::Snapshot after);
    std::vector<uint8_t> snapshotActiveLayerPixels() const;

    /// Draw the hover ring on the mesh at a given local position +
    /// normal. Shared between viewport-driven and panel-driven hover.
    void drawHoverRingAt(const Ogre::Vector3& localPos,
                         const Ogre::Vector3& localNormal);

    /// Flood-fill connected pixels at UV with the brush color.
    bool floodFillAtUV(const Ogre::Vector2& uv);

    /// Sample the buffer color at a UV and set it as the brush color.
    void pickColorAtUV(const Ogre::Vector2& uv);

    bool m_paintEnabled = false;
    /// Composite display buffer uploaded to the GPU.
    TexturePaintBuffer m_buffer;
    PaintLayerStack m_layerStack;
    quint64 m_layerPreviewVersion = 0;

    // Paint v2 Slice D (#547) — per-channel sessions.
    //
    // The active channel's session lives in the m_buffer / m_layerStack /
    // m_originalTexture* fields (unchanged — the whole stroke/GPU/undo path
    // keeps operating on "the active session"). Switching channels STASHES the
    // current session's layer stack + baseline here and RESTORES the target
    // channel's, so each channel keeps its own layer stack across switches.
    PaintChannelNS::Channel m_activeChannel = PaintChannelNS::Channel::BaseColor;
    struct ChannelSessionState {
        PaintLayerStack layerStack;
        std::vector<uint8_t> layerStrokeBaseline;
        bool initialized = false;   ///< has this channel's stack been built?
    };
    QMap<int, ChannelSessionState> m_channelSessions;  ///< key = (int)Channel
    /// The entity the per-channel sessions belong to. When the painted entity
    /// changes, the stashed stacks are cleared so one entity's paint never
    /// leaks into (or bakes onto) another.
    Ogre::Entity* m_channelSessionEntity = nullptr;
    /// For the Normal channel: the real normal_map texture the slot held when
    /// the session opened (NOT the transient paint texture, and NOT seeded into
    /// the paint buffer). bakeChannel whiteout-blends the painted detail normal
    /// onto this base so the model's existing normal survives where unpainted.
    QString m_channelBaseTextureName;
    /// Stash the live session into m_channelSessions[channel].
    void stashChannelSession(PaintChannelNS::Channel channel);
    /// Restore m_channelSessions[channel] into the live session (or mark it
    /// uninitialized so ensurePaintableTexture builds it fresh). Returns true
    /// if a stored stack was restored.
    bool restoreChannelSession(PaintChannelNS::Channel channel);
    /// The canonical PBR slot name the active channel targets (or "" for
    /// Height, which bakes into normal_map). Drives findOrCreateActiveTextureUnit.
    std::string activeChannelSlotName() const;
    /// Texture name currently bound to the named slot on the active entity's
    /// first material (empty if none). Used by scalar bake to merge ORM lanes.
    QString currentSlotTextureName(const std::string& slot) const;
    /// Load an Ogre-resolvable texture name into a QImage (across groups /
    /// generated_textures / media). Empty QImage on failure.
    QImage loadImageAcrossGroups(const QString& textureName) const;
    /// Bind a baked texture file (basename) into `slot` on every material of
    /// `entity`, then wire the material for FFP + Cook-Torrance/IBL (+ tangents
    /// for normal maps). The live-IBL binding recipe for baked channels.
    void bindBakedChannelTexture(Ogre::Entity* entity, const std::string& slot,
                                 const std::string& textureBaseName,
                                 PaintChannelNS::Channel channel);
    QString m_textureName;
    Ogre::TexturePtr m_ogreTexture;
    /// The original texture we're painting into. We blit our dirty
    /// rects directly to its GPU buffer when possible, so the
    /// existing material binding is untouched. Falls back to
    /// `m_ogreTexture` (our manual paint texture) when the original
    /// isn't writable.
    Ogre::TexturePtr m_originalTexture;
    QString m_originalTextureName;
    bool m_useOriginalTexture = false;
    /// When true, skip in-place blit into `m_originalTexture` and always
    /// upload to `m_ogreTexture` (rebind required for the viewport).
    bool m_forceManualPaintTexture = false;
    bool m_loggedInPlaceBlit = false;
    bool m_rebindScheduled = false;
    /// Debounce flag for the GPU upload. We accumulate dirty pixels
    /// in m_buffer and schedule a single blit per ~16ms instead of
    /// blitting on every mouse-move (which hits 100+ Hz and uploads
    /// 4 MB each time on macOS Metal).
    bool m_gpuFlushScheduled = false;
    /// Set during an active stroke when CPU pixels changed; consumed
    /// by scheduleStrokeGpuFlush() for tiled GPU uploads.
    bool m_strokeGpuFlushScheduled = false;
    bool m_strokeLiveUploadStarted = false;
    bool m_strokeGpuFlushPending = false;
    struct UploadTile { int x0 = 0; int y0 = 0; int x1 = 0; int y1 = 0; };
    std::vector<UploadTile> m_tiledUploadQueue;
    int m_tiledUploadIndex = 0;
    bool m_tiledUploadRunning = false;
    /// Whether the current tiled upload pass is the final flush before idle.
    bool m_uploadFinishingStroke = false;
    /// Incremented on every new stroke and whenever pixels change — stale
    /// uploads from a prior stroke must not clearDirty() or finish undo.
    uint64_t m_gpuUploadGeneration = 0;
    uint64_t m_activeUploadGeneration = 0;
    uint64_t m_bufferDirtyEpoch = 0;
    uint64_t m_uploadEpochSnapshot = 0;
    TexturePaintBuffer::DirtyRect m_uploadPassDirty;
    uint64_t m_strokeUndoGeneration = 0;
    OgreWidget* m_pendingStrokeWidget = nullptr;
    QPoint m_pendingStrokePos;
    double m_pendingStrokeU = 0.0;
    double m_pendingStrokeV = 0.0;
    /// Screen→UV gradient for cheap extrapolation between raycasts.
    bool m_strokeHaveHitScreen = false;
    QPoint m_strokeLastHitScreen;
    Ogre::Vector2 m_strokeLastHitUV = Ogre::Vector2::ZERO;
    float m_strokeUvPerScreenX = 0.0f;
    float m_strokeUvPerScreenY = 0.0f;
    Ogre::Entity* m_sessionEntity = nullptr;

    bool m_strokeActive = false;
    bool m_strokeJustBegan = false; ///< Fill/picker tools fire only once per stroke.
    bool m_strokeFromUvPreview = false;
    bool m_strokeMadeChanges = false;
    bool m_embeddedCacheUpdateScheduled = false;
    bool m_layerOpacityDragging = false;
    bool m_layerOpacityDragChanged = false;
    PaintLayerStack::Snapshot m_layerOpacityDragBefore;
    /// Layer pixels at the end of the last stroke — O(1) handoff as the next pre-image.
    std::vector<uint8_t> m_layerStrokeBaseline;
    std::vector<uint8_t> m_strokePreSnapshot; // for undo
    BrushTool m_tool = ToolPaint;
    PaintTarget m_target = TargetVertex;

    // Paint v2 Slice A — gradient ramp state.
    ColorSource m_colorSource = ColorSolid;
    GradientMode m_gradientMode = GradientLinear;
    QString m_activeRampName = QStringLiteral("Sunset");
    bool m_useFgBgRamp = false;
    bool m_gradientStepped = false;
    double m_rampJitter = 0.0; ///< 0..1 max random phase offset per stroke.
    GradientRamp::Ramp m_activeRamp;
    QString m_rampPreviewUri;
    QObject* m_rampEditorWindow = nullptr;

    // Per-stroke path tracking for linear gradients (smoothed length).
    Ogre::Vector2 m_strokePrevUV = Ogre::Vector2::ZERO;
    bool m_strokeHavePrevUV = false;
    float m_strokePathLength = 0.0f;
    float m_strokePhaseJitter = 0.0f;
    /// EMA of the stroke direction unit vector — keeps linear sampling
    /// stable when the cursor turns sharply mid-stroke.
    Ogre::Vector2 m_strokeDirSmoothed = Ogre::Vector2::ZERO;

    // Paint v2 Slice B — stamp / tiling footprint state.
    BrushFootprint::FootprintType m_footprintType = BrushFootprint::FootprintType::Round;
    QString m_activeStampName = QStringLiteral("Soft Circle");
    QString m_activeTilingName = QStringLiteral("Wood");
    BrushFootprint::StampSettings m_stampSettings;
    BrushFootprint::TilingSettings m_tilingSettings;
    BrushFootprint::ImageRgba m_stampImage;
    BrushFootprint::ImageRgba m_tilingImage;
    BrushFootprint::RasterizedStamp m_stampCache;
    int m_stampCachePixelSize = 0;
    QString m_stampPreviewUri;
    QString m_tilingPreviewUri;
    float m_lastStampDabPathLength = 0.0f;

    /// Last ray-hit triangle for fast stroke tracking (avoids walking
    /// every triangle on each mouse-move while the cursor stays on the
    /// same surface patch).
    struct PaintHitCache {
        int submesh = -1;
        int triangle = -1;
        QPoint screenPos;
        bool valid = false;
    };
    mutable PaintHitCache m_hitCache;

    /// Track every TUS we rebound to the paint texture so closeSession()
    /// can restore the originals. We keep the *material name* (not a
    /// raw pointer) so a destroyed/reloaded material doesn't dangle.
    struct BoundSlot {
        std::string materialName;
        unsigned short techIdx = 0;
        unsigned short passIdx = 0;
        unsigned short tusIdx = 0;
        std::string originalTexture;
    };
    std::vector<BoundSlot> m_boundSlots;

    // Private EditableMesh — built from the active entity so painting
    // doesn't depend on the user-facing Edit Mode workspace.
    std::unique_ptr<EditableMesh> m_paintMesh;
    Ogre::Entity* m_paintMeshEntity = nullptr;

    // Texture slot model. Populated from the selected entity's
    // materials. The active slot drives which TUS the painter writes
    // back to and which texture name the QML preview shows.
    QVariantList m_slots;
    int m_activeSlot = 0;

    // Smudge state: hold the previous stamp's pre-paint sample so the
    // next stamp can copy it forward.
    Ogre::Vector2 m_smudgePrev = Ogre::Vector2::ZERO;
    bool m_smudgeHavePrev = false;

    // Brush-ring overlay on the 3D mesh surface — drawn at the
    // current hover hit point so the user sees brush size in world
    // units while painting.
    Ogre::SceneNode* m_ringNode = nullptr;
    Ogre::ManualObject* m_ringObj = nullptr;

    // --- Paint v2 Slice E (#548): symmetry state ---
    bool m_symmetryEnabled = false;                       // starts OFF
    SymmetrySpace m_symmetrySpace = SymLocal;             // LOCAL X default when enabled
    int  m_symmetryAxes = SymAxisX;                       // bitmask; default X
    bool m_topologyMirror = true;                         // full-spec default on
    Ogre::Vector3 m_symmetryPivotLocal = Ogre::Vector3::ZERO;  // mesh local origin
    /// Per-single-axis topology mirror maps, built lazily; guarded by the entity
    /// they were built for (invalidated on entity/mesh change).
    std::unordered_map<int, SymmetryMirrorMap> m_symmetryMaps;
    Ogre::Entity* m_symmetryMapEntity = nullptr;
    /// Per-mirror-subset previous UV, so mirror strokes fan segments without gaps.
    std::vector<Ogre::Vector2> m_mirrorPrevUV;
    std::vector<bool> m_mirrorHavePrevUV;
    // Symmetry-plane overlay (mirrors the m_ring* hover-ring pattern).
    Ogre::SceneNode* m_symPlaneNode = nullptr;
    Ogre::ManualObject* m_symPlaneObj = nullptr;

    // --- Paint v2 Slice E (#548): line stabilizer state ---
    StabilizerMode m_stabilizerMode = StabAverage;
    double m_stabilizerAmount = 0.0;      // 0..100; 0 = passthrough (zero latency)
    std::deque<QPointF> m_stabSamples;    // moving-average window
    QPointF m_stabTrailPos;               // trail-mode lagging brush position
    bool    m_stabHaveTrail = false;
    QPointF m_stabLastRaw;                // true last cursor (for end catch-up)
    bool    m_stabHaveLastRaw = false;

    // --- Paint v2 Slice F (#549): projection / stencil painting ---
    int     m_projectionMode = 0;         // 0 off / 1 stencil-brush / 2 camera-locked
    QString m_stencilImagePath;
    QImage  m_stencilImage;               // loaded stencil (alpha-masks each dab)
    bool    m_projBackfaceCull = true;
    bool    m_projUseOcclusion = false;
    double  m_projDepthLimit = 0.0;       // fraction of bounds radius; 0 = off
    bool    m_cameraLocked = false;
    ProjectionPainter::View m_lockedView; // captured on snap (mode 2)
    bool    m_haveLockedView = false;
    ProjectionPainter::OcclusionMap m_projOcc;
    bool    m_haveProjOcc = false;
    std::vector<ProjectionPainter::Triangle> m_projTris;  // world tris cached per stroke
    bool    m_haveProjTris = false;

    // --- Paint v2 Slice F (#549): decal tool ---
    DecalSession m_decal;
    Ogre::SceneNode* m_decalNode = nullptr;
    Ogre::ManualObject* m_decalObj = nullptr;
    QPoint  m_decalDragLastPos;           // last screen pos during a handle drag
    bool    m_haveDecalDragPos = false;
    /// Live WYSIWYG preview of the decal image on the quad (Slice F follow-up):
    /// the decal image uploaded to a GPU texture + the unlit transparent material
    /// that samples it. Built lazily on the first Editing refresh, re-uploaded
    /// only when the image itself changes (NOT on every drag), torn down in
    /// closeSession.
    Ogre::TexturePtr m_decalPreviewTex;
    std::string      m_decalPreviewMatName;
    qint64           m_decalPreviewImageKey = 0;   // cacheKey() of the uploaded image
    /// Upload `m_decal.image()` into m_decalPreviewTex + ensure the sampling
    /// material exists. Returns the material name, or "" if unavailable.
    std::string ensureDecalPreviewMaterial();
    /// Create/resize m_decalPreviewTex to (W,H). False if unavailable.
    bool ensureDecalPreviewTexture(int W, int H);
    /// Feather `img` to match the commit and blit it into m_decalPreviewTex.
    /// False if the blit could not run — the caller must then NOT stamp the
    /// cache key, so the next refresh retries.
    bool uploadDecalPreviewPixels(const QImage& img, int W, int H);
    /// Build (once) or re-point the material that samples the preview texture.
    bool ensureDecalPreviewPass();
    /// Destroy the decal preview texture/material (called from closeSession).
    void destroyDecalPreview();
    /// Rebuild the decal rectangle + handle overlay (or hide it when inactive).
    void refreshDecalOverlay();
    /// Ray-pick the decal rect plane at `screenPos` → world hit (on the plane).
    bool decalPlaneHit(OgreWidget* widget, const QPoint& screenPos,
                       Ogre::Vector3& outWorld) const;

    /// Cache the entity's world triangles for the projection stroke (once).
    void ensureProjTris();
    /// Read the View straight off the live viewport camera, ignoring any locked
    /// pose. `snapProjectionCamera` uses this so a re-snap re-pins to where the
    /// camera is NOW instead of copying the stored pose onto itself.
    bool liveCameraView(OgreWidget* widget, ProjectionPainter::View& out) const;
    /// Build the projection View from the live viewport camera (mode 1) or the
    /// locked view (mode 2). Returns false if no camera is available.
    bool currentProjectionView(OgreWidget* widget, ProjectionPainter::View& out) const;
    /// Render an occlusion depth map for `view` from the live entity; returns
    /// false (and leaves *occ untouched) if depth rendering is unavailable.
    bool buildOcclusionForView(const ProjectionPainter::View& view,
                               ProjectionPainter::OcclusionMap& occ) const;
    /// Fill Options from the current projection settings (bounds-scaled depth limit).
    ProjectionPainter::Options projectionOptions() const;
    /// Commit a fully-projected buffer as a NEW active layer (one undo step).
    int commitProjectedLayer(const TexturePaintBuffer& projected, const QString& name);

    // Preview PNG cache.
    QString m_previewUri;
    /// Monotonic counter appended to the full-res image URL so QML
    /// invalidates Image cache on every refresh and re-pulls from
    /// the `paintbuffer` provider.
    quint64 m_fullResVersion = 0;
    /// Debounce flag: prevents stroke moves from regenerating the
    /// base64 PNG on every dirty flush (expensive at 1024×1024).
    bool m_previewRefreshScheduled = false;

    // UV wireframe overlay (data: URI of a transparent PNG with the
    // triangle outlines drawn at the texture resolution).
    QString m_uvOverlayUri;
    bool m_uvOverlayVisible = false;

    // Smart-select state. Mask is per-pixel boolean, paired with
    // m_buffer; the overlay PNG mirrors it for QML.
    PaintSelectionMask m_mask;
    double m_smartSelectTolerance = 0.15;
    QString m_maskOverlayUri;
    bool m_maskOverlayRefreshScheduled = false;

    // Wand "drag during stroke" state. While the user holds the mouse
    // down with the smart-select tool active we remember the seed UV
    // and the tolerance at press time; subsequent moves don't reseed
    // (would jitter the selection) — they nudge the tolerance via
    // horizontal mouse displacement and re-run smartSelect at the
    // original seed.
    bool m_wandStrokeActive = false;
    Ogre::Vector2 m_wandSeedUV = Ogre::Vector2::ZERO;
    double m_wandStartTolerance = 0.15;
    QPoint m_wandStartScreenPos;

    /// Regenerate `m_maskOverlayUri` (PNG, base64) from `m_mask`.
    /// Debounced through QTimer::singleShot.
    void scheduleMaskOverlayRefresh();
    void refreshMaskOverlay();

    /// Re-encode the current buffer as PNG and push it into
    /// EmbeddedTextureCache so the next FBX export (or RTSS rebind)
    /// sees the painted pixels. Must be called from every code path
    /// that mutates `m_buffer` — stroke end, mask actions, undo /
    /// redo — otherwise downstream consumers observe stale bytes
    /// from before the mutation. Cheap PNG encode (~1024² is a few
    /// ms) so callers don't need to debounce.
    void updateEmbeddedTextureCache();

    // Detached texture editor window. Owned heap-allocated; instantiated
    // lazily when the user clicks "Open Editor Window" and torn down on
    // window close. Held as a generic QObject* so the header doesn't
    // need to pull in the QML engine headers.
    QObject* m_editorWindow = nullptr;

    // On-mesh wand-selection overlay: a yellow tinted copy of the mesh
    // rendered just above the original geometry in mesh-local space,
    // textured with the mask PNG so users can see which pixels in UV
    // space line up with which spots on the 3D model. Built on demand
    // when the mask becomes non-empty; torn down when it clears.
    Ogre::Entity*       m_maskOverlayEntity = nullptr;
    Ogre::SceneNode*    m_maskOverlayNode = nullptr;
    Ogre::TexturePtr    m_maskOverlayTex;
    std::string         m_maskOverlayMatName;

    /// (Re)build the per-mesh mask overlay so the selected pixels
    /// appear highlighted on the 3D model. Called whenever the mask
    /// changes shape. Tears down the overlay when the mask is empty.
    void refreshMeshMaskOverlay();
    /// Free overlay scene state. Safe to call when nothing is bound.
    void destroyMeshMaskOverlay();

    static TexturePaintController* s_instance;
};

#endif // TEXTUREPAINTCONTROLLER_H
