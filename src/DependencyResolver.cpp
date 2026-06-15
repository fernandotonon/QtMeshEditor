#include "DependencyResolver.h"

#include "Manager.h"
#include "PS1/PS1RSD.h"

#include <OgreEntity.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreSubEntity.h>
#include <OgreTechnique.h>
#include <OgreTextureUnitState.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

namespace {

QString canonicalPath(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists() && !info.isAbsolute())
        return QFileInfo(path).absoluteFilePath();
    return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
}

QString roleForExtension(const QString& fileName)
{
    const QString ext = QFileInfo(fileName).suffix().toLower();
    if (ext == QLatin1String("mesh") || ext == QLatin1String("obj") || ext == QLatin1String("fbx")
        || ext == QLatin1String("gltf") || ext == QLatin1String("glb") || ext == QLatin1String("dae")
        || ext == QLatin1String("collada") || ext == QLatin1String("stl") || ext == QLatin1String("tmd"))
        return QStringLiteral("main");
    if (ext == QLatin1String("skeleton") || ext == QLatin1String("skel"))
        return QStringLiteral("skeleton");
    if (ext == QLatin1String("anim") || ext == QLatin1String("animation"))
        return QStringLiteral("animation");
    if (ext == QLatin1String("material") || ext == QLatin1String("mat") || ext == QLatin1String("mtl"))
        return QStringLiteral("material");
    if (ext == QLatin1String("png") || ext == QLatin1String("jpg") || ext == QLatin1String("jpeg")
        || ext == QLatin1String("tga") || ext == QLatin1String("bmp") || ext == QLatin1String("dds")
        || ext == QLatin1String("tif") || ext == QLatin1String("tiff") || ext == QLatin1String("tim"))
        return QStringLiteral("texture");
    if (ext == QLatin1String("json") || ext == QLatin1String("yml") || ext == QLatin1String("yaml")
        || ext == QLatin1String("xml"))
        return QStringLiteral("metadata");
    if (ext == QLatin1String("rsd"))
        return QStringLiteral("main");
    if (ext == QLatin1String("ply"))
        return QStringLiteral("ps1-ply");
    return QStringLiteral("other");
}

void appendEntry(QVector<DependencyEntry>& out,
                 QSet<QString>& seen,
                 const QString& absolutePath,
                 const QString& role,
                 const QString& referencedBy)
{
    if (absolutePath.trimmed().isEmpty())
        return;

    const QString key = canonicalPath(absolutePath);
    if (seen.contains(key))
        return;
    seen.insert(key);

    DependencyEntry entry;
    entry.absolutePath = key;
    entry.role = role;
    entry.referencedBy = referencedBy;
    entry.exists = QFileInfo::exists(key);
    entry.checkedByDefault = entry.exists;
    out.append(entry);
}

QString resolveRelativePath(const QString& baseDir, const QString& reference)
{
    QString ref = reference.trimmed();
    if (ref.isEmpty())
        return {};

    if (ref.startsWith(QLatin1String("file://"), Qt::CaseInsensitive))
        ref = QUrl(ref).toLocalFile();

    const QFileInfo refInfo(ref);
    if (refInfo.isAbsolute())
        return refInfo.absoluteFilePath();

    const QString joined = QDir(baseDir).filePath(ref);
    return QFileInfo(joined).absoluteFilePath();
}

void collectMtlDependencies(const QString& mtlPath,
                            QVector<DependencyEntry>& out,
                            QSet<QString>& seen)
{
    QFile file(mtlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    const QString baseDir = QFileInfo(mtlPath).absolutePath();
    static const QRegularExpression mapLine(
        QStringLiteral("^(map_[A-Za-z0-9_]+|bump|disp|decal)\\s+(.+)$"));
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        const auto match = mapLine.match(line);
        if (!match.hasMatch())
            continue;
        const QString textureRef = match.captured(2).trimmed();
        const QString resolved = resolveRelativePath(baseDir, textureRef);
        appendEntry(out, seen, resolved, QStringLiteral("texture"), QFileInfo(mtlPath).fileName());
    }
}

