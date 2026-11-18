/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "BoneTextureBaker.h"

#include "MeshImporterExporter.h"
#include "MinimalEXRWriter.h"
#include "SentryReporter.h"
#include "VATShaderEmitter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>
#include <OgreSubMesh.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace BoneTexture {

// ---------------------------------------------------------------------------
// Matrix helpers
// ---------------------------------------------------------------------------

void Mat3x4::transformPoint(const float in[3], float out[3]) const
{
    for (int r = 0; r < 3; ++r)
        out[r] = at(r, 0) * in[0] + at(r, 1) * in[1] + at(r, 2) * in[2] + at(r, 3);
}

void Mat3x4::transformVector(const float in[3], float out[3]) const
{
    for (int r = 0; r < 3; ++r)
        out[r] = at(r, 0) * in[0] + at(r, 1) * in[1] + at(r, 2) * in[2];
}

bool Mat3x4::isFinite() const
{
    return std::all_of(m.begin(), m.end(), [](float v) { return std::isfinite(v); });
}

Mat3x4 lerp(const Mat3x4& a, const Mat3x4& b, float t)
{
    Mat3x4 r;
    for (size_t i = 0; i < r.m.size(); ++i)
        r.m[i] = a.m[i] * (1.0f - t) + b.m[i] * t;
    return r;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

int frameCountFor(double duration, double fps)
{
    if (!(duration > 0.0) || !(fps > 0.0)) return 1;
    // Saturate instead of overflowing: anything this large is rejected by
    // the texture-size check long before it would be sampled.
    const double intervals = std::round(duration * fps);
    if (!(intervals < static_cast<double>(std::numeric_limits<int>::max() - 1)))
        return std::numeric_limits<int>::max();
    return std::max(1, static_cast<int>(intervals)) + 1;
}

int layoutClips(std::vector<Clip>& clips)
{
    int row = 0;
    for (Clip& c : clips) {
        c.firstRow = row;
        row += std::max(0, c.frameCount);
    }
    return row;
}

QString validate(const BakedClips& baked, int maxTextureSize)
{
    if (baked.bones.empty())
        return QStringLiteral("no bones to bake");
    if (baked.clips.empty())
        return QStringLiteral("no clips to bake");
    if (baked.frames.size() != baked.clips.size())
        return QStringLiteral("frame data covers %1 clips, expected %2")
            .arg(baked.frames.size()).arg(baked.clips.size());

    const int boneCount = static_cast<int>(baked.bones.size());
    const int width = textureWidth(boneCount);
    if (width > maxTextureSize)
        return QStringLiteral(
            "%1 bones need a %2-texel-wide texture, over the %3 limit - remove "
            "unused helper bones or raise the texture size limit")
            .arg(boneCount).arg(width).arg(maxTextureSize);

    int expectedRow = 0;
    for (size_t ci = 0; ci < baked.clips.size(); ++ci) {
        const Clip& clip = baked.clips[ci];
        if (clip.frameCount < 1)
            return QStringLiteral("clip '%1' has no frames").arg(clip.name);
        if (clip.firstRow != expectedRow)
            return QStringLiteral("clip '%1' starts at row %2, expected %3 (call layoutClips)")
                .arg(clip.name).arg(clip.firstRow).arg(expectedRow);
        expectedRow += clip.frameCount;
        const auto& frames = baked.frames[ci];
        if (static_cast<int>(frames.size()) != clip.frameCount)
            return QStringLiteral("clip '%1' has %2 frames of data, expected %3")
                .arg(clip.name).arg(frames.size()).arg(clip.frameCount);
        for (size_t f = 0; f < frames.size(); ++f) {
            if (static_cast<int>(frames[f].size()) != boneCount)
                return QStringLiteral("clip '%1' frame %2 has %3 bone matrices, expected %4")
                    .arg(clip.name).arg(f).arg(frames[f].size()).arg(boneCount);
            for (size_t b = 0; b < frames[f].size(); ++b) {
                if (!frames[f][b].isFinite())
                    return QStringLiteral("clip '%1' frame %2: bone '%3' has a non-finite matrix "
                                          "(invalid bind pose or animation keys)")
                        .arg(clip.name).arg(f).arg(baked.bones[b].name);
            }
        }
    }
    if (expectedRow > maxTextureSize)
        return QStringLiteral(
            "%1 frames in total exceed the %2-row texture limit - lower the fps "
            "or bake fewer clips per texture")
            .arg(expectedRow).arg(maxTextureSize);
    return {};
}

std::vector<float> pack(const BakedClips& baked, int* outWidth, int* outHeight)
{
    if (!validate(baked, std::numeric_limits<int>::max()).isEmpty()) return {};
    const int boneCount = static_cast<int>(baked.bones.size());
    const int width = textureWidth(boneCount);
    int height = 0;
    for (const Clip& c : baked.clips) height += c.frameCount;

    std::vector<float> texels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0.0f);
    for (size_t ci = 0; ci < baked.clips.size(); ++ci) {
        const Clip& clip = baked.clips[ci];
        for (int f = 0; f < clip.frameCount; ++f) {
            const size_t rowBase = static_cast<size_t>(clip.firstRow + f) * static_cast<size_t>(width);
            for (int b = 0; b < boneCount; ++b) {
                const Mat3x4& m = baked.frames[ci][static_cast<size_t>(f)][static_cast<size_t>(b)];
                for (int r = 0; r < kTexelsPerBone; ++r) {
                    float* t = &texels[(rowBase + static_cast<size_t>(b * kTexelsPerBone + r)) * 4u];
                    for (int c = 0; c < 4; ++c) t[c] = m.at(r, c);
                }
            }
        }
    }
    if (outWidth) *outWidth = width;
    if (outHeight) *outHeight = height;
    return texels;
}

