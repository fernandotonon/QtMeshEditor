#include "PsxGoldenCapture.h"

#include <QFileInfo>

namespace PsxGoldenCapture {

namespace {

QString existingFileFromEnv(const char *name)
{
    const QString path = qEnvironmentVariable(name);
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    if (!info.isFile())
        return {};
    return path;
}

} // namespace

QStringList allSceneIds()
{
    return {QString::fromLatin1(kSceneHomebrewStatic), QString::fromLatin1(kSceneRetailA),
            QString::fromLatin1(kSceneRetailB)};
}

bool isKnownSceneId(const QString &sceneId)
{
    return allSceneIds().contains(sceneId);
}

QString isoEnvVarForScene(const QString &sceneId)
{
    if (sceneId == QLatin1String(kSceneHomebrewStatic))
        return QString::fromLatin1(kEnvHomebrewIso);
    if (sceneId == QLatin1String(kSceneRetailA))
        return QString::fromLatin1(kEnvRetailAIso);
    if (sceneId == QLatin1String(kSceneRetailB))
        return QString::fromLatin1(kEnvRetailBIso);
    return {};
}

QString isoPathForScene(const QString &sceneId)
{
    if (sceneId == QLatin1String(kSceneHomebrewStatic)) {
        const QString path = existingFileFromEnv(kEnvHomebrewIso);
        if (!path.isEmpty())
            return path;
        return existingFileFromEnv(kEnvHomebrewIsoLegacy);
    }
    if (sceneId == QLatin1String(kSceneRetailA)) {
        const QString path = existingFileFromEnv(kEnvRetailAIso);
        if (!path.isEmpty())
            return path;
        return existingFileFromEnv(kEnvRetailAIsoLegacy);
    }
    if (sceneId == QLatin1String(kSceneRetailB))
        return existingFileFromEnv(kEnvRetailBIso);
    return {};
}

QString biosPath()
{
    return existingFileFromEnv(kEnvBios);
}

QString activeSceneId()
{
    const QString id = qEnvironmentVariable(kEnvSceneId).trimmed();
    if (id.isEmpty() || !isKnownSceneId(id))
        return {};
    return id;
}

QStringList configuredSceneIds()
{
    QStringList scenes;
    for (const QString &sceneId : allSceneIds()) {
        if (!isoPathForScene(sceneId).isEmpty())
            scenes.append(sceneId);
    }
    return scenes;
}

} // namespace PsxGoldenCapture
