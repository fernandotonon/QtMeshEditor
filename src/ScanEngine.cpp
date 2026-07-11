#include "ScanEngine.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QLocale>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTextStream>
#include <QWidget>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "SentryReporter.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "MeshImporterExporter.h"
#include "AnimationMerger.h"
#include "CLIPipeline.h"
#include "FBX/FBXExporter.h"
#include "VertexCacheOptimizer.h"

#include <OgreLogManager.h>
#include <OgreMaterialManager.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreSkeletonManager.h>
#include <OgreSubMesh.h>

#include "PS1/PS1PLY.h"
#include "PS1/PS1TMD.h"
#include "PS1/PS1RSD.h"
#include "TestHelpers.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <numeric>
#include <set>
#include <unordered_map>
#include <vector>

namespace {

static bool meshNameLooksLikeLod(const QString& name)
{
    static const QRegularExpression re(
        QStringLiteral(R"((?:^|[_\-.])(?:lod|level_of_detail)(?:[_\-.]?\d*)?$)"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(name).hasMatch();
}

static void syncTriangleDerivedFields(AssetInfo& info)
{
    info.triangleCount = info.faceCount;
}

static void finalizeMeshStatsAggregates(AssetInfo& info)
{
    info.submeshCount = 0;
    info.maxTrianglesPerMesh = 0;
    for (const MeshStats& ms : info.meshStats) {
        info.submeshCount += ms.submeshCount;
        info.maxTrianglesPerMesh = std::max(info.maxTrianglesPerMesh, ms.triangleCount);
    }
}

static void appendMeshStat(AssetInfo& info, const MeshStats& row)
{
    if (info.meshStats.size() >= AssetInfo::kMaxMeshStatsEntries)
        return;
    info.meshStats.append(row);
}

static void fillMeshStatsFromAssimpScene(const aiScene* scene, AssetInfo& info)
{
    if (!scene)
        return;
    info.lodMeshCount = 0;
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* m = scene->mMeshes[i];
        if (!m)
            continue;
        const QString name = QString::fromUtf8(m->mName.C_Str());
        if (meshNameLooksLikeLod(name))
            info.lodMeshCount++;

        MeshStats row;
        row.name = name.isEmpty() ? QStringLiteral("mesh_%1").arg(i) : name;
        row.vertexCount = m->mNumVertices;
        row.triangleCount = m->mNumFaces;
        row.submeshCount = 1;
        appendMeshStat(info, row);
    }
    finalizeMeshStatsAggregates(info);
    info.estimatedDrawCalls = scene->mNumMeshes;
}

static void fillMeshStatsFromOgreMesh(const Ogre::MeshPtr& mesh, const QString& meshLabel,
                                    AssetInfo& info)
{
    if (!mesh)
        return;
    MeshStats row;
    row.name = meshLabel.isEmpty() ? QString::fromStdString(mesh->getName()) : meshLabel;
    row.submeshCount = mesh->getNumSubMeshes();
    const bool hasShared = mesh->sharedVertexData != nullptr;
    if (hasShared && mesh->sharedVertexData)
        row.vertexCount = mesh->sharedVertexData->vertexCount;

    for (unsigned s = 0; s < mesh->getNumSubMeshes(); ++s) {
        const Ogre::SubMesh* sm = mesh->getSubMesh(s);
        if (!sm || !sm->indexData)
            continue;
        row.triangleCount += sm->indexData->indexCount / 3;
        if (!hasShared) {
            if (sm->vertexData)
                row.vertexCount += sm->vertexData->vertexCount;
        } else if (!sm->useSharedVertices && sm->vertexData) {
            row.vertexCount += sm->vertexData->vertexCount;
        }
    }
    if (meshNameLooksLikeLod(row.name))
        info.lodMeshCount++;
    appendMeshStat(info, row);
    finalizeMeshStatsAggregates(info);
}

static void fillMeshStatsFromOgreEntities(const QList<Ogre::Entity*>& entities, AssetInfo& info)
{
    info.meshStats.clear();
    info.lodMeshCount = 0;
    unsigned int drawCalls = 0;
    for (const Ogre::Entity* entity : entities) {
        if (!entity)
            continue;
        drawCalls += entity->getNumSubEntities();
        const Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh)
            continue;
        fillMeshStatsFromOgreMesh(mesh, QString::fromStdString(entity->getName()), info);
    }
    info.estimatedDrawCalls = drawCalls;
    if (info.meshStats.isEmpty())
        finalizeMeshStatsAggregates(info);
}

static QString resolveTexturePath(const QString& assetFilePath, const QString& textureRef)
{
    if (textureRef.isEmpty())
        return {};
    const QFileInfo refFi(textureRef);
    if (refFi.isAbsolute())
        return refFi.absoluteFilePath();
    const QFileInfo assetFi(assetFilePath);
    const QString besideAsset = QDir(assetFi.absolutePath()).filePath(textureRef);
    if (QFileInfo::exists(besideAsset))
        return QFileInfo(besideAsset).absoluteFilePath();
    return refFi.filePath();
}

static void probeReferencedTextures(AssetInfo& info, const AssetInspectOptions& options)
{
    if (!options.probeTextureFiles)
        return;

    info.textureStats.clear();
    info.probedTextureMaxDimension = 0;
    info.estimatedTextureVramBytes = 0;

    std::set<QString> seen;
    for (const QString& ref : info.texturePaths) {
        if (ref.isEmpty() || seen.count(ref) != 0)
            continue;
        seen.insert(ref);
        if (info.textureStats.size() >= AssetInfo::kMaxTextureStatsEntries)
            break;

        TextureRefStats ts;
        ts.path = ref;
        ts.resolvedPath = resolveTexturePath(info.filePath, ref);
        const QFileInfo fi(ts.resolvedPath);
        if (!fi.exists() || !fi.isFile()) {
            ts.missing = true;
            info.textureStats.append(ts);
            continue;
        }

        ts.fileSizeBytes = fi.size();
        ts.format = fi.suffix().toLower();

        QImageReader reader(ts.resolvedPath);
        const QSize dim = reader.size();
        if (dim.isValid() && dim.width() > 0 && dim.height() > 0) {
            ts.width = dim.width();
            ts.height = dim.height();
            const int maxDim = std::max(ts.width, ts.height);
            info.probedTextureMaxDimension = std::max(info.probedTextureMaxDimension, maxDim);
            // Estimate only: assumes uncompressed RGBA8 per texel (actual GPU format may differ).
            info.estimatedTextureVramBytes += static_cast<qint64>(ts.width) * ts.height * 4;
        }

        info.textureStats.append(ts);
    }

    if (info.probedTextureMaxDimension > 0)
        info.maxTextureDimension = std::max(info.maxTextureDimension, info.probedTextureMaxDimension);
}

static void copyAssetInspectExtensionFields(AssetInfo& dst, const AssetInfo& src)
{
    dst.triangleCount = src.triangleCount;
    dst.submeshCount = src.submeshCount;
    dst.maxTrianglesPerMesh = src.maxTrianglesPerMesh;
    dst.meshStats = src.meshStats;
    dst.textureStats = src.textureStats;
    dst.probedTextureMaxDimension = src.probedTextureMaxDimension;
    dst.estimatedTextureVramBytes = src.estimatedTextureVramBytes;
    dst.estimatedDrawCalls = src.estimatedDrawCalls;
    dst.lodMeshCount = src.lodMeshCount;
}

static void finalizeAssetInspect(AssetInfo& info, const AssetInspectOptions& options)
{
    syncTriangleDerivedFields(info);
    probeReferencedTextures(info, options);
}

bool ensureOgreHeadlessQuiet()
{
    // ScanEngine uses Ogre for fix paths and (slice C2) for ACMR, but doesn't
    // need the Ogre log spam in the CLI output. Mirror CLIPipeline's default
    // behavior: suppress debug output unless the user explicitly asked for
    // verbose logs (ScanEngine has no --verbose flag).
    if (!Ogre::LogManager::getSingletonPtr()) {
        auto* logMgr = new Ogre::LogManager();
        logMgr->createLog("ogre.log", true, false, true); // default, debugOut=false, suppressFile=true
    } else {
        auto* log = Ogre::LogManager::getSingleton().getDefaultLog();
        if (log)
            log->setDebugOutputEnabled(false);
    }

    try {
        Manager::getSingleton();
        // ScanEngine runs inside CLI/GUI processes where Qt is already initialized,
        // but Ogre still needs a render target for hardware buffers to exist.
        auto* root = Manager::getSingleton()->getRoot();
        if (root) {
            try {
                if (root->getRenderTarget("ScanHidden") || root->getRenderTarget("CLIHidden") || root->getRenderTarget("TestHidden"))
                    return true;
            } catch (...) {
                // ignore
            }
        }

        static QWidget* hiddenWidget = nullptr;
        if (!hiddenWidget) {
            hiddenWidget = new QWidget();
            hiddenWidget->setAttribute(Qt::WA_DontShowOnScreen);
            hiddenWidget->resize(1, 1);
            hiddenWidget->show();
        }

        try {
            Ogre::NameValuePairList params;
            params["externalWindowHandle"] = Ogre::StringConverter::toString(
                static_cast<unsigned long>(hiddenWidget->winId()));
#ifdef Q_OS_MACOS
            params["macAPI"] = "cocoa";
            params["macAPICocoaUseNSView"] = "true";
#endif
            Manager::getSingleton()->getRoot()->createRenderWindow(
                "ScanHidden", 1, 1, false, &params);
        } catch (...) {
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

// MeshImporterExporter::importer appends to the scene without clearing prior
// imports. The scan loops over many files in one process — both inspect and
// --fix import each one — so without per-file cleanup stale entities,
// meshes, and skeletons pile up in Ogre's resource managers across a scan
// of a large directory. Clear the scene graph and ask each resource manager
// to drop anything no longer referenced. Wrapped in try/catch because
// unloadUnreferencedResources can throw if some asset is still pinned by
// a resource group's load order.
static void clearOgreSceneForScanImport()
{
    if (!Manager::getSingletonPtr())
        return;
    SelectionSet::getSingleton()->clearList();
    auto* manager = Manager::getSingleton();
    const QList<Ogre::SceneNode*> sceneNodesCopy = manager->getSceneNodes();
    for (auto* sn : sceneNodesCopy) {
        if (sn)
            manager->destroySceneNode(sn);
    }

    // Free per-file resources so a 1000-asset scan doesn't accumulate
    // ~N meshes / materials / skeletons in the manager pools. Exceptions
    // from the unloads are non-fatal — they typically mean the manager
    // still has a reference somewhere (a Skeleton pinned by a Mesh, etc.).
    // Log and move on so the next file can still be scanned.
    auto* log = Ogre::LogManager::getSingletonPtr()
                  ? Ogre::LogManager::getSingleton().getDefaultLog()
                  : nullptr;
    try {
        Ogre::MeshManager::getSingleton().unloadUnreferencedResources(true);
    } catch (const Ogre::Exception& e) {
        if (log) log->logMessage(
            std::string("ScanEngine: MeshManager unload skipped — ") + e.what(),
            Ogre::LML_NORMAL);
    }
    try {
        Ogre::SkeletonManager::getSingleton().unloadUnreferencedResources(true);
    } catch (const Ogre::Exception& e) {
        if (log) log->logMessage(
            std::string("ScanEngine: SkeletonManager unload skipped — ") + e.what(),
            Ogre::LML_NORMAL);
    }
}

} // namespace

static bool pathEndsWithInsensitive(const QString& p, QLatin1String suf)
{
    if (p.size() < suf.size())
        return false;
    return p.endsWith(suf, Qt::CaseInsensitive);
}

static int nextScanInspectMeshId()
{
    static std::atomic<int> seq{0};
    return ++seq;
}

static void ensureBaseMaterialForScanInspect()
{
    if (Ogre::MaterialManager::getSingleton().getByName(
            "BaseMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)) {
        return;
    }
    Ogre::MaterialPtr m = Ogre::MaterialManager::getSingleton().create(
        "BaseMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    m->getTechnique(0)->getPass(0)->setDiffuse(1.0f, 1.0f, 1.0f, 1.0f);
    m->getTechnique(0)->getPass(0)->setAmbient(1.0f, 1.0f, 1.0f);
}

static bool ensureOgreMaterialsForScanInspect(QString* outErr)
{
    if (!ensureOgreHeadlessQuiet()) {
        if (outErr)
            *outErr = QStringLiteral("Ogre headless init failed (needed for PlayStation mesh scan)");
        return false;
    }
    createStandardOgreMaterials();
    ensureBaseMaterialForScanInspect();
    return true;
}

static void fillAssetInfoFromOgreMesh(AssetInfo& info, const Ogre::MeshPtr& mesh)
{
    info.loadError = false;
    info.errorMessage.clear();
    info.meshCount = 1;
    info.materialCount = mesh->getNumSubMeshes();
    info.animationCount = 0;
    info.vertexCount = 0;
    info.faceCount = 0;
    info.boneCount = 0;
    info.hasSkeleton = false;
    info.hasEmbeddedTextures = false;
    info.materialNames.clear();
    info.texturePaths.clear();
    info.textureRefCount = 0;
    info.animationNames.clear();
    info.animationDurations.clear();
    info.animationKeyframeCounts.clear();
    info.boneNames.clear();
    info.animationRedundantKeyframeRatio = 0.0;
    info.totalKeyframes = 0;
    info.redundantKeyframes = 0;

    const unsigned nSub = mesh->getNumSubMeshes();
    const bool hasShared = mesh->sharedVertexData != nullptr;
    if (hasShared && mesh->sharedVertexData)
        info.vertexCount = mesh->sharedVertexData->vertexCount;

    for (unsigned i = 0; i < nSub; ++i) {
        const Ogre::SubMesh* sm = mesh->getSubMesh(i);
        if (!sm || !sm->indexData)
            continue;
        if (!hasShared) {
            if (sm->vertexData)
                info.vertexCount += sm->vertexData->vertexCount;
        } else if (!sm->useSharedVertices && sm->vertexData) {
            // Shared pool is already counted; add submesh-local vertex buffers.
            info.vertexCount += sm->vertexData->vertexCount;
        }
        info.faceCount += sm->indexData->indexCount / 3;
    }

    for (unsigned i = 0; i < nSub; ++i) {
        if (const Ogre::SubMesh* sm = mesh->getSubMesh(i)) {
            const Ogre::String& mat = sm->getMaterialName();
            if (!mat.empty())
                info.materialNames.append(QString::fromStdString(mat));
        }
    }

    info.meshStats.clear();
    fillMeshStatsFromOgreMesh(mesh, {}, info);
    syncTriangleDerivedFields(info);
    info.estimatedDrawCalls = info.submeshCount;
}

#ifdef QTMESH_UNIT_TESTS
void ScanEngine::testApplyOgreMeshInspectCounts(AssetInfo& info, const Ogre::MeshPtr& mesh)
{
    if (!mesh) {
        info.vertexCount = 0;
        info.faceCount = 0;
        return;
    }
    fillAssetInfoFromOgreMesh(info, mesh);
}
#endif

// Phase 6 slice C2: Ogre-backed ACMR. The Assimp face-array walk that used
// to populate AssetInfo::weightedAcmr produced numbers that did not match
// the editor's in-app validator (Assimp re-orders triangles during import,
// so its flattened indices have different cache locality than what Ogre's
// MeshSerializer ultimately ships to the GPU). This helper imports the
// file through MeshImporterExporter — the same path the editor uses — and
// computes ACMR over the actual Ogre index buffers, so scan-side thresholds
// (max_acmr) and in-editor metrics agree on every asset.

// Read an Ogre SubMesh's index buffer into a uint32 vector, transparently
// handling the 16/32-bit variants. Returns empty on non-triangulated or
// degenerate input.
static std::vector<uint32_t> readSubmeshIndexBuffer(const Ogre::SubMesh* sub)
{
    std::vector<uint32_t> idxFlat;
    if (!sub || !sub->indexData || !sub->indexData->indexBuffer) return idxFlat;
    const Ogre::IndexData* id = sub->indexData;
    if (id->indexCount < 3 || id->indexCount % 3 != 0) return idxFlat;

    const bool use16 = id->indexBuffer->getType() == Ogre::HardwareIndexBuffer::IT_16BIT;
    idxFlat.resize(id->indexCount);
    const void* src = id->indexBuffer->lock(Ogre::HardwareBuffer::HBL_READ_ONLY);
    if (use16) {
        const auto* in = static_cast<const uint16_t*>(src);
        for (size_t i = 0; i < idxFlat.size(); ++i)
            idxFlat[i] = in[id->indexStart + i];
    } else {
        const auto* in = static_cast<const uint32_t*>(src);
        for (size_t i = 0; i < idxFlat.size(); ++i)
            idxFlat[i] = in[id->indexStart + i];
    }
    id->indexBuffer->unlock();
    return idxFlat;
}

// Walk one entity's submeshes, accumulating ACMR weighted by triangle count.
static void accumulateEntityAcmr(const Ogre::Entity* entity,
                                 double& weightedSum, unsigned int& totalTris)
{
    if (!entity) return;
    const Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return;
    for (unsigned int s = 0; s < mesh->getNumSubMeshes(); ++s) {
        const std::vector<uint32_t> idxFlat = readSubmeshIndexBuffer(mesh->getSubMesh(s));
        if (idxFlat.empty()) continue;
        const double acmr = VertexCacheOptimizer::computeAcmr(idxFlat);
        const auto tris = static_cast<unsigned int>(idxFlat.size() / 3);
        weightedSum += acmr * tris;
        totalTris += tris;
    }
}

// Walk a single Ogre entity, accumulating CLIPipeline::extractMeshInfo into the
// AssetInfo aggregate fields. Shared helper so both the per-entity loop and
// future per-entity tests can use the same merge logic.
static void mergeOgreEntityIntoAssetInfo(const Ogre::Entity* entity,
                                         AssetInfo& info,
                                         std::set<std::string>& seenMaterials,
                                         std::set<std::string>& seenBones,
                                         std::set<std::string>& seenAnims,
                                         std::set<std::string>& seenTextures)
{
    if (!entity) return;
    // extractMeshInfo lives in CLIPipeline and is the same routine the in-app
    // MeshInfoOverlay uses — so scan, info CLI, and overlay all show the same
    // numbers for the same asset.
    const MeshInfo mi = CLIPipeline::extractMeshInfo(entity, QString());

    info.vertexCount += mi.vertices;
    info.faceCount   += mi.triangles;
    info.meshCount   += 1;   // one mesh per entity (multi-entity scenes count both)

    for (const QString& m : mi.materials) {
        if (m.isEmpty()) continue;
        if (seenMaterials.insert(m.toStdString()).second) {
            info.materialNames.append(m);
            info.materialCount++;
        }
    }

    for (const QString& t : mi.textures) {
        if (t.isEmpty()) continue;
        if (seenTextures.insert(t.toStdString()).second) {
            info.texturePaths.append(t);
            info.textureRefCount++;
        }
    }

    for (const QString& b : mi.bones) {
        if (b.isEmpty()) continue;
        if (seenBones.insert(b.toStdString()).second)
            info.boneNames.append(b);
    }
    info.boneCount = static_cast<unsigned int>(info.boneNames.size());
    info.hasSkeleton = info.boneCount > 0;

    // De-duplicate animations by name across entities.
    for (const auto& a : mi.animations) {
        if (a.name.isEmpty()) continue;
        if (!seenAnims.insert(a.name.toStdString()).second)
            continue;
        info.animationNames.append(a.name);
        info.animationDurations.append(static_cast<double>(a.duration));
        info.animationCount++;
    }
}

// Run AnimationMerger::analyzeRedundantKeyframes — the same analyzer used by
// `qtmesh anim --simplify` and the Inspector's Simplify action — on every
// animation on the entity's skeleton, accumulating per-anim maxKeyframes and
// total/redundant counts into AssetInfo.
static void fillRedundancyFromOgreSkeleton(const Ogre::Entity* entity,
                                           AssetInfo& info,
                                           const AnimationMerger::SimplifyTolerances& tol,
                                           std::set<std::string>& seenAnims)
{
    if (!entity || !entity->hasSkeleton()) return;
    const Ogre::SkeletonPtr skel = entity->getMesh() ? entity->getMesh()->getSkeleton()
                                                     : Ogre::SkeletonPtr();
    if (!skel) return;

    for (unsigned short a = 0; a < skel->getNumAnimations(); ++a) {
        const Ogre::Animation* anim = skel->getAnimation(a);
        if (!anim) continue;
        // Only count once per animation name across multi-entity scenes — the
        // metadata pass already de-dup'd, mirror that here.
        const std::string animKey = anim->getName() + "::analyzed";
        if (!seenAnims.insert(animKey).second) continue;

        unsigned maxKeys = 0;
        for (const auto& [handle, track] : anim->_getNodeTrackList()) {
            if (track)
                maxKeys = std::max(maxKeys, static_cast<unsigned>(track->getNumKeyFrames()));
        }
        info.animationKeyframeCounts.append(static_cast<int>(maxKeys));

        int t = 0, r = 0;
        AnimationMerger::analyzeRedundantKeyframes(anim, tol, &t, &r);
        info.totalKeyframes     += t;
        info.redundantKeyframes += r;
    }
}

// --- Phase 6 slice C4 collectors --------------------------------------------
//
// These all consume the same Ogre scene the C3 walk uses. They live next to
// the existing helpers so each new rule's data source is right next to the
// rule itself in the file. Each collector either:
//   - accumulates into AssetInfo directly (texture resolution, UV channels)
//   - returns data the rule evaluator consumes via AssetInfo (zero-weight
//     bones, overlapping-UV ratio, non-manifold edges ratio).

// Largest single-axis pixel dimension across every Texture bound through
// every SubEntity's material. Walks TextureUnitState::getTextureName() and
// resolves the Texture; gracefully handles unbound names.
static int maxTextureDimensionForEntities(const QList<Ogre::Entity*>& entities)
{
    auto& tmgr = Ogre::TextureManager::getSingleton();
    int worst = 0;
    for (const Ogre::Entity* entity : entities) {
        if (!entity) continue;
        for (unsigned int i = 0; i < entity->getNumSubEntities(); ++i) {
            const auto* sub = entity->getSubEntity(i);
            const auto mat = sub ? sub->getMaterial() : Ogre::MaterialPtr();
            if (!mat) continue;
            for (auto* tech : mat->getTechniques()) {
                for (auto* pass : tech->getPasses()) {
                    for (auto* tus : pass->getTextureUnitStates()) {
                        if (tus->getContentType() != Ogre::TextureUnitState::CONTENT_NAMED)
                            continue;
                        const Ogre::String n = tus->getTextureName();
                        if (n.empty()) continue;
                        const Ogre::TexturePtr t = tmgr.getByName(
                            n, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
                        if (!t) continue;
                        const int dim = static_cast<int>(std::max(t->getWidth(), t->getHeight()));
                        worst = std::max(worst, dim);
                    }
                }
            }
        }
    }
    return worst;
}

// Count distinct UV sets (VES_TEXTURE_COORDINATES) in a single VertexData's
// declaration. Returns 0 when vd is null.
static int countUvSets(const Ogre::VertexData* vd)
{
    if (!vd || !vd->vertexDeclaration) return 0;
    int seen = 0;
    const auto& elems = vd->vertexDeclaration->getElements();
    for (const auto& e : elems) {
        if (e.getSemantic() == Ogre::VES_TEXTURE_COORDINATES)
            seen++;
    }
    return seen;
}

// Minimum UV-set count across every submesh of every entity. The conservative
// "what's guaranteed to be there" value. 0 when no submesh has any UVs.
static int minUvChannelCountForEntities(const QList<Ogre::Entity*>& entities)
{
    int worst = INT_MAX;
    bool seenAny = false;
    for (const Ogre::Entity* entity : entities) {
        if (!entity) continue;
        const Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh) continue;
        for (unsigned int s = 0; s < mesh->getNumSubMeshes(); ++s) {
            const Ogre::SubMesh* sub = mesh->getSubMesh(s);
            if (!sub) continue;
            const Ogre::VertexData* vd = sub->useSharedVertices
                ? mesh->sharedVertexData
                : sub->vertexData;
            const int uvCount = countUvSets(vd);
            worst = std::min(worst, uvCount);
            seenAny = true;
        }
    }
    return seenAny ? worst : 0;
}

// Bones that exist in the skeleton but have no VertexBoneAssignment on any
// submesh — typical Mixamo bloat where the armature carries 70 bones but
// only ~30 actually influence vertices. Empty when there's no skeleton or
// every bone is used.
static QStringList zeroWeightBonesForEntities(const QList<Ogre::Entity*>& entities)
{
    QStringList dead;
    for (const Ogre::Entity* entity : entities) {
        if (!entity || !entity->hasSkeleton()) continue;
        const Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh) continue;
        const Ogre::SkeletonPtr skel = mesh->getSkeleton();
        if (!skel) continue;

        std::set<unsigned short> usedHandles;
        // Mesh-level (shared-geometry skinned meshes — common Ogre layout
        // for skinned characters where one VertexData feeds every submesh).
        for (const auto& [vertexIdx, vba] : mesh->getBoneAssignments())
            usedHandles.insert(vba.boneIndex);
        // SubMesh-level (per-submesh skin — used when submeshes carry their
        // own VertexData rather than sharing the mesh-level pool).
        for (unsigned int s = 0; s < mesh->getNumSubMeshes(); ++s) {
            const Ogre::SubMesh* sub = mesh->getSubMesh(s);
            if (!sub) continue;
            for (const auto& [vertexIdx, vba] : sub->getBoneAssignments())
                usedHandles.insert(vba.boneIndex);
        }

        for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
            const Ogre::Bone* b = skel->getBone(i);
            if (!b) continue;
            if (usedHandles.count(b->getHandle()) == 0)
                dead.append(QString::fromStdString(b->getName()));
        }
    }
    return dead;
}

// UV0 AABB overlap detection (O(n log n) sweep on triangle-AABB x-ranges).
// Returns -1 when UV0 isn't available or no triangulated submesh exists.
//
// This is an *upper bound* on true UV overlap: if two triangle AABBs don't
// intersect, the triangles can't overlap; if they do, true SAT is needed
// to confirm. Cheap proxy that catches the common cases (Mixamo body /
// clothing UV islands sharing the same 0-1 range, etc.).
static double overlappingUvsRatioForEntities(const QList<Ogre::Entity*>& entities)
{
    struct TriBox { float xmin, xmax, ymin, ymax; };
    std::vector<TriBox> boxes;
    bool sawUv0 = false;

    for (const Ogre::Entity* entity : entities) {
        if (!entity) continue;
        const Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh) continue;

        for (unsigned int s = 0; s < mesh->getNumSubMeshes(); ++s) {
            const Ogre::SubMesh* sub = mesh->getSubMesh(s);
            if (!sub || !sub->indexData || !sub->indexData->indexBuffer) continue;
            const Ogre::VertexData* vd = sub->useSharedVertices
                ? mesh->sharedVertexData
                : sub->vertexData;
            if (!vd || !vd->vertexDeclaration) continue;

            const Ogre::VertexElement* uvElem = vd->vertexDeclaration
                ->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES, 0);
            if (!uvElem) continue;
            sawUv0 = true;

            const Ogre::HardwareVertexBufferSharedPtr vbuf =
                vd->vertexBufferBinding->getBuffer(uvElem->getSource());
            if (!vbuf) continue;

            const auto* vbase = static_cast<const unsigned char*>(
                vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            const size_t vstride = vbuf->getVertexSize();

            const std::vector<uint32_t> idx = readSubmeshIndexBuffer(sub);
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                float u[3], v[3];
                for (int k = 0; k < 3; ++k) {
                    const unsigned char* vp = vbase + idx[t + k] * vstride;
                    float* uv;
                    uvElem->baseVertexPointerToElement(const_cast<unsigned char*>(vp),
                                                      &uv);
                    u[k] = uv[0];
                    v[k] = uv[1];
                }
                TriBox box;
                box.xmin = std::min({u[0], u[1], u[2]});
                box.xmax = std::max({u[0], u[1], u[2]});
                box.ymin = std::min({v[0], v[1], v[2]});
                box.ymax = std::max({v[0], v[1], v[2]});
                boxes.push_back(box);
            }
            vbuf->unlock();
        }
    }

    if (!sawUv0 || boxes.empty()) return -1.0;

    // Sweep on xmin; for each box, scan neighbours whose xmin <= our xmax
    // and check 2D AABB overlap. With ascending sort on xmin this is
    // O(n log n + overlaps).
    std::vector<size_t> order(boxes.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return boxes[a].xmin < boxes[b].xmin;
    });

