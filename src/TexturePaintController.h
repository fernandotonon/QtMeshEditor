#ifndef TEXTUREPAINTCONTROLLER_H
#define TEXTUREPAINTCONTROLLER_H

#include "PaintSelectionMask.h"
#include "TexturePaintBuffer.h"
#include "BrushEngine.h"
#include "GradientRamp.h"

#include <QColor>
#include <QObject>
#include <QPoint>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <OgreTexture.h>
#include <OgreVector.h>

#include <memory>
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

public:
    enum BrushTool {
        ToolPaint       = 0,  ///< Lerp pixels toward brush color.
        ToolErase       = 1,  ///< Paint with BG color (was: paint transparent).
        ToolFill        = 2,  ///< Flood-fill connected pixels under cursor.
        ToolColorPicker = 3,  ///< Sample color at hit UV into the brush color.
        ToolSmudge      = 4,  ///< Drag pixels in the brush direction.
        ToolSmartSelect = 5,  ///< Fuzzy / magic-wand region select by color.
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

    /// Preview data URI (PNG, base64) regenerated on every dirty flush.
    QString previewDataUri() const { return m_previewUri; }
    /// Full-resolution preview URL — see the property doc.
    QString fullResPreviewUrl() const;
    /// Snapshot the current paint buffer as a QImage. Used by the
    /// `paintbuffer` QQuickImageProvider to hand QML a fresh copy
    /// on every request (no PNG encode, no base64).
    QImage snapshotBufferImage() const;

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
    /// @deprecated Stroke GPU sync is deferred to stroke end / live-preview timer.
    void onRenderFrame();
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
                            Ogre::Vector3& outNormal) const;

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

    /// Internal: replace the buffer pixel data and re-upload to the
    /// live Ogre texture. Used by the undo command. Width/height must
    /// match the current buffer.
    void applyPixelSnapshot(const std::vector<uint8_t>& pixels);

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
    /// Coalesce rapid mouse-move events into one paint step per tick.
    void scheduleStrokeUpdate(OgreWidget* widget, const QPoint& screenPos);
    void processPendingStrokeUpdate();
    void scheduleStrokeUpdateUV(double u, double v);
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
    void startTiledGpuUpload(bool finishingStroke);
    void processNextTiledUploadTile();
    void onTiledUploadPassComplete();
    /// Texture the viewport samples — original (in-place) or manual paint tex.
    Ogre::TexturePtr gpuUploadTargetTexture() const;
    bool blitBufferRectToOgreTexture(int x0, int y0, int x1, int y1);
    void finishStrokeAfterGpuUpload();
    void finishStrokeAfterGpuUploadDeferred();
    void ensureStrokePreSnapshot();
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

    /// Draw the hover ring on the mesh at a given local position +
    /// normal. Shared between viewport-driven and panel-driven hover.
    void drawHoverRingAt(const Ogre::Vector3& localPos,
                         const Ogre::Vector3& localNormal);

    /// Flood-fill connected pixels at UV with the brush color.
    bool floodFillAtUV(const Ogre::Vector2& uv);

    /// Sample the buffer color at a UV and set it as the brush color.
    void pickColorAtUV(const Ogre::Vector2& uv);

    bool m_paintEnabled = false;
    TexturePaintBuffer m_buffer;
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
    bool m_strokeEndAfterUpload = false;
    TexturePaintBuffer::DirtyRect m_uploadPassDirty;
    /// Batches hundreds of mouse-move events into one paint/upload step.
    bool m_strokeUpdateScheduled = false;
    OgreWidget* m_pendingStrokeWidget = nullptr;
    QPoint m_pendingStrokePos;
    bool m_strokeUpdateUVScheduled = false;
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
