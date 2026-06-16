/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

// Coverage-focused companion to PS1PLY_test.cpp. Distinct suite name
// (PS1PLYExportCoverageTest) and distinct file name so there is no ODR/registration
// clash with the existing PS1PLY / PS1PLYOgreTest suites.
//
// Targets the under-exercised slices of PS1PLY:
//   * exportPsyqPlyFromEntity filling BOTH out-params at once (outFaceColors +
//     outFaceTextures) on a textured + vertex-coloured entity — every face has UVs
//     (textured=true) AND a per-face colour, exercising the "allColored" branch in
//     the outFaceColors fill plus hasCornerColors population in outFaceTextures.
//   * importPsyqPlyWithFaceMaterials with two DISTINCT textureIndex values + one solid
//     face -> three submeshes (_tex0, _tex1, _solid) with per-submesh UV storage.
//   * importPsyqPlyWithFaceMaterials per-corner vertColors path (3- and 4-entry) wiring
//     VES_DIFFUSE onto textured submesh vertices.
//   * exportPsyqPlyFromEntity multi-submesh entity where ExportFaceTexture::submeshIndex
//     distinguishes faces from a textured submesh vs. a solid submesh.

#include <gtest/gtest.h>

#include <algorithm>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QThread>
#include <QVector>

#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include "Manager.h"
#include "PS1/PS1PLY.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

namespace {

constexpr unsigned long kSettleMs = 30;

static void ensureBaseMaterialForPlyImport()
{
    if (Ogre::MaterialManager::getSingleton().getByName(
            "BaseMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)) {
        return;
    }
    Ogre::MaterialPtr m = Ogre::MaterialManager::getSingleton().create(
        "BaseMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    m->getTechnique(0)->getPass(0)->setDiffuse(1.0f, 1.0f, 1.0f, 1.0f);
    m->getTechnique(0)->getPass(0)->setAmbient(1.0f, 1.0f, 1.0f);
}

// Writes a minimal Psy-Q PLY with `nF` triangle/quad face lines. The header carries
// `nV` vertices and `nN` normals; geometry is a unit square in the +Z plane.
// Each entry of `faceLines` is a full Psy-Q face line (already formatted).
static bool writePsyqPly(const QString& path,
                         int nV, int nN, int nF,
                         const QStringList& vertexLines,
                         const QStringList& normalLines,
                         const QStringList& faceLines)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream ts(&f);
    ts << "@PLY940102\n";
    ts << nV << ' ' << nN << ' ' << nF << '\n';
    for (const QString& v : vertexLines)
        ts << v << '\n';
    for (const QString& n : normalLines)
        ts << n << '\n';
    for (const QString& fl : faceLines)
        ts << fl << '\n';
    return true;
}

} // namespace

class PS1PLYExportCoverageTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override
    {
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(kSettleMs);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed";
        createStandardOgreMaterials();
        ensureBaseMaterialForPlyImport();
    }

    void TearDown() override
    {
        if (Manager::getSingletonPtr())
            SelectionSet::getSingleton()->clear();
        SelectionSet::kill();
        Manager::kill();
        if (app)
            app->processEvents();
        QThread::msleep(kSettleMs);
    }

    // Helper: a single quad PLY (4 verts, 1 normal, 1 quad face).
    bool writeSingleQuadPly(const QString& path)
    {
        return writePsyqPly(
            path, 4, 1, 1,
            {QStringLiteral("0 0 0"), QStringLiteral("1 0 0"),
             QStringLiteral("1 1 0"), QStringLiteral("0 1 0")},
            {QStringLiteral("0 0 1")},
            {QStringLiteral("1 0 1 2 3 0 0 0 0")});
    }

    // Helper: a single triangle PLY (3 verts, 1 normal, 1 tri face).
    bool writeSingleTriPly(const QString& path)
    {
        return writePsyqPly(
            path, 3, 1, 1,
            {QStringLiteral("0 0 0"), QStringLiteral("1 0 0"),
             QStringLiteral("0 1 0")},
            {QStringLiteral("0 0 1")},
            {QStringLiteral("0 0 1 2 0 0 0 0 0")});
    }
};

