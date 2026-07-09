#include "SceneLightsIO.h"

#include "LightManager.h"
#include "LightRigLibrary.h"
#include "Manager.h"
#include "ShadowController.h"
#include "SentryReporter.h"

#include <assimp/light.h>
#include <assimp/metadata.h>
#include <assimp/scene.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>

#include <OgreMath.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QDir>

#include <cmath>
#include <algorithm>
#include <map>
#include <vector>

namespace
{

// Assimp aiString stores AI_MAXLEN bytes including the terminator.
constexpr int kAiStringMaxPayload = 1023;
constexpr QLatin1String kSceneLightsChunkCountKey("qtmesh.scene.lights.chunks");

bool readLightsJsonFromMetadata(const aiMetadata* meta, QByteArray& jsonOut)
{
    jsonOut.clear();
    if (!meta)
        return false;

    aiString encoded;
    if (meta->Get(SceneLightsIO::kSceneLightsMetadataKey, encoded))
    {
        jsonOut = QByteArray(encoded.C_Str());
        if (!jsonOut.isEmpty())
            return true;
    }

    int chunkCount = 0;
    if (!meta->Get(kSceneLightsChunkCountKey.data(), chunkCount) || chunkCount <= 0)
        return false;

    for (int i = 0; i < chunkCount; ++i)
    {
        const QString key = QStringLiteral("qtmesh.scene.lights.%1").arg(i);
        aiString chunk;
        if (!meta->Get(key.toUtf8().constData(), chunk))
            return false;
        jsonOut.append(chunk.C_Str());
    }
    return !jsonOut.isEmpty();
}

void writeLightsJsonToMetadata(aiMetadata* meta, const QByteArray& json)
{
    if (!meta || json.isEmpty())
        return;

    if (json.size() <= kAiStringMaxPayload)
    {
        meta->Add(SceneLightsIO::kSceneLightsMetadataKey, aiString(json.constData()));
        return;
    }

    const int chunkCount =
        (static_cast<int>(json.size()) + kAiStringMaxPayload - 1) / kAiStringMaxPayload;
    meta->Add(kSceneLightsChunkCountKey.data(), chunkCount);
    for (int i = 0; i < chunkCount; ++i)
    {
        const QByteArray slice = json.mid(i * kAiStringMaxPayload, kAiStringMaxPayload);
        const QString key = QStringLiteral("qtmesh.scene.lights.%1").arg(i);
        meta->Add(key.toUtf8().constData(), aiString(slice.constData()));
    }
}

QString lightsSidecarPath(const QFileInfo& fi)
{
    return fi.absoluteDir().filePath(fi.completeBaseName() + QStringLiteral(".lights.json"));
}

} // namespace

