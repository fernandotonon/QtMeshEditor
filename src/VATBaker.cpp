/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "VATBaker.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

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

inline unsigned char toByteNormalised(float v, float lo, float hi)
{
    if (hi <= lo) return 0;
    const float t = (v - lo) / (hi - lo);
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return static_cast<unsigned char>(std::lround(clamped * 255.0f));
}

inline float fromByteNormalised(unsigned char b, float lo, float hi)
{
    return lo + (static_cast<float>(b) / 255.0f) * (hi - lo);
}

inline uint16_t toShortNormalised(float v, float lo, float hi)
{
    if (hi <= lo) return 0;
    const float t = (v - lo) / (hi - lo);
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return static_cast<uint16_t>(std::lround(clamped * 65535.0f));
}

inline float fromShortNormalised(uint16_t s, float lo, float hi)
{
    return lo + (static_cast<float>(s) / 65535.0f) * (hi - lo);
}

// Same submesh walk as collectPostSkinPositions but for normals. Kept
// separate so the position-only path (slice 1) doesn't lock the normal
// buffer when no one will read it — the second lock acquisition on the
// same vbuf is cheap but pointless.
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
// Static encoders (public for tests).
// ---------------------------------------------------------------------------

std::vector<unsigned char> VATBaker::encodeRGBA8(
    const std::vector<Ogre::Vector3>& flatPositions,
    int frameCount,
    int vertexCount,
    const Ogre::Vector3& minBound,
    const Ogre::Vector3& maxBound)
{
    const size_t expected = static_cast<size_t>(frameCount)
                          * static_cast<size_t>(vertexCount);
    std::vector<unsigned char> out;
    if (frameCount <= 0 || vertexCount <= 0) return out;
    if (flatPositions.size() != expected) return out;
    out.resize(expected * 4u, 0);
    for (size_t i = 0; i < expected; ++i) {
        const auto& p = flatPositions[i];
        out[i * 4 + 0] = toByteNormalised(p.x, minBound.x, maxBound.x);
        out[i * 4 + 1] = toByteNormalised(p.y, minBound.y, maxBound.y);
        out[i * 4 + 2] = toByteNormalised(p.z, minBound.z, maxBound.z);
        out[i * 4 + 3] = 255;
    }
    return out;
}

std::vector<Ogre::Vector3> VATBaker::decodeRGBA8(
    const std::vector<unsigned char>& pixels,
    int frameCount,
    int vertexCount,
    const Ogre::Vector3& minBound,
    const Ogre::Vector3& maxBound)
{
    const size_t expected = static_cast<size_t>(frameCount)
                          * static_cast<size_t>(vertexCount);
    std::vector<Ogre::Vector3> out;
    if (frameCount <= 0 || vertexCount <= 0) return out;
    if (pixels.size() != expected * 4u) return out;
    out.reserve(expected);
    for (size_t i = 0; i < expected; ++i) {
        Ogre::Vector3 p;
        p.x = fromByteNormalised(pixels[i * 4 + 0], minBound.x, maxBound.x);
        p.y = fromByteNormalised(pixels[i * 4 + 1], minBound.y, maxBound.y);
        p.z = fromByteNormalised(pixels[i * 4 + 2], minBound.z, maxBound.z);
        out.push_back(p);
    }
    return out;
}

std::vector<uint16_t> VATBaker::encodeRGBA16(
    const std::vector<Ogre::Vector3>& flatPositions,
    int frameCount,
    int vertexCount,
    const Ogre::Vector3& minBound,
    const Ogre::Vector3& maxBound)
{
    const size_t expected = static_cast<size_t>(frameCount)
                          * static_cast<size_t>(vertexCount);
    std::vector<uint16_t> out;
    if (frameCount <= 0 || vertexCount <= 0) return out;
    if (flatPositions.size() != expected) return out;
    out.resize(expected * 4u, 0);
    for (size_t i = 0; i < expected; ++i) {
        const auto& p = flatPositions[i];
        out[i * 4 + 0] = toShortNormalised(p.x, minBound.x, maxBound.x);
        out[i * 4 + 1] = toShortNormalised(p.y, minBound.y, maxBound.y);
        out[i * 4 + 2] = toShortNormalised(p.z, minBound.z, maxBound.z);
        out[i * 4 + 3] = 65535;
    }
    return out;
}