// ---------------------------------------------------------------------------
// 1. SIMULTANEOUS dual out-param fill: textured + vertex-coloured single quad.
//    Every written face must carry both a UV envelope (textured=true) AND a flat
//    per-face colour (outFaceColors non-empty, one per written face), with
//    per-corner colours surfaced via ExportFaceTexture::hasCornerColors.
// ---------------------------------------------------------------------------
TEST_F(PS1PLYExportCoverageTest, ExportFillsFaceColorsAndFaceTexturesTogether)
{
    ASSERT_TRUE(canLoadMeshFiles());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString plyIn = QDir(dir.path()).filePath(QStringLiteral("tex_colored.ply"));
    ASSERT_TRUE(writeSingleQuadPly(plyIn));

    // A textured quad WITH four per-corner vertex colours: the import builds one
    // submesh that carries BOTH a UV stream and a VES_DIFFUSE stream.
    QVector<PS1PLY::FaceMaterial> mats(1);
    mats[0].textured = true;
    mats[0].textureIndex = 0;
    mats[0].u = {0.0f, 1.0f, 1.0f, 0.0f};
    mats[0].v = {0.0f, 0.0f, 1.0f, 1.0f};
    mats[0].vertColors = {QColor(255, 0, 0), QColor(0, 255, 0),
                          QColor(0, 0, 255), QColor(255, 255, 0)};

    const std::string meshName = "PS1PlyCovTexColMesh";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);
    Ogre::MeshPtr mesh = PS1PLY::importPsyqPlyWithFaceMaterials(plyIn, meshName, mats);
    ASSERT_TRUE(mesh);
    ASSERT_EQ(mesh->getNumSubMeshes(), 1u);

    // Confirm the import wired BOTH UV and diffuse streams onto the textured submesh.
    const Ogre::VertexData* vd = mesh->getSubMesh(0)->vertexData;
    ASSERT_NE(vd, nullptr);
    EXPECT_NE(vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES), nullptr);
    EXPECT_NE(vd->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE), nullptr);

    auto* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode(QStringLiteral("PS1PlyCovTexColNode"));
    ASSERT_NE(node, nullptr);
    Ogre::Entity* ent = mgr->createEntity(node, mesh);
    ASSERT_NE(ent, nullptr);

    QTemporaryFile outPly(QDir::tempPath() + QStringLiteral("/qtmesh_ps1ply_cov_dual_XXXXXX.ply"));
    outPly.setAutoRemove(true);
    ASSERT_TRUE(outPly.open());
    outPly.close();

    QVector<QColor> faceColors;
    QVector<PS1PLY::ExportFaceTexture> faceTex;
    QString err;
    ASSERT_TRUE(PS1PLY::exportPsyqPlyFromEntity(ent, outPly.fileName(),
                                                &faceColors, &faceTex, &err))
        << err.toUtf8().constData();

    // Both sinks populated, with matching cardinality (one entry per written face).
    ASSERT_FALSE(faceTex.isEmpty());
    EXPECT_FALSE(faceColors.isEmpty())
        << "outFaceColors must be filled when every face carries a colour.";
    EXPECT_EQ(faceColors.size(), faceTex.size());

    // Every written face is textured, has a valid submeshIndex, a 3-or-4 corner count
    // matching its UV fill, and surfaces per-corner colours.
    bool sawCornerColors = false;
    for (const auto& f : faceTex) {
        EXPECT_TRUE(f.textured);
        EXPECT_GE(f.submeshIndex, 0);
        EXPECT_TRUE(f.cornerCount == 3 || f.cornerCount == 4);

        // The UV envelope should span the [0..1] square we configured.
        float minU = 1.f, maxU = 0.f, minV = 1.f, maxV = 0.f;
        for (int k = 0; k < f.cornerCount; ++k) {
            minU = std::min(minU, f.u[k]);
            maxU = std::max(maxU, f.u[k]);
            minV = std::min(minV, f.v[k]);
            maxV = std::max(maxV, f.v[k]);
        }
        EXPECT_NEAR(minU, 0.0f, 1e-3f);
        EXPECT_NEAR(maxU, 1.0f, 1e-3f);
        EXPECT_NEAR(minV, 0.0f, 1e-3f);
        EXPECT_NEAR(maxV, 1.0f, 1e-3f);

        if (f.hasCornerColors)
            sawCornerColors = true;
    }
    EXPECT_TRUE(sawCornerColors)
        << "Textured + coloured face should surface per-corner colours.";

    // Every flat face colour must be a valid QColor.
    for (const QColor& c : faceColors)
        EXPECT_TRUE(c.isValid());

    mgr->destroySceneNode(QStringLiteral("PS1PlyCovTexColNode"));
    Ogre::MeshManager::getSingleton().remove(meshName);
}

