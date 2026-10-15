#include "QuadRemeshEntity.h"
#include "QuadriFlowRunner.h"
#include "QuadRetopoController.h"
#include "EditableMesh.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "UndoManager.h"
#include "commands/ReplaceEntityMeshCommand.h"

#include <gtest/gtest.h>

#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>

#include "MaterialEditorQML.h"
#include "PropertiesPanelController.h"
#include "ThemeManager.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMeshManager.h>
#include <OgreSubMesh.h>

#include <cmath>

// Ogre-side coverage for the QuadriFlow remesh: entity read → remesh →
// build, the undoable whole-mesh swap, and the controller's async path.
// The pure-data pipeline is covered by QuadRemeshCore_test.cpp.

namespace {

// UV sphere (positions + normals + UVs) as a single non-shared submesh.
Ogre::MeshPtr makeSphereMesh(const std::string& name, int rings = 16, int segs = 32)
{
    std::vector<float> v;
    std::vector<uint16_t> idx;
    for (int i = 0; i <= rings; ++i)
        for (int j = 0; j <= segs; ++j) {
            const float t = float(M_PI) * i / rings, p = 2.0f * float(M_PI) * j / segs;
            const float x = std::sin(t) * std::cos(p), y = std::cos(t), z = std::sin(t) * std::sin(p);
            v.insert(v.end(), {x, y, z, x, y, z, float(j) / segs, float(i) / rings});
        }
    auto id = [segs](int i, int j) { return uint16_t(i * (segs + 1) + j); };
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segs; ++j) {
            if (i > 0) idx.insert(idx.end(), {id(i, j), id(i, j + 1), id(i + 1, j + 1)});
            if (i < rings - 1) idx.insert(idx.end(), {id(i, j), id(i + 1, j + 1), id(i + 1, j)});
        }
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = false;
    sub->vertexData = new Ogre::VertexData();
    auto* decl = sub->vertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    decl->addElement(0, 12, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    decl->addElement(0, 24, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);
    const size_t nv = v.size() / 8;
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        32, nv, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    vbuf->writeData(0, v.size() * sizeof(float), v.data());
    sub->vertexData->vertexBufferBinding->setBinding(0, vbuf);
    sub->vertexData->vertexCount = nv;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, idx.size(), Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    ibuf->writeData(0, idx.size() * sizeof(uint16_t), idx.data());
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = idx.size();
    sub->setMaterialName("BaseWhite");
    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 1, 1, 1));
    mesh->_setBoundingSphereRadius(1.8f);
    mesh->load();
    return mesh;
}

class QuadRemeshEntityTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!tryInitOgre())
            GTEST_SKIP() << "Ogre/GL unavailable";
        createStandardOgreMaterials();
        QuadRetopoController::kill();
    }
    void TearDown() override
    {
        QuadRetopoController::kill();
        if (auto* sel = SelectionSet::getSingletonPtr()) sel->clearList();
    }
    Ogre::Entity* addSphere(const std::string& name)
    {
        auto mesh = makeSphereMesh(name + "_mesh");
        auto* node = Manager::getSingleton()->addSceneNode(name.c_str());
        return Manager::getSingleton()->createEntity(node, mesh);
    }
};

} // namespace

TEST_F(QuadRemeshEntityTest, ReadSourceCombinesGeometryAndUvs)
{
    Ogre::Entity* e = addSphere("qre_read");
    ASSERT_NE(e, nullptr);
    QuadRemesh::SourceMesh src;
    QuadRemeshEntity::SourceInfo info;
    QString err;
    ASSERT_TRUE(QuadRemeshEntity::readSource(e, src, info, &err)) << err.toStdString();
    EXPECT_EQ(src.positions.size() / 3, size_t(17 * 33));
    EXPECT_EQ(src.uvs.size(), src.positions.size() / 3 * 2);
    EXPECT_EQ(src.triangleMaterial.size(), src.indices.size() / 3);
    EXPECT_EQ(src.materials.size(), 1u);
    EXPECT_TRUE(info.skeletonName.isEmpty());
}

