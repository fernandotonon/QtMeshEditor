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
#include <QSaveFile>

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
    const long intervals = std::lround(duration * fps);
    return static_cast<int>(std::max(1L, intervals)) + 1;
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

WriteResult write(const BakedClips& baked, const QString& outputDir,
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

    const QDir dir(outputDir);
    const QString texName = basename + QStringLiteral("_bones.exr");
    const QString texPath = dir.filePath(texName);
    const QString jsonPath = dir.filePath(basename + QStringLiteral("_bones.json"));

    // Texture: temp file + rename so an existing bake is never truncated.
    const QString tmpTex = texPath + QStringLiteral(".qtmesh_tmp");
    const bool wrote = (enc == QStringLiteral("rgba16f"))
        ? MinimalEXR::writeRGBA16F(tmpTex, width, height, texels)
        : MinimalEXR::writeRGBA32F(tmpTex, width, height, texels);
    if (!wrote) {
        QFile::remove(tmpTex);
        res.error = QStringLiteral("failed to write the bone texture %1").arg(texPath);
        return res;
    }
    QFile::remove(texPath);
    if (!QFile::rename(tmpTex, texPath)) {
        QFile::remove(tmpTex);
        res.error = QStringLiteral("failed to move the bone texture into place at %1").arg(texPath);
        return res;
    }

    QSaveFile json(jsonPath);
    const QByteArray bytes = buildSidecarJson(baked, texName, enc, width, height).toUtf8();
    if (!json.open(QIODevice::WriteOnly) || json.write(bytes) != bytes.size() || !json.commit()) {
        res.error = QStringLiteral("failed to write the bone texture sidecar %1").arg(jsonPath);
        return res;
    }

    res.ok = true;
    res.texturePath = texPath;
    res.jsonPath = jsonPath;
    res.width = width;
    res.height = height;
    return res;
}

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

    // Snapshot every state so the caller's playback is untouched.
    std::vector<StateSnapshot> snapshot;
    for (auto it = states->getAnimationStateIterator(); it.hasMoreElements();) {
        Ogre::AnimationState* s = it.getNext();
        snapshot.push_back({ s, s->getEnabled(), s->getLoop(), s->getTimePosition(), s->getWeight() });
    }
    struct Restore {
        std::vector<StateSnapshot>& snap;
        Ogre::SkeletonInstance* skel;
        Ogre::AnimationStateSet* states;
        ~Restore() {
            for (const auto& s : snap) {
                s.state->setEnabled(s.enabled);
                s.state->setLoop(s.loop);
                s.state->setTimePosition(s.time);
                s.state->setWeight(s.weight);
            }
            skel->setAnimationState(*states);
            states->_notifyDirty();
        }
    } restore{ snapshot, skel, states };

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

    // Mesh first: exporting after the sampler has driven the animation
    // states was what broke the VAT's glTF export.
    if (req.exportMesh) {
        const QString meshPath = QDir(req.outputDir).filePath(basename + QStringLiteral(".glb"));
        const int rc = MeshImporterExporter::exporter(
            entity->getParentSceneNode(), meshPath, QStringLiteral("glTF 2.0 Binary (*.glb)"));
        if (rc == 0 && QFileInfo::exists(meshPath))
            rep.meshPath = QFileInfo(meshPath).absoluteFilePath();
        else
            rep.warnings << QStringLiteral("mesh export to %1 failed (rc=%2) - the texture is still "
                                           "valid; export the rigged mesh separately").arg(meshPath).arg(rc);
    }

    SampleOptions so;
    so.clips = req.clips;
    so.fps = req.fps;
    BakedClips baked;
    QString err;
    if (!sampleEntity(entity, so, &baked, &err)) {
        rep.error = err;
        return rep;
    }
    rep.files = write(baked, req.outputDir, basename, rep.encoding, req.maxTextureSize);
    if (!rep.files.ok) {
        rep.error = rep.files.error;
        return rep;
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
        if (!supported.isEmpty())
            rep.shaders = VATShaderEmitter::writeBoneTextureShaders(req.outputDir, supported);
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
