#pragma once

#include "HDR/HdrEquirectLoader.h"
#include "HDR/HdrIblPrecompute.h"
#include "HDR/HdrTonemap.h"

#include <Ogre.h>

#include <QObject>
#include <QHash>
#include <QString>

class HdrPrecomputeWorker;
class QThread;

/// Slice A–D (#467–#470): HDR environment, IBL, tonemap defaults, skybox.
class HDREnvironmentManager : public QObject
{
    Q_OBJECT

public:
    using TonemapOperator = HdrTonemap::Operator;

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

    /// Highest prefiltered-specular mip level (0-based) for RTSS lod lookup.
    float prefilterMaxLodLevel() const { return m_prefilterMaxLodLevel; }

    /// When false, `loadEnvironment` skips the background IBL worker (used by
    /// fast integration tests — bake/cache logic is covered in HdrIbl/ HdrCache tests).
    void setBackgroundIblPrecomputeEnabled(bool enabled) { m_backgroundIblPrecompute = enabled; }

    /// Register precomputed IBL textures with Ogre (worker path + unit tests).
    bool installIblBake(const QString& cacheKey, HdrIbl::IblBakeResult& result, QString& error);

    /// Global tone-mapping defaults (Slice D). New viewports inherit these.
    TonemapOperator tonemapOperator() const { return m_tonemapOperator; }
    float exposureEv() const { return m_exposureEv; }
    float whitePoint() const { return m_whitePoint; }
    bool defaultSkyBoxVisible() const { return m_defaultSkyBoxVisible; }

    void setTonemapOperator(TonemapOperator op);
    void setExposureEv(float exposureEv);
    void setWhitePoint(float whitePoint);
    void setDefaultSkyBoxVisible(bool visible);

    /// Background blur for skybox display (0 = sharp env cubemap, 1 = max prefilter mip).
    float backgroundBlur() const { return m_backgroundBlur; }
    void setBackgroundBlur(float blur);

    /// Scan bundled HDRIs under `media/hdri/` (registered via resources.cfg).
    static QStringList listBundledEnvironments();

    /// Install or refresh the shared scene skybox from the active cubemap.
    void applySkyBox(Ogre::SceneManager* sceneMgr);
    void removeSkyBox(Ogre::SceneManager* sceneMgr);

    /// True when an environment cubemap is loaded (IBL may still be baking).
    bool hasEnvironment() const { return !m_cacheKey.isEmpty() && m_cubemap; }

signals:
    void environmentChanged();
    void iblPrecomputeCompleted(bool fromDiskCache);
    void tonemapChanged();
    void skyboxDefaultChanged();
    void backgroundBlurChanged();

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
    void updateSkyBoxMaterial();

    static HDREnvironmentManager* s_singleton;

    QString m_currentPath;
    QString m_cacheKey;
    int m_faceSize = 0;
    bool m_iblReady = false;
    float m_prefilterMaxLodLevel = 0.f;
    bool m_backgroundIblPrecompute = true;
    quint64 m_precomputeGeneration = 0;

    TonemapOperator m_tonemapOperator = TonemapOperator::ACES;
    float m_exposureEv = 0.f;
    float m_whitePoint = 1.f;
    bool m_defaultSkyBoxVisible = true;
    float m_backgroundBlur = 0.f;
    bool m_skyBoxInstalled = false;

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
