#pragma once

#include "HDR/HdrEquirectLoader.h"
#include "HDR/HdrIblPrecompute.h"

#include <Ogre.h>

#include <QObject>
#include <QHash>
#include <QString>

class HdrPrecomputeWorker;
class QThread;

/// Slice A (#467) + B (#468): HDR environment + IBL precompute.
class HDREnvironmentManager : public QObject
{
    Q_OBJECT

public:
    static HDREnvironmentManager* getSingleton();
    static HDREnvironmentManager* getSingletonPtr();
    static void kill();

    /// Load an environment from an absolute path or a bundled name under
    /// `media/hdri/`. Returns false and leaves the previous environment
    /// unchanged on failure. IBL precompute continues asynchronously.
    bool loadEnvironment(const QString& pathOrBundledName);

    /// Path of the currently loaded environment (absolute file path or
    /// bundled name). Empty when nothing is loaded.
    QString currentEnvironment() const { return m_currentPath; }

    /// SHA-1 hex digest of the source file bytes for the active environment.
    QString currentCacheKey() const { return m_cacheKey; }

    /// Cube-map texture registered with Ogre (PF_FLOAT32_RGB). Null when
    /// no environment is loaded.
    Ogre::TexturePtr cubemap() const { return m_cubemap; }

    /// Irradiance / prefiltered specular / BRDF LUT textures (Slice B).
    Ogre::TexturePtr irradianceMap() const { return m_irradiance; }
    Ogre::TexturePtr prefilteredSpecularMap() const { return m_prefiltered; }
    Ogre::TexturePtr brdfLut() const { return m_brdfLut; }

    /// Cubemap face resolution of the active environment (0 when unloaded).
    int faceSize() const { return m_faceSize; }

    /// True once IBL textures for the active environment are registered.
    bool isIblReady() const { return m_iblReady; }

    /// When false, `loadEnvironment` skips the background IBL worker (used by
    /// fast integration tests — bake/cache logic is covered in HdrIbl/ HdrCache tests).
    void setBackgroundIblPrecomputeEnabled(bool enabled) { m_backgroundIblPrecompute = enabled; }

signals:
    void environmentChanged();
    void iblPrecomputeCompleted(bool fromDiskCache);

private slots:
    void onPrecomputeCompleted(HdrIbl::IblBakeResult result,
                               bool fromDiskCache,
                               qint64 elapsedMs,
                               quint64 generation);
    void onPrecomputeError(const QString& error, quint64 generation);

private:
    explicit HDREnvironmentManager(QObject* parent = nullptr);
    ~HDREnvironmentManager() override;

    void initializeWorkerThread();
    void shutdownWorkerThread();
    void startIblPrecompute(const QString& cacheKey, const HdrEquirect::CubemapFaces& envFaces);

    QString resolvePath(const QString& pathOrBundledName) const;
    bool createOgreCubemap(const QString& cacheKey,
                           HdrEquirect::CubemapFaces& faces,
                           QString& error);
    bool registerIblTextures(const QString& cacheKey,
                             HdrIbl::IblBakeResult& result,
                             QString& error);

    static HDREnvironmentManager* s_singleton;

    QString m_currentPath;
    QString m_cacheKey;
    int m_faceSize = 0;
    bool m_iblReady = false;
    bool m_backgroundIblPrecompute = true;
    quint64 m_precomputeGeneration = 0;

    Ogre::TexturePtr m_cubemap;
    Ogre::TexturePtr m_irradiance;
    Ogre::TexturePtr m_prefiltered;
    Ogre::TexturePtr m_brdfLut;

    QThread* m_workerThread = nullptr;
    HdrPrecomputeWorker* m_worker = nullptr;

    struct CachedBake {
        QString sourcePath;
        QString cacheKey;
        int faceSize = 0;
        HdrEquirect::CubemapFaces faces;
        QString textureName;
    };
    QHash<QString, CachedBake> m_bakeCache;
};
