/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "PS1/PS1PLY.h"

#include "EditableMesh.h"

#include <OgreHardwareBufferManager.h>
#include <OgreLogManager.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgrePass.h>
#include <OgreSubMesh.h>
#include <OgreTechnique.h>
#include <OgreVertexIndexData.h>

#include <QColor>
#include <QFile>
#include <QRegularExpression>
#include <QStringConverter>
#include <QTextStream>

#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct TriSoup {
    std::vector<Ogre::Vector3> pos;
    std::vector<Ogre::Vector3> nrm;
    std::vector<Ogre::RGBA> col; // optional; if present must match pos size
};

static int32_t quantizeWorld(Ogre::Real v)
{
    return static_cast<int32_t>(std::lround(double(v) * 100000.0));
}

struct WeldKey {
    int32_t px, py, pz, nx, ny, nz;
    int32_t crgba;

    bool operator==(const WeldKey& o) const
    {
        return px == o.px && py == o.py && pz == o.pz && nx == o.nx && ny == o.ny && nz == o.nz && crgba == o.crgba;
    }
};

struct WeldKeyHash {
    size_t operator()(const WeldKey& k) const noexcept
    {
        size_t h = 1469598103934665603ull;
        auto mix = [&](int32_t x) {
            h ^= static_cast<size_t>(static_cast<uint32_t>(x)) * 1099511628211ull;
        };
        mix(k.px);
        mix(k.py);
        mix(k.pz);
        mix(k.nx);
        mix(k.ny);
        mix(k.nz);
        mix(k.crgba);
        return h;
    }
};

static void applyPlyImportWorldTransform(Ogre::Vector3& p)
{
    p *= PS1PLY::kPsyqPlyEditorUniformScale;
    p.x = -p.x;
    p.y = -p.y;
}

static void applyPlyImportWorldTransformNormal(Ogre::Vector3& n)
{
    if (n.isZeroLength())
        return;
    n.normalise();
    n.x = -n.x;
    n.y = -n.y;
}

static void appendCorner(TriSoup& out, const Ogre::Vector3& p, const Ogre::Vector3& n)
{
    Ogre::Vector3 pp = p;
    Ogre::Vector3 nn = n;
    applyPlyImportWorldTransform(pp);
    applyPlyImportWorldTransformNormal(nn);
    out.pos.push_back(pp);
    out.nrm.push_back(nn);
}

static void appendCornerWithColor(TriSoup& out, const Ogre::Vector3& p, const Ogre::Vector3& n, Ogre::RGBA c)
{
    appendCorner(out, p, n);
    out.col.push_back(c);
}

static void appendTriMaybeFlip(TriSoup& out,
                               const std::vector<Ogre::Vector3>& verts,
                               const std::vector<Ogre::Vector3>& norms,
                               int v0, int v1, int v2,
                               int n0, int n1, int n2)
{
    // Transform positions/normals the same way appendCorner does, so the dot test matches what Ogre renders.
    Ogre::Vector3 p0 = verts[static_cast<size_t>(v0)];
    Ogre::Vector3 p1 = verts[static_cast<size_t>(v1)];
    Ogre::Vector3 p2 = verts[static_cast<size_t>(v2)];
    applyPlyImportWorldTransform(p0);
    applyPlyImportWorldTransform(p1);
    applyPlyImportWorldTransform(p2);

    Ogre::Vector3 fn = (p1 - p0).crossProduct(p2 - p0);
    const float fnLen = fn.length();
    if (fnLen > 1e-10f)
        fn /= fnLen;

    Ogre::Vector3 an = norms[static_cast<size_t>(n0)]
                     + norms[static_cast<size_t>(n1)]
                     + norms[static_cast<size_t>(n2)];
    applyPlyImportWorldTransformNormal(an);

    // If normals disagree with geometry, flip winding (and normals indices) for this tri.
    const bool shouldFlip = (!an.isZeroLength() && fnLen > 1e-10f && fn.dotProduct(an) < 0.0f);
    if (!shouldFlip) {
        appendCorner(out, verts[static_cast<size_t>(v0)], norms[static_cast<size_t>(n0)]);
        appendCorner(out, verts[static_cast<size_t>(v1)], norms[static_cast<size_t>(n1)]);
        appendCorner(out, verts[static_cast<size_t>(v2)], norms[static_cast<size_t>(n2)]);
    } else {
        appendCorner(out, verts[static_cast<size_t>(v0)], norms[static_cast<size_t>(n0)]);
        appendCorner(out, verts[static_cast<size_t>(v2)], norms[static_cast<size_t>(n2)]);
        appendCorner(out, verts[static_cast<size_t>(v1)], norms[static_cast<size_t>(n1)]);
    }
}

static void appendTriMaybeFlipColored(TriSoup& out,
                                      const std::vector<Ogre::Vector3>& verts,
                                      const std::vector<Ogre::Vector3>& norms,
                                      int v0, int v1, int v2,
                                      int n0, int n1, int n2,
                                      Ogre::RGBA c)
{
    const size_t before = out.pos.size();
    appendTriMaybeFlip(out, verts, norms, v0, v1, v2, n0, n1, n2);
    const size_t added = out.pos.size() - before;
    for (size_t i = 0; i < added; ++i)
        out.col.push_back(c);
}

static void appendTri(TriSoup& out,
                      const std::vector<Ogre::Vector3>& verts,
                      const std::vector<Ogre::Vector3>& norms,
                      int v0, int v1, int v2,
                      int n0, int n1, int n2)
{
    // Default to keeping source order, but auto-flip if provided normals disagree with geometry.
    appendTriMaybeFlip(out, verts, norms, v0, v1, v2, n0, n1, n2);
}

static void appendQuadAsTwoTris(TriSoup& out,
                                const std::vector<Ogre::Vector3>& verts,
                                const std::vector<Ogre::Vector3>& norms,
                                int v0, int v1, int v2, int v3,
                                int n0, int n1, int n2, int n3)
{
    // Match the TMD quad triangulation convention:
    //   (v0, v1, v2) + (v1, v2, v3)
    // This avoids cracks/holes on many PS1 assets where quads form strips/rings.
    appendTri(out, verts, norms, v0, v1, v2, n0, n1, n2);
    appendTri(out, verts, norms, v1, v2, v3, n1, n2, n3);
}

static QStringList readNonEmptyLines(const QString& text)
{
    QStringList out;
    const QStringList raw = text.split(QRegularExpression(QStringLiteral(R"(\r\n|\n|\r)")), Qt::SkipEmptyParts);
    for (QString line : raw) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(QStringLiteral("//")))
            continue;
        out << line;
    }
    return out;
}

static bool parseCountsLine(const QString& line, int& outV, int& outN, int& outF)
{
    const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
    if (parts.size() < 3)
        return false;
    bool ok = false;
    outV = parts[0].toInt(&ok);
    if (!ok || outV < 0)
        return false;
    outN = parts[1].toInt(&ok);
    if (!ok || outN < 0)
        return false;
    outF = parts[2].toInt(&ok);
    if (!ok || outF < 0)
        return false;
    return true;
}