    std::vector<bool> overlapping(boxes.size(), false);
    for (size_t i = 0; i < order.size(); ++i) {
        const TriBox& A = boxes[order[i]];
        for (size_t j = i + 1; j < order.size(); ++j) {
            const TriBox& B = boxes[order[j]];
            if (B.xmin > A.xmax) break; // sweep terminator
            if (B.xmax < A.xmin) continue;
            if (A.ymax < B.ymin || B.ymax < A.ymin) continue;
            overlapping[order[i]] = true;
            overlapping[order[j]] = true;
        }
    }
    const auto over = std::count(overlapping.begin(), overlapping.end(), true);
    return static_cast<double>(over) / static_cast<double>(boxes.size());
}

// Non-manifold edge fraction. An edge shared by != 2 triangles is
// non-manifold. Boundary edges (1 face) count as non-manifold here — a
// proper manifold closed-surface check needs that. Returns -1 when there
// are no triangulated submeshes.
//
// The edge key carries the *vertex-pool pointer* (mesh->sharedVertexData when
// useSharedVertices, sub->vertexData otherwise) alongside the (min,max) index
// pair. Without this, raw indices like (0,1) collide across unrelated
// submeshes and entities — every fresh index buffer starts at 0 — and the
// non-manifold count would be wildly wrong on multi-submesh assets.
static double nonManifoldEdgesRatioForEntities(const QList<Ogre::Entity*>& entities)
{
    struct EdgeKey {
        const void* pool;
        uint32_t a;
        uint32_t b;
        bool operator==(const EdgeKey& o) const noexcept {
            return pool == o.pool && a == o.a && b == o.b;
        }
    };
    struct EdgeHash {
        std::size_t operator()(const EdgeKey& k) const noexcept {
            const auto p = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(k.pool));
            const uint64_t ab = (static_cast<uint64_t>(k.a) << 32) | k.b;
            // Mix the pool pointer in so edges from different vertex pools
            // never collide in the same hash bucket.
            return std::hash<uint64_t>{}(p ^ (ab * 0x9E3779B97F4A7C15ULL));
        }
    };
    std::unordered_map<EdgeKey, unsigned int, EdgeHash> edges;
    bool sawTris = false;

    for (const Ogre::Entity* entity : entities) {
        if (!entity) continue;
        const Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh) continue;
        for (unsigned int s = 0; s < mesh->getNumSubMeshes(); ++s) {
            const Ogre::SubMesh* sub = mesh->getSubMesh(s);
            if (!sub) continue;
            const std::vector<uint32_t> idx = readSubmeshIndexBuffer(sub);
            if (idx.empty()) continue;
            const void* pool = sub->useSharedVertices
                ? static_cast<const void*>(mesh->sharedVertexData)
                : static_cast<const void*>(sub->vertexData);
            sawTris = true;
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                for (int k = 0; k < 3; ++k) {
                    uint32_t a = idx[t + k];
                    uint32_t b = idx[t + ((k + 1) % 3)];
                    if (a > b) std::swap(a, b);
                    edges[EdgeKey{pool, a, b}]++;
                }
            }
        }
    }

    if (!sawTris || edges.empty()) return -1.0;
    unsigned int nonManifold = 0;
    for (const auto& [_, count] : edges)
        if (count != 2) nonManifold++;
    return static_cast<double>(nonManifold) / static_cast<double>(edges.size());
}