std::vector<Ogre::Vector3> VATBaker::decodeRGBA16(
    const std::vector<uint16_t>& pixels,
    int frameCount,
    int vertexCount,
    const Ogre::Vector3& minBound,
    const Ogre::Vector3& maxBound)
{
    const size_t expected = static_cast<size_t>(frameCount)
                          * static_cast<size_t>(vertexCount);
    std::vector<Ogre::Vector3> out;
    if (frameCount <= 0 || vertexCount <= 0) return out;
    if (pixels.size() != expected * 4u) return out;
    out.reserve(expected);
    for (size_t i = 0; i < expected; ++i) {
        Ogre::Vector3 p;
        p.x = fromShortNormalised(pixels[i * 4 + 0], minBound.x, maxBound.x);
        p.y = fromShortNormalised(pixels[i * 4 + 1], minBound.y, maxBound.y);
        p.z = fromShortNormalised(pixels[i * 4 + 2], minBound.z, maxBound.z);
        out.push_back(p);
    }
    return out;
}

std::vector<unsigned char> VATBaker::encodeNormalsRGBA8(
    const std::vector<Ogre::Vector3>& flatNormals,
    int frameCount,
    int vertexCount)
{
    const size_t expected = static_cast<size_t>(frameCount)
                          * static_cast<size_t>(vertexCount);
    std::vector<unsigned char> out;
    if (frameCount <= 0 || vertexCount <= 0) return out;
    if (flatNormals.size() != expected) return out;
    out.resize(expected * 4u, 0);
    for (size_t i = 0; i < expected; ++i) {
        // Normals come in as unit vectors in [-1, 1]; remap to [0, 1]
        // for the unsigned byte channel.
        const auto& n = flatNormals[i];
        const float nx = std::clamp((n.x + 1.0f) * 0.5f, 0.0f, 1.0f);
        const float ny = std::clamp((n.y + 1.0f) * 0.5f, 0.0f, 1.0f);
        const float nz = std::clamp((n.z + 1.0f) * 0.5f, 0.0f, 1.0f);
        out[i * 4 + 0] = static_cast<unsigned char>(std::lround(nx * 255.0f));
        out[i * 4 + 1] = static_cast<unsigned char>(std::lround(ny * 255.0f));
        out[i * 4 + 2] = static_cast<unsigned char>(std::lround(nz * 255.0f));
        out[i * 4 + 3] = 255;
    }
    return out;
}

std::vector<Ogre::Vector3> VATBaker::decodeNormalsRGBA8(
    const std::vector<unsigned char>& pixels,
    int frameCount,
    int vertexCount)
{
    const size_t expected = static_cast<size_t>(frameCount)
                          * static_cast<size_t>(vertexCount);
    std::vector<Ogre::Vector3> out;
    if (frameCount <= 0 || vertexCount <= 0) return out;
    if (pixels.size() != expected * 4u) return out;
    out.reserve(expected);
    for (size_t i = 0; i < expected; ++i) {
        Ogre::Vector3 n;
        n.x = (static_cast<float>(pixels[i * 4 + 0]) / 255.0f) * 2.0f - 1.0f;
        n.y = (static_cast<float>(pixels[i * 4 + 1]) / 255.0f) * 2.0f - 1.0f;
        n.z = (static_cast<float>(pixels[i * 4 + 2]) / 255.0f) * 2.0f - 1.0f;
        out.push_back(n);
    }
    return out;
}

std::vector<uint16_t> VATBaker::encodeNormalsRGBA16(
    const std::vector<Ogre::Vector3>& flatNormals,
    int frameCount,
    int vertexCount)
{
    const size_t expected = static_cast<size_t>(frameCount)
                          * static_cast<size_t>(vertexCount);
    std::vector<uint16_t> out;
    if (frameCount <= 0 || vertexCount <= 0) return out;
    if (flatNormals.size() != expected) return out;
    out.resize(expected * 4u, 0);
    for (size_t i = 0; i < expected; ++i) {
        const auto& n = flatNormals[i];
        const float nx = std::clamp((n.x + 1.0f) * 0.5f, 0.0f, 1.0f);
        const float ny = std::clamp((n.y + 1.0f) * 0.5f, 0.0f, 1.0f);
        const float nz = std::clamp((n.z + 1.0f) * 0.5f, 0.0f, 1.0f);
        out[i * 4 + 0] = static_cast<uint16_t>(std::lround(nx * 65535.0f));
        out[i * 4 + 1] = static_cast<uint16_t>(std::lround(ny * 65535.0f));
        out[i * 4 + 2] = static_cast<uint16_t>(std::lround(nz * 65535.0f));
        out[i * 4 + 3] = 65535;
    }
    return out;
}