static bool parseVertexLine(const QString& line, Ogre::Vector3& out)
{
    const QStringList p = line.split(' ', Qt::SkipEmptyParts);
    if (p.size() < 3)
        return false;
    bool ok = false;
    out.x = p[0].toFloat(&ok);
    if (!ok)
        return false;
    out.y = p[1].toFloat(&ok);
    if (!ok)
        return false;
    out.z = p[2].toFloat(&ok);
    if (!ok)
        return false;
    return true;
}

static std::vector<int> parseIntTokens(const QString& line)
{
    std::vector<int> t;
    const QStringList p = line.split(' ', Qt::SkipEmptyParts);
    t.reserve(static_cast<size_t>(p.size()));
    for (const QString& s : p) {
        bool ok = false;
        const int v = s.toInt(&ok, 10);
        if (!ok)
            return {};
        t.push_back(v);
    }
    return t;
}

static size_t triCountFromPsyqFaceLayout(const std::vector<uint8_t>& layout)
{
    size_t t = 0;
    for (uint8_t c : layout) {
        if (c == 3)
            ++t;
        else if (c == 4)
            t += 2;
    }
    return t;
}

/** True if welded triangle (a0,a1,a2) is (c0,c1,c2) up to cyclic rotation and/or reversal. */
static bool weldedTriMatchesCanonical(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t c0, uint32_t c1, uint32_t c2)
{
    const uint32_t a[3] = {a0, a1, a2};
    for (int r = 0; r < 3; ++r) {
        if (a[r] == c0 && a[(r + 1) % 3] == c1 && a[(r + 2) % 3] == c2)
            return true;
        if (a[r] == c0 && a[(r + 2) % 3] == c1 && a[(r + 1) % 3] == c2)
            return true;
    }
    return false;
}

/**
 * Recover PS1/TMD quad corner order (v0,v1,v2,v3) from two welded triangles that were
 * emitted as (v0,v1,v2) + (v1,v2,v3) before welding (each tri may be winding-flipped).
 */
static bool mergeWeldedTriPairToQuad(const std::vector<Ogre::Vector3>& pos,
                                     const std::vector<Ogre::Vector3>& nrm,
                                     uint32_t a0,
                                     uint32_t a1,
                                     uint32_t a2,
                                     uint32_t b0,
                                     uint32_t b1,
                                     uint32_t b2,
                                     std::vector<unsigned int>& quadOut)
{
    quadOut.clear();
    const uint32_t triA[3] = {a0, a1, a2};
    const uint32_t triB[3] = {b0, b1, b2};
    const std::unordered_set<uint32_t> sb{triB[0], triB[1], triB[2]};
    const std::unordered_set<uint32_t> sa{triA[0], triA[1], triA[2]};

    uint32_t v0 = UINT32_MAX;
    for (uint32_t x : triA) {
        if (!sb.count(x))
            v0 = x;
    }
    uint32_t v3 = UINT32_MAX;
    for (uint32_t x : triB) {
        if (!sa.count(x))
            v3 = x;
    }
    if (v0 == UINT32_MAX || v3 == UINT32_MAX)
        return false;

    uint32_t p = UINT32_MAX, q = UINT32_MAX;
    for (uint32_t x : triA) {
        if (x != v0) {
            if (p == UINT32_MAX)
                p = x;
            else
                q = x;
        }
    }
    if (p == UINT32_MAX || q == UINT32_MAX || p == q)
        return false;
    if (!sb.count(p) || !sb.count(q))
        return false;

    const std::unordered_set<uint32_t> uniq{v0, p, q, v3};
    if (uniq.size() != 4)
        return false;

    auto scoreAgainstRef = [&](uint32_t cv0, uint32_t cv1, uint32_t cv2) -> float {
        const Ogre::Vector3& P0 = pos[cv0];
        const Ogre::Vector3& P1 = pos[cv1];
        const Ogre::Vector3& P2 = pos[cv2];
        Ogre::Vector3 fn = (P1 - P0).crossProduct(P2 - P0);
        const float len = fn.length();
        if (len > 1e-20f)
            fn /= len;
        Ogre::Vector3 an = nrm[cv0] + nrm[cv1] + nrm[cv2];
        if (!an.isZeroLength())
            an.normalise();
        else
            return 0.f;
        return fn.dotProduct(an);
    };

    const std::array<std::array<uint32_t, 4>, 2> candidates{{{v0, p, q, v3}, {v0, q, p, v3}}};
    int bestIdx = -1;
    float bestScore = -2.f;
    for (int ci = 0; ci < 2; ++ci) {
        const uint32_t qv0 = candidates[ci][0];
        const uint32_t qv1 = candidates[ci][1];
        const uint32_t qv2 = candidates[ci][2];
        const uint32_t qv3 = candidates[ci][3];
        if (!weldedTriMatchesCanonical(a0, a1, a2, qv0, qv1, qv2))
            continue;
        if (!weldedTriMatchesCanonical(b0, b1, b2, qv1, qv2, qv3))
            continue;
        const float s = scoreAgainstRef(qv0, qv1, qv2) + scoreAgainstRef(qv1, qv2, qv3);
        if (s > bestScore) {
            bestScore = s;
            bestIdx = ci;
        }
    }
    if (bestIdx < 0)
        return false;
    quadOut.assign(candidates[static_cast<size_t>(bestIdx)].begin(), candidates[static_cast<size_t>(bestIdx)].end());
    return true;
}

static bool buildNgonPayloadFromWeldedTriangles(const std::vector<uint32_t>& triIndices,
                                                const std::vector<uint8_t>& layout,
                                                const std::vector<Ogre::Vector3>& uniqPos,
                                                const std::vector<Ogre::Vector3>& uniqNrm,
                                                std::vector<std::vector<unsigned int>>& outFaces)
{
    outFaces.clear();
    const size_t nTri = triIndices.size() / 3;
    size_t triIdx = 0;
    for (uint8_t nc : layout) {
        if (nc == 3) {
            if (triIdx >= nTri)
                return false;
            outFaces.push_back({static_cast<unsigned int>(triIndices[triIdx * 3]),
                                static_cast<unsigned int>(triIndices[triIdx * 3 + 1]),
                                static_cast<unsigned int>(triIndices[triIdx * 3 + 2])});
            ++triIdx;
        } else if (nc == 4) {
            if (triIdx + 1 >= nTri)
                return false;
            const uint32_t a0 = triIndices[triIdx * 3], a1 = triIndices[triIdx * 3 + 1], a2 = triIndices[triIdx * 3 + 2];
            const uint32_t b0 = triIndices[(triIdx + 1) * 3], b1 = triIndices[(triIdx + 1) * 3 + 1],
                           b2 = triIndices[(triIdx + 1) * 3 + 2];
            std::vector<unsigned int> quad;
            if (mergeWeldedTriPairToQuad(uniqPos, uniqNrm, a0, a1, a2, b0, b1, b2, quad))
                outFaces.push_back(std::move(quad));
            else {
                outFaces.push_back({a0, a1, a2});
                outFaces.push_back({b0, b1, b2});
            }
            triIdx += 2;
        } else
            return false;
    }
    return triIdx == nTri;
}