// PS1-capture degeneracy fractions (#428). Walks every triangulated submesh
// once and returns two ratios via out-params:
//   *zeroAreaOut       — fraction of triangles with ~zero POSITION-space area
//                        (collinear / duplicate-vertex slivers).
//   *degenerateUvOut   — fraction of triangles whose UV0 triangle has ~zero
//                        area (all three UVs coincident/collinear). -1 when
//                        UV0 is absent.
// Both are -1 when the asset has no triangulated submeshes. Mirrors the
// buffer-locking idiom of overlappingUvsRatioForEntities so the scan sees the
// exact geometry the editor loaded.
static void ps1RipDegeneracyRatiosForEntities(const QList<Ogre::Entity*>& entities,
                                              double* zeroAreaOut, double* degenerateUvOut)
{
    // Editor-space area threshold. PS1 verts land at ×0.01 magnitude, so a
    // triangle smaller than 1e-7 units² is a sliver. UV space is [0,1], so a
    // 1e-8 UV-area threshold flags fully-collapsed UV triangles.
    constexpr double kAreaEps = 1.0e-7;
    constexpr double kUvAreaEps = 1.0e-8;

    long long triTotal = 0;
    long long zeroArea = 0;
    long long uvTriTotal = 0;
    long long degenerateUv = 0;
    bool sawTris = false;

    for (const Ogre::Entity* entity : entities) {
        if (!entity) continue;
        const Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh) continue;

        for (unsigned int s = 0; s < mesh->getNumSubMeshes(); ++s) {
            const Ogre::SubMesh* sub = mesh->getSubMesh(s);
            if (!sub) continue;
            const std::vector<uint32_t> idx = readSubmeshIndexBuffer(sub);
            if (idx.empty()) continue;
            const Ogre::VertexData* vd = sub->useSharedVertices
                ? mesh->sharedVertexData
                : sub->vertexData;
            if (!vd || !vd->vertexDeclaration) continue;

            const Ogre::VertexElement* posElem =
                vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
            if (!posElem) continue;
            const Ogre::VertexElement* uvElem =
                vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES, 0);

            const Ogre::HardwareVertexBufferSharedPtr posBuf =
                vd->vertexBufferBinding->getBuffer(posElem->getSource());
            if (!posBuf) continue;
            const auto* posBase = static_cast<const unsigned char*>(
                posBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            const size_t posStride = posBuf->getVertexSize();

            const unsigned char* uvBase = nullptr;
            size_t uvStride = 0;
            Ogre::HardwareVertexBufferSharedPtr uvBuf;
            if (uvElem) {
                uvBuf = vd->vertexBufferBinding->getBuffer(uvElem->getSource());
                if (uvBuf) {
                    // Same buffer as position → already locked; reuse the map.
                    if (uvBuf == posBuf) {
                        uvBase = posBase;
                    } else {
                        uvBase = static_cast<const unsigned char*>(
                            uvBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                    }
                    uvStride = uvBuf->getVertexSize();
                }
            }

            sawTris = true;
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                float p[3][3];
                for (int k = 0; k < 3; ++k) {
                    const unsigned char* vp = posBase + idx[t + k] * posStride;
                    float* fp;
                    posElem->baseVertexPointerToElement(const_cast<unsigned char*>(vp), &fp);
                    p[k][0] = fp[0]; p[k][1] = fp[1]; p[k][2] = fp[2];
                }
                const double ux = p[1][0]-p[0][0], uy = p[1][1]-p[0][1], uz = p[1][2]-p[0][2];
                const double vx = p[2][0]-p[0][0], vy = p[2][1]-p[0][1], vz = p[2][2]-p[0][2];
                const double cxp = uy*vz - uz*vy, cyp = uz*vx - ux*vz, czp = ux*vy - uy*vx;
                const double area = 0.5 * std::sqrt(cxp*cxp + cyp*cyp + czp*czp);
                ++triTotal;
                if (area <= kAreaEps) ++zeroArea;

                if (uvBase) {
                    float uv[3][2];
                    for (int k = 0; k < 3; ++k) {
                        const unsigned char* vp = uvBase + idx[t + k] * uvStride;
                        float* fp;
                        uvElem->baseVertexPointerToElement(const_cast<unsigned char*>(vp), &fp);
                        uv[k][0] = fp[0]; uv[k][1] = fp[1];
                    }
                    const double uvArea = 0.5 * std::fabs(
                        (uv[1][0]-uv[0][0]) * (uv[2][1]-uv[0][1])
                        - (uv[2][0]-uv[0][0]) * (uv[1][1]-uv[0][1]));
                    ++uvTriTotal;
                    if (uvArea <= kUvAreaEps) ++degenerateUv;
                }
            }

            if (uvBuf && uvBuf != posBuf && uvBase)
                uvBuf->unlock();
            posBuf->unlock();
        }
    }

    if (zeroAreaOut)
        *zeroAreaOut = (sawTris && triTotal > 0)
            ? static_cast<double>(zeroArea) / static_cast<double>(triTotal) : -1.0;
    if (degenerateUvOut)
        *degenerateUvOut = (uvTriTotal > 0)
            ? static_cast<double>(degenerateUv) / static_cast<double>(uvTriTotal) : -1.0;
}

// Detect embedded textures: walk each TextureUnitState used by every
// SubEntity's Material and ask Ogre if the texture was manually loaded
// (i.e. fed bytes from an in-memory stream rather than resolved through
// resource locations). MaterialProcessor::loadTexture calls loadImage()
// for embedded textures, which sets isManuallyLoaded()==true.
static bool detectEmbeddedTexturesFromEntities(const QList<Ogre::Entity*>& entities)
{
    auto& tmgr = Ogre::TextureManager::getSingleton();
    for (const Ogre::Entity* entity : entities) {
        if (!entity) continue;
        for (unsigned int i = 0; i < entity->getNumSubEntities(); ++i) {
            const auto* sub = entity->getSubEntity(i);
            const auto mat = sub ? sub->getMaterial() : Ogre::MaterialPtr();
            if (!mat) continue;
            for (auto* tech : mat->getTechniques()) {
                for (auto* pass : tech->getPasses()) {
                    for (auto* tus : pass->getTextureUnitStates()) {
                        if (tus->getContentType() != Ogre::TextureUnitState::CONTENT_NAMED)
                            continue;
                        const Ogre::String texName = tus->getTextureName();
                        if (texName.empty()) continue;
                        const Ogre::TexturePtr tex = tmgr.getByName(texName,
                            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
                        if (tex && tex->isManuallyLoaded())
                            return true;
                    }
                }
            }
        }
    }
    return false;
}

// Slice C3 single-pass scan inspector. Loads the asset through the editor's
// MeshImporterExporter, walks the resulting Ogre scene, and fills every
// AssetInfo field the scan rules consume — counts, materials, textures,
// skeleton, animations, redundant-keyframe analysis, embedded-texture flag,
// and ACMR — using CLIPipeline::extractMeshInfo (the same extractor the
// in-app MeshInfoOverlay uses) plus AnimationMerger::analyzeRedundantKeyframes
// (the same analyzer powering `qtmesh anim --simplify` and the Inspector's
// "Simplify" action). Returns true on successful import.
//
// Replaces both the Assimp aiScene traversal that previously filled metadata
// AND the separate computeWeightedAcmrViaOgre import — we load each file once.
// Enumerate every aiMaterial's texture references via a cheap Assimp
// ReadFile (no triangulation, no expensive post-processing). Restores the
// pre-C3 behavior of `info.texturePaths` capturing referenced-but-missing
// texture files — Ogre's TextureUnitState only sees textures that bound
// successfully, so without this pass require_textures_exist would have
// nothing to flag. Native .mesh files don't have an Assimp reader for
// v1.40, and their texture refs already came from the .material script via
// Ogre, so they short-circuit here.
static void enumerateTextureRefsViaAssimp(const QString& filePath, AssetInfo& info,
                                          std::set<std::string>& seenTextures)
{
    if (filePath.endsWith(QLatin1String(".mesh"), Qt::CaseInsensitive))
        return;
    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* scene = imp.ReadFile(filePath.toStdString(),
                                        aiProcess_ValidateDataStructure);
    if (!scene || ScanEngine::isAssimpResultLoadFailure(scene, imp.GetErrorString(), nullptr))
        return;

    for (unsigned m = 0; m < scene->mNumMaterials; ++m) {
        const aiMaterial* mat = scene->mMaterials[m];
        if (!mat) continue;
        for (int type = aiTextureType_DIFFUSE; type <= aiTextureType_UNKNOWN; ++type) {
            const unsigned count = mat->GetTextureCount(static_cast<aiTextureType>(type));
            for (unsigned j = 0; j < count; ++j) {
                aiString p;
                mat->GetTexture(static_cast<aiTextureType>(type), j, &p);
                const QString tp = QString::fromUtf8(p.C_Str());
                if (tp.isEmpty()) continue;
                if (tp.startsWith('*')) {
                    info.hasEmbeddedTextures = true;
                    continue;
                }
                if (seenTextures.insert(tp.toStdString()).second) {
                    info.texturePaths.append(tp);
                    info.textureRefCount++;
                }
            }
        }
    }
    // aiScene-level embedded texture blobs (FBX/glTF) — flag even when no
    // material referenced them via a '*' path.
    if (scene->mNumTextures > 0)
        info.hasEmbeddedTextures = true;
}

// Fallback: when the Ogre import fails (e.g. an OBJ that references a missing
// texture file — MaterialProcessor throws on TextureManager::load), still
// produce useful counts by walking the aiScene directly. The scan can then
// flag the missing texture via require_textures_exist instead of bailing
// with a load_error on the whole file.
static bool fillAssetInfoFromAssimpFallback(const QString& filePath, AssetInfo& info)
{
    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* scene = imp.ReadFile(filePath.toStdString(),
                                        aiProcess_Triangulate | aiProcess_ValidateDataStructure);
    if (!scene || ScanEngine::isAssimpResultLoadFailure(scene, imp.GetErrorString(), nullptr))
        return false;

    info.meshCount      = scene->mNumMeshes;
    info.materialCount  = scene->mNumMaterials;
    info.animationCount = scene->mNumAnimations;

    std::set<std::string> bones;
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* m = scene->mMeshes[i];
        if (!m) continue;
        info.vertexCount += m->mNumVertices;
        info.faceCount   += m->mNumFaces;
        for (unsigned b = 0; b < m->mNumBones; ++b)
            if (m->mBones[b])
                bones.insert(m->mBones[b]->mName.C_Str());
    }
    info.boneCount   = static_cast<unsigned int>(bones.size());
    info.hasSkeleton = !bones.empty();
    for (const auto& bn : bones)
        info.boneNames.append(QString::fromStdString(bn));

    for (unsigned i = 0; i < scene->mNumMaterials; ++i) {
        const aiMaterial* mat = scene->mMaterials[i];
        if (!mat) continue;
        aiString n;
        mat->Get(AI_MATKEY_NAME, n);
        info.materialNames.append(QString::fromUtf8(n.C_Str()));
    }

    fillMeshStatsFromAssimpScene(scene, info);
    syncTriangleDerivedFields(info);
    return true;
}