namespace
{

QString lightTypeToString(Ogre::Light::LightTypes type)
{
    switch (type)
    {
    case Ogre::Light::LT_DIRECTIONAL:
        return QStringLiteral("directional");
    case Ogre::Light::LT_POINT:
        return QStringLiteral("point");
    case Ogre::Light::LT_SPOTLIGHT:
        return QStringLiteral("spot");
    default:
        return QStringLiteral("point");
    }
}

bool lightTypeFromString(const QString& text, Ogre::Light::LightTypes& out)
{
    if (text == QStringLiteral("directional"))
    {
        out = Ogre::Light::LT_DIRECTIONAL;
        return true;
    }
    if (text == QStringLiteral("point"))
    {
        out = Ogre::Light::LT_POINT;
        return true;
    }
    if (text == QStringLiteral("spot") || text == QStringLiteral("spotlight"))
    {
        out = Ogre::Light::LT_SPOTLIGHT;
        return true;
    }
    return false;
}

QJsonArray colourToJson(const Ogre::ColourValue& c)
{
    return QJsonArray{c.r, c.g, c.b, c.a};
}

bool colourFromJson(const QJsonValue& value, Ogre::ColourValue& out)
{
    if (!value.isArray())
        return false;
    const QJsonArray arr = value.toArray();
    if (arr.size() < 3)
        return false;
    out.r = static_cast<float>(arr.at(0).toDouble());
    out.g = static_cast<float>(arr.at(1).toDouble());
    out.b = static_cast<float>(arr.at(2).toDouble());
    out.a = arr.size() > 3 ? static_cast<float>(arr.at(3).toDouble()) : 1.0f;
    return true;
}

QJsonArray vector3ToJson(const Ogre::Vector3& v)
{
    return QJsonArray{v.x, v.y, v.z};
}

bool vector3FromJson(const QJsonValue& value, Ogre::Vector3& out)
{
    if (!value.isArray())
        return false;
    const QJsonArray arr = value.toArray();
    if (arr.size() < 3)
        return false;
    out.x = static_cast<float>(arr.at(0).toDouble());
    out.y = static_cast<float>(arr.at(1).toDouble());
    out.z = static_cast<float>(arr.at(2).toDouble());
    return true;
}

QJsonArray quaternionToJson(const Ogre::Quaternion& q)
{
    return QJsonArray{q.w, q.x, q.y, q.z};
}

bool quaternionFromJson(const QJsonValue& value, Ogre::Quaternion& out)
{
    if (!value.isArray())
        return false;
    const QJsonArray arr = value.toArray();
    if (arr.size() < 4)
        return false;
    out.w = static_cast<float>(arr.at(0).toDouble());
    out.x = static_cast<float>(arr.at(1).toDouble());
    out.y = static_cast<float>(arr.at(2).toDouble());
    out.z = static_cast<float>(arr.at(3).toDouble());
    return true;
}

QJsonObject snapshotToJson(const LightSnapshot& snapshot)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("name"), snapshot.name);
    obj.insert(QStringLiteral("type"), lightTypeToString(snapshot.type));
    obj.insert(QStringLiteral("enabled"), snapshot.enabled);
    obj.insert(QStringLiteral("diffuse"), colourToJson(snapshot.diffuse));
    obj.insert(QStringLiteral("specular"), colourToJson(snapshot.specular));
    obj.insert(QStringLiteral("powerScale"), snapshot.powerScale);
    obj.insert(QStringLiteral("position"), vector3ToJson(snapshot.position));
    obj.insert(QStringLiteral("orientation"), quaternionToJson(snapshot.orientation));
    obj.insert(QStringLiteral("scale"), vector3ToJson(snapshot.scale));
    obj.insert(QStringLiteral("usesDirection"), snapshot.usesDirection);
    obj.insert(QStringLiteral("direction"), vector3ToJson(snapshot.direction));
    obj.insert(QStringLiteral("attenuationRange"), snapshot.attenuationRange);
    obj.insert(QStringLiteral("attenuationConstant"), snapshot.attenuationConstant);
    obj.insert(QStringLiteral("attenuationLinear"), snapshot.attenuationLinear);
    obj.insert(QStringLiteral("attenuationQuadratic"), snapshot.attenuationQuadratic);
    obj.insert(QStringLiteral("spotlightInnerAngleDeg"), snapshot.spotlightInnerAngleDeg);
    obj.insert(QStringLiteral("spotlightOuterAngleDeg"), snapshot.spotlightOuterAngleDeg);
    obj.insert(QStringLiteral("spotlightFalloff"), snapshot.spotlightFalloff);
    obj.insert(QStringLiteral("castShadows"), snapshot.castShadows);
    obj.insert(QStringLiteral("shadowDepthBias"), snapshot.shadowDepthBias);
    obj.insert(QStringLiteral("shadowSlopeBias"), snapshot.shadowSlopeBias);
    return obj;
}