static Ogre::MeshPtr buildMeshFromTriSoup(const std::string& meshName, const TriSoup& soup,
                                          const std::vector<uint8_t>* psyqFaceLayoutForNgons = nullptr)
{
    if (soup.pos.empty() || soup.pos.size() % 3u != 0 || soup.nrm.size() != soup.pos.size())
        return {};
    const bool haveColors = (!soup.col.empty() && soup.col.size() == soup.pos.size());

    std::vector<Ogre::Vector3> uniqPos;
    std::vector<Ogre::Vector3> uniqNrm;
    std::vector<Ogre::RGBA> uniqCol;
    std::vector<uint32_t> indices;
    uniqPos.reserve(soup.pos.size());
    uniqNrm.reserve(soup.nrm.size());
    indices.reserve(soup.pos.size());
    if (haveColors)
        uniqCol.reserve(soup.pos.size());

    std::unordered_map<WeldKey, uint32_t, WeldKeyHash> cornerWeld;
    cornerWeld.reserve(soup.pos.size() / 2);

    for (size_t i = 0; i < soup.pos.size(); ++i) {
        int32_t crgba = 0;
        if (haveColors)
            crgba = static_cast<int32_t>(soup.col[i]);
        const WeldKey key{quantizeWorld(soup.pos[i].x), quantizeWorld(soup.pos[i].y), quantizeWorld(soup.pos[i].z),
                          quantizeWorld(soup.nrm[i].x), quantizeWorld(soup.nrm[i].y), quantizeWorld(soup.nrm[i].z),
                          crgba};
        const auto it = cornerWeld.find(key);
        if (it == cornerWeld.end()) {
            const uint32_t ni = static_cast<uint32_t>(uniqPos.size());
            cornerWeld.emplace(key, ni);
            uniqPos.push_back(soup.pos[i]);
            uniqNrm.push_back(soup.nrm[i]);
            if (haveColors)
                uniqCol.push_back(soup.col[i]);
            indices.push_back(ni);
        } else
            indices.push_back(it->second);
    }

    const size_t nVert = uniqPos.size();
    const size_t nIdx = indices.size();
    const size_t nTri = nIdx / 3;

    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(
        meshName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::SubMesh* sm = mesh->createSubMesh();
    const std::string plyMatName = std::string("PLY/") + meshName;
    try {
        if (!Ogre::MaterialManager::getSingleton().getByName(plyMatName)) {
            if (auto base = Ogre::MaterialManager::getSingleton().getByName("BaseMaterial")) {
                base->clone(plyMatName);
            }
        }
    } catch (...) {
    }
    sm->setMaterialName(Ogre::MaterialManager::getSingleton().getByName(plyMatName) ? plyMatName : "BaseMaterial");
    sm->useSharedVertices = false;

    sm->vertexData = new Ogre::VertexData();
    sm->vertexData->vertexCount = static_cast<uint32_t>(nVert);
    auto* decl = sm->vertexData->vertexDeclaration;
    auto* bind = sm->vertexData->vertexBufferBinding;
    size_t off = 0;
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    if (haveColors) {
        decl->addElement(0, off, Ogre::VET_COLOUR, Ogre::VES_DIFFUSE);
        off += Ogre::VertexElement::getTypeSize(Ogre::VET_COLOUR);
    }
    const size_t vsize = decl->getVertexSize(0);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        vsize, nVert, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint8_t* dst = static_cast<uint8_t*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
    for (size_t i = 0; i < nVert; ++i) {
        uint8_t* row = dst + i * vsize;
        float* pf = nullptr;
        decl->findElementBySemantic(Ogre::VES_POSITION)->baseVertexPointerToElement(row, &pf);
        pf[0] = uniqPos[i].x;
        pf[1] = uniqPos[i].y;
        pf[2] = uniqPos[i].z;
        decl->findElementBySemantic(Ogre::VES_NORMAL)->baseVertexPointerToElement(row, &pf);
        pf[0] = uniqNrm[i].x;
        pf[1] = uniqNrm[i].y;
        pf[2] = uniqNrm[i].z;
        if (haveColors) {
            Ogre::RGBA* cp = nullptr;
            decl->findElementBySemantic(Ogre::VES_DIFFUSE)->baseVertexPointerToElement(row, (void**)&cp);
            *cp = uniqCol[i];
        }
    }
    vbuf->unlock();
    bind->setBinding(0, vbuf);

    try {
        if (auto mat = Ogre::MaterialManager::getSingleton().getByName(plyMatName)) {
            if (!mat->isLoaded())
                mat->load();
            if (mat->getNumTechniques() > 0 && mat->getTechnique(0)->getNumPasses() > 0) {
                Ogre::Pass* p0 = mat->getTechnique(0)->getPass(0);
                if (p0) {
                    p0->setLightingEnabled(true);
                    p0->setAmbient(1.0f, 1.0f, 1.0f);
                    p0->setDiffuse(1.0f, 1.0f, 1.0f, 1.0f);
                    p0->setEmissive(0.0f, 0.0f, 0.0f);
                    p0->setVertexColourTracking(haveColors ? (Ogre::TVC_AMBIENT | Ogre::TVC_DIFFUSE) : Ogre::TVC_NONE);
                }
            }
        }
    } catch (...) {
    }

    const bool use32 = nVert > 65535;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        use32 ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT, nIdx,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    if (use32) {
        auto* ip = static_cast<uint32_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (size_t i = 0; i < nIdx; ++i)
            ip[i] = indices[i];
        ibuf->unlock();
    } else {
        auto* ip = static_cast<uint16_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (size_t i = 0; i < nIdx; ++i)
            ip[i] = static_cast<uint16_t>(indices[i]);
        ibuf->unlock();
    }
    sm->indexData->indexBuffer = ibuf;
    sm->indexData->indexCount = static_cast<uint32_t>(nTri * 3);
    sm->indexData->indexStart = 0;

    Ogre::AxisAlignedBox bounds;
    for (const auto& v : uniqPos)
        bounds.merge(v);
    mesh->_setBounds(bounds);
    mesh->_setBoundingSphereRadius(bounds.getHalfSize().length());
    mesh->load();

    if (psyqFaceLayoutForNgons && !psyqFaceLayoutForNgons->empty()
        && triCountFromPsyqFaceLayout(*psyqFaceLayoutForNgons) == nTri) {
        std::vector<std::vector<unsigned int>> ngonPayload;
        if (buildNgonPayloadFromWeldedTriangles(indices, *psyqFaceLayoutForNgons, uniqPos, uniqNrm, ngonPayload)
            && !ngonPayload.empty()) {
            std::vector<EditableSubMesh> es(1);
            es[0].faces.reserve(ngonPayload.size());
            for (auto& poly : ngonPayload) {
                EditableFace ef;
                ef.indices = std::move(poly);
                if (ef.isValid())
                    es[0].faces.push_back(std::move(ef));
            }
            if (!es[0].faces.empty())
                writeNgonFacesToMesh(mesh.get(), es);
        }
    }

    return mesh;
}

static bool parsePsyqPlyLines(const QStringList& lines, TriSoup& outSoup, const QVector<QColor>* faceColors,
                              std::vector<uint8_t>* logicalFaceVertCounts = nullptr)
{
    outSoup = {};
    if (logicalFaceVertCounts)
        logicalFaceVertCounts->clear();
    static const QRegularExpression kHeaderRe(QStringLiteral("^@PLY\\d*\\s*$"),
                                             QRegularExpression::CaseInsensitiveOption);
    int idx = 0;
    while (idx < lines.size()) {
        if (kHeaderRe.match(lines[idx]).hasMatch()) {
            ++idx;
            break;
        }
        ++idx;
    }
    if (idx >= lines.size())
        return false;

    int nV = 0, nN = 0, nF = 0;
    while (idx < lines.size()) {
        if (parseCountsLine(lines[idx], nV, nN, nF))
            break;
        ++idx;
    }
    if (idx >= lines.size() || nV <= 0 || nN < 0 || nF <= 0)
        return false;
    ++idx;

    std::vector<Ogre::Vector3> verts(static_cast<size_t>(nV));
    for (int vi = 0; vi < nV; ++vi) {
        if (idx >= lines.size())
            return false;
        if (!parseVertexLine(lines[idx], verts[static_cast<size_t>(vi)]))
            return false;
        ++idx;
    }

    std::vector<Ogre::Vector3> norms(static_cast<size_t>(nN));
    for (int ni = 0; ni < nN; ++ni) {
        if (idx >= lines.size())
            return false;
        if (!parseVertexLine(lines[idx], norms[static_cast<size_t>(ni)]))
            return false;
        ++idx;
    }

    TriSoup soup;
    for (int fi = 0; fi < nF; ++fi) {
        if (idx >= lines.size())
            return false;
        const std::vector<int> tok = parseIntTokens(lines[idx]);
        ++idx;
        if (tok.empty())
            return false;
        const bool useFaceColors = (faceColors && faceColors->size() == nF);
        Ogre::RGBA faceRgba = 0;
        if (useFaceColors) {
            const QColor c = (*faceColors)[fi];
            const Ogre::ColourValue cv(float(c.redF()), float(c.greenF()), float(c.blueF()), 1.0f);
            faceRgba = cv.getAsBYTE();
        }

        if (tok[0] == 0) {
            // Triangle formats seen in the wild:
            //  - Psy-Q: 0 v0 v1 v2 0 n0 n1 n2 0
            //  - Blender/RSD exporter: 0 v0 v2 v1 sep n0 n2 n1 end
            if (tok.size() < 9)
                return false;

            int v0 = 0, v1 = 0, v2 = 0;
            int n0 = 0, n1 = 0, n2 = 0;

            const bool psyq = (tok.size() >= 9 && tok[4] == 0 && tok[8] == 0);
            if (psyq) {
                v0 = tok[1]; v1 = tok[2]; v2 = tok[3];
                n0 = tok[5]; n1 = tok[6]; n2 = tok[7];
            } else {
                v0 = tok[1]; v2 = tok[2]; v1 = tok[3];
                n0 = tok[5]; n2 = tok[6]; n1 = tok[7];
            }

            if (v0 < 0 || v1 < 0 || v2 < 0 || n0 < 0 || n1 < 0 || n2 < 0)
                return false;
            if (v0 >= nV || v1 >= nV || v2 >= nV)
                return false;
            if (n0 >= nN || n1 >= nN || n2 >= nN)
                return false;
            if (logicalFaceVertCounts)
                logicalFaceVertCounts->push_back(3);
            if (useFaceColors)
                appendTriMaybeFlipColored(soup, verts, norms, v0, v1, v2, n0, n1, n2, faceRgba);
            else
                appendTri(soup, verts, norms, v0, v1, v2, n0, n1, n2);
        } else if (tok[0] == 1) {
            // Quad in Psy-Q PLY: 1 v0 v1 v2 v3 n0 n1 n2 n3
            if (tok.size() < 9)
                return false;
            const int v0 = tok[1], v1 = tok[2], v2 = tok[3], v3 = tok[4];
            const int n0 = tok[5], n1 = tok[6], n2 = tok[7], n3 = tok[8];
            if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0)
                return false;
            if (v0 >= nV || v1 >= nV || v2 >= nV || v3 >= nV)
                return false;
            if (n0 < 0 || n1 < 0 || n2 < 0 || n3 < 0)
                return false;
            if (n0 >= nN || n1 >= nN || n2 >= nN || n3 >= nN)
                return false;
            if (logicalFaceVertCounts)
                logicalFaceVertCounts->push_back(4);
            if (useFaceColors) {
                appendTriMaybeFlipColored(soup, verts, norms, v0, v1, v2, n0, n1, n2, faceRgba);
                appendTriMaybeFlipColored(soup, verts, norms, v1, v2, v3, n1, n2, n3, faceRgba);
            } else {
                appendQuadAsTwoTris(soup, verts, norms, v0, v1, v2, v3, n0, n1, n2, n3);
            }
        } else if (tok[0] == 3 || tok[0] == 4) {
            // Some Psy-Q toolchains use a "vertex-count first" face format:
            //   3 v0 v1 v2 ... n0 n1 n2
            //   4 v0 v1 v2 v3 ... n0 n1 n2 n3
            // We interpret "..." as optional separators/extra tokens and read normals from the tail.
            const int cnt = tok[0];
            if (tok.size() < static_cast<size_t>(1 + cnt))
                return false;

            const int v0 = tok[1];
            const int v1 = tok[2];
            const int v2 = tok[3];
            const int v3 = (cnt == 4) ? tok[4] : 0;

            if (v0 < 0 || v1 < 0 || v2 < 0 || (cnt == 4 && v3 < 0))
                return false;
            if (v0 >= nV || v1 >= nV || v2 >= nV || (cnt == 4 && v3 >= nV))
                return false;

            // Pick normals from the end if present; otherwise fall back to 0.
            int n0 = 0, n1 = 0, n2 = 0, n3 = 0;
            if (nN > 0 && tok.size() >= static_cast<size_t>(1 + cnt + cnt)) {
                const size_t base = tok.size() - static_cast<size_t>(cnt);
                n0 = tok[base + 0];
                n1 = tok[base + 1];
                n2 = tok[base + 2];
                if (cnt == 4)
                    n3 = tok[base + 3];
            }
            if (n0 < 0 || n1 < 0 || n2 < 0 || (cnt == 4 && n3 < 0))
                return false;
            if (n0 >= nN || n1 >= nN || n2 >= nN || (cnt == 4 && n3 >= nN))
                return false;

            if (cnt == 3) {
                if (logicalFaceVertCounts)
                    logicalFaceVertCounts->push_back(3);
                if (useFaceColors)
                    appendTriMaybeFlipColored(soup, verts, norms, v0, v1, v2, n0, n1, n2, faceRgba);
                else
                    appendTri(soup, verts, norms, v0, v1, v2, n0, n1, n2);
            } else {
                if (logicalFaceVertCounts)
                    logicalFaceVertCounts->push_back(4);
                if (useFaceColors) {
                    appendTriMaybeFlipColored(soup, verts, norms, v0, v1, v2, n0, n1, n2, faceRgba);
                    appendTriMaybeFlipColored(soup, verts, norms, v1, v2, v3, n1, n2, n3, faceRgba);
                } else {
                    appendQuadAsTwoTris(soup, verts, norms, v0, v1, v2, v3, n0, n1, n2, n3);
                }
            }
        } else {
            return false;
        }
    }

    outSoup = std::move(soup);
    return !outSoup.pos.empty();
}