std::vector<Ogre::Vector3> VATBaker::decodeNormalsRGBA16(
    const std::vector<uint16_t>& pixels,
    int frameCount,
    int vertexCount)
{
    const size_t expected = static_cast<size_t>(frameCount)
                          * static_cast<size_t>(vertexCount);
    std::vector<Ogre::Vector3> out;
    if (frameCount <= 0 || vertexCount <= 0) return out;
    if (pixels.size() != expected * 4u) return out;
    out.reserve(expected);
    for (size_t i = 0; i < expected; ++i) {
        Ogre::Vector3 n;
        n.x = (static_cast<float>(pixels[i * 4 + 0]) / 65535.0f) * 2.0f - 1.0f;
        n.y = (static_cast<float>(pixels[i * 4 + 1]) / 65535.0f) * 2.0f - 1.0f;
        n.z = (static_cast<float>(pixels[i * 4 + 2]) / 65535.0f) * 2.0f - 1.0f;
        out.push_back(n);
    }
    return out;
}

QString VATBaker::buildSidecarJson(const BakeResult& result, const Options& opts)
{
    QJsonObject bounds;
    QJsonObject lo, hi;
    lo["x"] = result.minBound.x; lo["y"] = result.minBound.y; lo["z"] = result.minBound.z;
    hi["x"] = result.maxBound.x; hi["y"] = result.maxBound.y; hi["z"] = result.maxBound.z;
    bounds["min"] = lo;
    bounds["max"] = hi;

    QString encStr;
    switch (opts.encoding) {
        case Encoding::RGBA8:  encStr = QStringLiteral("rgba8");  break;
        case Encoding::RGBA16: encStr = QStringLiteral("rgba16"); break;
    }
    QString tgtStr;
    switch (opts.target) {
        case Target::Agnostic: tgtStr = QStringLiteral("agnostic"); break;
        case Target::Unity:    tgtStr = QStringLiteral("unity");    break;
        case Target::Unreal:   tgtStr = QStringLiteral("unreal");   break;
        case Target::Godot:    tgtStr = QStringLiteral("godot");    break;
    }

    QJsonObject root;
    root["version"]     = 1;
    root["target"]      = tgtStr;
    root["encoding"]    = encStr;
    root["frameCount"]  = result.frameCount;
    root["vertexCount"] = result.vertexCount;
    root["fps"]         = opts.fps;
    root["animation"]   = opts.animationName;
    root["bounds"]      = bounds;
    root["posTexture"]  = QFileInfo(result.posTexPath).fileName();
    if (!result.nrmTexPath.isEmpty())
        root["nrmTexture"] = QFileInfo(result.nrmTexPath).fileName();

    QJsonDocument doc(root);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

// ---------------------------------------------------------------------------
// Per-target conventions.
// ---------------------------------------------------------------------------

namespace {

// Apply per-engine axis swizzle to a single position vector. Sampling
// happens in Ogre space (Y-up, right-handed); the resulting texture
// has to be readable by the target engine without runtime fix-ups, so
// the swizzle happens once at encode time and is documented in the
// sidecar.
//
// - Agnostic / Unity / Godot — no swizzle. All three are Y-up + right-
//   handed (Unity is Y-up; Godot is Y-up; agnostic is whatever the
//   downstream tooling expects). Unity's vertical-flip is handled by
//   reversing the row order, not the axes.
// - Unreal — Z-up + left-handed. Standard Ogre→Unreal remap is
//   `(x, y, z) → (x, z, y)` (swap Y/Z to lift "up" onto Z). Unreal
//   handles its own LH chirality in the importer.
inline Ogre::Vector3 swizzleForTarget(VATBaker::Target t, const Ogre::Vector3& v)
{
    switch (t) {
        case VATBaker::Target::Unreal: return { v.x, v.z, v.y };
        default:                       return v;
    }
}

// Unity's PNG loader treats row 0 as the top of the image, whereas
// the VAT layout has frame 0 at the top already — but Unity's auto-
// generated UV convention then flips V again at sample time. The
// net effect is that *for Unity*, the runtime shader would have to
// compute `frameIndex` as `frameCount - 1 - row` if we wrote frames
// top-down. Pre-flip the rows here so the shader can use the plain
// `row / frameCount` indexing on every target.
inline bool targetFlipsRowsAtWriteTime(VATBaker::Target t)
{
    return t == VATBaker::Target::Unity;
}

// Minimal Unity `.meta` sidecar. Hand-written rather than full Unity
// YAML — Unity's importer is happy with the standard texture-asset
// shape and rejects unknown keys silently. The values matter:
//   - `guid` must be a unique 32-char hex string. Sharing a GUID
//     between assets causes Unity to silently remap references at
//     import time or refuse to import the duplicate altogether.
//     We generate a fresh random GUID per bake.
//   - `sRGBTexture: 0`  → data texture, no gamma curve.
//   - `textureCompression: 0`  → uncompressed, lossless.
//   - `filterMode: 0`  → point/nearest, no bilinear smearing
//     between vertex columns.
//   - `wrapU: 1` / `wrapV: 1`  → clamp, so OOB UVs don't wrap.
QString buildUnityMeta()
{
    // QUuid::toString returns `{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}`;
    // strip the braces and dashes to land on Unity's 32-hex-char shape.
    const QString guid = QUuid::createUuid()
                             .toString(QUuid::WithoutBraces)
                             .remove(QChar('-'));
    return QStringLiteral(
        "fileFormatVersion: 2\n"
        "guid: %1\n"
        "TextureImporter:\n"
        "  serializedVersion: 11\n"
        "  sRGBTexture: 0\n"
        "  textureCompression: 0\n"
        "  filterMode: 0\n"
        "  wrapU: 1\n"
        "  wrapV: 1\n").arg(guid);
}

// Minimal Godot 4 spatial shader that samples the position texture
// and applies the per-vertex offset. Users typically copy this into
// a Material on a MeshInstance3D. Bounds + frame count are encoded
// as uniform defaults pulled from the sidecar at bake time.
QString buildGodotShader(int frameCount,
                         int vertexCount,
                         const Ogre::Vector3& lo,
                         const Ogre::Vector3& hi)
{
    return QStringLiteral(
        "shader_type spatial;\n"
        "render_mode unshaded;\n"
        "\n"
        "uniform sampler2D pos_tex : filter_nearest;\n"
        "uniform float current_frame = 0.0;\n"
        "uniform int frame_count = %1;\n"
        "uniform int vertex_count = %2;\n"
        "uniform vec3 bounds_min = vec3(%3, %4, %5);\n"
        "uniform vec3 bounds_max = vec3(%6, %7, %8);\n"
        "\n"
        "void vertex() {\n"
        "    float u = (float(VERTEX_ID) + 0.5) / float(vertex_count);\n"
        "    float v = (current_frame + 0.5) / float(frame_count);\n"
        "    vec3 t = texture(pos_tex, vec2(u, v)).rgb;\n"
        "    VERTEX = bounds_min + t * (bounds_max - bounds_min);\n"
        "}\n")
        .arg(frameCount).arg(vertexCount)
        .arg(lo.x, 0, 'f', 6).arg(lo.y, 0, 'f', 6).arg(lo.z, 0, 'f', 6)
        .arg(hi.x, 0, 'f', 6).arg(hi.y, 0, 'f', 6).arg(hi.z, 0, 'f', 6);
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

    // First sample to discover the vertex count + seed the bounds.
    std::vector<Ogre::Vector3> flat;
    std::vector<Ogre::Vector3> normals;  // populated only when opts.bakeNormals
    Ogre::Vector3 lo(std::numeric_limits<float>::infinity());
    Ogre::Vector3 hi(-std::numeric_limits<float>::infinity());
    flat.reserve(static_cast<size_t>(frameCount) * 1024);
    if (opts.bakeNormals)
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
        // Bump Ogre's global frame counter so
        // Entity::cacheBoneMatrices reads the AnimationState (the
        // cache key on line 1300 of OgreEntity.cpp is
        // Root::getNextFrameNumber). In a headless bake we never
        // call Root::renderOneFrame, so without this manual bump
        // the bone matrices are computed once for the first frame
        // and then cached forever — every subsequent setTimePosition
        // updates state.time but the SW-skinned vertex buffer never
        // gets re-blended, so every row of the bake's position
        // texture ends up identical to row 0. The Godot test harness
        // (#371 follow-up) exposed this; tests passed because they
        // only checked file dimensions, not per-row content.
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

        // Apply per-target axis swizzle in-place on the just-appended
        // positions so the bounds computed below match the encoded
        // space. (Cheaper than walking the buffer twice — and keeps
        // the bounds → encoder → decoder chain consistent.)
        for (size_t i = before; i < flat.size(); ++i) {
            flat[i] = swizzleForTarget(opts.target, flat[i]);
            const auto& p = flat[i];
            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
        }

        if (opts.bakeNormals) {
            const size_t nrmBefore = normals.size();
            const size_t nrmAppended = collectPostSkinNormals(entity, normals);
            if (nrmAppended == kCollectNormalsMissingSentinel) {
                entity->removeSoftwareAnimationRequest(true);
                result.error = QStringLiteral(
                    "frame %1 has a submesh without VES_NORMAL — "
                    "cannot bake normals (regenerate normals on the source mesh "
                    "or omit --normals)")
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
            // Apply the same axis swizzle to normals — they live in
            // the same coordinate frame as positions. (Direction
            // vectors don't have a separate translation component, so
            // the swizzle is exactly the same operation.)
            for (size_t i = nrmBefore; i < normals.size(); ++i) {
                normals[i] = swizzleForTarget(opts.target, normals[i]);
            }
        }
    }

    entity->removeSoftwareAnimationRequest(true);

    // Degenerate bounds (single point on an axis): pad by 1 unit so the
    // encoder doesn't divide-by-zero and the runtime decode reads back
    // the constant value. Choice of pad is irrelevant — every sample
    // lands at the same byte.
    if (hi.x <= lo.x) hi.x = lo.x + 1.0f;
    if (hi.y <= lo.y) hi.y = lo.y + 1.0f;
    if (hi.z <= lo.z) hi.z = lo.z + 1.0f;

    result.frameCount  = frameCount;
    result.vertexCount = vertexCount;
    result.minBound    = lo;
    result.maxBound    = hi;

    // Ensure output dir exists.
    QDir().mkpath(opts.outputDir);
    const QString base = opts.basename.isEmpty() ? opts.animationName : opts.basename;
    result.posTexPath = QDir(opts.outputDir).filePath(base + "_pos.png");
    result.jsonPath   = QDir(opts.outputDir).filePath(base + ".json");

    const bool flipRows = targetFlipsRowsAtWriteTime(opts.target);

    // Encode + write the position texture. PNG preserves both 8-bit
    // and 16-bit channel depths; QImage handles the format switch for us.
    if (opts.encoding == Encoding::RGBA8) {
        auto rgba = encodeRGBA8(flat, frameCount, vertexCount, lo, hi);
        if (rgba.empty()) {
            result.error = QStringLiteral("encodeRGBA8 produced empty buffer");
            return result;
        }
        QImage img(vertexCount, frameCount, QImage::Format_RGBA8888);
        for (int y = 0; y < frameCount; ++y) {
            const int srcRow = flipRows ? (frameCount - 1 - y) : y;
            const unsigned char* src = rgba.data() + static_cast<size_t>(srcRow)
                                                      * static_cast<size_t>(vertexCount) * 4u;
            std::memcpy(img.scanLine(y), src, static_cast<size_t>(vertexCount) * 4u);
        }
        if (!img.save(result.posTexPath, "PNG")) {
            result.error = QStringLiteral("failed to write position texture: %1")
                               .arg(result.posTexPath);
            return result;
        }
    } else {  // RGBA16
        auto rgba = encodeRGBA16(flat, frameCount, vertexCount, lo, hi);
        if (rgba.empty()) {
            result.error = QStringLiteral("encodeRGBA16 produced empty buffer");
            return result;
        }
        // Format_RGBA64 is 16 bits per channel — Qt's `save("PNG")` writes
        // a 16-bit PNG which Godot/Unity/Unreal all read losslessly.
        QImage img(vertexCount, frameCount, QImage::Format_RGBA64);
        for (int y = 0; y < frameCount; ++y) {
            const int srcRow = flipRows ? (frameCount - 1 - y) : y;
            const uint16_t* src = rgba.data() + static_cast<size_t>(srcRow)
                                                * static_cast<size_t>(vertexCount) * 4u;
            std::memcpy(img.scanLine(y), src, static_cast<size_t>(vertexCount) * 4u * sizeof(uint16_t));
        }
        if (!img.save(result.posTexPath, "PNG")) {
            result.error = QStringLiteral("failed to write 16-bit position texture: %1")
                               .arg(result.posTexPath);
            return result;
        }
    }

    // Normal texture — only when requested. Same layout / size, normals
    // mapped from [-1, 1] → [0, MAX] per channel.
    if (opts.bakeNormals) {
        result.nrmTexPath = QDir(opts.outputDir).filePath(base + "_nrm.png");
        if (opts.encoding == Encoding::RGBA8) {
            auto rgba = encodeNormalsRGBA8(normals, frameCount, vertexCount);
            if (rgba.empty()) {
                result.error = QStringLiteral("encodeNormalsRGBA8 produced empty buffer");
                return result;
            }
            QImage img(vertexCount, frameCount, QImage::Format_RGBA8888);
            for (int y = 0; y < frameCount; ++y) {
                const int srcRow = flipRows ? (frameCount - 1 - y) : y;
                const unsigned char* src = rgba.data() + static_cast<size_t>(srcRow)
                                                          * static_cast<size_t>(vertexCount) * 4u;
                std::memcpy(img.scanLine(y), src, static_cast<size_t>(vertexCount) * 4u);
            }
            if (!img.save(result.nrmTexPath, "PNG")) {
                result.error = QStringLiteral("failed to write normal texture: %1")
                                   .arg(result.nrmTexPath);
                return result;
            }
        } else {  // RGBA16
            auto rgba = encodeNormalsRGBA16(normals, frameCount, vertexCount);
            if (rgba.empty()) {
                result.error = QStringLiteral("encodeNormalsRGBA16 produced empty buffer");
                return result;
            }
            QImage img(vertexCount, frameCount, QImage::Format_RGBA64);
            for (int y = 0; y < frameCount; ++y) {
                const int srcRow = flipRows ? (frameCount - 1 - y) : y;
                const uint16_t* src = rgba.data() + static_cast<size_t>(srcRow)
                                                    * static_cast<size_t>(vertexCount) * 4u;
                std::memcpy(img.scanLine(y), src, static_cast<size_t>(vertexCount) * 4u * sizeof(uint16_t));
            }
            if (!img.save(result.nrmTexPath, "PNG")) {
                result.error = QStringLiteral("failed to write 16-bit normal texture: %1")
                                   .arg(result.nrmTexPath);
                return result;
            }
        }
    }

    // Sidecar.
    const QString sidecar = buildSidecarJson(result, opts);
    QFile jf(result.jsonPath);
    if (!jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.error = QStringLiteral("failed to open JSON for write: %1")
                           .arg(result.jsonPath);
        return result;
    }
    jf.write(sidecar.toUtf8());
    jf.close();

    // Per-engine extras — emitted alongside the JSON sidecar so a
    // dropped-in asset folder is engine-ready without further tooling.
    if (opts.target == Target::Unity) {
        result.unityMetaPath = result.posTexPath + QStringLiteral(".meta");
        QFile mf(result.unityMetaPath);
        if (!mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            result.error = QStringLiteral("failed to open .meta for write: %1")
                               .arg(result.unityMetaPath);
            return result;
        }
        mf.write(buildUnityMeta().toUtf8());
        mf.close();
    } else if (opts.target == Target::Godot) {
        result.godotShaderPath = QDir(opts.outputDir).filePath(base + ".gdshader");
        QFile sf(result.godotShaderPath);
        if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            result.error = QStringLiteral("failed to open .gdshader for write: %1")
                               .arg(result.godotShaderPath);
            return result;
        }
        sf.write(buildGodotShader(frameCount, vertexCount, lo, hi).toUtf8());
        sf.close();
    }
    // Target::Unreal — no extra sidecar; users wire the texture into a
    // Niagara VAT module which references it by name. Documenting in
    // future slice 4 (Inspector) which sidecar to expect.

    result.ok = true;
    return result;
}