bool snapshotFromJson(const QJsonObject& obj, LightSnapshot& snapshot)
{
    snapshot = {};
    snapshot.name = obj.value(QStringLiteral("name")).toString();
    if (snapshot.name.isEmpty())
        return false;

    Ogre::Light::LightTypes type = Ogre::Light::LT_POINT;
    if (!lightTypeFromString(obj.value(QStringLiteral("type")).toString(), type))
        return false;
    snapshot.type = type;

    snapshot.enabled = obj.value(QStringLiteral("enabled")).toBool(true);
    if (!colourFromJson(obj.value(QStringLiteral("diffuse")), snapshot.diffuse))
        snapshot.diffuse = Ogre::ColourValue::White;
    if (!colourFromJson(obj.value(QStringLiteral("specular")), snapshot.specular))
        snapshot.specular = Ogre::ColourValue(0.8f, 0.8f, 0.8f, 1.0f);
    snapshot.powerScale = static_cast<float>(obj.value(QStringLiteral("powerScale")).toDouble(1.0));
    vector3FromJson(obj.value(QStringLiteral("position")), snapshot.position);
    quaternionFromJson(obj.value(QStringLiteral("orientation")), snapshot.orientation);
    vector3FromJson(obj.value(QStringLiteral("scale")), snapshot.scale);
    if (snapshot.scale == Ogre::Vector3::ZERO)
        snapshot.scale = Ogre::Vector3::UNIT_SCALE;
    snapshot.usesDirection = obj.value(QStringLiteral("usesDirection")).toBool(false);
    vector3FromJson(obj.value(QStringLiteral("direction")), snapshot.direction);
    snapshot.attenuationRange =
        static_cast<float>(obj.value(QStringLiteral("attenuationRange")).toDouble(1000.0));
    snapshot.attenuationConstant =
        static_cast<float>(obj.value(QStringLiteral("attenuationConstant")).toDouble(1.0));
    snapshot.attenuationLinear =
        static_cast<float>(obj.value(QStringLiteral("attenuationLinear")).toDouble(0.0));
    snapshot.attenuationQuadratic =
        static_cast<float>(obj.value(QStringLiteral("attenuationQuadratic")).toDouble(0.0));
    snapshot.spotlightInnerAngleDeg =
        static_cast<float>(obj.value(QStringLiteral("spotlightInnerAngleDeg")).toDouble(30.0));
    snapshot.spotlightOuterAngleDeg =
        static_cast<float>(obj.value(QStringLiteral("spotlightOuterAngleDeg")).toDouble(40.0));
    snapshot.spotlightFalloff =
        static_cast<float>(obj.value(QStringLiteral("spotlightFalloff")).toDouble(1.0));
    snapshot.castShadows = obj.value(QStringLiteral("castShadows")).toBool(false);
    snapshot.shadowDepthBias =
        static_cast<float>(obj.value(QStringLiteral("shadowDepthBias")).toDouble(0.00005));
    snapshot.shadowSlopeBias =
        static_cast<float>(obj.value(QStringLiteral("shadowSlopeBias")).toDouble(1.0));
    return true;
}

QString rigIdFromSceneNode(Ogre::SceneNode* node)
{
    if (!node)
        return {};
    const auto any = node->getUserObjectBindings().getUserAny(SceneLightsIO::kRigIdUserKey);
    if (!any.has_value())
        return {};
    try
    {
        return QString::fromStdString(Ogre::any_cast<std::string>(any));
    }
    catch (...)
    {
        return {};
    }
}

aiMatrix4x4 toAiMatrix(const Ogre::Matrix4& m)
{
    aiMatrix4x4 out;
    out.a1 = m[0][0];
    out.a2 = m[0][1];
    out.a3 = m[0][2];
    out.a4 = m[0][3];
    out.b1 = m[1][0];
    out.b2 = m[1][1];
    out.b3 = m[1][2];
    out.b4 = m[1][3];
    out.c1 = m[2][0];
    out.c2 = m[2][1];
    out.c3 = m[2][2];
    out.c4 = m[2][3];
    out.d1 = m[3][0];
    out.d2 = m[3][1];
    out.d3 = m[3][2];
    out.d4 = m[3][3];
    return out;
}

aiMatrix4x4 localAiMatrix(const LightSnapshot& snapshot)
{
    Ogre::Matrix4 m;
    m.makeTransform(snapshot.position, snapshot.scale, snapshot.orientation);
    return toAiMatrix(m);
}

aiLightSourceType ogreTypeToAssimp(Ogre::Light::LightTypes type)
{
    switch (type)
    {
    case Ogre::Light::LT_DIRECTIONAL:
        return aiLightSource_DIRECTIONAL;
    case Ogre::Light::LT_POINT:
        return aiLightSource_POINT;
    case Ogre::Light::LT_SPOTLIGHT:
        return aiLightSource_SPOT;
    default:
        return aiLightSource_POINT;
    }
}

