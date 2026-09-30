/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "VATBaker.h"

#include "MinimalEXRWriter.h"
#include "Mocap/FaceCapPose.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstring>

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreCommon.h>
#include <OgreEntity.h>
#include <OgreFrameListener.h>
#include <OgreKeyFrame.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreMesh.h>
#include <OgreQuaternion.h>
#include <OgreRoot.h>
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include <algorithm>
#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// Mode / encoding / target ids.
// ---------------------------------------------------------------------------

QString VATBaker::modeId(Mode mode)
{
    switch (mode) {
    case Mode::Skeletal: return QStringLiteral("skeletal");
    case Mode::Rigid:    return QStringLiteral("rigid");
    case Mode::MeshAnim: return QStringLiteral("mesh-anim");
    case Mode::Morph:    return QStringLiteral("morph");
    }
    return QStringLiteral("skeletal");
}

bool VATBaker::modeFromId(const QString& id, Mode* out)
{
    const QString t = id.trimmed().toLower();
    Mode m;
    if (t == QLatin1String("skeletal") || t == QLatin1String("skin")
        || t == QLatin1String("skinned")) {
        m = Mode::Skeletal;
    } else if (t == QLatin1String("rigid") || t == QLatin1String("rigid-body")
               || t == QLatin1String("rigidbody") || t == QLatin1String("rbd")) {
        m = Mode::Rigid;
    } else if (t == QLatin1String("mesh-anim") || t == QLatin1String("mesh_anim")
               || t == QLatin1String("meshanim") || t == QLatin1String("vertex")
               || t == QLatin1String("vertex-anim") || t == QLatin1String("alembic")) {
        m = Mode::MeshAnim;
    } else if (t == QLatin1String("morph") || t == QLatin1String("blendshape")
               || t == QLatin1String("blendshapes")) {
        m = Mode::Morph;
    } else {
        return false;
    }
    if (out) *out = m;
    return true;
}

QStringList VATBaker::modeIds()
{
    return { QStringLiteral("skeletal"), QStringLiteral("rigid"),
             QStringLiteral("mesh-anim"), QStringLiteral("morph") };
}

QString VATBaker::encodingId(int bitDepth)
{
    if (bitDepth == 8)  return QStringLiteral("rgba8");
    if (bitDepth == 32) return QStringLiteral("exr");
    return QStringLiteral("rgba16");
}

bool VATBaker::bitDepthFromEncodingId(const QString& id, int* outBitDepth)
{
    const QString t = id.trimmed().toLower();
    int bd;
    if (t == QLatin1String("rgba8") || t == QLatin1String("8")
        || t == QLatin1String("png8")) {
        bd = 8;
    } else if (t == QLatin1String("rgba16") || t == QLatin1String("16")
               || t == QLatin1String("png16") || t == QLatin1String("png")) {
        bd = 16;
    } else if (t == QLatin1String("exr") || t == QLatin1String("32")
               || t == QLatin1String("float") || t == QLatin1String("rgba32f")) {
        bd = 32;
    } else {
        return false;
    }
    if (outBitDepth) *outBitDepth = bd;
    return true;
}

QStringList VATBaker::encodingIds()
{
    return { QStringLiteral("rgba8"), QStringLiteral("rgba16"), QStringLiteral("exr") };
}

QStringList VATBaker::targetIds()
{
    return { QStringLiteral("agnostic"), QStringLiteral("unity"),
             QStringLiteral("unreal"), QStringLiteral("godot") };
}

bool VATBaker::isValidTargetId(const QString& id)
{
    const QString t = id.trimmed().toLower();
    if (t.isEmpty()) return true;   // empty == agnostic
    return targetIds().contains(t);
}

namespace {

QString normalisedTarget(const QString& target)
{
    const QString t = target.trimmed().toLower();
    return t.isEmpty() ? QStringLiteral("agnostic") : t;
}

// ---------------------------------------------------------------------------
// Sampling.
// ---------------------------------------------------------------------------

// Resolve the vertex data that holds `entity`'s DEFORMED geometry for
// submesh `si` this frame:
//   - skinned entity → the software-skinned buffer. When the entity
//     ALSO carries vertex animation, Ogre feeds the skinning stage from
//     the vertex-anim buffer, so this buffer is "morph then skin" —
//     which is exactly what every mode wants (skeletal mode enables a
//     skeletal clip; mesh-anim/morph modes leave the skeleton in bind
//     pose, so the skin stage is an identity pass-through).
//   - unskinned entity with vertex animation on that submesh → the
//     software vertex-anim buffer.
//   - anything else → the mesh's original (static) buffer.
// Requires `addSoftwareAnimationRequest(true)` to have been called so
// the software buffers exist in a headless bake.
const Ogre::VertexData* deformedVertexData(Ogre::Entity* entity,
                                          Ogre::Mesh* mesh,
                                          Ogre::SubMesh* sub,
                                          unsigned short si)
{
    if (sub->useSharedVertices) {
        if (entity->hasSkeleton())
            return entity->_getSkelAnimVertexData();
        if (mesh->getSharedVertexDataAnimationType() != Ogre::VAT_NONE)
            return entity->_getSoftwareVertexAnimVertexData();
        return mesh->sharedVertexData;
    }
    Ogre::SubEntity* se = entity->getSubEntity(si);
    if (!se) return nullptr;
    if (entity->hasSkeleton())
        return se->_getSkelAnimVertexData();
    if (sub->getVertexAnimationType() != Ogre::VAT_NONE)
        return se->_getSoftwareVertexAnimVertexData();
    return sub->vertexData;
}

// Append every vertex's `semantic` element (a float3) from `vData`.
size_t appendFloat3(const Ogre::VertexData* vData,
                    Ogre::VertexElementSemantic semantic,
                    std::vector<Ogre::Vector3>& out)
{
    const auto* elem = vData->vertexDeclaration->findElementBySemantic(semantic);
    if (!elem) return 0;
    auto vbuf = vData->vertexBufferBinding->getBuffer(elem->getSource());
    if (!vbuf) return 0;
    auto* bytes = static_cast<unsigned char*>(
        vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    const size_t vstride = vbuf->getVertexSize();
    for (size_t j = 0; j < vData->vertexCount; ++j) {
        Ogre::Real* p = nullptr;
        elem->baseVertexPointerToElement(bytes + j * vstride, &p);
        out.emplace_back(p[0], p[1], p[2]);
    }
    vbuf->unlock();
    return vData->vertexCount;
}

// Read one frame of deformed positions from `entity`. The vertex
// ordering follows the same submesh walk NormalVisualizer uses:
// shared vertex data appears first (and only once across all submeshes
// that share it), then each non-shared submesh contributes its own
// vertex range in submesh-index order.
//
// Returns the appended count for the caller's sanity check.
size_t collectDeformedPositions(Ogre::Entity* entity,
                                std::vector<Ogre::Vector3>& out)
{
    entity->_updateAnimation();

    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return 0;

    size_t appended = 0;
    bool sharedAppended = false;

    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub) continue;

        // Skip shared-vertex submeshes after the first (they all point at
        // the same vertex buffer, and double-counting would inflate the
        // vertex column count in the output texture).
        if (sub->useSharedVertices && sharedAppended) continue;

        const Ogre::VertexData* animData = deformedVertexData(entity, mesh.get(), sub, si);
        if (!animData) continue;

        appended += appendFloat3(animData, Ogre::VES_POSITION, out);

        if (sub->useSharedVertices) sharedAppended = true;
    }

