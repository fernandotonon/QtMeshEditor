#include "ApplyAtlas.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMaterialManager.h>
#include <OgreMesh.h>
#include <OgrePass.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>
#include <OgreTechnique.h>
#include <OgreTextureUnitState.h>
#include <OgreVertexIndexData.h>

#include "RTShaderHelper.h"

namespace ApplyAtlas {

namespace {

// Read a tile from a JSON object, returning false if any required field is
// missing. Strict — a half-baked manifest is a bug, not a "best effort".
bool readTile(const QJsonObject& o, ManifestTile& out, QString& err)
{
    const QStringList required = {"source","x","y","w","h","u0","v0","u1","v1"};
    for (const QString& k : required) {
        if (!o.contains(k)) {
            err = QString("manifest tile missing required field '%1'").arg(k);
            return false;
        }
    }
    out.sourcePath = o.value("source").toString();
    out.x  = o.value("x").toInt();
    out.y  = o.value("y").toInt();
    out.w  = o.value("w").toInt();
    out.h  = o.value("h").toInt();
    out.u0 = static_cast<float>(o.value("u0").toDouble());
    out.v0 = static_cast<float>(o.value("v0").toDouble());
    out.u1 = static_cast<float>(o.value("u1").toDouble());
    out.v1 = static_cast<float>(o.value("v1").toDouble());
    return true;
}

// Find a tile whose source matches `texName` under the chosen MatchMode.
// Returns nullptr when no tile matches.
const ManifestTile* findTile(const Manifest& m, const QString& texName, MatchMode mode)
{
    if (texName.isEmpty()) return nullptr;
    const QString target = (mode == MatchMode::Basename)
        ? QFileInfo(texName).fileName()
        : texName;
    for (const ManifestTile& t : m.tiles) {
        const QString candidate = (mode == MatchMode::Basename)
            ? QFileInfo(t.sourcePath).fileName()
            : t.sourcePath;
        if (candidate.compare(target, Qt::CaseInsensitive) == 0)
            return &t;
    }
    return nullptr;
}

// Get the diffuse texture name a submesh's material currently binds. We
// look for the first TUS named "diffuse_map" or "albedo" (the canonical
// PBR slot from MaterialProcessor) and fall back to TUS index 0 — which
// matches how the channel packer / normal-map generator treat materials.
Ogre::String diffuseTexNameForSubEntity(const Ogre::SubEntity* sub)
{
    if (!sub) return {};
    const auto mat = sub->getMaterial();
    if (!mat || mat->getNumTechniques() == 0) return {};
    auto* tech = mat->getTechnique(0);
    if (!tech || tech->getNumPasses() == 0) return {};
    auto* pass = tech->getPass(0);
    // Preferred names first
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        const auto name = tus->getName();
        if (name == "diffuse_map" || name == "albedo") {
            if (!tus->getTextureName().empty())
                return tus->getTextureName();
        }
    }
    // Fallback: first TUS with a real texture
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        if (!tus->getTextureName().empty())
            return tus->getTextureName();
    }
    return {};
}

