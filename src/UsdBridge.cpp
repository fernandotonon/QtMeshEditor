#include "UsdBridge.h"
#include "SentryReporter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <assimp/Exceptional.h>
#include <assimp/BaseImporter.h>
#include <assimp/material.h>
#include <assimp/mesh.h>
#include <assimp/scene.h>

#ifdef ENABLE_USD
#include "lightusd.hh"
#include "usda-writer.hh"
#include "usdc-writer.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "usdSkel.hh"
#include "tydra/render-data.hh"
#endif

namespace UsdBridge {

bool available()
{
#ifdef ENABLE_USD
    return true;
#else
    return false;
#endif
}

QStringList extensions()
{
    return { QStringLiteral("usd"), QStringLiteral("usda"), QStringLiteral("usdc"),
             QStringLiteral("usdz") };
}

bool isUsdPath(const QString& path)
{
    return extensions().contains(QFileInfo(path).suffix().toLower());
}

QString unavailableMessage()
{
    return QStringLiteral("USD support is not compiled into this build "
                          "(rebuild with -DENABLE_USD=ON)");
}

namespace {
thread_local std::vector<MorphWeightClip> t_morphClips;
}

void setWriteExtras(const std::vector<MorphWeightClip>& morphClips) { t_morphClips = morphClips; }
void clearWriteExtras() { t_morphClips.clear(); }

#ifndef ENABLE_USD

void registerImporter(Assimp::Importer&) {}
void registerExporter(Assimp::Exporter&) {}

aiScene* readScene(const QString&, QString* err, QString*)
{
    if (err) *err = unavailableMessage();
    return nullptr;
}

bool writeScene(const QString&, const aiScene*, QString* err, QString*)
{
    if (err) *err = unavailableMessage();
    return false;
}

#else  // ENABLE_USD

namespace {

using namespace lightusd;

// ─── small helpers ──────────────────────────────────────────────────────────

// USD matrices are row-vector (translation in m[3][0..2]); aiMatrix4x4 is
// column-vector. Both directions are a transpose; composition order carries
// over because (A*B)^T = B^T*A^T.
value::matrix4d toUsd(const aiMatrix4x4& a)
{
    value::matrix4d m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m.m[r][c] = static_cast<double>(a[c][r]);
    return m;
}

aiMatrix4x4 toAi(const value::matrix4d& m)
{
    aiMatrix4x4 a;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            a[r][c] = static_cast<ai_real>(m.m[c][r]);
    return a;
}

std::string sanitizeIdentifier(const std::string& in, const std::string& fallback)
{
    std::string s;
    s.reserve(in.size());
    for (unsigned char c : in)
        s += (std::isalnum(c) || c == '_') ? static_cast<char>(c) : '_';
    if (s.empty()) s = fallback;
    if (std::isdigit(static_cast<unsigned char>(s[0]))) s.insert(s.begin(), '_');
    return s;
}

// Hands out prim names that are unique within one parent.
class NameSet {
public:
    std::string unique(const std::string& base)
    {
        std::string n = base;
        for (int i = 1; m_used.count(n); ++i)
            n = base + "_" + std::to_string(i);
        m_used.insert(n);
        return n;
    }
private:
    std::set<std::string> m_used;
};

aiMatrix4x4 globalOf(const aiNode* n)
{
    aiMatrix4x4 g;
    for (const aiNode* p = n; p; p = p->mParent)
        g = p->mTransformation * g;
    return g;
}

constexpr double kTimeCodesPerSecond = 24.0;

// ─── WRITE: aiScene -> USD ──────────────────────────────────────────────────

struct Joint {
    const aiNode* node{};
    int parent{-1};
    std::string path;   // "root/spine/head"
};

struct SkeletonInfo {
    std::vector<Joint> joints;
    std::map<std::string, int> byName;
    bool valid() const { return !joints.empty(); }
};

bool subtreeHasBone(const aiNode* n, const std::set<std::string>& bones)
{
    if (bones.count(n->mName.C_Str())) return true;
    for (unsigned c = 0; c < n->mNumChildren; ++c)
        if (subtreeHasBone(n->mChildren[c], bones)) return true;
    return false;
}

void collectJoints(const aiNode* n, int parent, const std::string& parentPath,
                   SkeletonInfo& out, std::map<const aiNode*, int>& index)
{
    NameSet dummy;  // sibling uniqueness is handled by the caller via paths below
    std::string seg = sanitizeIdentifier(n->mName.C_Str(), "joint");
    std::string path = parentPath.empty() ? seg : parentPath + "/" + seg;
    // Sibling collisions would make two joints share a path; disambiguate.
    for (int i = 1; true; ++i) {
        bool clash = false;
        for (const auto& j : out.joints)
            if (j.path == path) { clash = true; break; }
        if (!clash) break;
        path = (parentPath.empty() ? seg : parentPath + "/" + seg) + "_" + std::to_string(i);
    }
    const int me = static_cast<int>(out.joints.size());
    out.joints.push_back({ n, parent, path });
    out.byName[n->mName.C_Str()] = me;
    index[n] = me;
    for (unsigned c = 0; c < n->mNumChildren; ++c)
        collectJoints(n->mChildren[c], me, path, out, index);
}

SkeletonInfo buildSkeleton(const aiScene* scene)
{
    SkeletonInfo sk;
    std::set<std::string> bones;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m)
        for (unsigned b = 0; b < scene->mMeshes[m]->mNumBones; ++b)
            bones.insert(scene->mMeshes[m]->mBones[b]->mName.C_Str());
    if (bones.empty() || !scene->mRootNode) return sk;

    std::map<const aiNode*, int> index;
    const aiNode* root = scene->mRootNode;
    for (unsigned c = 0; c < root->mNumChildren; ++c) {
        const aiNode* child = root->mChildren[c];
        if (child->mNumMeshes == 0 && subtreeHasBone(child, bones))
            collectJoints(child, -1, "", sk, index);
    }
    return sk;
}

struct MaterialOut {
    std::string primName;
};

// Builds `/<root>/Materials/<name>` and returns the prim path of each
// aiMaterial. Textures are referenced by file name; for .usdz the bytes are
// collected into `assets` so the package is self-contained.
void buildMaterials(const aiScene* scene, const std::string& rootName,
                    const QString& outDir, bool packageAssets,
                    Prim& materialsScope, std::vector<std::string>& materialPaths,
                    std::map<std::string, std::vector<uint8_t>>& assets,
                    std::string& warn)
{
    NameSet names;
    const std::string scopePath = "/" + rootName + "/Materials";

    auto textureOf = [&](const aiMaterial* m, aiTextureType t, std::string* assetPath) {
        aiString p;
        if (m->GetTexture(t, 0, &p) != AI_SUCCESS || p.length == 0) return false;
        const QString src = QString::fromUtf8(p.C_Str());
        if (src.startsWith('*')) return false;  // embedded; not supported on export
        const QString base = QFileInfo(src).fileName();
        *assetPath = base.toStdString();
        if (packageAssets) {
            QString found;
            for (const QString& cand : { src, QDir(outDir).filePath(base) })
                if (QFileInfo::exists(cand)) { found = cand; break; }
            QFile f(found);
            if (!found.isEmpty() && f.open(QIODevice::ReadOnly)) {
                const QByteArray bytes = f.readAll();
                assets[base.toStdString()] =
                    std::vector<uint8_t>(bytes.begin(), bytes.end());
            } else {
                warn += "texture not found, not packaged: " + base.toStdString() + "\n";
            }
        }
        return true;
    };

    for (unsigned mi = 0; mi < scene->mNumMaterials; ++mi) {
        const aiMaterial* am = scene->mMaterials[mi];
        aiString nm;
        am->Get(AI_MATKEY_NAME, nm);
        const std::string primName =
            names.unique(sanitizeIdentifier(nm.C_Str(), "Material"));
        const std::string matPath = scopePath + "/" + primName;
        materialPaths.push_back(matPath);

        Material mat;
        mat.name = primName;
        mat.surface.set(Path(matPath + "/Surface", "outputs:surface"));

        Prim matPrim(mat);
        std::string err;

        // ---- UsdPreviewSurface ----
        Shader surface;
        surface.name = "Surface";
        surface.info_id = kUsdPreviewSurface;
        {
            UsdPreviewSurface ps;
            ps.outputsSurface.set_authored(true);

            aiColor3D diffuse(0.8f, 0.8f, 0.8f);
            am->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);
            ps.diffuseColor = value::color3f{diffuse.r, diffuse.g, diffuse.b};

            aiColor3D emissive(0, 0, 0);
            if (am->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS &&
                (emissive.r > 0 || emissive.g > 0 || emissive.b > 0))
                ps.emissiveColor = value::color3f{emissive.r, emissive.g, emissive.b};

            float opacity = 1.0f;
            if (am->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 1.0f)
                ps.opacity = opacity;
            float metallic = 0.0f, roughness = 0.5f;
            if (am->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS)
                ps.metallic = metallic;
            if (am->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS)
                ps.roughness = roughness;

            std::string diffuseFile, normalFile;
            if (!textureOf(am, aiTextureType_BASE_COLOR, &diffuseFile))
                textureOf(am, aiTextureType_DIFFUSE, &diffuseFile);
            if (!textureOf(am, aiTextureType_NORMALS, &normalFile))
                textureOf(am, aiTextureType_HEIGHT, &normalFile);

            if (!diffuseFile.empty()) {
                ps.diffuseColor.set_connection(Path(matPath + "/DiffuseTexture", "outputs:rgb"));
                ps.diffuseColor.set_value_empty();
            }
            if (!normalFile.empty()) {
                ps.normal.set_connection(Path(matPath + "/NormalTexture", "outputs:rgb"));
                ps.normal.set_value_empty();
            }
            surface.value = std::move(ps);

            // ---- texture network (shared st reader) ----
            if (!diffuseFile.empty() || !normalFile.empty()) {
                Shader reader;
                reader.name = "StReader";
                reader.info_id = kUsdPrimvarReader_float2;
                {
                    UsdPrimvarReader_float2 pr;
                    pr.varname.set_value(Animatable<std::string>(std::string("st")));
                    pr.result.set_authored(true);
                    reader.value = std::move(pr);
                }
                matPrim.add_child(Prim(reader), true, &err);

                auto addTex = [&](const char* primName2, const std::string& file, bool srgb) {
                    Shader tex;
                    tex.name = primName2;
                    tex.info_id = kUsdUVTexture;
                    UsdUVTexture t;
                    t.file.set_value(Animatable<value::AssetPath>(value::AssetPath(file)));
                    t.st.set_connection(Path(matPath + "/StReader", "outputs:result"));
                    t.st.set_value_empty();
                    t.wrapS.set_value(Animatable<UsdUVTexture::Wrap>(UsdUVTexture::Wrap::Repeat));
                    t.wrapT.set_value(Animatable<UsdUVTexture::Wrap>(UsdUVTexture::Wrap::Repeat));
                    t.sourceColorSpace.set_value(Animatable<UsdUVTexture::SourceColorSpace>(
                        srgb ? UsdUVTexture::SourceColorSpace::SRGB
                             : UsdUVTexture::SourceColorSpace::Raw));
                    if (!srgb) {
                        // tangent-space normal map decode: n = tex * 2 - 1
                        t.scale = value::float4{2.0f, 2.0f, 2.0f, 1.0f};
                        t.bias = value::float4{-1.0f, -1.0f, -1.0f, 0.0f};
                    }
                    t.outputsRGB.set_authored(true);
                    tex.value = std::move(t);
                    matPrim.add_child(Prim(tex), true, &err);
                };
                if (!diffuseFile.empty()) addTex("DiffuseTexture", diffuseFile, true);
                if (!normalFile.empty()) addTex("NormalTexture", normalFile, false);
            }
        }
        matPrim.add_child(Prim(surface), true, &err);
        materialsScope.add_child(std::move(matPrim), true, &err);
    }
}