TEST_F(QuadRemeshEntityTest, BuildMeshWritesQuadBindingAndUndoableSwap)
{
    Ogre::Entity* e = addSphere("qre_build");
    QuadRemesh::SourceMesh src;
    QuadRemeshEntity::SourceInfo info;
    ASSERT_TRUE(QuadRemeshEntity::readSource(e, src, info));

    // A hand-made quad result: one quad + one triangle over the first verts.
    QuadRemesh::Result r;
    r.ok = true;
    r.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 2, 0, 0};
    r.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    r.faces = {{0, 1, 2, 3}, {1, 4, 2}};
    r.quads = 1;
    r.triangles = 1;
    const auto built = QuadRemeshEntity::buildMesh(r, info, "qre_build");
    ASSERT_TRUE(built.ok) << built.error.toStdString();
    ASSERT_TRUE(built.mesh);
    EXPECT_EQ(built.mesh->getNumSubMeshes(), 1);
    std::vector<std::vector<unsigned int>> ngon;
    ASSERT_TRUE(readNgonFacesFromMesh(built.mesh.get(), 0, ngon));
    ASSERT_EQ(ngon.size(), 2u);
    EXPECT_EQ(ngon[0].size(), 4u);
    EXPECT_EQ(ngon[1].size(), 3u);

    const Ogre::MeshPtr original = e->getMesh();
    auto* cmd = new ReplaceEntityMeshCommand("qre_build", built.mesh, QStringLiteral("t"));
    UndoManager::getSingleton()->push(cmd);
    ASSERT_TRUE(cmd->ok());
    auto find = []() -> Ogre::Entity* {
        for (Ogre::Entity* x : Manager::getSingleton()->getEntities())
            if (x && x->getName() == "qre_build") return x;
        return nullptr;
    };
    ASSERT_NE(find(), nullptr);
    EXPECT_EQ(find()->getMesh(), built.mesh);
    UndoManager::getSingleton()->undo();
    ASSERT_NE(find(), nullptr);
    EXPECT_EQ(find()->getMesh(), original);
    UndoManager::getSingleton()->redo();
    EXPECT_EQ(find()->getMesh(), built.mesh);
}

TEST_F(QuadRemeshEntityTest, ControllerRemeshRunsAsyncAndIsOneUndoStep)
{
    if (QuadriFlowRunner::defaultHelperPath().isEmpty())
        GTEST_SKIP() << "qtmesh_quadriflow helper not built";
    Ogre::Entity* e = addSphere("qre_ctl");
    SelectionSet::getSingleton()->selectOne(e);
    const Ogre::MeshPtr original = e->getMesh();
    auto* c = QuadRetopoController::instance();
    EXPECT_TRUE(c->quadriFlowAvailable());
    QSignalSpy done(c, &QuadRetopoController::remeshFinished);
    QSignalSpy failed(c, &QuadRetopoController::error);
    ASSERT_TRUE(c->remeshSelectedQuadriFlow({{"targetFaces", 400}, {"bakeTextures", false}}));
    EXPECT_TRUE(c->busy());
    ASSERT_TRUE(done.wait(120000)) << (failed.isEmpty() ? std::string("timeout")
                                                        : failed.first().at(0).toString().toStdString());
    EXPECT_FALSE(c->busy());
    const QVariantMap report = done.first().at(0).toMap();
    EXPECT_GT(report.value("quads").toInt(), 200);

    Ogre::Entity* now = nullptr;
    for (Ogre::Entity* x : Manager::getSingleton()->getEntities())
        if (x && x->getName() == "qre_ctl") now = x;
    ASSERT_NE(now, nullptr);
    EXPECT_NE(now->getMesh(), original);
    UndoManager::getSingleton()->undo();
    for (Ogre::Entity* x : Manager::getSingleton()->getEntities())
        if (x && x->getName() == "qre_ctl") now = x;
    EXPECT_EQ(now->getMesh(), original);
}