// Rewrite UV0 on a single submesh's vertex buffer. Returns the number of
// vertices touched. `outOfRange` is incremented for each UV that fell
// outside [0..1] and was clamped (or would have been skipped — caller
// decides).
int rewriteUv0(Ogre::SubMesh* sub, const Ogre::Mesh* mesh,
               const ManifestTile& tile, bool clamp, int& outOfRange)
{
    if (!sub) return 0;
    const bool shared = sub->useSharedVertices;
    Ogre::VertexData* vData = shared ? mesh->sharedVertexData : sub->vertexData;
    if (!vData) return 0;

    const auto* tcElem =
        vData->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);
    if (!tcElem) return 0;
    // Only handle FLOAT2 UVs. Atlas-remapping a packed half-float channel
    // is doable but unusual; falling through with a count of 0 means the
    // submesh is reported as "no UV change" rather than silently broken.
    if (tcElem->getType() != Ogre::VET_FLOAT2) return 0;

    auto vbuf = vData->vertexBufferBinding->getBuffer(tcElem->getSource());
    auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));

    const float du = tile.u1 - tile.u0;
    const float dv = tile.v1 - tile.v0;

    int touched = 0;
    const size_t stride = vbuf->getVertexSize();
    const size_t numV = vData->vertexCount;
    for (size_t v = 0; v < numV; ++v) {
        float* uv = nullptr;
        tcElem->baseVertexPointerToElement(base + v * stride, &uv);
        float u = uv[0];
        float vv = uv[1];
        if (u < 0.f || u > 1.f || vv < 0.f || vv > 1.f) {
            ++outOfRange;
            if (!clamp) continue;
            u  = std::clamp(u,  0.f, 1.f);
            vv = std::clamp(vv, 0.f, 1.f);
        }
        uv[0] = tile.u0 + u  * du;
        uv[1] = tile.v0 + vv * dv;
        ++touched;
    }

    vbuf->unlock();
    return touched;
}

// Point the submesh's first-pass diffuse TUS at the atlas texture. Same
// targeting rules as diffuseTexNameForSubEntity. Returns true on update.
bool retargetDiffuseTus(Ogre::SubEntity* sub, const Ogre::String& atlasTexName)
{
    if (!sub) return false;
    const auto mat = sub->getMaterial();
    if (!mat || mat->getNumTechniques() == 0) return false;
    auto* tech = mat->getTechnique(0);
    if (!tech || tech->getNumPasses() == 0) return false;
    auto* pass = tech->getPass(0);

    Ogre::TextureUnitState* target = nullptr;
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        const auto name = tus->getName();
        if (name == "diffuse_map" || name == "albedo") { target = tus; break; }
    }
    if (!target) {
        for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
            auto* tus = pass->getTextureUnitState(i);
            if (!tus->getTextureName().empty()) { target = tus; break; }
        }
    }
    if (!target) return false;
    target->setTextureName(atlasTexName);
    return true;
}

// Strip every TUS that is NOT the diffuse target — normal map, AO,
// emissive, etc. — because they all sample UV0 and we just remapped
// UV0 into the diffuse atlas's sub-rect. If the user wants those
// lighting channels back they need to atlas them too. Returns the
// count of TUSes removed. Also clears the `qtme.normal_map` UOB so
// the FBX exporter doesn't re-emit a NormalMap connection on save.
int stripNonDiffuseTexUnits(Ogre::SubEntity* sub)
{
    if (!sub) return 0;
    const auto mat = sub->getMaterial();
    if (!mat || mat->getNumTechniques() == 0) return 0;
    auto* tech = mat->getTechnique(0);
    if (!tech || tech->getNumPasses() == 0) return 0;
    auto* pass = tech->getPass(0);

    // Identify the diffuse TUS index using the same logic as
    // retargetDiffuseTus so we keep exactly the one we just retargeted.
    int diffuseIdx = -1;
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        const auto name = tus->getName();
        if (name == "diffuse_map" || name == "albedo") { diffuseIdx = i; break; }
    }
    if (diffuseIdx < 0) {
        for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
            auto* tus = pass->getTextureUnitState(i);
            if (!tus->getTextureName().empty()) { diffuseIdx = i; break; }
        }
    }
    if (diffuseIdx < 0) return 0;

    // Walk in reverse so removeTextureUnitState index math stays simple.
    int removed = 0;
    for (int i = static_cast<int>(pass->getNumTextureUnitStates()) - 1; i >= 0; --i) {
        if (i == diffuseIdx) continue;
        pass->removeTextureUnitState(static_cast<unsigned short>(i));
        ++removed;
    }
    // Clear the slice E2-related UOB hint (slice #507 stashed it for the
    // FBX exporter so the normal map round-trips). Atlasing diffuse-only
    // makes that hint stale — leaving it would re-emit a NormalMap
    // connection on FBX save and confuse downstream loaders.
    pass->getUserObjectBindings().eraseUserAny("qtme.normal_map");
    return removed;
}

} // namespace

