#include "MeshGenBuilder.h"

#include "Manager.h"
#include "AIAssistManager.h"   // #404 PBR map synthesis (normal + roughness)
#include "RTShaderHelper.h"    // canonical-slot FFP wiring + RTSS normal map

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreLogManager.h>
#include <OgreMaterialManager.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgrePass.h>
#include <OgreTechnique.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

// Accumulate face normals into per-vertex normals and normalize. Marching-cubes
// output is welded but carries no normals, so we derive them here (Slice C's
// requirement). Degenerate faces contribute nothing.
std::vector<float> computeNormals(const std::vector<float>& pos,
                                  const std::vector<uint32_t>& idx)
{
    std::vector<float> nrm(pos.size(), 0.0f);
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const uint32_t a = idx[t + 0], b = idx[t + 1], c = idx[t + 2];
        const float* pa = &pos[static_cast<size_t>(a) * 3];
        const float* pb = &pos[static_cast<size_t>(b) * 3];
        const float* pc = &pos[static_cast<size_t>(c) * 3];
        const float e1[3] = {pb[0]-pa[0], pb[1]-pa[1], pb[2]-pa[2]};
        const float e2[3] = {pc[0]-pa[0], pc[1]-pa[1], pc[2]-pa[2]};
        const float fn[3] = {
            e1[1]*e2[2] - e1[2]*e2[1],
            e1[2]*e2[0] - e1[0]*e2[2],
            e1[0]*e2[1] - e1[1]*e2[0],
        };
        for (uint32_t v : {a, b, c}) {
            nrm[static_cast<size_t>(v)*3 + 0] += fn[0];
            nrm[static_cast<size_t>(v)*3 + 1] += fn[1];
            nrm[static_cast<size_t>(v)*3 + 2] += fn[2];
        }
    }
    for (size_t i = 0; i < nrm.size(); i += 3) {
        float len = std::sqrt(nrm[i]*nrm[i] + nrm[i+1]*nrm[i+1] + nrm[i+2]*nrm[i+2]);
        if (len < 1e-12f) { nrm[i+1] = 1.0f; len = 1.0f; }   // fallback +Y
        nrm[i]   /= len; nrm[i+1] /= len; nrm[i+2] /= len;
    }
    return nrm;
}

} // namespace

