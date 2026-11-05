// Hair Studio import: split a model file into hairstyles and register them. Uses Assimp (the project's importer) directly:
// the styles are kept as authored (positions in the source's units, per-vertex UVs split on UV seams); fitting happens later.

#include "HairLibrary.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImage>
#include <QPainter>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>
#include <map>

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
    std::vector<TexturePart> ranges;
    std::map<unsigned, QByteArray> pngByMaterial;
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
        ranges.push_back({base, m->mNumVertices, m->mMaterialIndex});
        if (pngByMaterial.find(m->mMaterialIndex) == pngByMaterial.end()) pngByMaterial[m->mMaterialIndex] = texturePngFor(scene, m, baseDir);
    }
    assignTextures(h, ranges, pngByMaterial);
    return h;
}

}  // namespace

// The vertices one part of a style occupies in the merged mesh, and the material it came from.
// A style whose parts use different materials is rendered as ONE mesh with ONE texture: stack the parts' textures into an atlas (all scaled
// to a common width) and remap each part's UVs into its own band. A part without a texture gets a flat opaque tile, so it renders as hair
// colour instead of sampling somebody else's alpha. A single texture (the usual case) is kept untouched.
void assignTextures(ImportedHair& h, const std::vector<TexturePart>& ranges, const std::map<unsigned, QByteArray>& pngByMaterial)
{
    std::vector<unsigned> materials;
    for (const TexturePart& r : ranges)
        if (std::find(materials.begin(), materials.end(), r.material) == materials.end()) materials.push_back(r.material);
    if (materials.size() <= 1) {
        for (const auto& kv : pngByMaterial)
            if (!kv.second.isEmpty()) { h.texturePng = kv.second; break; }
        return;
    }
    constexpr int kFlatHeight = 16, kMaxWidth = 2048, kMaxHeight = 4096;
    std::map<unsigned, QImage> images;
    int width = 16;
    for (unsigned m : materials) {
        QImage img;
        const auto it = pngByMaterial.find(m);
        if (it != pngByMaterial.end() && !it->second.isEmpty()) img = QImage::fromData(it->second, "PNG").convertToFormat(QImage::Format_RGBA8888);
        images[m] = img;
        width = std::max(width, img.width());
    }
    width = std::min(width, kMaxWidth);
    std::map<unsigned, std::pair<int, int>> band;       // material -> (first row, rows)
    int height = 0;
    for (unsigned m : materials) {
        QImage& img = images[m];
        if (img.isNull()) {
            img = QImage(width, kFlatHeight, QImage::Format_RGBA8888);
            img.fill(QColor(128, 128, 128, 255));
        } else {
            img = img.scaledToWidth(width, Qt::SmoothTransformation);
        }
        band[m] = {height, img.height()};
        height += img.height();
    }
    QImage atlas(width, height, QImage::Format_RGBA8888);
    atlas.fill(Qt::transparent);
    {
        QPainter painter(&atlas);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        for (unsigned m : materials) painter.drawImage(0, band[m].first, images[m]);
    }
    for (const TexturePart& r : ranges) {
        const auto [row0, rows] = band[r.material];
        for (uint32_t v = r.first; v < r.first + r.count; ++v) {
            const auto wrap = [](float x) { return x - std::floor(x) < 1e-6f && x > 0.0f ? 1.0f : x - std::floor(x); };   // repeat, keeping 1.0 as 1.0
            const float u = wrap(h.uv[size_t(v) * 2]);
            const float vv = wrap(h.uv[size_t(v) * 2 + 1]);
            // inset by half a texel inside the band: a UV on the band's border would blend the neighbouring part's texture in (bilinear filtering)
            h.uv[size_t(v) * 2] = (0.5f + u * float(width - 1)) / float(width);
            h.uv[size_t(v) * 2 + 1] = 1.0f - (float(row0) + 0.5f + (1.0f - vv) * float(rows - 1)) / float(height);     // v is up in the file, image rows run down
        }
    }
    if (height > kMaxHeight) atlas = atlas.scaled(width * kMaxHeight / height, kMaxHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    atlas.save(&buf, "PNG");
    h.texturePng = bytes;
}

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