// Per-vertex influences (top `k`, normalised) for one aiMesh.
struct Influences {
    int k{0};
    std::vector<int> indices;
    std::vector<float> weights;
};

Influences gatherInfluences(const aiMesh* mesh, const SkeletonInfo& sk)
{
    Influences out;
    std::vector<std::vector<std::pair<float, int>>> per(mesh->mNumVertices);
    for (unsigned b = 0; b < mesh->mNumBones; ++b) {
        const aiBone* bone = mesh->mBones[b];
        auto it = sk.byName.find(bone->mName.C_Str());
        if (it == sk.byName.end()) continue;
        for (unsigned w = 0; w < bone->mNumWeights; ++w) {
            const auto& vw = bone->mWeights[w];
            if (vw.mVertexId < per.size() && vw.mWeight > 0.0f)
                per[vw.mVertexId].push_back({ vw.mWeight, it->second });
        }
    }
    size_t k = 1;
    for (auto& v : per) {
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        k = std::max(k, v.size());
    }
    k = std::min<size_t>(k, 8);
    out.k = static_cast<int>(k);
    out.indices.assign(per.size() * k, 0);
    out.weights.assign(per.size() * k, 0.0f);
    for (size_t v = 0; v < per.size(); ++v) {
        float sum = 0;
        const size_t n = std::min(per[v].size(), k);
        for (size_t i = 0; i < n; ++i) sum += per[v][i].first;
        for (size_t i = 0; i < n; ++i) {
            out.indices[v * k + i] = per[v][i].second;
            out.weights[v * k + i] = sum > 0 ? per[v][i].first / sum : 0.0f;
        }
    }
    return out;
}

void setPrimvar(GeomMesh& mesh, const char* name, Attribute&& attr, Interpolation interp)
{
    attr.metas().set_interpolation_enum(interp);
    mesh.props.emplace(name, Property(std::move(attr)));
}

// Blend-shape name -> unique USD identifier, built ONCE over every mesh's shapes
// and every morph clip, so the BlendShape prim names, `skel:blendShapes` and the
// SkelAnimation's `blendShapes` can never disagree (two names that sanitise to
// the same identifier get distinct tokens, e.g. "Smile-L" / "Smile L").
using ShapeTokens = std::map<std::string, std::string>;

void applyApis(Prim& prim, bool materialBinding, bool skelBinding);

Prim buildMeshPrim(const aiMesh* am, const std::string& primName, const SkeletonInfo* sk,
                   const std::string* skelPath, const std::string* materialPath,
                   bool forceSkinToJoint0, const std::string& meshPath,
                   const ShapeTokens& shapeTokenMap)
{
    GeomMesh mesh;
    mesh.name = primName;

    std::vector<value::point3f> pts(am->mNumVertices);
    for (unsigned v = 0; v < am->mNumVertices; ++v)
        pts[v] = { am->mVertices[v].x, am->mVertices[v].y, am->mVertices[v].z };
    mesh.points.set_value(std::move(pts));

    std::vector<int> counts, indices;
    for (unsigned f = 0; f < am->mNumFaces; ++f) {
        const aiFace& face = am->mFaces[f];
        if (face.mNumIndices < 3) continue;  // points/lines are not USD polygons
        counts.push_back(static_cast<int>(face.mNumIndices));
        for (unsigned i = 0; i < face.mNumIndices; ++i)
            indices.push_back(static_cast<int>(face.mIndices[i]));
    }
    mesh.faceVertexCounts.set_value(std::move(counts));
    mesh.faceVertexIndices.set_value(std::move(indices));

    if (am->HasNormals()) {
        std::vector<value::normal3f> ns(am->mNumVertices);
        for (unsigned v = 0; v < am->mNumVertices; ++v)
            ns[v] = { am->mNormals[v].x, am->mNormals[v].y, am->mNormals[v].z };
        mesh.normals.set_value(std::move(ns));
        mesh.normals.metas().set_interpolation_enum(Interpolation::Vertex);
    }

    if (am->HasTextureCoords(0)) {
        GeomPrimvar uv;
        uv.set_name("st");
        std::vector<value::texcoord2f> uvs(am->mNumVertices);
        for (unsigned v = 0; v < am->mNumVertices; ++v)
            uvs[v] = { am->mTextureCoords[0][v].x, am->mTextureCoords[0][v].y };
        uv.set_value(std::move(uvs));
        uv.set_interpolation(Interpolation::Vertex);
        std::string err;
        mesh.set_primvar(uv, &err);
    }

    if (materialPath) {
        Relationship rel;
        rel.set(Path(*materialPath, ""));
        mesh.materialBinding = rel;
    }

    const bool skinned = sk && sk->valid() && am->mNumBones > 0;
    const bool jointZero = forceSkinToJoint0 && !skinned;
    if ((skinned || jointZero) && skelPath) {
        Relationship skelRel;
        skelRel.set(Path(*skelPath, ""));
        mesh.skeleton = skelRel;

        Influences inf;
        if (skinned) {
            inf = gatherInfluences(am, *sk);
        } else {
            inf.k = 1;
            inf.indices.assign(am->mNumVertices, 0);
            inf.weights.assign(am->mNumVertices, 1.0f);
        }
        {
            Attribute a;
            a.set_value(std::move(inf.indices));
            a.metas().set_elementSize(static_cast<uint32_t>(inf.k));
            setPrimvar(mesh, "primvars:skel:jointIndices", std::move(a), Interpolation::Vertex);
        }
        {
            Attribute a;
            a.set_value(std::move(inf.weights));
            a.metas().set_elementSize(static_cast<uint32_t>(inf.k));
            setPrimvar(mesh, "primvars:skel:jointWeights", std::move(a), Interpolation::Vertex);
        }
        {
            Attribute a;
            value::matrix4d id;
            Identity(&id);
            a.set_value(id);
            a.variability() = Variability::Uniform;
            mesh.props.emplace("primvars:skel:geomBindTransform", Property(std::move(a)));
        }
    }

    Prim prim(mesh);

    // ---- blend shapes ----
    if (am->mNumAnimMeshes > 0) {
        std::vector<value::token> shapeNames;
        std::vector<Path> shapePaths;
        NameSet names;
        for (unsigned s = 0; s < am->mNumAnimMeshes; ++s) {
            const aiAnimMesh* shape = am->mAnimMeshes[s];
            if (!shape->mVertices || shape->mNumVertices != am->mNumVertices) continue;
            const std::string original = shape->mName.C_Str();
            auto tokIt = shapeTokenMap.find(original);
            const std::string bsName = names.unique(
                tokIt != shapeTokenMap.end()
                    ? tokIt->second
                    : sanitizeIdentifier(original, "shape" + std::to_string(s)));

            BlendShape bs;
            bs.name = bsName;
            std::vector<value::vector3f> offsets, normalOffsets;
            std::vector<int> pointIndices;
            for (unsigned v = 0; v < am->mNumVertices; ++v) {
                const aiVector3D d = shape->mVertices[v] - am->mVertices[v];
                if (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f) continue;
                offsets.push_back({ d.x, d.y, d.z });
                normalOffsets.push_back({ 0.0f, 0.0f, 0.0f });
                pointIndices.push_back(static_cast<int>(v));
            }
            bs.offsets.set_value(std::move(offsets));
            bs.normalOffsets.set_value(std::move(normalOffsets));
            bs.pointIndices.set_value(std::move(pointIndices));
            if (original != bsName) bs.metas().set_displayName(original);

            Prim bsPrim(bs);
            std::string err;
            prim.add_child(std::move(bsPrim), true, &err);
            shapeNames.push_back(value::token(bsName));
            shapePaths.push_back(Path(meshPath + "/" + bsName, ""));
        }
        if (!shapeNames.empty()) {
            // `prim` was built from `mesh` before the shapes existed; re-fetch
            // the typed object to author the binding.
            if (auto* gm = prim.get_data().as<GeomMesh>()) {
                gm->blendShapes.set_value(std::move(shapeNames));
                Relationship rel;
                rel.set(shapePaths);
                gm->blendShapeTargets = rel;
            }
        }
    }
    applyApis(prim, materialPath != nullptr, (skinned || jointZero) && skelPath != nullptr);
    return prim;
}

