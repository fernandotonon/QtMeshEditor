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
    static MaterialPreviewRenderer* instance();
    static MaterialPreviewRenderer* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// Render a 64x64 preview of the named Ogre material on a lit sphere.
    /// Returns a null QImage if the material cannot be found or Ogre is not ready.
    QImage renderPreview(const QString& materialName);

    /// Convenience: returns the preview as a data:image/png;base64,... URI string.
    /// Returns an empty string on failure.
    Q_INVOKABLE QString renderPreviewAsDataUri(const QString& materialName);

    /// Clear the cached previews (e.g. when materials are reloaded).
    Q_INVOKABLE void clearCache();

    /// Parse a .material file and return the first material name found.
    /// Returns an empty string if none found.
    static QString firstMaterialNameInFile(const QString& filePath);

private:
    MaterialPreviewRenderer();
    ~MaterialPreviewRenderer() override;

    bool ensureScene();

    static MaterialPreviewRenderer* m_pSingleton;

    Ogre::SceneManager* m_sceneMgr = nullptr;
    Ogre::Camera* m_camera = nullptr;
    Ogre::Light* m_light = nullptr;
    Ogre::Entity* m_sphere = nullptr;
    Ogre::SceneNode* m_sphereNode = nullptr;
    Ogre::TexturePtr m_rttTexture;
    Ogre::RenderTarget* m_renderTarget = nullptr;

    bool m_initialized = false;

    // Cache: materialName -> base64 data URI
    QHash<QString, QString> m_cache;

    static constexpr int PREVIEW_SIZE = 64;
};

#endif // MATERIAL_PREVIEW_RENDERER_H