Mat3x4 unpack(const std::vector<float>& texels, int width, int bone, int row)
{
    Mat3x4 m;
    const size_t rowBase = static_cast<size_t>(row) * static_cast<size_t>(width);
    for (int r = 0; r < kTexelsPerBone; ++r) {
        const size_t idx = (rowBase + static_cast<size_t>(bone * kTexelsPerBone + r)) * 4u;
        if (idx + 3 >= texels.size()) return Mat3x4::identity();
        for (int c = 0; c < 4; ++c) m.at(r, c) = texels[idx + static_cast<size_t>(c)];
    }
    return m;
}

double rowForTime(const Clip& clip, double t, bool loop)
{
    if (clip.frameCount <= 1 || !(clip.duration > 0.0))
        return clip.firstRow;
    double local = t;
    if (loop) {
        local = std::fmod(local, clip.duration);
        if (local < 0.0) local += clip.duration;
    } else {
        local = std::clamp(local, 0.0, clip.duration);
    }
    return clip.firstRow + local / clip.duration * (clip.frameCount - 1);
}

Mat3x4 sample(const std::vector<float>& texels, int width, const Clip& clip,
              int bone, double t, bool loop)
{
    const double row = rowForTime(clip, t, loop);
    if (clip.frameCount <= 1)
        return unpack(texels, width, bone, clip.firstRow);
    const int lastPair = clip.firstRow + clip.frameCount - 2;
    const int r0 = std::min(static_cast<int>(std::floor(row)), lastPair);
    const float frac = static_cast<float>(row - r0);
    return lerp(unpack(texels, width, bone, r0), unpack(texels, width, bone, r0 + 1), frac);
}

Mat3x4 crossfade(const std::vector<float>& texels, int width,
                 const Clip& a, double ta, const Clip& b, double tb,
                 int bone, float weightB, bool loop)
{
    return lerp(sample(texels, width, a, bone, ta, loop),
                sample(texels, width, b, bone, tb, loop),
                std::clamp(weightB, 0.0f, 1.0f));
}

void skinVertex(const std::vector<Mat3x4>& boneMatrices,
                const float position[3], const float* normal,
                const int indices[kMaxInfluences], const float weights[kMaxInfluences],
                float outPosition[3], float* outNormal)
{
    Mat3x4 blended;
    blended.m.fill(0.0f);
    float total = 0.0f;
    for (int i = 0; i < kMaxInfluences; ++i) {
        const int idx = indices[i];
        const float w = weights[i];
        if (w <= 0.0f || idx < 0 || idx >= static_cast<int>(boneMatrices.size())) continue;
        const Mat3x4& m = boneMatrices[static_cast<size_t>(idx)];
        for (size_t k = 0; k < blended.m.size(); ++k) blended.m[k] += m.m[k] * w;
        total += w;
    }
    if (total <= 0.0f) {
        blended = Mat3x4::identity();
    } else {
        for (float& v : blended.m) v /= total;
    }
    blended.transformPoint(position, outPosition);
    if (normal && outNormal) {
        blended.transformVector(normal, outNormal);
        const float len = std::sqrt(outNormal[0] * outNormal[0] + outNormal[1] * outNormal[1]
                                    + outNormal[2] * outNormal[2]);
        if (len > 1e-20f)
            for (int i = 0; i < 3; ++i) outNormal[i] /= len;
    }
}

// ---------------------------------------------------------------------------
// Sidecar + files
// ---------------------------------------------------------------------------

QStringList encodingIds()
{
    return { QStringLiteral("rgba32f"), QStringLiteral("rgba16f") };
}

bool isValidEncoding(const QString& id)
{
    return encodingIds().contains(id.trimmed().toLower());
}

