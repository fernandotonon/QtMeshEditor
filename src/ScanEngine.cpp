#include "ScanEngine.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QLocale>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTextStream>
#include <QWidget>

#include <assimp/Exporter.hpp>
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
#include <cmath>
#include <set>
#include <vector>

namespace {

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

AssetInfo ScanEngine::inspectAsset(const QString& filePath, const QString& scanRoot)
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
            return info;
        }
        if (gext == QLatin1String("ply") && PS1PLY::isPsyqPlyFile(geomPath)) {
            loadAndFillOgreInspect(
                info, [geomPath](const std::string& mn) { return PS1PLY::importPsyqPly(geomPath, mn); }, &detailErr);
            return info;
        }
        if (gext == QLatin1String("rsd")) {
            info.loadError = true;
            info.errorMessage = QStringLiteral("RSD references another RSD as geometry (not supported for scan)");
            return info;
        }
        AssetInfo inner = ScanEngine::inspectAsset(geomPath, QFileInfo(geomPath).absolutePath());
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
        return info;
    }

    if (extLower == QLatin1String("ply") && PS1PLY::isPsyqPlyFile(filePath)) {
        QString detailErr;
        loadAndFillOgreInspect(
            info, [filePath](const std::string& mn) { return PS1PLY::importPsyqPly(filePath, mn); }, &detailErr);
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
// Auto-fixes
// ---------------------------------------------------------------------------

namespace {

bool vecApproxEqualAssimp(const aiVector3D& a, const aiVector3D& b, float eps = 1e-6f)
{
    return (std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps);
}

bool quatApproxEqualAssimp(const aiQuaternion& a, const aiQuaternion& b, float eps = 1e-3f)
{
    const float d = std::fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
    return d > 1.f - eps;
}

void compactVectorTrackInPlace(aiVectorKey*& keys, unsigned& n)
{
    if (n <= 2 || !keys)
        return;
    std::vector<aiVectorKey> out;
    out.reserve(n);
    out.push_back(keys[0]);
    for (unsigned i = 1; i + 1 < n; ++i) {
        if (!vecApproxEqualAssimp(keys[i].mValue, out.back().mValue))
            out.push_back(keys[i]);
    }
    if (!vecApproxEqualAssimp(keys[n - 1].mValue, out.back().mValue))
        out.push_back(keys[n - 1]);
    if (out.size() == 1 && n >= 2) {
        out.clear();
        out.push_back(keys[0]);
        out.push_back(keys[n - 1]);
    }
    const unsigned newN = static_cast<unsigned>(out.size());
    if (newN == n)
        return;
    auto* nk = new aiVectorKey[newN];
    for (unsigned i = 0; i < newN; ++i)
        nk[i] = out[i];
    delete[] keys;
    keys = nk;
    n = newN;
}

void compactQuatTrackInPlace(aiQuatKey*& keys, unsigned& n)
{
    if (n <= 2 || !keys)
        return;
    std::vector<aiQuatKey> out;
    out.reserve(n);
    out.push_back(keys[0]);
    for (unsigned i = 1; i + 1 < n; ++i) {
        if (!quatApproxEqualAssimp(keys[i].mValue, out.back().mValue))
            out.push_back(keys[i]);
    }
    if (!quatApproxEqualAssimp(keys[n - 1].mValue, out.back().mValue))
        out.push_back(keys[n - 1]);
    if (out.size() == 1 && n >= 2) {
        out.clear();
        out.push_back(keys[0]);
        out.push_back(keys[n - 1]);
    }
    const unsigned newN = static_cast<unsigned>(out.size());
    if (newN == n)
        return;
    auto* nk = new aiQuatKey[newN];
    for (unsigned i = 0; i < newN; ++i)
        nk[i] = out[i];
    delete[] keys;
    keys = nk;
    n = newN;
}

void stripRedundantAnimKeys(aiScene* scene)
{
    if (!scene)
        return;
    for (unsigned ai = 0; ai < scene->mNumAnimations; ++ai) {
        aiAnimation* anim = scene->mAnimations[ai];
        if (!anim || !anim->mChannels)
            continue;
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            aiNodeAnim* ch = anim->mChannels[c];
            if (!ch)
                continue;
            compactVectorTrackInPlace(ch->mPositionKeys, ch->mNumPositionKeys);
            compactQuatTrackInPlace(ch->mRotationKeys, ch->mNumRotationKeys);
            compactVectorTrackInPlace(ch->mScalingKeys, ch->mNumScalingKeys);
        }
    }
}

long long totalAnimKeysForScene(const aiScene* scene)
{
    if (!scene)
        return 0;
    long long total = 0;
    for (unsigned ai = 0; ai < scene->mNumAnimations; ++ai) {
        const aiAnimation* anim = scene->mAnimations[ai];
        if (!anim || !anim->mChannels)
            continue;
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim* ch = anim->mChannels[c];
            if (!ch)
                continue;
            total += static_cast<long long>(ch->mNumPositionKeys);
            total += static_cast<long long>(ch->mNumRotationKeys);
            total += static_cast<long long>(ch->mNumScalingKeys);
        }
    }
    return total;
}

