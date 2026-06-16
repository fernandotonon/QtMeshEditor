#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMaterialManager.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgrePass.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>
#include <OgreTechnique.h>
#include <OgreTextureUnitState.h>
#include <OgreVertexIndexData.h>

#include "ApplyAtlas.h"
#include "Manager.h"
#include "TestHelpers.h"

// Coverage suite for ApplyAtlas::applyToEntity — the central ~125-line Ogre
// execution path that the standalone ApplyAtlas_test.cpp does not touch.
//
// Distinct filename + distinct suite name (ApplyAtlasCoverageTest) from the
// existing ApplyAtlasStandaloneTest so there is no duplicate registration.
//
// Strategy: build manual meshes with a FLOAT2 UV0 channel and a material
// whose first pass has a named "diffuse_map" TUS (and sometimes an extra
// "normal" TUS to exercise the strip path). Drive applyToEntity directly
// with a hand-built Manifest, then re-lock the vertex buffer to assert the
// UVs landed inside the tile sub-rect.

using namespace ApplyAtlas;

namespace {

// Create (or fetch) a material with a named "diffuse_map" TUS pointing at
// `diffuseTex`. If `extraTex` is non-empty, add a second TUS named "normal"
// to exercise stripNonDiffuseTexUnits.
Ogre::MaterialPtr makeMaterial(const std::string& matName,
                               const std::string& diffuseTex,
                               const std::string& extraTex)
{
    auto& mm = Ogre::MaterialManager::getSingleton();
    if (auto existing = mm.getByName(matName, Ogre::RGN_DEFAULT))
        mm.remove(existing);
    auto mat = mm.create(matName, Ogre::RGN_DEFAULT);
    auto* pass = mat->getTechnique(0)->getPass(0);

    auto* diff = pass->createTextureUnitState();
    diff->setName("diffuse_map");
    diff->setTextureName(diffuseTex);

    if (!extraTex.empty()) {
        auto* nrm = pass->createTextureUnitState();
        nrm->setName("normal");
        nrm->setTextureName(extraTex);
    }
    return mat;
}

// Build a single-submesh mesh with shared FLOAT2 UVs. uvScale lets a caller
// push a vertex outside [0..1] to exercise the clamp / no-clamp branches.
Ogre::MeshPtr makeUvMesh(const std::string& name, float uvScale = 1.0f)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::RGN_DEFAULT);

    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC);
    const float u = uvScale;
    float verts[] = {
        0,0,0,  0,0,1,  0.0f,   0.0f,
        1,0,0,  0,0,1,  u,      0.0f,
        0,1,0,  0,0,1,  0.0f,   u,
    };
    vbuf->writeData(0, sizeof(verts), verts);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 3;

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;

    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,1,1,1));
    mesh->_setBoundingSphereRadius(2.0);
    mesh->load();
    return mesh;
}

// Build a two-submesh mesh, each submesh with its own FLOAT2 UV buffer.
// Used for the shared-material dedup case.
Ogre::MeshPtr makeTwoSubmeshUvMesh(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::RGN_DEFAULT);

    for (int s = 0; s < 2; ++s) {
        auto* sub = mesh->createSubMesh();
        sub->useSharedVertices = false;
        sub->vertexData = new Ogre::VertexData();
        auto* decl = sub->vertexData->vertexDeclaration;
        size_t offset = 0;
        decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
        offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
        decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

        auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC);
        const float base = static_cast<float>(s) * 10.0f;
        float verts[] = {
            base+0,0,0,  0.0f,0.0f,
            base+1,0,0,  1.0f,0.0f,
            base+0,1,0,  0.0f,1.0f,
        };
        vbuf->writeData(0, sizeof(verts), verts);
        sub->vertexData->vertexBufferBinding->setBinding(0, vbuf);
        sub->vertexData->vertexCount = 3;

        auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            Ogre::HardwareIndexBuffer::IT_16BIT, 3,
            Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        uint16_t idx[] = {0, 1, 2};
        ibuf->writeData(0, sizeof(idx), idx);
        sub->indexData->indexBuffer = ibuf;
        sub->indexData->indexCount = 3;
    }

    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,11,1,1));
    mesh->_setBoundingSphereRadius(12.0);
    mesh->load();
    return mesh;
}