static void applyPlyExportWorldTransform(Ogre::Vector3& p)
{
    // Inverse of applyPlyImportWorldTransform (scale then 180° about Z).
    p.x = -p.x;
    p.y = -p.y;
    p /= PS1PLY::kPsyqPlyEditorUniformScale;
}

static void applyPlyExportWorldTransformNormal(Ogre::Vector3& n)
{
    if (n.isZeroLength())
        return;
    n.normalise();
    n.x = -n.x;
    n.y = -n.y;
}

static Ogre::ColourValue decodePackedColour(const Ogre::VertexElement* colEl, Ogre::RGBA packed)
{
    Ogre::ColourValue cv;
    if (!colEl)
        return cv;

    // Ogre 14.x aliases VET_COLOUR / VET_COLOUR_ARGB / VET_COLOUR_ABGR to the same underlying type,
    // so we cannot reliably infer channel order from colEl->getType().
    // For our PS1 RSD/PLY pipeline we pack colours using ColourValue::getAsBYTE() (via import),
    // which on little-endian corresponds to ABGR in a uint32, i.e. bytes in memory are [R,G,B,A].
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&packed);
#if OGRE_ENDIAN == OGRE_ENDIAN_BIG
    // Big-endian: bytes in memory are [A,B,G,R] for ABGR.
    const float r = float(b[3]) / 255.0f;
    const float g = float(b[2]) / 255.0f;
    const float bl = float(b[1]) / 255.0f;
    const float a = float(b[0]) / 255.0f;