// Applies API schemas to a prim ("apiSchemas = prepend [...]"). OpenUSD only
// honours `material:binding` / `skel:*` relationships on prims that apply the
// matching API, so a file without them reads as an unbound, unskinned mesh.
void applyApis(Prim& prim, bool materialBinding, bool skelBinding)
{
    auto& schemas = prim.metas().get_apiSchemas_mutable();
    schemas.listOpQual = ListEditQual::Prepend;
    std::vector<std::pair<std::string, std::string>> authored;
    if (materialBinding) {
        schemas.names.push_back({ APISchemas::APIName::MaterialBindingAPI, std::string() });
        authored.push_back({ "MaterialBindingAPI", std::string() });
    }
    if (skelBinding) {
        schemas.names.push_back({ APISchemas::APIName::SkelBindingAPI, std::string() });
        authored.push_back({ "SkelBindingAPI", std::string() });
    }
    if (!authored.empty())
        schemas.authoredOps.push_back({ ListEditQual::Prepend, authored });
}

// Evaluate an aiNodeAnim channel at time t (seconds-ticks already normalised).
struct Trs { aiVector3D t; aiQuaternion r; aiVector3D s; };

template <typename Key>
const Key* keyBefore(const Key* keys, unsigned n, double t, unsigned* next)
{
    unsigned i = 0;
    while (i + 1 < n && keys[i + 1].mTime <= t) ++i;
    *next = std::min(i + 1, n - 1);
    return &keys[i];
}

aiVector3D sampleVec(const aiVectorKey* keys, unsigned n, double t, const aiVector3D& rest)
{
    if (n == 0) return rest;
    unsigned nx;
    const aiVectorKey* a = keyBefore(keys, n, t, &nx);
    const aiVectorKey& b = keys[nx];
    if (&b == a || b.mTime <= a->mTime) return a->mValue;
    const double f = std::clamp((t - a->mTime) / (b.mTime - a->mTime), 0.0, 1.0);
    return a->mValue + (b.mValue - a->mValue) * static_cast<ai_real>(f);
}

aiQuaternion sampleQuat(const aiQuatKey* keys, unsigned n, double t, const aiQuaternion& rest)
{
    if (n == 0) return rest;
    unsigned nx;
    const aiQuatKey* a = keyBefore(keys, n, t, &nx);
    const aiQuatKey& b = keys[nx];
    if (&b == a || b.mTime <= a->mTime) return a->mValue;
    const double f = std::clamp((t - a->mTime) / (b.mTime - a->mTime), 0.0, 1.0);
    aiQuaternion out;
    aiQuaternion::Interpolate(out, a->mValue, b.mValue, static_cast<ai_real>(f));
    return out.Normalize();
}

value::quatf toQuatf(const aiQuaternion& q)
{
    value::quatf o;
    o.real = q.w;
    o.imag = { q.x, q.y, q.z };
    return o;
}

std::vector<double> unionTimes(const aiAnimation* anim, const std::set<std::string>& names)
{
    std::vector<double> times;
    const double tps = anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 1.0;
    for (unsigned c = 0; c < anim->mNumChannels; ++c) {
        const aiNodeAnim* ch = anim->mChannels[c];
        if (!names.count(ch->mNodeName.C_Str())) continue;
        for (unsigned i = 0; i < ch->mNumPositionKeys; ++i) times.push_back(ch->mPositionKeys[i].mTime / tps);
        for (unsigned i = 0; i < ch->mNumRotationKeys; ++i) times.push_back(ch->mRotationKeys[i].mTime / tps);
        for (unsigned i = 0; i < ch->mNumScalingKeys; ++i) times.push_back(ch->mScalingKeys[i].mTime / tps);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(),
                            [](double a, double b) { return std::fabs(a - b) < 1e-6; }),
                times.end());
    return times;
}

// One SkelAnimation: every joint is keyed at every union time so the three
// arrays line up (UsdSkel requires translations/rotations/scales together).
// `morph` (optional) adds blendShapes + weights.
Prim buildSkelAnimation(const std::string& name, const SkeletonInfo& sk,
                        const aiAnimation* anim, const MorphWeightClip* morph,
                        double* endTime, const ShapeTokens& shapeTokenMap)
{
    SkelAnimation sa;
    sa.name = name;

    std::set<std::string> jointNames;
    for (const auto& j : sk.joints) jointNames.insert(j.node->mName.C_Str());

    std::vector<double> times;
    if (anim) times = unionTimes(anim, jointNames);
    if (morph) times.insert(times.end(), morph->times.begin(), morph->times.end());
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(),
                            [](double a, double b) { return std::fabs(a - b) < 1e-6; }),
                times.end());
    if (times.empty()) times.push_back(0.0);

    std::map<std::string, const aiNodeAnim*> channels;
    const double tps = anim && anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 1.0;
    if (anim)
        for (unsigned c = 0; c < anim->mNumChannels; ++c)
            channels[anim->mChannels[c]->mNodeName.C_Str()] = anim->mChannels[c];

    std::vector<value::token> joints;
    for (const auto& j : sk.joints) joints.push_back(value::token(j.path));
    sa.joints.set_value(std::move(joints));

    Animatable<std::vector<value::float3>> translations;
    Animatable<std::vector<value::quatf>> rotations;
    Animatable<std::vector<value::half3>> scales;

    // Rest decomposition per joint (used when a joint has no channel).
    std::vector<Trs> rest(sk.joints.size());
    for (size_t j = 0; j < sk.joints.size(); ++j)
        sk.joints[j].node->mTransformation.Decompose(rest[j].s, rest[j].r, rest[j].t);

    for (double t : times) {
        std::vector<value::float3> tv(sk.joints.size());
        std::vector<value::quatf> rv(sk.joints.size());
        std::vector<value::half3> sv(sk.joints.size());
        for (size_t j = 0; j < sk.joints.size(); ++j) {
            const aiNodeAnim* ch = nullptr;
            auto it = channels.find(sk.joints[j].node->mName.C_Str());
            if (it != channels.end()) ch = it->second;
            aiVector3D p = rest[j].t, s = rest[j].s;
            aiQuaternion r = rest[j].r;
            if (ch) {
                const double ticks = t * tps;
                p = sampleVec(ch->mPositionKeys, ch->mNumPositionKeys, ticks, p);
                r = sampleQuat(ch->mRotationKeys, ch->mNumRotationKeys, ticks, r);
                s = sampleVec(ch->mScalingKeys, ch->mNumScalingKeys, ticks, s);
            }
            tv[j] = { p.x, p.y, p.z };
            rv[j] = toQuatf(r);
            sv[j] = { value::float_to_half_full(s.x), value::float_to_half_full(s.y),
                      value::float_to_half_full(s.z) };
        }
        const double tc = t * kTimeCodesPerSecond;
        translations.add_sample(tc, tv);
        rotations.add_sample(tc, rv);
        scales.add_sample(tc, sv);
    }
    sa.translations.set_value(std::move(translations));
    sa.rotations.set_value(std::move(rotations));
    sa.scales.set_value(std::move(scales));

    if (morph && !morph->shapes.empty()) {
        std::vector<value::token> shapeTokens;
        for (const auto& s : morph->shapes) {
            auto it = shapeTokenMap.find(s);
            shapeTokens.push_back(value::token(
                it != shapeTokenMap.end() ? it->second : sanitizeIdentifier(s, "shape")));
        }
        sa.blendShapes.set_value(std::move(shapeTokens));

        Animatable<std::vector<float>> weights;
        for (double t : times) {
            // Linear between the surrounding morph keys (what the glTF export
            // does). The sample times are the UNION with the joint clip's, so
            // holding the earlier key would change the authored animation at
            // every inserted time.
            const size_t nShapes = morph->shapes.size();
            std::vector<float> w(nShapes, 0.0f);
            const size_t nKeys = std::min(morph->times.size(), morph->weights.size());
            if (nKeys > 0) {
                size_t hi = 0;
                while (hi < nKeys && morph->times[hi] < t - 1e-9) ++hi;
                if (hi == 0) {
                    w = morph->weights[0];
                } else if (hi >= nKeys) {
                    w = morph->weights[nKeys - 1];
                } else {
                    const double t0 = morph->times[hi - 1], t1 = morph->times[hi];
                    const float f = t1 > t0 ? static_cast<float>((t - t0) / (t1 - t0)) : 0.0f;
                    const auto& a = morph->weights[hi - 1];
                    const auto& b = morph->weights[hi];
                    for (size_t i = 0; i < nShapes; ++i) {
                        const float av = i < a.size() ? a[i] : 0.0f;
                        const float bv = i < b.size() ? b[i] : 0.0f;
                        w[i] = av + (bv - av) * f;
                    }
                }
                w.resize(nShapes, 0.0f);
            }
            weights.add_sample(t * kTimeCodesPerSecond, w);
        }
        sa.blendShapeWeights.set_value(std::move(weights));
    }

    if (endTime) *endTime = std::max(*endTime, times.back());
    return Prim(sa);
}

}  // namespace