// ---------------------------------------------------------------------------
// 2. Multi-texture-slot split: two DISTINCT textureIndex values + one solid face
//    -> three submeshes (_tex0, _tex1, _solid). Each textured submesh stores UVs;
//    the solid one does not.
// ---------------------------------------------------------------------------
TEST_F(PS1PLYExportCoverageTest, ImportSplitsTwoDistinctTextureSlotsPlusSolid)
{
    ASSERT_TRUE(canLoadMeshFiles());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString plyIn = QDir(dir.path()).filePath(QStringLiteral("multitex.ply"));
    // Six verts forming three independent triangles, one normal (+Z), three tri faces.
    ASSERT_TRUE(writePsyqPly(
        plyIn, 6, 1, 3,
        {QStringLiteral("0 0 0"), QStringLiteral("1 0 0"), QStringLiteral("0 1 0"),
         QStringLiteral("2 0 0"), QStringLiteral("3 0 0"), QStringLiteral("2 1 0")},
        {QStringLiteral("0 0 1")},
        {QStringLiteral("0 0 1 2 0 0 0 0 0"),
         QStringLiteral("0 3 4 5 0 0 0 0 0"),
         QStringLiteral("0 0 1 2 0 0 0 0 0")}));

    QVector<PS1PLY::FaceMaterial> mats(3);
    // Face 0 -> texture slot 0.
    mats[0].textured = true;
    mats[0].textureIndex = 0;
    mats[0].u = {0.0f, 1.0f, 1.0f, 0.0f};
    mats[0].v = {0.0f, 0.0f, 1.0f, 0.0f};
    // Face 1 -> texture slot 1 (distinct slot from face 0).
    mats[1].textured = true;
    mats[1].textureIndex = 1;
    mats[1].u = {0.0f, 0.5f, 0.5f, 0.0f};
    mats[1].v = {0.0f, 0.0f, 0.5f, 0.0f};
    // Face 2 -> solid.
    mats[2].textured = false;
    mats[2].color = QColor(10, 20, 30);

    const std::string meshName = "PS1PlyCovMultiTexMesh";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);
    Ogre::MeshPtr mesh = PS1PLY::importPsyqPlyWithFaceMaterials(plyIn, meshName, mats);
    ASSERT_TRUE(mesh);

    // Three distinct buckets: tex0, tex1, solid.
    ASSERT_EQ(mesh->getNumSubMeshes(), 3u);

    bool foundTex0 = false, foundTex1 = false, foundSolid = false;
    for (unsigned int si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sm = mesh->getSubMesh(si);
        const std::string m = sm->getMaterialName();
        const bool isTex0 = (m.find("_tex0") != std::string::npos);
        const bool isTex1 = (m.find("_tex1") != std::string::npos);
        const bool isSolid = (m.find("_solid") != std::string::npos);
        EXPECT_TRUE(isTex0 || isTex1 || isSolid) << "Unexpected material: " << m;
        if (isTex0) foundTex0 = true;
        if (isTex1) foundTex1 = true;
        if (isSolid) foundSolid = true;

        ASSERT_NE(sm->vertexData, nullptr);
        EXPECT_GE(sm->vertexData->vertexCount, 3u);

        const Ogre::VertexElement* uvEl =
            sm->vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES);
        if (isTex0 || isTex1)
            EXPECT_NE(uvEl, nullptr) << "Textured submesh missing UV element: " << m;
        else
            EXPECT_EQ(uvEl, nullptr) << "Solid submesh should not carry UVs: " << m;
    }
    EXPECT_TRUE(foundTex0);
    EXPECT_TRUE(foundTex1);
    EXPECT_TRUE(foundSolid);

    Ogre::MeshManager::getSingleton().remove(meshName);
}