static bool inspectAssetViaOgre(const QString& filePath, AssetInfo& info,
                                const AnimationMerger::SimplifyTolerances& redundancyTol)
{
    if (!ensureOgreHeadlessQuiet()) {
        info.loadError = true;
        info.errorMessage = QStringLiteral("Ogre headless context unavailable");
        return false;
    }

    clearOgreSceneForScanImport();
    // Default flags (0) match File > Open: same index ordering and same
    // material/texture binding the user actually ships. MaterialProcessor
    // can throw if a referenced texture file is missing on disk; swallow
    // that so the scan still produces metadata + can flag the missing
    // texture via require_textures_exist (instead of dropping the asset
    // entirely with a load_error).
    try {
        MeshImporterExporter::importer({QFileInfo(filePath).absoluteFilePath()}, 0);
    } catch (const Ogre::Exception&) {
        // Continue — try to extract whatever entities did make it in.
    } catch (const std::exception&) {
        // Same.
    }

    auto* mgr = Manager::getSingleton();
    if (!mgr) {
        clearOgreSceneForScanImport();
        info.loadError = true;
        info.errorMessage = QStringLiteral("Manager unavailable");
        return false;
    }
    QList<Ogre::Entity*> entities = mgr->getEntities();
    if (entities.isEmpty()) {
        // Partial / failed Ogre import (e.g. OBJ with a missing texture
        // reference that aborted MaterialProcessor). Fall back to Assimp's
        // scene graph for at least the count metadata so the rule evaluator
        // can still flag what's wrong instead of dropping the file.
        clearOgreSceneForScanImport();
        const bool ok = fillAssetInfoFromAssimpFallback(filePath, info);
        std::set<std::string> seenTextures;
        enumerateTextureRefsViaAssimp(filePath, info, seenTextures);
        return ok;
    }

    std::set<std::string> seenMaterials, seenBones, seenAnims, seenTextures;
    double weightedAcmrSum = 0.0;
    unsigned int totalTris = 0;

    for (const Ogre::Entity* entity : entities) {
        mergeOgreEntityIntoAssetInfo(entity, info,
                                     seenMaterials, seenBones, seenAnims, seenTextures);
        accumulateEntityAcmr(entity, weightedAcmrSum, totalTris);
    }

    if (totalTris > 0)
        info.weightedAcmr = weightedAcmrSum / totalTris;

    // Redundant-keyframe analysis — use the editor's analyzer so scan numbers
    // match what `qtmesh anim --simplify` and the Inspector's Simplify button
    // would actually remove at the configured tolerances.
    std::set<std::string> seenAnimsForAnalysis;
    for (const Ogre::Entity* entity : entities)
        fillRedundancyFromOgreSkeleton(entity, info, redundancyTol, seenAnimsForAnalysis);

    info.animationRedundantKeyframeRatio = (info.totalKeyframes > 0)
        ? static_cast<double>(info.redundantKeyframes) / static_cast<double>(info.totalKeyframes)
        : 0.0;

    info.hasEmbeddedTextures = detectEmbeddedTexturesFromEntities(entities);

    enumerateTextureRefsViaAssimp(filePath, info, seenTextures);

    // C4 quality data — collected unconditionally so the JSON report carries
    // it for downstream tooling. The rule evaluator only emits findings when
    // the corresponding rule is enabled, so leaving these populated for free
    // doesn't trigger noise.
    info.maxTextureDimension  = maxTextureDimensionForEntities(entities);
    info.minUvChannelCount    = minUvChannelCountForEntities(entities);
    info.zeroWeightBoneNames  = zeroWeightBonesForEntities(entities);
    info.overlappingUvsRatio  = overlappingUvsRatioForEntities(entities);
    info.nonManifoldEdgesRatio = nonManifoldEdgesRatioForEntities(entities);
    ps1RipDegeneracyRatiosForEntities(entities, &info.ps1RipZeroAreaRatio,
                                      &info.ps1RipDegenerateUvRatio);

    fillMeshStatsFromOgreEntities(entities, info);
    syncTriangleDerivedFields(info);

    clearOgreSceneForScanImport();
    return true;
}

