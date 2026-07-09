#include "SceneLightsCLI.h"

#include "CLIPipeline.h"
#include "LightManager.h"
#include "LightRigLibrary.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "SceneLightsIO.h"
#include "SentryReporter.h"

#include <OgreLight.h>

#include <QColor>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace
{

bool parseVec3(const QString& text, Ogre::Vector3& out)
{
    QString normalized = text;
    normalized.replace(QLatin1Char(';'), QLatin1Char(','));
    const QStringList parts = normalized.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.size() != 3)
        return false;
    bool okX = false, okY = false, okZ = false;
    out.x = parts[0].trimmed().toFloat(&okX);
    out.y = parts[1].trimmed().toFloat(&okY);
    out.z = parts[2].trimmed().toFloat(&okZ);
    return okX && okY && okZ;
}

bool parseColour(const QString& text, Ogre::ColourValue& out)
{
    QString value = text.trimmed();
    if (value.startsWith(QLatin1Char('#')))
    {
        QColor colour(value);
        if (!colour.isValid())
            return false;
        out = Ogre::ColourValue(colour.redF(), colour.greenF(), colour.blueF(), colour.alphaF());
        return true;
    }

    Ogre::Vector3 rgb;
    if (!parseVec3(value, rgb))
        return false;
    out = Ogre::ColourValue(rgb.x, rgb.y, rgb.z, 1.0f);
    return true;
}

bool parseLightType(const QString& text, Ogre::Light::LightTypes& out)
{
    const QString lower = text.trimmed().toLower();
    if (lower == QStringLiteral("directional"))
    {
        out = Ogre::Light::LT_DIRECTIONAL;
        return true;
    }
    if (lower == QStringLiteral("point"))
    {
        out = Ogre::Light::LT_POINT;
        return true;
    }
    if (lower == QStringLiteral("spot") || lower == QStringLiteral("spotlight"))
    {
        out = Ogre::Light::LT_SPOTLIGHT;
        return true;
    }
    return false;
}

bool isSceneLikePath(const QString& path)
{
    const QString lower = path.toLower();
    return lower.endsWith(QStringLiteral(".scene.gltf"))
           || lower.endsWith(QStringLiteral(".scene.glb"))
           || lower.endsWith(QStringLiteral(".gltf"))
           || lower.endsWith(QStringLiteral(".glb"));
}

bool loadInputFile(const QString& path, QString* error)
{
    if (error)
        error->clear();

    if (isSceneLikePath(path))
    {
        if (!MeshImporterExporter::sceneImporter(path))
        {
            if (error)
                *error = QStringLiteral("Failed to import scene file");
            return false;
        }
        return true;
    }

    MeshImporterExporter::importer({path});
    SceneLightsIO::importLightsFromFile(path, false);

    if (Manager::getSingleton()->getEntities().isEmpty()
        && LightManager::getSingleton()->lights().isEmpty())
    {
        if (error)
            *error = QStringLiteral("Failed to load file or no scene content found");
        return false;
    }
    return true;
}

int exportOutput(const QString& outputPath, QString* error)
{
    if (outputPath.isEmpty())
    {
        if (error)
            *error = QStringLiteral("Output path (-o) is required");
        return 2;
    }

    if (isSceneLikePath(outputPath))
    {
        if (MeshImporterExporter::sceneExporter(outputPath) != 0)
        {
            if (error)
                *error = QStringLiteral("Failed to export scene");
            return 1;
        }
        return 0;
    }

    // Mesh output: export the first entity node when present.
    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty())
    {
        if (error)
            *error = QStringLiteral("No mesh entity to export (use a .scene.gltf/.scene.glb output path)");
        return 1;
    }

    Ogre::Entity* entity = nullptr;
    for (auto* obj : entities)
    {
        if (obj && obj->getMovableType() == QStringLiteral("Entity"))
        {
            entity = static_cast<Ogre::Entity*>(obj);
            break;
        }
    }
    if (!entity)
    {
        if (error)
            *error = QStringLiteral("No entity found for mesh export");
        return 1;
    }

    const QString format = CLIPipeline::formatForExtension(outputPath);
    if (MeshImporterExporter::exporter(entity->getParentSceneNode(), outputPath, format) != 0)
    {
        if (error)
            *error = QStringLiteral("Failed to export mesh");
        return 1;
    }
    SceneLightsIO::writeLightsSidecar(outputPath);
    return 0;
}

