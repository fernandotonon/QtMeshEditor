#include "CharacterScene.h"

#include "../Manager.h"
#include "../RTShaderHelper.h"
#include "../AppStorage.h"
#include "../SentryReporter.h"
#include "SkinTexture.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QImage>
#include <algorithm>
#include <array>
#include <cstring>
#include <QFile>
#include <QFileInfo>
#include <OgreEntity.h>
#include <OgreImage.h>
#include <OgreTextureManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreTextureUnitState.h>
#include <OgreMaterialManager.h>
#include <OgreMesh.h>
#include <OgrePass.h>
#include <OgreTechnique.h>
#include <OgreSceneNode.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>

namespace CharacterGen {

namespace {
Rgb colourFor(Surface s, const Params& p)
{
    switch (s) {
        case Surface::Skin: return p.skin;
        case Surface::Mouth: return p.lip;
        case Surface::Teeth: return {0.90f, 0.88f, 0.82f};
        case Surface::Sclera: return {0.93f, 0.92f, 0.90f};
        case Surface::Iris: return p.eye;
        case Surface::Lashes: return p.lash;
        case Surface::Eye: return {1.0f, 1.0f, 1.0f};
        case Surface::Hair: return {1.0f, 1.0f, 1.0f};
    }
    return p.skin;
}
const char* surfaceName(Surface s)
{
    switch (s) {
        case Surface::Skin: return "skin";
        case Surface::Mouth: return "mouth";
        case Surface::Teeth: return "teeth";
        case Surface::Sclera: return "sclera";
        case Surface::Iris: return "iris";
        case Surface::Lashes: return "lashes";
        case Surface::Eye: return "eye";
        case Surface::Hair: return "hair";
    }
    return "skin";
}
// The iris texture whose colour is closest to the requested eye colour (blue / brown / grey / green / hazel / dark).
QByteArray irisTextureFor(const Rgb& c)
{
    struct IrisTile {
        const char* file;
        Rgb ref;
    };
    static const std::array<IrisTile, 6> tiles = {{
        {"iris_blue.png", {0.35f, 0.50f, 0.65f}},  {"iris_brown.png", {0.45f, 0.25f, 0.10f}}, {"iris_grey.png", {0.55f, 0.58f, 0.60f}},
        {"iris_green.png", {0.30f, 0.50f, 0.20f}}, {"iris_hazel.png", {0.50f, 0.40f, 0.15f}}, {"iris_dark.png", {0.20f, 0.10f, 0.07f}}}};
    auto distance = [&c](const IrisTile& t) {
        const float dr = c[0] - t.ref[0];
        const float dg = c[1] - t.ref[1];
        const float db = c[2] - t.ref[2];
        return dr * dr + dg * dg + db * db;
    };
    const auto best = std::min_element(tiles.begin(), tiles.end(), [&](const IrisTile& x, const IrisTile& y) { return distance(x) < distance(y); });
    return QByteArray(best->file);
}

// Ogre's texture manager is global by name, so the resource is named after the directory it came from: another asset that
// already loaded an "iris_blue.png" must not be mistaken for ours.
std::string eyeTextureName(const QString& dir, const char* file)
{
    const QByteArray key = QFileInfo(dir).absoluteFilePath().toUtf8();
    return std::string("qmc_eye_") + QByteArray::number(qHash(key), 16).toStdString() + "_" + file;
}

void loadEyeTexture(const QString& dir, const char* file)
{
    const std::string name = eyeTextureName(dir, file);
    if (Ogre::TextureManager::getSingleton().resourceExists(name)) return;
    QFile f(QDir(dir).filePath(QString::fromLatin1(file)));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = f.readAll();
    Ogre::Image image;
    auto* stream = new Ogre::MemoryDataStream(size_t(bytes.size()));   // owns a copy of the file
    std::memcpy(stream->getPtr(), bytes.constData(), size_t(bytes.size()));
    Ogre::DataStreamPtr ds(stream);
    image.load(ds, "png");
    Ogre::TextureManager::getSingleton().loadImage(name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, image);
}
// Textures of the optional textured eye: the iris matching the eye colour, from characterEyeDir() (or QTMESH_CHARACTER_EYE_TEX).
void bindEyeTextures(Ogre::Pass* pass, const Rgb& eyeColour)
{
    QString dir = QString::fromLocal8Bit(qgetenv("QTMESH_CHARACTER_EYE_TEX"));
    if (dir.isEmpty()) dir = characterEyeDir();
    const QByteArray irisFile = irisTextureFor(eyeColour);
    if (!QFileInfo::exists(QDir(dir).filePath(QString::fromLatin1(irisFile)))) return;
    loadEyeTexture(dir, irisFile.constData());
    pass->setDiffuse(Ogre::ColourValue::White);
    pass->setAmbient(Ogre::ColourValue(0.6f, 0.6f, 0.6f));
    const std::string name = eyeTextureName(dir, irisFile.constData());
    pass->createTextureUnitState(name)->setName("diffuse_map");
    pass->createTextureUnitState(name)->setName("albedo");
}

// Bind a baked skin PNG as the diffuse texture. The texture is loaded under the file's own name and its directory is registered
// with the resource system, so the exporters find the same file.
void bindSkinTexture(Ogre::Pass* pass, const QString& file)
{
    const QFileInfo info(file);
    const std::string name = info.fileName().toStdString();
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = f.readAll();
    Ogre::Image image;
    auto* stream = new Ogre::MemoryDataStream(size_t(bytes.size()));
    std::memcpy(stream->getPtr(), bytes.constData(), size_t(bytes.size()));
    Ogre::DataStreamPtr ds(stream);
    image.load(ds, "png");
    // a reused name (a character regenerated under the same node name) must show the NEW image, not the cached one
    auto& textures = Ogre::TextureManager::getSingleton();
    if (textures.resourceExists(name)) textures.remove(name);
    textures.loadImage(name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, image);
    try {
        auto& rgm = Ogre::ResourceGroupManager::getSingleton();
        const std::string dir = info.absolutePath().toStdString();
        if (!rgm.resourceLocationExists(dir, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME))
            rgm.addResourceLocation(dir, "FileSystem", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    } catch (const Ogre::Exception&) {
        // the viewport still has the in-memory texture; only an export would miss the file
    }
    pass->setDiffuse(Ogre::ColourValue::White);
    pass->setAmbient(Ogre::ColourValue(0.6f, 0.6f, 0.6f));
    pass->createTextureUnitState(name)->setName("diffuse_map");
    pass->createTextureUnitState(name)->setName("albedo");
}

// Bind a hairstyle texture (PNG with alpha) to a hair material: alpha-tested cards, visible from both sides.
void bindHairTexture(Ogre::Pass* pass, const QString& file)
{
    bindSkinTexture(pass, file);
    pass->setCullingMode(Ogre::CULL_NONE);
    pass->setManualCullingMode(Ogre::MANUAL_CULL_NONE);
    pass->setAlphaRejectSettings(Ogre::CMPF_GREATER_EQUAL, 128);
    pass->setAlphaToCoverageEnabled(true);
}

// Write a style's (tinted) texture to <AppData>/generated_textures/hair_<style>_<content hash>.png and return the path. The name
// carries a hash of the bytes: a new colour must not reuse a cached texture of the same name (RTSS / Ogre cache by name).
QString hairTextureFile(const BuildReport& build)
{
    if (build.hairStyle.isEmpty() || build.hairTexturePng.isEmpty()) return QString();
    const QString dir = QDir(AppStorage::persistentRoot()).filePath(QStringLiteral("generated_textures"));
    QDir().mkpath(dir);
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(build.hairTexturePng, QCryptographicHash::Sha1).toHex().left(10));
    const QString file = QDir(dir).filePath(QStringLiteral("hair_%1_%2.png").arg(build.hairStyle, hash));
    QFileInfo info(file);
    if (!info.exists() || info.size() != build.hairTexturePng.size()) {
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly) || f.write(build.hairTexturePng) != build.hairTexturePng.size()) return QString();
    }
    return file;
}