template<typename ImportFn>
static bool loadAndFillOgreInspect(AssetInfo& info, ImportFn&& importFn, QString* detailErr)
{
    QString ogreErr;
    if (!ensureOgreMaterialsForScanInspect(&ogreErr)) {
        info.loadError = true;
        info.errorMessage = ogreErr;
        if (detailErr)
            *detailErr = ogreErr;
        return false;
    }
    const std::string meshName = std::string("_qtmesh_scan_") + std::to_string(nextScanInspectMeshId());
    Ogre::MeshPtr mesh = importFn(meshName);
    if (!mesh) {
        info.loadError = true;
        info.errorMessage = QStringLiteral("Could not import mesh geometry");
        if (detailErr)
            *detailErr = info.errorMessage;
        return false;
    }
    fillAssetInfoFromOgreMesh(info, mesh);
    try {
        Ogre::MeshManager::getSingleton().remove(meshName,
                                                 Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    } catch (...) {
    }
    return true;
}

// ---------------------------------------------------------------------------
// Glob matching
// ---------------------------------------------------------------------------

bool ScanEngine::matchesGlob(const QString& path, const QString& pattern)
{
    // Convert a simplified glob to a QRegularExpression:
    //   **/  → matches zero or more directory components
    //   **   → matches anything (including /)
    //   *    → matches anything except /
    //   ?    → matches single char except /
    QString escaped = QRegularExpression::escape(pattern);
    // Qt 6.9+ escapes '/' to '\/' — handle both escaped and unescaped slashes.
    // Must process **/ before ** before * to avoid partial matches.
    escaped.replace("\\/", "/");            // normalize: \/ → /
    escaped.replace("\\*\\*/", "(.+/)?");  // **/ → zero or more dirs
    escaped.replace("\\*\\*", ".*");       // **  → anything
    escaped.replace("\\*", "[^/]*");       // *   → anything except /
    escaped.replace("\\?", "[^/]");        // ?   → single non-/
    QRegularExpression re("^" + escaped + "$",
                          QRegularExpression::CaseInsensitiveOption);
    return re.match(path).hasMatch();
}

bool ScanEngine::matchesWildcard(const QString& text, const QString& pattern)
{
    // Simple wildcard matching for names (not paths): * = anything, ? = one char
    QString escaped = QRegularExpression::escape(pattern);
    escaped.replace("\\/", "/");
    escaped.replace("\\*", ".*");
    escaped.replace("\\?", ".");
    QRegularExpression re("^" + escaped + "$",
                          QRegularExpression::CaseInsensitiveOption);
    return re.match(text).hasMatch();
}

// ---------------------------------------------------------------------------
// File enumeration
// ---------------------------------------------------------------------------

QStringList ScanEngine::enumerateFiles(const ScanConfig& config, const QString& scanRoot)
{
    QStringList result;
    QDir rootDir(scanRoot);
    if (!rootDir.exists()) return result;

    QStringList nameFilters;
    bool useNameFilters = !config.includePatterns.isEmpty();
    static const QRegularExpression simpleExtRe(QStringLiteral(R"(^\*\*/\*\.([a-zA-Z0-9]+)$)"));
    for (const auto& pattern : config.includePatterns) {
        const auto m = simpleExtRe.match(pattern);
        if (!m.hasMatch()) {
            useNameFilters = false;
            break;
        }
        nameFilters << QStringLiteral("*.%1").arg(m.captured(1));
    }
    if (useNameFilters && nameFilters.isEmpty())
        useNameFilters = false;

    QDirIterator it(rootDir.absolutePath(),
                    useNameFilters ? nameFilters : QStringList(),
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    while (it.hasNext()) {
        it.next();
        QString absPath = it.filePath();
        QString relPath = rootDir.relativeFilePath(absPath);

        // Check excludes first
        bool excluded = false;
        for (const auto& pattern : config.excludePatterns) {
            if (matchesGlob(relPath, pattern)) { excluded = true; break; }
        }
        if (excluded) continue;

        // Check includes (if empty, accept everything)
        if (!config.includePatterns.isEmpty()) {
            bool included = false;
            for (const auto& pattern : config.includePatterns) {
                if (matchesGlob(relPath, pattern)) { included = true; break; }
            }
            if (!included) continue;
        }

        result.append(absPath);
    }

    result.sort(Qt::CaseInsensitive);
    return result;
}

// ---------------------------------------------------------------------------
// Asset inspection (slice C3): every supported format flows through
// MeshImporterExporter (Ogre) and the resulting Ogre scene is walked with
// CLIPipeline::extractMeshInfo and AnimationMerger::analyzeRedundantKeyframes
// — the same extractors the in-app MeshInfoOverlay and the "Simplify" action
// use. A single headless Ogre context is shared across files; per-file
// cleanup happens in clearOgreSceneForScanImport (which also flushes
// MeshManager / SkeletonManager so a 1000-asset scan doesn't pile state).
// Assimp remains only as a recognized "did this file actually load?" check
// for formats whose detailed metadata is still useful before deciding to
// load through Ogre at all.
// ---------------------------------------------------------------------------

bool ScanEngine::isAssimpResultLoadFailure(const aiScene* scene, const char* assimpErrorString,
                                            QString* outErrorMessage)
{
    if (!scene) {
        QString e = assimpErrorString ? QString::fromUtf8(assimpErrorString) : QString();
        if (e.isEmpty())
            e = QStringLiteral("Assimp failed to read file");
        if (outErrorMessage)
            *outErrorMessage = e;
        return true;
    }
    if (scene->mNumMeshes == 0 && !scene->HasAnimations()) {
        QString e = assimpErrorString ? QString::fromUtf8(assimpErrorString) : QString();
        if (e.isEmpty())
            e = QStringLiteral("Scene has no meshes and no animations");
        if (outErrorMessage)
            *outErrorMessage = e;
        return true;
    }
    return false;
}

AssetInfo ScanEngine::inspectAsset(const QString& filePath, const QString& scanRoot,
                                   const AssetInspectOptions& options)
{
    AssetInfo info;
    info.filePath = filePath;
    info.relativePath = QDir(scanRoot).relativeFilePath(filePath);
    info.format = QFileInfo(filePath).suffix().toLower();
    info.fileSize = QFileInfo(filePath).size();

    const QString extLower = info.format;

    if (extLower == QLatin1String("rsd")) {
        PS1RSD::RsdDescriptor d;
        QString rsdErr;
        if (!PS1RSD::parseRsdFile(filePath, d, &rsdErr)) {
            info.loadError = true;
            info.errorMessage = rsdErr;
            return info;
        }
        const QFileInfo fiRsd(filePath);
        const QString rsdDir = fiRsd.absolutePath();
        auto resolveGeom = [rsdDir](const QString& rel) -> QString {
            if (rel.isEmpty())
                return {};
            const QFileInfo r(rel);
            return r.isAbsolute() ? r.absoluteFilePath() : QDir(rsdDir).filePath(rel);
        };
        const QString geomPath = resolveGeom(d.plyPath);
        if (geomPath.isEmpty() || !QFileInfo::exists(geomPath)) {
            info.loadError = true;
            info.errorMessage = QStringLiteral("RSD does not reference an existing geometry file (PLY=...)");
            return info;
        }
        const QFileInfo gfi(geomPath);
        const QString gext = gfi.suffix().toLower();
        QString detailErr;
        if (gext == QLatin1String("tmd")) {
            loadAndFillOgreInspect(
                info, [geomPath](const std::string& mn) { return PS1TMD::importTmd(geomPath, mn); }, &detailErr);
            finalizeAssetInspect(info, options);
            return info;
        }
        if (gext == QLatin1String("ply") && PS1PLY::isPsyqPlyFile(geomPath)) {
            loadAndFillOgreInspect(
                info, [geomPath](const std::string& mn) { return PS1PLY::importPsyqPly(geomPath, mn); }, &detailErr);
            finalizeAssetInspect(info, options);
            return info;
        }
        if (gext == QLatin1String("rsd")) {
            info.loadError = true;
            info.errorMessage = QStringLiteral("RSD references another RSD as geometry (not supported for scan)");
            return info;
        }
        AssetInfo inner = ScanEngine::inspectAsset(geomPath, QFileInfo(geomPath).absolutePath(), options);
        info.loadError = inner.loadError;
        info.errorMessage = inner.errorMessage;
        info.meshCount = inner.meshCount;
        info.materialCount = inner.materialCount;
        info.animationCount = inner.animationCount;
        info.vertexCount = inner.vertexCount;
        info.faceCount = inner.faceCount;
        info.boneCount = inner.boneCount;
        info.textureRefCount = inner.textureRefCount;
        info.hasSkeleton = inner.hasSkeleton;
        info.hasEmbeddedTextures = inner.hasEmbeddedTextures;
        info.materialNames = inner.materialNames;
        info.texturePaths = inner.texturePaths;
        info.animationNames = inner.animationNames;
        info.animationDurations = inner.animationDurations;
        info.animationKeyframeCounts = inner.animationKeyframeCounts;
        info.boneNames = inner.boneNames;
        info.animationRedundantKeyframeRatio = inner.animationRedundantKeyframeRatio;
        info.totalKeyframes = inner.totalKeyframes;
        info.redundantKeyframes = inner.redundantKeyframes;
        // Slice C4 metric forwarding — without these copies, .rsd assets
        // silently skip every C4 rule because their AssetInfo loses the
        // collector output from the inner inspect call.
        info.maxTextureDimension = inner.maxTextureDimension;
        info.minUvChannelCount = inner.minUvChannelCount;
        info.zeroWeightBoneNames = inner.zeroWeightBoneNames;
        info.overlappingUvsRatio = inner.overlappingUvsRatio;
        info.nonManifoldEdgesRatio = inner.nonManifoldEdgesRatio;
        info.ps1RipZeroAreaRatio = inner.ps1RipZeroAreaRatio;
        info.ps1RipDegenerateUvRatio = inner.ps1RipDegenerateUvRatio;
        copyAssetInspectExtensionFields(info, inner);
        info.filePath = filePath;
        info.relativePath = QDir(scanRoot).relativeFilePath(filePath);
        info.format = extLower;
        info.fileSize = QFileInfo(filePath).size();
        return info;
    }

    if (extLower == QLatin1String("tmd")) {
        QString detailErr;
        loadAndFillOgreInspect(
            info, [filePath](const std::string& mn) { return PS1TMD::importTmd(filePath, mn); }, &detailErr);
        finalizeAssetInspect(info, options);
        return info;
    }

    if (extLower == QLatin1String("ply") && PS1PLY::isPsyqPlyFile(filePath)) {
        QString detailErr;
        loadAndFillOgreInspect(
            info, [filePath](const std::string& mn) { return PS1PLY::importPsyqPly(filePath, mn); }, &detailErr);
        finalizeAssetInspect(info, options);
        return info;
    }

    // Slice C3: single Ogre-backed pass. inspectAssetViaOgre loads the asset
    // through MeshImporterExporter (the editor's own loader) and walks the
    // resulting Ogre scene to fill EVERY AssetInfo field — counts, materials,
    // textures, skeleton, animations, redundant-keyframe analysis, embedded
    // flag, and ACMR — using the same extractors the in-app MeshInfoOverlay
    // (CLIPipeline::extractMeshInfo) and "Simplify" action
    // (AnimationMerger::analyzeRedundantKeyframes) use.
    //
    // Default tolerances are sufficient for the JSON report; the
    // redundant_keyframes_pct rule re-imports with config-specific tolerances
    // when the rule is enabled.
    if (!inspectAssetViaOgre(filePath, info, AnimationMerger::SimplifyTolerances{})) {
        if (!info.loadError) {
            info.loadError = true;
            info.errorMessage = QStringLiteral("Failed to load asset via Ogre");
        }
        return info;
    }

    finalizeAssetInspect(info, options);
    return info;
}

// ---------------------------------------------------------------------------
// Name case helpers
// ---------------------------------------------------------------------------

bool ScanEngine::checkNameCase(const QString& fileName, const QString& convention)
{
    QString stem = QFileInfo(fileName).completeBaseName();
    if (stem.isEmpty()) return true;

    if (convention == "snake_case") {
        static QRegularExpression re("^[a-z0-9]+(_[a-z0-9]+)*$");
        return re.match(stem).hasMatch();
    }
    if (convention == "kebab-case") {
        static QRegularExpression re("^[a-z0-9]+(-[a-z0-9]+)*$");
        return re.match(stem).hasMatch();
    }
    if (convention == "camelCase") {
        static QRegularExpression re("^[a-z][a-zA-Z0-9]*$");
        return re.match(stem).hasMatch();
    }
    if (convention == "PascalCase") {
        static QRegularExpression re("^[A-Z][a-zA-Z0-9]*$");
        return re.match(stem).hasMatch();
    }
    if (convention == "lowercase") {
        return stem == stem.toLower();
    }

    return true; // unknown convention → pass
}

QString ScanEngine::convertNameToCase(const QString& fileName, const QString& convention)
{
    QString stem = QFileInfo(fileName).completeBaseName();
    QString ext  = QFileInfo(fileName).suffix();

    if (convention == "snake_case") {
        QString result;
        for (int i = 0; i < stem.size(); ++i) {
            QChar c = stem[i];
            if (c.isUpper() && i > 0 && stem[i - 1] != '_' && !stem[i - 1].isUpper())
                result += '_';
            result += c.toLower();
        }
        result.replace('-', '_').replace(' ', '_');
        // Collapse runs of underscores
        static QRegularExpression multiUnderscore("_+");
        result.replace(multiUnderscore, "_");
        return result + "." + ext;
    }
    if (convention == "kebab-case") {
        QString result;
        for (int i = 0; i < stem.size(); ++i) {
            QChar c = stem[i];
            if (c.isUpper() && i > 0 && stem[i - 1] != '-' && !stem[i - 1].isUpper())
                result += '-';
            result += c.toLower();
        }
        result.replace('_', '-').replace(' ', '-');
        static QRegularExpression multiDash("-+");
        result.replace(multiDash, "-");
        return result + "." + ext;
    }
    if (convention == "lowercase") {
        return stem.toLower() + "." + ext;
    }

    return fileName; // no conversion for camelCase/PascalCase (ambiguous from arbitrary input)
}

// ---------------------------------------------------------------------------
// Rule evaluation
// ---------------------------------------------------------------------------

namespace {

bool isPowerOfTwoDimension(int value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

int effectiveTextureMaxDimension(const AssetInfo& asset)
{
    return std::max(asset.maxTextureDimension, asset.probedTextureMaxDimension);
}

bool textureFormatMatchesList(const QString& format, const QStringList& patterns)
{
    if (format.isEmpty())
        return false;
    for (const QString& pattern : patterns) {
        if (pattern.isEmpty())
            continue;
        if (format.compare(pattern, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

} // namespace

QList<Finding> ScanEngine::evaluateRules(const AssetInfo& asset, const ScanConfig& globalConfig)
{
    // Apply scoped rule overrides for this asset's path
    ScanConfig config = globalConfig.withScopeOverrides(asset.relativePath);

    QList<Finding> findings;

    if (asset.loadError) {
        findings.append({asset.relativePath, "load_error", Severity::Error,
                         QString("Failed to load: %1").arg(asset.errorMessage)});
        return findings;
    }

    // ---- allowed_formats ----
    if (!config.allowedFormats.isEmpty()) {
        bool allowed = false;
        for (const auto& fmt : config.allowedFormats) {
            if (asset.format.compare(fmt, Qt::CaseInsensitive) == 0) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            findings.append({asset.relativePath, "allowed_formats", Severity::Error,
                             QString(".%1 is not an allowed format (allowed: %2)")
                                 .arg(asset.format, config.allowedFormats.join(", "))});
        }
    }

    // ---- forbidden_extensions ----
    for (const auto& ext : config.forbiddenExtensions) {
        if (asset.format.compare(ext, Qt::CaseInsensitive) == 0) {
            findings.append({asset.relativePath, "forbidden_extensions", Severity::Error,
                             QString(".%1 is a forbidden format").arg(asset.format)});
            break;
        }
    }

    // ---- max/min_file_size_mb ----
    {
        double sizeMb = asset.fileSize / (1024.0 * 1024.0);
        if (config.maxFileSizeMb > 0 && sizeMb > config.maxFileSizeMb)
            findings.append({asset.relativePath, "max_file_size_mb", Severity::Error,
                             QString("%.2f MB exceeds maximum of %.1f MB")
                                 .arg(sizeMb).arg(config.maxFileSizeMb)});
        if (config.minFileSizeMb > 0 && sizeMb < config.minFileSizeMb)
            findings.append({asset.relativePath, "min_file_size_mb", Severity::Warning,
                             QString("%.2f MB is below minimum of %.2f MB")
                                 .arg(sizeMb).arg(config.minFileSizeMb)});
    }

    // ---- max/min_mesh_count ----
    if (config.maxMeshCount > 0 && static_cast<int>(asset.meshCount) > config.maxMeshCount)
        findings.append({asset.relativePath, "max_mesh_count", Severity::Error,
                         QString("%1 meshes exceeds limit of %2")
                             .arg(asset.meshCount).arg(config.maxMeshCount)});
    if (config.minMeshCount > 0 && static_cast<int>(asset.meshCount) < config.minMeshCount)
        findings.append({asset.relativePath, "min_mesh_count", Severity::Warning,
                         QString("%1 meshes is below minimum of %2")
                             .arg(asset.meshCount).arg(config.minMeshCount)});

    // ---- max/min_material_count ----
    if (config.maxMaterialCount > 0 && static_cast<int>(asset.materialCount) > config.maxMaterialCount)
        findings.append({asset.relativePath, "max_material_count", Severity::Error,
                         QString("%1 materials exceeds limit of %2")
                             .arg(asset.materialCount).arg(config.maxMaterialCount)});
    if (config.minMaterialCount > 0 && static_cast<int>(asset.materialCount) < config.minMaterialCount)
        findings.append({asset.relativePath, "min_material_count", Severity::Warning,
                         QString("%1 materials is below minimum of %2")
                             .arg(asset.materialCount).arg(config.minMaterialCount)});

    // ---- max/min_vertex_count ----
    if (config.maxVertexCount > 0 && static_cast<int>(asset.vertexCount) > config.maxVertexCount)
        findings.append({asset.relativePath, "max_vertex_count", Severity::Error,
                         QString("%1 vertices exceeds limit of %2")
                             .arg(asset.vertexCount).arg(config.maxVertexCount)});
    if (config.minVertexCount > 0 && static_cast<int>(asset.vertexCount) < config.minVertexCount)
        findings.append({asset.relativePath, "min_vertex_count", Severity::Warning,
                         QString("%1 vertices is below minimum of %2")
                             .arg(asset.vertexCount).arg(config.minVertexCount)});

    // ---- max_triangle_count / max_triangles_per_mesh ---- (issue #365)
    if (config.maxTriangleCount > 0
        && static_cast<int>(asset.triangleCount) > config.maxTriangleCount) {
        findings.append({asset.relativePath, "max_triangle_count", Severity::Error,
                         QString("%1 triangles exceeds limit of %2")
                             .arg(asset.triangleCount).arg(config.maxTriangleCount)});
    }
    if (config.maxTrianglesPerMesh > 0
        && static_cast<int>(asset.maxTrianglesPerMesh) > config.maxTrianglesPerMesh) {
        findings.append({asset.relativePath, "max_triangles_per_mesh", Severity::Error,
                         QString("Largest mesh has %1 triangles (limit %2)")
                             .arg(asset.maxTrianglesPerMesh).arg(config.maxTrianglesPerMesh)});
    }

    // ---- max_bones ----
    if (config.maxBoneCount > 0 && static_cast<int>(asset.boneCount) > config.maxBoneCount) {
        findings.append({asset.relativePath, "max_bones", Severity::Error,
                         QString("%1 bones exceeds limit of %2")
                             .arg(asset.boneCount).arg(config.maxBoneCount)});
    }

    // ---- max_submesh_count / max_draw_calls ---- (draw-call proxies)
    if (config.maxSubmeshCount > 0
        && static_cast<int>(asset.submeshCount) > config.maxSubmeshCount) {
        findings.append({asset.relativePath, "max_submesh_count", Severity::Warning,
                         QString("%1 submeshes exceeds limit of %2 — merge materials or "
                                 "combine meshes to cut draw calls")
                             .arg(asset.submeshCount).arg(config.maxSubmeshCount)});
    }
    if (config.maxDrawCalls > 0
        && static_cast<int>(asset.estimatedDrawCalls) > config.maxDrawCalls) {
        findings.append({asset.relativePath, "max_draw_calls", Severity::Warning,
                         QString("Estimated %1 draw calls exceeds limit of %2")
                             .arg(asset.estimatedDrawCalls).arg(config.maxDrawCalls)});
    }

    // ---- max_acmr ---- (Phase 6 slice C / C2)
    //
    // ACMR is measured on the same Ogre index buffer the editor's in-app
    // validator sees, so scan-side numbers match in-editor numbers
    // one-for-one. A typical reorder-friendly ceiling is around 1.0; assets
    // above that benefit from `qtmesh vertex-cache -o <out>`.
    if (config.maxAcmr > 0.0 && asset.weightedAcmr > config.maxAcmr) {
        findings.append({asset.relativePath, "max_acmr", Severity::Warning,
                         QString("ACMR %1 exceeds limit of %2 — reorder index buffer for "
                                 "GPU vertex cache (qtmesh vertex-cache -o <out>)")
                             .arg(QString::number(asset.weightedAcmr, 'f', 3))
                             .arg(QString::number(config.maxAcmr, 'f', 3))});
    }

    // ---- require_skeleton ----
    if (config.requireSkeleton && !asset.hasSkeleton) {
        findings.append({asset.relativePath, "require_skeleton", Severity::Error,
                         "No skeleton found (require_skeleton is enabled)"});
    }

    // ---- require_animations ----
    if (config.requireAnimations && asset.animationCount == 0) {
        findings.append({asset.relativePath, "require_animations", Severity::Error,
                         "No animations found (require_animations is enabled)"});
    }

    // ---- allow_embedded_textures ----
    if (!config.allowEmbeddedTextures && asset.hasEmbeddedTextures) {
        findings.append({asset.relativePath, "allow_embedded_textures", Severity::Warning,
                         "Asset contains embedded textures (allow_embedded_textures is false)"});
    }

    // ---- require_textures_exist ----
    if (config.requireTexturesExist) {
        QDir assetDir = QFileInfo(asset.filePath).dir();
        for (const auto& texPath : asset.texturePaths) {
            // Try relative to asset directory
            if (!QFileInfo::exists(assetDir.filePath(texPath))) {
                findings.append({asset.relativePath, "require_textures_exist", Severity::Warning,
                                 QString("Referenced texture not found: %1").arg(texPath)});
            }
        }
    }

    // ---- max_texture_resolution / max_texture_dimension ---- (Phase 6 slice C4 + #365)
    if (config.maxTextureResolution > 0) {
        const int texMax = effectiveTextureMaxDimension(asset);
        if (texMax > config.maxTextureResolution) {
            findings.append({asset.relativePath, "max_texture_resolution", Severity::Warning,
                QString("Largest texture is %1px on its longest edge — exceeds limit of %2px. "
                        "Downscale or pack with `qtmesh pack-textures` to reduce VRAM cost.")
                    .arg(texMax)
                    .arg(config.maxTextureResolution)});
        }
    }

    // ---- texture_not_power_of_two / texture format allow-list ---- (#365)
    // Requires inspect_textures / probeTextureFiles so textureStats carry dimensions.
    if (!asset.textureStats.isEmpty()) {
        if (config.requireTexturePowerOfTwo) {
            QStringList offenders;
            for (const TextureRefStats& ts : asset.textureStats) {
                if (ts.missing || ts.width <= 0 || ts.height <= 0)
                    continue;
                if (!isPowerOfTwoDimension(ts.width) || !isPowerOfTwoDimension(ts.height))
                    offenders.append(QStringLiteral("%1 (%2×%3)")
                                         .arg(ts.path)
                                         .arg(ts.width)
                                         .arg(ts.height));
            }
            if (!offenders.isEmpty()) {
                const QStringList sample = offenders.mid(0, 3);
                const QString suffix = offenders.size() > sample.size()
                    ? QStringLiteral(" …+%1 more").arg(offenders.size() - sample.size())
                    : QString();
                findings.append({asset.relativePath, "texture_not_power_of_two", Severity::Warning,
                    QString("Non power-of-two texture dimensions: %1%2. "
                            "Resize to POT (e.g. 256, 512) for older GPU paths.")
                        .arg(sample.join(QStringLiteral(", ")))
                        .arg(suffix)});
            }
        }

        if (!config.allowedTextureFormats.isEmpty()) {
            QStringList offenders;
            for (const TextureRefStats& ts : asset.textureStats) {
                if (ts.missing || ts.format.isEmpty())
                    continue;
                if (!textureFormatMatchesList(ts.format, config.allowedTextureFormats))
                    offenders.append(QStringLiteral("%1 (.%2)")
                                         .arg(ts.path, ts.format));
            }
            if (!offenders.isEmpty()) {
                findings.append({asset.relativePath, "texture_format_disallowed", Severity::Warning,
                    QString("Texture format not in allow-list [%1]: %2")
                        .arg(config.allowedTextureFormats.join(QStringLiteral(", ")),
                             offenders.mid(0, 3).join(QStringLiteral(", ")))});
            }
        } else if (!config.disallowedTextureFormats.isEmpty()) {
            QStringList offenders;
            for (const TextureRefStats& ts : asset.textureStats) {
                if (ts.missing || ts.format.isEmpty())
                    continue;
                if (textureFormatMatchesList(ts.format, config.disallowedTextureFormats))
                    offenders.append(QStringLiteral("%1 (.%2)")
                                         .arg(ts.path, ts.format));
            }
            if (!offenders.isEmpty()) {
                findings.append({asset.relativePath, "texture_format_disallowed", Severity::Warning,
                    QString("Disallowed texture format(s): %1")
                        .arg(offenders.mid(0, 3).join(QStringLiteral(", ")))});
            }
        }
    }

    // ---- require_uv_channels ----
    // Skip when the asset has no triangulated geometry at all (e.g.
    // animation-only FBX); the geometry-side rules will already flag it.
    if (config.requireUvChannels > 0
        && asset.faceCount > 0
        && asset.minUvChannelCount < config.requireUvChannels) {
        findings.append({asset.relativePath, "require_uv_channels", Severity::Warning,
            QString("Submesh has only %1 UV set(s); rule requires %2. "
                    "Add a second UV set in your DCC for lightmap or detail-map workflows.")
                .arg(asset.minUvChannelCount)
                .arg(config.requireUvChannels)});
    }

    // ---- detect_zero_weight_bones ----
    if (config.detectZeroWeightBones && !asset.zeroWeightBoneNames.isEmpty()) {
        const int n = asset.zeroWeightBoneNames.size();
        QStringList sample = asset.zeroWeightBoneNames.mid(0, 5);
        QString suffix = (n > sample.size())
            ? QString(", …+%1 more").arg(n - sample.size())
            : QString();
        findings.append({asset.relativePath, "detect_zero_weight_bones", Severity::Info,
            QString("%1 bone(s) have no vertex weights: %2%3. "
                    "Strip unused bones to shrink the skeleton and reduce per-vertex skin math.")
                .arg(n).arg(sample.join(", ")).arg(suffix)});
    }

    // ---- detect_overlapping_uvs_pct ---- (UV0 AABB sweep — upper bound)
    if (config.detectOverlappingUvsPct > 0.0
        && asset.overlappingUvsRatio >= 0.0) {
        const double pct = 100.0 * asset.overlappingUvsRatio;
        if (pct >= config.detectOverlappingUvsPct) {
            findings.append({asset.relativePath, "detect_overlapping_uvs_pct", Severity::Warning,
                QString("%1% of triangles have UV0 AABBs overlapping another — likely "
                        "shared/overlapping UV islands. Bake lightmaps will smear; "
                        "consider a non-overlapping unwrap for UV1.")
                    .arg(pct, 0, 'f', 1)});
        }
    }

    // ---- detect_non_manifold_edges_pct ----
    if (config.detectNonManifoldEdgesPct > 0.0
        && asset.nonManifoldEdgesRatio >= 0.0) {
        const double pct = 100.0 * asset.nonManifoldEdgesRatio;
        if (pct >= config.detectNonManifoldEdgesPct) {
            findings.append({asset.relativePath, "detect_non_manifold_edges_pct", Severity::Warning,
                QString("%1% of edges are non-manifold (shared by != 2 faces). "
                        "Boolean ops, fluid sims and 3D printing expect manifold input — "
                        "weld duplicate verts and cap open boundaries in your DCC.")
                    .arg(pct, 0, 'f', 1)});
        }
    }

    // ---- ps1-rip-zero-area (#428) ----
    if (config.ps1RipZeroAreaPct > 0.0 && asset.ps1RipZeroAreaRatio >= 0.0) {
        const double pct = 100.0 * asset.ps1RipZeroAreaRatio;
        if (pct >= config.ps1RipZeroAreaPct) {
            findings.append({asset.relativePath, "ps1-rip-zero-area", Severity::Warning,
                QString("%1% of triangles are zero-area (collinear / duplicate-vertex "
                        "slivers). Common in raw PS1 captures — run the \"Clean PS1 "
                        "Capture\" pipeline (Remove zero-area triangles) to strip them.")
                    .arg(pct, 0, 'f', 1)});
        }
    }

    // ---- ps1-rip-degenerate-uv (#428) ----
    if (config.ps1RipDegenerateUvPct > 0.0 && asset.ps1RipDegenerateUvRatio >= 0.0) {
        const double pct = 100.0 * asset.ps1RipDegenerateUvRatio;
        if (pct >= config.ps1RipDegenerateUvPct) {
            findings.append({asset.relativePath, "ps1-rip-degenerate-uv", Severity::Info,
                QString("%1% of triangles have a degenerate (zero-area) UV0. Harmless for "
                        "solid-colour PS1 prims, but breaks texture-atlas and lightmap "
                        "workflows — re-unwrap the affected submeshes if you need UVs.")
                    .arg(pct, 0, 'f', 1)});
        }
    }

    // ---- allow_missing_materials ----
    if (!config.allowMissingMaterials) {
        for (const auto& name : asset.materialNames) {
            if (name.isEmpty() || name == "DefaultMaterial" || name == "(null)" ||
                name.startsWith("AI_DEFAULT") || name == "None") {
                findings.append({asset.relativePath, "allow_missing_materials", Severity::Warning,
                                 QString("Placeholder/missing material detected: '%1'").arg(name)});
            }
        }
    }

    // ---- file_name_case ----
    if (!config.fileNameCase.isEmpty()) {
        QString fileName = QFileInfo(asset.filePath).fileName();
        if (!checkNameCase(fileName, config.fileNameCase)) {
            QString suggestion = convertNameToCase(fileName, config.fileNameCase);
            findings.append({asset.relativePath, "file_name_case", Severity::Warning,
                             QString("Expected %1 (suggestion: %2)")
                                 .arg(config.fileNameCase, suggestion),
                             /*fixable=*/true});
        }
    }

    // ---- max/min_anim_keyframes ----
    for (int i = 0; i < asset.animationKeyframeCounts.size(); ++i) {
        int kf = asset.animationKeyframeCounts[i];
        if (config.maxAnimKeyframes > 0 && kf > config.maxAnimKeyframes)
            findings.append({asset.relativePath, "max_anim_keyframes", Severity::Error,
                             QString("Animation '%1' has %2 keyframes (max: %3)")
                                 .arg(asset.animationNames.value(i)).arg(kf).arg(config.maxAnimKeyframes)});
        if (config.minAnimKeyframes > 0 && kf < config.minAnimKeyframes)
            findings.append({asset.relativePath, "min_anim_keyframes", Severity::Warning,
                             QString("Animation '%1' has %2 keyframes (min: %3)")
                                 .arg(asset.animationNames.value(i)).arg(kf).arg(config.minAnimKeyframes)});
    }

    // ---- max/min_anim_duration ----
    for (int i = 0; i < asset.animationDurations.size(); ++i) {
        double dur = asset.animationDurations[i];
        if (config.maxAnimDuration > 0 && dur > config.maxAnimDuration)
            findings.append({asset.relativePath, "max_anim_duration", Severity::Error,
                             QString("Animation '%1' is %2s long (max: %3s)")
                                 .arg(asset.animationNames.value(i))
                                 .arg(dur, 0, 'f', 1).arg(config.maxAnimDuration, 0, 'f', 1)});
        if (config.minAnimDuration > 0 && dur < config.minAnimDuration)
            findings.append({asset.relativePath, "min_anim_duration", Severity::Warning,
                             QString("Animation '%1' is %2s long (min: %3s)")
                                 .arg(asset.animationNames.value(i))
                                 .arg(dur, 0, 'f', 1).arg(config.minAnimDuration, 0, 'f', 1)});
    }

    // ---- require_animation_names ----
    if (!config.requireAnimationNames.isEmpty()) {
        for (const auto& required : config.requireAnimationNames) {
            bool found = false;
            for (const auto& name : asset.animationNames) {
                if (matchesWildcard(name, required)) { found = true; break; }
            }
            if (!found) {
                findings.append({asset.relativePath, "require_animation_names", Severity::Error,
                                 QString("Required animation '%1' not found (has: %2)")
                                     .arg(required, asset.animationNames.join(", "))});
            }
        }
    }

    // ---- redundant_keyframes_pct ----
    // Walks the animation tracks under the configured tolerances and warns if
    // a meaningful share could be folded out by `qtmesh anim --simplify`. Uses
    // AnimationMerger::analyzeRedundantKeyframes — the same analyzer the
    // --simplify CLI action and the Inspector "Simplify" button run, so the
    // scan's count matches what the fix would actually remove.
    if (config.redundantKeyframesPctThreshold > 0.0 && asset.animationCount > 0
        && ensureOgreHeadlessQuiet()) {

        AnimationMerger::SimplifyTolerances tol;
        tol.translation = static_cast<float>(config.redundantKeyframesTranslationTol);
        tol.rotationDeg = static_cast<float>(config.redundantKeyframesRotationDegTol);
        tol.scale       = static_cast<float>(config.redundantKeyframesScaleTol);

        clearOgreSceneForScanImport();
        MeshImporterExporter::importer({QFileInfo(asset.filePath).absoluteFilePath()}, 0);
        const QList<Ogre::Entity*> entities = Manager::getSingleton()
            ? Manager::getSingleton()->getEntities() : QList<Ogre::Entity*>();

        int total = 0;
        int redundant = 0;
        std::set<std::string> seenAnims;
        for (const Ogre::Entity* entity : entities) {
            if (!entity || !entity->hasSkeleton()) continue;
            const Ogre::SkeletonPtr skel = entity->getMesh()
                                              ? entity->getMesh()->getSkeleton()
                                              : Ogre::SkeletonPtr();
            if (!skel) continue;
            for (unsigned short a = 0; a < skel->getNumAnimations(); ++a) {
                const Ogre::Animation* anim = skel->getAnimation(a);
                if (!anim) continue;
                // Multi-entity scenes can carry the same animation under more
                // than one skin (Mixamo Twist Dance, etc.). Count it once.
                if (!seenAnims.insert(anim->getName()).second) continue;
                int t = 0, r = 0;
                AnimationMerger::analyzeRedundantKeyframes(anim, tol, &t, &r);
                total += t;
                redundant += r;
            }
        }
        clearOgreSceneForScanImport();

        // The default-tolerance numbers were already filled at inspectAsset
        // time, so we don't write back here — the rule's percentage may use
        // different tolerances than the JSON report's totals, and we don't
        // want them to diverge mid-scan.

        if (total > 0) {
            const double pct = 100.0 * redundant / total;
            if (pct >= config.redundantKeyframesPctThreshold) {
                const qint64 originalSize  = asset.fileSize;
                const qint64 projectedSize = static_cast<qint64>(
                    originalSize * (1.0 - (pct / 100.0)));
                const qint64 savedBytes    = originalSize - projectedSize;

                auto formatBytes = [](qint64 bytes) -> QString {
                    if (bytes >= 1024 * 1024)
                        return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
                    if (bytes >= 1024)
                        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
                    return QString("%1 B").arg(bytes);
                };

                findings.append({asset.relativePath, "redundant_keyframes_pct", Severity::Warning,
                    QString("%1% redundant keyframes (%2/%3). Simplify it to save ~%4. "
                            "Original size: %5, projected size: %6. "
                            "Run `qtmesh scan ... --fix` to apply (FBX uses the same simplify as `qtmesh anim --simplify`; use `--dry-run` to preview).")
                        .arg(pct, 0, 'f', 1).arg(redundant).arg(total)
                        .arg(formatBytes(savedBytes))
                        .arg(formatBytes(originalSize))
                        .arg(formatBytes(projectedSize)),
                    /*fixable=*/true});
            }
        }
    }

    // ---- require_bone_names ----
    if (!config.requireBoneNames.isEmpty()) {
        for (const auto& required : config.requireBoneNames) {
            bool found = false;
            for (const auto& name : asset.boneNames) {
                if (matchesWildcard(name, required)) { found = true; break; }
            }
            if (!found) {
                findings.append({asset.relativePath, "require_bone_names", Severity::Error,
                                 QString("Required bone '%1' not found").arg(required)});
            }
        }
    }

    return findings;
}

// ---------------------------------------------------------------------------
// Auto-fixes — the redundant_keyframes_pct fix is now driven entirely by
// AnimationMerger::simplifyAnimation + MeshImporterExporter::exporter, the
// same path the FBX branch used since slice C2. No Assimp::Importer or
// Assimp::Exporter for the rewrite step.
// ---------------------------------------------------------------------------

void ScanEngine::applyFixes(const ScanConfig& config, const QString& scanRoot, AssetInfo& asset,
                            QList<Finding>& findings)
{
    if (!config.fixEnabled) return;

    for (auto& f : findings) {
        if (!f.fixable) continue;

        if (f.rule == "file_name_case") {
            QString oldPath = asset.filePath;
            QString dir = QFileInfo(oldPath).path();
            QString newName = convertNameToCase(QFileInfo(oldPath).fileName(), config.fileNameCase);
            QString newPath = dir + "/" + newName;

            if (oldPath == newPath) continue;

            if (config.dryRun) {
                f.message += QString(" [dry-run: would rename to %1]").arg(newName);
            } else {
                if (QFile::rename(oldPath, newPath)) {
                    SentryReporter::addBreadcrumb("file.export",
                        QString("Rename: %1 -> %2").arg(QFileInfo(oldPath).fileName(), newName));
                    f.message += QString(" [fixed: renamed to %1]").arg(newName);
                    f.fixed = true;
                    asset.filePath = newPath;
                    QString relDir = QFileInfo(asset.relativePath).path();
                    asset.relativePath = (relDir == ".") ? newName : relDir + "/" + newName;
                } else {
                    f.message += " [fix failed: could not rename file]";
                }
            }
        } else if (f.rule == "redundant_keyframes_pct") {
            if (config.dryRun) {
                f.message += QStringLiteral(" [dry-run: would simplify animation keys (same as qtmesh anim --simplify)]");
                continue;
            }

            // Slice C4: unified Ogre fix path for every format the editor
            // can export. Load via MeshImporterExporter (same as the in-app
            // import), run AnimationMerger::simplifyAnimation under the
            // configured tolerances (same as `qtmesh anim --simplify` and
            // the Inspector Simplify button), then re-export via the
            // editor's exporter for the matching format. No Assimp.
            if (!ensureOgreHeadlessQuiet()) {
                f.message = QStringLiteral("Fix failed: could not initialize Ogre");
                continue;
            }

            // Map asset extension to the MeshImporterExporter format
            // descriptor string. Unknown extensions are dropped here — the
            // fix only runs on formats Ogre can both import and export.
            const QString suf = QFileInfo(asset.filePath).suffix().toLower();
            static const QMap<QString, QString> formatByExt = {
                {QStringLiteral("fbx"),   QStringLiteral("FBX Binary (*.fbx)")},
                {QStringLiteral("fbxa"),  QStringLiteral("FBX Binary (*.fbx)")},
                {QStringLiteral("gltf"),  QStringLiteral("glTF 2.0 (*.gltf)")},
                {QStringLiteral("glb"),   QStringLiteral("glTF 2.0 Binary (*.glb)")},
                {QStringLiteral("dae"),   QStringLiteral("Collada (*.dae)")},
                {QStringLiteral("obj"),   QStringLiteral("OBJ (*.obj)")},
                {QStringLiteral("ply"),   QStringLiteral("PLY (*.ply)")},
                {QStringLiteral("stl"),   QStringLiteral("STL (*.stl)")},
                {QStringLiteral("mesh"),  QStringLiteral("Ogre Mesh (*.mesh)")},
            };
            if (!formatByExt.contains(suf)) {
                f.message = QStringLiteral("Fix failed: unsupported export format for .%1").arg(suf);
                continue;
            }
            const QString exportFormat = formatByExt.value(suf);

            clearOgreSceneForScanImport();
            QList<Ogre::SkeletonPtr> animOnlySkeletons;
            // Importer can throw on malformed assets (e.g. a referenced texture
            // missing on disk crashes MaterialProcessor). Swallow it here so a
            // single bad file doesn't kill the whole --fix pass; if the import
            // produces nothing usable we fall through to the no-skeleton branch
            // below and emit a per-file fix-failed message instead.
            try {
                MeshImporterExporter::importer({asset.filePath}, 0, &animOnlySkeletons);
            } catch (const Ogre::Exception& e) {
                f.message = QStringLiteral("Fix failed: importer threw — %1")
                                .arg(QString::fromUtf8(e.what()));
                clearOgreSceneForScanImport();
                continue;
            } catch (const std::exception& e) {
                f.message = QStringLiteral("Fix failed: importer threw — %1")
                                .arg(QString::fromUtf8(e.what()));
                clearOgreSceneForScanImport();
                continue;
            }

            auto& ents = Manager::getSingleton()->getEntities();
            Ogre::Entity* entity = ents.isEmpty() ? nullptr : ents.last();
            Ogre::SkeletonPtr skel;
            if (entity && entity->hasSkeleton())
                skel = entity->getMesh()->getSkeleton();
            if (!skel && !animOnlySkeletons.isEmpty())
                skel = animOnlySkeletons.last();

            if (!skel) {
                f.message = QStringLiteral("Fix failed: could not load skeleton via Ogre importer");
                continue;
            }

            AnimationMerger::SimplifyTolerances tol;
            tol.translation = static_cast<float>(config.redundantKeyframesTranslationTol);
            tol.rotationDeg = static_cast<float>(config.redundantKeyframesRotationDegTol);
            tol.scale       = static_cast<float>(config.redundantKeyframesScaleTol);

            // Total keyframes before simplifying (for reporting).
            long long totalKeysBefore = 0;
            for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai) {
                const Ogre::Animation* a = skel->getAnimation(ai);
                if (!a) continue;
                for (const auto& [handle, track] : a->_getNodeTrackList()) {
                    Q_UNUSED(handle);
                    if (track)
                        totalKeysBefore += track->getNumKeyFrames();
                }
            }

            int totalRemoved = 0;
            const unsigned short numAnims = skel->getNumAnimations();
            std::vector<std::string> animNames;
            animNames.reserve(numAnims);
            for (unsigned short ai = 0; ai < numAnims; ++ai)
                animNames.push_back(skel->getAnimation(ai)->getName());

            for (const auto& name : animNames)
                totalRemoved += AnimationMerger::simplifyAnimation(skel.get(), name, tol);

            if (totalRemoved <= 0) {
                f.message = QStringLiteral("Fix not needed: no additional simplification within configured tolerances");
                f.severity = Severity::Info;
                f.skipped = true;
                continue;
            }

            // Export to a temp path first and only apply if it improves size.
            const qint64 originalBytes = QFileInfo(asset.filePath).size();
            const QString tmpPath = asset.filePath + QStringLiteral(".qtmesh-simplify.tmp");
            if (QFile::exists(tmpPath))
                QFile::remove(tmpPath);

            bool ok = false;
            // FBX skeleton-only path: animation-only files (no mesh) take the
            // custom skeleton-only exporter which preserves the bind pose. All
            // other paths go through MeshImporterExporter::exporter.
            if (!entity && (suf == QStringLiteral("fbx") || suf == QStringLiteral("fbxa"))) {
                ok = FBXExporter::exportSkeletonOnlyFBX(skel.get(), tmpPath);
            } else if (entity) {
                entity->refreshAvailableAnimationState();
                auto* node = entity->getParentSceneNode();
                ok = (MeshImporterExporter::exporter(node, tmpPath, exportFormat) == 0);
            } else {
                f.message = QStringLiteral("Fix failed: animation-only export not supported for .%1").arg(suf);
                continue;
            }

            if (!ok) {
                QFile::remove(tmpPath);
                f.message = QStringLiteral("Fix failed: %1 export failed").arg(suf.toUpper());
                continue;
            }

            const qint64 rewrittenBytes = QFileInfo(tmpPath).size();
            // Ogre exporters are not always size-stable; allow a small overhead
            // so fixes still apply when they meaningfully improve animation
            // data, but hard-skip large blowups (>5% or >64KB growth).
            const qint64 maxAllowedGrowthBytes = std::max<qint64>(64 * 1024, originalBytes / 20);
            const qint64 maxAllowedBytes = (originalBytes > 0) ? (originalBytes + maxAllowedGrowthBytes) : 0;
            if (originalBytes > 0 && rewrittenBytes > maxAllowedBytes) {
                QFile::remove(tmpPath);
                f.message = QStringLiteral("output would be larger (%1 KB -> %2 KB), keeping original")
                                .arg(originalBytes / 1024)
                                .arg(rewrittenBytes / 1024);
                f.severity = Severity::Info;
                f.skipped = true;
                continue;
            }

            QFile orig(asset.filePath);
            if (!orig.remove()) {
                QFile::remove(tmpPath);
                f.message = QStringLiteral("Fix failed: could not replace original file");
                continue;
            }
            if (!QFile::rename(tmpPath, asset.filePath)) {
                f.message = QStringLiteral("Fix failed: could not install rewritten file");
                continue;
            }

            SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                QStringLiteral("Scan fix: simplified anim keys (%1): %2").arg(suf, asset.relativePath));
            const qint64 savedBytes = (originalBytes > 0 && rewrittenBytes > 0)
                ? (originalBytes - rewrittenBytes)
                : 0;
            const double keysPct = (totalKeysBefore > 0)
                ? (static_cast<double>(totalRemoved) * 100.0 / static_cast<double>(totalKeysBefore))
                : 0.0;
            const double sizePct = (originalBytes > 0)
                ? (static_cast<double>(savedBytes) * 100.0 / static_cast<double>(originalBytes))
                : 0.0;
            f.severity = Severity::Info;
            f.bytesSaved = savedBytes;
            f.keysRemoved = totalRemoved;
            f.message = QStringLiteral("removed %1/%2 keys (%3%), saved %4 KB (%5%)")
                            .arg(totalRemoved)
                            .arg(totalKeysBefore)
                            .arg(QString::number(keysPct, 'f', 1))
                            .arg(savedBytes / 1024)
                            .arg(QString::number(sizePct, 'f', 1));
            f.fixed = true;
            AssetInspectOptions inspectOpts;
            inspectOpts.probeTextureFiles = config.probeTextureFiles;
            asset = inspectAsset(asset.filePath, scanRoot, inspectOpts);
        }
    }
}

// ---------------------------------------------------------------------------
// Main scan pipeline
// ---------------------------------------------------------------------------

ScanResult ScanEngine::run(const ScanConfig& config, const QString& rootOverride,
                           const AssetProcessedCallback& onAssetProcessed)
{
    ScanResult result;
    const QString utcFmt = QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'");
    result.scanStartedUtc = QDateTime::currentDateTimeUtc().toString(utcFmt);
    QElapsedTimer timer;
    timer.start();

    // Determine scan root(s)
    QStringList roots;
    if (!rootOverride.isEmpty()) {
        roots.append(QDir(rootOverride).absolutePath());
    } else if (!config.roots.isEmpty()) {
        for (const auto& r : config.roots)
            roots.append(QDir(r).absolutePath());
    } else {
        roots.append(QDir::currentPath());
    }

    // Enumerate and inspect all files across all roots
    for (const auto& scanRoot : roots) {
        SentryReporter::addBreadcrumb("file.import",
            QString("Scan start: %1").arg(scanRoot));
        QStringList files = enumerateFiles(config, scanRoot);

        AssetInspectOptions inspectOpts;
        inspectOpts.probeTextureFiles = config.probeTextureFiles;

        for (const auto& filePath : files) {
            AssetInfo asset = inspectAsset(filePath, scanRoot, inspectOpts);
            if (asset.loadError)
                SentryReporter::addBreadcrumb("file.import",
                    QString("Load error: %1 — %2").arg(asset.relativePath, asset.errorMessage));
            QList<Finding> findings = evaluateRules(asset, config);

            // Apply fixes where possible
            applyFixes(config, scanRoot, asset, findings);

            if (onAssetProcessed)
                onAssetProcessed(asset, findings);

            // Tally — fixed findings don't count toward error/warning totals.
            // Note: Finding::skipped is "fix attempted but intentionally skipped" and should
            // still be considered a pass at the asset level (it doesn't mean the asset was
            // skipped from scanning).
            bool hasError = false, hasWarning = false;
            bool hasFixSkipped = false;
            for (const auto& f : findings) {
                if (f.fixed) {
                    result.fixed++;
                    result.bytesSaved += std::max<qint64>(0, f.bytesSaved);
                    result.keysRemoved += std::max<qint64>(0, f.keysRemoved);
                    continue;
                }
                if (f.skipped) { hasFixSkipped = true; continue; }
                switch (f.severity) {
                case Severity::Error:   result.errors++;   hasError   = true; break;
                case Severity::Warning: result.warnings++; hasWarning = true; break;
                case Severity::Info:    result.infos++;    break;
                }
            }

            if (asset.loadError)
                result.skipped++;
            else {
                // A fix-skip is still a "pass" (nothing failed), but we also want to
                // surface that something was skipped in the summary.
                if (!hasError && !hasWarning)
                    result.passed++;
                if (hasFixSkipped)
                    result.skipped++;
            }

            result.scanned++;
            result.findings.append(findings);
            result.assets.append(asset);
        }
    }

    result.elapsedMs = timer.elapsed();
    result.scanCompletedUtc = QDateTime::currentDateTimeUtc().toString(utcFmt);
    return result;
}

// ---------------------------------------------------------------------------
// Text formatter
// ---------------------------------------------------------------------------

static QString severityLabel(Severity s)
{
    switch (s) {
    case Severity::Error:   return "ERROR";
    case Severity::Warning: return "WARN ";
    case Severity::Info:    return "INFO ";
    }
    return "     ";
}

static QString colorizeToken(const QString& text, const char* ansiColor, bool enabled)
{
    if (!enabled)
        return text;
    return QStringLiteral("\x1b[%1m%2\x1b[0m").arg(QString::fromLatin1(ansiColor), text);
}

QString ScanEngine::formatText(const ScanResult& result, const ScanConfig& config, bool colorize,
                               const QString& activeProfileId)
{
    Q_UNUSED(config);
    QString out;
    QTextStream s(&out);

    if (!activeProfileId.isEmpty())
        s << "Profile: " << activeProfileId << "\n\n";

    // Per-asset output
    for (const auto& asset : result.assets) {
        // Collect this asset's findings
        QList<Finding> assetFindings;
        for (const auto& f : result.findings) {
            if (f.file == asset.relativePath)
                assetFindings.append(f);
        }

        bool hasError = false, hasWarning = false;
        for (const auto& f : assetFindings) {
            if (f.severity == Severity::Error)   hasError   = true;
            if (f.severity == Severity::Warning) hasWarning = true;
        }

        // Status label
        if (hasError)
            s << colorizeToken("ERROR", "31", colorize) << "   " << asset.relativePath << "\n";
        else if (hasWarning)
            s << colorizeToken("WARN", "33", colorize) << "    " << asset.relativePath << "\n";
        else
            s << "  " << colorizeToken("OK", "32", colorize) << "    " << asset.relativePath << "\n";

        // Findings detail
        for (const auto& f : assetFindings) {
            QString label = f.fixed ? QStringLiteral("FIXED") : severityLabel(f.severity);
            s << "         [" << label.trimmed().toLower() << "] "
              << f.rule << ": " << f.message << "\n";
        }
    }

    // Summary
    s << "\n";
    s << "Summary:\n";
    s << "  • Scanned:  " << result.scanned  << "\n";
    s << "  ✓ Passed:   " << result.passed   << "\n";
    s << "  ▲ Warnings: " << result.warnings << "\n";
    s << "  ✗ Errors:   " << result.errors   << "\n";
    if (result.infos > 0)
        s << "  ℹ Info:     " << result.infos << "\n";
    if (result.fixed > 0)
        s << "  🔧 Fixed:    " << result.fixed << "\n";
    if (result.bytesSaved > 0)
        s << "  📉 Saved:    " << QString::number(result.bytesSaved / (1024.0 * 1024.0), 'f', 2) << " MB\n";
    if (result.keysRemoved > 0) {
        const QString n = QLocale::system().toString(result.keysRemoved);
        s << "  🧹 Keys removed: " << n << "\n";
    }
    if (result.skipped > 0)
        s << "  ⏭ Skipped:  " << result.skipped << "\n";
    s << "  ⏱ Time:     " << QString::number(result.elapsedMs / 1000.0, 'f', 1) << "s\n";
    QString utcStart, utcEnd;
    scanReportUtcTimes(result, &utcStart, &utcEnd);
    s << "  UTC start:  " << utcStart << "\n";
    s << "  UTC end:    " << utcEnd << "\n";

    return out;
}

// ---------------------------------------------------------------------------
// JSON formatter
// ---------------------------------------------------------------------------

static QString severityStr(Severity s)
{
    switch (s) {
    case Severity::Error:   return "error";
    case Severity::Warning: return "warning";
    case Severity::Info:    return "info";
    }
    return "info";
}

/// Exported JSON (and cloud upload) must not embed Assimp paths or other local details.
static QString findingMessageForExport(const Finding& f)
{
    if (f.rule == QLatin1String("load_error"))
        return QStringLiteral("Failed to load asset (details redacted from exported JSON)");
    return f.message;
}

void ScanEngine::scanReportUtcTimes(const ScanResult& result, QString* scanStartedUtc,
                                    QString* scanCompletedUtc)
{
    const QString fmt = QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'");
    QString end = result.scanCompletedUtc;
    QString start = result.scanStartedUtc;
    if (end.isEmpty())
        end = QDateTime::currentDateTimeUtc().toString(fmt);
    if (start.isEmpty())
        start = end;
    *scanStartedUtc = start;
    *scanCompletedUtc = end;
}

QJsonObject ScanEngine::scanReportToJsonObject(const ScanResult& result)
{
    QJsonObject root;
    root["version"] = QTMESHEDITOR_VERSION;
    QString utcStart, utcEnd;
    scanReportUtcTimes(result, &utcStart, &utcEnd);
    root["scanStartedUtc"] = utcStart;
    root["scanCompletedUtc"] = utcEnd;

    // Summary
    QJsonObject summary;
    summary["scanned"]  = result.scanned;
    summary["passed"]   = result.passed;
    summary["warnings"] = result.warnings;
    summary["errors"]   = result.errors;
    summary["infos"]    = result.infos;
    summary["fixed"]    = result.fixed;
    summary["skipped"]  = result.skipped;
    if (result.bytesSaved > 0)
        summary["bytesSaved"] = static_cast<qint64>(result.bytesSaved);
    if (result.keysRemoved > 0)
        summary["keysRemoved"] = static_cast<qint64>(result.keysRemoved);
    summary["elapsedMs"] = result.elapsedMs;
    root["summary"] = summary;

    // Assets
    QJsonArray assetsArr;
    for (const auto& asset : result.assets) {
        QJsonObject ao;
        ao["file"]           = asset.relativePath;
        ao["format"]         = asset.format;
        ao["fileSize"]       = static_cast<qint64>(asset.fileSize);
        ao["meshCount"]      = static_cast<int>(asset.meshCount);
        ao["materialCount"]  = static_cast<int>(asset.materialCount);
        ao["vertexCount"]    = static_cast<int>(asset.vertexCount);
        ao["faceCount"]      = static_cast<int>(asset.faceCount);
        ao["triangleCount"]  = static_cast<int>(asset.triangleCount);
        if (asset.submeshCount > 0)
            ao["submeshCount"] = static_cast<int>(asset.submeshCount);
        if (asset.maxTrianglesPerMesh > 0)
            ao["maxTrianglesPerMesh"] = static_cast<int>(asset.maxTrianglesPerMesh);
        if (asset.estimatedDrawCalls > 0)
            ao["estimatedDrawCalls"] = static_cast<int>(asset.estimatedDrawCalls);
        if (asset.lodMeshCount > 0)
            ao["lodMeshCount"] = static_cast<int>(asset.lodMeshCount);
        ao["animationCount"] = static_cast<int>(asset.animationCount);
        ao["hasSkeleton"]    = asset.hasSkeleton;
        ao["boneCount"]      = static_cast<int>(asset.boneCount);
        ao["textureRefCount"] = static_cast<int>(asset.textureRefCount);
        if (asset.animationRedundantKeyframeRatio > 0.0)
            ao["animationRedundantKeyframeRatio"] = asset.animationRedundantKeyframeRatio;
        if (asset.weightedAcmr > 0.0)
            ao["weightedAcmr"] = asset.weightedAcmr;

        if (!asset.animationNames.isEmpty()) {
            QJsonArray anims;
            for (int i = 0; i < asset.animationNames.size(); ++i) {
                QJsonObject anim;
                anim["name"] = asset.animationNames[i];
                if (i < asset.animationDurations.size())
                    anim["duration"] = asset.animationDurations[i];
                if (i < asset.animationKeyframeCounts.size())
                    anim["keyframes"] = asset.animationKeyframeCounts[i];
                anims.append(anim);
            }
            ao["animations"] = anims;
        }
        if (!asset.boneNames.isEmpty()) {
            QJsonArray bones;
            for (const auto& b : asset.boneNames) bones.append(b);
            ao["bones"] = bones;
        }
        if (asset.totalKeyframes > 0) {
            ao["totalKeyframes"]     = asset.totalKeyframes;
            ao["redundantKeyframes"] = asset.redundantKeyframes;
        }

        // C4 quality fields — written only when populated (non-default)
        // so the JSON stays compact for assets the rule doesn't fire on.
        if (asset.maxTextureDimension > 0)
            ao["maxTextureDimension"] = asset.maxTextureDimension;
        if (asset.probedTextureMaxDimension > 0)
            ao["probedTextureMaxDimension"] = asset.probedTextureMaxDimension;
        if (asset.estimatedTextureVramBytes > 0)
            ao["estimatedTextureVramBytes"] = asset.estimatedTextureVramBytes;
        if (!asset.meshStats.isEmpty()) {
            QJsonArray meshes;
            for (const MeshStats& ms : asset.meshStats) {
                QJsonObject mo;
                mo["name"] = ms.name;
                mo["vertexCount"] = static_cast<int>(ms.vertexCount);
                mo["triangleCount"] = static_cast<int>(ms.triangleCount);
                mo["submeshCount"] = static_cast<int>(ms.submeshCount);
                meshes.append(mo);
            }
            ao["meshStats"] = meshes;
        }
        if (!asset.textureStats.isEmpty()) {
            QJsonArray textures;
            for (const TextureRefStats& ts : asset.textureStats) {
                QJsonObject to;
                to["path"] = ts.path;
                if (!ts.resolvedPath.isEmpty())
                    to["resolvedPath"] = ts.resolvedPath;
                if (ts.width > 0)
                    to["width"] = ts.width;
                if (ts.height > 0)
                    to["height"] = ts.height;
                if (ts.fileSizeBytes >= 0)
                    to["fileSizeBytes"] = ts.fileSizeBytes;
                if (!ts.format.isEmpty())
                    to["format"] = ts.format;
                if (ts.missing)
                    to["missing"] = true;
                textures.append(to);
            }
            ao["textureStats"] = textures;
        }
        if (asset.minUvChannelCount > 0)
            ao["minUvChannelCount"]   = asset.minUvChannelCount;
        if (!asset.zeroWeightBoneNames.isEmpty()) {
            QJsonArray zwb;
            for (const auto& n : asset.zeroWeightBoneNames) zwb.append(n);
            ao["zeroWeightBones"] = zwb;
        }
        if (asset.overlappingUvsRatio >= 0.0)
            ao["overlappingUvsRatio"]  = asset.overlappingUvsRatio;
        if (asset.nonManifoldEdgesRatio >= 0.0)
            ao["nonManifoldEdgesRatio"] = asset.nonManifoldEdgesRatio;
        if (asset.ps1RipZeroAreaRatio >= 0.0)
            ao["ps1RipZeroAreaRatio"] = asset.ps1RipZeroAreaRatio;
        if (asset.ps1RipDegenerateUvRatio >= 0.0)
            ao["ps1RipDegenerateUvRatio"] = asset.ps1RipDegenerateUvRatio;

        if (asset.loadError)
            ao["loadError"] = true;

        // Inline findings for this asset
        QJsonArray findingsArr;
        for (const auto& f : result.findings) {
            if (f.file != asset.relativePath) continue;
            QJsonObject fo;
            fo["rule"]     = f.rule;
            fo["severity"] = severityStr(f.severity);
            fo["message"]  = findingMessageForExport(f);
            if (f.fixable) fo["fixable"] = true;
            if (f.fixed)   fo["fixed"]   = true;
            if (f.skipped) fo["skipped"] = true;
            findingsArr.append(fo);
        }
        ao["findings"] = findingsArr;

        assetsArr.append(ao);
    }
    root["assets"] = assetsArr;

    return root;
}

QJsonObject ScanEngine::mergeGithubActionsMetaIntoReport(const QJsonObject& report)
{
    // PR workflows set GITHUB_HEAD_REF (source branch). Push workflows only set GITHUB_REF_NAME.
    const QByteArray branch = [&]() -> QByteArray {
        const QByteArray headRef = qgetenv("GITHUB_HEAD_REF");
        if (!headRef.isEmpty())
            return headRef;
        return qgetenv("GITHUB_REF_NAME");
    }();

    static const struct {
        const char* envVar;
        const char* jsonKey;
    } kMappings[] = {
        {"GITHUB_REPOSITORY", "repository"},
        {"GITHUB_BASE_REF", "baseBranch"},
        {"GITHUB_SHA", "commitSha"},
        {"GITHUB_RUN_ID", "runId"},
        {"GITHUB_RUN_NUMBER", "runNumber"},
        {"GITHUB_WORKFLOW", "workflow"},
        {"GITHUB_JOB", "job"},
        {"GITHUB_ACTOR", "actor"},
    };

    QJsonObject out = report;
    QJsonObject meta = out.value(QStringLiteral("meta")).toObject();
    if (!branch.isEmpty())
        meta[QStringLiteral("branch")] = QString::fromUtf8(branch);
    for (const auto& m : kMappings) {
        const QByteArray v = qgetenv(m.envVar);
        if (!v.isEmpty())
            meta[QString::fromLatin1(m.jsonKey)] = QString::fromUtf8(v);
    }
    if (!meta.isEmpty())
        out.insert(QStringLiteral("meta"), meta);
    return out;
}

QString ScanEngine::formatJson(const ScanResult& result)
{
    return QString::fromUtf8(QJsonDocument(scanReportToJsonObject(result)).toJson(QJsonDocument::Indented));
}

// ---------------------------------------------------------------------------
// SARIF formatter (Static Analysis Results Interchange Format 2.1.0)
// ---------------------------------------------------------------------------

QString ScanEngine::formatSarif(const ScanResult& result, const QString& activeProfileId)
{
    // Build rule definitions from unique rule IDs
    QMap<QString, QString> ruleDescriptions;
    ruleDescriptions["load_error"]              = "Asset file could not be loaded";
    ruleDescriptions["allowed_formats"]         = "Asset format is not in the allowed list";
    ruleDescriptions["forbidden_extensions"]    = "Asset format is explicitly forbidden";
    ruleDescriptions["max_file_size_mb"]        = "Asset file exceeds maximum file size";
    ruleDescriptions["max_mesh_count"]          = "Asset exceeds maximum mesh count";
    ruleDescriptions["max_material_count"]      = "Asset exceeds maximum material count";
    ruleDescriptions["max_vertex_count"]        = "Asset exceeds maximum vertex count";
    ruleDescriptions["max_acmr"]                = "Asset's vertex-cache ACMR exceeds the configured ceiling — reorder for GPU efficiency";
    ruleDescriptions["require_skeleton"]        = "Asset is missing a required skeleton";
    ruleDescriptions["require_animations"]      = "Asset is missing required animations";
    ruleDescriptions["allow_embedded_textures"] = "Asset contains embedded textures";
    ruleDescriptions["require_textures_exist"]  = "Referenced texture file not found on disk";
    ruleDescriptions["allow_missing_materials"] = "Asset has placeholder or missing materials";
    ruleDescriptions["file_name_case"]          = "File name does not match naming convention";
    ruleDescriptions["max_anim_keyframes"]      = "Animation exceeds maximum keyframe count";
    ruleDescriptions["min_anim_keyframes"]      = "Animation has fewer than minimum keyframes";
    ruleDescriptions["max_anim_duration"]       = "Animation exceeds maximum duration";
    ruleDescriptions["min_anim_duration"]       = "Animation is shorter than minimum duration";
    ruleDescriptions["min_file_size_mb"]        = "Asset file is smaller than minimum size";
    ruleDescriptions["min_mesh_count"]          = "Asset has fewer than minimum meshes";
    ruleDescriptions["min_material_count"]      = "Asset has fewer than minimum materials";
    ruleDescriptions["min_vertex_count"]        = "Asset has fewer than minimum vertices";
    ruleDescriptions["require_animation_names"] = "Required animation not found in asset";
    ruleDescriptions["require_bone_names"]      = "Required bone not found in skeleton";
    ruleDescriptions["redundant_keyframes_pct"] = "Animation contains redundant keyframes that could be safely simplified";
    // C4 quality rules
    ruleDescriptions["max_texture_resolution"]    = "Asset has textures larger than the configured pixel ceiling";
    ruleDescriptions["require_uv_channels"]       = "Submesh has fewer UV sets than the rule requires";
    ruleDescriptions["detect_zero_weight_bones"]  = "Skeleton has bones with no vertex weights (Mixamo bloat)";
    ruleDescriptions["detect_overlapping_uvs_pct"]    = "Triangles share overlapping UV0 regions (lightmap-unsafe)";
    ruleDescriptions["detect_non_manifold_edges_pct"] = "Mesh has non-manifold edges (booleans / printing will fail)";
    ruleDescriptions["ps1-rip-zero-area"]             = "Zero-area (sliver) triangles from a PS1 capture — clean with the PS1 cleanup pipeline";
    ruleDescriptions["ps1-rip-degenerate-uv"]         = "Degenerate UV0 triangles from a PS1 capture — re-unwrap if UVs are needed";
    ruleDescriptions["max_triangle_count"]            = "Asset exceeds maximum triangle count";
    ruleDescriptions["max_triangles_per_mesh"]        = "Single mesh exceeds maximum triangle count";
    ruleDescriptions["max_bones"]                   = "Skeleton exceeds maximum bone count";
    ruleDescriptions["max_submesh_count"]           = "Asset exceeds maximum submesh count (draw-call proxy)";
    ruleDescriptions["max_draw_calls"]                = "Estimated draw-call count exceeds limit";
    ruleDescriptions["max_texture_dimension"]       = "Texture exceeds maximum dimension (alias of max_texture_resolution)";
    ruleDescriptions["texture_not_power_of_two"]    = "Referenced texture dimensions are not power-of-two";
    ruleDescriptions["texture_format_disallowed"]   = "Referenced texture uses a disallowed or non-allow-listed format";

    // Collect unique rules used in findings
    QSet<QString> usedRules;
    for (const auto& f : result.findings)
        usedRules.insert(f.rule);

    QJsonArray rulesArr;
    for (const auto& ruleId : usedRules) {
        QJsonObject rule;
        rule["id"] = ruleId;
        QJsonObject shortDesc;
        shortDesc["text"] = ruleDescriptions.value(ruleId, ruleId);
        rule["shortDescription"] = shortDesc;
        rulesArr.append(rule);
    }

    // Build results
    QJsonArray resultsArr;
    for (const auto& f : result.findings) {
        QJsonObject r;
        r["ruleId"] = f.rule;

        QJsonObject message;
        message["text"] = f.message;
        r["message"] = message;

        QString level;
        switch (f.severity) {
        case Severity::Error:   level = "error"; break;
        case Severity::Warning: level = "warning"; break;
        case Severity::Info:    level = "note"; break;
        }
        r["level"] = level;

        // Location
        QJsonObject physicalLocation;
        QJsonObject artifactLocation;
        artifactLocation["uri"] = f.file;
        physicalLocation["artifactLocation"] = artifactLocation;

        QJsonObject location;
        location["physicalLocation"] = physicalLocation;
        r["locations"] = QJsonArray{location};

        if (f.fixable) {
            QJsonObject props;
            props["fixable"] = true;
            if (f.fixed) props["fixed"] = true;
            if (f.skipped) props["skipped"] = true;
            r["properties"] = props;
        }

        resultsArr.append(r);
    }

    // Assemble SARIF document
    QJsonObject driver;
    driver["name"] = "qtmesh scan";
    driver["version"] = QTMESHEDITOR_VERSION;
    driver["rules"] = rulesArr;

    QJsonObject tool;
    tool["driver"] = driver;

    QJsonObject run;
    run["tool"] = tool;
    if (!activeProfileId.isEmpty()) {
        QJsonObject props;
        props["profile"] = activeProfileId;
        run["properties"] = props;
    }
    run["results"] = resultsArr;
    QString sarifStart, sarifEnd;
    scanReportUtcTimes(result, &sarifStart, &sarifEnd);
    QJsonObject invocation;
    invocation["startTimeUtc"] = sarifStart;
    invocation["endTimeUtc"] = sarifEnd;
    invocation["executionSuccessful"] = true;
    run["invocations"] = QJsonArray{invocation};

    QJsonObject sarif;
    sarif["$schema"] = "https://json.schemastore.org/sarif-2.1.0.json";
    sarif["version"] = "2.1.0";
    sarif["runs"] = QJsonArray{run};

    return QString::fromUtf8(QJsonDocument(sarif).toJson(QJsonDocument::Indented));
}