QString buildSidecarJson(const BakedClips& baked, const QString& textureFile,
                         const QString& encoding, int width, int height)
{
    QJsonObject texture{
        { QStringLiteral("file"), textureFile },
        { QStringLiteral("width"), width },
        { QStringLiteral("height"), height },
        { QStringLiteral("encoding"), encoding },
        { QStringLiteral("texelsPerBone"), kTexelsPerBone },
        { QStringLiteral("layout"),
          QStringLiteral("x = bone*3 + r, y = clip.firstRow + frame; texel = matrix row r "
                         "(m[r][0], m[r][1], m[r][2], m[r][3])") },
        { QStringLiteral("origin"), QStringLiteral("row 0 = first scanline (top)") },
        { QStringLiteral("filter"), QStringLiteral("point (texelFetch); interpolate rows in the shader") },
    };

    QJsonObject matrix{
        { QStringLiteral("convention"),
          QStringLiteral("skin = animatedGlobal * inverseBind; 3x4 affine rows, row 3 = (0,0,0,1); "
                         "p' = sum_i w_i * M_i * vec4(p, 1)") },
        { QStringLiteral("space"),
          QStringLiteral("mesh-local, Ogre Y-up right-handed, source units; scene-node transform not included") },
        { QStringLiteral("bindPose"),
          QStringLiteral("the mesh is exported in its bind pose; positions are skinned from it") },
        { QStringLiteral("interpolation"),
          QStringLiteral("component-wise lerp of adjacent rows (not rigid: large per-frame "
                         "rotations shrink slightly)") },
    };

    QJsonArray bones;
    for (size_t i = 0; i < baked.bones.size(); ++i) {
        bones.append(QJsonObject{
            { QStringLiteral("index"), static_cast<int>(i) },
            { QStringLiteral("name"), baked.bones[i].name },
            { QStringLiteral("parent"), baked.bones[i].parent },
        });
    }

    QJsonArray clips;
    for (const Clip& c : baked.clips) {
        clips.append(QJsonObject{
            { QStringLiteral("name"), c.name },
            { QStringLiteral("firstRow"), c.firstRow },
            { QStringLiteral("frameCount"), c.frameCount },
            { QStringLiteral("duration"), c.duration },
            { QStringLiteral("fps"), c.fps },
            { QStringLiteral("loop"), c.loop },
        });
    }

    QJsonObject skinning{
        { QStringLiteral("maxInfluences"), kMaxInfluences },
        { QStringLiteral("sourceMaxInfluences"), baked.sourceMaxInfluences },
        { QStringLiteral("verticesOverLimit"), baked.verticesOverLimit },
        { QStringLiteral("blend"),
          QStringLiteral("linear blend skinning; weights beyond the strongest 4 dropped and renormalised") },
        { QStringLiteral("boneIndices"),
          QStringLiteral("index into 'bones' (Ogre bone handle order); map an exported skin's "
                         "joints to it by name") },
    };

    QJsonObject root{
        { QStringLiteral("schema"), QStringLiteral("qtmesh-bone-texture-v1") },
        { QStringLiteral("producer"), QStringLiteral("QtMeshEditor") },
        { QStringLiteral("texture"), texture },
        { QStringLiteral("matrix"), matrix },
        { QStringLiteral("bones"), bones },
        { QStringLiteral("clips"), clips },
        { QStringLiteral("skinning"), skinning },
    };
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

namespace detail {
bool g_failSidecarForTest = false;
bool g_failMeshExportForTest = false;
bool g_failRollbackForTest = false;
bool g_failMeshPublishForTest = false;
void setSidecarWriteFailureForTest(bool fail) { g_failSidecarForTest = fail; }
void setMeshExportFailureForTest(bool fail) { g_failMeshExportForTest = fail; }
void setRollbackFailureForTest(bool fail) { g_failRollbackForTest = fail; }
void setMeshPublishFailureForTest(bool fail) { g_failMeshPublishForTest = fail; }
} // namespace detail

namespace {

bool saveFileAtomically(const QString& path, const QByteArray& bytes)
{
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
}

/// Put the previous texture back after a failed publication (or remove the
/// new one when there was none). If that fails, the stale sidecar is
/// removed so it cannot describe the new texture; if THAT fails too, the
/// error says so plainly: the folder then holds a mismatched pair.
QString rollbackPair(const QString& texPath, const QString& jsonPath,
                     bool hadPreviousTexture, const QByteArray& previousTexture,
                     const QString& cause)
{
    const bool restored = !detail::g_failRollbackForTest
        && (hadPreviousTexture ? saveFileAtomically(texPath, previousTexture)
                               : QFile::remove(texPath));
    if (restored)
        return cause + QStringLiteral(" (the previous bake was left as it was)");
    const bool sidecarRemoved = !detail::g_failRollbackForTest
        && (!QFileInfo::exists(jsonPath) || QFile::remove(jsonPath));
    if (sidecarRemoved)
        return cause + QStringLiteral("; the previous texture could not be restored, so its "
                                      "sidecar %1 was removed - re-run the bake").arg(jsonPath);
    return cause + QStringLiteral("; the previous texture could not be restored and the sidecar "
                                  "%1 could not be removed - %1 does NOT match %2: delete both "
                                  "and re-run the bake").arg(jsonPath, texPath);
}

/// One writer per (folder, basename) at a time, across processes: the
/// texture, sidecar and mesh of a bake are published as one unit, and two
/// bakes interleaving their commits would otherwise leave one bake's
/// texture next to the other's sidecar.
class PublishLock
{
public:
    PublishLock(const QString& outputDir, const QString& basename)
        : m_lock(QDir(outputDir).filePath(QStringLiteral(".") + basename + QStringLiteral("_bones.lock")))
    {
        m_lock.setStaleLockTime(5 * 60 * 1000);
        m_locked = m_lock.tryLock(2 * 60 * 1000);
    }
    bool locked() const { return m_locked; }
private:
    QLockFile m_lock;
    bool m_locked = false;
};

WriteResult writeUnlocked(const BakedClips& baked, const QString& outputDir,
                          const QString& basename, const QString& encoding,
                          int maxTextureSize);

} // namespace

WriteResult write(const BakedClips& baked, const QString& outputDir,
                  const QString& basename, const QString& encoding,
                  int maxTextureSize)
{
    if (!outputDir.isEmpty() && !basename.trimmed().isEmpty() && QDir().mkpath(outputDir)) {
        PublishLock lock(outputDir, basename);
        if (!lock.locked()) {
            WriteResult res;
            res.error = QStringLiteral("another bake is writing %1 in %2").arg(basename, outputDir);
            return res;
        }
        return writeUnlocked(baked, outputDir, basename, encoding, maxTextureSize);
    }
    return writeUnlocked(baked, outputDir, basename, encoding, maxTextureSize);
}

namespace {

WriteResult writeUnlocked(const BakedClips& baked, const QString& outputDir,
                          const QString& basename, const QString& encoding,
                          int maxTextureSize)
{
    WriteResult res;
    const QString enc = encoding.trimmed().toLower().isEmpty()
        ? QStringLiteral("rgba32f") : encoding.trimmed().toLower();
    if (!isValidEncoding(enc)) {
        res.error = QStringLiteral("unknown encoding '%1' (accepted: %2)")
                        .arg(encoding, encodingIds().join(QStringLiteral(", ")));
        return res;
    }
    if (outputDir.isEmpty()) {
        res.error = QStringLiteral("output directory is required");
        return res;
    }
    if (basename.trimmed().isEmpty()) {
        res.error = QStringLiteral("basename is required");
        return res;
    }
    if (const QString err = validate(baked, maxTextureSize); !err.isEmpty()) {
        res.error = err;
        return res;
    }
    if (!QDir().mkpath(outputDir)) {
        res.error = QStringLiteral("cannot create output directory %1").arg(outputDir);
        return res;
    }

    int width = 0, height = 0;
    const std::vector<float> texels = pack(baked, &width, &height);
    if (texels.empty()) {
        res.error = QStringLiteral("failed to pack the bone texture");
        return res;
    }

    if (enc == QStringLiteral("rgba16f")) {
        // A finite float past the half range would be stored as infinity and
        // the shader would read an infinite translation: refuse instead.
        for (size_t i = 0; i < texels.size(); ++i) {
            if (std::isinf(MinimalEXR::halfToFloat(MinimalEXR::floatToHalf(texels[i])))) {
                const size_t texel = i / 4;
                const int bone = static_cast<int>((texel % static_cast<size_t>(width)) / kTexelsPerBone);
                res.error = QStringLiteral("bone '%1' has a matrix value (%2) outside the half-float "
                                           "range (+/-65504) - use the rgba32f encoding")
                                .arg(baked.bones[static_cast<size_t>(bone)].name).arg(texels[i]);
                return res;
            }
        }
    }

    const QDir dir(outputDir);
    const QString texName = basename + QStringLiteral("_bones.exr");
    const QString texPath = dir.filePath(texName);
    const QString jsonPath = dir.filePath(basename + QStringLiteral("_bones.json"));

    // Stage the texture under a name unique to this bake (two bakes into the
    // same folder must not share a staging file), then publish texture and
    // sidecar as a PAIR: both through QSaveFile (an existing file is only
    // replaced once its replacement is complete), and if the sidecar cannot
    // be published the previous texture is put back, so an old sidecar never
    // describes a new texture.
    QByteArray texBytes;
    {
        QTemporaryFile staging(dir.filePath(basename + QStringLiteral("_bones.XXXXXX.exr.tmp")));
        if (!staging.open()) {
            res.error = QStringLiteral("cannot create a staging file in %1").arg(outputDir);
            return res;
        }
        const QString stagingPath = staging.fileName();
        staging.close();
        const bool wrote = (enc == QStringLiteral("rgba16f"))
            ? MinimalEXR::writeRGBA16F(stagingPath, width, height, texels)
            : MinimalEXR::writeRGBA32F(stagingPath, width, height, texels);
        QFile f(stagingPath);
        if (!wrote || !f.open(QIODevice::ReadOnly)) {
            res.error = QStringLiteral("failed to write the bone texture %1").arg(texPath);
            return res;
        }
        texBytes = f.readAll();
    }   // the staging file is removed here
    const QByteArray jsonBytes = buildSidecarJson(baked, texName, enc, width, height).toUtf8();

    // Keep the previous texture so a failed sidecar can put it back. If it
    // exists but cannot be read, stop BEFORE touching anything: without it
    // a failure later could not be undone.
    QByteArray previousTexture;
    const bool hadPreviousTexture = QFileInfo::exists(texPath);
    if (hadPreviousTexture) {
        QFile old(texPath);
        if (!old.open(QIODevice::ReadOnly)) {
            res.error = QStringLiteral("cannot read the existing %1 - refusing to replace it "
                                       "(check its permissions)").arg(texPath);
            return res;
        }
        previousTexture = old.readAll();
    }
    if (!saveFileAtomically(texPath, texBytes)) {
        res.error = QStringLiteral("failed to write the bone texture %1").arg(texPath);
        return res;
    }
    if (detail::g_failSidecarForTest || !saveFileAtomically(jsonPath, jsonBytes)) {
        res.error = rollbackPair(texPath, jsonPath, hadPreviousTexture, previousTexture,
                                 QStringLiteral("failed to write the bone texture sidecar %1").arg(jsonPath));
        return res;
    }

    res.ok = true;
    res.texturePath = texPath;
    res.jsonPath = jsonPath;
    res.width = width;
    res.height = height;
    return res;
}

} // namespace

// ---------------------------------------------------------------------------
// Ogre adapter
// ---------------------------------------------------------------------------

namespace {

Mat3x4 fromAffine(const Ogre::Affine3& a)
{
    Mat3x4 m;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            m.at(r, c) = static_cast<float>(a[r][c]);
    return m;
}

bool finite3(const Ogre::Vector3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Influence statistics over every vertex-data owner (shared + per submesh).
void countInfluences(const Ogre::Mesh* mesh, BakedClips* out)
{
    auto tally = [out](const Ogre::Mesh::VertexBoneAssignmentList& list) {
        std::map<unsigned int, int> perVertex;
        for (const auto& kv : list)
            if (kv.second.weight > 0.0f) ++perVertex[kv.second.vertexIndex];
        for (const auto& pv : perVertex) {
            out->sourceMaxInfluences = std::max(out->sourceMaxInfluences, pv.second);
            if (pv.second > kMaxInfluences) ++out->verticesOverLimit;
        }
    };
    tally(mesh->getBoneAssignments());
    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        const Ogre::SubMesh* sub = mesh->getSubMesh(i);
        if (sub && !sub->useSharedVertices) tally(sub->getBoneAssignments());
    }
}

struct StateSnapshot {
    Ogre::AnimationState* state;
    bool enabled;
    bool loop;
    float time;
    float weight;
    std::vector<float> blendMask;   ///< Empty = no mask.
};

} // namespace

bool sampleEntity(Ogre::Entity* entity, const SampleOptions& opts,
                  BakedClips* out, QString* error)
{
    auto fail = [error](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    if (!out) return fail(QStringLiteral("no output"));
    *out = BakedClips{};
    if (!entity) return fail(QStringLiteral("entity is null"));
    if (!(opts.fps > 0.0)) return fail(QStringLiteral("fps must be > 0"));
    if (!entity->hasSkeleton())
        return fail(QStringLiteral("entity has no skeleton - bone texture baking needs a rigged "
                                   "mesh (use the vertex VAT for unrigged animation)"));

    Ogre::SkeletonInstance* skel = entity->getSkeleton();
    const unsigned short boneCount = skel->getNumBones();
    if (boneCount == 0) return fail(QStringLiteral("skeleton has no bones"));

    for (unsigned short i = 0; i < boneCount; ++i) {
        const Ogre::Bone* bone = skel->getBone(i);
        const Ogre::Vector3& invScale = bone->_getBindingPoseInverseScale();
        const Ogre::Quaternion& invRot = bone->_getBindingPoseInverseOrientation();
        const bool rotFinite = std::isfinite(invRot.w) && std::isfinite(invRot.x)
            && std::isfinite(invRot.y) && std::isfinite(invRot.z);
        if (!finite3(invScale) || !finite3(bone->_getBindingPoseInversePosition()) || !rotFinite
            || invScale.x == 0.0f || invScale.y == 0.0f || invScale.z == 0.0f)
            return fail(QStringLiteral("bone '%1' has an invalid bind pose (non-finite or "
                                       "zero-scale inverse bind matrix) - fix the rig's rest pose "
                                       "and re-import")
                            .arg(QString::fromStdString(bone->getName())));
        Bone b;
        b.name = QString::fromStdString(bone->getName());
        if (const auto* parent = dynamic_cast<const Ogre::Bone*>(bone->getParent()))
            b.parent = parent->getHandle();
        out->bones.push_back(b);
    }

    QStringList clipNames = opts.clips;
    if (clipNames.isEmpty()) {
        for (unsigned short i = 0; i < skel->getNumAnimations(); ++i)
            clipNames << QString::fromStdString(skel->getAnimation(i)->getName());
    }
    if (clipNames.isEmpty())
        return fail(QStringLiteral("the skeleton has no animations to bake"));

    Ogre::AnimationStateSet* states = entity->getAllAnimationStates();
    if (!states) return fail(QStringLiteral("entity has no animation states"));
    for (const QString& name : clipNames) {
        const std::string n = name.toStdString();
        if (!skel->hasAnimation(n) || !states->hasAnimationState(n))
            return fail(QStringLiteral("skeletal animation '%1' not found").arg(name));
    }

    // Preflight the texture size BEFORE sampling anything: a huge fps would
    // otherwise allocate gigabytes of matrices before write() refuses them.
    {
        const int width = textureWidth(boneCount);
        if (width > opts.maxTextureSize)
            return fail(QStringLiteral("%1 bones need a %2-texel-wide texture, over the %3 limit - "
                                       "remove unused helper bones or raise the texture size limit")
                            .arg(boneCount).arg(width).arg(opts.maxTextureSize));
        double rows = 0.0;
        for (const QString& name : clipNames) {
            const double d = states->getAnimationState(name.toStdString())->getLength();
            rows += (d > 0.0) ? std::max(1.0, std::round(d * opts.fps)) + 1.0 : 1.0;
        }
        if (rows > opts.maxTextureSize)
            return fail(QStringLiteral("%1 frames in total exceed the %2-row texture limit - lower "
                                       "the fps or bake fewer clips per texture")
                            .arg(rows, 0, 'f', 0).arg(opts.maxTextureSize));
    }

    // Snapshot every state so the caller's playback is untouched: flags,
    // time, weight, per-bone blend masks (a layered motion graph or a
    // masked pose would otherwise attenuate bones in the bake) and the
    // ORDER of the enabled list, which Ogre blends in.
    std::vector<StateSnapshot> snapshot;
    for (auto it = states->getAnimationStateIterator(); it.hasMoreElements();) {
        Ogre::AnimationState* s = it.getNext();
        StateSnapshot snap{ s, s->getEnabled(), s->getLoop(), s->getTimePosition(), s->getWeight(), {} };
        if (s->hasBlendMask()) snap.blendMask = *s->getBlendMask();
        snapshot.push_back(std::move(snap));
    }
    std::vector<Ogre::AnimationState*> enabledOrder;
    for (auto it = states->getEnabledAnimationStateIterator(); it.hasMoreElements();)
        enabledOrder.push_back(it.getNext());
    struct Restore {
        std::vector<StateSnapshot>& snap;
        std::vector<Ogre::AnimationState*>& enabledOrder;
        Ogre::SkeletonInstance* skel;
        Ogre::AnimationStateSet* states;
        ~Restore() {
            for (const auto& s : snap) {
                s.state->setEnabled(false);
                s.state->setLoop(s.loop);
                s.state->setTimePosition(s.time);
                s.state->setWeight(s.weight);
                if (!s.blendMask.empty()) {
                    s.state->createBlendMask(s.blendMask.size());
                    s.state->_setBlendMask(&s.blendMask);
                } else if (s.state->hasBlendMask()) {
                    s.state->destroyBlendMask();
                }
            }
            for (Ogre::AnimationState* s : enabledOrder) s->setEnabled(true);
            skel->setAnimationState(*states);
            states->_notifyDirty();
        }
    } restore{ snapshot, enabledOrder, skel, states };
    for (const auto& s : snapshot)
        if (s.state->hasBlendMask()) s.state->destroyBlendMask();

    countInfluences(entity->getMesh().get(), out);

    std::vector<Ogre::Affine3> mats(boneCount);
    for (const QString& name : clipNames) {
        const std::string n = name.toStdString();
        for (const auto& s : snapshot) s.state->setEnabled(false);
        Ogre::AnimationState* state = states->getAnimationState(n);
        const bool wasLooping = std::find_if(snapshot.begin(), snapshot.end(),
            [state](const StateSnapshot& s) { return s.state == state; })->loop;
        state->setEnabled(true);
        state->setWeight(1.0f);
        // Ogre wraps setTimePosition with fmod while looping, so t == length
        // would read frame 0 (the VAT lesson). Sample with clamp semantics.
        state->setLoop(false);

        Clip clip;
        clip.name = name;
        clip.duration = state->getLength();
        clip.frameCount = frameCountFor(clip.duration, opts.fps);
        clip.fps = clip.frameCount > 1 ? (clip.frameCount - 1) / clip.duration : 0.0;
        clip.loop = wasLooping;

        std::vector<std::vector<Mat3x4>> frames;
        frames.reserve(static_cast<size_t>(clip.frameCount));
        for (int f = 0; f < clip.frameCount; ++f) {
            const double t = clip.frameCount == 1 ? 0.0
                : clip.duration * static_cast<double>(f) / static_cast<double>(clip.frameCount - 1);
            state->setTimePosition(static_cast<Ogre::Real>(t));
            skel->setAnimationState(*states);
            skel->_getBoneMatrices(mats.data());
            std::vector<Mat3x4> row;
            row.reserve(boneCount);
            for (unsigned short b = 0; b < boneCount; ++b) {
                row.push_back(fromAffine(mats[b]));
                if (!row.back().isFinite())
                    return fail(QStringLiteral("clip '%1' at %2 s: bone '%3' produced a non-finite "
                                               "matrix (check the clip's keys)")
                                    .arg(name).arg(t).arg(out->bones[b].name));
            }
            frames.push_back(std::move(row));
        }
        out->clips.push_back(clip);
        out->frames.push_back(std::move(frames));
    }
    layoutClips(out->clips);
    return true;
}

// ---------------------------------------------------------------------------
// Surface core
// ---------------------------------------------------------------------------

QString modeId()
{
    return QStringLiteral("bone-texture");
}

bool isModeId(const QString& id)
{
    const QString k = id.trimmed().toLower();
    return k == QLatin1String("bone-texture") || k == QLatin1String("bone_texture")
        || k == QLatin1String("bonetexture") || k == QLatin1String("bones")
        || k == QLatin1String("bta");
}

bool normaliseEncoding(const QString& id, QString* out)
{
    const QString k = id.trimmed().toLower();
    QString r;
    if (k.isEmpty() || k == QLatin1String("rgba32f") || k == QLatin1String("exr")
        || k == QLatin1String("float32") || k == QLatin1String("f32"))
        r = QStringLiteral("rgba32f");
    else if (k == QLatin1String("rgba16f") || k == QLatin1String("half")
             || k == QLatin1String("float16") || k == QLatin1String("f16"))
        r = QStringLiteral("rgba16f");
    else
        return false;
    if (out) *out = r;
    return true;
}

namespace {

QString sanitisedBasename(const QString& name)
{
    QString b;
    for (const QChar c : name)
        b += (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'))
            ? c : QLatin1Char('_');
    return b.isEmpty() ? QStringLiteral("bones") : b;
}

} // namespace

BakeReport bakeToDirectory(Ogre::Entity* entity, const BakeRequest& req)
{
    BakeReport rep;
    if (!normaliseEncoding(req.encoding, &rep.encoding)) {
        rep.error = QStringLiteral("unknown bone texture encoding '%1' (accepted: %2)")
                        .arg(req.encoding, encodingIds().join(QStringLiteral(", ")));
        return rep;
    }
    if (!entity) { rep.error = QStringLiteral("no entity"); return rep; }
    if (req.outputDir.isEmpty()) { rep.error = QStringLiteral("output directory is required"); return rep; }
    if (!entity->hasSkeleton()) {
        rep.error = QStringLiteral("entity has no skeleton - bone texture baking needs a rigged "
                                   "mesh (use the vertex VAT modes for unrigged animation)");
        return rep;
    }
    if (!QDir().mkpath(req.outputDir)) {
        rep.error = QStringLiteral("cannot create output directory %1").arg(req.outputDir);
        return rep;
    }
    const QString basename = sanitisedBasename(
        req.basename.isEmpty() ? QString::fromStdString(entity->getName()) : req.basename);

    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
        QStringLiteral("Bone texture bake start: clips=%1 fps=%2 encoding=%3 -> %4")
            .arg(req.clips.isEmpty() ? QStringLiteral("all") : req.clips.join(QLatin1Char(',')))
            .arg(req.fps).arg(rep.encoding, req.outputDir));

    SampleOptions so;
    so.clips = req.clips;
    so.fps = req.fps;
    so.maxTextureSize = req.maxTextureSize;
    BakedClips baked;
    QString err;
    if (!sampleEntity(entity, so, &baked, &err)) {
        rep.error = err;
        return rep;
    }
    PublishLock lock(req.outputDir, basename);
    if (!lock.locked()) {
        rep.error = QStringLiteral("another bake is writing %1 in %2").arg(basename, req.outputDir);
        return rep;
    }

    // Stage the mesh first (the exporter also writes its material/texture
    // companions next to it), so an export failure stops the bake before
    // anything in the output folder changes; publish it after the pair.
    QTemporaryDir meshStaging(QDir(req.outputDir).filePath(QStringLiteral(".") + basename
                                                            + QStringLiteral("_mesh_XXXXXX")));
    if (req.exportMesh) {
        const QString staged = meshStaging.isValid()
            ? QDir(meshStaging.path()).filePath(basename + QStringLiteral(".glb")) : QString();
        const int rc = (staged.isEmpty() || detail::g_failMeshExportForTest) ? -1
            : MeshImporterExporter::exporter(entity->getParentSceneNode(), staged,
                                             QStringLiteral("glTF 2.0 Binary (*.glb)"));
        if (rc != 0 || !QFileInfo::exists(staged)) {
            rep.error = QStringLiteral("exporting the rigged mesh failed (rc=%1) - nothing was "
                                       "written; retry, or bake with --no-mesh and export it "
                                       "separately").arg(rc);
            return rep;
        }
    }

    // Keep the current pair so a failed mesh publication can restore it.
    const QString texPath = QDir(req.outputDir).filePath(basename + QStringLiteral("_bones.exr"));
    const QString jsonPath = QDir(req.outputDir).filePath(basename + QStringLiteral("_bones.json"));
    const bool hadTexture = QFileInfo::exists(texPath);
    const bool hadSidecar = QFileInfo::exists(jsonPath);
    QByteArray oldTexture, oldSidecar;
    if (req.exportMesh) {
        QFile t(texPath), j(jsonPath);
        if ((hadTexture && !t.open(QIODevice::ReadOnly)) || (hadSidecar && !j.open(QIODevice::ReadOnly))) {
            rep.error = QStringLiteral("cannot read the existing %1 bake in %2 - refusing to replace it "
                                       "(check its permissions)").arg(basename, req.outputDir);
            return rep;
        }
        if (hadTexture) oldTexture = t.readAll();
        if (hadSidecar) oldSidecar = j.readAll();
    }

    rep.files = writeUnlocked(baked, req.outputDir, basename, rep.encoding, req.maxTextureSize);
    if (!rep.files.ok) {
        rep.error = rep.files.error;
        return rep;
    }
    if (req.exportMesh) {
        // Publish the staged .glb and its companions; the .glb itself last.
        const QDir staged(meshStaging.path());
        const QString glbName = basename + QStringLiteral(".glb");
        QStringList names = staged.entryList(QDir::Files);
        names.removeAll(glbName);
        names.append(glbName);
        for (const QString& name : names) {
            QFile f(staged.filePath(name));
            const bool published = !(detail::g_failMeshPublishForTest && name == glbName)
                && f.open(QIODevice::ReadOnly)
                && saveFileAtomically(QDir(req.outputDir).filePath(name), f.readAll());
            if (published) continue;
            // The texture/sidecar pair is already published: put the previous
            // pair back so the folder never mixes a new pair with an old mesh.
            QString cause = QStringLiteral("could not publish %1 into %2").arg(name, req.outputDir);
            if (hadSidecar && !detail::g_failRollbackForTest && saveFileAtomically(jsonPath, oldSidecar)) {
                rep.error = rollbackPair(texPath, jsonPath, hadTexture, oldTexture, cause);
            } else if (!hadSidecar && !detail::g_failRollbackForTest && QFile::remove(jsonPath)) {
                rep.error = rollbackPair(texPath, jsonPath, hadTexture, oldTexture, cause);
            } else {
                rep.error = cause + QStringLiteral("; the previous sidecar could not be restored - "
                                                  "%1 does NOT match the rest of the bake: delete "
                                                  "%1 and %2 and re-run").arg(jsonPath, texPath);
            }
            return rep;
        }
        const QString meshPath = QDir(req.outputDir).filePath(glbName);
        if (QFileInfo::exists(meshPath)) rep.meshPath = QFileInfo(meshPath).absoluteFilePath();
    }
    rep.boneCount = static_cast<int>(baked.bones.size());
    rep.totalFrames = rep.files.height;
    rep.clips = baked.clips;
    rep.sourceMaxInfluences = baked.sourceMaxInfluences;
    rep.verticesOverLimit = baked.verticesOverLimit;
    if (baked.verticesOverLimit > 0)
        rep.warnings << QStringLiteral("%1 vertices have more than %2 bone influences (up to %3); "
                                       "the shader keeps the strongest %2, renormalised")
                            .arg(baked.verticesOverLimit).arg(kMaxInfluences)
                            .arg(baked.sourceMaxInfluences);

    if (!req.shaderEngines.isEmpty()) {
        QStringList supported, skipped;
        const QStringList capable = VATShaderEmitter::boneTextureEngines();
        for (const QString& e : req.shaderEngines) {
            const QString k = e.trimmed().toLower();
            if (k.isEmpty() || k == QLatin1String("agnostic")) continue;
            (capable.contains(k) ? supported : skipped) << k;
        }
        if (!supported.isEmpty()) {
            rep.shaders = VATShaderEmitter::writeBoneTextureShaders(req.outputDir, supported);
            // Each engine ships a fixed set of files; name any that did not land.
            for (const QString& file : VATShaderEmitter::boneTextureFiles(supported)) {
                const QString dst = QFileInfo(QDir(req.outputDir).filePath(file)).absoluteFilePath();
                if (!rep.shaders.contains(dst))
                    rep.warnings << QStringLiteral("could not write the shader template %1").arg(dst);
            }
            if (supported.contains(QLatin1String("godot")) && rep.clips.size() > kGodotTemplateMaxClips)
                rep.warnings << QStringLiteral("the Godot template addresses %1 clips, this bake has %2 - "
                                               "split the clips into several bakes")
                                    .arg(kGodotTemplateMaxClips).arg(rep.clips.size());
        }
        if (!skipped.isEmpty())
            rep.warnings << QStringLiteral("no bone texture shader template for %1 yet - "
                                           "OpenVAT_README.md describes the texel layout and math")
                                .arg(skipped.join(QStringLiteral(", ")));
    }

    rep.ok = true;
    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
        QStringLiteral("Bone texture bake ok: %1 bones x %2 rows (%3 clips) -> %4")
            .arg(rep.boneCount).arg(rep.totalFrames).arg(rep.clips.size())
            .arg(rep.files.texturePath));
    return rep;
}

QJsonObject reportToJson(const BakeReport& r)
{
    QJsonObject o;
    o[QStringLiteral("ok")] = r.ok;
    o[QStringLiteral("mode")] = modeId();
    if (!r.ok) {
        o[QStringLiteral("error")] = r.error;
        return o;
    }
    o[QStringLiteral("encoding")] = r.encoding;
    o[QStringLiteral("texture")] = r.files.texturePath;
    o[QStringLiteral("sidecar")] = r.files.jsonPath;
    if (!r.meshPath.isEmpty()) o[QStringLiteral("mesh")] = r.meshPath;
    o[QStringLiteral("width")] = r.files.width;
    o[QStringLiteral("height")] = r.files.height;
    o[QStringLiteral("boneCount")] = r.boneCount;
    o[QStringLiteral("totalFrames")] = r.totalFrames;
    const qint64 bytesPerTexel = r.encoding == QLatin1String("rgba16f") ? 8 : 16;
    o[QStringLiteral("textureBytes")] =
        static_cast<double>(static_cast<qint64>(r.files.width) * r.files.height * bytesPerTexel);
    QJsonArray clips;
    for (const Clip& c : r.clips)
        clips.append(QJsonObject{ { QStringLiteral("name"), c.name },
                                  { QStringLiteral("firstRow"), c.firstRow },
                                  { QStringLiteral("frameCount"), c.frameCount },
                                  { QStringLiteral("duration"), c.duration },
                                  { QStringLiteral("fps"), c.fps } });
    o[QStringLiteral("clips")] = clips;
    o[QStringLiteral("sourceMaxInfluences")] = r.sourceMaxInfluences;
    o[QStringLiteral("verticesOverLimit")] = r.verticesOverLimit;
    if (!r.shaders.isEmpty()) o[QStringLiteral("shaders")] = QJsonArray::fromStringList(r.shaders);
    if (!r.warnings.isEmpty()) o[QStringLiteral("warnings")] = QJsonArray::fromStringList(r.warnings);
    return o;
}

} // namespace BoneTexture