int ApplyReport::rewrittenCount() const
{
    int n = 0;
    for (const auto& s : submeshes) if (s.uvsRewritten) ++n;
    return n;
}

QJsonObject ApplyReport::toJson() const
{
    QJsonObject root;
    root["ok"] = ok;
    if (!error.isEmpty()) root["error"] = error;
    QJsonArray arr;
    for (const auto& s : submeshes) {
        QJsonObject o;
        o["submeshIndex"]      = s.submeshIndex;
        o["materialName"]      = s.materialName;
        o["diffuseTextureName"]= s.diffuseTextureName;
        o["matchedTileSource"] = s.matchedTileSource;
        o["uvsRewritten"]      = s.uvsRewritten;
        o["materialUpdated"]   = s.materialUpdated;
        o["verticesTouched"]   = s.verticesTouched;
        o["outOfRangeUVs"]     = s.outOfRangeUVs;
        o["strippedExtraTextures"] = s.strippedExtraTextures;
        if (!s.note.isEmpty()) o["note"] = s.note;
        arr.append(o);
    }
    root["submeshes"] = arr;
    root["submeshCount"]   = submeshCount();
    root["rewrittenCount"] = rewrittenCount();
    return root;
}

ParseResult parseManifestJson(const QByteArray& json)
{
    ParseResult r;
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError) {
        r.error = QString("manifest JSON parse error: %1").arg(perr.errorString());
        return r;
    }
    if (!doc.isObject()) {
        r.error = "manifest JSON root must be an object";
        return r;
    }
    const QJsonObject root = doc.object();
    r.manifest.width   = root.value("width").toInt();
    r.manifest.height  = root.value("height").toInt();
    r.manifest.padding = root.value("padding").toInt();
    if (r.manifest.width <= 0 || r.manifest.height <= 0) {
        r.error = "manifest 'width' and 'height' must be positive integers";
        return r;
    }
    const QJsonValue tilesVal = root.value("tiles");
    if (!tilesVal.isArray()) {
        r.error = "manifest 'tiles' must be an array";
        return r;
    }
    const QJsonArray tilesArr = tilesVal.toArray();
    for (int i = 0; i < tilesArr.size(); ++i) {
        const QJsonValue v = tilesArr.at(i);
        if (!v.isObject()) {
            r.error = QString("manifest tile #%1 is not an object").arg(i);
            return r;
        }
        ManifestTile t;
        QString err;
        if (!readTile(v.toObject(), t, err)) {
            r.error = QString("tile #%1: %2").arg(i).arg(err);
            return r;
        }
        r.manifest.tiles.append(t);
    }
    r.ok = true;
    return r;
}