QString assimpExportFormatIdForAssetPath(const QString& outputPath)
{
    const QString suf = QFileInfo(outputPath).suffix().toLower();
    static const QMap<QString, QString> fromExt{
        {QStringLiteral("fbx"), QStringLiteral("fbx")},
        {QStringLiteral("dae"), QStringLiteral("collada")},
        {QStringLiteral("obj"), QStringLiteral("obj")},
        {QStringLiteral("stl"), QStringLiteral("stl")},
        {QStringLiteral("ply"), QStringLiteral("ply")},
        {QStringLiteral("3ds"), QStringLiteral("3ds")},
        {QStringLiteral("gltf"), QStringLiteral("gltf2")},
        {QStringLiteral("glb"), QStringLiteral("glb2")},
        {QStringLiteral("assbin"), QStringLiteral("assbin")},
        {QStringLiteral("x"), QStringLiteral("x")},
    };
    return fromExt.value(suf, QStringLiteral("fbx"));
}

} // namespace

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
            const QString suf = QFileInfo(asset.filePath).suffix().toLower();
            if (suf == QStringLiteral("fbx") || suf == QStringLiteral("fbxa")) {
                // Ogre + tolerance-based simplify (same core as `qtmesh anim --simplify`) + custom FBX exporter.
                if (!ensureOgreHeadlessQuiet()) {
                    f.message = QStringLiteral("Fix failed: could not initialize Ogre");
                    continue;
                }

                clearOgreSceneForScanImport();

                QList<Ogre::SkeletonPtr> animOnlySkeletons;
                MeshImporterExporter::importer({asset.filePath}, 0, &animOnlySkeletons);

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
                tol.scale = static_cast<float>(config.redundantKeyframesScaleTol);

                // Compute total keyframes before simplifying (for reporting).
                long long totalKeysBefore = 0;
                for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai) {
                    const Ogre::Animation* a = skel->getAnimation(ai);
                    if (!a) continue;
                    for (const auto& [handle, track] : a->_getNodeTrackList()) {
                        Q_UNUSED(handle);
                        if (!track) continue;
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
                if (!entity) {
                    ok = FBXExporter::exportSkeletonOnlyFBX(skel.get(), tmpPath);
                } else {
                    entity->refreshAvailableAnimationState();
                    auto* node = entity->getParentSceneNode();
                    ok = (MeshImporterExporter::exporter(node, tmpPath, QStringLiteral("FBX Binary (*.fbx)")) == 0);
                }

                if (!ok) {
                    QFile::remove(tmpPath);
                    f.message = QStringLiteral("Fix failed: FBX export failed");
                    continue;
                }

                const qint64 rewrittenBytes = QFileInfo(tmpPath).size();
                // The goal of this fix is to reduce animation payload. If the rewritten file
                // isn't smaller, keep the original to avoid regressions from exporter variance.
                if (originalBytes > 0 && rewrittenBytes >= originalBytes) {
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
                    QStringLiteral("Scan fix: simplified anim keys (custom FBX): %1").arg(asset.relativePath));
                const qint64 savedBytes = (originalBytes > 0 && rewrittenBytes > 0) ? (originalBytes - rewrittenBytes) : 0;
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
                asset = inspectAsset(asset.filePath, scanRoot);
                continue;
            }

            const qint64 originalBytes = QFileInfo(asset.filePath).size();
            Assimp::Importer imp;
            imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
            unsigned int impFlags = aiProcess_Triangulate | aiProcess_ValidateDataStructure;
            const aiScene* loaded = imp.ReadFile(asset.filePath.toStdString(), impFlags);
            if (isAssimpResultLoadFailure(loaded, imp.GetErrorString(), nullptr) &&
                (pathEndsWithInsensitive(asset.filePath, QLatin1String(".fbx")) ||
                 pathEndsWithInsensitive(asset.filePath, QLatin1String(".fbxa")))) {
                impFlags = aiProcess_Triangulate | aiProcess_ValidateDataStructure |
                           aiProcess_LimitBoneWeights | aiProcess_PopulateArmatureData |
                           aiProcess_GlobalScale;
                loaded = imp.ReadFile(asset.filePath.toStdString(), impFlags);
            }
            if (isAssimpResultLoadFailure(loaded, imp.GetErrorString(), nullptr)) {
                f.message += QStringLiteral(" [fix failed: could not re-read asset]");
                continue;
            }
            if (loaded->mNumAnimations == 0) {
                f.message += QStringLiteral(" [fix failed: no animations in file]");
                continue;
            }
            aiScene* mutScene = const_cast<aiScene*>(loaded);
            const long long keysBefore = totalAnimKeysForScene(mutScene);
            stripRedundantAnimKeys(mutScene);
            const long long keysAfter = totalAnimKeysForScene(mutScene);
            if (keysAfter >= keysBefore) {
                f.message = QStringLiteral("Fix not needed: no reducible consecutive duplicate keys (per-channel)");
                f.severity = Severity::Info;
                f.skipped = true;
                continue;
            }

            const QString tmpPath = asset.filePath + QStringLiteral(".qtmesh-strip.tmp");
            if (QFile::exists(tmpPath))
                QFile::remove(tmpPath);

            const QString formatId = assimpExportFormatIdForAssetPath(asset.filePath);
            const unsigned int exportFlags =
                (formatId == QLatin1String("x")) ? 0u : aiProcess_ConvertToLeftHanded;

            Assimp::Exporter exporter;
            const aiReturn expRet = exporter.Export(mutScene, formatId.toStdString().c_str(),
                                                    tmpPath.toStdString().c_str(), exportFlags);
            if (expRet != AI_SUCCESS) {
                QFile::remove(tmpPath);
                f.message += QStringLiteral(" [fix failed: export: %1]")
                                 .arg(QString::fromUtf8(exporter.GetErrorString()));
                continue;
            }
            const qint64 rewrittenBytes = QFileInfo(tmpPath).size();
            // Assimp FBX export is not size-stable; allow a small overhead so fixes still apply
            // when they meaningfully improve animation data. Hard-skip large blowups.
            const qint64 maxAllowedGrowthBytes = std::max<qint64>(64 * 1024, originalBytes / 20); // max(64KB, 5%)
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
                QStringLiteral("Scan fix: stripped consecutive duplicate anim keys (Assimp): %1").arg(asset.relativePath));
            f.message = QStringLiteral("Fixed: stripped consecutive duplicate animation keys (non-FBX path)");
            f.fixed = true;
            asset = inspectAsset(asset.filePath, scanRoot);
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

        for (const auto& filePath : files) {
            AssetInfo asset = inspectAsset(filePath, scanRoot);
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

QString ScanEngine::formatText(const ScanResult& result, const ScanConfig& config, bool colorize)
{
    Q_UNUSED(config);
    QString out;
    QTextStream s(&out);

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

QString ScanEngine::formatSarif(const ScanResult& result)
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