#else
    // Little-endian: bytes in memory are [R,G,B,A].
    const float r = float(b[0]) / 255.0f;
    const float g = float(b[1]) / 255.0f;
    const float bl = float(b[2]) / 255.0f;
    const float a = float(b[3]) / 255.0f;
#endif
    cv = Ogre::ColourValue(r, g, bl, a);
    return cv;
}

static Ogre::Vector3 triFaceNormalWelded(const Ogre::Vector3& p0, const Ogre::Vector3& p1, const Ogre::Vector3& p2)
{
    Ogre::Vector3 n = (p1 - p0).crossProduct(p2 - p0);
    const float len = n.normalise();
    if (len <= 1e-20f)
        return Ogre::Vector3::ZERO;
    return n;
}

static bool hasDirectedEdge(uint32_t e0, uint32_t e1, uint32_t x, uint32_t y, uint32_t z)
{
    return (x == e0 && y == e1) || (y == e0 && z == e1) || (z == e0 && x == e1);
}

static bool tryMergeTrisToQuad(const std::array<uint32_t, 3>& A,
                               const std::array<uint32_t, 3>& B,
                               const std::vector<Ogre::Vector3>& wp,
                               float minNormalDot,
                               std::array<uint32_t, 4>& quad)
{
    for (int e = 0; e < 3; ++e) {
        const uint32_t o = A[static_cast<size_t>(e)];
        const uint32_t e0 = A[static_cast<size_t>((e + 1) % 3)];
        const uint32_t e1 = A[static_cast<size_t>((e + 2) % 3)];

        if (!hasDirectedEdge(e0, e1, B[0], B[1], B[2]))
            continue;

        uint32_t d = 0;
        bool foundD = false;
        for (uint32_t t : {B[0], B[1], B[2]}) {
            if (t != e0 && t != e1) {
                d = t;
                foundD = true;
                break;
            }
        }
        if (!foundD || d == o)
            continue;
        if (o == e0 || o == e1)
            continue;

        std::unordered_set<uint32_t> uniq({o, e0, e1, d});
        if (uniq.size() != 4)
            continue;

        const Ogre::Vector3 nA = triFaceNormalWelded(wp[o], wp[e0], wp[e1]);
        const Ogre::Vector3 nB = triFaceNormalWelded(wp[e0], wp[e1], wp[d]);
        if (nA.isZeroLength() || nB.isZeroLength())
            continue;
        if (nA.dotProduct(nB) < minNormalDot)
            continue;

        quad = {o, e0, e1, d};
        return true;
    }
    return false;
}

struct PsyqExportFace {
    bool isQuad = false;
    uint32_t v[4] = {};
    QColor color;
    bool hasColor = false;
};

static void mergeSubmeshTrisToQuads(const std::vector<uint32_t>& I0,
                                    const std::vector<uint32_t>& I1,
                                    const std::vector<uint32_t>& I2,
                                    const std::vector<Ogre::Vector3>& weldPos,
                                    const QVector<QColor>* triFaceColors,
                                    std::vector<PsyqExportFace>& outFaces)
{
    const size_t n = I0.size();
    if (I1.size() != n || I2.size() != n || n == 0)
        return;

    std::vector<char> used(n, 0);
    const float minDot = 0.94f;
    const bool haveTriColors = (triFaceColors && triFaceColors->size() == static_cast<int>(n));

    auto triRgb = [&](size_t i) -> QColor {
        return haveTriColors ? (*triFaceColors)[static_cast<int>(i)] : QColor();
    };

    for (size_t i = 0; i < n; ++i) {
        if (used[i])
            continue;

        bool merged = false;
        for (size_t j = i + 1; j < n && !merged; ++j) {
            if (used[j])
                continue;

            std::array<uint32_t, 4> q{};
            if (!tryMergeTrisToQuad({I0[i], I1[i], I2[i]}, {I0[j], I1[j], I2[j]}, weldPos, minDot, q))
                continue;

            PsyqExportFace f;
            f.isQuad = true;
            f.v[0] = q[0];
            f.v[1] = q[1];
            f.v[2] = q[2];
            f.v[3] = q[3];
            if (haveTriColors) {
                const QColor a = triRgb(i);
                const QColor b = triRgb(j);
                f.color = QColor((a.red() + b.red()) / 2, (a.green() + b.green()) / 2, (a.blue() + b.blue()) / 2);
                f.hasColor = true;
            }
            outFaces.push_back(f);
            used[i] = used[j] = 1;
            merged = true;
        }

        if (!merged) {
            PsyqExportFace f;
            f.isQuad = false;
            f.v[0] = I0[i];
            f.v[1] = I1[i];
            f.v[2] = I2[i];
            if (haveTriColors) {
                f.color = triRgb(i);
                f.hasColor = true;
            }
            outFaces.push_back(f);
            used[i] = 1;
        }
    }
}

} // namespace