// Deterministic seed of a character: its shape coefficients and skin colour.
uint32_t skinSeedOf(const Params& p)
{
    uint32_t h = 2166136261u;
    auto mix = [&h](float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        h = (h ^ bits) * 16777619u;
    };
    for (float c : p.bodyShape) mix(c);
    for (float c : p.faceIdentity) mix(c);
    for (float c : p.skin) mix(c);
    return h;
}

// Every live-preview edit bakes a new texture, so only the newest ones are kept: older files are deleted and their GPU textures
// released (a material that still uses one keeps its own reference).
constexpr int kKeptSkinTextures = 32;
constexpr int kKeptHairTextures = 32;

void pruneGeneratedTextures(const QString& dir, const QString& pattern, int keep)
{
    QFileInfoList files = QDir(dir).entryInfoList(QStringList{pattern}, QDir::Files, QDir::Time);   // newest first
    auto& textures = Ogre::TextureManager::getSingleton();
    for (int i = keep; i < files.size(); ++i) {
        const std::string name = files[i].fileName().toStdString();
        if (textures.resourceExists(name)) textures.remove(name);
        QFile::remove(files[i].absoluteFilePath());
    }
}

// Bake the body's skin texture to <AppData>/generated_textures/<unique>_skin.png. Empty when it is off or cannot be written.
QString bakeSkinFile(const Dna::Rig& rig, const Params& params, const QString& unique, const BuildReport& build)
{
    if (!params.skinTexture || !params.nativeHead || rig.meshes.empty()) return QString();
    SkinTextureOptions options;
    options.size = params.skinTextureSize;
    options.seed = skinSeedOf(params);
    options.lips = build.lips;
    options.nipples = build.nipples;
    options.bodyHair = params.bodyHair;
    options.hairDensity = build.bodyHair;
    if (params.openEyes) options.brows = build.brows;   // the closed lids have no layer to frame the brows on
    const QImage image = bakeSkinAlbedo(rig.meshes.front(), params.skin, options);
    if (image.isNull()) return QString();
    const QString dir = QDir(AppStorage::persistentRoot()).filePath(QStringLiteral("generated_textures"));
    QDir().mkpath(dir);
    // a fresh file name per bake: Ogre and the shader generator cache textures by name, so reusing one (a character regenerated
    // under the same node name) kept showing the previous character's image
    static int counter = 0;
    const QString file = QDir(dir).filePath(unique + QStringLiteral("_%1_%2_skin.png").arg(++counter).arg(QDateTime::currentMSecsSinceEpoch()));
    if (!image.save(file, "PNG")) return QString();
    pruneGeneratedTextures(dir, QStringLiteral("*_skin.png"), kKeptSkinTextures);
    return file;
}
}  // namespace

