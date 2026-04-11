#include "AssetBrowserController.h"
#include "MaterialPreviewRenderer.h"
#include "SentryReporter.h"

#include <Ogre.h>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

AssetBrowserController* AssetBrowserController::m_pSingleton = nullptr;

const QStringList AssetBrowserController::s_meshExtensions = {
    "fbx", "gltf", "glb", "gltf2", "obj", "dae", "stl", "mesh", "3ds", "blend", "ply",
    "x", "x3d", "lwo", "lws", "ac", "ms3d", "cob", "scn", "bvh", "irrmesh", "irr",
    "mdl", "md2", "md3", "md5mesh", "smd", "ogex", "b3d", "q3d", "nff", "off",
    "raw", "ter", "hmp", "assbin", "mesh.xml"
};

const QStringList AssetBrowserController::s_textureExtensions = {
    "png", "jpg", "jpeg", "tga", "bmp", "dds", "hdr", "exr", "tif", "tiff"
};

const QStringList AssetBrowserController::s_materialExtensions = {
    "material"
};

AssetBrowserController::AssetBrowserController()
    : QObject(nullptr)
    , m_watcher(new QFileSystemWatcher(this))
{
    // Restore last browsed directory from QSettings
    QSettings settings;
    QString savedPath = settings.value("AssetBrowser/rootPath").toString();
    if (!savedPath.isEmpty() && QDir(savedPath).exists()) {
        m_rootPath = savedPath;
    } else {
        m_rootPath = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }

    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString&) {
        refreshFiles();
    });

    setupWatcher();
    refreshFiles();
}

AssetBrowserController::~AssetBrowserController()
{
    // m_watcher is a child QObject, cleaned up automatically
}

AssetBrowserController* AssetBrowserController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new AssetBrowserController();
    return m_pSingleton;
}