    return appended;
}

inline uint16_t toShortNormalised(float v, float lo, float hi)
{
    if (hi <= lo) return 0;
    const float t = (v - lo) / (hi - lo);
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return static_cast<uint16_t>(std::lround(clamped * 65535.0f));
}

inline uint8_t toByteNormalised(float v, float lo, float hi)
{
    if (hi <= lo) return 0;
    const float t = (v - lo) / (hi - lo);
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

// Same submesh walk as collectDeformedPositions but for normals.
//
// On a submesh without `VES_NORMAL`, returns SIZE_MAX as a sentinel so
// the caller can fail the bake with a clear error. We deliberately do
// NOT pad with fabricated up-vectors — that produces a normal texture
// that looks plausible but lights the mesh wrong while reporting
// success, which is worse than refusing to bake.
constexpr size_t kCollectNormalsMissingSentinel =
    std::numeric_limits<size_t>::max();

size_t collectDeformedNormals(Ogre::Entity* entity,
                              std::vector<Ogre::Vector3>& out)
{
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return 0;

    size_t appended = 0;
    bool sharedAppended = false;

    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub) continue;
        if (sub->useSharedVertices && sharedAppended) continue;

        const Ogre::VertexData* animData = deformedVertexData(entity, mesh.get(), sub, si);
        if (!animData) continue;

        if (!animData->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL)) {
            // Missing-normal submesh: surface as a hard failure rather
            // than fabricate up-vectors. The caller's error message
            // identifies which submesh hit this.
            return kCollectNormalsMissingSentinel;
        }
        appended += appendFloat3(animData, Ogre::VES_NORMAL, out);

        if (sub->useSharedVertices) sharedAppended = true;
    }

    return appended;
}

// Bind-pose (undeformed) positions in the same walk order — the rigid
// fit's source points.
size_t collectBindPositions(Ogre::Entity* entity,
                            std::vector<Ogre::Vector3>& out)
{
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return 0;
    size_t appended = 0;
    bool sharedAppended = false;
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub) continue;
        if (sub->useSharedVertices && sharedAppended) continue;
        const Ogre::VertexData* vData = sub->useSharedVertices
            ? mesh->sharedVertexData : sub->vertexData;
        if (!vData) continue;
        appended += appendFloat3(vData, Ogre::VES_POSITION, out);
        if (sub->useSharedVertices) sharedAppended = true;
    }
    return appended;
}

// Does the mesh animation `name` carry at least one vertex track?
bool meshAnimationHasVertexTracks(Ogre::Mesh* mesh, const std::string& name)
{
    if (!mesh || !mesh->hasAnimation(name)) return false;
    Ogre::Animation* a = mesh->getAnimation(name);
    return a && !a->_getVertexTrackList().empty();
}