Ogre::Light::LightTypes assimpTypeToOgre(aiLightSourceType type)
{
    switch (type)
    {
    case aiLightSource_DIRECTIONAL:
        return Ogre::Light::LT_DIRECTIONAL;
    case aiLightSource_POINT:
        return Ogre::Light::LT_POINT;
    case aiLightSource_SPOT:
        return Ogre::Light::LT_SPOTLIGHT;
    default:
        return Ogre::Light::LT_POINT;
    }
}

const aiNode* findNodeByName(const aiNode* node, const std::string& name)
{
    if (!node)
        return nullptr;
    if (node->mName == aiString(name))
        return node;
    for (unsigned int i = 0; i < node->mNumChildren; ++i)
    {
        if (const aiNode* found = findNodeByName(node->mChildren[i], name))
            return found;
    }
    return nullptr;
}

void decomposeAiMatrix(const aiMatrix4x4& m,
                       Ogre::Vector3& pos,
                       Ogre::Quaternion& orient,
                       Ogre::Vector3& scale)
{
    aiVector3D aiPos;
    aiVector3D aiScale;
    aiQuaternion aiRot;
    m.Decompose(aiScale, aiRot, aiPos);
    pos = Ogre::Vector3(aiPos.x, aiPos.y, aiPos.z);
    orient = Ogre::Quaternion(aiRot.w, aiRot.x, aiRot.y, aiRot.z);
    scale = Ogre::Vector3(aiScale.x, aiScale.y, aiScale.z);
}

LightSnapshot snapshotFromAssimpLight(const aiLight* light, const aiNode* node)
{
    LightSnapshot snapshot;
    if (!light || !node)
        return snapshot;

    snapshot.name = QString::fromUtf8(light->mName.C_Str());
    snapshot.type = assimpTypeToOgre(light->mType);
    snapshot.enabled = true;
    snapshot.diffuse =
        Ogre::ColourValue(light->mColorDiffuse.r, light->mColorDiffuse.g, light->mColorDiffuse.b);
    snapshot.specular =
        Ogre::ColourValue(light->mColorSpecular.r, light->mColorSpecular.g, light->mColorSpecular.b);

    const float importedIntensity = std::max(SceneLightsIO::ogreLuminance(snapshot.diffuse), 1e-6f);
    snapshot.powerScale = importedIntensity;
    snapshot.diffuse.r /= importedIntensity;
    snapshot.diffuse.g /= importedIntensity;
    snapshot.diffuse.b /= importedIntensity;

    decomposeAiMatrix(node->mTransformation, snapshot.position, snapshot.orientation, snapshot.scale);
    if (snapshot.scale == Ogre::Vector3::ZERO)
        snapshot.scale = Ogre::Vector3::UNIT_SCALE;

    snapshot.attenuationConstant = light->mAttenuationConstant;
    snapshot.attenuationLinear = light->mAttenuationLinear;
    snapshot.attenuationQuadratic = light->mAttenuationQuadratic;
    snapshot.attenuationRange = 1000.0f;

    if (snapshot.type == Ogre::Light::LT_SPOTLIGHT)
    {
        snapshot.spotlightInnerAngleDeg =
            Ogre::Radian(light->mAngleInnerCone).valueDegrees();
        snapshot.spotlightOuterAngleDeg =
            Ogre::Radian(light->mAngleOuterCone).valueDegrees();
        snapshot.spotlightFalloff = 1.0f;
    }

    if (snapshot.type == Ogre::Light::LT_DIRECTIONAL || snapshot.type == Ogre::Light::LT_SPOTLIGHT)
    {
        snapshot.usesDirection = true;
        Ogre::Vector3 localDir(light->mDirection.x, light->mDirection.y, light->mDirection.z);
        if (localDir.squaredLength() < 1e-8f)
            localDir = Ogre::Vector3::NEGATIVE_UNIT_Z;
        snapshot.direction = snapshot.orientation * localDir;
        snapshot.direction.normalise();
    }

    return snapshot;
}