void collectGltfDependencies(const QString& gltfPath,
                             QVector<DependencyEntry>& out,
                             QSet<QString>& seen)
{
    QFile file(gltfPath);
    if (!file.open(QIODevice::ReadOnly))
        return;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;

    const QString baseDir = QFileInfo(gltfPath).absolutePath();
    const QJsonObject root = doc.object();

    const QJsonArray buffers = root.value(QStringLiteral("buffers")).toArray();
    for (const QJsonValue& value : buffers) {
        const QString uri = value.toObject().value(QStringLiteral("uri")).toString();
        if (uri.startsWith(QStringLiteral("data:"), Qt::CaseInsensitive))
            continue;
        appendEntry(out, seen, resolveRelativePath(baseDir, uri), QStringLiteral("other"),
                    QFileInfo(gltfPath).fileName());
    }

    const QJsonArray images = root.value(QStringLiteral("images")).toArray();
    for (const QJsonValue& value : images) {
        const QString uri = value.toObject().value(QStringLiteral("uri")).toString();
        if (uri.startsWith(QStringLiteral("data:"), Qt::CaseInsensitive))
            continue;
        appendEntry(out, seen, resolveRelativePath(baseDir, uri), QStringLiteral("texture"),
                    QFileInfo(gltfPath).fileName());
    }
}