// ---------------------------------------------------------------------------
// 3. Per-corner vertColors path: a textured triangle (3 colours) and a textured quad
//    (4 colours), each wiring VES_DIFFUSE onto its submesh vertices.
// ---------------------------------------------------------------------------
TEST_F(PS1PLYExportCoverageTest, ImportPerCornerVertColorsWiresDiffuseOnTexturedSubmeshes)
{
    ASSERT_TRUE(canLoadMeshFiles());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // --- 3-corner case: textured triangle with three distinct vertex colours. ---
    {
        const QString plyTri = QDir(dir.path()).filePath(QStringLiteral("tri_vc.ply"));
        ASSERT_TRUE(writeSingleTriPly(plyTri));

        QVector<PS1PLY::FaceMaterial> mats(1);
        mats[0].textured = true;
        mats[0].textureIndex = 0;
        mats[0].u = {0.0f, 1.0f, 0.0f, 0.0f};
        mats[0].v = {0.0f, 0.0f, 1.0f, 0.0f};
        mats[0].vertColors = {QColor(255, 0, 0), QColor(0, 255, 0), QColor(0, 0, 255)};

        const std::string meshName = "PS1PlyCovVcTriMesh";
        if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
            Ogre::MeshManager::getSingleton().remove(old);
        Ogre::MeshPtr mesh = PS1PLY::importPsyqPlyWithFaceMaterials(plyTri, meshName, mats);
        ASSERT_TRUE(mesh);
        ASSERT_EQ(mesh->getNumSubMeshes(), 1u);

        const Ogre::VertexData* vd = mesh->getSubMesh(0)->vertexData;
        ASSERT_NE(vd, nullptr);
        EXPECT_NE(vd->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE), nullptr)
            << "3-corner vertColors path should wire VES_DIFFUSE.";
        EXPECT_NE(vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES), nullptr);
        // Three distinct corner colours -> three unique welded corners.
        EXPECT_GE(vd->vertexCount, 3u);

        Ogre::MeshManager::getSingleton().remove(meshName);
    }

    // --- 4-corner case: textured quad with four distinct vertex colours. ---
    {
        const QString plyQuad = QDir(dir.path()).filePath(QStringLiteral("quad_vc.ply"));
        ASSERT_TRUE(writeSingleQuadPly(plyQuad));

        QVector<PS1PLY::FaceMaterial> mats(1);
        mats[0].textured = true;
        mats[0].textureIndex = 0;
        mats[0].u = {0.0f, 1.0f, 1.0f, 0.0f};
        mats[0].v = {0.0f, 0.0f, 1.0f, 1.0f};
        mats[0].vertColors = {QColor(10, 10, 10), QColor(90, 90, 90),
                              QColor(170, 170, 170), QColor(250, 250, 250)};

        const std::string meshName = "PS1PlyCovVcQuadMesh";
        if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
            Ogre::MeshManager::getSingleton().remove(old);
        Ogre::MeshPtr mesh = PS1PLY::importPsyqPlyWithFaceMaterials(plyQuad, meshName, mats);
        ASSERT_TRUE(mesh);
        ASSERT_EQ(mesh->getNumSubMeshes(), 1u);

        const Ogre::VertexData* vd = mesh->getSubMesh(0)->vertexData;
        ASSERT_NE(vd, nullptr);
        EXPECT_NE(vd->vertexDeclaration->findElementBySemantic(Ogre::VES_DIFFUSE), nullptr)
            << "4-corner vertColors path should wire VES_DIFFUSE.";
        EXPECT_NE(vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES), nullptr);
        // Quad expands to two triangles -> 6 corner references, 4 unique positions/colours.
        EXPECT_EQ(mesh->getSubMesh(0)->indexData->indexCount, 6u);
        EXPECT_GE(vd->vertexCount, 4u);

        Ogre::MeshManager::getSingleton().remove(meshName);
    }
}