aiLight* buildAssimpLight(const LightSnapshot& snapshot)
{
    auto* light = new aiLight();
    light->mName = aiString(snapshot.name.toUtf8().constData());
    light->mType = ogreTypeToAssimp(snapshot.type);
    light->mColorDiffuse =
        aiColor3D(snapshot.diffuse.r, snapshot.diffuse.g, snapshot.diffuse.b);
    light->mColorSpecular =
        aiColor3D(snapshot.specular.r, snapshot.specular.g, snapshot.specular.b);
    light->mAttenuationConstant = snapshot.attenuationConstant;
    light->mAttenuationLinear = snapshot.attenuationLinear;
    light->mAttenuationQuadratic = snapshot.attenuationQuadratic;

    const float gltfIntensity = SceneLightsIO::powerScaleToGltfIntensity(snapshot);
    const float lum = std::max(SceneLightsIO::ogreLuminance(snapshot.diffuse), 1e-6f);
    const float chromaScale = gltfIntensity / lum;
    light->mColorDiffuse.r *= chromaScale;
    light->mColorDiffuse.g *= chromaScale;
    light->mColorDiffuse.b *= chromaScale;

    if (snapshot.type == Ogre::Light::LT_SPOTLIGHT)
    {
        light->mAngleInnerCone = Ogre::Degree(snapshot.spotlightInnerAngleDeg).valueRadians();
        light->mAngleOuterCone = Ogre::Degree(snapshot.spotlightOuterAngleDeg).valueRadians();
    }

    if (snapshot.usesDirection)
    {
        Ogre::Vector3 localDir = snapshot.orientation.Inverse() * snapshot.direction;
        localDir.normalise();
        light->mDirection = aiVector3D(localDir.x, localDir.y, localDir.z);
    }

    return light;
}

aiNode* makeAiNodeTree(const QString& name,
                       const aiMatrix4x4& transform,
                       aiNode* parent,
                       std::vector<aiNode*>& storage)
{
    auto* node = new aiNode(name.toUtf8().constData());
    node->mParent = parent;
    node->mTransformation = transform;
    storage.push_back(node);
    return node;
}

void appendChildNode(aiNode* parent, aiNode* child, std::vector<aiNode*>& rootChildren)
{
    if (!parent)
    {
        rootChildren.push_back(child);
        return;
    }

    const unsigned int oldCount = parent->mNumChildren;
    auto** newChildren = new aiNode*[oldCount + 1];
    for (unsigned int i = 0; i < oldCount; ++i)
        newChildren[i] = parent->mChildren[i];
    newChildren[oldCount] = child;
    delete[] parent->mChildren;
    parent->mChildren = newChildren;
    parent->mNumChildren = oldCount + 1;
}

} // namespace

namespace SceneLightsIO
{

float ogreLuminance(const Ogre::ColourValue& colour)
{
    return 0.2126f * colour.r + 0.7152f * colour.g + 0.0722f * colour.b;
}

float powerScaleToGltfIntensity(const LightSnapshot& snapshot)
{
    // KHR_lights_punctual stores a scalar intensity separate from colour.
    // QtMeshEditor stores per-light colour × powerScale in Ogre. We map:
    //   intensity = powerScale × Rec.601_luminance(diffuse)
    // Directional lights are interpreted as lux-like; point/spot as candela-like.
    // This is best-effort for third-party viewers — use qtmesh.scene.lights metadata
    // (written on every scene export) for bit-exact round-trip inside QtMeshEditor.
    return snapshot.powerScale * ogreLuminance(snapshot.diffuse);
}

SceneLightsDocument captureFromScene()
{
    SceneLightsDocument doc;
    auto* mgr = Manager::getSingletonPtr();
    auto* lights = LightManager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr() || !lights)
        return doc;

    doc.ambient = mgr->getSceneMgr()->getAmbientLight();

    Ogre::SceneNode* root = mgr->getSceneMgr()->getRootSceneNode();
    std::map<Ogre::SceneNode*, int> groupIndexByNode;

    for (const auto& child : root->getChildren())
    {
        auto* node = static_cast<Ogre::SceneNode*>(child);
        if (!LightRigLibrary::sceneNodeIsRigGroup(node))
            continue;

        RigGroupExport group;
        group.name = QString::fromStdString(node->getName());
        group.rigId = rigIdFromSceneNode(node);
        group.preserveGrouping = true;
        groupIndexByNode[node] = doc.rigGroups.size();
        doc.rigGroups.append(group);
    }