bool writeSceneImpl(const QString& path, const aiScene* scene, QString* err, QString* warnOut)
{
    auto fail = [&](const QString& m) {
        if (err) *err = m;
        return false;
    };
    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
        QStringLiteral("export format=usd %1").arg(QFileInfo(path).fileName()));
    if (!scene || !scene->mRootNode) return fail(QStringLiteral("empty scene"));

    const QString ext = QFileInfo(path).suffix().toLower();
    const bool isUsdz = ext == QLatin1String("usdz");
    const QString outDir = QFileInfo(path).absolutePath();

    const std::string rootName = sanitizeIdentifier(scene->mRootNode->mName.C_Str(), "Root");
    const std::string rootPath = "/" + rootName;

    std::string warn;
    std::map<std::string, std::vector<uint8_t>> assets;

    SkeletonInfo sk = buildSkeleton(scene);

    // Morph-weight clips need a skeleton to drive them; a mesh with only blend
    // shapes gets a single-joint skeleton (what DCC exporters do too).
    bool anyShapes = false;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m)
        if (scene->mMeshes[m]->mNumAnimMeshes > 0) anyShapes = true;
    const bool morphOnlySkeleton = !sk.valid() && anyShapes && !t_morphClips.empty();

    // ---------- node animation on the root xform ----------
    const aiNodeAnim* rootAnim = nullptr;
    double rootAnimTps = 1.0;
    for (unsigned a = 0; a < scene->mNumAnimations && !rootAnim; ++a)
        for (unsigned c = 0; c < scene->mAnimations[a]->mNumChannels; ++c)
            if (std::string(scene->mAnimations[a]->mChannels[c]->mNodeName.C_Str()) ==
                scene->mRootNode->mName.C_Str()) {
                rootAnim = scene->mAnimations[a]->mChannels[c];
                rootAnimTps = scene->mAnimations[a]->mTicksPerSecond > 0
                    ? scene->mAnimations[a]->mTicksPerSecond : 1.0;
                break;
            }

    Xform rootXform;
    rootXform.name = rootName;
    double endTime = 0.0;
    if (rootAnim) {
        auto addOp = [&](XformOp::OpType type, auto&& writer) {
            XformOp op;
            op.op_type = type;
            writer(op);
            rootXform.xformOps.push_back(op);
        };
        if (rootAnim->mNumPositionKeys)
            addOp(XformOp::OpType::Translate, [&](XformOp& op) {
                for (unsigned i = 0; i < rootAnim->mNumPositionKeys; ++i) {
                    const auto& k = rootAnim->mPositionKeys[i];
                    op.set_timesample(static_cast<float>(k.mTime / rootAnimTps * kTimeCodesPerSecond),
                                      value::double3{ k.mValue.x, k.mValue.y, k.mValue.z });
                    endTime = std::max(endTime, k.mTime / rootAnimTps);
                }
            });
        if (rootAnim->mNumRotationKeys)
            addOp(XformOp::OpType::Orient, [&](XformOp& op) {
                for (unsigned i = 0; i < rootAnim->mNumRotationKeys; ++i) {
                    const auto& k = rootAnim->mRotationKeys[i];
                    op.set_timesample(static_cast<float>(k.mTime / rootAnimTps * kTimeCodesPerSecond),
                                      toQuatf(k.mValue));
                    endTime = std::max(endTime, k.mTime / rootAnimTps);
                }
            });
        if (rootAnim->mNumScalingKeys)
            addOp(XformOp::OpType::Scale, [&](XformOp& op) {
                for (unsigned i = 0; i < rootAnim->mNumScalingKeys; ++i) {
                    const auto& k = rootAnim->mScalingKeys[i];
                    op.set_timesample(static_cast<float>(k.mTime / rootAnimTps * kTimeCodesPerSecond),
                                      value::float3{ k.mValue.x, k.mValue.y, k.mValue.z });
                    endTime = std::max(endTime, k.mTime / rootAnimTps);
                }
            });
    }
    Prim rootPrim(rootXform);
    std::string aerr;

    // ---------- materials ----------
    Scope materialsScopeData;
    materialsScopeData.name = "Materials";
    Prim materialsScope(materialsScopeData);
    std::vector<std::string> materialPaths;
    buildMaterials(scene, rootName, outDir, isUsdz, materialsScope, materialPaths, assets, warn);
    if (scene->mNumMaterials > 0)
        rootPrim.add_child(std::move(materialsScope), true, &aerr);

    // ---------- blend-shape tokens (shared by meshes and clips) ----------
    ShapeTokens shapeTokenMap;
    {
        NameSet tokens;
        auto add = [&](const std::string& original) {
            if (!shapeTokenMap.count(original))
                shapeTokenMap[original] = tokens.unique(sanitizeIdentifier(
                    original, "shape" + std::to_string(shapeTokenMap.size())));
        };
        for (unsigned m = 0; m < scene->mNumMeshes; ++m)
            for (unsigned a = 0; a < scene->mMeshes[m]->mNumAnimMeshes; ++a)
                add(scene->mMeshes[m]->mAnimMeshes[a]->mName.C_Str());
        for (const auto& mc : t_morphClips)
            for (const auto& s : mc.shapes) add(s);
    }

    // ---------- geometry (+ skeleton) ----------
    const bool needSkel = sk.valid() || morphOnlySkeleton;
    const std::string rigName = "Rig";
    const std::string rigPath = rootPath + "/" + rigName;
    const std::string skelPath = rigPath + "/Skel";

    // Mesh parent: SkelRoot when skinned, else the root xform.
    SkelRoot rigData;
    rigData.name = rigName;
    Prim rigPrim(rigData);
    applyApis(rigPrim, false, true);

    NameSet meshNames;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* am = scene->mMeshes[m];
        const std::string meshName = meshNames.unique(
            sanitizeIdentifier(am->mName.length ? am->mName.C_Str() : "mesh", "mesh"));
        const std::string* matPath = am->mMaterialIndex < materialPaths.size()
            ? &materialPaths[am->mMaterialIndex] : nullptr;
        const std::string parentPath = needSkel ? rigPath : rootPath;
        const bool shapeBound = needSkel && am->mNumAnimMeshes > 0;
        Prim meshPrim = buildMeshPrim(am, meshName, needSkel ? &sk : nullptr,
                                      needSkel ? &skelPath : nullptr, matPath,
                                      shapeBound && morphOnlySkeleton,
                                      parentPath + "/" + meshName, shapeTokenMap);
        if (needSkel)
            rigPrim.add_child(std::move(meshPrim), true, &aerr);
        else
            rootPrim.add_child(std::move(meshPrim), true, &aerr);
    }

    if (needSkel) {
        Skeleton skel;
        skel.name = "Skel";

        std::vector<value::token> joints, jointNames;
        std::vector<value::matrix4d> bind, restLocal;
        if (sk.valid()) {
            for (const auto& j : sk.joints) {
                joints.push_back(value::token(j.path));
                jointNames.push_back(value::token(j.node->mName.C_Str()));
                bind.push_back(toUsd(globalOf(j.node)));
                restLocal.push_back(toUsd(j.node->mTransformation));
            }
        } else {
            value::matrix4d id;
            Identity(&id);
            joints.push_back(value::token("root"));
            jointNames.push_back(value::token("root"));
            bind.push_back(id);
            restLocal.push_back(id);
        }
        skel.joints.set_value(std::move(joints));
        // Original joint names. NOT `jointNames`: OpenUSD readers (Blender)
        // parse that attribute as SdfPaths, so a name like "mixamorig:Hips"
        // invalidates the whole joint order and drops the skeleton. The
        // verbatim names ride a custom attribute instead, and the importer
        // prefers it.
        {
            Attribute names;
            names.set_value(std::move(jointNames));
            names.variability() = Variability::Uniform;
            skel.props.emplace("qtmesh:jointNames", Property(std::move(names), /* custom */ true));
        }
        skel.bindTransforms.set_value(std::move(bind));
        skel.restTransforms.set_value(std::move(restLocal));

        Prim skelPrim(skel);
        applyApis(skelPrim, false, true);

        // Joint-less skeleton helper for clip building.
        SkeletonInfo animSk = sk;
        aiNode fakeRoot;
        if (!animSk.valid()) {
            fakeRoot.mName = aiString("root");
            animSk.joints.push_back({ &fakeRoot, -1, "root" });
        }

        NameSet clipNames;
        std::string firstClipPath;
        auto addClip = [&](const std::string& rawName, const aiAnimation* anim,
                           const MorphWeightClip* morph) {
            const std::string clipName = clipNames.unique(sanitizeIdentifier(rawName, "Clip"));
            Prim clipPrim = buildSkelAnimation(clipName, animSk, anim, morph, &endTime, shapeTokenMap);
            if (firstClipPath.empty())
                firstClipPath = skelPath + "/" + clipName;
            // The display name keeps the original (possibly non-identifier) text.
            if (rawName != clipName)
                if (auto* sa = clipPrim.get_data().as<SkelAnimation>())
                    sa->metas().set_displayName(rawName);
            skelPrim.add_child(std::move(clipPrim), true, &aerr);
        };

        std::set<std::string> consumedMorph;
        if (sk.valid()) {
            std::set<std::string> jointSet;
            for (const auto& j : sk.joints) jointSet.insert(j.node->mName.C_Str());
            for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
                const aiAnimation* anim = scene->mAnimations[a];
                bool drivesJoint = false;
                for (unsigned c = 0; c < anim->mNumChannels; ++c)
                    if (jointSet.count(anim->mChannels[c]->mNodeName.C_Str())) { drivesJoint = true; break; }
                if (!drivesJoint) continue;
                const MorphWeightClip* morph = nullptr;
                for (const auto& mc : t_morphClips)
                    if (mc.name == anim->mName.C_Str()) { morph = &mc; consumedMorph.insert(mc.name); }
                addClip(anim->mName.C_Str(), anim, morph);
            }
        }
        for (const auto& mc : t_morphClips)
            if (!consumedMorph.count(mc.name) && anyShapes)
                addClip(mc.name, nullptr, &mc);

        if (!firstClipPath.empty())
            if (auto* sp = skelPrim.get_data().as<Skeleton>()) {
                Relationship rel;
                rel.set(Path(firstClipPath, ""));
                sp->animationSource = rel;
            }

        rigPrim.add_child(std::move(skelPrim), true, &aerr);

        if (auto* rp = rigPrim.get_data().as<SkelRoot>()) {
            Relationship rel;
            rel.set(Path(skelPath, ""));
            rp->skeleton = rel;
        }
        rootPrim.add_child(std::move(rigPrim), true, &aerr);
    }

    Stage stage;
    stage.metas().upAxis.set_value(Axis::Y);
    stage.metas().metersPerUnit.set_value(1.0);
    stage.metas().timeCodesPerSecond.set_value(kTimeCodesPerSecond);
    stage.metas().framesPerSecond.set_value(kTimeCodesPerSecond);
    stage.metas().startTimeCode.set_value(0.0);
    stage.metas().endTimeCode.set_value(endTime * kTimeCodesPerSecond);
    stage.metas().defaultPrim = value::token(rootName);
    stage.metas().comment = "Exported by QtMeshEditor";
    if (!stage.add_root_prim(std::move(rootPrim)))
        return fail(QStringLiteral("could not build the USD stage"));

    // Write through a temp file + rename so a failure never truncates an
    // existing file.
    const QString tmp = path + QStringLiteral(".qtmesh_tmp");
    QFile::remove(tmp);
    std::string w, e;
    bool ok = false;
    if (isUsdz) {
        ok = SaveAsUSDZToFile(tmp.toStdString(), stage, assets, &w, &e);
    } else if (ext == QLatin1String("usda")) {
        ok = usda::SaveAsUSDA(tmp.toStdString(), stage, &w, &e);
    } else {
        ok = usdc::SaveAsUSDCToFile(tmp.toStdString(), stage, &w, &e);
    }
    warn += w;
    if (warnOut) *warnOut = QString::fromStdString(warn);
    if (!ok) {
        QFile::remove(tmp);
        return fail(QStringLiteral("USD write failed: %1").arg(QString::fromStdString(e)));
    }
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) {
        QFile::remove(tmp);
        return fail(QStringLiteral("could not move the written file into place: %1").arg(path));
    }
    return true;
}