// A tile occupying the top-right quadrant of the atlas so the remap is
// observable: u in [0.5, 1.0], v in [0.5, 1.0].
ManifestTile quadrantTile(const QString& source)
{
    ManifestTile t;
    t.sourcePath = source;
    t.x = 512; t.y = 512; t.w = 512; t.h = 512;
    t.u0 = 0.5f; t.v0 = 0.5f; t.u1 = 1.0f; t.v1 = 1.0f;
    return t;
}

Manifest manifestWith(const ManifestTile& t)
{
    Manifest m;
    m.width = 1024;
    m.height = 1024;
    m.padding = 0;
    m.tiles.append(t);
    return m;
}

// Read back UV0 of the shared (or first sub's) vertex buffer.
struct Uv { float u, v; };
std::vector<Uv> readUv0(Ogre::Mesh* mesh, unsigned int submeshIndex = 0)
{
    std::vector<Uv> out;
    auto* sub = mesh->getSubMesh(submeshIndex);
    Ogre::VertexData* vData = sub->useSharedVertices ? mesh->sharedVertexData
                                                     : sub->vertexData;
    if (!vData) return out;
    const auto* el = vData->vertexDeclaration->findElementBySemantic(
        Ogre::VES_TEXTURE_COORDINATES);
    if (!el) return out;
    auto vbuf = vData->vertexBufferBinding->getBuffer(el->getSource());
    auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    const size_t stride = vbuf->getVertexSize();
    for (size_t v = 0; v < vData->vertexCount; ++v) {
        float* uv = nullptr;
        el->baseVertexPointerToElement(base + v * stride, &uv);
        out.push_back({uv[0], uv[1]});
    }
    vbuf->unlock();
    return out;
}

} // namespace

class ApplyAtlasCoverageTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    Ogre::SceneManager* sceneMgr = nullptr;
    int counter = 0;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed — invalid CI/runtime environment";
        createStandardOgreMaterials();
        sceneMgr = Manager::getSingleton()->getSceneMgr();
        ASSERT_NE(sceneMgr, nullptr);
    }

    void TearDown() override {
        if (app) app->processEvents();
        Manager::kill();
        QThread::msleep(20);
    }

    // Unique suffix so meshes/materials/entities don't collide across tests.
    std::string uniq(const std::string& base) {
        return base + "_" + std::to_string(reinterpret_cast<uintptr_t>(this))
             + "_" + std::to_string(counter++);
    }

    Ogre::Entity* makeEntity(const Ogre::MeshPtr& mesh, const std::string& entName) {
        return sceneMgr->createEntity(entName, mesh);
    }
};

// ---- error / guard branches on the live entity ---------------------------

TEST_F(ApplyAtlasCoverageTest, NullEntityReturnsError) {
    Manifest m = manifestWith(quadrantTile("soccer.png"));
    ApplyOptions opts; opts.atlasTextureName = "atlas.png";
    const ApplyReport r = applyToEntity(nullptr, m, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error.toStdString(), "entity is null");
}

TEST_F(ApplyAtlasCoverageTest, EmptyTilesReturnsError) {
    auto mesh = makeUvMesh(uniq("at_empty"));
    auto* mat = makeMaterial(uniq("mat_empty"), "soccer.png", "").get();
    auto* ent = makeEntity(mesh, uniq("ent_empty"));
    ent->setMaterialName(mat->getName());

    Manifest m; m.width = 1024; m.height = 1024; // no tiles
    ApplyOptions opts; opts.atlasTextureName = "atlas.png";
    const ApplyReport r = applyToEntity(ent, m, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error.toStdString(), "manifest has no tiles");
}

TEST_F(ApplyAtlasCoverageTest, EmptyAtlasTextureNameReturnsError) {
    auto mesh = makeUvMesh(uniq("at_noatlas"));
    auto* ent = makeEntity(mesh, uniq("ent_noatlas"));

    Manifest m = manifestWith(quadrantTile("soccer.png"));
    ApplyOptions opts; // atlasTextureName left empty
    const ApplyReport r = applyToEntity(ent, m, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error.toStdString(), "atlasTextureName is empty");
}

// ---- happy path: match + UV rewrite + diffuse retarget -------------------

