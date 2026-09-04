/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "VATBaker.h"

#include "MinimalEXRWriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstring>

#include <OgreAnimationState.h>
#include <OgreCommon.h>
#include <OgreEntity.h>
#include <OgreFrameListener.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreMesh.h>
#include <OgreRoot.h>
#include <OgreSkeleton.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// Read one frame of post-skin positions from `entity`. The vertex
// ordering follows the same submesh walk NormalVisualizer uses:
// shared vertex data appears first (and only once across all submeshes
// that share it), then each non-shared submesh contributes its own
// vertex range in submesh-index order.
//
// Returns the appended count for the caller's sanity check.
size_t collectPostSkinPositions(Ogre::Entity* entity,
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

        Ogre::VertexData* animData = sub->useSharedVertices
            ? entity->_getSkelAnimVertexData()
            : entity->getSubEntity(si)->_getSkelAnimVertexData();
        if (!animData) continue;

        const auto* posElem = animData->vertexDeclaration->findElementBySemantic(
            Ogre::VES_POSITION);
        if (!posElem) continue;

        auto vbuf = animData->vertexBufferBinding->getBuffer(posElem->getSource());
        if (!vbuf) continue;

        auto* bytes = static_cast<unsigned char*>(
            vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        const size_t vstride = vbuf->getVertexSize();

        for (size_t j = 0; j < animData->vertexCount; ++j) {
            Ogre::Real* pPos = nullptr;
            posElem->baseVertexPointerToElement(bytes + j * vstride, &pPos);
            out.emplace_back(pPos[0], pPos[1], pPos[2]);
            ++appended;
        }

        vbuf->unlock();

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

// Same submesh walk as collectPostSkinPositions but for normals.
//
// On a submesh without `VES_NORMAL`, returns SIZE_MAX as a sentinel so
// the caller can fail the bake with a clear error. We deliberately do
// NOT pad with fabricated up-vectors — that produces a normal texture
// that looks plausible but lights the mesh wrong while reporting
// success, which is worse than refusing to bake.
constexpr size_t kCollectNormalsMissingSentinel =
    std::numeric_limits<size_t>::max();

size_t collectPostSkinNormals(Ogre::Entity* entity,
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

        Ogre::VertexData* animData = sub->useSharedVertices
            ? entity->_getSkelAnimVertexData()
            : entity->getSubEntity(si)->_getSkelAnimVertexData();
        if (!animData) continue;

        const auto* normElem = animData->vertexDeclaration->findElementBySemantic(
            Ogre::VES_NORMAL);
        if (!normElem) {
            // Missing-normal submesh: surface as a hard failure rather
            // than fabricate up-vectors. The caller's error message
            // identifies which submesh hit this.
            return kCollectNormalsMissingSentinel;
        }

        auto vbuf = animData->vertexBufferBinding->getBuffer(normElem->getSource());
        if (!vbuf) continue;

        auto* bytes = static_cast<unsigned char*>(
            vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        const size_t vstride = vbuf->getVertexSize();

        for (size_t j = 0; j < animData->vertexCount; ++j) {
            Ogre::Real* pNorm = nullptr;
            normElem->baseVertexPointerToElement(bytes + j * vstride, &pNorm);
            out.emplace_back(pNorm[0], pNorm[1], pNorm[2]);
            ++appended;
        }

        vbuf->unlock();

        if (sub->useSharedVertices) sharedAppended = true;
    }

    return appended;
}

} // namespace

// ---------------------------------------------------------------------------
// Sidecar emission.
// ---------------------------------------------------------------------------

namespace { QString buildOpenVATSidecar(int frameCount,
                                        const Ogre::Vector3& lo,
                                        const Ogre::Vector3& hi,
                                        int bitDepth); }

QString VATBaker::buildSidecarJson(const BakeResult& result,
                                   const Options& opts)
{
    return buildOpenVATSidecar(result.frameCount,
                               result.minBound,
                               result.maxBound,
                               opts.bitDepth);
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
// `_origin` key so a consumer's shader knows what swizzle to apply.

// Round a float OUTWARD to the nearest 0.1 — floor for min, ceil for
// max — matching openvat's `round_to_nearest_ten` (utils.py:186-188).
// We never want a bound tighter than the actual data, so even on a
// value already exactly at 0.1 we still pad outward by 0.1 to match
// the Blender add-on's behavior (their math.floor(v*10)/10 yields the
// same value, then min/max are intentionally not snapped tighter).
inline float openvatRoundMin(float v) {
    // floor(v * 10) / 10 — pulls down to the nearest multiple of 0.1.
    return std::floor(v * 10.0f) / 10.0f;
}
inline float openvatRoundMax(float v) {
    // ceil(v * 10) / 10 — pushes up to the nearest multiple of 0.1.
    return std::ceil(v * 10.0f) / 10.0f;
}

// Format a float as an 8-decimal-place string with trailing zeros
// preserved (matches openvat's `CustomEncoder` `"%.8f"`).
inline QString openvatFormatFloat(float v) {
    return QString::number(static_cast<double>(v), 'f', 8);
}

// Build the openvat sidecar JSON. Returned string is a single root
// object with `os-remap` at the top level. Optional `_origin` key
// documents our source coordinate space for shader authors.
// `lo`/`hi` are expected to already be on the 0.1 OpenVAT grid (use
// openvatRoundMin/openvatRoundMax to snap before calling). We just
// format them; rounding here would silently re-snap rounded values
// and decouple the sidecar from whatever the texture encoder used.
//
// `bitDepth` is recorded as an extension field (`_bit_depth`) so a
// consumer can tell at a glance whether the texture next to this
// sidecar is uint16 (PNG) or float32 (EXR). Standard openvat
// consumers ignore unknown top-level keys; the field is purely for
// our own consumers (the Unreal demo dispatches on it).
QString buildOpenVATSidecar(int frameCount,
                            const Ogre::Vector3& lo,
                            const Ogre::Vector3& hi,
                            int bitDepth = 16)
{
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
    osRemap["Frames"] = frameCount;

    QJsonObject root;
    root["os-remap"] = osRemap;
    // Non-standard extension keys — openvat consumer shaders ignore
    // unknown top-level fields. `_producer` identifies the tool that
    // wrote the file. `_axes` documents the source coordinate
    // convention so shader authors know what swizzle to apply on read
    // (the openvat Godot reference shader hardcodes a Blender→Godot
    // `vec3(x, z, -y)` swizzle; our output is Y-up right-handed Ogre,
    // which needs a different one).
    root["_producer"]  = QStringLiteral("QtMeshEditor");
    root["_axes"]      = QStringLiteral("y-up-rh");
    // Per-channel bit depth of the position+normal texture sitting
    // next to this sidecar. 16 → PNG (uint16), 32 → EXR (float32).
    // Consumers that don't care can ignore it.
    root["_bit_depth"] = bitDepth;

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

} // namespace

// ---------------------------------------------------------------------------
// bake()
// ---------------------------------------------------------------------------

VATBaker::BakeResult VATBaker::bake(Ogre::Entity* entity, const Options& opts)
{
    BakeResult result;

    if (!entity) {
        result.error = QStringLiteral("entity is null");
        return result;
    }
    if (!entity->hasSkeleton()) {
        result.error = QStringLiteral("entity has no skeleton");
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

    auto* states = entity->getAllAnimationStates();
    if (!states || !states->hasAnimationState(opts.animationName.toStdString())) {
        result.error = QStringLiteral("animation '%1' not found").arg(opts.animationName);
        return result;
    }
    auto* state = states->getAnimationState(opts.animationName.toStdString());

    // Disable every animation state first so a previously-enabled state
    // doesn't blend into the bake.
    auto it = states->getAnimationStateIterator();
    while (it.hasMoreElements()) {
        auto* s = it.getNext();
        if (s) s->setEnabled(false);
    }
    state->setEnabled(true);

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

    // Ensure CPU-side skin data is available even when no render is
    // happening (which is the case during a headless bake).
    entity->addSoftwareAnimationRequest(true);

    std::vector<Ogre::Vector3> flat;
    std::vector<Ogre::Vector3> normals;
    Ogre::Vector3 lo(std::numeric_limits<float>::infinity());
    Ogre::Vector3 hi(-std::numeric_limits<float>::infinity());
    flat.reserve(static_cast<size_t>(frameCount) * 1024);
    normals.reserve(static_cast<size_t>(frameCount) * 1024);

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
        const size_t appended = collectPostSkinPositions(entity, flat);
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

        // EXPORT space (provenance-gated, see Options::exportSpaceMirrorZ):
        // the glTF exporter mirrors Z for Assimp-imported meshes (inverse of
        // the import's ConvertToLeftHanded), and the consumer pairs this
        // bake with that exported mesh, so the texture must match its space.
        if (opts.exportSpaceMirrorZ)
            for (size_t i = before; i < flat.size(); ++i)
                flat[i].z = -flat[i].z;

        for (size_t i = before; i < flat.size(); ++i) {
            const auto& p = flat[i];
            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
        }

        const size_t nrmAppended = collectPostSkinNormals(entity, normals);
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
        // Same export-space Z mirror as positions (a mirror maps normals
        // the same way: n.z = -n.z).
        if (opts.exportSpaceMirrorZ)
            for (size_t i = normals.size() - nrmAppended; i < normals.size(); ++i)
                normals[i].z = -normals[i].z;
    }

    entity->removeSoftwareAnimationRequest(true);

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
    result.vertexCount = vertexCount;
    result.minBound    = roundedLo;
    result.maxBound    = roundedHi;

    // bitDepth in {16, 32}; anything else is a caller bug. Normalize
    // here so the rest of bake() doesn't have to defend against it.
    const int bitDepth = (opts.bitDepth == 32) ? 32 : 16;

    QDir().mkpath(opts.outputDir);
    const QString base = opts.basename.isEmpty() ? opts.animationName : opts.basename;
    const char* posExt = (bitDepth == 32) ? "_pos.exr" : "_pos.png";
    result.posTexPath = QDir(opts.outputDir).filePath(base + QString::fromLatin1(posExt));
    result.jsonPath   = QDir(opts.outputDir).filePath(base + "-remap_info.json");

    if (!opts.vertexPermutation.empty()) {
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

    if (bitDepth == 32) {
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
        if (!MinimalEXR::writeRGB32F(result.posTexPath, vertexCount,
                                     imgHeight, packed)) {
            result.error = QStringLiteral("failed to write OpenVAT EXR: %1")
                               .arg(result.posTexPath);
            return result;
        }
    } else {
        auto packed = packOpenVAT16(flat, normals, frameCount, vertexCount,
                                    roundedLo, roundedHi,
                                    opts.vertexPermutation);
        if (packed.empty()) {
            result.error = QStringLiteral("OpenVAT pack produced empty buffer");
            return result;
        }
        // RGBX64 is Qt's 16-bit-per-channel 4-channel format. The X channel
        // is padding; PNG can store 3-channel data losslessly but Qt's PNG
        // writer infers RGB-vs-RGBA from the QImage format, and Format_RGB
        // doesn't exist at 16-bit precision. Padding to RGBX64 costs a few
        // hundred KB on a 5828×142 image — acceptable for a one-off bake.
        QImage img(vertexCount, imgHeight, QImage::Format_RGBX64);
        img.fill(0);
        for (int y = 0; y < imgHeight; ++y) {
            const uint16_t* src = packed.data()
                                + static_cast<size_t>(y)
                                  * static_cast<size_t>(vertexCount) * 3u;
            auto* dst = reinterpret_cast<uint16_t*>(img.scanLine(y));
            for (int x = 0; x < vertexCount; ++x) {
                dst[x * 4 + 0] = src[x * 3 + 0];
                dst[x * 4 + 1] = src[x * 3 + 1];
                dst[x * 4 + 2] = src[x * 3 + 2];
                dst[x * 4 + 3] = 65535;
            }
        }
        if (!img.save(result.posTexPath, "PNG")) {
            result.error = QStringLiteral("failed to write OpenVAT texture: %1")
                               .arg(result.posTexPath);
            return result;
        }
    }

    const QString sidecar = buildOpenVATSidecar(frameCount, roundedLo, roundedHi, bitDepth);
    QFile jf(result.jsonPath);
    if (!jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.error = QStringLiteral("failed to open OpenVAT sidecar for write: %1")
                           .arg(result.jsonPath);
        return result;
    }
    const QByteArray sidecarBytes = sidecar.toUtf8();
    const qint64 written = jf.write(sidecarBytes);
    jf.close();
    if (written != sidecarBytes.size()) {
        // Short write — disk full, network volume hiccup, etc. Surface
        // it instead of leaving a truncated sidecar behind that the
        // consumer would read partially and misdecode against.
        QFile::remove(result.jsonPath);
        result.error = QStringLiteral(
            "short write to OpenVAT sidecar %1 (wrote %2 of %3 bytes)")
                .arg(result.jsonPath).arg(written).arg(sidecarBytes.size());
        return result;
    }

    result.ok = true;
    return result;
}
