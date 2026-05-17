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

#include <OgreAnimationState.h>
#include <OgreEntity.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreMesh.h>
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

QString VATBaker::buildSidecarJson(const BakeResult& result, const Options& opts)
{
    QJsonObject bounds;
    QJsonObject lo, hi;
    lo["x"] = result.minBound.x; lo["y"] = result.minBound.y; lo["z"] = result.minBound.z;
    hi["x"] = result.maxBound.x; hi["y"] = result.maxBound.y; hi["z"] = result.maxBound.z;
    bounds["min"] = lo;
    bounds["max"] = hi;

    QJsonObject root;
    root["version"]     = 1;
    root["target"]      = "agnostic";
    root["encoding"]    = "rgba8";
    root["frameCount"]  = result.frameCount;
    root["vertexCount"] = result.vertexCount;
    root["fps"]         = opts.fps;
    root["animation"]   = opts.animationName;
    root["bounds"]      = bounds;
    root["posTexture"]  = QFileInfo(result.posTexPath).fileName();

    QJsonDocument doc(root);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

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
    Ogre::Vector3 lo(std::numeric_limits<float>::infinity());
    Ogre::Vector3 hi(-std::numeric_limits<float>::infinity());
    flat.reserve(static_cast<size_t>(frameCount) * 1024);

    int vertexCount = -1;
    for (int f = 0; f < frameCount; ++f) {
        const double t = (frameCount == 1) ? t0
                       : t0 + span * (static_cast<double>(f)
                                       / static_cast<double>(frameCount - 1));
        state->setTimePosition(static_cast<Ogre::Real>(t));

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

        for (size_t i = before; i < flat.size(); ++i) {
            const auto& p = flat[i];
            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
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

    // Encode + write the position texture.
    auto rgba = encodeRGBA8(flat, frameCount, vertexCount, lo, hi);
    if (rgba.empty()) {
        result.error = QStringLiteral("encodeRGBA8 produced empty buffer");
        return result;
    }
    QImage img(vertexCount, frameCount, QImage::Format_RGBA8888);
    // Row-by-row copy. QImage's stride may differ from the packed
    // `vertexCount * 4` we generate, so copy line-by-line.
    for (int y = 0; y < frameCount; ++y) {
        const unsigned char* src = rgba.data() + static_cast<size_t>(y)
                                                  * static_cast<size_t>(vertexCount) * 4u;
        std::memcpy(img.scanLine(y), src, static_cast<size_t>(vertexCount) * 4u);
    }
    if (!img.save(result.posTexPath, "PNG")) {
        result.error = QStringLiteral("failed to write position texture: %1")
                           .arg(result.posTexPath);
        return result;
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

    result.ok = true;
    return result;
}