namespace PS1PLY {

bool isPsyqPlyFile(const QString& filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(8192);
    const QString s = QString::fromLatin1(head);
    return s.contains(QRegularExpression(QStringLiteral("@PLY\\d*"), QRegularExpression::CaseInsensitiveOption));
}

Ogre::MeshPtr importPsyqPly(const QString& filePath, const std::string& meshName)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    const QString text = QString::fromLatin1(f.readAll());
    const QStringList lines = readNonEmptyLines(text);
    TriSoup soup;
    std::vector<uint8_t> faceLayout;
    if (!parsePsyqPlyLines(lines, soup, nullptr, &faceLayout))
        return {};

    const std::vector<uint8_t>* layoutPtr = nullptr;
    if (!faceLayout.empty() && triCountFromPsyqFaceLayout(faceLayout) == soup.pos.size() / 3u)
        layoutPtr = &faceLayout;
    return buildMeshFromTriSoup(meshName, soup, layoutPtr);
}

Ogre::MeshPtr importPsyqPlyWithFaceColors(const QString& filePath,
                                         const std::string& meshName,
                                         const QVector<QColor>& faceColors)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QString text = QString::fromLatin1(f.readAll());
    const QStringList lines = readNonEmptyLines(text);
    TriSoup soup;
    std::vector<uint8_t> faceLayout;
    if (!parsePsyqPlyLines(lines, soup, &faceColors, &faceLayout))
        return {};
    const std::vector<uint8_t>* layoutPtr = nullptr;
    if (!faceLayout.empty() && triCountFromPsyqFaceLayout(faceLayout) == soup.pos.size() / 3u)
        layoutPtr = &faceLayout;
    return buildMeshFromTriSoup(meshName, soup, layoutPtr);
}