// Morph target names (unique pose names) referenced by the clip's
// VAT_POSE keyframes, in first-seen order — ONLY those: a weight clip
// that keys a subset deforms only that subset, and a consumer may use
// this list to strip the unbaked shapes from its runtime mesh. A clip
// that references no pose yields an empty list, never "every pose".
QStringList morphTargetsDrivenBy(Ogre::Mesh* mesh, const std::string& clip)
{
    QStringList out;
    if (!mesh || !mesh->hasAnimation(clip)) return out;
    const auto& poses = mesh->getPoseList();
    Ogre::Animation* anim = mesh->getAnimation(clip);
    for (const auto& kv : anim->_getVertexTrackList()) {
        const Ogre::VertexAnimationTrack* track = kv.second;
        if (!track || track->getAnimationType() != Ogre::VAT_POSE) continue;
        for (unsigned short k = 0; k < track->getNumKeyFrames(); ++k) {
            const auto* kf = static_cast<const Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(k));
            for (const auto& ref : kf->getPoseReferences()) {
                if (ref.poseIndex >= poses.size() || !poses[ref.poseIndex]) continue;
                const QString n = QString::fromStdString(poses[ref.poseIndex]->getName());
                if (!n.isEmpty() && !out.contains(n)) out << n;
            }
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Sidecar emission.
// ---------------------------------------------------------------------------

namespace { QString buildOpenVATSidecar(const VATBaker::BakeResult& result,
                                        int bitDepth,
                                        const QString& target); }

QString VATBaker::buildSidecarJson(const BakeResult& result,
                                   const Options& opts)
{
    return buildOpenVATSidecar(result, opts.bitDepth, opts.target);
}

// ---------------------------------------------------------------------------
// OpenVAT sidecar + texture packing.
// ---------------------------------------------------------------------------

namespace {

// ─── OpenVAT compatibility ───────────────────────────────────────────
//
// Reference: https://github.com/sharpen3d/openvat
// (the Blender add-on by sharpen3d, "OpenVAT-Engine_Tools" submodule)
//
// What OpenVAT consumers expect:
//
//   1. Sidecar JSON shape (file is `<basename>-remap_info.json`):
//
//        { "os-remap": { "Min": ["-1.20000000","0.00000000","-1.10000000"],
//                        "Max": ["1.10000000","2.00000000","0.40000000"],
//                        "Frames": 71 } }
//
//      `Min`/`Max` are quoted 8-decimal-place strings (matches the
//      Blender add-on's `CustomEncoder` output in utils.py:135-139),
//      rounded OUTWARD to the nearest 0.1 (floor for min, ceil for max
//      — utils.py:186-191 / round_to_nearest_ten). Frames is a bare
//      integer.
//
//   2. Texture is one 16-bit-per-channel PNG, RGB (no alpha):
//        height = 2 * frameCount, width = vertexCount
//        rows  [0   .. frameCount)        → positions
//        rows  [frameCount .. 2*frameCount) → normals
//      The Godot reference shader computes
//        `int frame_count = resolution.y / 2;`
//      and applies `normals_uv_shift = vec2(0.0, 0.5);` when sampling
//      the normal half. (See OpenVAT-Engine_Tools GLSL shader L75-100.)
//
//   3. Channel encoding:
//        positions: linear normalize to [Min, Max] like our existing path.
//        normals:   (n + 1) * 0.5 → channel, decoded with `2*c - 1`.
//        Both written as 16-bit unsigned (PNG16 / `Format_RGBX64`).
//
//   4. Sampling convention: rows=frames + cols=verts, **frame 0 at
//      the top of the texture** (the Blender add-on writes top-down
//      in pixel-space; the shader flips V on read with `1.0 - …`).
//      Same orientation as our Agnostic / Godot / Unreal targets.
//
// We don't apply any axis swizzle. OpenVAT consumers traditionally
// run their own swizzle on read (the Godot reference shader does
// `vec3(x, z, -y)` to go Blender → Godot). We document the source
// space (Ogre Y-up RH) in the sidecar via an extra non-conflicting
// `_axes` key so a consumer's shader knows what swizzle to apply.

// Round a float OUTWARD to the nearest 0.1 — floor for min, ceil for
// max — matching openvat's `round_to_nearest_ten` (utils.py:186-188).
inline float openvatRoundMin(float v) {
    return std::floor(v * 10.0f) / 10.0f;
}
inline float openvatRoundMax(float v) {
    return std::ceil(v * 10.0f) / 10.0f;
}

// Format a float as an 8-decimal-place string with trailing zeros
// preserved (matches openvat's `CustomEncoder` `"%.8f"`).
inline QString openvatFormatFloat(float v) {
    return QString::number(static_cast<double>(v), 'f', 8);
}

QJsonArray vec3Array(const Ogre::Vector3& v)
{
    return QJsonArray{ static_cast<double>(v.x), static_cast<double>(v.y),
                       static_cast<double>(v.z) };
}

// Build the openvat sidecar JSON. Returned string is a single root
// object with `os-remap` at the top level, plus extension keys.
// `result.minBound/maxBound` are expected to already be on the 0.1
// OpenVAT grid (bake() snaps them before encoding the texture). We
// just format them; rounding here would silently re-snap rounded
// values and decouple the sidecar from whatever the encoder used.
//
// Extension keys (openvat consumers ignore unknown top-level keys):
//   _producer      "QtMeshEditor"
//   _axes          "y-up-rh" — source coordinate convention
//   _bit_depth     8 / 16 → PNG (uint8/uint16), 32 → EXR (float32)
//   _mode          skeletal | rigid | mesh-anim | morph
//   _target        agnostic | unity | unreal | godot
//   _rigid         { chunk_count, chunks:[{name,pivot,vertex_start,
//                    vertex_count,max_residual}], max_residual } (rigid)
//   _track         the vertex clip id (mesh-anim)
//   _morph_targets [names] (morph)
QString buildOpenVATSidecar(const VATBaker::BakeResult& result,
                            int bitDepth,
                            const QString& target)
{
    const Ogre::Vector3& lo = result.minBound;
    const Ogre::Vector3& hi = result.maxBound;
    QJsonArray jMin {
        openvatFormatFloat(lo.x),
        openvatFormatFloat(lo.y),
        openvatFormatFloat(lo.z)
    };
    QJsonArray jMax {
        openvatFormatFloat(hi.x),
        openvatFormatFloat(hi.y),
        openvatFormatFloat(hi.z)
    };
    QJsonObject osRemap;
    osRemap["Min"]    = jMin;
    osRemap["Max"]    = jMax;
    osRemap["Frames"] = result.frameCount;

    QJsonObject root;
    root["os-remap"] = osRemap;
    root["_producer"]  = QStringLiteral("QtMeshEditor");
    root["_axes"]      = QStringLiteral("y-up-rh");
    root["_bit_depth"] = bitDepth;
    root["_mode"]      = VATBaker::modeId(result.mode);
    root["_target"]    = normalisedTarget(target);

    switch (result.mode) {
    case VATBaker::Mode::Rigid: {
        QJsonObject rigid;
        rigid["chunk_count"] = result.chunkCount;
        QJsonArray chunks;
        for (const auto& c : result.chunks) {
            QJsonObject jc;
            jc["name"]         = c.name;
            jc["pivot"]        = vec3Array(c.pivot);
            jc["vertex_start"] = c.vertexStart;
            jc["vertex_count"] = c.vertexCount;
            jc["max_residual"] = static_cast<double>(c.maxResidual);
            chunks.append(jc);
        }
        rigid["chunks"]       = chunks;
        rigid["max_residual"] = static_cast<double>(result.maxRigidResidual);
        // Documents the texel layout so a consumer never has to guess.
        rigid["layout"] = QStringLiteral(
            "column = chunk; rows [0..Frames) = pivot position normalized to "
            "Min..Max; rows [Frames..2*Frames) = rotation quaternion (x,y,z,w) "
            "encoded (q+1)/2 in RGBA; p' = q * (p - pivot) + pivot_frame");
        root["_rigid"] = rigid;
        break;
    }
    case VATBaker::Mode::MeshAnim:
        root["_track"] = result.trackId;
        break;
    case VATBaker::Mode::Morph: {
        QJsonArray names;
        for (const QString& n : result.morphTargets) names.append(n);
        root["_morph_targets"] = names;
        root["_track"]         = result.trackId;
        break;
    }
    case VATBaker::Mode::Skeletal:
        break;
    }

    QJsonDocument doc(root);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

// Encode `flat` positions + `normals` into a single contiguous buffer
// laid out as a packed 16-bit RGB image:
//   row 0..frameCount-1      = positions (3 channels: x, y, z)
//   row frameCount..2*N-1    = normals   (3 channels: x, y, z)
// width  = vertexCount, height = 2 * frameCount.
// Positions are normalized into [lo..hi]; normals into [0..1] via
// (n+1)/2. Output buffer is row-major, 3 uint16 per pixel.
//
// `permutation` (optional): the column to write each Ogre vertex into.
// Empty = identity. Used to align the bake's column order with what
// downstream consumers see in their imported mesh — Assimp's gltf2
// exporter permutes vertices via JoinIdenticalVertices and the
// consumer's vertex-index UV2 references the permuted buffer.
std::vector<uint16_t> packOpenVAT16(
    const std::vector<Ogre::Vector3>& flat,
    const std::vector<Ogre::Vector3>& normals,
    int frameCount,
    int vertexCount,
    const Ogre::Vector3& lo,
    const Ogre::Vector3& hi,
    const std::vector<uint32_t>& permutation = {})
{
    const size_t framesCount = static_cast<size_t>(frameCount);
    const size_t vcount      = static_cast<size_t>(vertexCount);
    const size_t imgHeight   = framesCount * 2u;
    const size_t pixels      = imgHeight * vcount;

    std::vector<uint16_t> out;
    if (frameCount <= 0 || vertexCount <= 0) return out;
    if (flat.size()    != framesCount * vcount) return out;
    if (normals.size() != framesCount * vcount) return out;
    if (!permutation.empty() && permutation.size() != vcount) return out;

    out.resize(pixels * 3u, 0);
    const bool hasPerm = !permutation.empty();

    auto dstCol = [&](size_t srcCol) -> size_t {
        return hasPerm ? static_cast<size_t>(permutation[srcCol]) : srcCol;
    };

    // Top half: positions normalized to [lo..hi].
    for (size_t f = 0; f < framesCount; ++f) {
        const size_t rowBase = f * vcount;
        for (size_t c = 0; c < vcount; ++c) {
            const auto& p = flat[rowBase + c];
            const size_t pix = rowBase + dstCol(c);
            out[pix * 3 + 0] = toShortNormalised(p.x, lo.x, hi.x);
            out[pix * 3 + 1] = toShortNormalised(p.y, lo.y, hi.y);
            out[pix * 3 + 2] = toShortNormalised(p.z, lo.z, hi.z);
        }
    }
    // Bottom half: normals (n+1)/2 → [0..65535].
    const size_t offset = framesCount * vcount;
    for (size_t f = 0; f < framesCount; ++f) {
        const size_t rowBase = f * vcount;
        for (size_t c = 0; c < vcount; ++c) {
            const auto& n = normals[rowBase + c];
            const float nx = std::clamp((n.x + 1.0f) * 0.5f, 0.0f, 1.0f);
            const float ny = std::clamp((n.y + 1.0f) * 0.5f, 0.0f, 1.0f);
            const float nz = std::clamp((n.z + 1.0f) * 0.5f, 0.0f, 1.0f);
            const size_t off = (offset + rowBase + dstCol(c)) * 3u;
            out[off + 0] = static_cast<uint16_t>(std::lround(nx * 65535.0f));
            out[off + 1] = static_cast<uint16_t>(std::lround(ny * 65535.0f));
            out[off + 2] = static_cast<uint16_t>(std::lround(nz * 65535.0f));
        }
    }
    return out;
}

// 32-bit variant of `packOpenVAT16`. Same layout (top half positions,
// bottom half normals; same column-permutation contract), but emits
// raw float32s instead of quantizing to uint16.
//
//   Positions: stored as the literal post-skin coord (in the bake's
//              Y-up-RH meters space). NO normalization — consumers
//              read the value directly. This means the EXR is NOT
//              compatible with the 16-bit `bounds_min..bounds_max`
//              decode path; the sidecar's `_bit_depth: 32` tells the
//              consumer to skip the bounds remap and use the texel
//              value as-is.
//
//   Normals:  (n+1)/2 still, so the value lives in [0..1] same as
//             the 16-bit path. Identical decode on the consumer
//             side ((tex * 2) - 1).
std::vector<float> packOpenVAT32(
    const std::vector<Ogre::Vector3>& flat,
    const std::vector<Ogre::Vector3>& normals,
    int frameCount,
    int vertexCount,
    const std::vector<uint32_t>& permutation = {})
{
    const size_t framesCount = static_cast<size_t>(frameCount);
    const size_t vcount      = static_cast<size_t>(vertexCount);
    const size_t imgHeight   = framesCount * 2u;
    const size_t pixels      = imgHeight * vcount;

    std::vector<float> out;
    if (frameCount <= 0 || vertexCount <= 0) return out;
    if (flat.size()    != framesCount * vcount) return out;
    if (normals.size() != framesCount * vcount) return out;
    if (!permutation.empty() && permutation.size() != vcount) return out;

    out.resize(pixels * 3u, 0.0f);
    const bool hasPerm = !permutation.empty();
    auto dstCol = [&](size_t srcCol) -> size_t {
        return hasPerm ? static_cast<size_t>(permutation[srcCol]) : srcCol;
    };

    // Top half — raw position floats (Y-up-RH meters).
    for (size_t f = 0; f < framesCount; ++f) {
        const size_t rowBase = f * vcount;
        for (size_t c = 0; c < vcount; ++c) {
            const auto& p = flat[rowBase + c];
            const size_t pix = rowBase + dstCol(c);
            out[pix * 3 + 0] = p.x;
            out[pix * 3 + 1] = p.y;
            out[pix * 3 + 2] = p.z;
        }
    }
    // Bottom half — (n+1)/2 in [0..1].
    const size_t offset = framesCount * vcount;
    for (size_t f = 0; f < framesCount; ++f) {
        const size_t rowBase = f * vcount;
        for (size_t c = 0; c < vcount; ++c) {
            const auto& n = normals[rowBase + c];
            const size_t pix = offset + rowBase + dstCol(c);
            out[pix * 3 + 0] = (n.x + 1.0f) * 0.5f;
            out[pix * 3 + 1] = (n.y + 1.0f) * 0.5f;
            out[pix * 3 + 2] = (n.z + 1.0f) * 0.5f;
        }
    }
    return out;
}

// Write a 16-bit-per-channel PNG from a packed 3-channel uint16 buffer.
// RGBX64 is Qt's 16-bit-per-channel 4-channel format. The X channel
// is padding; PNG can store 3-channel data losslessly but Qt's PNG
// writer infers RGB-vs-RGBA from the QImage format, and Format_RGB
// doesn't exist at 16-bit precision. Padding to RGBX64 costs a few
// hundred KB on a 5828×142 image — acceptable for a one-off bake.
bool writePng16From3(const QString& path, int width, int height,
                     const std::vector<uint16_t>& packed)
{
    QImage img(width, height, QImage::Format_RGBX64);
    img.fill(0);
    for (int y = 0; y < height; ++y) {
        const uint16_t* src = packed.data()
                            + static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
        auto* dst = reinterpret_cast<uint16_t*>(img.scanLine(y));
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * 3 + 0];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 2];
            dst[x * 4 + 3] = 65535;
        }
    }
    return img.save(path, "PNG");
}

// 8-bit PNG from the SAME packed uint16 buffer: each channel is
// re-quantized to 0..255 (rounded), alpha = 255. Deliberately derived
// from the 16-bit pack so the two encodings decode against the same
// bounds and differ only in precision.
bool writePng8From3(const QString& path, int width, int height,
                    const std::vector<uint16_t>& packed)
{
    QImage img(width, height, QImage::Format_RGBA8888);
    img.fill(0);
    for (int y = 0; y < height; ++y) {
        const uint16_t* src = packed.data()
                            + static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
        auto* dst = img.scanLine(y);
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c)
                dst[x * 4 + c] = static_cast<uint8_t>(
                    std::lround(src[x * 3 + c] / 65535.0f * 255.0f));
            dst[x * 4 + 3] = 255;
        }
    }
    return img.save(path, "PNG");
}

