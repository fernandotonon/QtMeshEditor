#pragma once

#include "LightManager.h"

#include <OgreColourValue.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

struct aiScene;

namespace SceneLightsIO
{

/// Metadata key written to aiScene::mMetaData for bit-exact QtMeshEditor round-trip.
inline constexpr const char* kSceneLightsMetadataKey = "qtmesh.scene.lights";
inline constexpr int kDocumentVersion = 1;

/// User binding on rig-group scene nodes — stores the preset rig id when known.
inline constexpr const char* kRigIdUserKey = "light_rig_id";

struct RigGroupExport
{
    QString name;
    QString rigId;
    bool preserveGrouping = true;
    QList<LightSnapshot> lights;
};

struct SceneLightsDocument
{
    int version = kDocumentVersion;
    Ogre::ColourValue ambient = Ogre::ColourValue(0.3f, 0.3f, 0.3f);
    QList<RigGroupExport> rigGroups;
    QList<LightSnapshot> standaloneLights;
};

/// Capture every user light + rig grouping from the live Ogre scene.
SceneLightsDocument captureFromScene();

/// Recreate lights via LightManager (emits lightCreated). When the document is
/// empty and @p useDefaultWhenEmpty is true, applies the default scene rig.
bool applyToLightManager(const SceneLightsDocument& doc, bool useDefaultWhenEmpty = true);

QByteArray documentToJson(const SceneLightsDocument& doc);
bool documentFromJson(const QByteArray& json, SceneLightsDocument& out);

/// Write qtmesh metadata + best-effort aiLight/aiNode entries for glTF/FBX export.
void appendLightsToAiScene(aiScene* scene, const SceneLightsDocument& doc);

/// Prefer qtmesh metadata; fall back to Assimp punctual lights when absent.
bool readDocumentFromAiScene(const aiScene* scene, SceneLightsDocument& out);

/// Import path used by sceneImporter and Assimp mesh import.
bool importFromAssimpScene(const aiScene* scene, bool useDefaultWhenEmpty = true);

/// Rec.601 luminance used for glTF intensity mapping.
float ogreLuminance(const Ogre::ColourValue& colour);

/// Map Ogre powerScale × diffuse colour to KHR_lights_punctual intensity.
/// Directional → lux approximation; point/spot → candela approximation.
/// Documented in SceneLightsIO.cpp — not bit-exact; use qtmesh metadata for that.
float powerScaleToGltfIntensity(const LightSnapshot& snapshot);

/// Build the standard lights list JSON object from an in-memory document.
QJsonObject documentToListJson(const SceneLightsDocument& doc,
                               const QString& sourceLabel = QString());

/// Headless CLI / info: read lights from a file on disk (Assimp only).
QJsonObject lightsInfoJsonFromFile(const QString& path, QString* error = nullptr);

/// FBX round-trip: write/read a `.lights.json` sidecar next to the mesh file.
/// Assimp FBX lights are best-effort; the sidecar preserves bit-exact QtMeshEditor state.
bool writeLightsSidecar(const QString& meshPath);
bool importLightsSidecar(const QString& meshPath, bool useDefaultWhenEmpty = true);
bool importLightsFromFile(const QString& path, bool useDefaultWhenEmpty = true);

} // namespace SceneLightsIO