AssetBrowserController* AssetBrowserController::qmlInstance(QQmlEngine* engine, QJSEngine* /*scriptEngine*/)
{
    auto* inst = instance();
    engine->setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void AssetBrowserController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

QString AssetBrowserController::rootPath() const
{
    return m_rootPath;
}

void AssetBrowserController::setRootPath(const QString& path)
{
    if (m_rootPath == path)
        return;

    QDir dir(path);
    if (!dir.exists())
        return;

    m_rootPath = dir.absolutePath();

    SentryReporter::addBreadcrumb("asset_browser", "Root directory changed: " + m_rootPath);

    // Persist to settings
    QSettings settings;
    settings.setValue("AssetBrowser/rootPath", m_rootPath);

    emit rootPathChanged();
    setupWatcher();
    refreshFiles();
}

QVariantList AssetBrowserController::files() const
{
    return m_files;
}

QString AssetBrowserController::filter() const
{
    return m_filter;
}

void AssetBrowserController::setFilter(const QString& filter)
{
    if (m_filter == filter)
        return;
    m_filter = filter;
    emit filterChanged();
    refreshFiles();
}

QString AssetBrowserController::searchQuery() const
{
    return m_searchQuery;
}

void AssetBrowserController::setSearchQuery(const QString& query)
{
    if (m_searchQuery == query)
        return;
    m_searchQuery = query;
    emit searchQueryChanged();
    refreshFiles();
}

void AssetBrowserController::browseForDirectory()
{
    SentryReporter::addBreadcrumb("asset_browser", "Browse for directory requested");
    emit browseRequested();
}

void AssetBrowserController::openFile(const QString& path)
{
    QFileInfo fi(path);
    if (!fi.exists())
        return;

    if (fi.isDir()) {
        SentryReporter::addBreadcrumb("asset_browser",
            QString("Navigate into directory: %1").arg(fi.fileName()));
        navigateToDirectory(path);
        return;
    }

    QString type = classifyExtension(fi.suffix().toLower());

    SentryReporter::addBreadcrumb("asset_browser",
        QString("Open file: %1 (type: %2)").arg(fi.fileName(), type));

    if (type == "mesh") {
        emit importMeshRequested(QStringList{path});
    }
    // Texture and material handling can be added as stretch goals
}

void AssetBrowserController::navigateToDirectory(const QString& path)
{
    setRootPath(path);
}

void AssetBrowserController::navigateUp()
{
    QDir dir(m_rootPath);
    if (dir.cdUp()) {
        setRootPath(dir.absolutePath());
    }
}

QString AssetBrowserController::fileTypeForPath(const QString& path) const
{
    QFileInfo fi(path);
    if (fi.isDir())
        return "directory";
    return classifyExtension(fi.suffix().toLower());
}

void AssetBrowserController::refreshFiles()
{
    m_files.clear();

    QDir dir(m_rootPath);
    if (!dir.exists()) {
        emit filesChanged();
        return;
    }

    // List directories first, then files
    QFileInfoList entries = dir.entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);

    for (const QFileInfo& fi : entries) {
        QString type;
        if (fi.isDir()) {
            type = "directory";
        } else {
            type = classifyExtension(fi.suffix().toLower());
        }

        // Apply filter
        if (m_filter != "all") {
            if (fi.isDir()) {
                // Always show directories regardless of filter
            } else if (m_filter == "meshes" && type != "mesh") {
                continue;
            } else if (m_filter == "textures" && type != "texture") {
                continue;
            } else if (m_filter == "materials" && type != "material") {
                continue;
            }
        }

        // Apply search query
        if (!m_searchQuery.isEmpty()) {
            if (!fi.fileName().contains(m_searchQuery, Qt::CaseInsensitive))
                continue;
        }

        QVariantMap entry;
        entry["name"] = fi.fileName();
        entry["path"] = fi.absoluteFilePath();
        entry["type"] = type;
        entry["size"] = fi.isDir() ? 0 : fi.size();
        entry["isDir"] = fi.isDir();

        // Generate a material preview for .material files
        if (type == "material") {
            QString previewUrl = materialPreview(fi.absoluteFilePath());
            if (!previewUrl.isEmpty())
                entry["previewUrl"] = previewUrl;
        }

        m_files.append(entry);
    }

    emit filesChanged();
}

void AssetBrowserController::setupWatcher()
{
    // Remove old watched directories
    QStringList watched = m_watcher->directories();
    if (!watched.isEmpty())
        m_watcher->removePaths(watched);

    // Watch the current root path
    if (!m_rootPath.isEmpty() && QDir(m_rootPath).exists())
        m_watcher->addPath(m_rootPath);
}

QString AssetBrowserController::materialPreview(const QString& filePath) const
{
    // Parse the .material file for the first material name
    QString matName = MaterialPreviewRenderer::firstMaterialNameInFile(filePath);
    if (matName.isEmpty())
        return {};

    // Try to load the material if it doesn't already exist in Ogre
    auto* matMgr = Ogre::MaterialManager::getSingletonPtr();
    if (!matMgr)
        return {};

    if (!matMgr->resourceExists(matName.toStdString())) {
        // Ensure the directory containing the .material file is an Ogre resource location
        QFileInfo fi(filePath);
        try {
            Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
                fi.absolutePath().toStdString(), "FileSystem",
                Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false);
            // Parse all .material scripts in the resource group
            Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();
        } catch (...) {
            // Ignore errors from duplicate resource locations or parse failures
        }
    }

    return MaterialPreviewRenderer::instance()->renderPreviewAsDataUri(matName);
}

QString AssetBrowserController::classifyExtension(const QString& ext) const
{
    if (s_meshExtensions.contains(ext, Qt::CaseInsensitive))
        return "mesh";
    if (s_textureExtensions.contains(ext, Qt::CaseInsensitive))
        return "texture";
    if (s_materialExtensions.contains(ext, Qt::CaseInsensitive))
        return "material";
    return "other";
}