void applyTints(Ogre::Entity* entity, const Params& params, const QString& prefix, const QString& skinTextureFile, const QString& hairTextureFile)
{
    if (!entity) return;
    auto& matMgr = Ogre::MaterialManager::getSingleton();
    Ogre::MeshPtr mesh = entity->getMesh();
    for (const auto& kv : mesh->getSubMeshNameMap()) {
        Surface surface;
        if (!surfaceForMesh(QString::fromStdString(kv.first), &surface)) continue;
        if (kv.second >= entity->getNumSubEntities()) continue;
        const std::string matName = (prefix + QLatin1Char('_') + QLatin1String(surfaceName(surface))).toStdString();
        const Rgb c = colourFor(surface, params);
        Ogre::MaterialPtr mat = matMgr.getByName(matName);
        if (!mat) mat = matMgr.create(matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
        pass->removeAllTextureUnitStates();   // the material may be reused (a node name regenerated): start from a clean state
        pass->setLightingEnabled(true);
        pass->setDiffuse(Ogre::ColourValue(c[0], c[1], c[2]));
        pass->setAmbient(Ogre::ColourValue(c[0] * 0.5f, c[1] * 0.5f, c[2] * 0.5f));
        const bool glossy = surface == Surface::Sclera || surface == Surface::Iris || surface == Surface::Teeth;
        pass->setSpecular(glossy ? Ogre::ColourValue(0.4f, 0.4f, 0.4f) : Ogre::ColourValue(0.06f, 0.06f, 0.06f));
        pass->setShininess(glossy ? 60.0f : 12.0f);
        if (surface == Surface::Skin && kv.first == "body" && !skinTextureFile.isEmpty()) {
            bindSkinTexture(pass, skinTextureFile);
            RTShaderHelper::syncMaterialForViewport(mat);
        }
        if (surface == Surface::Hair && !hairTextureFile.isEmpty()) {
            bindHairTexture(pass, hairTextureFile);
            pass->setSpecular(Ogre::ColourValue(0.12f, 0.12f, 0.12f));
            RTShaderHelper::syncMaterialForViewport(mat);
        }
        if (surface == Surface::Eye) {   // wet cornea
            pass->setSpecular(Ogre::ColourValue(0.7f, 0.7f, 0.7f));
            pass->setShininess(96.0f);
            bindEyeTextures(pass, params.eye);
            RTShaderHelper::syncMaterialForViewport(mat);
        }
        mat->compile();
        entity->getSubEntity(kv.second)->setMaterialName(matName);
        // ...and on the MESH's submesh too: Entity::_initialise(true) (applying an animation, editing the skeleton or a morph)
        // rebuilds the sub-entities from the submesh materials, which dropped every per-entity override (the character came
        // back with the default material: no skin colour, no eye texture).
        entity->getMesh()->getSubMesh(kv.second)->setMaterialName(matName);
    }
}

Ogre::SceneNode* createCharacterInScene(const Assets& assets, const Params& params, const QString& baseName,
                                        SceneBuild* report, QString* error)
{
    SceneBuild local;
    SceneBuild& rep = report ? *report : local;
    rep = SceneBuild();
    Dna::Rig rig = buildRig(assets, params, &rep.build, error);
    if (rig.meshes.empty()) return nullptr;
    Ogre::SceneNode* node = Dna::importRigToScene(rig, baseName, &rep.import, error);
    if (!node) return nullptr;
    const QString skinFile = bakeSkinFile(rig, params, QString::fromStdString(node->getName()), rep.build);
    const QString hairFile = hairTextureFile(rep.build);
    if (!hairFile.isEmpty()) pruneGeneratedTextures(QFileInfo(hairFile).absolutePath(), QStringLiteral("hair_*.png"), kKeptHairTextures);
    for (auto it = node->getAttachedObjectIterator(); it.hasMoreElements();) {
        Ogre::MovableObject* mo = it.getNext();
        if (mo->getMovableType() == "Entity") applyTints(static_cast<Ogre::Entity*>(mo), params, QString::fromStdString(node->getName()), skinFile, hairFile);   // node names are unique: a reused material would keep the old tint
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.character"),
                                  QStringLiteral("create vertices=%1 morphs=%2").arg(rep.import.vertices).arg(rep.import.morphTargets));
    return node;
}

}  // namespace CharacterGen