void printHelp()
{
    CLIPipeline::writeOutput(
        QStringLiteral(
            "Usage:\n"
            "  qtmesh light <file> --list [--json]\n"
            "  qtmesh light --list-rigs [--json]\n"
            "  qtmesh light <file> --add directional|point|spot --pos x,y,z\n"
            "      [--dir x,y,z] [--colour #rrggbb] [--intensity N] -o <out>\n"
            "  qtmesh light <file> --remove <name> -o <out>\n"
            "  qtmesh light <file> --edit <name> [--intensity N] [--colour #rrggbb]\n"
            "      [--pos x,y,z] [--dir x,y,z] [--range N] [--enabled 0|1] -o <out>\n"
            "  qtmesh light <file> --apply-rig <rig_id> [--replace] -o <out>\n"
            "\n"
            "Intensity is QtMeshEditor powerScale (see CLAUDE.md Scene Lighting).\n"
            "Scene outputs (.scene.gltf / .scene.glb) embed metadata and write a\n"
            "*.lights.json sidecar for reliable round-trip.\n"));
}

int cmdListRigs(bool jsonOutput)
{
    const QStringList ids = LightRigLibrary::rigIds();
    if (jsonOutput)
    {
        QJsonArray rigs;
        for (const QString& id : ids)
        {
            QJsonObject obj;
            obj.insert(QStringLiteral("id"), id);
            obj.insert(QStringLiteral("name"), LightRigLibrary::displayNameForId(id));
            rigs.append(obj);
        }
        QJsonObject root;
        root.insert(QStringLiteral("rigs"), rigs);
        root.insert(QStringLiteral("count"), rigs.size());
        CLIPipeline::writeOutput(
            QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)));
    }
    else
    {
        QString out = QStringLiteral("Light rigs (%1):\n").arg(ids.size());
        for (const QString& id : ids)
            out += QStringLiteral("  %1 — %2\n").arg(id, LightRigLibrary::displayNameForId(id));
        CLIPipeline::writeOutput(out);
    }
    return 0;
}

} // namespace

