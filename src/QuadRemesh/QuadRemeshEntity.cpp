#include "QuadRemeshEntity.h"

#include "AppStorage.h"
#include "EditableMesh.h"
#include "EmbeddedTextureCache.h"
#include "PartOpsMesh.h"
#include "RTShaderHelper.h"

#include <OgreEntity.h>
#include <OgreImage.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgrePixelFormat.h>
#include <OgreResourceGroupManager.h>
#include <OgreSkeleton.h>
#include <OgreSubEntity.h>
#include <OgreTechnique.h>
#include <OgreTextureUnitState.h>
#include <OgreRTShaderSystem.h>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>

#include <cstring>

namespace QuadRemeshEntity {

namespace {

// Canonical slot key for a texture unit: the diffuse aliases collapse onto
// "diffuse_map" (and an unnamed FIRST unit is the diffuse by convention), so
// materials that name the same channel differently bake into one map.
QString slotKey(const Ogre::TextureUnitState* tus, unsigned short index)
{
    const QString name = QString::fromStdString(tus->getName());
    const QString lower = name.toLower();
    bool numeric = false;
    name.toInt(&numeric);
    if (name.isEmpty() || numeric)
        return index == 0 ? QStringLiteral("diffuse_map") : QStringLiteral("tus%1").arg(index);
    if (lower == QLatin1String("diffuse") || lower == QLatin1String("basecolor")
        || lower == QLatin1String("base_color") || lower == QLatin1String("diffuse_map"))
        return QStringLiteral("diffuse_map");
    return name;
}

bool bakeableUnit(const Ogre::TextureUnitState* tus)
{
    if (!tus || tus->getTextureName().empty()) return false;
    if (tus->getTextureType() != Ogre::TEX_TYPE_2D) return false;    // env/cube maps
    if (tus->getTextureCoordSet() != 0) return false;                 // lightmaps etc.
    // The live texture-paint session's transient texture is not the asset.
    return tus->getTextureName().rfind("QMEPaint_", 0) != 0;
}

QImage loadTexture(const std::string& texName)
{
    // Embedded first (FBX textures are often inline and not on disk).
    const std::vector<uint8_t> embedded = EmbeddedTextureCache::retrieve(texName);
    if (!embedded.empty()) {
        QImage img;
        if (img.loadFromData(embedded.data(), int(embedded.size())))
            return img.convertToFormat(QImage::Format_ARGB32);
    }
    QImage direct;
    if (direct.load(QString::fromStdString(texName)))
        return direct.convertToFormat(QImage::Format_ARGB32);
    try {
        Ogre::Image oimg;
        oimg.load(texName, Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
        const int w = int(oimg.getWidth()), h = int(oimg.getHeight());
        if (w > 0 && h > 0) {
            QImage img(w, h, QImage::Format_RGBA8888);
            Ogre::PixelBox dst(w, h, 1, Ogre::PF_BYTE_RGBA, img.bits());
            Ogre::PixelUtil::bulkPixelConversion(oimg.getPixelBox(), dst);
            return img.convertToFormat(QImage::Format_ARGB32);
        }
    } catch (...) {
    }
    return {};
}

Ogre::Technique* firstTechnique(const Ogre::MaterialPtr& mat)
{
    if (!mat) return nullptr;
    Ogre::Technique* t = mat->getBestTechnique();
    if (!t && mat->getNumTechniques() > 0) t = mat->getTechnique(0);
    return t;
}

} // namespace

QuadRemesh::VoxelMode voxelModeFromString(const QString& s, bool* ok)
{
    const QString l = s.trimmed().toLower();
    if (ok) *ok = true;
    if (l.isEmpty() || l == QLatin1String("auto")) return QuadRemesh::VoxelMode::Auto;
    if (l == QLatin1String("on") || l == QLatin1String("always") || l == QLatin1String("true"))
        return QuadRemesh::VoxelMode::Always;
    if (l == QLatin1String("off") || l == QLatin1String("never") || l == QLatin1String("false"))
        return QuadRemesh::VoxelMode::Never;
    if (ok) *ok = false;
    return QuadRemesh::VoxelMode::Auto;
}

bool readSource(Ogre::Entity* entity, QuadRemesh::SourceMesh& out, SourceInfo& info, QString* error)
{
    out = QuadRemesh::SourceMesh();
    info = SourceInfo();
    std::vector<EditableSubMesh> subs;
    if (!entity || !PartOpsMesh::readSubMeshes(entity, subs)) {
        if (error) *error = QStringLiteral("could not read mesh geometry");
        return false;
    }
    const Ogre::MeshPtr mesh = entity->getMesh();
    info.meshName = mesh ? QString::fromStdString(mesh->getName()) : QString();
    if (mesh && mesh->hasSkeleton())
        info.skeletonName = QString::fromStdString(mesh->getSkeletonName());

    bool anyUv = false, anyBones = false;
    for (const EditableSubMesh& s : subs)
        for (const EditableVertex& v : s.vertices) {
            anyUv = anyUv || v.hasUV;
            anyBones = anyBones || !v.boneAssignments.empty();
        }
    anyBones = anyBones && !info.skeletonName.isEmpty();

    // One QImage per texture NAME: slots that alias the same file (diffuse_map
    // + albedo) then share a cacheKey, and the core bakes them once.
    QHash<QString, QImage> loaded;
    for (size_t si = 0; si < subs.size(); ++si) {
        const EditableSubMesh& s = subs[si];
        const uint32_t base = uint32_t(out.positions.size() / 3);
        for (const EditableVertex& v : s.vertices) {
            out.positions.insert(out.positions.end(), {float(v.position.x), float(v.position.y), float(v.position.z)});
            out.normals.insert(out.normals.end(), {float(v.normal.x), float(v.normal.y), float(v.normal.z)});
            if (anyUv) out.uvs.insert(out.uvs.end(), {float(v.uv.x), float(v.uv.y)});
            if (anyBones) {
                std::vector<QuadRemesh::BoneWeight> bw;
                for (const EditableBoneAssignment& ba : v.boneAssignments)
                    bw.push_back({uint16_t(ba.boneIndex), ba.weight});
                out.bones.push_back(std::move(bw));
            }
        }
        for (const EditableTriangle& t : s.triangles) {
            out.indices.insert(out.indices.end(), {base + t.indices[0], base + t.indices[1], base + t.indices[2]});
            out.triangleMaterial.push_back(int(si));
        }

        // This submesh's effective material (per-SubEntity override first).
        QuadRemesh::SourceMaterial sm;
        Ogre::MaterialPtr mat;
        if (si < entity->getNumSubEntities())
            mat = entity->getSubEntity(unsigned(si))->getMaterial();
        if (!mat && !s.materialName.empty())
            mat = Ogre::MaterialManager::getSingleton().getByName(s.materialName);
        if (si == 0 && mat) info.templateMaterial = mat->getName();
        if (Ogre::Technique* tech = firstTechnique(mat)) {
            if (tech->getNumPasses() > 0) {
                Ogre::Pass* pass = tech->getPass(0);
                for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
                    const Ogre::TextureUnitState* tus = pass->getTextureUnitState(i);
                    if (!bakeableUnit(tus)) continue;
                    const QString key = slotKey(tus, i);
                    if (sm.maps.contains(key)) continue;
                    const QString texName = QString::fromStdString(tus->getTextureName());
                    if (!loaded.contains(texName))
                        loaded.insert(texName, loadTexture(tus->getTextureName()));
                    const QImage img = loaded.value(texName);
                    if (!img.isNull()) sm.maps.insert(key, img);
                }
            }
        }
        out.materials.push_back(std::move(sm));
    }
    if (out.indices.empty()) {
        if (error) *error = QStringLiteral("mesh has no triangles");
        return false;
    }
    return true;
}

Built buildMesh(const QuadRemesh::Result& r, const SourceInfo& info, const std::string& baseName,
                const QString& textureDir)
{
    Built b;
    if (!r.ok || r.positions.empty() || r.faces.empty()) {
        b.error = QStringLiteral("nothing to build");
        return b;
    }
    static int s_counter = 0;
    const QString unique = QString::fromStdString(baseName)
                           + QStringLiteral("_qr%1_%2").arg(++s_counter).arg(QDateTime::currentMSecsSinceEpoch());

    // Baked textures → PNGs in a registered resource location.
    QHash<QString, QString> slotFile;   // slot → file name
    if (!r.maps.isEmpty()) {
        QString dir = textureDir;
        if (dir.isEmpty())
            dir = QDir(AppStorage::persistentRoot()).filePath(QStringLiteral("generated_textures/remesh"));
        QDir().mkpath(dir);
        QHash<qint64, QString> written;   // shared images (aliased slots) → one file
        for (auto it = r.maps.cbegin(); it != r.maps.cend(); ++it) {
            const qint64 key = it.value().cacheKey();
            if (written.contains(key)) {
                slotFile.insert(it.key(), written.value(key));
                continue;
            }
            const QString file = unique + QLatin1Char('_') + it.key() + QStringLiteral(".png");
            const QString path = QDir(dir).filePath(file);
            if (!it.value().save(path, "PNG")) {
                b.error = QStringLiteral("could not write %1").arg(path);
                return b;
            }
            written.insert(key, file);
            slotFile.insert(it.key(), file);
            b.texturePaths << path;
        }
        try {
            // Re-adding an existing location re-indexes it (see MeshGenBuilder
            // for why a remove-then-add is NOT safe).
            auto& rgm = Ogre::ResourceGroupManager::getSingleton();
            const std::string grp = Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;
            rgm.addResourceLocation(QDir(dir).absolutePath().toStdString(), "FileSystem", grp);
            rgm.initialiseResourceGroup(grp);
        } catch (const Ogre::Exception& e) {
            b.error = QStringLiteral("could not register texture location: %1").arg(QString::fromUtf8(e.what()));
            return b;
        }
    }

    // Material: clone of the source's first material, 2D UV0 slots rebound.
    const std::string matName = unique.toStdString() + "_mat";
    Ogre::MaterialPtr mat;
    Ogre::MaterialPtr tmpl = info.templateMaterial.empty()
                                 ? Ogre::MaterialPtr()
                                 : Ogre::MaterialManager::getSingleton().getByName(info.templateMaterial);
    if (tmpl)
        mat = tmpl->clone(matName);
    else
        mat = Ogre::MaterialManager::getSingleton().create(
            matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    // A clone() copies the source's RTSS-generated technique (scheme
    // ShaderGeneratorDefaultScheme) verbatim — shaders built for the SOURCE
    // texture-unit layout. The ShaderGenerator does not track the copy, and
    // the scheme resolver re-validates an existing RTSS technique rather than
    // regenerating it, so after the slots below are removed/added the mesh
    // rendered with stale samplers (dark/black). Drop every RTSS technique so
    // it is regenerated from the edited FFP technique on the next frame (the
    // MaterialEditorQML clone recipe).
    for (unsigned short ti = mat->getNumTechniques(); ti-- > 0;)
        if (mat->getTechnique(ti)->getSchemeName()
            == Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME)
            mat->removeTechnique(ti);
    if (mat->getNumTechniques() == 0) mat->createTechnique();
    for (Ogre::Technique* tech : mat->getTechniques()) {
        if (tech->getNumPasses() == 0) tech->createPass();
        Ogre::Pass* pass = tech->getPass(0);
        QSet<QString> bound;
        for (int i = int(pass->getNumTextureUnitStates()) - 1; i >= 0; --i) {
            Ogre::TextureUnitState* tus = pass->getTextureUnitState(static_cast<unsigned short>(i));
            if (tus->getTextureType() != Ogre::TEX_TYPE_2D || tus->getTextureCoordSet() != 0)
                continue;
            const QString key = slotKey(tus, static_cast<unsigned short>(i));
            if (slotFile.contains(key)) {
                tus->setTextureName(slotFile.value(key).toStdString());
                bound.insert(key);
            } else if (!tus->getTextureName().empty()) {
                // Its UVs referred to the old layout; nothing baked for it.
                pass->removeTextureUnitState(static_cast<unsigned short>(i));
            }
        }
        // Slots baked but absent from the template (another submesh's map, or
        // the generated detail normal) get a named unit of their own.
        for (auto it = slotFile.cbegin(); it != slotFile.cend(); ++it) {
            if (bound.contains(it.key()) || it.key().startsWith(QLatin1String("tus"))) continue;
            Ogre::TextureUnitState* tus = pass->createTextureUnitState(it.value().toStdString());
            tus->setName(it.key().toStdString());
        }
    }
    RTShaderHelper::wirePbrSlotsForFFP(mat.get());
    // A PBR-tagged source keeps Cook-Torrance (a no-op for plain materials).
    RTShaderHelper::applyPbrIfTagged(mat);
    mat->compile();
    b.materialName = matName;

    // Geometry: one submesh, quads on the n-gon binding.
    EditableSubMesh sub;
    sub.materialName = matName;
    const size_t nv = r.positions.size() / 3;
    const bool hasUv = r.uvs.size() == nv * 2;
    const bool hasBones = r.bones.size() == nv && !info.skeletonName.isEmpty();
    sub.vertices.resize(nv);
    for (size_t v = 0; v < nv; ++v) {
        EditableVertex& ev = sub.vertices[v];
        ev.position = Ogre::Vector3(r.positions[3 * v], r.positions[3 * v + 1], r.positions[3 * v + 2]);
        if (r.normals.size() == nv * 3) {
            ev.normal = Ogre::Vector3(r.normals[3 * v], r.normals[3 * v + 1], r.normals[3 * v + 2]);
            ev.hasNormal = true;
        }
        if (hasUv) {
            ev.uv = Ogre::Vector2(r.uvs[2 * v], r.uvs[2 * v + 1]);
            ev.hasUV = true;
        }
        if (hasBones)
            for (const QuadRemesh::BoneWeight& bw : r.bones[v])
                ev.boneAssignments.push_back({bw.bone, bw.weight});
    }
    sub.originalPositions.reserve(nv);
    for (const EditableVertex& ev : sub.vertices) sub.originalPositions.push_back(ev.position);
    for (const auto& f : r.faces) {
        EditableFace ef;
        ef.indices.assign(f.begin(), f.end());
        sub.faces.push_back(std::move(ef));
    }
    triangulateFaces(sub);

    std::vector<EditableSubMesh> subs{sub};
    b.mesh = PartOpsMesh::buildMesh(subs, unique.toStdString() + "_mesh",
                                    hasBones ? info.skeletonName : QString(), {}, /*recomputeNormals=*/false);
    if (!b.mesh) {
        b.error = QStringLiteral("could not build the remeshed mesh");
        return b;
    }
    writeNgonFacesToMesh(b.mesh.get(), subs);
    // Tangents NOW, before any entity is created from this mesh. Normal mapping
    // needs them, and MeshImporterExporter::applyNormalMapsToEntity would add
    // them only after the entity exists — too late for a skinned entity, whose
    // blend buffers were already cloned without a TANGENT element, so the RTSS
    // normal-map shader read a zero tangent basis and the mesh rendered dark.
    // (Imported assets get their tangents at import time, before the entity.)
    if (hasUv && slotFile.contains(QStringLiteral("normal_map"))) {
        try {
            b.mesh->buildTangentVectors(Ogre::VES_TANGENT, 0, 0, false, false, true);
        } catch (const Ogre::Exception&) {
            // No usable UV set: the normal map then simply has no effect.
        }
    }
    b.ok = true;
    return b;
}

Outcome remeshEntity(Ogre::Entity* entity, const QuadRemesh::Options& opts, const QString& textureDir)
{
    Outcome o;
    QuadRemesh::SourceMesh src;
    SourceInfo info;
    if (!readSource(entity, src, info, &o.error))
        return o;
    o.result = QuadRemesh::remesh(src, opts);
    if (!o.result.ok) {
        o.cancelled = o.result.cancelled;
        o.error = o.result.error;
        return o;
    }
    const Built b = buildMesh(o.result, info, entity->getName(), textureDir);
    if (!b.ok) {
        o.error = b.error;
        return o;
    }
    o.mesh = b.mesh;
    o.texturePaths = b.texturePaths;
    o.ok = true;
    return o;
}

QJsonObject reportToJson(const QuadRemesh::Result& r)
{
    QJsonObject j;
    j[QStringLiteral("algorithm")] = QStringLiteral("quadriflow");
    j[QStringLiteral("ok")] = r.ok;
    if (!r.error.isEmpty()) j[QStringLiteral("error")] = r.error;
    j[QStringLiteral("inputTriangles")] = r.inputTriangles;
    j[QStringLiteral("quads")] = r.quads;
    j[QStringLiteral("triangles")] = r.triangles;
    j[QStringLiteral("faces")] = r.quads + r.triangles;
    j[QStringLiteral("vertices")] = r.vertexCount;
    j[QStringLiteral("weldedVertices")] = r.weldedVertices;
    j[QStringLiteral("nonManifoldEdges")] = r.nonManifoldEdges;
    j[QStringLiteral("boundaryEdges")] = r.boundaryEdges;
    j[QStringLiteral("voxelized")] = r.voxelized;
    if (r.voxelized) j[QStringLiteral("voxelReason")] = r.voxelReason;
    if (!r.fallbackReason.isEmpty()) j[QStringLiteral("fallbackReason")] = r.fallbackReason;
    j[QStringLiteral("skinned")] = !r.bones.empty();
    j[QStringLiteral("meanDeviationPct")] = double(r.meanDeviation) * 100.0;
    j[QStringLiteral("maxDeviationPct")] = double(r.maxDeviation) * 100.0;
    QJsonArray maps;
    for (auto it = r.maps.cbegin(); it != r.maps.cend(); ++it) maps.append(it.key());
    j[QStringLiteral("bakedMaps")] = maps;
    if (r.textureWidth > 0) {
        j[QStringLiteral("textureWidth")] = r.textureWidth;
        j[QStringLiteral("textureHeight")] = r.textureHeight;
    }
    return j;
}

QString reportToText(const QuadRemesh::Result& r)
{
    QString s;
    s += QStringLiteral("Quad remesh (QuadriFlow): %1 triangles -> %2 faces (%3 quads, %4 triangles), %5 vertices\n")
             .arg(r.inputTriangles).arg(r.quads + r.triangles).arg(r.quads).arg(r.triangles).arg(r.vertexCount);
    s += QStringLiteral("  shape fidelity: mean %1%, max %2% of the bounding-box diagonal\n")
             .arg(double(r.meanDeviation) * 100.0, 0, 'f', 3).arg(double(r.maxDeviation) * 100.0, 0, 'f', 2);
    if (r.voxelized)
        s += QStringLiteral("  voxel pre-pass: %1\n").arg(r.voxelReason);
    if (!r.fallbackReason.isEmpty())
        s += QStringLiteral("  %1\n").arg(r.fallbackReason);
    if (!r.bones.empty())
        s += QStringLiteral("  skin weights transferred by closest point\n");
    if (!r.maps.isEmpty()) {
        QStringList k = r.maps.keys();
        k.sort();
        s += QStringLiteral("  re-baked textures (%1x%2): %3\n")
                 .arg(r.textureWidth).arg(r.textureHeight).arg(k.join(QStringLiteral(", ")));
    }
    return s;
}

} // namespace QuadRemeshEntity