bool writeScene(const QString& path, const aiScene* scene, QString* err, QString* warn)
{
    return writeSceneImpl(path, scene, err, warn);
}

// ─── READ: USD -> aiScene ───────────────────────────────────────────────────

namespace {

struct ReadCtx {
    QString baseDir;
    QString texDir;      // extracted .usdz textures
    std::string warn;
};

QString resolveTexture(const QString& id, const ReadCtx& ctx)
{
    if (id.isEmpty()) return id;
    if (QFileInfo(id).isAbsolute() && QFileInfo::exists(id)) return id;
    for (const QString& dir : { ctx.baseDir, ctx.texDir }) {
        if (dir.isEmpty()) continue;
        const QString cand = QDir(dir).filePath(id);
        if (QFileInfo::exists(cand)) return cand;
        const QString byName = QDir(dir).filePath(QFileInfo(id).fileName());
        if (QFileInfo::exists(byName)) return byName;
    }
    return id;
}

template <typename T>
bool readVec3(const tydra::VertexAttribute& a, size_t i, T* out)
{
    if (a.empty() || a.format != tydra::VertexAttributeFormat::Vec3) return false;
    const size_t stride = a.stride_bytes();
    if ((i + 1) * stride > a.data.size()) return false;
    const float* f = reinterpret_cast<const float*>(a.data.data() + i * stride);
    out->x = f[0]; out->y = f[1]; out->z = f[2];
    return true;
}

bool readVec2(const tydra::VertexAttribute& a, size_t i, float* u, float* v)
{
    if (a.empty() || a.format != tydra::VertexAttributeFormat::Vec2) return false;
    const size_t stride = a.stride_bytes();
    if ((i + 1) * stride > a.data.size()) return false;
    const float* f = reinterpret_cast<const float*>(a.data.data() + i * stride);
    *u = f[0]; *v = f[1];
    return true;
}

// Index into a vertex attribute for (corner, vertex, face) under its variability.
size_t attrIndex(const tydra::VertexAttribute& a, size_t corner, size_t vertex, size_t face)
{
    if (a.is_indexed() && !a.indices.empty() && corner < a.indices.size())
        return a.indices[corner];
    if (a.is_facevarying()) return corner;
    if (a.is_constant()) return 0;
    if (a.is_uniform()) return face;
    return vertex;
}

aiMaterial* buildAiMaterial(const tydra::RenderScene& rs, const tydra::RenderMaterial& rm,
                            const ReadCtx& ctx)
{
    auto* m = new aiMaterial();
    aiString nm(rm.name.empty() ? "Material" : rm.name);
    m->AddProperty(&nm, AI_MATKEY_NAME);

    auto texturePath = [&](int texId) -> QString {
        if (texId < 0 || static_cast<size_t>(texId) >= rs.textures.size()) return {};
        const int64_t imgId = rs.textures[static_cast<size_t>(texId)].texture_image_id;
        if (imgId < 0 || static_cast<size_t>(imgId) >= rs.images.size()) return {};
        return resolveTexture(QString::fromStdString(
            rs.images[static_cast<size_t>(imgId)].asset_identifier), ctx);
    };

    aiColor3D diffuse(0.8f, 0.8f, 0.8f);
    if (rm.surfaceShader) {
        const auto& s = *rm.surfaceShader;
        if (!s.diffuseColor.is_texture())
            diffuse = aiColor3D(s.diffuseColor.value[0], s.diffuseColor.value[1],
                                s.diffuseColor.value[2]);
        else
            diffuse = aiColor3D(1, 1, 1);
        m->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);

        const float opacity = s.opacity.value;
        if (opacity < 1.0f && !s.opacity.is_texture())
            m->AddProperty(&opacity, 1, AI_MATKEY_OPACITY);
        const float metallic = s.metallic.value, roughness = s.roughness.value;
        m->AddProperty(&metallic, 1, AI_MATKEY_METALLIC_FACTOR);
        m->AddProperty(&roughness, 1, AI_MATKEY_ROUGHNESS_FACTOR);
        const aiColor3D emissive(s.emissiveColor.value[0], s.emissiveColor.value[1],
                                 s.emissiveColor.value[2]);
        if (emissive.r > 0 || emissive.g > 0 || emissive.b > 0)
            m->AddProperty(&emissive, 1, AI_MATKEY_COLOR_EMISSIVE);

        if (s.diffuseColor.is_texture()) {
            const QString p = texturePath(s.diffuseColor.texture_id);
            if (!p.isEmpty()) {
                aiString as(p.toStdString());
                m->AddProperty(&as, AI_MATKEY_TEXTURE(aiTextureType_DIFFUSE, 0));
                m->AddProperty(&as, AI_MATKEY_TEXTURE(aiTextureType_BASE_COLOR, 0));
            }
        }
        if (s.normal.is_texture()) {
            const QString p = texturePath(s.normal.texture_id);
            if (!p.isEmpty()) {
                aiString as(p.toStdString());
                m->AddProperty(&as, AI_MATKEY_TEXTURE(aiTextureType_NORMALS, 0));
            }
        }
    } else {
        m->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
    }
    return m;
}