TEST_F(ApplyAtlasCoverageTest, MatchedSubmeshRewritesUvAndRetargetsDiffuse) {
    auto mesh = makeUvMesh(uniq("at_happy"));
    auto* mat = makeMaterial(uniq("mat_happy"), "soccer.png", "").get();
    auto* ent = makeEntity(mesh, uniq("ent_happy"));
    ent->setMaterialName(mat->getName());

    Manifest m = manifestWith(quadrantTile("/some/path/soccer.png"));
    ApplyOptions opts;
    opts.atlasTextureName = "atlas.png";
    opts.matchMode = MatchMode::Basename; // basename match: soccer.png

    const ApplyReport r = applyToEntity(ent, m, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.submeshCount(), 1);
    EXPECT_EQ(r.rewrittenCount(), 1);
    ASSERT_EQ(r.submeshes.size(), 1);
    const SubmeshReport& s = r.submeshes[0];
    EXPECT_EQ(s.submeshIndex, 0);
    EXPECT_TRUE(s.uvsRewritten);
    EXPECT_TRUE(s.materialUpdated);
    EXPECT_GT(s.verticesTouched, 0);
    EXPECT_EQ(s.outOfRangeUVs, 0);
    EXPECT_EQ(s.diffuseTextureName.toStdString(), "soccer.png");
    EXPECT_EQ(s.matchedTileSource.toStdString(), "/some/path/soccer.png");

    // UV0 must now fall inside [0.5,1.0]x[0.5,1.0].
    const auto uvs = readUv0(mesh.get());
    ASSERT_EQ(uvs.size(), 3u);
    for (const auto& uv : uvs) {
        EXPECT_GE(uv.u, 0.5f - 1e-4f);
        EXPECT_LE(uv.u, 1.0f + 1e-4f);
        EXPECT_GE(uv.v, 0.5f - 1e-4f);
        EXPECT_LE(uv.v, 1.0f + 1e-4f);
    }
    // Vertex (0,0) maps to (u0,v0); vertex with uv (1,0) maps to (u1,v0).
    EXPECT_NEAR(uvs[0].u, 0.5f, 1e-4f);
    EXPECT_NEAR(uvs[0].v, 0.5f, 1e-4f);
    EXPECT_NEAR(uvs[1].u, 1.0f, 1e-4f);
    EXPECT_NEAR(uvs[2].v, 1.0f, 1e-4f);

    // The diffuse TUS must now point at the atlas.
    auto* tus = ent->getSubEntity(0)->getMaterial()
                   ->getTechnique(0)->getPass(0)->getTextureUnitState("diffuse_map");
    ASSERT_NE(tus, nullptr);
    EXPECT_EQ(tus->getTextureName(), std::string("atlas.png"));
}

// ---- FullPath match mode -------------------------------------------------

TEST_F(ApplyAtlasCoverageTest, FullPathMatchMode) {
    auto mesh = makeUvMesh(uniq("at_full"));
    auto* mat = makeMaterial(uniq("mat_full"), "/abs/dir/soccer.png", "").get();
    auto* ent = makeEntity(mesh, uniq("ent_full"));
    ent->setMaterialName(mat->getName());

    Manifest m = manifestWith(quadrantTile("/abs/dir/soccer.png"));
    ApplyOptions opts;
    opts.atlasTextureName = "atlas.png";
    opts.matchMode = MatchMode::FullPath;

    const ApplyReport r = applyToEntity(ent, m, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.rewrittenCount(), 1);
}

TEST_F(ApplyAtlasCoverageTest, FullPathMatchFailsOnBasenameOnlyTile) {
    auto mesh = makeUvMesh(uniq("at_fullmiss"));
    auto* mat = makeMaterial(uniq("mat_fullmiss"), "/abs/dir/soccer.png", "").get();
    auto* ent = makeEntity(mesh, uniq("ent_fullmiss"));
    ent->setMaterialName(mat->getName());

    // Tile source is just the basename — full-path compare must NOT match.
    Manifest m = manifestWith(quadrantTile("soccer.png"));
    ApplyOptions opts;
    opts.atlasTextureName = "atlas.png";
    opts.matchMode = MatchMode::FullPath;

    const ApplyReport r = applyToEntity(ent, m, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.rewrittenCount(), 0);
    EXPECT_FALSE(r.submeshes[0].uvsRewritten);
    EXPECT_TRUE(r.submeshes[0].note.contains("no manifest tile"));
}

// ---- no-match path -------------------------------------------------------