namespace MeshGenBuilder {

Ogre::Mesh* buildMesh(const MeshGenPredictor::Result& result, const QString& meshName,
                      const QString& texturePngPath)
{
    if (result.vertexCount <= 0 || result.triangleCount <= 0
        || result.positions.size() != static_cast<size_t>(result.vertexCount) * 3)
        return nullptr;

    // Validate the index data before it's used to index into positions (in
    // computeNormals and the buffer fill): the count must be 3/triangle and every
    // index in-range. A malformed predictor result would otherwise read OOB.
    if (result.indices.size() != static_cast<size_t>(result.triangleCount) * 3)
        return nullptr;
    for (uint32_t i : result.indices)
        if (i >= static_cast<uint32_t>(result.vertexCount))
            return nullptr;

    // Baked-texture path: UV0 + a diffuse texture (preferred). Vertex colour is
    // the fallback when no bake ran.
    const bool hasUv =
        result.uvs.size() == static_cast<size_t>(result.vertexCount) * 2
        && !texturePngPath.isEmpty();
    const bool hasColor = !hasUv
        && result.colors.size() == static_cast<size_t>(result.vertexCount) * 3;
    const std::vector<float> normals = computeNormals(result.positions, result.indices);

    auto& mm = Ogre::MeshManager::getSingleton();
    const std::string name = meshName.toStdString();
    if (mm.resourceExists(name))
        mm.remove(name);
    Ogre::MeshPtr mesh = mm.createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    Ogre::SubMesh* sub = mesh->createSubMesh();
    sub->useSharedVertices = true;

    auto* vd = new Ogre::VertexData();
    mesh->sharedVertexData = vd;
    vd->vertexCount = static_cast<size_t>(result.vertexCount);
    auto* decl = vd->vertexDeclaration;

    size_t offset = 0;
    offset += decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION).getSize();
    offset += decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL).getSize();
    if (hasUv)
        offset += decl->addElement(0, offset, Ogre::VET_FLOAT2,
                                   Ogre::VES_TEXTURE_COORDINATES, 0).getSize();
    else if (hasColor)
        offset += decl->addElement(0, offset, Ogre::VET_COLOUR, Ogre::VES_DIFFUSE).getSize();

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), vd->vertexCount,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);

    // TripoSR's reconstruction frame lands the model lying on its back AND facing
    // 90° off relative to QtMeshEditor's +Y-up convention. Bake the fixed
    // orientation into the geometry (positions AND normals) so it stands upright
    // and faces forward — matching the source image. Baking into the vertex data
    // (rather than a node transform) keeps it correct through glTF export in any
    // viewer.
    //   step 1: -90° about X to stand it up:  (x, y, z) -> (x, z, -y)
    //   step 2: +90° about Y to face forward: (x, y, z) -> (z, y, -x)
    // Composed: (x, y, z) -> (-y, z, -x).
    // TripoSG results are already +Y-up (Result::bakeTripoSROrientation is
    // false) — baking the TripoSR frame onto them lays the model on its back.
    // BUT TripoSG comes out facing AWAY (its front is -Z here), so rotate it
    // 180° about Y so the reconstructed front faces the camera (+Z):
    //   180°Y: (x, y, z) -> (-x, y, -z).
    const bool bakeOrientation = result.bakeTripoSROrientation;
    const bool flipY180 = !bakeOrientation;   // TripoSG path only
    auto orient = [bakeOrientation, flipY180](float& x, float& y, float& z) {
        if (bakeOrientation) {
            const float nx = -y, ny = z, nz = -x;
            x = nx; y = ny; z = nz;
        } else if (flipY180) {
            x = -x; z = -z;   // 180° about Y — face the front forward
        }
    };

    Ogre::Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    {
        auto* p = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        auto* root = Ogre::Root::getSingletonPtr();
        for (int i = 0; i < result.vertexCount; ++i) {
            float* f = reinterpret_cast<float*>(p);
            float x = result.positions[3*i+0];
            float y = result.positions[3*i+1];
            float z = result.positions[3*i+2];
            orient(x, y, z);
            float nx = normals[3*i+0], ny = normals[3*i+1], nz = normals[3*i+2];
            orient(nx, ny, nz);
            f[0] = x; f[1] = y; f[2] = z;
            f[3] = nx; f[4] = ny; f[5] = nz;
            p += 6 * sizeof(float);
            if (hasUv) {
                float* uv = reinterpret_cast<float*>(p);
                uv[0] = result.uvs[2*i+0];
                uv[1] = result.uvs[2*i+1];
                p += 2 * sizeof(float);
            } else if (hasColor) {
                Ogre::ColourValue cv(result.colors[3*i+0], result.colors[3*i+1],
                                     result.colors[3*i+2], 1.0f);
                Ogre::RGBA packed;
                if (root) root->convertColourValue(cv, &packed);
                else      packed = cv.getAsARGB();
                *reinterpret_cast<Ogre::RGBA*>(p) = packed;
                p += sizeof(Ogre::RGBA);
            }
            mn.makeFloor(Ogre::Vector3(x, y, z));
            mx.makeCeil(Ogre::Vector3(x, y, z));
        }
        vbuf->unlock();
    }
    vd->vertexBufferBinding->setBinding(0, vbuf);

    // Index buffer: 16-bit when it fits, else 32-bit.
    const size_t idxCount = result.indices.size();
    const bool use32 = result.vertexCount > 65535;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        use32 ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT,
        idxCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    if (use32) {
        ibuf->writeData(0, idxCount * sizeof(uint32_t), result.indices.data(), true);
    } else {
        std::vector<uint16_t> idx16(idxCount);
        for (size_t i = 0; i < idxCount; ++i)
            idx16[i] = static_cast<uint16_t>(result.indices[i]);
        ibuf->writeData(0, idxCount * sizeof(uint16_t), idx16.data(), true);
    }
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount  = idxCount;
    sub->indexData->indexStart  = 0;

    // Baked-texture path: a per-mesh lit material with the baked diffuse bound
    // as the (named) diffuse_map slot, so the RTSS lighting path and PBR-aware
    // tooling both resolve it, and the exporters carry the reference.
    if (hasUv) {
        auto& matMgr = Ogre::MaterialManager::getSingleton();
        const std::string matName = meshName.toStdString() + "_mat";
        if (matMgr.resourceExists(matName))
            matMgr.remove(matName);
        Ogre::MaterialPtr tm = matMgr.create(
            matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        auto* pass = tm->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(true);
        pass->setDiffuse(Ogre::ColourValue::White);
        pass->setAmbient(Ogre::ColourValue(0.9f, 0.9f, 0.9f));
        pass->setCullingMode(Ogre::CULL_CLOCKWISE);
        auto* tus = pass->createTextureUnitState(
            QFileInfo(texturePngPath).fileName().toStdString());
        tus->setName("diffuse_map");
        tm->compile();
        sub->setMaterialName(matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }

    // When the mesh carries per-vertex color (TripoSR's predicted vertex color),
    // assign a lit material that TRACKS the diffuse channel from VES_DIFFUSE —
    // otherwise the default white material ignores the colors and the mesh renders
    // flat-white in the viewport. A shared named material (created once) keeps
    // lighting on so the surface still shades; the vertex color modulates it.
    if (hasColor) {
        auto& matMgr = Ogre::MaterialManager::getSingleton();
        const char* kMat = "MeshGen/VertexColor";
        Ogre::MaterialPtr vc = matMgr.getByName(kMat);
        if (!vc) {
            vc = matMgr.create(kMat, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            auto* pass = vc->getTechnique(0)->getPass(0);
            pass->setLightingEnabled(true);
            // Track diffuse (and ambient, so unlit-ish areas still show color)
            // from the per-vertex VES_DIFFUSE channel.
            pass->setVertexColourTracking(
                Ogre::TVC_DIFFUSE | Ogre::TVC_AMBIENT);
            pass->setCullingMode(Ogre::CULL_CLOCKWISE);
            vc->compile();
        }
        sub->setMaterialName(kMat, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }

    // Geometry-only result (TripoSG, or TripoSR with every colour stage off):
    // without a material the default flat-white one hides all surface relief.
    // Assign a shared neutral LIT clay material so the shape actually shades.
    if (!hasUv && !hasColor) {
        auto& matMgr = Ogre::MaterialManager::getSingleton();
        const char* kMat = "MeshGen/NeutralClay";
        Ogre::MaterialPtr clay = matMgr.getByName(kMat);
        if (!clay) {
            clay = matMgr.create(kMat, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            auto* pass = clay->getTechnique(0)->getPass(0);
            pass->setLightingEnabled(true);
            pass->setDiffuse(Ogre::ColourValue(0.72f, 0.68f, 0.62f));   // warm clay
            pass->setAmbient(Ogre::ColourValue(0.35f, 0.33f, 0.30f));
            pass->setSpecular(Ogre::ColourValue(0.15f, 0.15f, 0.15f));
            pass->setShininess(24.0f);
            pass->setCullingMode(Ogre::CULL_CLOCKWISE);
            clay->compile();
        }
        sub->setMaterialName(kMat, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }

    Ogre::AxisAlignedBox aabb(mn, mx);
    mesh->_setBounds(aabb);
    mesh->_setBoundingSphereRadius(0.5f * (mx - mn).length());
    mesh->load();
    return mesh.get();
}

Ogre::SceneNode* buildSceneNode(const MeshGenPredictor::Result& result,
                                const QString& baseName,
                                const BuildOptions& opts)
{
    // Make the mesh + node names UNIQUE per call so a second generation doesn't
    // clobber the first (or fail because the mesh/node name already exists). All
    // callers pass the same base ("qtmesh_gen3d"); disambiguate with a counter
    // PLUS a timestamp: the counter alone is per-PROCESS, so a second CLI run
    // (or a new GUI session) into the same output directory re-used the same
    // "…_1_diffuse.png" file name — OVERWRITING the previous generation's baked
    // texture and leaving the older mesh's UVs pointing into the wrong atlas
    // (a scrambled chart-patchwork on the first mesh). Ogre's TextureManager
    // also caches by name, so name collisions could serve stale pixels even
    // when the file was rewritten. The epoch-ms token makes the mesh, node,
    // material, and every texture sidecar name globally unique (same idiom as
    // MCP create_primitive's auto names).
    static int s_counter = 0;
    const QString unique = baseName + QStringLiteral("_%1_%2")
        .arg(++s_counter)
        .arg(QDateTime::currentMSecsSinceEpoch());

    // Baked-texture path: persist the QImage as a PNG (viewport material +
    // exporters resolve it from a registered resource location).
    QString texPath;
    QString normalPath, roughnessPath;   // optional #404 PBR stage outputs
    if (!result.texture.isNull()
        && result.uvs.size() == static_cast<size_t>(result.vertexCount) * 2) {
        QString dir = opts.textureDir;
        if (dir.isEmpty())
            dir = QDir(QStandardPaths::writableLocation(
                           QStandardPaths::AppDataLocation))
                      .filePath(QStringLiteral("generated_textures"));
        QDir().mkpath(dir);
        const QString candidate =
            QDir(dir).filePath(unique + QStringLiteral("_diffuse.png"));
        if (result.texture.save(candidate, "PNG")) {
            texPath = candidate;

            // Optional PBR stage (#404): synthesize normal + roughness from the
            // baked diffuse BEFORE the resource location is (re)indexed so the
            // new PNGs land in the index below. Height is skipped — nothing in
            // this material path consumes it. Fails soft: a missing model /
            // non-ONNX build just leaves the maps empty (diffuse-only result).
            if (opts.generatePbrMaps) {
                PbrMapSynth::Options po;
                po.generateHeight = false;
                const PbrMapSynthResult pr =
                    AIAssistManager::instance()->synthesizePbrMaps(texPath, po);
                if (pr.ok) {
                    normalPath    = pr.normalPath;
                    roughnessPath = pr.roughnessPath;
                } else {
                    Ogre::LogManager::getSingleton().logWarning(
                        ("MeshGenBuilder: PBR map synthesis failed ("
                         + pr.error + ") — continuing with diffuse only.")
                            .toStdString());
                }
            }

            // Register the directory so Ogre's resource system (and the
            // exporters' resource walk) can find the files. When the location
            // is ALREADY registered (second+ generation into the same dir),
            // ADD IT AGAIN — a re-add re-lists the directory into the group's
            // file index (picking up the fresh PNGs) while ArchiveManager
            // reuses the same Archive instance. Do NOT removeResourceLocation
            // to force the refresh: the same directory is commonly registered
            // in MULTIPLE groups (the mesh-import path uses a dir-named group
            // + DEFAULT), Ogre shares one Archive per path across groups, and
            // removal DESTROYS it — every other group's index then dangles and
            // the next openResource dies with EXC_BAD_ACCESS inside
            // ResourceGroupManager::openResourceImpl (reproduced under lldb:
            // generate → export .mesh → reload in the same session).
            try {
                auto& rgm = Ogre::ResourceGroupManager::getSingleton();
                const std::string loc = QDir(dir).absolutePath().toStdString();
                const std::string grp =
                    Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;
                rgm.addResourceLocation(loc, "FileSystem", grp);
                rgm.initialiseResourceGroup(grp);
            } catch (const Ogre::Exception& e) {
                Ogre::LogManager::getSingleton().logWarning(
                    "MeshGenBuilder: could not (re)register texture location: "
                    + Ogre::String(e.what()));
            }
        } else {
            // Losing the texture silently would produce a flat-white mesh with
            // no explanation — the bake already replaced the vertex colours,
            // so there is nothing to fall back to. Log loudly.
            Ogre::LogManager::getSingleton().logWarning(
                ("MeshGenBuilder: failed to save the baked texture to '"
                 + candidate + "' — the generated mesh will render untextured.")
                    .toStdString());
        }
    }

    Ogre::Mesh* mesh = buildMesh(result, unique + QStringLiteral("_mesh"), texPath);
    if (!mesh) return nullptr;

    // Bind the synthesized PBR maps into the material buildMesh just created —
    // the same recipe as the Material Editor's "Generate PBR maps from diffuse"
    // button: canonical named slots + FFP wiring + the RTSS normal-map
    // sub-render-state (without applyNormalMap the bind is invisible in the
    // viewport), then recompile.
    if (!normalPath.isEmpty() || !roughnessPath.isEmpty()) {
        const std::string matName =
            (unique + QStringLiteral("_mesh")).toStdString() + "_mat";
        Ogre::MaterialPtr mat =
            Ogre::MaterialManager::getSingleton().getByName(matName);
        if (mat) {
            auto bindSlot = [&](const char* slot, const QString& path) {
                if (path.isEmpty()) return;
                for (auto* tech : mat->getTechniques()) {
                    for (unsigned short p = 0; p < tech->getNumPasses(); ++p) {
                        Ogre::Pass* pass = tech->getPass(p);
                        Ogre::TextureUnitState* tus = nullptr;
                        for (unsigned short i = 0;
                             i < pass->getNumTextureUnitStates(); ++i)
                            if (pass->getTextureUnitState(i)->getName() == slot) {
                                tus = pass->getTextureUnitState(i); break;
                            }
                        if (!tus) {
                            tus = pass->createTextureUnitState();
                            tus->setName(slot);
                        }
                        tus->setTextureName(
                            QFileInfo(path).fileName().toStdString());
                    }
                }
            };
            bindSlot("normal_map", normalPath);
            bindSlot("roughness",  roughnessPath);
            RTShaderHelper::wirePbrSlotsForFFP(mat.get());
            if (!normalPath.isEmpty())
                RTShaderHelper::applyNormalMap(
                    mat, QFileInfo(normalPath).fileName().toStdString());
            mat->compile();
        }
    }

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    Ogre::SceneNode* node = mgr->addSceneNode(unique);
    if (!node) return nullptr;
    Ogre::MeshPtr ptr = Ogre::MeshManager::getSingleton().getByName(mesh->getName());
    mgr->createEntity(node, ptr);
    // Both image-to-3D backends reconstruct into a unit-ish box that lands the
    // model quite small in the editor scene; scale x2 so it arrives at a
    // workable size (matches TripoSR + TripoSG). Applied on the node, so it's
    // baked into any export via the node transform.
    node->setScale(2.0f, 2.0f, 2.0f);
    return node;
}

} // namespace MeshGenBuilder