aiMesh* buildAiMesh(const tydra::RenderScene& rs, const tydra::RenderMesh& rmesh,
                    const aiMatrix4x4& geomBindAi, const tydra::SkelHierarchy* skel,
                    const std::vector<std::string>& jointNames, ReadCtx& ctx)
{
    const auto& idx = rmesh.faceVertexIndices();
    const auto& cnt = rmesh.faceVertexCounts();
    const size_t nPoints = rmesh.points.size();
    if (nPoints == 0 || idx.empty() || cnt.empty()) return nullptr;

    // De-index per face corner only when some attribute is not per-vertex.
    const auto perVertex = [&](const tydra::VertexAttribute& a) {
        return a.empty() || (a.is_vertex() && a.indices.empty());
    };
    const bool direct = perVertex(rmesh.normals) &&
        (rmesh.texcoords.empty() ||
         std::all_of(rmesh.texcoords.begin(), rmesh.texcoords.end(),
                     [&](const auto& kv) { return perVertex(kv.second); }));

    auto* mesh = new aiMesh();
    mesh->mName = aiString(rmesh.display_name.empty() ? rmesh.prim_name : rmesh.display_name);
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;

    std::vector<unsigned> vertPoint;   // aiMesh vertex -> USD point
    std::vector<unsigned> cornerVert;  // face corner -> aiMesh vertex
    cornerVert.resize(idx.size());

    if (direct) {
        vertPoint.resize(nPoints);
        for (size_t p = 0; p < nPoints; ++p) vertPoint[p] = static_cast<unsigned>(p);
        for (size_t c = 0; c < idx.size(); ++c) cornerVert[c] = idx[c];
    } else {
        vertPoint.resize(idx.size());
        for (size_t c = 0; c < idx.size(); ++c) {
            vertPoint[c] = idx[c];
            cornerVert[c] = static_cast<unsigned>(c);
        }
    }

    const size_t nv = vertPoint.size();
    mesh->mNumVertices = static_cast<unsigned>(nv);
    mesh->mVertices = new aiVector3D[nv];
    const bool haveNormals = !rmesh.normals.empty();
    const bool haveUv = !rmesh.texcoords.empty() && rmesh.texcoords.count(0);
    if (haveNormals) mesh->mNormals = new aiVector3D[nv];
    if (haveUv) {
        mesh->mTextureCoords[0] = new aiVector3D[nv];
        mesh->mNumUVComponents[0] = 2;
    }

    // per-vertex face index for 'uniform' attributes (only in the corner path)
    std::vector<size_t> cornerFace(idx.size());
    {
        size_t c = 0;
        for (size_t f = 0; f < cnt.size(); ++f)
            for (uint32_t k = 0; k < cnt[f] && c < idx.size(); ++k) cornerFace[c++] = f;
    }

    for (size_t v = 0; v < nv; ++v) {
        const auto& pt = rmesh.points[vertPoint[v]];
        mesh->mVertices[v] = aiVector3D(pt[0], pt[1], pt[2]);
        const size_t corner = direct ? 0 : v;
        const size_t face = direct ? 0 : cornerFace[v];
        if (haveNormals) {
            aiVector3D n(0, 1, 0);
            readVec3(rmesh.normals, attrIndex(rmesh.normals, corner, vertPoint[v], face), &n);
            mesh->mNormals[v] = n;
        }
        if (haveUv) {
            float u = 0, w = 0;
            const auto& uvAttr = rmesh.texcoords.at(0);
            readVec2(uvAttr, attrIndex(uvAttr, corner, vertPoint[v], face), &u, &w);
            mesh->mTextureCoords[0][v] = aiVector3D(u, w, 0);
        }
    }

    // faces
    mesh->mNumFaces = static_cast<unsigned>(cnt.size());
    mesh->mFaces = new aiFace[cnt.size()];
    {
        size_t c = 0;
        for (size_t f = 0; f < cnt.size(); ++f) {
            aiFace& face = mesh->mFaces[f];
            face.mNumIndices = cnt[f];
            face.mIndices = new unsigned[cnt[f]];
            for (uint32_t k = 0; k < cnt[f]; ++k, ++c)
                face.mIndices[k] = c < cornerVert.size() ? cornerVert[c] : 0;
            if (cnt[f] != 3) mesh->mPrimitiveTypes |= aiPrimitiveType_POLYGON;
        }
    }
    if (mesh->mPrimitiveTypes & aiPrimitiveType_POLYGON)
        mesh->mPrimitiveTypes &= ~static_cast<unsigned>(aiPrimitiveType_TRIANGLE);

    mesh->mMaterialIndex = rmesh.material_id >= 0 ? static_cast<unsigned>(rmesh.material_id)
                                                  : static_cast<unsigned>(rs.materials.size());

    // ---- skinning ----
    const auto& jw = rmesh.joint_and_weights;
    if (skel && !jw.jointIndices.empty() && jw.elementSize > 0) {
        const size_t K = static_cast<size_t>(jw.elementSize);
        std::map<int, std::vector<aiVertexWeight>> perJoint;
        for (size_t v = 0; v < nv; ++v) {
            const size_t p = vertPoint[v];
            for (size_t k = 0; k < K; ++k) {
                const size_t at = p * K + k;
                if (at >= jw.jointIndices.size() || at >= jw.jointWeights.size()) break;
                const float w = jw.jointWeights[at];
                if (w <= 0.0f) continue;
                perJoint[jw.jointIndices[at]].push_back({ static_cast<unsigned>(v), w });
            }
        }
        std::vector<aiBone*> bones;
        for (auto& [joint, weights] : perJoint) {
            if (joint < 0 || static_cast<size_t>(joint) >= jointNames.size() ||
                static_cast<size_t>(joint) >= skel->bind_transforms.size())
                continue;
            auto* b = new aiBone();
            b->mName = aiString(jointNames[static_cast<size_t>(joint)]);
            // mesh space -> geomBind -> skeleton space -> joint space at bind
            aiMatrix4x4 bind = toAi(skel->bind_transforms[static_cast<size_t>(joint)]);
            b->mOffsetMatrix = aiMatrix4x4(bind).Inverse() * geomBindAi;
            b->mNumWeights = static_cast<unsigned>(weights.size());
            b->mWeights = new aiVertexWeight[weights.size()];
            std::copy(weights.begin(), weights.end(), b->mWeights);
            bones.push_back(b);
        }
        if (!bones.empty()) {
            mesh->mNumBones = static_cast<unsigned>(bones.size());
            mesh->mBones = new aiBone*[bones.size()];
            std::copy(bones.begin(), bones.end(), mesh->mBones);
        }
    }

    // ---- blend shapes ----
    if (!rmesh.targets.empty()) {
        std::vector<aiAnimMesh*> shapes;
        for (const auto& [primName, target] : rmesh.targets) {
            auto* am = new aiAnimMesh();
            am->mName = aiString(target.display_name.empty() ? primName : target.display_name);
            am->mNumVertices = static_cast<unsigned>(nv);
            am->mVertices = new aiVector3D[nv];
            for (size_t v = 0; v < nv; ++v) am->mVertices[v] = mesh->mVertices[v];

            // offsets are per USD point (sparse when pointIndices is present)
            std::vector<aiVector3D> perPoint(nPoints, aiVector3D(0, 0, 0));
            const bool sparse = !target.pointIndices.empty();
            const size_t n = target.pointOffsets.size();
            for (size_t i = 0; i < n; ++i) {
                const size_t p = sparse ? target.pointIndices[i] : i;
                if (p >= nPoints) continue;
                const auto& o = target.pointOffsets[i];
                perPoint[p] = aiVector3D(o[0], o[1], o[2]);
            }
            for (size_t v = 0; v < nv; ++v)
                am->mVertices[v] += perPoint[vertPoint[v]];
            am->mWeight = 1.0f;
            shapes.push_back(am);
        }
        if (!shapes.empty()) {
            mesh->mNumAnimMeshes = static_cast<unsigned>(shapes.size());
            mesh->mAnimMeshes = new aiAnimMesh*[shapes.size()];
            std::copy(shapes.begin(), shapes.end(), mesh->mAnimMeshes);
        }
    }
    return mesh;
}

void addChild(aiNode* parent, aiNode* child)
{
    std::vector<aiNode*> kids(parent->mChildren, parent->mChildren + parent->mNumChildren);
    kids.push_back(child);
    delete[] parent->mChildren;
    parent->mChildren = new aiNode*[kids.size()];
    std::copy(kids.begin(), kids.end(), parent->mChildren);
    parent->mNumChildren = static_cast<unsigned>(kids.size());
    child->mParent = parent;
}

void collectJointNames(const tydra::SkelNode& n, std::vector<std::string>& names)
{
    if (n.joint_id >= 0) {
        if (names.size() <= static_cast<size_t>(n.joint_id)) names.resize(n.joint_id + 1);
        std::string nm = n.joint_name;
        if (nm.empty()) {
            const size_t slash = n.joint_path.find_last_of('/');
            nm = slash == std::string::npos ? n.joint_path : n.joint_path.substr(slash + 1);
        }
        names[static_cast<size_t>(n.joint_id)] = nm;
    }
    for (const auto& c : n.children) collectJointNames(c, names);
}

struct NodeBuild {
    const tydra::RenderScene* rs{};
    std::vector<aiNode*> flat;                    // pre-order, matches animation target_node
    std::map<std::string, aiNode*> byPath;
    std::vector<std::vector<unsigned>> meshesOfNode;  // by flat index
};