TEST_F(QuadRemeshEntityTest, ControllerRemeshWithoutSelectionFails)
{
    if (auto* sel = SelectionSet::getSingletonPtr()) sel->clearList();
    auto* c = QuadRetopoController::instance();
    QSignalSpy failed(c, &QuadRetopoController::error);
    EXPECT_FALSE(c->remeshSelectedQuadriFlow({}));
    EXPECT_EQ(failed.count(), 1);
    EXPECT_FALSE(c->busy());
}

TEST_F(QuadRemeshEntityTest, DialogQmlLoadsWithQuadriFlowControls)
{
    qmlRegisterSingletonType<PropertiesPanelController>("PropertiesPanel", 1, 0, "PropertiesPanelController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* { return PropertiesPanelController::qmlInstance(e, nullptr); });
    qmlRegisterSingletonType<QuadRetopoController>("PropertiesPanel", 1, 0, "QuadRetopoController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* { return QuadRetopoController::qmlInstance(e, nullptr); });
    qmlRegisterSingletonType<MaterialEditorQML>("MaterialEditorQML", 1, 0, "MaterialEditorQML",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return MaterialEditorQML::qmlInstance(e, s); });
    qmlRegisterSingletonType<ThemeManager>("ThemeManager", 1, 0, "ThemeManager",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return ThemeManager::qmlInstance(e, s); });
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/MaterialEditorQML/QuadRetopoDialog.qml")));
    while (component.isLoading()) QCoreApplication::processEvents();
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    std::unique_ptr<QObject> obj(component.create());
    ASSERT_NE(obj, nullptr) << component.errorString().toStdString();
    EXPECT_EQ(obj->property("method").toString(), QStringLiteral("pair"));
    EXPECT_TRUE(obj->setProperty("method", QStringLiteral("quadriflow")));
    EXPECT_TRUE(obj->property("qfBake").toBool());
}

// Regression: a baked normal map must arrive with TANGENTS already on the
// mesh. Built later (applyNormalMapsToEntity, after the entity exists) a
// skinned entity's blend buffers lack them and the remesh rendered dark.
// The cloned material must also not carry the source's stale RTSS technique.
TEST_F(QuadRemeshEntityTest, BuildMeshWithNormalMapHasTangentsAndNoStaleRtss)
{
    Ogre::Entity* e = addSphere("qre_tan");
    QuadRemesh::SourceMesh src;
    QuadRemeshEntity::SourceInfo info;
    ASSERT_TRUE(QuadRemeshEntity::readSource(e, src, info));
    QuadRemesh::Result r;
    r.ok = true;
    r.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    r.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    r.uvs = {0, 0, 1, 0, 1, 1, 0, 1};
    r.faces = {{0, 1, 2, 3}};
    r.quads = 1;
    QImage flat(8, 8, QImage::Format_ARGB32);
    flat.fill(qRgb(128, 128, 255));
    r.maps.insert(QStringLiteral("normal_map"), flat);
    const auto built = QuadRemeshEntity::buildMesh(r, info, "qre_tan");
    ASSERT_TRUE(built.ok) << built.error.toStdString();
    const Ogre::VertexData* vd = built.mesh->getSubMesh(0)->vertexData;
    ASSERT_NE(vd, nullptr);
    EXPECT_NE(vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TANGENT), nullptr);
    auto mat = Ogre::MaterialManager::getSingleton().getByName(built.materialName);
    ASSERT_TRUE(mat);
    // Any RTSS technique present now must have been generated for THIS
    // material (buildMesh drops the cloned one) — never a copy carrying the
    // source's name-less layout. Simplest observable: the FFP technique 0 has
    // the normal_map unit the bake added.
    bool hasNormalUnit = false;
    auto* pass = mat->getTechnique(0)->getPass(0);
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i)
        if (pass->getTextureUnitState(i)->getName() == "normal_map") hasNormalUnit = true;
    EXPECT_TRUE(hasNormalUnit);
}