    for (const LightHandle& handle : lights->lights())
    {
        if (!handle.isValid())
            continue;
        const LightSnapshot snapshot = LightSnapshot::fromHandle(handle);
        auto* parent = static_cast<Ogre::SceneNode*>(handle.sceneNode->getParent());
        const auto groupIt = groupIndexByNode.find(parent);
        if (groupIt != groupIndexByNode.end())
            doc.rigGroups[groupIt->second].lights.append(snapshot);
        else
            doc.standaloneLights.append(snapshot);
    }

    return doc;
}

bool applyToLightManager(const SceneLightsDocument& doc, bool useDefaultWhenEmpty)
{
    auto* lights = LightManager::getSingletonPtr();
    auto* mgr = Manager::getSingletonPtr();
    if (!lights || !mgr || !mgr->getSceneMgr())
        return false;

    lights->deleteAllUserLights();
    mgr->getSceneMgr()->setAmbientLight(doc.ambient);

    int totalLights = doc.standaloneLights.size();
    for (const RigGroupExport& group : doc.rigGroups)
        totalLights += group.lights.size();

    if (totalLights == 0)
    {
        if (useDefaultWhenEmpty)
            LightRigLibrary::applyDefaultSceneLighting();
        return true;
    }

    for (const RigGroupExport& group : doc.rigGroups)
    {
        if (group.lights.isEmpty())
            continue;

        Ogre::SceneNode* rigNode = lights->createRigGroupNode(group.name);
        if (!rigNode)
            continue;

        LightRigLibrary::tagRigGroupNode(rigNode);
        if (!group.rigId.isEmpty())
        {
            rigNode->getUserObjectBindings().setUserAny(
                kRigIdUserKey, Ogre::Any(group.rigId.toStdString()));
        }

        for (const LightSnapshot& snapshot : group.lights)
            lights->restoreSnapshotUnderParent(rigNode, snapshot);
    }

    for (const LightSnapshot& snapshot : doc.standaloneLights)
        lights->restoreSnapshot(snapshot);

    if (auto* shadows = ShadowController::instance())
        shadows->syncFromScene();

    return true;
}

QByteArray documentToJson(const SceneLightsDocument& doc)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), doc.version);
    root.insert(QStringLiteral("ambient"), colourToJson(doc.ambient));

    QJsonArray rigGroups;
    for (const RigGroupExport& group : doc.rigGroups)
    {
        QJsonObject obj;
        obj.insert(QStringLiteral("name"), group.name);
        if (!group.rigId.isEmpty())
            obj.insert(QStringLiteral("rigId"), group.rigId);
        obj.insert(QStringLiteral("preserveGrouping"), group.preserveGrouping);
        QJsonArray lightsArr;
        for (const LightSnapshot& snapshot : group.lights)
            lightsArr.append(snapshotToJson(snapshot));
        obj.insert(QStringLiteral("lights"), lightsArr);
        rigGroups.append(obj);
    }
    root.insert(QStringLiteral("rigGroups"), rigGroups);

    QJsonArray standalone;
    for (const LightSnapshot& snapshot : doc.standaloneLights)
        standalone.append(snapshotToJson(snapshot));
    root.insert(QStringLiteral("lights"), standalone);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool documentFromJson(const QByteArray& json, SceneLightsDocument& out)
{
    out = {};
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return false;

    const QJsonObject root = doc.object();
    out.version = root.value(QStringLiteral("version")).toInt(kDocumentVersion);
    colourFromJson(root.value(QStringLiteral("ambient")), out.ambient);

    const QJsonArray rigGroups = root.value(QStringLiteral("rigGroups")).toArray();
    for (const QJsonValue& value : rigGroups)
    {
        if (!value.isObject())
            continue;
        const QJsonObject obj = value.toObject();
        RigGroupExport group;
        group.name = obj.value(QStringLiteral("name")).toString();
        group.rigId = obj.value(QStringLiteral("rigId")).toString();
        group.preserveGrouping = obj.value(QStringLiteral("preserveGrouping")).toBool(true);
        const QJsonArray lightsArr = obj.value(QStringLiteral("lights")).toArray();
        for (const QJsonValue& lightValue : lightsArr)
        {
            LightSnapshot snapshot;
            if (!snapshotFromJson(lightValue.toObject(), snapshot))
                return false;
            group.lights.append(snapshot);
        }
        if (!group.lights.isEmpty())
            out.rigGroups.append(group);
    }

    const QJsonArray standalone = root.value(QStringLiteral("lights")).toArray();
    for (const QJsonValue& value : standalone)
    {
        LightSnapshot snapshot;
        if (!snapshotFromJson(value.toObject(), snapshot))
            return false;
        out.standaloneLights.append(snapshot);
    }

    return true;
}