bool exportPsyqPlyFromEntity(const Ogre::Entity* entity,
                             const QString& plyPath,
                             QVector<QColor>* outFaceColors,
                             QString* outError)
{
    if (!entity || !entity->getMesh().get()) {
        if (outError) *outError = QStringLiteral("Missing entity/mesh.");
        return false;
    }

    const Ogre::MeshPtr mesh = entity->getMesh();
    const unsigned numSub = mesh->getNumSubMeshes();

    struct SubData {
        Ogre::VertexData* vd = nullptr;
        Ogre::IndexData* id = nullptr;
        const Ogre::VertexElement* posEl = nullptr;
        const Ogre::VertexElement* nrmEl = nullptr;
        const Ogre::VertexElement* colEl = nullptr;
        Ogre::HardwareVertexBufferSharedPtr posBuf;
        Ogre::HardwareVertexBufferSharedPtr nrmBuf;
        Ogre::HardwareVertexBufferSharedPtr colBuf;
        size_t posStride = 0, nrmStride = 0, colStride = 0;
        uint32_t vCount = 0;
    };

    std::vector<SubData> subs;
    subs.reserve(numSub);
    uint32_t totalFaces = 0;

    for (unsigned si = 0; si < numSub; ++si) {
        Ogre::SubMesh* sm = mesh->getSubMesh(si);
        if (!sm || !sm->indexData)
            continue;
        Ogre::VertexData* vd = sm->useSharedVertices ? mesh->sharedVertexData : sm->vertexData;
        if (!vd || vd->vertexCount == 0 || sm->indexData->indexCount < 3)
            continue;

        SubData sd;
        sd.vd = vd;
        sd.id = sm->indexData;
        sd.posEl = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        sd.nrmEl = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);
        sd.colEl = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE);
        if (!sd.posEl || !sd.nrmEl)
            continue;

        sd.vCount = static_cast<uint32_t>(vd->vertexCount);
        totalFaces += static_cast<uint32_t>(sd.id->indexCount / 3);

        sd.posBuf = vd->vertexBufferBinding->getBuffer(sd.posEl->getSource());
        sd.nrmBuf = vd->vertexBufferBinding->getBuffer(sd.nrmEl->getSource());
        if (sd.colEl)
            sd.colBuf = vd->vertexBufferBinding->getBuffer(sd.colEl->getSource());

        sd.posStride = sd.posBuf->getVertexSize();
        sd.nrmStride = sd.nrmBuf->getVertexSize();
        sd.colStride = sd.colBuf ? sd.colBuf->getVertexSize() : 0;

        subs.push_back(sd);
    }

    if (subs.empty() || totalFaces == 0) {
        if (outError) *outError = QStringLiteral("No exportable submeshes.");
        return false;
    }

    std::vector<std::vector<unsigned int>> ngonFaces;
    bool useNgonExport =
        (numSub == 1u && subs.size() == 1u && readNgonFacesFromMesh(mesh.get(), 0, ngonFaces));
    if (useNgonExport) {
        for (const auto& poly : ngonFaces) {
            if (poly.size() < 3) {
                useNgonExport = false;
                break;
            }
            for (unsigned int vid : poly) {
                if (vid >= subs[0].vCount) {
                    useNgonExport = false;
                    break;
                }
            }
            if (!useNgonExport)
                break;
        }
    }

    std::vector<Ogre::Vector3> weldedPos;
    std::vector<Ogre::Vector3> weldedNrm;
    weldedPos.reserve(size_t(totalFaces) * 3u);
    weldedNrm.reserve(size_t(totalFaces) * 3u);
    std::unordered_map<WeldKey, uint32_t, WeldKeyHash> weld;
    weld.reserve(size_t(totalFaces) * 3u);

    std::vector<PsyqExportFace> allExportFaces;
    allExportFaces.reserve(totalFaces);

    auto weldCorner = [&](const Ogre::Vector3& p, const Ogre::Vector3& n, int32_t crgba) -> uint32_t {
        const WeldKey key{quantizeWorld(p.x), quantizeWorld(p.y), quantizeWorld(p.z),
                          quantizeWorld(n.x), quantizeWorld(n.y), quantizeWorld(n.z), crgba};
        const auto it = weld.find(key);
        if (it != weld.end())
            return it->second;
        const uint32_t idx = static_cast<uint32_t>(weldedPos.size());
        weld.emplace(key, idx);
        weldedPos.push_back(p);
        weldedNrm.push_back(n);
        return idx;
    };

    if (useNgonExport) {
        SubData& sd = subs[0];
        const uint8_t* posBase = static_cast<const uint8_t*>(sd.posBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        const uint8_t* nrmBase = nullptr;
        if (sd.nrmEl->getSource() == sd.posEl->getSource()) {
            nrmBase = posBase;
        } else {
            nrmBase = static_cast<const uint8_t*>(sd.nrmBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        }
        const uint8_t* colBase = nullptr;
        if (sd.colBuf) {
            if (sd.colEl->getSource() == sd.posEl->getSource()) {
                colBase = posBase;
            } else if (sd.colEl->getSource() == sd.nrmEl->getSource() && sd.nrmEl->getSource() != sd.posEl->getSource()) {
                colBase = nrmBase;
            } else {
                colBase = static_cast<const uint8_t*>(sd.colBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            }
        }

        const bool collectFaceColors = (outFaceColors != nullptr && sd.colEl && colBase);

        auto readPN = [&](uint32_t ii, Ogre::Vector3& p, Ogre::Vector3& n) {
            const uint8_t* prow = posBase + size_t(ii) * sd.posStride;
            Ogre::Real* pf = nullptr;
            sd.posEl->baseVertexPointerToElement(const_cast<uint8_t*>(prow), &pf);
            p = Ogre::Vector3(pf[0], pf[1], pf[2]);
            applyPlyExportWorldTransform(p);
            const uint8_t* nrow = nrmBase + size_t(ii) * sd.nrmStride;
            Ogre::Real* nf = nullptr;
            sd.nrmEl->baseVertexPointerToElement(const_cast<uint8_t*>(nrow), &nf);
            n = Ogre::Vector3(nf[0], nf[1], nf[2]);
            applyPlyExportWorldTransformNormal(n);
        };

        auto cornerCrgba = [&](uint32_t vi) -> int32_t {
            int32_t c = 0;
            if (sd.colEl && colBase) {
                Ogre::RGBA* cp = nullptr;
                sd.colEl->baseVertexPointerToElement(const_cast<uint8_t*>(colBase + size_t(vi) * sd.colStride), &cp);
                c = static_cast<int32_t>(*cp);
            }
            return c;
        };

        constexpr uint32_t kMeshVertUnwelded = std::numeric_limits<uint32_t>::max();
        std::vector<uint32_t> meshToFile(sd.vCount, kMeshVertUnwelded);

        auto fileWeldForMeshVert = [&](uint32_t vi) -> uint32_t {
            if (meshToFile[vi] != kMeshVertUnwelded)
                return meshToFile[vi];
            Ogre::Vector3 p, n;
            readPN(vi, p, n);
            const uint32_t w = weldCorner(p, n, cornerCrgba(vi));
            meshToFile[vi] = w;
            return w;
        };

        auto avgRgbCorners = [&](const std::vector<unsigned int>& corners) -> QColor {
            Ogre::ColourValue acc(0, 0, 0, 1.0f);
            for (unsigned int vi : corners) {
                const Ogre::ColourValue cv =
                    decodePackedColour(sd.colEl, static_cast<Ogre::RGBA>(cornerCrgba(vi)));
                acc.r += cv.r;
                acc.g += cv.g;
                acc.b += cv.b;
            }
            const float inv = 1.0f / float(corners.size());
            return QColor::fromRgbF(acc.r * inv, acc.g * inv, acc.b * inv, 1.0f);
        };

        allExportFaces.reserve(ngonFaces.size() * 2);

        for (const auto& poly : ngonFaces) {
            const size_t ps = poly.size();
            if (ps == 3) {
                PsyqExportFace f;
                f.isQuad = false;
                f.v[0] = fileWeldForMeshVert(poly[0]);
                f.v[1] = fileWeldForMeshVert(poly[1]);
                f.v[2] = fileWeldForMeshVert(poly[2]);
                if (collectFaceColors) {
                    f.color = avgRgbCorners(poly);
                    f.hasColor = true;
                }
                allExportFaces.push_back(f);
            } else if (ps == 4) {
                PsyqExportFace f;
                f.isQuad = true;
                f.v[0] = fileWeldForMeshVert(poly[0]);
                f.v[1] = fileWeldForMeshVert(poly[1]);
                f.v[2] = fileWeldForMeshVert(poly[2]);
                f.v[3] = fileWeldForMeshVert(poly[3]);
                if (collectFaceColors) {
                    f.color = avgRgbCorners(poly);
                    f.hasColor = true;
                }
                allExportFaces.push_back(f);
            } else {
                const uint32_t hub = fileWeldForMeshVert(poly[0]);
                for (size_t k = 1; k + 1 < ps; ++k) {
                    PsyqExportFace t;
                    t.isQuad = false;
                    t.v[0] = hub;
                    t.v[1] = fileWeldForMeshVert(poly[k]);
                    t.v[2] = fileWeldForMeshVert(poly[k + 1]);
                    if (collectFaceColors) {
                        const std::vector<unsigned int> tri{poly[0], poly[static_cast<size_t>(k)],
                                                              poly[static_cast<size_t>(k + 1)]};
                        t.color = avgRgbCorners(tri);
                        t.hasColor = true;
                    }
                    allExportFaces.push_back(t);
                }
            }
        }

        if (sd.colBuf && colBase && sd.colEl->getSource() != sd.posEl->getSource()
            && !(sd.colEl->getSource() == sd.nrmEl->getSource() && sd.nrmEl->getSource() != sd.posEl->getSource())) {
            sd.colBuf->unlock();
        }
        if (sd.nrmEl->getSource() != sd.posEl->getSource())
            sd.nrmBuf->unlock();
        sd.posBuf->unlock();
    } else {
        for (unsigned si = 0; si < subs.size(); ++si) {
            SubData& sd = subs[si];
            const uint8_t* posBase = static_cast<const uint8_t*>(sd.posBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            const uint8_t* nrmBase = nullptr;
            if (sd.nrmEl->getSource() == sd.posEl->getSource()) {
                nrmBase = posBase;
            } else {
                nrmBase = static_cast<const uint8_t*>(sd.nrmBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            }
            const uint8_t* colBase = nullptr;
            if (sd.colBuf) {
                if (sd.colEl->getSource() == sd.posEl->getSource()) {
                    colBase = posBase;
                } else if (sd.colEl->getSource() == sd.nrmEl->getSource()
                           && sd.nrmEl->getSource() != sd.posEl->getSource()) {
                    colBase = nrmBase;
                } else {
                    colBase = static_cast<const uint8_t*>(sd.colBuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                }
            }

            auto ibuf = sd.id->indexBuffer;
            const uint8_t* idxBase = static_cast<const uint8_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            const bool idx32 = (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT);
            const unsigned ist = sd.id->indexStart;
            const size_t triCount = sd.id->indexCount / 3;

            std::vector<uint32_t> smI0;
            std::vector<uint32_t> smI1;
            std::vector<uint32_t> smI2;
            QVector<QColor> smFaceCols;
            smI0.reserve(triCount);
            smI1.reserve(triCount);
            smI2.reserve(triCount);
            const bool collectFaceColors = (outFaceColors != nullptr && sd.colEl && colBase);

            for (size_t t = 0; t < triCount; ++t) {
                uint32_t i0, i1, i2;
                if (idx32) {
                    const auto* ip = reinterpret_cast<const uint32_t*>(idxBase);
                    i0 = ip[ist + t * 3 + 0];
                    i1 = ip[ist + t * 3 + 1];
                    i2 = ip[ist + t * 3 + 2];
                } else {
                    const auto* ip = reinterpret_cast<const uint16_t*>(idxBase);
                    i0 = ip[ist + t * 3 + 0];
                    i1 = ip[ist + t * 3 + 1];
                    i2 = ip[ist + t * 3 + 2];
                }
                if (i0 >= sd.vCount || i1 >= sd.vCount || i2 >= sd.vCount)
                    continue;

                auto readPN = [&](uint32_t ii, Ogre::Vector3& p, Ogre::Vector3& n) {
                    const uint8_t* prow = posBase + size_t(ii) * sd.posStride;
                    Ogre::Real* pf = nullptr;
                    sd.posEl->baseVertexPointerToElement(const_cast<uint8_t*>(prow), &pf);
                    p = Ogre::Vector3(pf[0], pf[1], pf[2]);
                    applyPlyExportWorldTransform(p);
                    const uint8_t* nrow = nrmBase + size_t(ii) * sd.nrmStride;
                    Ogre::Real* nf = nullptr;
                    sd.nrmEl->baseVertexPointerToElement(const_cast<uint8_t*>(nrow), &nf);
                    n = Ogre::Vector3(nf[0], nf[1], nf[2]);
                    applyPlyExportWorldTransformNormal(n);
                };

                Ogre::Vector3 p0, p1, p2, n0, n1, n2;
                readPN(i0, p0, n0);
                readPN(i1, p1, n1);
                readPN(i2, p2, n2);

                int32_t c0 = 0, c1 = 0, c2 = 0;
                if (sd.colEl && colBase) {
                    Ogre::RGBA* cp = nullptr;
                    sd.colEl->baseVertexPointerToElement(const_cast<uint8_t*>(colBase + size_t(i0) * sd.colStride), &cp);
                    c0 = static_cast<int32_t>(*cp);
                    sd.colEl->baseVertexPointerToElement(const_cast<uint8_t*>(colBase + size_t(i1) * sd.colStride), &cp);
                    c1 = static_cast<int32_t>(*cp);
                    sd.colEl->baseVertexPointerToElement(const_cast<uint8_t*>(colBase + size_t(i2) * sd.colStride), &cp);
                    c2 = static_cast<int32_t>(*cp);
                }

                const uint32_t w0 = weldCorner(p0, n0, c0);
                const uint32_t w1 = weldCorner(p1, n1, c1);
                const uint32_t w2 = weldCorner(p2, n2, c2);
                smI0.push_back(w0);
                smI1.push_back(w1);
                smI2.push_back(w2);

                if (collectFaceColors) {
                    const Ogre::ColourValue cv0 = decodePackedColour(sd.colEl, static_cast<Ogre::RGBA>(c0));
                    const Ogre::ColourValue cv1 = decodePackedColour(sd.colEl, static_cast<Ogre::RGBA>(c1));
                    const Ogre::ColourValue cv2 = decodePackedColour(sd.colEl, static_cast<Ogre::RGBA>(c2));
                    const Ogre::ColourValue ca((cv0.r + cv1.r + cv2.r) / 3.0f,
                                               (cv0.g + cv1.g + cv2.g) / 3.0f,
                                               (cv0.b + cv1.b + cv2.b) / 3.0f,
                                               1.0f);
                    smFaceCols.push_back(QColor::fromRgbF(ca.r, ca.g, ca.b, 1.0));
                }
            }

            std::vector<PsyqExportFace> subFaces;
            QVector<QColor>* colorMerge =
                collectFaceColors && smFaceCols.size() == static_cast<int>(smI0.size()) ? &smFaceCols : nullptr;

            mergeSubmeshTrisToQuads(smI0, smI1, smI2, weldedPos, colorMerge, subFaces);
            allExportFaces.insert(allExportFaces.end(), subFaces.begin(), subFaces.end());

            ibuf->unlock();
            if (sd.colBuf && colBase && sd.colEl->getSource() != sd.posEl->getSource()
                && !(sd.colEl->getSource() == sd.nrmEl->getSource()
                     && sd.nrmEl->getSource() != sd.posEl->getSource())) {
                sd.colBuf->unlock();
            }
            if (sd.nrmEl->getSource() != sd.posEl->getSource())
                sd.nrmBuf->unlock();
            sd.posBuf->unlock();
        }
    }

    if (outFaceColors) {
        outFaceColors->clear();
        bool allColored = !allExportFaces.empty();
        for (const PsyqExportFace& ef : allExportFaces) {
            if (!ef.hasColor) {
                allColored = false;
                break;
            }
        }
        if (allColored) {
            outFaceColors->reserve(static_cast<int>(allExportFaces.size()));
            for (const PsyqExportFace& ef : allExportFaces)
                outFaceColors->push_back(ef.color);
        }
    }

    const uint32_t nV = static_cast<uint32_t>(weldedPos.size());
    const uint32_t nWrittenFaces = static_cast<uint32_t>(allExportFaces.size());

    QFile f(plyPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (outError) *outError = QStringLiteral("Could not open PLY file for writing.");
        return false;
    }

    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Latin1);
    ts << "@PLY940102\n";
    ts << nV << " " << nV << " " << nWrittenFaces << "\n";

    for (const Ogre::Vector3& p : weldedPos)
        ts << p.x << " " << p.y << " " << p.z << "\n";
    for (const Ogre::Vector3& n : weldedNrm)
        ts << n.x << " " << n.y << " " << n.z << "\n";

    for (const PsyqExportFace& ef : allExportFaces) {
        if (ef.isQuad) {
            ts << "1 " << ef.v[0] << " " << ef.v[1] << " " << ef.v[2] << " " << ef.v[3] << " " << ef.v[0] << " "
               << ef.v[1] << " " << ef.v[2] << " " << ef.v[3] << "\n";
        } else {
            ts << "0 " << ef.v[0] << " " << ef.v[1] << " " << ef.v[2] << " 0 " << ef.v[0] << " " << ef.v[1] << " "
               << ef.v[2] << " 0\n";
        }
    }

    if (ts.status() != QTextStream::Ok) {
        if (outError) *outError = QStringLiteral("Write failed.");
        return false;
    }
    return true;
}

} // namespace PS1PLY