TEST_F(ApplyAtlasCoverageTest, NoMatchSubmeshGetsNoteAndNoRewrite) {
    auto mesh = makeUvMesh(uniq("at_nomatch"));
    auto* mat = makeMaterial(uniq("mat_nomatch"), "totally_other.png", "").get();
    auto* ent = makeEntity(mesh, uniq("ent_nomatch"));
    ent->setMaterialName(mat->getName());

    Manifest m = manifestWith(quadrantTile("soccer.png"));
    ApplyOptions opts; opts.atlasTextureName = "atlas.png";

    const ApplyReport r = applyToEntity(ent, m, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.rewrittenCount(), 0);
    ASSERT_EQ(r.submeshes.size(), 1);
    EXPECT_FALSE(r.submeshes[0].uvsRewritten);
    EXPECT_EQ(r.submeshes[0].verticesTouched, 0);
    EXPECT_TRUE(r.submeshes[0].matchedTileSource.isEmpty());
    EXPECT_TRUE(r.submeshes[0].note.contains("no manifest tile"));

    // Diffuse TUS untouched.
    auto* tus = ent->getSubEntity(0)->getMaterial()
                   ->getTechnique(0)->getPass(0)->getTextureUnitState("diffuse_map");
    ASSERT_NE(tus, nullptr);
    EXPECT_EQ(tus->getTextureName(), std::string("totally_other.png"));
}

// ---- out-of-range UV: clamp branch ---------------------------------------

TEST_F(ApplyAtlasCoverageTest, OutOfRangeUvClamped) {
    // uvScale=2 pushes verts 1 and 2 to u/v == 2.0 (outside [0..1]).
    auto mesh = makeUvMesh(uniq("at_clamp"), 2.0f);
    auto* mat = makeMaterial(uniq("mat_clamp"), "soccer.png", "").get();
    auto* ent = makeEntity(mesh, uniq("ent_clamp"));
    ent->setMaterialName(mat->getName());

    Manifest m = manifestWith(quadrantTile("soccer.png"));
    ApplyOptions opts;
    opts.atlasTextureName = "atlas.png";
    opts.clampOutOfRangeUVs = true;

    const ApplyReport r = applyToEntity(ent, m, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.submeshes.size(), 1);
    const SubmeshReport& s = r.submeshes[0];
    EXPECT_TRUE(s.uvsRewritten);
    EXPECT_EQ(s.verticesTouched, 3); // all touched (clamped ones still written)
    EXPECT_GT(s.outOfRangeUVs, 0);   // at least the 2.0 verts counted

    // Clamped UVs must still land inside the tile rect.
    const auto uvs = readUv0(mesh.get());
    for (const auto& uv : uvs) {
        EXPECT_GE(uv.u, 0.5f - 1e-4f);
        EXPECT_LE(uv.u, 1.0f + 1e-4f);
        EXPECT_GE(uv.v, 0.5f - 1e-4f);
        EXPECT_LE(uv.v, 1.0f + 1e-4f);
    }
}

// ---- out-of-range UV: no-clamp skip branch + note ------------------------

TEST_F(ApplyAtlasCoverageTest, OutOfRangeUvNotClampedSkipsAndNotes) {
    auto mesh = makeUvMesh(uniq("at_noclamp"), 2.0f);
    auto* mat = makeMaterial(uniq("mat_noclamp"), "soccer.png", "").get();
    auto* ent = makeEntity(mesh, uniq("ent_noclamp"));
    ent->setMaterialName(mat->getName());

    Manifest m = manifestWith(quadrantTile("soccer.png"));
    ApplyOptions opts;
    opts.atlasTextureName = "atlas.png";
    opts.clampOutOfRangeUVs = false;

    const ApplyReport r = applyToEntity(ent, m, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.submeshes.size(), 1);
    const SubmeshReport& s = r.submeshes[0];
    EXPECT_GT(s.outOfRangeUVs, 0);
    // Only the in-range vertex (vertex 0 at uv 0,0) is touched.
    EXPECT_EQ(s.verticesTouched, 1);
    EXPECT_TRUE(s.uvsRewritten);
    EXPECT_TRUE(s.note.contains("clampOutOfRangeUVs=false")) << s.note.toStdString();

    // The out-of-range verts should be left at their original 2.0 value
    // (skipped), while vertex 0 was remapped into the tile.
    const auto uvs = readUv0(mesh.get());
    ASSERT_EQ(uvs.size(), 3u);
    EXPECT_NEAR(uvs[0].u, 0.5f, 1e-4f);
    EXPECT_NEAR(uvs[0].v, 0.5f, 1e-4f);
    // Vertex 1 (u was 2.0) untouched.
    EXPECT_NEAR(uvs[1].u, 2.0f, 1e-4f);
}