aiNode* buildNode(const tydra::Node& n, NodeBuild& nb)
{
    auto* an = new aiNode(n.display_name.empty() ? n.prim_name : n.display_name);
    an->mTransformation = toAi(n.local_matrix);
    nb.flat.push_back(an);
    nb.byPath[n.abs_path] = an;
    if (n.nodeType == tydra::NodeType::Mesh && n.id >= 0) {
        an->mNumMeshes = 1;
        an->mMeshes = new unsigned[1];
        an->mMeshes[0] = static_cast<unsigned>(n.id);
    }
    for (const auto& c : n.children)
        addChild(an, buildNode(c, nb));
    return an;
}

}  // namespace

bool fillSceneFromUsd(const QString& path, aiScene* out, QString* err, QString* warnOut)
{
    auto fail = [&](const QString& m) {
        if (err) *err = m;
        return false;
    };
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
        QStringLiteral("import format=usd %1").arg(QFileInfo(path).fileName()));
    if (!QFileInfo::exists(path)) return fail(QStringLiteral("file not found: %1").arg(path));

    Stage stage;
    std::string warn, e;
    USDLoadOptions lopts;
    if (!LoadUSDFromFile(path.toStdString(), &stage, &warn, &e, lopts))
        return fail(QStringLiteral("USD load failed: %1").arg(QString::fromStdString(e)));

    ReadCtx ctx;
    ctx.baseDir = QFileInfo(path).absolutePath();

    // .usdz keeps its textures inside the package; the importers reference
    // textures by file, so unpack the images next to a temp location.
    if (QFileInfo(path).suffix().compare(QLatin1String("usdz"), Qt::CaseInsensitive) == 0) {
        USDZAsset pkg;
        std::string w2, e2;
        if (ReadUSDZAssetInfoFromFile(path.toStdString(), &pkg, &w2, &e2)) {
            const QByteArray hash = QCryptographicHash::hash(
                QFileInfo(path).absoluteFilePath().toUtf8(), QCryptographicHash::Sha1).toHex();
            ctx.texDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                         QStringLiteral("/qtmesh_usdz_") + QString::fromLatin1(hash.left(12));
            QDir().mkpath(ctx.texDir);
            for (const auto& [name, range] : pkg.asset_map) {
                const QString q = QString::fromStdString(name);
                const QString lower = q.toLower();
                if (!(lower.endsWith(".png") || lower.endsWith(".jpg") ||
                      lower.endsWith(".jpeg") || lower.endsWith(".tga") ||
                      lower.endsWith(".bmp") || lower.endsWith(".exr")))
                    continue;
                // never write outside texDir (the name comes from the package)
                const QString clean = QDir::cleanPath(q);
                if (clean.startsWith("..") || QFileInfo(clean).isAbsolute()) continue;
                const QString target = QDir(ctx.texDir).filePath(clean);
                QDir().mkpath(QFileInfo(target).absolutePath());
                QFile f(target);
                if (f.open(QIODevice::WriteOnly)) {
                    const uint8_t* base = pkg.is_mmaped() ? pkg.addr : pkg.data.data();
                    f.write(reinterpret_cast<const char*>(base + range.first),
                            static_cast<qint64>(range.second - range.first));
                }
            }
        }
    }

    tydra::RenderSceneConverter converter;
    tydra::RenderSceneConverterEnv env(stage);
    env.set_search_paths({ ctx.baseDir.toStdString() });
    env.usd_filename = path.toStdString();
    env.scene_config.load_texture_assets = false;
    env.mesh_config.triangulate = true;
    tydra::RenderScene rs;
    if (!converter.ConvertToRenderScene(env, &rs))
        return fail(QStringLiteral("USD conversion failed: %1")
                        .arg(QString::fromStdString(converter.GetError())));
    warn += converter.GetWarning();

    // ---- node tree ----
    NodeBuild nb;
    nb.rs = &rs;
    auto* root = new aiNode(QFileInfo(path).completeBaseName().toStdString());
    out->mRootNode = root;
    for (const auto& n : rs.nodes)
        addChild(root, buildNode(n, nb));

    // Y-up / metres: the editor works Y-up in metres, so a Z-up or
    // centimetre stage is brought over through the root transform.
    {
        aiMatrix4x4 rootXf;
        const Axis up = stage.metas().upAxis.get_value();
        if (up == Axis::Z) {
            aiMatrix4x4 rot;
            aiMatrix4x4::RotationX(-static_cast<ai_real>(M_PI) / 2, rot);
            rootXf = rot * rootXf;
        }
        const double mpu = stage.metas().metersPerUnit.get_value();
        if (mpu > 0.0 && std::fabs(mpu - 1.0) > 1e-9) {
            aiMatrix4x4 sc;
            aiMatrix4x4::Scaling(aiVector3D(static_cast<ai_real>(mpu)), sc);
            rootXf = sc * rootXf;
        }
        root->mTransformation = rootXf;
    }

    // ---- skeletons: joint nodes ----
    std::vector<std::vector<aiNode*>> jointNodes(rs.skeletons.size());
    std::vector<std::vector<std::string>> jointNameTable(rs.skeletons.size());
    std::set<std::string> usedNames;
    for (const auto& n : nb.flat) usedNames.insert(n->mName.C_Str());
    for (size_t s = 0; s < rs.skeletons.size(); ++s) {
        const auto& sk = rs.skeletons[s];
        collectJointNames(sk.root_node, jointNameTable[s]);
        const size_t nj = sk.parent_joint_indices.size();
        jointNameTable[s].resize(nj);
        // Verbatim names written by the exporter (see writeSceneImpl).
        {
            const Prim* sp = nullptr;
            std::string perr;
            if (stage.find_prim_at_path(Path(sk.abs_path, ""), sp, &perr) && sp)
                if (const Skeleton* sd = sp->as<Skeleton>()) {
                    auto it = sd->props.find("qtmesh:jointNames");
                    if (it != sd->props.end()) {
                        auto names = it->second.get_attribute().get_value<std::vector<value::token>>();
                        if (names && names->size() == nj)
                            for (size_t j = 0; j < nj; ++j)
                                jointNameTable[s][j] = (*names)[j].str();
                    }
                }
        }
        jointNodes[s].assign(nj, nullptr);
        for (size_t j = 0; j < nj; ++j) {
            std::string nm = jointNameTable[s][j].empty() ? "joint" + std::to_string(j)
                                                           : jointNameTable[s][j];
            std::string base = nm;
            for (int i = 1; usedNames.count(nm); ++i) nm = base + "_" + std::to_string(i);
            usedNames.insert(nm);
            jointNameTable[s][j] = nm;
            auto* jn = new aiNode(nm);
            if (j < sk.rest_transforms.size()) jn->mTransformation = toAi(sk.rest_transforms[j]);
            jointNodes[s][j] = jn;
        }
        aiNode* skelNode = nullptr;
        auto it = nb.byPath.find(sk.abs_path);
        if (it != nb.byPath.end()) skelNode = it->second;
        if (!skelNode) skelNode = root;
        for (size_t j = 0; j < nj; ++j) {
            const int parent = sk.parent_joint_indices[j];
            if (parent >= 0 && static_cast<size_t>(parent) < nj)
                addChild(jointNodes[s][static_cast<size_t>(parent)], jointNodes[s][j]);
            else
                addChild(skelNode, jointNodes[s][j]);
        }
    }

    // ---- materials ----
    std::vector<aiMaterial*> mats;
    for (const auto& rm : rs.materials) mats.push_back(buildAiMaterial(rs, rm, ctx));
    bool needDefault = false;

    // ---- meshes ----
    std::vector<aiMesh*> meshes;
    std::map<size_t, size_t> meshRemap;  // USD mesh index -> aiScene mesh index
    for (size_t i = 0; i < rs.meshes.size(); ++i) {
        const auto& rm = rs.meshes[i];
        const tydra::SkelHierarchy* skel = nullptr;
        std::vector<std::string> names;
        if (rm.skel_id >= 0 && static_cast<size_t>(rm.skel_id) < rs.skeletons.size()) {
            skel = &rs.skeletons[static_cast<size_t>(rm.skel_id)];
            names = jointNameTable[static_cast<size_t>(rm.skel_id)];
        }
        aiMatrix4x4 geomBind = toAi(rm.joint_and_weights.geomBindTransform);
        aiMesh* am = buildAiMesh(rs, rm, geomBind, skel, names, ctx);
        if (!am) continue;
        if (rm.material_id < 0) needDefault = true;
        meshRemap[i] = meshes.size();
        meshes.push_back(am);
        // A skinned mesh is placed by its skeleton, not by its own transform.
        if (am->mNumBones > 0) {
            // detach from its original node, hang on the scene root
            for (auto* n : nb.flat)
                for (unsigned k = 0; k < n->mNumMeshes; ++k)
                    if (n->mMeshes[k] == i) {
                        n->mNumMeshes = 0;
                    }
            auto* holder = new aiNode(std::string(am->mName.C_Str()) + "_skinned");
            holder->mNumMeshes = 1;
            holder->mMeshes = new unsigned[1];
            holder->mMeshes[0] = static_cast<unsigned>(meshes.size() - 1);
            addChild(root, holder);
        }
    }
    if (meshes.empty())
        return fail(QStringLiteral("the USD file contains no polygon meshes"));
    if (needDefault) {
        auto* dm = new aiMaterial();
        aiString nm("DefaultMaterial");
        dm->AddProperty(&nm, AI_MATKEY_NAME);
        mats.push_back(dm);
        for (auto* am : meshes)
            if (am->mMaterialIndex >= rs.materials.size())
                am->mMaterialIndex = static_cast<unsigned>(mats.size() - 1);
    }
    // Remap mesh indices on the (non-skinned) nodes.
    for (auto* n : nb.flat)
        for (unsigned k = 0; k < n->mNumMeshes; ++k) {
            auto it = meshRemap.find(n->mMeshes[k]);
            n->mMeshes[k] = it != meshRemap.end() ? static_cast<unsigned>(it->second) : 0;
        }

    out->mNumMeshes = static_cast<unsigned>(meshes.size());
    out->mMeshes = new aiMesh*[meshes.size()];
    std::copy(meshes.begin(), meshes.end(), out->mMeshes);
    out->mNumMaterials = static_cast<unsigned>(mats.size());
    out->mMaterials = new aiMaterial*[mats.size()];
    std::copy(mats.begin(), mats.end(), out->mMaterials);

    // ---- animations ----
    std::vector<aiAnimation*> anims;
    for (const auto& clip : rs.animations) {
        struct PerNode {
            aiNode* node{};
            const tydra::KeyframeSampler* t{};
            const tydra::KeyframeSampler* r{};
            const tydra::KeyframeSampler* s{};
            double timeScale{1.0};  // Tydra keeps USD time codes; the aiScene wants seconds
        };
        std::map<aiNode*, PerNode> perNode;
        bool droppedWeights = false;
        for (const auto& ch : clip.channels) {
            if (ch.sampler < 0 || static_cast<size_t>(ch.sampler) >= clip.samplers.size()) continue;
            if (ch.path == tydra::AnimationPath::Weights ||
                ch.path == tydra::AnimationPath::CustomProperty) {
                droppedWeights = true;
                continue;
            }
            aiNode* target = nullptr;
            if (ch.is_skeletal()) {
                if (ch.skeleton_id >= 0 && static_cast<size_t>(ch.skeleton_id) < jointNodes.size() &&
                    ch.joint_id >= 0 &&
                    static_cast<size_t>(ch.joint_id) < jointNodes[static_cast<size_t>(ch.skeleton_id)].size())
                    target = jointNodes[static_cast<size_t>(ch.skeleton_id)][static_cast<size_t>(ch.joint_id)];
            } else if (ch.target_node >= 0 && static_cast<size_t>(ch.target_node) < nb.flat.size()) {
                target = nb.flat[static_cast<size_t>(ch.target_node)];
            }
            if (!target) continue;
            PerNode& pn = perNode[target];
            pn.node = target;
            {
                const double tcps = stage.metas().timeCodesPerSecond.get_value();
                pn.timeScale = tcps > 0.0 ? 1.0 / tcps : 1.0;
            }
            const auto* smp = &clip.samplers[static_cast<size_t>(ch.sampler)];
            if (ch.path == tydra::AnimationPath::Translation) pn.t = smp;
            else if (ch.path == tydra::AnimationPath::Rotation) pn.r = smp;
            else if (ch.path == tydra::AnimationPath::Scale) pn.s = smp;
        }
        if (droppedWeights)
            warn += "blend-shape weight animation in '" + clip.name + "' was not imported\n";
        if (perNode.empty()) continue;

        auto* anim = new aiAnimation();
        anim->mName = aiString(clip.display_name.empty()
                                   ? (clip.name.empty() ? clip.prim_name : clip.name)
                                   : clip.display_name);
        anim->mTicksPerSecond = 1.0;
        const double clipTcps = stage.metas().timeCodesPerSecond.get_value();
        double duration = clipTcps > 0.0 ? clip.duration / clipTcps : clip.duration;
        anim->mNumChannels = static_cast<unsigned>(perNode.size());
        anim->mChannels = new aiNodeAnim*[perNode.size()];
        unsigned ci = 0;
        for (auto& [node, pn] : perNode) {
            auto* na = new aiNodeAnim();
            na->mNodeName = node->mName;
            aiVector3D rs0, rt0;
            aiQuaternion rr0;
            node->mTransformation.Decompose(rs0, rr0, rt0);

            auto fillVec = [&](const tydra::KeyframeSampler* s, const aiVector3D& rest,
                               unsigned* n, aiVectorKey** keys) {
                if (s && !s->times.empty() && s->values.size() >= s->times.size() * 3) {
                    *n = static_cast<unsigned>(s->times.size());
                    *keys = new aiVectorKey[*n];
                    for (unsigned i = 0; i < *n; ++i) {
                        (*keys)[i].mTime = s->times[i] * pn.timeScale;
                        (*keys)[i].mValue = aiVector3D(s->values[i * 3], s->values[i * 3 + 1],
                                                       s->values[i * 3 + 2]);
                        duration = std::max(duration, static_cast<double>(s->times[i]) * pn.timeScale);
                    }
                } else {
                    *n = 1;
                    *keys = new aiVectorKey[1];
                    (*keys)[0].mTime = 0.0;
                    (*keys)[0].mValue = rest;
                }
            };
            fillVec(pn.t, rt0, &na->mNumPositionKeys, &na->mPositionKeys);
            fillVec(pn.s, rs0, &na->mNumScalingKeys, &na->mScalingKeys);
            if (pn.r && !pn.r->times.empty() && pn.r->values.size() >= pn.r->times.size() * 4) {
                na->mNumRotationKeys = static_cast<unsigned>(pn.r->times.size());
                na->mRotationKeys = new aiQuatKey[na->mNumRotationKeys];
                for (unsigned i = 0; i < na->mNumRotationKeys; ++i) {
                    na->mRotationKeys[i].mTime = pn.r->times[i] * pn.timeScale;
                    // values are x,y,z,w
                    na->mRotationKeys[i].mValue = aiQuaternion(
                        pn.r->values[i * 4 + 3], pn.r->values[i * 4], pn.r->values[i * 4 + 1],
                        pn.r->values[i * 4 + 2]);
                    duration = std::max(duration, static_cast<double>(pn.r->times[i]) * pn.timeScale);
                }
            } else {
                na->mNumRotationKeys = 1;
                na->mRotationKeys = new aiQuatKey[1];
                na->mRotationKeys[0].mTime = 0.0;
                na->mRotationKeys[0].mValue = rr0;
            }
            anim->mChannels[ci++] = na;
        }
        anim->mDuration = duration;
        anims.push_back(anim);
    }
    if (!anims.empty()) {
        out->mNumAnimations = static_cast<unsigned>(anims.size());
        out->mAnimations = new aiAnimation*[anims.size()];
        std::copy(anims.begin(), anims.end(), out->mAnimations);
    }

    if (warnOut) *warnOut = QString::fromStdString(warn + ctx.warn);
    return true;
}

