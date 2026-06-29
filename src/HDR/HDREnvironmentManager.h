#pragma once

#include "HDR/HdrEquirectLoader.h"

#include <Ogre.h>

#include <QObject>
#include <QHash>
#include <QString>

/// Slice A (#467): owns the active HDR environment cubemap.
///
/// Loads `.hdr` / `.exr` equirectangular maps, bakes a float cubemap, and
/// registers it with Ogre::TextureManager. IBL precompute, skybox, and
/// RTSS wiring land in later slices — this singleton is the asset foundation.
struct HDREnvironmentManagerDeleter;

class HDREnvironmentManager : public QObject
{
    Q_OBJECT

public:
    static HDREnvironmentManager* getSingleton();
    static HDREnvironmentManager* getSingletonPtr();
    static void kill();

    /// Load an environment from an absolute path or a bundled name under
    /// `media/hdri/`. Returns false and leaves the previous environment
    /// unchanged on failure.
    bool loadEnvironment(const QString& pathOrBundledName);

    /// Path of the currently loaded environment (absolute file path or
    /// bundled name). Empty when nothing is loaded.
    QString currentEnvironment() const { return m_currentPath; }

    /// SHA-1 hex digest of the source file bytes for the active environment.
    QString currentCacheKey() const { return m_cacheKey; }

    /// Cube-map texture registered with Ogre (PF_FLOAT32_RGB). Null when
    /// no environment is loaded.
    Ogre::TexturePtr cubemap() const { return m_cubemap; }

    /// Cubemap face resolution of the active environment (0 when unloaded).
    int faceSize() const { return m_faceSize; }

signals:
    void environmentChanged();

private:
    friend struct HDREnvironmentManagerDeleter;

    explicit HDREnvironmentManager(QObject* parent = nullptr);
    ~HDREnvironmentManager() override;

    QString resolvePath(const QString& pathOrBundledName) const;
    bool createOgreCubemap(const QString& cacheKey,
                           HdrEquirect::CubemapFaces& faces,
                           QString& error);

    QString m_currentPath;
    QString m_cacheKey;
    int m_faceSize = 0;
    Ogre::TexturePtr m_cubemap;

    struct CachedBake {
        QString sourcePath;
        QString cacheKey;
        int faceSize = 0;
        HdrEquirect::CubemapFaces faces;
        QString textureName;
    };
    /// In-memory cache keyed by SHA-1 of source bytes (Slice A — disk cache in B).
    QHash<QString, CachedBake> m_bakeCache;
};
