/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "PS1/PS1PLY.h"

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

#include <cmath>
#include <unordered_map>
#include <vector>

namespace {

struct TriSoup {
    std::vector<Ogre::Vector3> pos;
    std::vector<Ogre::Vector3> nrm;
    std::vector<Ogre::RGBA> col; // optional; if present must match pos size
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

static Ogre::MeshPtr buildMeshFromTriSoup(const std::string& meshName, const TriSoup& soup)
{
    if (soup.pos.empty() || soup.pos.size() % 3u != 0 || soup.nrm.size() != soup.pos.size())
        return {};
    const bool haveColors = (!soup.col.empty() && soup.col.size() == soup.pos.size());

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

    const size_t nVert = soup.pos.size();
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
        pf[0] = soup.pos[i].x;
        pf[1] = soup.pos[i].y;
        pf[2] = soup.pos[i].z;
        decl->findElementBySemantic(Ogre::VES_NORMAL)->baseVertexPointerToElement(row, &pf);
        pf[0] = soup.nrm[i].x;
        pf[1] = soup.nrm[i].y;
        pf[2] = soup.nrm[i].z;
        if (haveColors) {
            Ogre::RGBA* cp = nullptr;
            decl->findElementBySemantic(Ogre::VES_DIFFUSE)->baseVertexPointerToElement(row, (void**)&cp);
            *cp = soup.col[i];
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
                    // Lighting ON by default; vertex colors (if present) modulate diffuse/ambient.
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

    const size_t nTri = nVert / 3;
    const bool use32 = nVert > 65535;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        use32 ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT, nTri * 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    if (use32) {
        auto* ip = static_cast<uint32_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (size_t i = 0; i < nVert; ++i)
            ip[i] = static_cast<uint32_t>(i);
        ibuf->unlock();
    } else {
        auto* ip = static_cast<uint16_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
        for (size_t i = 0; i < nVert; ++i)
            ip[i] = static_cast<uint16_t>(i);
        ibuf->unlock();
    }
    sm->indexData->indexBuffer = ibuf;
    sm->indexData->indexCount = static_cast<uint32_t>(nTri * 3);
    sm->indexData->indexStart = 0;

    Ogre::AxisAlignedBox bounds;
    for (const auto& v : soup.pos)
        bounds.merge(v);
    mesh->_setBounds(bounds);
    mesh->_setBoundingSphereRadius(bounds.getHalfSize().length());
    mesh->load();
    return mesh;
}

static bool parsePsyqPlyLines(const QStringList& lines, TriSoup& outSoup, const QVector<QColor>* faceColors)
{
    outSoup = {};
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
                if (useFaceColors)
                    appendTriMaybeFlipColored(soup, verts, norms, v0, v1, v2, n0, n1, n2, faceRgba);
                else
                    appendTri(soup, verts, norms, v0, v1, v2, n0, n1, n2);
            } else {
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

static int32_t quantizeWorld(Ogre::Real v)
{
    return static_cast<int32_t>(std::lround(double(v) * 100000.0));
}

struct WeldKey {
    int32_t px, py, pz, nx, ny, nz;
    int32_t crgba; // raw packed diffuse, or 0 if mesh has no per-vertex colour

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
    if (!parsePsyqPlyLines(lines, soup, nullptr))
        return {};

    return buildMeshFromTriSoup(meshName, soup);
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
    if (!parsePsyqPlyLines(lines, soup, &faceColors))
        return {};
    return buildMeshFromTriSoup(meshName, soup);
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

    std::vector<Ogre::Vector3> weldedPos;
    std::vector<Ogre::Vector3> weldedNrm;
    weldedPos.reserve(size_t(totalFaces) * 3u);
    weldedNrm.reserve(size_t(totalFaces) * 3u);
    std::unordered_map<WeldKey, uint32_t, WeldKeyHash> weld;
    weld.reserve(size_t(totalFaces) * 3u);

    std::vector<uint32_t> triI0, triI1, triI2;
    triI0.reserve(totalFaces);
    triI1.reserve(totalFaces);
    triI2.reserve(totalFaces);
    if (outFaceColors)
        outFaceColors->clear();

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

    for (SubData& sd : subs) {
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

        auto ibuf = sd.id->indexBuffer;
        const uint8_t* idxBase = static_cast<const uint8_t*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        const bool idx32 = (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT);
        const unsigned ist = sd.id->indexStart;
        const size_t triCount = sd.id->indexCount / 3;

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
            triI0.push_back(w0);
            triI1.push_back(w1);
            triI2.push_back(w2);

            if (outFaceColors && sd.colEl && colBase) {
                Ogre::RGBA* cp = nullptr;
                sd.colEl->baseVertexPointerToElement(const_cast<uint8_t*>(colBase + size_t(i0) * sd.colStride), &cp);
                const Ogre::ColourValue cv0 = decodePackedColour(sd.colEl, *cp);
                sd.colEl->baseVertexPointerToElement(const_cast<uint8_t*>(colBase + size_t(i1) * sd.colStride), &cp);
                const Ogre::ColourValue cv1 = decodePackedColour(sd.colEl, *cp);
                sd.colEl->baseVertexPointerToElement(const_cast<uint8_t*>(colBase + size_t(i2) * sd.colStride), &cp);
                const Ogre::ColourValue cv2 = decodePackedColour(sd.colEl, *cp);
                const Ogre::ColourValue ca((cv0.r + cv1.r + cv2.r) / 3.0f,
                                           (cv0.g + cv1.g + cv2.g) / 3.0f,
                                           (cv0.b + cv1.b + cv2.b) / 3.0f,
                                           1.0f);
                outFaceColors->push_back(QColor::fromRgbF(ca.r, ca.g, ca.b, 1.0));
            }
        }

        ibuf->unlock();
        if (sd.colBuf && colBase && sd.colEl->getSource() != sd.posEl->getSource()
            && !(sd.colEl->getSource() == sd.nrmEl->getSource() && sd.nrmEl->getSource() != sd.posEl->getSource())) {
            sd.colBuf->unlock();
        }
        if (sd.nrmEl->getSource() != sd.posEl->getSource())
            sd.nrmBuf->unlock();
        sd.posBuf->unlock();
    }

    const uint32_t nV = static_cast<uint32_t>(weldedPos.size());
    const uint32_t nWrittenFaces = static_cast<uint32_t>(triI0.size());

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

    for (uint32_t fi = 0; fi < nWrittenFaces; ++fi) {
        const uint32_t v0 = triI0[fi];
        const uint32_t v1 = triI1[fi];
        const uint32_t v2 = triI2[fi];
        ts << "0 " << v0 << " " << v1 << " " << v2 << " 0 " << v0 << " " << v1 << " " << v2 << " 0\n";
    }

    if (ts.status() != QTextStream::Ok) {
        if (outError) *outError = QStringLiteral("Write failed.");
        return false;
    }
    return true;
}

} // namespace PS1PLY