// ---- stripNonDiffuseTextures = true --------------------------------------

TEST_F(ApplyAtlasCoverageTest, StripsNonDiffuseTextureUnits) {
    auto mesh = makeUvMesh(uniq("at_strip"));
    auto* mat = makeMaterial(uniq("mat_strip"), "soccer.png", "soccer_normal.png").get();
    auto* ent = makeEntity(mesh, uniq("ent_strip"));
    ent->setMaterialName(mat->getName());

    // Pre-condition: 2 TUSes.
    EXPECT_EQ(mat->getTechnique(0)->getPass(0)->getNumTextureUnitStates(), 2u);

    Manifest m = manifestWith(quadrantTile("soccer.png"));
    ApplyOptions opts;
    opts.atlasTextureName = "atlas.png";
    opts.stripNonDiffuseTextures = true;

    const ApplyReport r = applyToEntity(ent, m, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.submeshes.size(), 1);
    EXPECT_EQ(r.submeshes[0].strippedExtraTextures, 1);

    // Only the diffuse TUS should remain, pointing at the atlas.
    auto* pass = ent->getSubEntity(0)->getMaterial()->getTechnique(0)->getPass(0);
    EXPECT_EQ(pass->getNumTextureUnitStates(), 1u);
    auto* tus = pass->getTextureUnitState("diffuse_map");
    ASSERT_NE(tus, nullptr);
    EXPECT_EQ(tus->getTextureName(), std::string("atlas.png"));
}

// ---- stripNonDiffuseTextures = false -------------------------------------

TEST_F(ApplyAtlasCoverageTest, KeepsNonDiffuseTextureUnitsWhenStripDisabled) {
    auto mesh = makeUvMesh(uniq("at_keep"));
    auto* mat = makeMaterial(uniq("mat_keep"), "soccer.png", "soccer_normal.png").get();
    auto* ent = makeEntity(mesh, uniq("ent_keep"));
    ent->setMaterialName(mat->getName());

    Manifest m = manifestWith(quadrantTile("soccer.png"));
    ApplyOptions opts;
    opts.atlasTextureName = "atlas.png";
    opts.stripNonDiffuseTextures = false;

    const ApplyReport r = applyToEntity(ent, m, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.submeshes.size(), 1);
    EXPECT_EQ(r.submeshes[0].strippedExtraTextures, 0);

    // Both TUSes remain; diffuse retargeted, normal intact.
    auto* pass = ent->getSubEntity(0)->getMaterial()->getTechnique(0)->getPass(0);
    EXPECT_EQ(pass->getNumTextureUnitStates(), 2u);
    EXPECT_EQ(pass->getTextureUnitState("diffuse_map")->getTextureName(),
              std::string("atlas.png"));
    EXPECT_EQ(pass->getTextureUnitState("normal")->getTextureName(),
              std::string("soccer_normal.png"));
}

// ---- shared-material dedup across two submeshes --------------------------