// ---------------------------------------------------------------------------
// 4. Multi-submesh export: one textured submesh + one solid submesh. Export must
//    distinguish faces by ExportFaceTexture::submeshIndex, with textured faces
//    flagged textured=true and solid faces textured=false.
// ---------------------------------------------------------------------------
TEST_F(PS1PLYExportCoverageTest, ExportSubmeshIndexDistinguishesTexturedFromSolidSubmesh)
{
    ASSERT_TRUE(canLoadMeshFiles());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString plyIn = QDir(dir.path()).filePath(QStringLiteral("tex_plus_solid.ply"));
    // Two independent triangles (6 verts), one normal, two tri faces.
    ASSERT_TRUE(writePsyqPly(
        plyIn, 6, 1, 2,
        {QStringLiteral("0 0 0"), QStringLiteral("1 0 0"), QStringLiteral("0 1 0"),
         QStringLiteral("2 0 0"), QStringLiteral("3 0 0"), QStringLiteral("2 1 0")},
        {QStringLiteral("0 0 1")},
        {QStringLiteral("0 0 1 2 0 0 0 0 0"),
         QStringLiteral("0 3 4 5 0 0 0 0 0")}));

    QVector<PS1PLY::FaceMaterial> mats(2);
    // Face 0 -> textured slot 0.
    mats[0].textured = true;
    mats[0].textureIndex = 0;
    mats[0].u = {0.0f, 1.0f, 0.0f, 0.0f};
    mats[0].v = {0.0f, 0.0f, 1.0f, 0.0f};
    // Face 1 -> solid (no UVs, flat colour).
    mats[1].textured = false;
    mats[1].color = QColor(123, 45, 67);

    const std::string meshName = "PS1PlyCovTexSolidMesh";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);
    Ogre::MeshPtr mesh = PS1PLY::importPsyqPlyWithFaceMaterials(plyIn, meshName, mats);
    ASSERT_TRUE(mesh);
    ASSERT_EQ(mesh->getNumSubMeshes(), 2u);

    auto* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode(QStringLiteral("PS1PlyCovTexSolidNode"));
    ASSERT_NE(node, nullptr);
    Ogre::Entity* ent = mgr->createEntity(node, mesh);
    ASSERT_NE(ent, nullptr);

    QTemporaryFile outPly(QDir::tempPath() + QStringLiteral("/qtmesh_ps1ply_cov_multisub_XXXXXX.ply"));
    outPly.setAutoRemove(true);
    ASSERT_TRUE(outPly.open());
    outPly.close();

    QVector<QColor> faceColors;
    QVector<PS1PLY::ExportFaceTexture> faceTex;
    QString err;
    ASSERT_TRUE(PS1PLY::exportPsyqPlyFromEntity(ent, outPly.fileName(),
                                                &faceColors, &faceTex, &err))
        << err.toUtf8().constData();

    // Two faces in -> two faces out (textured submeshes disable quad merging, and the
    // solid triangle has no coplanar partner to merge with).
    ASSERT_EQ(faceTex.size(), 2);

    int texturedFaces = 0;
    int solidFaces = 0;
    int texturedSubmeshIndex = -1;
    int solidSubmeshIndex = -1;
    for (const auto& f : faceTex) {
        EXPECT_GE(f.submeshIndex, 0);
        EXPECT_TRUE(f.cornerCount == 3 || f.cornerCount == 4);
        if (f.textured) {
            ++texturedFaces;
            texturedSubmeshIndex = f.submeshIndex;
        } else {
            ++solidFaces;
            solidSubmeshIndex = f.submeshIndex;
        }
    }
    EXPECT_EQ(texturedFaces, 1);
    EXPECT_EQ(solidFaces, 1);
    // The two output faces originate from different submeshes — submeshIndex must differ.
    EXPECT_NE(texturedSubmeshIndex, solidSubmeshIndex);

    mgr->destroySceneNode(QStringLiteral("PS1PlyCovTexSolidNode"));
    Ogre::MeshManager::getSingleton().remove(meshName);
}