// ---------------------------------------------------------------------------
// Rigid-body helpers.
// ---------------------------------------------------------------------------

struct RigidFrameSample {
    Ogre::Vector3    pivotPos = Ogre::Vector3::ZERO;   // where the bind pivot went
    Ogre::Quaternion rot      = Ogre::Quaternion::IDENTITY;
    float            maxResidual = 0.0f;
};

// Horn fit of `bind[0..count)` → `deformed[0..count)` (raw pointers
// into the caller's frame-invariant bind array and the frame's slice of
// the deformed array — no per-frame copies). The scale
// the solver reports is DISCARDED (a rigid chunk has none; on a
// slightly non-rigid one it would smear the error into the rotation),
// and the translation is recomputed from the centroids so the pivot
// contract `p' = R*(p - pivot) + pivotFrame` holds exactly at the
// centroid. Degenerate chunks (< 3 points, collinear) fall back to a
// pure translation.
// `srcFlat`/`weights` are the chunk's bind points + unit weights in
// the solver's flat layout, built ONCE per chunk by the caller.
RigidFrameSample fitRigidChunk(const Ogre::Vector3* bind,
                               const Ogre::Vector3* deformed,
                               size_t count,
                               const Ogre::Vector3& pivot,
                               const std::vector<float>& srcFlat,
                               const std::vector<float>& weights,
                               std::vector<float>& dstScratch)
{
    RigidFrameSample s;
    if (count == 0) return s;

    Ogre::Vector3 centroid = Ogre::Vector3::ZERO;
    for (size_t i = 0; i < count; ++i) centroid += deformed[i];
    centroid /= static_cast<float>(count);
    s.pivotPos = centroid;

    if (count >= 3) {
        dstScratch.resize(count * 3);
        for (size_t i = 0; i < count; ++i) {
            const auto& b = deformed[i];
            dstScratch[i * 3 + 0] = b.x; dstScratch[i * 3 + 1] = b.y; dstScratch[i * 3 + 2] = b.z;
        }
        const FaceCapPose::Result r = FaceCapPose::solve(
            srcFlat.data(), dstScratch.data(), weights.data(), static_cast<int>(count));
        if (r.ok) {
            s.rot = Ogre::Quaternion(r.rotation[3], r.rotation[0],
                                     r.rotation[1], r.rotation[2]);
            s.rot.normalise();
        }
    }

    float worst = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const Ogre::Vector3 fitted = s.rot * (bind[i] - pivot) + s.pivotPos;
        worst = std::max(worst, (fitted - deformed[i]).length());
    }
    s.maxResidual = worst;
    return s;
}

