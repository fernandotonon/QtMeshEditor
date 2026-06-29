#include "HDREnvironmentManager.h"

#include "HdrEquirectLoader.h"
#include "SentryReporter.h"

#include <OgreTextureManager.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>

HDREnvironmentManager* HDREnvironmentManager::s_singleton = nullptr;

HDREnvironmentManager* HDREnvironmentManager::getSingleton()
{
    if (!s_singleton)
        s_singleton = new HDREnvironmentManager();
    return s_singleton;
}

HDREnvironmentManager* HDREnvironmentManager::getSingletonPtr()
{
    return s_singleton;
}

void HDREnvironmentManager::kill()
{
    delete s_singleton;
    s_singleton = nullptr;
}

HDREnvironmentManager::HDREnvironmentManager(QObject* parent)
    : QObject(parent)
{
}

HDREnvironmentManager::~HDREnvironmentManager()
{
    if (m_cubemap && Ogre::TextureManager::getSingletonPtr()) {
        const Ogre::String texName = m_cubemap->getName();
        if (Ogre::TextureManager::getSingleton().resourceExists(texName))
            Ogre::TextureManager::getSingleton().remove(texName);
    }
    m_cubemap.reset();
}

QString HDREnvironmentManager::resolvePath(const QString& pathOrBundledName) const
{
    QFileInfo info(pathOrBundledName);
    if (info.isAbsolute() && info.exists())
        return info.absoluteFilePath();

    if (info.exists())
        return info.absoluteFilePath();

    const QString fileName = info.fileName();
    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();
    candidates << appDir + QStringLiteral("/media/hdri/") + fileName
               << appDir + QStringLiteral("/../media/hdri/") + fileName;

#if OGRE_PLATFORM == OGRE_PLATFORM_APPLE
    candidates << appDir + QStringLiteral("/../../media/hdri/") + fileName
               << appDir + QStringLiteral("/../../../media/hdri/") + fileName;
#endif

#ifdef QTMESH_UT_SOURCE_ROOT
    candidates << QDir(QString::fromUtf8(QTMESH_UT_SOURCE_ROOT))
                        .filePath(QStringLiteral("media/hdri/") + fileName);
#endif

    for (const QString& candidate : candidates) {
        const QString canon = QDir(candidate).canonicalPath();
        if (!canon.isEmpty() && QFileInfo::exists(canon))
            return canon;
    }
    return {};
}

bool HDREnvironmentManager::createOgreCubemap(const QString& cacheKey,
                                              const HdrEquirect::CubemapFaces& faces,
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

    m_cubemap = texMgr.createManual(
        texName,
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_CUBE_MAP,
        static_cast<Ogre::uint32>(faceSize),
        static_cast<Ogre::uint32>(faceSize),
        0,
        Ogre::PF_FLOAT32_RGB,
        Ogre::TU_STATIC);

    for (size_t face = 0; face < 6; ++face) {
        const auto& faceRgb = faces.faces[face];
        const size_t expected = static_cast<size_t>(faceSize)
                                * static_cast<size_t>(faceSize) * 3u;
        if (faceRgb.size() < expected) {
            error = QStringLiteral("cubemap face %1 buffer too small").arg(static_cast<int>(face));
            texMgr.remove(texName);
            m_cubemap.reset();
            return false;
        }

        Ogre::PixelBox box(static_cast<Ogre::uint32>(faceSize),
                           static_cast<Ogre::uint32>(faceSize),
                           1,
                           Ogre::PF_FLOAT32_RGB,
                           const_cast<float*>(faceRgb.data()));
        m_cubemap->getBuffer(face)->blitFromMemory(box);
    }

    m_cubemap->load();
    return true;
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
        QString loadError;
        if (!HdrEquirect::loadFromFile(resolved, equirect, loadError))
            return false;

        const int faceSize = HdrEquirect::defaultFaceSizeForEquirect(equirect.width);
        CachedBake entry;
        entry.sourcePath = resolved;
        entry.cacheKey = cacheKey;
        entry.faceSize = faceSize;
        QString bakeError;
        if (!HdrEquirect::bakeEquirectToCubemap(equirect, faceSize, entry.faces, bakeError))
            return false;
        entry.textureName = QStringLiteral("HdrEnv_%1").arg(cacheKey.left(16));
        m_bakeCache.insert(cacheKey, std::move(entry));
        bake = &m_bakeCache[cacheKey];
    }

    QString ogreError;
    if (!createOgreCubemap(cacheKey, bake->faces, ogreError))
        return false;

    m_currentPath = resolved;
    m_cacheKey = cacheKey;
    m_faceSize = bake->faceSize;

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