aiScene* readScene(const QString& path, QString* err, QString* warn)
{
    auto* scene = new aiScene();
    if (!fillSceneFromUsd(path, scene, err, warn)) {
        delete scene;
        return nullptr;
    }
    return scene;
}

// ─── Assimp plumbing ────────────────────────────────────────────────────────

namespace {

class UsdAssimpImporter : public Assimp::BaseImporter {
public:
    bool CanRead(const std::string& file, Assimp::IOSystem*, bool) const override
    {
        return isUsdPath(QString::fromStdString(file));
    }
    const aiImporterDesc* GetInfo() const override
    {
        static const aiImporterDesc desc = {
            "Universal Scene Description (LightUSD)", "", "", "",
            aiImporterFlags_SupportTextFlavour | aiImporterFlags_SupportBinaryFlavour,
            0, 0, 0, 0, "usd usda usdc usdz"
        };
        return &desc;
    }
protected:
    void InternReadFile(const std::string& file, aiScene* scene, Assimp::IOSystem*) override
    {
        QString err;
        if (!fillSceneFromUsd(QString::fromStdString(file), scene, &err, nullptr))
            throw DeadlyImportError(err.toStdString());
    }
};

void exportUsd(const char* file, Assimp::IOSystem*, const aiScene* scene,
               const Assimp::ExportProperties*)
{
    QString err;
    if (!writeSceneImpl(QString::fromUtf8(file), scene, &err, nullptr))
        throw DeadlyExportError(err.toStdString());
}

}  // namespace

void registerImporter(Assimp::Importer& importer)
{
    importer.RegisterLoader(new UsdAssimpImporter());
}

void registerExporter(Assimp::Exporter& exporter)
{
    exporter.RegisterExporter(Assimp::Exporter::ExportFormatEntry(
        "usda", "USD ASCII (LightUSD)", "usda", &exportUsd));
    exporter.RegisterExporter(Assimp::Exporter::ExportFormatEntry(
        "usdc", "USD Crate (LightUSD)", "usdc", &exportUsd));
    exporter.RegisterExporter(Assimp::Exporter::ExportFormatEntry(
        "usd", "USD (LightUSD)", "usd", &exportUsd));
    exporter.RegisterExporter(Assimp::Exporter::ExportFormatEntry(
        "usdz", "USD Package (LightUSD)", "usdz", &exportUsd));
}

#endif  // ENABLE_USD

}  // namespace UsdBridge