// Pack rigid samples: width = chunkCount, height = 2*frameCount.
// Top half = pivot positions (normalized 16-bit / raw float), bottom
// half = quaternion (x,y,z,w) → (q+1)/2. 4 channels per texel.
std::vector<uint16_t> packRigid16(const std::vector<RigidFrameSample>& samples,
                                  int frameCount, int chunkCount,
                                  const Ogre::Vector3& lo, const Ogre::Vector3& hi)
{
    const size_t F = static_cast<size_t>(frameCount);
    const size_t C = static_cast<size_t>(chunkCount);
    std::vector<uint16_t> out;
    if (frameCount <= 0 || chunkCount <= 0 || samples.size() != F * C) return out;
    out.resize(F * 2u * C * 4u, 0);
    for (size_t f = 0; f < F; ++f) {
        for (size_t c = 0; c < C; ++c) {
            const auto& s = samples[f * C + c];
            const size_t top = (f * C + c) * 4u;
            out[top + 0] = toShortNormalised(s.pivotPos.x, lo.x, hi.x);
            out[top + 1] = toShortNormalised(s.pivotPos.y, lo.y, hi.y);
            out[top + 2] = toShortNormalised(s.pivotPos.z, lo.z, hi.z);
            out[top + 3] = 65535;
            const size_t bot = ((F + f) * C + c) * 4u;
            const float q[4] = { s.rot.x, s.rot.y, s.rot.z, s.rot.w };
            for (int k = 0; k < 4; ++k) {
                const float v = std::clamp((q[k] + 1.0f) * 0.5f, 0.0f, 1.0f);
                out[bot + k] = static_cast<uint16_t>(std::lround(v * 65535.0f));
            }
        }
    }
    return out;
}

std::vector<float> packRigid32(const std::vector<RigidFrameSample>& samples,
                               int frameCount, int chunkCount)
{
    const size_t F = static_cast<size_t>(frameCount);
    const size_t C = static_cast<size_t>(chunkCount);
    std::vector<float> out;
    if (frameCount <= 0 || chunkCount <= 0 || samples.size() != F * C) return out;
    out.resize(F * 2u * C * 4u, 0.0f);
    for (size_t f = 0; f < F; ++f) {
        for (size_t c = 0; c < C; ++c) {
            const auto& s = samples[f * C + c];
            const size_t top = (f * C + c) * 4u;
            out[top + 0] = s.pivotPos.x;
            out[top + 1] = s.pivotPos.y;
            out[top + 2] = s.pivotPos.z;
            out[top + 3] = 1.0f;
            const size_t bot = ((F + f) * C + c) * 4u;
            out[bot + 0] = (s.rot.x + 1.0f) * 0.5f;
            out[bot + 1] = (s.rot.y + 1.0f) * 0.5f;
            out[bot + 2] = (s.rot.z + 1.0f) * 0.5f;
            out[bot + 3] = (s.rot.w + 1.0f) * 0.5f;
        }
    }
    return out;
}

bool writePng16From4(const QString& path, int width, int height,
                     const std::vector<uint16_t>& packed)
{
    QImage img(width, height, QImage::Format_RGBA64);
    img.fill(0);
    for (int y = 0; y < height; ++y) {
        const uint16_t* src = packed.data()
                            + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
        std::memcpy(img.scanLine(y), src, static_cast<size_t>(width) * 4u * sizeof(uint16_t));
    }
    return img.save(path, "PNG");
}

bool writePng8From4(const QString& path, int width, int height,
                    const std::vector<uint16_t>& packed)
{
    QImage img(width, height, QImage::Format_RGBA8888);
    img.fill(0);
    for (int y = 0; y < height; ++y) {
        const uint16_t* src = packed.data()
                            + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
        auto* dst = img.scanLine(y);
        for (int x = 0; x < width * 4; ++x)
            dst[x] = static_cast<uint8_t>(std::lround(src[x] / 65535.0f * 255.0f));
    }
    return img.save(path, "PNG");
}

