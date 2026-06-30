#include "HDR/HDREnvironmentManager.h"

#include "HDR/HdrEquirectLoader.h"
#include "HDR/HdrPrecomputeWorker.h"
#include "SentryReporter.h"

#include <OgreTextureManager.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QMetaType>
#include <QThread>

namespace {

void removeTextureIfExists(const Ogre::TexturePtr& tex)
{
    if (!tex || !Ogre::TextureManager::getSingletonPtr())
        return;
    const Ogre::String name = tex->getName();
    if (Ogre::TextureManager::getSingleton().resourceExists(name))
        Ogre::TextureManager::getSingleton().remove(name);
}

} // namespace

HDREnvironmentManager* HDREnvironmentManager::s_singleton = nullptr;

HDREnvironmentManager* HDREnvironmentManager::getSingleton()
{
    if (!s_singleton)
        s_singleton = new HDREnvironmentManager(); // NOSONAR — singleton
    return s_singleton;
}

HDREnvironmentManager* HDREnvironmentManager::getSingletonPtr()
{
    return s_singleton;
}

void HDREnvironmentManager::kill()
{
    delete s_singleton; // NOSONAR — singleton
    s_singleton = nullptr;
}

HDREnvironmentManager::HDREnvironmentManager(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<HdrEquirect::CubemapFaces>("HdrEquirect::CubemapFaces");
    qRegisterMetaType<HdrIbl::IblBakeResult>("HdrIbl::IblBakeResult");
    initializeWorkerThread();
}

HDREnvironmentManager::~HDREnvironmentManager()
{
    shutdownWorkerThread();
    removeTextureIfExists(m_cubemap);
    removeTextureIfExists(m_irradiance);
    removeTextureIfExists(m_prefiltered);
    removeTextureIfExists(m_brdfLut);
    m_cubemap.reset();
    m_irradiance.reset();
    m_prefiltered.reset();
    m_brdfLut.reset();
}

void HDREnvironmentManager::initializeWorkerThread()
{
    m_workerThread = new QThread(this);
    m_worker = new HdrPrecomputeWorker();
    m_worker->moveToThread(m_workerThread);

    connect(m_worker,
            &HdrPrecomputeWorker::precomputeCompleted,
            this,
            &HDREnvironmentManager::onPrecomputeCompleted,
            Qt::QueuedConnection);
    connect(m_worker,
            &HdrPrecomputeWorker::precomputeError,
            this,
            &HDREnvironmentManager::onPrecomputeError,
            Qt::QueuedConnection);
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_workerThread->start();
}

void HDREnvironmentManager::shutdownWorkerThread()
{
    if (!m_workerThread)
        return;
    ++m_precomputeGeneration;
    m_workerThread->quit();
    if (!m_workerThread->wait(5000))
        m_workerThread->terminate();
    m_workerThread->wait(1000);
    m_workerThread = nullptr;
    m_worker = nullptr;
}

QString HDREnvironmentManager::resolvePath(const QString& pathOrBundledName) const
{
    QFileInfo info(pathOrBundledName);
    if (info.isAbsolute())
        return info.exists() ? info.absoluteFilePath() : QString{};

    if (info.exists())
        return info.absoluteFilePath();

    const QString fileName = info.fileName();
    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();
    candidates << appDir + QStringLiteral("/media/hdri/") + fileName
               << appDir + QStringLiteral("/../media/hdri/") + fileName;

#ifdef Q_OS_MACOS
    candidates << appDir + QStringLiteral("/../../media/hdri/") + fileName
               << appDir + QStringLiteral("/../../../media/hdri/") + fileName;
#endif

#ifdef QTMESH_UT_SOURCE_ROOT
    candidates << QDir(QString::fromUtf8(QTMESH_UT_SOURCE_ROOT))
                        .filePath(QStringLiteral("media/hdri/") + fileName);
#endif

    for (const QString& candidate : candidates) {
        const QFileInfo candidateInfo(candidate);
        if (!candidateInfo.exists() || !candidateInfo.isFile())
            continue;
        const QString canon = candidateInfo.canonicalFilePath();
        if (!canon.isEmpty())
            return canon;
    }
    return {};
}