TEST_F(ApplyAtlasCoverageTest, SharedMaterialRetargetedOnceBothUvsRewritten) {
    auto mesh = makeTwoSubmeshUvMesh(uniq("at_shared"));
    // One material shared by both submeshes (the Mixamo Skin_MAT case).
    auto* mat = makeMaterial(uniq("mat_shared"), "soccer.png", "soccer_normal.png").get();
    auto* ent = makeEntity(mesh, uniq("ent_shared"));
    ent->setMaterialName(mat->getName()); // applies to all submeshes

    ASSERT_EQ(ent->getNumSubEntities(), 2u);

    Manifest m = manifestWith(quadrantTile("soccer.png"));
    ApplyOptions opts;
    opts.atlasTextureName = "atlas.png";
    opts.stripNonDiffuseTextures = true;

    const ApplyReport r = applyToEntity(ent, m, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.submeshes.size(), 2);

    // Both submeshes had UVs rewritten (per-submesh operation).
    EXPECT_TRUE(r.submeshes[0].uvsRewritten);
    EXPECT_TRUE(r.submeshes[1].uvsRewritten);
    EXPECT_EQ(r.rewrittenCount(), 2);

    // Both report materialUpdated true...
    EXPECT_TRUE(r.submeshes[0].materialUpdated);
    EXPECT_TRUE(r.submeshes[1].materialUpdated);

    // ...but the strip / retarget happened exactly once (dedup), so the
    // second submesh reports 0 stripped extras (it took the shared-material
    // branch) while the first stripped the single normal TUS.
    const int totalStripped = r.submeshes[0].strippedExtraTextures
                            + r.submeshes[1].strippedExtraTextures;
    EXPECT_EQ(totalStripped, 1);

    // Both submeshes' UVs landed inside the tile sub-rect.
    for (unsigned int s = 0; s < 2; ++s) {
        const auto uvs = readUv0(mesh.get(), s);
        ASSERT_EQ(uvs.size(), 3u);
        for (const auto& uv : uvs) {
            EXPECT_GE(uv.u, 0.5f - 1e-4f);
            EXPECT_LE(uv.u, 1.0f + 1e-4f);
            EXPECT_GE(uv.v, 0.5f - 1e-4f);
            EXPECT_LE(uv.v, 1.0f + 1e-4f);
        }
    }

    // The shared material's diffuse points at the atlas; the (single) pass
    // now has just the diffuse TUS.
    auto* pass = mat->getTechnique(0)->getPass(0);
    EXPECT_EQ(pass->getNumTextureUnitStates(), 1u);
    EXPECT_EQ(pass->getTextureUnitState("diffuse_map")->getTextureName(),
              std::string("atlas.png"));
}

// ---- fallback diffuse targeting (no named diffuse_map TUS) ---------------

TEST_F(ApplyAtlasCoverageTest, FallbackToFirstTusWhenNoNamedDiffuse) {
    auto mesh = makeUvMesh(uniq("at_fallback"));
    auto& mm = Ogre::MaterialManager::getSingleton();
    const std::string matName = uniq("mat_fallback");
    auto mat = mm.create(matName, Ogre::RGN_DEFAULT);
    // Unnamed first TUS with a real texture — exercises the fallback path
    // in diffuseTexNameForSubEntity / retargetDiffuseTus / strip.
    auto* tus = mat->getTechnique(0)->getPass(0)->createTextureUnitState();
    tus->setTextureName("soccer.png");

    auto* ent = makeEntity(mesh, uniq("ent_fallback"));
    ent->setMaterialName(matName);

    Manifest m = manifestWith(quadrantTile("soccer.png"));
    ApplyOptions opts;
    opts.atlasTextureName = "atlas.png";

    const ApplyReport r = applyToEntity(ent, m, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.submeshes.size(), 1);
    EXPECT_EQ(r.submeshes[0].diffuseTextureName.toStdString(), "soccer.png");
    EXPECT_TRUE(r.submeshes[0].uvsRewritten);
    EXPECT_TRUE(r.submeshes[0].materialUpdated);

    auto* pass = ent->getSubEntity(0)->getMaterial()->getTechnique(0)->getPass(0);
    EXPECT_EQ(pass->getTextureUnitState(0)->getTextureName(), std::string("atlas.png"));
}

// ---- report JSON for a real apply ----------------------------------------

TEST_F(ApplyAtlasCoverageTest, RealApplyReportSerialisesToJson) {
    auto mesh = makeUvMesh(uniq("at_json"));
    auto* mat = makeMaterial(uniq("mat_json"), "soccer.png", "").get();
    auto* ent = makeEntity(mesh, uniq("ent_json"));
    ent->setMaterialName(mat->getName());

    Manifest m = manifestWith(quadrantTile("soccer.png"));
    ApplyOptions opts; opts.atlasTextureName = "atlas.png";

    const ApplyReport r = applyToEntity(ent, m, opts);
    ASSERT_TRUE(r.ok);
    const QJsonObject o = r.toJson();
    EXPECT_TRUE(o.value("ok").toBool());
    EXPECT_EQ(o.value("submeshCount").toInt(), 1);
    EXPECT_EQ(o.value("rewrittenCount").toInt(), 1);
    ASSERT_TRUE(o.value("submeshes").isArray());
    const QJsonObject sub = o.value("submeshes").toArray().first().toObject();
    EXPECT_EQ(sub.value("diffuseTextureName").toString(), QString("soccer.png"));
    EXPECT_TRUE(sub.value("uvsRewritten").toBool());
    EXPECT_GT(sub.value("verticesTouched").toInt(), 0);
}