ApplyReport applyToEntity(Ogre::Entity* entity,
                          const Manifest& manifest,
                          const ApplyOptions& opts)
{
    ApplyReport report;
    if (!entity) {
        report.error = "entity is null";
        return report;
    }
    const Ogre::Mesh* mesh = entity->getMesh().get();
    if (!mesh) {
        report.error = "entity has no mesh";
        return report;
    }
    if (manifest.tiles.isEmpty()) {
        report.error = "manifest has no tiles";
        return report;
    }
    if (opts.atlasTextureName.isEmpty()) {
        report.error = "atlasTextureName is empty";
        return report;
    }

    const Ogre::String atlasTex = opts.atlasTextureName.toStdString();

    const unsigned int nSub = mesh->getNumSubMeshes();

    // Pass 1 — snapshot each submesh's pre-mutation diffuse texture name
    // and resolve its manifest tile. We MUST decide based on the original
    // texture name: two submeshes that share an Ogre::Material (Mixamo
    // exports do this a lot — one Skin_MAT shared by face, arms, etc.)
    // would otherwise see the atlas-rebound texture as their "current"
    // diffuse on the second visit and fall through with a "no match"
    // note. The atlas swap is one-shot per Material; UV rewrites are
    // per-submesh.
    struct Plan {
        Ogre::SubEntity* subEnt = nullptr;
        Ogre::SubMesh*   subMsh = nullptr;
        const ManifestTile* tile = nullptr;
        Ogre::String origDiffuse;
    };
    std::vector<Plan> plans(nSub);
    for (unsigned int si = 0; si < nSub; ++si) {
        SubmeshReport sr;
        sr.submeshIndex = static_cast<int>(si);

        Ogre::SubEntity* subEnt = entity->getSubEntity(si);
        Ogre::SubMesh*   subMsh = mesh->getSubMesh(si);
        if (!subEnt || !subMsh) {
            sr.note = "subentity / submesh missing";
            report.submeshes.append(sr);
            continue;
        }
        const auto mat = subEnt->getMaterial();
        sr.materialName = mat ? QString::fromStdString(mat->getName()) : QString();
        const Ogre::String diff = diffuseTexNameForSubEntity(subEnt);
        sr.diffuseTextureName = QString::fromStdString(diff);

        const ManifestTile* tile = findTile(manifest,
                                            sr.diffuseTextureName,
                                            opts.matchMode);
        if (!tile)
            sr.note = "no manifest tile matched this submesh's diffuse texture";
        else
            sr.matchedTileSource = tile->sourcePath;

        plans[si] = {subEnt, subMsh, tile, diff};
        report.submeshes.append(sr);
    }

    // Pass 2 — rewrite UVs per submesh, then retarget each unique Material's
    // diffuse TUS exactly once. Submeshes that share a Material will all
    // see the atlas binding after the swap; that's correct because they
    // also share the source texture (they came from the same input asset).
    std::set<std::string> retargetedMats;
    for (unsigned int si = 0; si < nSub; ++si) {
        const Plan& p = plans[si];
        if (!p.subEnt || !p.subMsh || !p.tile) continue;
        SubmeshReport& sr = report.submeshes[static_cast<int>(si)];

        int outOfRange = 0;
        const int touched = rewriteUv0(p.subMsh, mesh, *p.tile,
                                       opts.clampOutOfRangeUVs, outOfRange);
        sr.verticesTouched = touched;
        sr.outOfRangeUVs   = outOfRange;
        sr.uvsRewritten    = touched > 0;
        if (outOfRange > 0 && !opts.clampOutOfRangeUVs)
            sr.note = QString("%1 UV(s) outside [0..1] left unchanged "
                              "(clampOutOfRangeUVs=false)").arg(outOfRange);

        const auto mat = p.subEnt->getMaterial();
        const std::string matName = mat ? mat->getName() : std::string();
        if (!matName.empty() && retargetedMats.insert(matName).second) {
            sr.materialUpdated = retargetDiffuseTus(p.subEnt, atlasTex);
            // Optionally strip every non-diffuse TUS (normal, AO, emissive…)
            // BEFORE rewiring RTSS. Those textures sample UV0, which we
            // just remapped into a diffuse-only sub-rect — leaving them
            // bound makes lighting align to the wrong region of every
            // auxiliary texture. Users who pre-atlased the auxiliary
            // channels to match can set opts.stripNonDiffuseTextures=false.
            if (opts.stripNonDiffuseTextures)
                sr.strippedExtraTextures = stripNonDiffuseTexUnits(p.subEnt);
            // Rewire FFP+RTSS so lighting recomputes against the freshly-
            // bound atlas. Without this the shader system caches the old
            // diffuse binding and lights look subtly off. Mirrors what
            // MaterialProcessor does at end of import.
            if (mat) {
                try {
                    RTShaderHelper::wirePbrSlotsForFFP(mat.get());
                    mat->compile();
                    mat->reload();
                } catch (...) {
                    // RTSS rewiring is best-effort. A failure here is a
                    // lighting regression, not a correctness one; the
                    // UV remap + texture swap above is what callers
                    // actually depend on.
                }
            }
        } else {
            sr.materialUpdated = true;  // shared material already swapped
        }
    }

    report.ok = true;
    return report;
}

} // namespace ApplyAtlas