bool readDocumentFromAiScene(const aiScene* scene, SceneLightsDocument& out)
{
    out = {};
    if (!scene)
        return false;

    if (scene->mMetaData)
    {
        QByteArray encoded;
        if (readLightsJsonFromMetadata(scene->mMetaData, encoded)
            && documentFromJson(encoded, out))
        {
            return true;
        }
    }

    if (!scene->HasLights() || !scene->mRootNode)
        return false;

    for (unsigned int i = 0; i < scene->mNumLights; ++i)
    {
        const aiLight* light = scene->mLights[i];
        if (!light)
            continue;
        const aiNode* node = findNodeByName(scene->mRootNode, light->mName.C_Str());
        if (!node)
            continue;
        out.standaloneLights.append(snapshotFromAssimpLight(light, node));
    }

    return !out.standaloneLights.isEmpty() || !out.rigGroups.isEmpty();
}

void appendLightsToAiScene(aiScene* scene, const SceneLightsDocument& doc)
{
    if (!scene || !scene->mRootNode)
        return;

    if (!scene->mMetaData)
        scene->mMetaData = new aiMetadata();
    writeLightsJsonToMetadata(scene->mMetaData, documentToJson(doc));

    std::vector<aiLight*> newLights;
    std::vector<aiNode*> ownedNodes;
    std::vector<aiNode*> newRootChildren;
    const unsigned int oldLightCount = scene->mNumLights;

    auto addLight = [&](const LightSnapshot& snapshot, aiNode* parent) {
        aiNode* lightNode =
            makeAiNodeTree(snapshot.name, localAiMatrix(snapshot), parent, ownedNodes);
        appendChildNode(parent, lightNode, newRootChildren);
        newLights.push_back(buildAssimpLight(snapshot));
    };

    for (const RigGroupExport& group : doc.rigGroups)
    {
        aiMatrix4x4 identity;
        aiNode* rigNode =
            makeAiNodeTree(group.name, identity, scene->mRootNode, ownedNodes);
        appendChildNode(scene->mRootNode, rigNode, newRootChildren);
        for (const LightSnapshot& snapshot : group.lights)
            addLight(snapshot, rigNode);
    }

    for (const LightSnapshot& snapshot : doc.standaloneLights)
        addLight(snapshot, scene->mRootNode);

    if (newLights.empty())
        return;

    auto** combinedLights = new aiLight*[oldLightCount + newLights.size()];
    for (unsigned int i = 0; i < oldLightCount; ++i)
        combinedLights[i] = scene->mLights[i];
    for (size_t i = 0; i < newLights.size(); ++i)
        combinedLights[oldLightCount + i] = newLights[i];
    delete[] scene->mLights;
    scene->mLights = combinedLights;
    scene->mNumLights = oldLightCount + static_cast<unsigned int>(newLights.size());

    const unsigned int oldChildCount = scene->mRootNode->mNumChildren;
    auto** combinedChildren = new aiNode*[oldChildCount + newRootChildren.size()];
    for (unsigned int i = 0; i < oldChildCount; ++i)
        combinedChildren[i] = scene->mRootNode->mChildren[i];
    for (size_t i = 0; i < newRootChildren.size(); ++i)
        combinedChildren[oldChildCount + i] = newRootChildren[i];
    delete[] scene->mRootNode->mChildren;
    scene->mRootNode->mChildren = combinedChildren;
    scene->mRootNode->mNumChildren =
        oldChildCount + static_cast<unsigned int>(newRootChildren.size());

    (void)ownedNodes;
}

bool importFromAssimpScene(const aiScene* scene, bool useDefaultWhenEmpty)
{
    SceneLightsDocument doc;
    if (!readDocumentFromAiScene(scene, doc))
    {
        if (useDefaultWhenEmpty)
            return applyToLightManager({}, true);
        return false;
    }
    return applyToLightManager(doc, useDefaultWhenEmpty);
}