void collectColladaDependencies(const QString& daePath,
                                QVector<DependencyEntry>& out,
                                QSet<QString>& seen)
{
    QFile file(daePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    const QString baseDir = QFileInfo(daePath).absolutePath();
    static const QRegularExpression imageInit(
        QStringLiteral("<init_from>([^<]+)</init_from>"),
        QRegularExpression::CaseInsensitiveOption);
    const QString content = QString::fromUtf8(file.readAll());
    auto it = imageInit.globalMatch(content);
    while (it.hasNext()) {
        const auto match = it.next();
        appendEntry(out, seen, resolveRelativePath(baseDir, match.captured(1).trimmed()),
                    QStringLiteral("texture"), QFileInfo(daePath).fileName());
    }
}

void collectMeshSidecars(const QString& meshPath,
                         QVector<DependencyEntry>& out,
                         QSet<QString>& seen)
{
    const QFileInfo meshInfo(meshPath);
    const QString base = meshInfo.absolutePath() + QLatin1Char('/') + meshInfo.completeBaseName();
    const QStringList sidecars = {
        base + QStringLiteral(".material"),
        base + QStringLiteral(".skeleton"),
        base + QStringLiteral(".skel"),
    };
    for (const QString& sidecar : sidecars) {
        if (QFileInfo::exists(sidecar))
            appendEntry(out, seen, sidecar, roleForExtension(sidecar), meshInfo.fileName());
    }
}

void collectRsdDependencies(const QString& rsdPath,
                            QVector<DependencyEntry>& out,
                            QSet<QString>& seen)
{
    PS1RSD::RsdDescriptor descriptor;
    QString err;
    if (!PS1RSD::parseRsdFile(rsdPath, descriptor, &err))
        return;

    const QString baseDir = QFileInfo(rsdPath).absolutePath();
    const auto addRef = [&](const QString& ref, const QString& role) {
        if (ref.isEmpty())
            return;
        appendEntry(out, seen, resolveRelativePath(baseDir, ref), role, QFileInfo(rsdPath).fileName());
    };

    addRef(descriptor.plyPath, QStringLiteral("ps1-ply"));
    addRef(descriptor.matPath, QStringLiteral("ps1-mat"));
    addRef(descriptor.grpPath, QStringLiteral("metadata"));
    for (const QString& texture : descriptor.textures)
        addRef(texture, QStringLiteral("ps1-tim"));
}

QString locateTextureOnDisk(const Ogre::String& textureName, const Ogre::String& groupName)
{
    if (textureName.empty() || !Ogre::Root::getSingletonPtr())
        return {};

    const Ogre::StringVectorPtr locationsPtr =
        Ogre::ResourceGroupManager::getSingleton().findResourceLocation(groupName, textureName);
    if (!locationsPtr)
        return {};
    for (const Ogre::String& location : *locationsPtr) {
        const QString path = QString::fromStdString(location);
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

} // namespace

QVector<DependencyEntry> DependencyResolver::detectFromFiles(const QString& mainAssetPath)
{
    QVector<DependencyEntry> out;
    QSet<QString> seen;

    const QFileInfo mainInfo(mainAssetPath);
    if (!mainInfo.exists())
        return out;

    appendEntry(out, seen, mainInfo.absoluteFilePath(), QStringLiteral("main"), QStringLiteral("main"));

    const QString ext = mainInfo.suffix().toLower();
    if (ext == QLatin1String("obj")) {
        bool foundMtl = false;
        QFile objFile(mainInfo.absoluteFilePath());
        if (objFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!objFile.atEnd()) {
                const QString line = QString::fromUtf8(objFile.readLine()).trimmed();
                if (!line.startsWith(QLatin1String("mtllib "), Qt::CaseInsensitive))
                    continue;
                const QString mtlName = line.mid(7).trimmed();
                if (mtlName.isEmpty())
                    continue;
                const QString mtlPath = QDir(mainInfo.absolutePath()).filePath(mtlName);
                if (!QFileInfo::exists(mtlPath))
                    continue;
                foundMtl = true;
                appendEntry(out, seen, mtlPath, QStringLiteral("material"), mainInfo.fileName());
                collectMtlDependencies(mtlPath, out, seen);
            }
        }
        if (!foundMtl) {
            const QString mtlPath = mainInfo.absolutePath() + QLatin1Char('/')
                + mainInfo.completeBaseName() + QStringLiteral(".mtl");
            if (QFileInfo::exists(mtlPath)) {
                appendEntry(out, seen, mtlPath, QStringLiteral("material"), mainInfo.fileName());
                collectMtlDependencies(mtlPath, out, seen);
            }
        }
    } else if (ext == QLatin1String("gltf")) {
        collectGltfDependencies(mainInfo.absoluteFilePath(), out, seen);
    } else if (ext == QLatin1String("dae") || ext == QLatin1String("collada")) {
        collectColladaDependencies(mainInfo.absoluteFilePath(), out, seen);
    } else if (ext == QLatin1String("mesh")) {
        collectMeshSidecars(mainInfo.absoluteFilePath(), out, seen);
    } else if (ext == QLatin1String("rsd")) {
        collectRsdDependencies(mainInfo.absoluteFilePath(), out, seen);
    }

    return out;
}

QVector<DependencyEntry> DependencyResolver::detectFromLoadedScene(const QString& referencedBy)
{
    QVector<DependencyEntry> out;
    QSet<QString> seen;

    Manager* manager = Manager::getSingletonPtr();
    // Scene texture probing needs a live editor session. Unit tests may leave a
    // headless Manager singleton around without a MainWindow or valid Ogre scene.
    if (!manager || !manager->getMainWindow() || !manager->getRoot() || !manager->getSceneMgr())
        return out;

    try {
    for (Ogre::Entity* entity : manager->getEntities()) {
        if (!entity || entity->getMovableType() != "Entity")
            continue;

        for (unsigned int sub = 0; sub < entity->getNumSubEntities(); ++sub) {
            Ogre::SubEntity* subEntity = entity->getSubEntity(sub);
            if (!subEntity)
                continue;

            Ogre::MaterialPtr material = subEntity->getMaterial();
            if (!material)
                continue;

            for (Ogre::Technique* technique : material->getTechniques()) {
                if (!technique)
                    continue;
                for (unsigned short passIndex = 0; passIndex < technique->getNumPasses(); ++passIndex) {
                    Ogre::Pass* pass = technique->getPass(passIndex);
                    if (!pass)
                        continue;
                    for (unsigned short tusIndex = 0; tusIndex < pass->getNumTextureUnitStates(); ++tusIndex) {
                        Ogre::TextureUnitState* tus = pass->getTextureUnitState(tusIndex);
                        if (!tus)
                            continue;
                        const QString texturePath = locateTextureOnDisk(
                            tus->getTextureName(), material->getGroup());
                        if (!texturePath.isEmpty())
                            appendEntry(out, seen, texturePath, QStringLiteral("texture"), referencedBy);
                    }
                }
            }
        }
    }
    } catch (const Ogre::Exception&) {
        return out;
    }

    return out;
}

QVector<DependencyEntry> DependencyResolver::detect(const QString& mainAssetPath)
{
    QVector<DependencyEntry> out = detectFromFiles(mainAssetPath);
    QSet<QString> seen;
    for (const DependencyEntry& entry : out)
        seen.insert(entry.absolutePath);

    const QVector<DependencyEntry> sceneDeps =
        detectFromLoadedScene(QFileInfo(mainAssetPath).fileName());
    for (const DependencyEntry& entry : sceneDeps) {
        if (!seen.contains(entry.absolutePath)) {
            seen.insert(entry.absolutePath);
            out.append(entry);
        }
    }
    return out;
}