namespace SceneLightsCLI
{

int run(int argc, char* argv[])
{
    bool jsonOutput = false;
    bool listOnly = false;
    bool listRigs = false;
    bool addLight = false;
    bool removeLight = false;
    bool editLight = false;
    bool applyRig = false;
    bool replaceRig = false;

    QString inputPath;
    QString outputPath;
    QString lightTypeText;
    QString removeName;
    QString editName;
    QString rigId;

    Ogre::Vector3 position = Ogre::Vector3::ZERO;
    Ogre::Vector3 direction = Ogre::Vector3(0, -1, 0);
    bool hasPosition = false;
    bool hasDirection = false;
    bool hasColour = false;
    bool hasIntensity = false;
    bool hasRange = false;
    bool hasEnabled = false;
    Ogre::ColourValue colour = Ogre::ColourValue::White;
    float intensity = 1.0f;
    float range = 10.0f;
    bool enabled = true;

    for (int i = 2; i < argc; ++i)
    {
        const QString arg = QString::fromUtf8(argv[i]);
        if (arg == QStringLiteral("--json"))
            jsonOutput = true;
        else if (arg == QStringLiteral("--list"))
            listOnly = true;
        else if (arg == QStringLiteral("--list-rigs"))
            listRigs = true;
        else if (arg == QStringLiteral("--add") && i + 1 < argc)
        {
            addLight = true;
            lightTypeText = QString::fromUtf8(argv[++i]);
        }
        else if (arg == QStringLiteral("--remove") && i + 1 < argc)
            removeName = QString::fromUtf8(argv[++i]);
        else if (arg == QStringLiteral("--edit") && i + 1 < argc)
            editName = QString::fromUtf8(argv[++i]);
        else if (arg == QStringLiteral("--apply-rig") && i + 1 < argc)
        {
            applyRig = true;
            rigId = QString::fromUtf8(argv[++i]);
        }
        else if (arg == QStringLiteral("--replace"))
            replaceRig = true;
        else if ((arg == QStringLiteral("-o") || arg == QStringLiteral("--output")) && i + 1 < argc)
            outputPath = QString::fromUtf8(argv[++i]);
        else if (arg == QStringLiteral("--pos") && i + 1 < argc)
        {
            if (!parseVec3(QString::fromUtf8(argv[++i]), position))
                return 2;
            hasPosition = true;
        }
        else if (arg == QStringLiteral("--dir") && i + 1 < argc)
        {
            if (!parseVec3(QString::fromUtf8(argv[++i]), direction))
                return 2;
            hasDirection = true;
        }
        else if ((arg == QStringLiteral("--colour") || arg == QStringLiteral("--color")) && i + 1 < argc)
        {
            if (!parseColour(QString::fromUtf8(argv[++i]), colour))
                return 2;
            hasColour = true;
        }
        else if (arg == QStringLiteral("--intensity") && i + 1 < argc)
        {
            intensity = QString::fromUtf8(argv[++i]).toFloat();
            hasIntensity = true;
        }
        else if (arg == QStringLiteral("--range") && i + 1 < argc)
        {
            range = QString::fromUtf8(argv[++i]).toFloat();
            hasRange = true;
        }
        else if (arg == QStringLiteral("--enabled") && i + 1 < argc)
        {
            const QString enabledText = QString::fromUtf8(argv[++i]).trimmed().toLower();
            enabled = enabledText == QStringLiteral("1") || enabledText == QStringLiteral("true")
                      || enabledText == QStringLiteral("yes");
            hasEnabled = true;
        }
        else if (arg == QStringLiteral("--help") || arg == QStringLiteral("-h"))
        {
            printHelp();
            return 0;
        }
        else if (!arg.startsWith(QLatin1Char('-')) && inputPath.isEmpty())
            inputPath = arg;
        else
            return 2;
    }

    SentryReporter::addBreadcrumb(QStringLiteral("cli.light"),
                                  QStringLiteral("qtmesh light"));

    if (listRigs)
        return cmdListRigs(jsonOutput);

    if (inputPath.isEmpty())
    {
        printHelp();
        return 2;
    }

    QFileInfo inputFi(inputPath);
    if (!inputFi.exists())
    {
        CLIPipeline::writeCliError(QStringLiteral("Error: File not found: %1\n").arg(inputPath));
        return 1;
    }

    if (listOnly)
    {
        QString lightError;
        const QJsonObject payload =
            SceneLightsIO::lightsInfoJsonFromFile(inputFi.absoluteFilePath(), &lightError);
        if (payload.isEmpty() && !lightError.isEmpty())
        {
            CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(lightError));
            return 1;
        }

        if (jsonOutput)
        {
            CLIPipeline::writeOutput(
                QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented)));
        }
        else
        {
            const QJsonArray lights = payload.value(QStringLiteral("lights")).toArray();
            CLIPipeline::writeOutput(
                QStringLiteral("Lights in %1 (%2):\n")
                    .arg(inputFi.fileName())
                    .arg(lights.size()));
            for (const QJsonValue& value : lights)
            {
                const QJsonObject obj = value.toObject();
                const QString rigGroup = obj.value(QStringLiteral("rigGroup")).toString();
                CLIPipeline::writeOutput(
                    QStringLiteral("  %1 [%2]%3\n")
                        .arg(obj.value(QStringLiteral("name")).toString(),
                             obj.value(QStringLiteral("type")).toString(),
                             rigGroup.isEmpty() ? QString() : QStringLiteral(" rig=") + rigGroup));
            }
            const QJsonArray ambient = payload.value(QStringLiteral("ambient")).toArray();
            if (ambient.size() >= 3)
            {
                CLIPipeline::writeOutput(
                    QStringLiteral("Ambient: %1, %2, %3\n")
                        .arg(ambient.at(0).toDouble())
                        .arg(ambient.at(1).toDouble())
                        .arg(ambient.at(2).toDouble()));
            }
        }
        return 0;
    }

    const bool mutates = addLight || !removeName.isEmpty() || !editName.isEmpty() || applyRig;
    if (!mutates)
    {
        printHelp();
        return 2;
    }

    if (!CLIPipeline::initOgreHeadless())
        return 1;

    LightManager::getSingleton()->tryConnectToManager();

    QString loadError;
    if (!loadInputFile(inputFi.absoluteFilePath(), &loadError))
    {
        CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(loadError));
        return 1;
    }

    auto* lights = LightManager::getSingleton();

    if (addLight)
    {
        Ogre::Light::LightTypes type = Ogre::Light::LT_POINT;
        if (!parseLightType(lightTypeText, type))
        {
            CLIPipeline::writeCliError(
                QStringLiteral("Error: Unknown light type '%1'\n").arg(lightTypeText));
            return 2;
        }
        if (!hasPosition)
            position = Ogre::Vector3(0, 2, 0);
        const bool setDirection =
            type == Ogre::Light::LT_DIRECTIONAL || type == Ogre::Light::LT_SPOTLIGHT;
        if (!hasDirection && setDirection)
            direction = Ogre::Vector3(0, -1, -1);

        LightHandle handle = lights->createLightAt(
            type, LightManager::defaultBaseNameForType(type), position, direction, setDirection);
        if (!handle.isValid())
        {
            CLIPipeline::writeCliError(QStringLiteral("Error: Failed to create light\n"));
            return 1;
        }
        if (hasColour)
            handle.light->setDiffuseColour(colour);
        if (hasIntensity)
            handle.light->setPowerScale(intensity);
        if (hasRange && type != Ogre::Light::LT_DIRECTIONAL)
            handle.light->setAttenuation(range, 1.0f, 0.0f, 0.0f);

        SentryReporter::addBreadcrumb(QStringLiteral("scene.light.create"),
                                      QStringLiteral("CLI add %1").arg(handle.name));
    }

    if (!removeName.isEmpty())
    {
        if (!lights->deleteLight(removeName))
        {
            CLIPipeline::writeCliError(
                QStringLiteral("Error: Light not found: %1\n").arg(removeName));
            return 1;
        }
        SentryReporter::addBreadcrumb(QStringLiteral("scene.light.delete"),
                                      QStringLiteral("CLI remove %1").arg(removeName));
    }

    if (!editName.isEmpty())
    {
        LightHandle* handle = lights->findLight(editName);
        if (!handle || !handle->isValid())
        {
            CLIPipeline::writeCliError(QStringLiteral("Error: Light not found: %1\n").arg(editName));
            return 1;
        }

        LightSnapshot snapshot = LightSnapshot::fromHandle(*handle);
        if (hasPosition)
            handle->sceneNode->setPosition(position);
        if (hasDirection)
            handle->sceneNode->setDirection(direction);
        if (hasColour)
            snapshot.diffuse = colour;
        if (hasIntensity)
            snapshot.powerScale = intensity;
        if (hasRange)
            snapshot.attenuationRange = range;
        if (hasEnabled)
            snapshot.enabled = enabled;

        if (!lights->applyProperties(editName, snapshot))
        {
            CLIPipeline::writeCliError(
                QStringLiteral("Error: Failed to edit light: %1\n").arg(editName));
            return 1;
        }
        SentryReporter::addBreadcrumb(QStringLiteral("scene.light.edit"),
                                      QStringLiteral("CLI edit %1").arg(editName));
    }

    if (applyRig)
    {
        const LightRigApplyResult result = LightRigLibrary::apply(rigId, replaceRig);
        if (!result.ok)
        {
            CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(result.error));
            return 1;
        }
        SentryReporter::addBreadcrumb(QStringLiteral("scene.light.apply_rig"),
                                      QStringLiteral("CLI rig %1").arg(rigId));
    }

    QString exportError;
    const int exportRc = exportOutput(outputPath, &exportError);
    if (exportRc != 0)
    {
        CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(exportError));
        return exportRc;
    }

    if (jsonOutput)
    {
        const QJsonObject payload = SceneLightsIO::documentToListJson(
            SceneLightsIO::captureFromScene(), QFileInfo(outputPath).fileName());
        CLIPipeline::writeOutput(
            QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented)));
    }
    else
    {
        CLIPipeline::writeOutput(QStringLiteral("Wrote %1\n").arg(outputPath));
    }

    return 0;
}

} // namespace SceneLightsCLI