bool HDREnvironmentManager::createOgreCubemap(const QString& cacheKey,
                                              HdrEquirect::CubemapFaces& faces,
                                              QString& error)
{
    if (!Ogre::Root::getSingletonPtr() || !Ogre::TextureManager::getSingletonPtr()) {
        error = QStringLiteral("Ogre is not initialised");
        return false;
    }

    const int faceSize = faces.faceSize;
    if (faceSize <= 0) {
        error = QStringLiteral("invalid cubemap face size");
        return false;
    }

    const Ogre::String texName =
        Ogre::String("HdrEnv_") + cacheKey.left(16).toStdString();

    auto& texMgr = Ogre::TextureManager::getSingleton();
    if (texMgr.resourceExists(texName))
        texMgr.remove(texName);

    Ogre::TexturePtr cubemap = texMgr.createManual(
        texName,
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_CUBE_MAP,
        static_cast<Ogre::uint32>(faceSize),
        static_cast<Ogre::uint32>(faceSize),
        0,
        Ogre::PF_FLOAT32_RGB,
        Ogre::TU_STATIC);

    for (size_t face = 0; face < 6; ++face) {
        auto& faceRgb = faces.faces[face];
        if (const size_t expected = static_cast<size_t>(faceSize)
                                    * static_cast<size_t>(faceSize) * 3u;
            faceRgb.size() < expected) {
            error = QStringLiteral("cubemap face %1 buffer too small").arg(static_cast<int>(face));
            texMgr.remove(texName);
            return false;
        }

        Ogre::PixelBox box(static_cast<Ogre::uint32>(faceSize),
                           static_cast<Ogre::uint32>(faceSize),
                           1,
                           Ogre::PF_FLOAT32_RGB,
                           faceRgb.data());
        cubemap->getBuffer(face)->blitFromMemory(box);
    }

    cubemap->load();
    m_cubemap = cubemap;
    return true;
}

bool HDREnvironmentManager::registerIblTextures(const QString& cacheKey,
                                                HdrIbl::IblBakeResult& result,
                                                QString& error)
{
    if (!Ogre::Root::getSingletonPtr() || !Ogre::TextureManager::getSingletonPtr()) {
        error = QStringLiteral("Ogre is not initialised");
        return false;
    }

    auto& texMgr = Ogre::TextureManager::getSingleton();
    const Ogre::String key = cacheKey.left(16).toStdString();

    removeTextureIfExists(m_irradiance);
    removeTextureIfExists(m_prefiltered);
    removeTextureIfExists(m_brdfLut);
    m_irradiance.reset();
    m_prefiltered.reset();
    m_brdfLut.reset();

    const Ogre::String irradianceName = Ogre::String("HdrIrradiance_") + key;
    if (texMgr.resourceExists(irradianceName))
        texMgr.remove(irradianceName);

    const int irrSize = result.irradiance.faceSize;
    m_irradiance = texMgr.createManual(
        irradianceName,
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_CUBE_MAP,
        static_cast<Ogre::uint32>(irrSize),
        static_cast<Ogre::uint32>(irrSize),
        0,
        Ogre::PF_FLOAT32_RGB,
        Ogre::TU_STATIC);
    for (size_t face = 0; face < 6; ++face) {
        auto& faceRgb = result.irradiance.faces[face];
        Ogre::PixelBox box(static_cast<Ogre::uint32>(irrSize),
                           static_cast<Ogre::uint32>(irrSize),
                           1,
                           Ogre::PF_FLOAT32_RGB,
                           faceRgb.data());
        m_irradiance->getBuffer(face)->blitFromMemory(box);
    }
    m_irradiance->load();

    const Ogre::String prefilterName = Ogre::String("HdrPrefilter_") + key;
    if (texMgr.resourceExists(prefilterName))
        texMgr.remove(prefilterName);

    const int baseSize = result.prefilter.mips.empty()
                             ? HdrIbl::kPrefilterBaseFaceSize
                             : result.prefilter.mips.front().faceSize;
    const int mipCount = static_cast<int>(result.prefilter.mips.size());
    m_prefiltered = texMgr.createManual(
        prefilterName,
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_CUBE_MAP,
        static_cast<Ogre::uint32>(baseSize),
        static_cast<Ogre::uint32>(baseSize),
        static_cast<Ogre::uint32>(std::max(0, mipCount - 1)),
        Ogre::PF_FLOAT32_RGB,
        Ogre::TU_STATIC);
    for (int mip = 0; mip < mipCount; ++mip) {
        const auto& level = result.prefilter.mips[static_cast<size_t>(mip)];
        const int faceSize = level.faceSize;
        for (size_t face = 0; face < 6; ++face) {
            auto& faceRgb = result.prefilter.mips[static_cast<size_t>(mip)].faces.faces[face];
            Ogre::PixelBox box(static_cast<Ogre::uint32>(faceSize),
                               static_cast<Ogre::uint32>(faceSize),
                               1,
                               Ogre::PF_FLOAT32_RGB,
                               faceRgb.data());
            m_prefiltered->getBuffer(face, static_cast<Ogre::uint32>(mip))->blitFromMemory(box);
        }
    }
    m_prefiltered->load();

    const Ogre::String brdfName = Ogre::String("HdrBrdfLut_") + key;
    if (texMgr.resourceExists(brdfName))
        texMgr.remove(brdfName);

    const int lutSize = result.brdfLut.size;
    m_brdfLut = texMgr.createManual(
        brdfName,
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_2D,
        static_cast<Ogre::uint32>(lutSize),
        static_cast<Ogre::uint32>(lutSize),
        0,
        Ogre::PF_FLOAT32_GR,
        Ogre::TU_STATIC);
    Ogre::PixelBox lutBox(static_cast<Ogre::uint32>(lutSize),
                          static_cast<Ogre::uint32>(lutSize),
                          1,
                          Ogre::PF_FLOAT32_GR,
                          result.brdfLut.rg.data());
    m_brdfLut->getBuffer()->blitFromMemory(lutBox);
    m_brdfLut->load();

    m_iblReady = true;
    return true;
}

