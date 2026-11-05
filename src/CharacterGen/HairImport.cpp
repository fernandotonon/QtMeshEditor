// Hair Studio import: split a model file into hairstyles and register them. Uses Assimp (the project's importer) directly:
// the styles are kept as authored (positions in the source's units, per-vertex UVs split on UV seams); fitting happens later.

#include "HairLibrary.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImage>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>

namespace CharacterGen {
namespace Hair {
namespace Library {

namespace {

const QStringList kExtensions = {QStringLiteral("fbx"), QStringLiteral("obj"), QStringLiteral("glb"), QStringLiteral("gltf"), QStringLiteral("dae"), QStringLiteral("blend"), QStringLiteral("3ds"), QStringLiteral("ply")};

struct Group {
    QString name;
    std::vector<std::pair<const aiMesh*, aiMatrix4x4>> parts;
};

aiMatrix4x4 worldOf(const aiNode* n)
{
    aiMatrix4x4 m;
    for (const aiNode* p = n; p; p = p->mParent) m = p->mTransformation * m;
    return m;
}

void collect(const aiScene* scene, const aiNode* n, std::vector<std::pair<const aiMesh*, aiMatrix4x4>>& out)
{
    const aiMatrix4x4 w = worldOf(n);
    for (unsigned i = 0; i < n->mNumMeshes; ++i)
        if (const aiMesh* m = scene->mMeshes[n->mMeshes[i]]) out.emplace_back(m, w);
    for (unsigned c = 0; c < n->mNumChildren; ++c) collect(scene, n->mChildren[c], out);
}

bool hasGeometry(const aiScene* scene, const aiNode* n)
{
    if (n->mNumMeshes > 0) return true;
    for (unsigned c = 0; c < n->mNumChildren; ++c)
        if (hasGeometry(scene, n->mChildren[c])) return true;
    return false;
}

// One group per hairstyle: descend through pass-through nodes; then each geometry-carrying child is a style, else each mesh.
std::vector<Group> splitIntoGroups(const aiScene* scene)
{
    std::vector<Group> groups;
    const aiNode* node = scene->mRootNode;
    while (node && node->mNumMeshes == 0 && node->mNumChildren == 1) node = node->mChildren[0];
    if (!node) return groups;
    std::vector<const aiNode*> styleNodes;
    for (unsigned c = 0; c < node->mNumChildren; ++c)
        if (hasGeometry(scene, node->mChildren[c])) styleNodes.push_back(node->mChildren[c]);
    if (styleNodes.size() > 1 || (styleNodes.size() == 1 && node->mNumMeshes == 0)) {
        for (const aiNode* s : styleNodes) {
            Group g;
            g.name = QString::fromUtf8(s->mName.C_Str());
            collect(scene, s, g.parts);
            groups.push_back(std::move(g));
        }
        return groups;
    }
    std::vector<std::pair<const aiMesh*, aiMatrix4x4>> all;
    collect(scene, node, all);
    if (all.size() > 1 && node->mNumChildren == 0) {
        for (const auto& part : all) {
            Group g;
            g.name = QString::fromUtf8(part.first->mName.C_Str());
            g.parts.push_back(part);
            groups.push_back(std::move(g));
        }
        return groups;
    }
    Group g;
    g.name = QString::fromUtf8(node->mName.C_Str());
    g.parts = std::move(all);
    groups.push_back(std::move(g));
    return groups;
}

QImage loadTexture(const aiScene* scene, const aiMaterial* mat, aiTextureType type, const QString& baseDir)
{
    aiString path;
    if (!mat || mat->GetTexture(type, 0, &path) != AI_SUCCESS) return QImage();
    if (const aiTexture* emb = scene->GetEmbeddedTexture(path.C_Str())) {
        if (emb->mHeight == 0) return QImage::fromData(reinterpret_cast<const uchar*>(emb->pcData), int(emb->mWidth));
        QImage img(int(emb->mWidth), int(emb->mHeight), QImage::Format_RGBA8888);
        for (unsigned y = 0; y < emb->mHeight; ++y)
            for (unsigned x = 0; x < emb->mWidth; ++x) {
                const aiTexel& t = emb->pcData[y * emb->mWidth + x];
                img.setPixel(int(x), int(y), qRgba(t.r, t.g, t.b, t.a));
            }
        return img;
    }
    const QString rel = QString::fromUtf8(path.C_Str()).replace(QLatin1Char('\\'), QLatin1Char('/'));
    for (const QString& cand : {baseDir + QLatin1Char('/') + rel, baseDir + QLatin1Char('/') + QFileInfo(rel).fileName(), baseDir + QStringLiteral("/textures/") + QFileInfo(rel).fileName()}) {
        QImage img(cand);
        if (!img.isNull()) return img;
    }
    return QImage();
}

QByteArray texturePngFor(const aiScene* scene, const aiMesh* mesh, const QString& baseDir)
{
    if (mesh->mMaterialIndex >= scene->mNumMaterials) return QByteArray();
    const aiMaterial* mat = scene->mMaterials[mesh->mMaterialIndex];
    QImage img = loadTexture(scene, mat, aiTextureType_BASE_COLOR, baseDir);
    if (img.isNull()) img = loadTexture(scene, mat, aiTextureType_DIFFUSE, baseDir);
    if (img.isNull()) return QByteArray();
    img = img.convertToFormat(QImage::Format_RGBA8888);
    // a separate opacity map (common for hair cards) goes into the alpha channel
    const QImage opacity = loadTexture(scene, mat, aiTextureType_OPACITY, baseDir);
    if (!opacity.isNull()) {
        const QImage o = opacity.scaled(img.size()).convertToFormat(QImage::Format_Grayscale8);
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                const QRgb c = img.pixel(x, y);
                img.setPixel(x, y, qRgba(qRed(c), qGreen(c), qBlue(c), qGray(o.pixel(x, y))));
            }
    }
    if (img.width() > 2048 || img.height() > 2048) img = img.scaled(2048, 2048, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

ImportedHair buildHair(const aiScene* scene, const Group& g, const QString& baseDir)
{
    ImportedHair h;
    h.name = g.name;
    for (const auto& part : g.parts) {
        const aiMesh* m = part.first;
        if (!m->HasPositions() || m->mNumFaces == 0) continue;
        const uint32_t base = uint32_t(h.pos.size());
        for (unsigned v = 0; v < m->mNumVertices; ++v) {
            const aiVector3D p = part.second * m->mVertices[v];
            h.pos.push_back({p.x, p.y, p.z});
            const bool hasUv = m->HasTextureCoords(0);
            h.uv.push_back(hasUv ? m->mTextureCoords[0][v].x : 0.0f);
            h.uv.push_back(hasUv ? m->mTextureCoords[0][v].y : 0.0f);
        }
        for (unsigned f = 0; f < m->mNumFaces; ++f) {
            const aiFace& face = m->mFaces[f];
            if (face.mNumIndices != 3) continue;
            h.idx.push_back(base + face.mIndices[0]);
            h.idx.push_back(base + face.mIndices[1]);
            h.idx.push_back(base + face.mIndices[2]);
        }
        if (h.texturePng.isEmpty()) h.texturePng = texturePngFor(scene, m, baseDir);
    }
    return h;
}

}  // namespace

QList<ImportedHair> extractFromFile(const QString& path, QStringList* errors)
{
    QList<ImportedHair> out;
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path.toStdString().c_str(), aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_SortByPType | aiProcess_RemoveRedundantMaterials);
    if (!scene || !scene->mRootNode) {
        if (errors) *errors << QStringLiteral("%1: %2").arg(path, QString::fromUtf8(importer.GetErrorString()));
        return out;
    }
    const QString baseDir = QFileInfo(path).absolutePath();
    for (const Group& g : splitIntoGroups(scene)) {
        ImportedHair h = buildHair(scene, g, baseDir);
        if (h.pos.empty() || h.idx.size() < 3) continue;
        out.append(std::move(h));
    }
    if (out.isEmpty() && errors) *errors << QStringLiteral("%1: no triangle geometry found").arg(path);
    return out;
}

ImportResult importFile(const QString& path, const ImportOptions& opt)
{
    ImportResult r;
    QStringList errors;
    const QList<ImportedHair> styles = extractFromFile(path, &errors);
    r.errors = errors;
    if (styles.isEmpty()) { r.failed = 1; return r; }
    const QString collection = opt.collection.isEmpty() ? QFileInfo(path).completeBaseName() : opt.collection;
    for (const ImportedHair& h : styles) {
        ++r.found;
        Meta m;
        m.name = h.name.isEmpty() ? collection : h.name;
        m.author = opt.author; m.license = opt.license; m.attribution = opt.attribution; m.collection = collection;
        m.source = opt.source.isEmpty() ? path : opt.source;
        QString id, dup, err;
        if (!save(h, m, &id, &dup, &err)) { ++r.failed; r.errors << err; continue; }
        if (id.isEmpty()) { ++r.duplicates; continue; }
        ++r.added;
        r.ids << id;
    }
    return r;
}

ImportResult importFolder(const QString& folder, const ImportOptions& opt)
{
    ImportResult total;
    QDirIterator it(folder, QDir::Files, opt.recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        const QString f = it.next();
        if (!kExtensions.contains(QFileInfo(f).suffix().toLower())) continue;
        const ImportResult r = importFile(f, opt);
        total.found += r.found; total.added += r.added; total.duplicates += r.duplicates; total.failed += r.failed;
        total.ids << r.ids; total.errors << r.errors;
    }
    return total;
}

}  // namespace Library
}  // namespace Hair
}  // namespace CharacterGen
