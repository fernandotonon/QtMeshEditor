#include "DnaTextures.h"

#include "../MeshImporterExporter.h"
#include "../RTShaderHelper.h"

#include <OgreEntity.h>
#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreSubEntity.h>
#include <OgreTechnique.h>

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace Dna {

namespace {

bool isImage(const QFileInfo& f)
{
    static const QStringList ext{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("tga"),
                                 QStringLiteral("webp"), QStringLiteral("bmp")};
    return ext.contains(f.suffix().toLower());
}

enum class Kind { None, Albedo, Normal, Roughness, Metallic, Occlusion };

// Whole-word match on the file's tokens, so "colorful_rock" is not a "color" map but "head_color" is.
Kind kindOf(const QString& stem)
{
    const QStringList tokens = stem.toLower().split(QRegularExpression(QStringLiteral("[^a-z0-9]+")), Qt::SkipEmptyParts);
    auto has = [&](std::initializer_list<const char*> words) {
        for (const char* w : words) if (tokens.contains(QLatin1String(w))) return true;
        return false;
    };
    if (has({"normal", "normals", "nrm", "nor"})) return Kind::Normal;
    if (has({"roughness", "rough", "rgh"})) return Kind::Roughness;
    if (has({"metallic", "metalness", "metal"})) return Kind::Metallic;
    if (has({"ao", "occlusion", "ambientocclusion"})) return Kind::Occlusion;
    if (has({"basecolor", "albedo", "diffuse", "color", "colour", "base", "bc"})) return Kind::Albedo;
    return Kind::None;
}

}  // namespace

QString meshToken(const QString& meshName)
{
    QString t = meshName;
    const int lod = t.indexOf(QRegularExpression(QStringLiteral("_?lod\\d+"), QRegularExpression::CaseInsensitiveOption));
    if (lod > 0) t = t.left(lod);
    if (t.endsWith(QLatin1String("_mesh"), Qt::CaseInsensitive)) t.chop(5);
    return t;
}

TextureScan findCompanionTextures(const QString& dnaPath, const QStringList& meshNames)
{
    TextureScan out;
    const QFileInfo dna(dnaPath);
    QList<QFileInfo> files;
    for (const QString& sub : {QString(), QStringLiteral("textures"), QStringLiteral("Textures")}) {
        const QDir d(sub.isEmpty() ? dna.absolutePath() : dna.absolutePath() + QLatin1Char('/') + sub);
        if (!d.exists()) continue;
        for (const QFileInfo& f : d.entryInfoList(QDir::Files, QDir::Name))
            if (isImage(f)) files.append(f);
    }
    const QString dnaStem = dna.completeBaseName().toLower();

    QStringList skippedKinds;
    for (const QString& meshName : meshNames) {
        MeshTextures mt;
        mt.meshName = meshName;
        const QString token = meshToken(meshName).toLower();
        const QStringList tokenWords = token.split(QRegularExpression(QStringLiteral("[^a-z0-9]+")), Qt::SkipEmptyParts);
        // score: a file naming this mesh beats one that merely starts with the DNA's name, which beats an anonymous
        // one (accepted only for a single-mesh character)
        auto score = [&](const QFileInfo& f) {
            const QString base = f.completeBaseName().toLower();
            const QStringList words = base.split(QRegularExpression(QStringLiteral("[^a-z0-9]+")), Qt::SkipEmptyParts);
            int s = 0;
            bool named = !tokenWords.isEmpty();
            for (const QString& w : tokenWords) if (!words.contains(w)) named = false;
            if (named) s += 4;
            if (base.startsWith(dnaStem) && !dnaStem.isEmpty()) s += 1;
            if (s == 0 && meshNames.size() == 1) s = 1;   // the only mesh: generic names are fine
            return s;
        };
        auto pick = [&](Kind k, QString& slot) {
            int best = 0;
            for (const QFileInfo& f : files) {
                if (kindOf(f.completeBaseName()) != k) continue;
                const int s = score(f);
                if (s > best) { best = s; slot = f.absoluteFilePath(); }
            }
        };
        pick(Kind::Albedo, mt.albedo);
        pick(Kind::Normal, mt.normal);
        pick(Kind::Roughness, mt.roughness);
        out.perMesh.push_back(mt);
    }
    for (const QFileInfo& f : files) {
        const Kind k = kindOf(f.completeBaseName());
        if ((k == Kind::Metallic || k == Kind::Occlusion)) skippedKinds << f.fileName();
    }
    if (!skippedKinds.isEmpty())
        out.notes << QStringLiteral("Metallic / occlusion maps were found (%1) but are not bound: the editor packs them into one "
                                    "ORM texture — use Material Mode > Pack Texture Channels.").arg(skippedKinds.join(QStringLiteral(", ")));
    return out;
}

int bindCompanionTextures(Ogre::Entity* entity, const QString& dnaPath, const QStringList& meshNames, QStringList* notes)
{
    if (!entity) return 0;
    const TextureScan scan = findCompanionTextures(dnaPath, meshNames);
    if (notes) *notes << scan.notes;
    int bound = 0;
    auto& rgm = Ogre::ResourceGroupManager::getSingleton();
    for (size_t i = 0; i < scan.perMesh.size() && i < entity->getNumSubEntities(); ++i) {
        const MeshTextures& mt = scan.perMesh[i];
        if (mt.albedo.isEmpty() && mt.normal.isEmpty() && mt.roughness.isEmpty()) continue;
        try {
            Ogre::MaterialPtr base = Ogre::MaterialManager::getSingleton().getByName("BaseWhite");
            if (!base) continue;
            const std::string matName = entity->getName() + "_" + meshToken(mt.meshName).toStdString() + "_mat";
            Ogre::MaterialPtr mat = base->clone(matName);
            // each file's folder must be a resource location for the texture to load by name
            for (const QString& p : {mt.albedo, mt.normal, mt.roughness}) {
                if (p.isEmpty()) continue;
                const std::string loc = QFileInfo(p).absolutePath().toStdString();
                rgm.addResourceLocation(loc, "FileSystem", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            }
            rgm.initialiseResourceGroup(Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            auto bindSlot = [&](const char* slot, const QString& path) {
                if (path.isEmpty()) return;
                for (auto* tech : mat->getTechniques())
                    for (unsigned short pi = 0; pi < tech->getNumPasses(); ++pi) {
                        Ogre::Pass* pass = tech->getPass(pi);
                        Ogre::TextureUnitState* tus = nullptr;
                        for (unsigned short k = 0; k < pass->getNumTextureUnitStates(); ++k)
                            if (pass->getTextureUnitState(k)->getName() == slot) { tus = pass->getTextureUnitState(k); break; }
                        if (!tus) { tus = pass->createTextureUnitState(); tus->setName(slot); }
                        tus->setTextureName(QFileInfo(path).fileName().toStdString());
                    }
            };
            bindSlot("diffuse_map", mt.albedo);
            bindSlot("normal_map", mt.normal);
            bindSlot("roughness", mt.roughness);
            RTShaderHelper::wirePbrSlotsForFFP(mat.get());
            mat->compile();
            entity->getSubEntity(static_cast<unsigned int>(i))->setMaterialName(matName);
            ++bound;
        } catch (const Ogre::Exception& e) {
            if (notes) *notes << QStringLiteral("Could not bind the textures of '%1': %2").arg(mt.meshName, QString::fromStdString(e.getDescription()));
        }
    }
    if (bound > 0 && !scan.perMesh.empty()) {
        bool anyNormal = false;
        for (const auto& m : scan.perMesh) anyNormal = anyNormal || !m.normal.isEmpty();
        if (anyNormal) MeshImporterExporter::applyNormalMapsToEntity(entity);   // tangents + the RTSS normal-map stage
    }
    if (notes && bound > 0) *notes << QStringLiteral("Bound companion textures to %1 submesh(es).").arg(bound);
    return bound;
}

}  // namespace Dna