void HDREnvironmentManager::startIblPrecompute(const QString& cacheKey,
                                               const HdrEquirect::CubemapFaces& envFaces)
{
    if (!m_worker)
        return;

    m_iblReady = false;
    removeTextureIfExists(m_irradiance);
    removeTextureIfExists(m_prefiltered);
    removeTextureIfExists(m_brdfLut);
    m_irradiance.reset();
    m_prefiltered.reset();
    m_brdfLut.reset();

    const quint64 generation = ++m_precomputeGeneration;
    QMetaObject::invokeMethod(
        m_worker,
        "precomputeIbl",
        Qt::QueuedConnection,
        Q_ARG(QString, cacheKey),
        Q_ARG(HdrEquirect::CubemapFaces, envFaces),
        Q_ARG(quint64, generation));
}

void HDREnvironmentManager::onPrecomputeCompleted(HdrIbl::IblBakeResult result,
                                                  bool fromDiskCache,
                                                  qint64 elapsedMs,
                                                  quint64 generation)
{
    if (generation != m_precomputeGeneration || m_cacheKey.isEmpty())
        return;

    QString error;
    if (!registerIblTextures(m_cacheKey, result, error))
        return;

    SentryReporter::addBreadcrumb(
        QStringLiteral("render.hdr.precompute"),
        QStringLiteral("cacheKey=%1 loadMs=%2 cached=%3")
            .arg(m_cacheKey.left(8))
            .arg(elapsedMs)
            .arg(fromDiskCache ? QStringLiteral("yes") : QStringLiteral("no")));

    emit iblPrecomputeCompleted(fromDiskCache);
}

void HDREnvironmentManager::onPrecomputeError(const QString& error, quint64 generation)
{
    if (generation != m_precomputeGeneration)
        return;
    Q_UNUSED(error);
    m_iblReady = false;
}

bool HDREnvironmentManager::loadEnvironment(const QString& pathOrBundledName)
{
    QElapsedTimer timer;
    timer.start();

    const QString resolved = resolvePath(pathOrBundledName);
    if (resolved.isEmpty())
        return false;

    const QString cacheKey = HdrEquirect::sha1HexOfFile(resolved);
    if (cacheKey.isEmpty())
        return false;

    const qint64 fileSize = QFileInfo(resolved).size();

    CachedBake* bake = nullptr;
    const bool fromBakeCache = m_bakeCache.contains(cacheKey);
    if (fromBakeCache) {
        bake = &m_bakeCache[cacheKey];
    } else {
        HdrEquirect::FloatImage equirect;
        if (QString loadError;
            !HdrEquirect::loadFromFile(resolved, equirect, loadError)) {
            return false;
        }

        const int faceSize = HdrEquirect::defaultFaceSizeForEquirect(equirect.width);
        CachedBake entry;
        entry.sourcePath = resolved;
        entry.cacheKey = cacheKey;
        entry.faceSize = faceSize;
        if (QString bakeError;
            !HdrEquirect::bakeEquirectToCubemap(equirect, faceSize, entry.faces, bakeError)) {
            return false;
        }
        entry.textureName = QStringLiteral("HdrEnv_%1").arg(cacheKey.left(16));
        m_bakeCache.emplace(cacheKey, std::move(entry));
        bake = &m_bakeCache[cacheKey];
    }

    QString ogreError;
    Ogre::TexturePtr previousCubemap = m_cubemap;
    m_cubemap.reset();
    if (!createOgreCubemap(cacheKey, bake->faces, ogreError)) {
        m_cubemap = previousCubemap;
        return false;
    }
    removeTextureIfExists(previousCubemap);

    m_currentPath = resolved;
    m_cacheKey = cacheKey;
    m_faceSize = bake->faceSize;

    startIblPrecompute(cacheKey, bake->faces);

    SentryReporter::addBreadcrumb(
        QStringLiteral("render.hdr.load"),
        QStringLiteral("path=%1 bytes=%2 faceSize=%3 loadMs=%4 cached=%5")
            .arg(QFileInfo(resolved).fileName())
            .arg(fileSize)
            .arg(m_faceSize)
            .arg(timer.elapsed())
            .arg(fromBakeCache ? QStringLiteral("yes") : QStringLiteral("no")));

    emit environmentChanged();
    return true;
}