// Write the sidecar atomically-enough: full write + size check, remove
// on short write so a consumer never reads a truncated file.
bool writeSidecarFile(const QString& path, const QString& json, QString* error)
{
    QFile jf(path);
    if (!jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("failed to open OpenVAT sidecar for write: %1").arg(path);
        return false;
    }
    const QByteArray bytes = json.toUtf8();
    const qint64 written = jf.write(bytes);
    jf.close();
    if (written != bytes.size()) {
        QFile::remove(path);
        if (error) *error = QStringLiteral(
            "short write to OpenVAT sidecar %1 (wrote %2 of %3 bytes)")
                .arg(path).arg(written).arg(bytes.size());
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// bake()
// ---------------------------------------------------------------------------

VATBaker::BakeResult VATBaker::bake(Ogre::Entity* entity, const Options& opts)
{
    BakeResult result;
    result.mode = opts.mode;

    if (!entity) {
        result.error = QStringLiteral("entity is null");
        return result;
    }
    if (opts.animationName.isEmpty()) {
        result.error = QStringLiteral("animationName is required");
        return result;
    }
    if (opts.fps <= 0.0) {
        result.error = QStringLiteral("fps must be > 0");
        return result;
    }
    if (opts.outputDir.isEmpty()) {
        result.error = QStringLiteral("outputDir is required");
        return result;
    }
    if (!isValidTargetId(opts.target)) {
        result.error = QStringLiteral("unknown target '%1' (accepted: %2)")
                           .arg(opts.target, targetIds().join(QStringLiteral(", ")));
        return result;
    }
    if (opts.bitDepth != 8 && opts.bitDepth != 16 && opts.bitDepth != 32) {
        result.error = QStringLiteral("bitDepth must be 8, 16 or 32 (got %1)").arg(opts.bitDepth);
        return result;
    }

    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) {
        result.error = QStringLiteral("entity has no mesh");
        return result;
    }
    const std::string animStd = opts.animationName.toStdString();
    const bool skeletalClip = entity->hasSkeleton()
        && entity->getSkeleton()->hasAnimation(animStd);
    const bool vertexClip = meshAnimationHasVertexTracks(mesh.get(), animStd);

    // ── Per-mode preconditions ──────────────────────────────────────
    switch (opts.mode) {
    case Mode::Skeletal:
        if (!entity->hasSkeleton()) {
            result.error = QStringLiteral("entity has no skeleton");
            return result;
        }
        // A rigged mesh can ALSO carry a vertex/morph clip under the
        // requested name; the state lookup below would find it and the
        // bake would sample vertex deformation while labelling the
        // output `_mode: skeletal`. Insist on a skeleton animation.
        if (!skeletalClip) {
            result.error = vertexClip
                ? QStringLiteral("'%1' is a vertex/morph clip, not a skeletal animation - "
                                 "use --mode mesh-anim or --mode morph").arg(opts.animationName)
                : QStringLiteral("animation '%1' not found on the skeleton").arg(opts.animationName);
            return result;
        }
        break;
    case Mode::MeshAnim:
        if (!vertexClip) {
            result.error = mesh->hasAnimation(animStd) || skeletalClip
                ? QStringLiteral("animation '%1' has no vertex tracks — mesh-anim "
                                 "mode needs a vertex clip (Alembic cache / "
                                 "VAT_POSE stream); use --mode skeletal for a "
                                 "skeletal clip").arg(opts.animationName)
                : QStringLiteral("vertex animation '%1' not found on the mesh")
                      .arg(opts.animationName);
            return result;
        }
        break;
    case Mode::Morph:
        if (mesh->getPoseCount() == 0) {
            result.error = QStringLiteral("mesh has no morph targets (poses) — "
                                          "morph mode needs blend shapes");
            return result;
        }
        if (!vertexClip) {
            result.error = QStringLiteral(
                "morph weight clip '%1' not found on the mesh (key some morph "
                "weights first — the default clip is \"MorphAnim\")")
                    .arg(opts.animationName);
            return result;
        }
        break;
    case Mode::Rigid:
        if (!skeletalClip && !vertexClip) {
            result.error = QStringLiteral("animation '%1' not found (rigid mode "
                                          "accepts a skeletal or a vertex clip)")
                               .arg(opts.animationName);
            return result;
        }
        for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
            const Ogre::SubMesh* sub = mesh->getSubMesh(si);
            if (sub && sub->useSharedVertices) {
                result.error = QStringLiteral(
                    "rigid mode chunks per submesh, which needs every submesh "
                    "to own its vertex data — submesh %1 uses shared vertices "
                    "(re-export through glTF/FBX first)").arg(si);
                return result;
            }
        }
        break;
    }

    auto* states = entity->getAllAnimationStates();
    if (!states || !states->hasAnimationState(animStd)) {
        result.error = QStringLiteral("animation '%1' not found").arg(opts.animationName);
        return result;
    }
    auto* state = states->getAnimationState(animStd);

    // Disable every animation state first so a previously-enabled state
    // doesn't blend into the bake.
    auto it = states->getAnimationStateIterator();
    while (it.hasMoreElements()) {
        auto* s = it.getNext();
        if (s) s->setEnabled(false);
    }
    state->setEnabled(true);
    state->setWeight(1.0f);
    // Ogre wraps `setTimePosition(t)` with `fmod(t, length)` while the
    // state loops (the default), so sampling the LAST frame at t ==
    // length silently reads frame 0 — a looping walk hides it, a
    // lipsync clip ends on the wrong mouth shape. Bake with loop off
    // (clamp semantics) and restore the caller's flag afterwards.
    const bool wasLooping = state->getLoop();
    state->setLoop(false);
    struct LoopRestore {
        Ogre::AnimationState* s; bool loop;
        ~LoopRestore() { if (s) s->setLoop(loop); }
    } loopRestore{ state, wasLooping };

    const float animLen = state->getLength();
    const double t0 = (opts.startTime < 0.0) ? 0.0 : opts.startTime;
    const double t1 = (opts.endTime   < 0.0) ? static_cast<double>(animLen) : opts.endTime;
    if (t1 <= t0) {
        result.error = QStringLiteral("endTime (%1) must be > startTime (%2)")
                           .arg(t1).arg(t0);
        return result;
    }

    // Frame count: round to nearest, but require at least 1 frame.
    const double span = t1 - t0;
    int frameCount = static_cast<int>(std::lround(span * opts.fps));
    if (frameCount < 1) frameCount = 1;

    const bool rigid = (opts.mode == Mode::Rigid);

    // Ensure CPU-side skin / vertex-anim data is available even when no
    // render is happening (which is the case during a headless bake).
    entity->addSoftwareAnimationRequest(true);

    std::vector<Ogre::Vector3> flat;
    std::vector<Ogre::Vector3> normals;
    Ogre::Vector3 lo(std::numeric_limits<float>::infinity());
    Ogre::Vector3 hi(-std::numeric_limits<float>::infinity());
    flat.reserve(static_cast<size_t>(frameCount) * 1024);
    if (!rigid) normals.reserve(static_cast<size_t>(frameCount) * 1024);

    int vertexCount = -1;
    for (int f = 0; f < frameCount; ++f) {
        const double t = (frameCount == 1) ? t0
                       : t0 + span * (static_cast<double>(f)
                                       / static_cast<double>(frameCount - 1));
        state->setTimePosition(static_cast<Ogre::Real>(t));
        // Bump the AnimationStateSet's dirty counter so
        // Entity::_updateAnimation sees the new state. Without this,
        // setTimePosition is a no-op when the requested time happens
        // to match the prior value (e.g. frame 0 at t=0 after fresh
        // setEnabled). The editor's AnimationControlController calls
        // this same method after every setTimePosition.
        states->_notifyDirty();
        // Bump Ogre's global frame counter so Entity::cacheBoneMatrices
        // reads the AnimationState (cache key in OgreEntity.cpp:1300 is
        // Root::getNextFrameNumber). In a headless bake we never call
        // Root::renderOneFrame, so without this manual bump the bone
        // matrices are computed once for the first frame and cached
        // forever — every row of the bake's position texture would end
        // up identical to row 0.
        Ogre::FrameEvent ev{}; ev.timeSinceLastFrame = 0.0f; ev.timeSinceLastEvent = 0.0f;
        Ogre::Root::getSingleton()._fireFrameRenderingQueued(ev);

        const size_t before = flat.size();
        const size_t appended = collectDeformedPositions(entity, flat);
        if (appended == 0) {
            entity->removeSoftwareAnimationRequest(true);
            result.error = QStringLiteral("frame %1 read 0 vertices").arg(f);
            return result;
        }
        const int frameVerts = static_cast<int>(appended);
        if (vertexCount < 0) {
            vertexCount = frameVerts;
        } else if (frameVerts != vertexCount) {
            entity->removeSoftwareAnimationRequest(true);
            result.error = QStringLiteral(
                "frame %1 vertex count (%2) differs from frame 0 (%3)")
                    .arg(f).arg(frameVerts).arg(vertexCount);
            return result;
        }

        if (!rigid) {
            for (size_t i = before; i < flat.size(); ++i) {
                const auto& p = flat[i];
                lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
                hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
            }

            const size_t nrmAppended = collectDeformedNormals(entity, normals);
            if (nrmAppended == kCollectNormalsMissingSentinel) {
                entity->removeSoftwareAnimationRequest(true);
                result.error = QStringLiteral(
                    "frame %1 has a submesh without VES_NORMAL — "
                    "OpenVAT requires per-vertex normals "
                    "(regenerate normals on the source mesh and re-import)")
                        .arg(f);
                return result;
            }
            if (nrmAppended != static_cast<size_t>(frameVerts)) {
                entity->removeSoftwareAnimationRequest(true);
                result.error = QStringLiteral(
                    "frame %1 normals count (%2) differs from positions (%3)")
                        .arg(f).arg(nrmAppended).arg(frameVerts);
                return result;
            }
        }
    }

    entity->removeSoftwareAnimationRequest(true);

    // ── Rigid: fit one transform per chunk per frame ────────────────
    std::vector<RigidFrameSample> rigidSamples;
    int chunkCount = 0;
    if (rigid) {
        std::vector<Ogre::Vector3> bind;
        const size_t bindCount = collectBindPositions(entity, bind);
        if (bindCount != static_cast<size_t>(vertexCount)) {
            result.error = QStringLiteral(
                "bind-pose vertex count (%1) differs from deformed count (%2)")
                    .arg(bindCount).arg(vertexCount);
            return result;
        }
        // Chunk ranges in walk order (every submesh owns its data —
        // validated above — so each is a contiguous range).
        const auto& nameMap = mesh->getSubMeshNameMap();
        size_t cursor = 0;
        for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
            const Ogre::SubMesh* sub = mesh->getSubMesh(si);
            if (!sub || !sub->vertexData) continue;
            RigidChunk c;
            c.vertexStart = static_cast<int>(cursor);
            c.vertexCount = static_cast<int>(sub->vertexData->vertexCount);
            c.name = QString::number(si);
            for (const auto& kv : nameMap)
                if (kv.second == si) { c.name = QString::fromStdString(kv.first); break; }
            Ogre::Vector3 centroid = Ogre::Vector3::ZERO;
            for (int i = 0; i < c.vertexCount; ++i) centroid += bind[cursor + i];
            if (c.vertexCount > 0) centroid /= static_cast<float>(c.vertexCount);
            c.pivot = centroid;
            result.chunks.push_back(c);
            cursor += static_cast<size_t>(c.vertexCount);
        }
        chunkCount = static_cast<int>(result.chunks.size());
        if (chunkCount == 0) {
            result.error = QStringLiteral("rigid mode found no submesh chunks");
            return result;
        }
        rigidSamples.resize(static_cast<size_t>(frameCount) * chunkCount);
        // Per-chunk solver inputs that never change across frames.
        std::vector<std::vector<float>> chunkSrc(chunkCount), chunkW(chunkCount);
        for (int ci = 0; ci < chunkCount; ++ci) {
            const RigidChunk& c = result.chunks[ci];
            chunkSrc[ci].resize(static_cast<size_t>(c.vertexCount) * 3);
            chunkW[ci].assign(static_cast<size_t>(c.vertexCount), 1.0f);
            for (int i = 0; i < c.vertexCount; ++i) {
                const auto& a = bind[static_cast<size_t>(c.vertexStart) + i];
                chunkSrc[ci][i * 3 + 0] = a.x; chunkSrc[ci][i * 3 + 1] = a.y; chunkSrc[ci][i * 3 + 2] = a.z;
            }
        }
        std::vector<float> dstScratch;
        for (int f = 0; f < frameCount; ++f) {
            const size_t frameBase = static_cast<size_t>(f) * vertexCount;
            for (int ci = 0; ci < chunkCount; ++ci) {
                RigidChunk& c = result.chunks[ci];
                // `bind` is frame-invariant; `flat` is offset by the frame.
                RigidFrameSample s = fitRigidChunk(
                    bind.data() + c.vertexStart,
                    flat.data() + frameBase + c.vertexStart,
                    static_cast<size_t>(c.vertexCount),
                    c.pivot, chunkSrc[ci], chunkW[ci], dstScratch);
                // Hemisphere continuity so a consumer can lerp adjacent
                // frames' quaternions without a 360° flip.
                if (f > 0) {
                    const auto& prev = rigidSamples[(static_cast<size_t>(f) - 1) * chunkCount + ci].rot;
                    if (prev.Dot(s.rot) < 0.0f) s.rot = -s.rot;
                }
                c.maxResidual = std::max(c.maxResidual, s.maxResidual);
                result.maxRigidResidual = std::max(result.maxRigidResidual, s.maxResidual);
                lo.x = std::min(lo.x, s.pivotPos.x); lo.y = std::min(lo.y, s.pivotPos.y); lo.z = std::min(lo.z, s.pivotPos.z);
                hi.x = std::max(hi.x, s.pivotPos.x); hi.y = std::max(hi.y, s.pivotPos.y); hi.z = std::max(hi.z, s.pivotPos.z);
                rigidSamples[static_cast<size_t>(f) * chunkCount + ci] = s;
            }
        }
        result.chunkCount = chunkCount;
    }

    // Degenerate bounds (single point on an axis): pad so the encoder
    // doesn't divide-by-zero and the runtime decode reads back the
    // constant value. Choice of pad is irrelevant — every sample lands
    // at the same byte.
    if (hi.x <= lo.x) hi.x = lo.x + 1.0f;
    if (hi.y <= lo.y) hi.y = lo.y + 1.0f;
    if (hi.z <= lo.z) hi.z = lo.z + 1.0f;

    // OpenVAT consumers decode positions against the JSON sidecar's
    // Min/Max — which are rounded outward to the nearest 0.1. The
    // texture must be encoded against the SAME rounded bounds, or
    // every sample drifts by up to one rounding step (~0.05 per axis
    // on a 1-unit model). Both halves of the os-remap contract live
    // off `roundedLo`/`roundedHi` from here on.
    const Ogre::Vector3 roundedLo(openvatRoundMin(lo.x),
                                  openvatRoundMin(lo.y),
                                  openvatRoundMin(lo.z));
    const Ogre::Vector3 roundedHi(openvatRoundMax(hi.x),
                                  openvatRoundMax(hi.y),
                                  openvatRoundMax(hi.z));

    result.frameCount  = frameCount;
    result.vertexCount = rigid ? chunkCount : vertexCount;
    result.minBound    = roundedLo;
    result.maxBound    = roundedHi;
    if (opts.mode == Mode::MeshAnim || opts.mode == Mode::Morph)
        result.trackId = opts.animationName;
    if (opts.mode == Mode::Morph)
        result.morphTargets = morphTargetsDrivenBy(mesh.get(), animStd);

    const int bitDepth = opts.bitDepth;

    QDir().mkpath(opts.outputDir);
    const QString base = opts.basename.isEmpty() ? opts.animationName : opts.basename;
    const char* posExt = (bitDepth == 32) ? "_pos.exr" : "_pos.png";
    result.posTexPath = QDir(opts.outputDir).filePath(base + QString::fromLatin1(posExt));
    result.jsonPath   = QDir(opts.outputDir).filePath(base + "-remap_info.json");

    if (!rigid && !opts.vertexPermutation.empty()) {
        if (opts.vertexPermutation.size() != static_cast<size_t>(vertexCount)) {
            result.error = QStringLiteral(
                "vertexPermutation size (%1) does not match vertex count (%2)")
                    .arg(opts.vertexPermutation.size()).arg(vertexCount);
            return result;
        }
        // A non-bijective permutation would silently corrupt the bake:
        // an out-of-range entry walks past the PNG's row stride into the
        // normal half, and a duplicate entry overwrites one column while
        // leaving another zero — both produce a packed texture that
        // decodes to garbage at runtime. Reject upfront with a clear
        // error so the caller can fall back to identity packing.
        std::vector<bool> seen(static_cast<size_t>(vertexCount), false);
        for (uint32_t dst : opts.vertexPermutation) {
            if (dst >= static_cast<uint32_t>(vertexCount) || seen[dst]) {
                result.error = QStringLiteral(
                    "vertexPermutation must be a unique mapping over [0, %1)")
                        .arg(vertexCount);
                return result;
            }
            seen[dst] = true;
        }
    }
    const int imgHeight = frameCount * 2;
    const int imgWidth  = result.vertexCount;

    bool wrote = false;
    if (rigid) {
        if (bitDepth == 32) {
            auto packed = packRigid32(rigidSamples, frameCount, chunkCount);
            if (packed.empty()) {
                result.error = QStringLiteral("rigid 32-bit pack produced empty buffer");
                return result;
            }
            wrote = MinimalEXR::writeRGBA32F(result.posTexPath, imgWidth, imgHeight, packed);
        } else {
            auto packed = packRigid16(rigidSamples, frameCount, chunkCount, roundedLo, roundedHi);
            if (packed.empty()) {
                result.error = QStringLiteral("rigid pack produced empty buffer");
                return result;
            }
            wrote = (bitDepth == 8)
                ? writePng8From4(result.posTexPath, imgWidth, imgHeight, packed)
                : writePng16From4(result.posTexPath, imgWidth, imgHeight, packed);
        }
    } else if (bitDepth == 32) {
        // 32-bit float EXR. Bypasses uint16 quantization entirely: the
        // texture stores raw post-skin meters; the consumer reads them
        // back via `texel.rgb` directly (no bounds_min/bounds_max
        // decode). Eliminates the sub-mm precision artifacts that
        // cause Mixamo's eye-sphere/head-plug z-fights to flicker on
        // adjacent frames.
        auto packed = packOpenVAT32(flat, normals, frameCount, vertexCount,
                                    opts.vertexPermutation);
        if (packed.empty()) {
            result.error = QStringLiteral("OpenVAT 32-bit pack produced empty buffer");
            return result;
        }
        wrote = MinimalEXR::writeRGB32F(result.posTexPath, vertexCount, imgHeight, packed);
    } else {
        auto packed = packOpenVAT16(flat, normals, frameCount, vertexCount,
                                    roundedLo, roundedHi,
                                    opts.vertexPermutation);
        if (packed.empty()) {
            result.error = QStringLiteral("OpenVAT pack produced empty buffer");
            return result;
        }
        wrote = (bitDepth == 8)
            ? writePng8From3(result.posTexPath, vertexCount, imgHeight, packed)
            : writePng16From3(result.posTexPath, vertexCount, imgHeight, packed);
    }
    if (!wrote) {
        result.error = QStringLiteral("failed to write OpenVAT texture: %1")
                           .arg(result.posTexPath);
        return result;
    }

    const QString sidecar = buildOpenVATSidecar(result, bitDepth, opts.target);
    QString sidecarErr;
    if (!writeSidecarFile(result.jsonPath, sidecar, &sidecarErr)) {
        result.error = sidecarErr;
        return result;
    }

    result.ok = true;
    return result;
}
