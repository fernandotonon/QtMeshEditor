#ifndef MATERIAL_PREVIEW_RENDERER_H
#define MATERIAL_PREVIEW_RENDERER_H

#include <QObject>
#include <QImage>
#include <QString>
#include <QHash>
#include <QQmlEngine>
#include <Ogre.h>

/**
 * @brief Singleton that renders material previews using Ogre render-to-texture.
 *
 * Maintains a small offscreen Ogre scene with a lit sphere. For a given
 * material name the sphere is assigned that material, the scene is rendered
 * to a 64x64 RGBA texture, and the result is returned as a QImage or
 * base64-encoded data URI suitable for QML Image.source.
 */
class MaterialPreviewRenderer : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    /// Slice I: which procedural mesh the preview is rendered on. The
    /// thumbnail path stays on Sphere; the interactive Inspector
    /// preview can switch.
    enum Shape {
        ShapeSphere = 0,
        ShapeCube   = 1,
        ShapePlane  = 2,
    };
    Q_ENUM(Shape)

    static MaterialPreviewRenderer* instance();
    static MaterialPreviewRenderer* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// Render a 64x64 preview of the named Ogre material on a lit sphere.
    /// Returns a null QImage if the material cannot be found or Ogre is not ready.
    /// Always uses Shape=Sphere and yaw=0 for cache stability.
    QImage renderPreview(const QString& materialName);

    /// Convenience: returns the preview as a data:image/png;base64,... URI string.
    /// Returns an empty string on failure.
    Q_INVOKABLE QString renderPreviewAsDataUri(const QString& materialName);

    /// Slice I: render an arbitrary-size preview with a chosen shape and
    /// environment yaw. Not cached — meant for the interactive Inspector
    /// preview panel which re-renders on user input. Returns a
    /// `data:image/png;base64,…` URL the QML Image element shows directly.
    /// `size` is clamped to [32, 1024]; `yawDegrees` is wrapped to [0, 360).
    /// `shape` is one of the Shape enum values; out-of-range falls back to Sphere.
    /// Results are cached per (material, size, shape, yaw) tuple.
    Q_INVOKABLE QString renderInteractivePreview(const QString& materialName,
                                                  int size,
                                                  int shape,
                                                  double yawDegrees);

    /// Clear the cached previews (e.g. when materials are reloaded).
    Q_INVOKABLE void clearCache();

    /// Parse a .material file and return the first material name found.
    /// Returns an empty string if none found.
    static QString firstMaterialNameInFile(const QString& filePath);

private:
    MaterialPreviewRenderer();
    ~MaterialPreviewRenderer() override;

    bool ensureScene();
    /// Slice I: ensure the procedural mesh for `shape` is created and
    /// return its name. Lazily creates each mesh on first use.
    Ogre::String ensureShapeMesh(Shape shape);
    /// Slice I: restore the shared preview scene to the canonical
    /// "Sphere + default light" state before the thumbnail path
    /// renders. renderInteractivePreview can leave the entity on a
    /// Cube/Plane mesh or rotate the light; without this, the cached
    /// thumbnails would pick up that interactive state and violate
    /// the documented thumbnail-always-Sphere invariant.
    void resetToCanonicalThumbnailState();

    static MaterialPreviewRenderer* m_pSingleton;

    Ogre::SceneManager* m_sceneMgr = nullptr;
    Ogre::Camera* m_camera = nullptr;
    Ogre::Light* m_light = nullptr;
    Ogre::SceneNode* m_lightNode = nullptr;
    Ogre::Entity* m_sphere = nullptr;
    Ogre::SceneNode* m_sphereNode = nullptr;
    Ogre::TexturePtr m_rttTexture;
    Ogre::RenderTarget* m_renderTarget = nullptr;

    // Slice I: separate larger RTT for the interactive preview. We
    // reallocate it when the requested size changes.
    Ogre::TexturePtr m_interactiveRtt;
    Ogre::RenderTarget* m_interactiveRenderTarget = nullptr;
    int m_interactiveSize = 0;
    Shape m_interactiveCurrentShape = ShapeSphere;

    bool m_initialized = false;

    // Cache: materialName -> base64 data URI for the 64x64 thumbnail path.
    QHash<QString, QString> m_cache;

    /// Slice I: interactive preview was documented as uncached; cache now
    /// avoids re-entering Ogre RTT when QML re-evaluates bindings.
    QHash<QString, QString> m_interactiveCache;

    static constexpr int PREVIEW_SIZE = 64;
};

#endif // MATERIAL_PREVIEW_RENDERER_H