QJsonObject lightsInfoJsonFromFile(const QString& path, QString* error)
{
    if (error)
        error->clear();

    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const unsigned int flags = aiProcess_Triangulate | aiProcess_ValidateDataStructure;
    const aiScene* scene = importer.ReadFile(path.toUtf8().constData(), flags);
    if (!scene)
    {
        if (error)
            *error = QString::fromUtf8(importer.GetErrorString());
        return {};
    }

    SceneLightsDocument doc;
    if (!readDocumentFromAiScene(scene, doc))
    {
        const QFileInfo fi(path);
        const QString sidecarPath = lightsSidecarPath(fi);
        if (QFile::exists(sidecarPath))
        {
            QFile sidecar(sidecarPath);
            if (sidecar.open(QIODevice::ReadOnly))
                documentFromJson(sidecar.readAll(), doc);
        }
    }

    if (doc.standaloneLights.isEmpty() && doc.rigGroups.isEmpty())
        return QJsonObject{{QStringLiteral("lights"), QJsonArray{}}};

    QJsonArray lights;
    auto appendLightInfo = [&](const LightSnapshot& snapshot, const QString& parentGroup) {
        QJsonObject obj = snapshotToJson(snapshot);
        obj.insert(QStringLiteral("gltfIntensity"), powerScaleToGltfIntensity(snapshot));
        if (!parentGroup.isEmpty())
            obj.insert(QStringLiteral("rigGroup"), parentGroup);
        lights.append(obj);
    };

    for (const RigGroupExport& group : doc.rigGroups)
    {
        for (const LightSnapshot& snapshot : group.lights)
            appendLightInfo(snapshot, group.name);
    }
    for (const LightSnapshot& snapshot : doc.standaloneLights)
        appendLightInfo(snapshot, {});

    const bool hasQtMeshBlock = [&]() {
        if (!scene->mMetaData)
            return false;
        aiString single;
        if (scene->mMetaData->Get(kSceneLightsMetadataKey, single))
            return true;
        int chunkCount = 0;
        return scene->mMetaData->Get(kSceneLightsChunkCountKey.data(), chunkCount) && chunkCount > 0;
    }();

    QJsonObject root;
    root.insert(QStringLiteral("file"), QFileInfo(path).fileName());
    root.insert(QStringLiteral("ambient"), colourToJson(doc.ambient));
    root.insert(QStringLiteral("lights"), lights);
    root.insert(QStringLiteral("lightCount"), lights.size());
    root.insert(QStringLiteral("source"),
                hasQtMeshBlock ? QStringLiteral("qtmesh.scene.lights")
                               : QStringLiteral("assimp"));
    return root;
}

bool writeLightsSidecar(const QString& meshPath)
{
    const QFileInfo fi(meshPath);
    if (!fi.exists())
        return false;

    const QString sidecarPath = lightsSidecarPath(fi);
    QFile file(sidecarPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(documentToJson(captureFromScene()));
    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                                  QStringLiteral("Wrote lights sidecar %1").arg(sidecarPath));
    return true;
}

bool importLightsSidecar(const QString& meshPath, bool useDefaultWhenEmpty)
{
    const QFileInfo fi(meshPath);
    const QString sidecarPath = lightsSidecarPath(fi);
    if (!QFile::exists(sidecarPath))
        return false;

    QFile file(sidecarPath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    SceneLightsDocument doc;
    if (!documentFromJson(file.readAll(), doc))
        return false;
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                  QStringLiteral("Loaded lights sidecar %1").arg(sidecarPath));
    return applyToLightManager(doc, useDefaultWhenEmpty);
}

bool importLightsFromFile(const QString& path, bool useDefaultWhenEmpty)
{
    if (importLightsSidecar(path, useDefaultWhenEmpty))
        return true;

    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const unsigned int flags = aiProcess_Triangulate | aiProcess_ValidateDataStructure;
    const aiScene* scene = importer.ReadFile(path.toUtf8().constData(), flags);
    if (!scene)
        return false;

    SceneLightsDocument doc;
    if (!readDocumentFromAiScene(scene, doc))
        return false;

    return applyToLightManager(doc, useDefaultWhenEmpty);
}

} // namespace SceneLightsIO
