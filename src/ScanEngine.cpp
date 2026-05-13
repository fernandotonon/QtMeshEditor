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

// Walk an Ogre skeleton populating boneCount + animation list on AssetInfo.
// Mirrors what the Assimp pass extracts so .mesh + .skeleton assets show the
// same metadata as Assimp-loaded formats. Keyframe count per animation is the
// maximum across node tracks, matching the per-anim "maxKeys" the Assimp path
// computes. Redundant-keyframe analysis is intentionally skipped here — that
// pass is Assimp-channel-specific and not load-bearing for .mesh files.
static void fillAssetInfoFromOgreSkeleton(AssetInfo& info, const Ogre::SkeletonPtr& skel)
{
    if (!skel)
        return;
    info.hasSkeleton = true;
    info.boneCount = skel->getNumBones();
    for (unsigned short i = 0; i < skel->getNumBones(); ++i)
        if (auto* b = skel->getBone(i))
            info.boneNames.append(QString::fromStdString(b->getName()));

    info.animationCount = skel->getNumAnimations();
    for (unsigned short a = 0; a < skel->getNumAnimations(); ++a) {
        const Ogre::Animation* anim = skel->getAnimation(a);
        if (!anim)
            continue;
        info.animationNames.append(QString::fromStdString(anim->getName()));
        info.animationDurations.append(static_cast<double>(anim->getLength()));
        unsigned maxKeys = 0;
        for (const auto& [handle, track] : anim->_getNodeTrackList()) {
            if (track)
                maxKeys = std::max(maxKeys, static_cast<unsigned>(track->getNumKeyFrames()));
        }
        info.animationKeyframeCounts.append(static_cast<int>(maxKeys));
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

// Returns true when an ACMR was produced; false on import failure or no
// triangulated submeshes. Clears the scene + unloads Ogre resources on the
// way out so the caller's loop doesn't accumulate state.
static bool computeWeightedAcmrViaOgre(const QString& filePath, AssetInfo& info)
{
    if (!ensureOgreHeadlessQuiet())
        return false;

    clearOgreSceneForScanImport();
    // Import via the editor's loader; default flags (0) match what the GUI
    // uses for File > Open, so the index order we measure is the order the
    // user actually ships.
    MeshImporterExporter::importer({QFileInfo(filePath).absoluteFilePath()}, 0);

    auto* mgr = Manager::getSingleton();
    if (!mgr) return false;
    const auto& entities = mgr->getEntities();
    if (entities.isEmpty()) {
        // Some formats (animation-only FBX) produce no entity; not an error
        // for the scan, just nothing to measure ACMR on.
        clearOgreSceneForScanImport();
        return false;
    }

    double weightedSum = 0.0;
    unsigned int totalTris = 0;
    for (const Ogre::Entity* entity : entities)
        accumulateEntityAcmr(entity, weightedSum, totalTris);

    if (totalTris > 0)
        info.weightedAcmr = weightedSum / totalTris;

    clearOgreSceneForScanImport();
    return totalTris > 0;
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
// Redundant-keyframe analysis (Assimp-side, no Ogre).
//
// Mirrors AnimationMerger::simplifyAnimation, which treats an Ogre node
// keyframe atomically (a single record holding T+R+S at one time). We build
// the same per-node view from Assimp by unioning the position/rotation/scale
// time arrays per aiNodeAnim and sampling all three streams at each unique
// time. Then a key is redundant when removing it leaves the lerp/slerp of
// the *atomic* neighbors within tolerance for every channel — same definition
// the simplifier applies. First and last keys per node track are preserved.
// ---------------------------------------------------------------------------

namespace {

struct RedundancyTolerances {
    double translation = 1e-3;
    double rotationDeg = 0.5;
    double scale       = 1e-3;
};

struct NodeKey {
    double time = 0.0;
    double tx = 0.0, ty = 0.0, tz = 0.0;     // position
    double sx = 1.0, sy = 1.0, sz = 1.0;     // scale
    double qx = 0.0, qy = 0.0, qz = 0.0, qw = 1.0; // rotation
};

// Sample a sorted aiVectorKey array at time `t` using piecewise-linear
// interpolation (Assimp's storage convention). Empty arrays return the
// supplied default.
static aiVector3D sampleVecKeys(const aiVectorKey* keys, unsigned n, double t,
                                const aiVector3D& fallback)
{
    if (n == 0 || !keys) return fallback;
    if (n == 1 || t <= keys[0].mTime) return keys[0].mValue;
    if (t >= keys[n-1].mTime) return keys[n-1].mValue;
    // Linear scan — channel arrays are short (~hundreds), no need for binary search.
    for (unsigned i = 1; i < n; ++i) {
        if (t <= keys[i].mTime) {
            const double span = keys[i].mTime - keys[i-1].mTime;
            if (span <= 1e-9) return keys[i].mValue;
            const float u = static_cast<float>((t - keys[i-1].mTime) / span);
            return keys[i-1].mValue + (keys[i].mValue - keys[i-1].mValue) * u;
        }
    }
    return keys[n-1].mValue;
}

static aiQuaternion sampleQuatKeys(const aiQuatKey* keys, unsigned n, double t,
                                   const aiQuaternion& fallback)
{
    if (n == 0 || !keys) return fallback;
    if (n == 1 || t <= keys[0].mTime) return keys[0].mValue;
    if (t >= keys[n-1].mTime) return keys[n-1].mValue;
    for (unsigned i = 1; i < n; ++i) {
        if (t <= keys[i].mTime) {
            const double span = keys[i].mTime - keys[i-1].mTime;
            if (span <= 1e-9) return keys[i].mValue;
            const float u = static_cast<float>((t - keys[i-1].mTime) / span);
            aiQuaternion out;
            aiQuaternion::Interpolate(out, keys[i-1].mValue, keys[i].mValue, u);
            return out;
        }
    }
    return keys[n-1].mValue;
}

// Union of distinct times across T/R/S streams for one aiNodeAnim, in order.
static std::vector<double> unionTimes(const aiNodeAnim* ch)
{
    if (!ch)
        return {};
    std::vector<double> times;
    times.reserve(ch->mNumPositionKeys + ch->mNumRotationKeys + ch->mNumScalingKeys);
    if (ch->mPositionKeys) {
        for (unsigned i = 0; i < ch->mNumPositionKeys; ++i)
            times.push_back(ch->mPositionKeys[i].mTime);
    }
    if (ch->mRotationKeys) {
        for (unsigned i = 0; i < ch->mNumRotationKeys; ++i)
            times.push_back(ch->mRotationKeys[i].mTime);
    }
    if (ch->mScalingKeys) {
        for (unsigned i = 0; i < ch->mNumScalingKeys; ++i)
            times.push_back(ch->mScalingKeys[i].mTime);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(),
        [](double a, double b) { return std::fabs(a - b) < 1e-7; }), times.end());
    return times;
}

// Quaternion dot product (avoids depending on Assimp's operator).
static double quatDot(const aiQuaternion& a, const aiQuaternion& b)
{
    return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
}
static aiQuaternion alignedTo(const aiQuaternion& q, const aiQuaternion& ref)
{
    if (quatDot(q, ref) < 0.0)
        return aiQuaternion(-q.w, -q.x, -q.y, -q.z);
    return q;
}

// Decide whether key B (between A and C) can be safely dropped given that
// the atomic node value contains T, R, and S — i.e. all three channels must
// be within their respective tolerances at B's time.
static bool nodeKeyIsRedundant(const NodeKey& a, const NodeKey& b, const NodeKey& c,
                               const RedundancyTolerances& tol)
{
    const double span = c.time - a.time;
    if (span <= 1e-7) return true;
    const double u = (b.time - a.time) / span;
    if (u <= 0.0 || u >= 1.0) return false;

    // Translation
    const double dtx = (a.tx + (c.tx - a.tx) * u) - b.tx;
    const double dty = (a.ty + (c.ty - a.ty) * u) - b.ty;
    const double dtz = (a.tz + (c.tz - a.tz) * u) - b.tz;
    if (std::sqrt(dtx*dtx + dty*dty + dtz*dtz) > tol.translation) return false;

    // Scale
    const double dsx = (a.sx + (c.sx - a.sx) * u) - b.sx;
    const double dsy = (a.sy + (c.sy - a.sy) * u) - b.sy;
    const double dsz = (a.sz + (c.sz - a.sz) * u) - b.sz;
    if (std::sqrt(dsx*dsx + dsy*dsy + dsz*dsz) > tol.scale) return false;

    // Rotation: slerp on hemisphere-aligned quats, compare angular distance.
    const aiQuaternion qA(static_cast<float>(a.qw), static_cast<float>(a.qx),
                          static_cast<float>(a.qy), static_cast<float>(a.qz));
    aiQuaternion qC(static_cast<float>(c.qw), static_cast<float>(c.qx),
                    static_cast<float>(c.qy), static_cast<float>(c.qz));
    aiQuaternion qB(static_cast<float>(b.qw), static_cast<float>(b.qx),
                    static_cast<float>(b.qy), static_cast<float>(b.qz));
    qC = alignedTo(qC, qA);
    qB = alignedTo(qB, qA);
    aiQuaternion slerp;
    aiQuaternion::Interpolate(slerp, qA, qC, static_cast<float>(u));
    double d = std::fabs(quatDot(slerp, qB));
    if (d > 1.0) d = 1.0;
    const double angleDeg = std::acos(d) * 2.0 * 180.0 / M_PI;
    return angleDeg <= tol.rotationDeg;
}

// Walk one node's atomic keys and count how many would be folded out by
// simplifyAnimation under the same tolerances. First/last preserved.
static int countRedundantNodeKeys(const std::vector<NodeKey>& keys,
                                  const RedundancyTolerances& tol)
{
    if (keys.size() < 3) return 0;
    std::vector<size_t> kept;
    kept.reserve(keys.size());
    kept.push_back(0);
    for (size_t i = 1; i + 1 < keys.size(); ++i) {
        kept.push_back(i);
        while (kept.size() >= 3) {
            const size_t a = kept[kept.size() - 3];
            const size_t b = kept[kept.size() - 2];
            const size_t c = kept[kept.size() - 1];
            if (nodeKeyIsRedundant(keys[a], keys[b], keys[c], tol))
                kept.erase(kept.begin() + (kept.size() - 2));
            else
                break;
        }
    }
    kept.push_back(keys.size() - 1);
    while (kept.size() >= 3) {
        const size_t a = kept[kept.size() - 3];
        const size_t b = kept[kept.size() - 2];
        const size_t c = kept[kept.size() - 1];
        if (nodeKeyIsRedundant(keys[a], keys[b], keys[c], tol))
            kept.erase(kept.begin() + (kept.size() - 2));
        else
            break;
    }
    return static_cast<int>(keys.size()) - static_cast<int>(kept.size());
}

static void analyzeAnimationRedundancy(const aiAnimation* anim,
                                       const RedundancyTolerances& tol,
                                       int* outTotal, int* outRedundant)
{
    int total = 0;
    int redundant = 0;
    if (!anim || !anim->mChannels) {
        if (outTotal) *outTotal = 0;
        if (outRedundant) *outRedundant = 0;
        return;
    }
    for (unsigned c = 0; c < anim->mNumChannels; ++c) {
        const aiNodeAnim* ch = anim->mChannels[c];
        if (!ch)
            continue;
        const std::vector<double> times = unionTimes(ch);
        if (times.empty()) continue;

        // Build atomic node keys by sampling every stream at each unique time.
        std::vector<NodeKey> keys;
        keys.reserve(times.size());
        for (double t : times) {
            NodeKey k;
            k.time = t;
            const aiVector3D pos = sampleVecKeys(ch->mPositionKeys, ch->mNumPositionKeys, t,
                                                 aiVector3D(0,0,0));
            const aiVector3D scl = sampleVecKeys(ch->mScalingKeys, ch->mNumScalingKeys, t,
                                                 aiVector3D(1,1,1));
            const aiQuaternion rot = sampleQuatKeys(ch->mRotationKeys, ch->mNumRotationKeys, t,
                                                    aiQuaternion(1,0,0,0));
            k.tx = pos.x; k.ty = pos.y; k.tz = pos.z;
            k.sx = scl.x; k.sy = scl.y; k.sz = scl.z;
            k.qw = rot.w; k.qx = rot.x; k.qy = rot.y; k.qz = rot.z;
            keys.push_back(k);
        }
        total += static_cast<int>(keys.size());
        redundant += countRedundantNodeKeys(keys, tol);
    }
    if (outTotal)     *outTotal     = total;
    if (outRedundant) *outRedundant = redundant;
}

} // namespace

// ---------------------------------------------------------------------------
// Asset inspection: Assimp drives scene-graph metadata extraction (vertex /
// face counts, materials, animations, redundant-keyframe analysis); Ogre
// drives ACMR (slice C2) and the PlayStation TMD / Psy-Q PLY / RSD paths.
// Both run through a single headless Ogre context shared across files; per-
// file cleanup happens in clearOgreSceneForScanImport (which also flushes
// MeshManager / SkeletonManager so a 1000-asset scan doesn't pile state).
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

    // Ogre native .mesh: load via Ogre's MeshSerializer (the same path the
    // editor uses) instead of routing through Assimp. Assimp's .mesh reader
    // is limited to v1.41+ and fails on older serializer versions (e.g.
    // robot.mesh / v1.40). ACMR is computed below via the shared Ogre path.
    if (extLower == QLatin1String("mesh")) {
        QString detailErr;
        Ogre::MeshPtr loaded;
        const bool ok = loadAndFillOgreInspect(
            info,
            [filePath, &loaded](const std::string& /*mn*/) -> Ogre::MeshPtr {
                const QFileInfo fi(filePath);
                const Ogre::String meshResName = fi.fileName().toStdString();
                const Ogre::String meshGroup   = fi.absolutePath().toStdString();
                Ogre::ResourceGroupManager& rgm = Ogre::ResourceGroupManager::getSingleton();
                if (!rgm.resourceGroupExists(meshGroup)) {
                    rgm.createResourceGroup(meshGroup);
                    rgm.addResourceLocation(meshGroup, "FileSystem", meshGroup);
                    rgm.initialiseResourceGroup(meshGroup);
                }
                if (auto existing = Ogre::MeshManager::getSingleton().getByName(meshResName, meshGroup))
                    Ogre::MeshManager::getSingleton().remove(existing);
                loaded = Ogre::MeshManager::getSingleton().load(meshResName, meshGroup);
                return loaded;
            },
            &detailErr);
        if (ok && loaded && loaded->hasSkeleton())
            fillAssetInfoFromOgreSkeleton(info, loaded->getSkeleton());
        // Run the standard ACMR path (loads through MeshImporterExporter into
        // its own scene). Safe to call after the inspect helper above because
        // computeWeightedAcmrViaOgre uses a separate per-scan clear cycle.
        computeWeightedAcmrViaOgre(filePath, info);
        info.filePath = filePath;
        info.relativePath = QDir(scanRoot).relativeFilePath(filePath);
        info.format = extLower;
        info.fileSize = QFileInfo(filePath).size();
        return info;
    }

    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    // Triangulate for consistent vertex/face counts; otherwise minimal processing.
    unsigned int readFlags = aiProcess_Triangulate | aiProcess_ValidateDataStructure;
    const aiScene* scene = importer.ReadFile(filePath.toStdString(), readFlags);

    if (isAssimpResultLoadFailure(scene, importer.GetErrorString(), nullptr) &&
        (pathEndsWithInsensitive(filePath, QLatin1String(".fbx")) ||
         pathEndsWithInsensitive(filePath, QLatin1String(".fbxa")))) {
        readFlags = aiProcess_Triangulate | aiProcess_ValidateDataStructure |
                    aiProcess_LimitBoneWeights | aiProcess_PopulateArmatureData |
                    aiProcess_GlobalScale;
        scene = importer.ReadFile(filePath.toStdString(), readFlags);
    }

    // A null scene is a true load failure.  Do NOT treat AI_SCENE_FLAGS_INCOMPLETE
    // as fatal: Assimp sets it on many valid FBX files (e.g. Unreal/ Mixamo
    // animation takes with no mesh geometry).  Match AssimpToOgreImporter: use
    // mesh/animation presence as the authoritative check.
    if (isAssimpResultLoadFailure(scene, importer.GetErrorString(), &info.errorMessage)) {
        info.loadError = true;
        return info;
    }

    info.meshCount     = scene->mNumMeshes;
    info.materialCount = scene->mNumMaterials;
    info.animationCount = scene->mNumAnimations;

    // Vertex / face / skeleton metadata from Assimp's scene graph. ACMR is
    // computed via Ogre below (slice C2) so the numbers line up with what
    // the editor's in-app validator reports.
    std::set<std::string> uniqueBones;
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* mesh = scene->mMeshes[i];
        if (!mesh)
            continue;
        info.vertexCount += mesh->mNumVertices;
        info.faceCount   += mesh->mNumFaces;
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            if (!mesh->mBones[b])
                continue;
            uniqueBones.insert(mesh->mBones[b]->mName.C_Str());
        }
    }
    info.boneCount  = static_cast<unsigned int>(uniqueBones.size());
    info.hasSkeleton = !uniqueBones.empty();
    for (const auto& boneName : uniqueBones)
        info.boneNames.append(QString::fromStdString(boneName));

    // Animation details
    for (unsigned i = 0; i < scene->mNumAnimations; ++i) {
        const aiAnimation* anim = scene->mAnimations[i];
        if (!anim)
            continue;
        info.animationNames.append(QString::fromUtf8(anim->mName.C_Str()));

        double ticksPerSec = anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 25.0;
        info.animationDurations.append(anim->mDuration / ticksPerSec);

        unsigned maxKeys = 0;
        if (anim->mChannels) {
            for (unsigned c = 0; c < anim->mNumChannels; ++c) {
                const aiNodeAnim* ch = anim->mChannels[c];
                if (!ch)
                    continue;
                maxKeys = std::max(maxKeys, ch->mNumPositionKeys);
                maxKeys = std::max(maxKeys, ch->mNumRotationKeys);
                maxKeys = std::max(maxKeys, ch->mNumScalingKeys);
            }
        }
        info.animationKeyframeCounts.append(static_cast<int>(maxKeys));

        // Always compute redundant-keyframe counts under the default
        // (Balanced) tolerances. The scan rule re-evaluates with its own
        // configured tolerances if it's enabled — these defaults just keep
        // the JSON report informative for tooling that consumes it.
        int animTotal = 0, animRedundant = 0;
        analyzeAnimationRedundancy(anim, RedundancyTolerances{}, &animTotal, &animRedundant);
        info.totalKeyframes     += animTotal;
        info.redundantKeyframes += animRedundant;
    }

    info.animationRedundantKeyframeRatio = (info.totalKeyframes > 0)
        ? static_cast<double>(info.redundantKeyframes) / static_cast<double>(info.totalKeyframes)
        : 0.0;

    // Material names + texture references
    for (unsigned i = 0; i < scene->mNumMaterials; ++i) {
        const aiMaterial* mat = scene->mMaterials[i];

        aiString name;
        mat->Get(AI_MATKEY_NAME, name);
        info.materialNames.append(QString::fromUtf8(name.C_Str()));

        // Iterate all texture types
        for (int type = aiTextureType_DIFFUSE; type <= aiTextureType_UNKNOWN; ++type) {
            unsigned count = mat->GetTextureCount(static_cast<aiTextureType>(type));
            for (unsigned j = 0; j < count; ++j) {
                aiString texPath;
                mat->GetTexture(static_cast<aiTextureType>(type), j, &texPath);
                QString tp = QString::fromUtf8(texPath.C_Str());
                info.textureRefCount++;
                if (tp.startsWith('*')) {
                    // Embedded texture (Assimp convention: "*0", "*1", ...)
                    info.hasEmbeddedTextures = true;
                } else if (!tp.isEmpty()) {
                    info.texturePaths.append(tp);
                }
            }
        }
    }

    // Deduplicate embedded-texture flag from scene
    if (scene->mNumTextures > 0)
        info.hasEmbeddedTextures = true;

    // Slice C2: compute ACMR via Ogre so the scan numbers line up with the
    // editor's in-app validator. Failures are non-fatal — the rest of the
    // AssetInfo stays valid and we just leave weightedAcmr at 0.
    computeWeightedAcmrViaOgre(filePath, info);

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
    // Walks the animation channels under the configured tolerances and warns if
    // a meaningful share could be folded out by `qtmesh anim --simplify`.
    if (config.redundantKeyframesPctThreshold > 0.0 && asset.animationCount > 0) {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(
            asset.filePath.toStdString(),
            aiProcess_ValidateDataStructure);
        // Use the same accept-policy as inspectAsset so animation-only FBX
        // (which carries AI_SCENE_FLAGS_INCOMPLETE) still goes through the rule.
        QString reimportErr;
        const bool readFailed = ScanEngine::isAssimpResultLoadFailure(
            scene, importer.GetErrorString(), &reimportErr);
        if (!readFailed) {
            RedundancyTolerances tol;
            tol.translation = config.redundantKeyframesTranslationTol;
            tol.rotationDeg = config.redundantKeyframesRotationDegTol;
            tol.scale       = config.redundantKeyframesScaleTol;

            int total = 0;
            int redundant = 0;
            for (unsigned i = 0; i < scene->mNumAnimations; ++i) {
                if (!scene->mAnimations[i])
                    continue;
                int t = 0, r = 0;
                analyzeAnimationRedundancy(scene->mAnimations[i], tol, &t, &r);
                total += t;
                redundant += r;
            }

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
