// Coverage test for MeshValidator::doValidate() narrow conditional branches that
// the existing MeshValidator_test.cpp / MeshValidatorOptimize_coverage_test.cpp
// suites leave uncovered. Distinct filename + distinct suite name
// (MeshValidatorChecklistCoverageTest) to avoid any ODR / duplicate-registration
// clash with the existing MeshValidatorTest suite.
//
// Targeted uncovered paths in MeshValidator.cpp:
//   * lines 335-341  : no-UV-mesh branch -> "UVs: ... skipped" info row
//                       (fires only when meshesWithUVs==0 && meshesWithoutUVs>0)
//   * lines 320-331  : >10000-triangle "Tri budget: ... consider decimating" hint
//   * lines 262-266  : 32-bit index buffer path (idx32 read; helpers use IT_16BIT)
//   * lines 226-228  : sharedBuf==true interleaved pos+UV single-source branch
//   * lines 268-269  : index out-of-range skip ("if i0>=vertexCount continue")
//
// Per task instructions: does NOT call optimizeVertexCache() (export path; already
// covered elsewhere). Drives the real doValidate() and asserts observable rows.

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <array>
#include <cstdint>
#include <vector>

#include "MeshValidator.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

namespace {

constexpr unsigned long kSingletonSettleTimeMs = 30;

Ogre::Entity* createEntityFromMesh(const std::string& nodeName, const Ogre::MeshPtr& mesh)
{
    if (!mesh)
        return nullptr;
    auto* manager = Manager::getSingleton();
    auto* node = manager->addSceneNode(nodeName.c_str());
    if (!node)
        return nullptr;
    return manager->createEntity(node, mesh);
}

// (a) Positions only, NO VES_TEXTURE_COORDINATES -> exercises the no-UV branch.
Ogre::MeshPtr createNoUvMesh(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    const std::array<float, 9> verts{{0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f}};
    vbuf->writeData(0, verts.size() * sizeof(float), verts.data());
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 3;

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    const std::array<uint16_t, 3> idx{{0, 1, 2}};
    ibuf->writeData(0, idx.size() * sizeof(uint16_t), idx.data());
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;

    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 1, 1, 1));
    mesh->_setBoundingSphereRadius(2.0f);
    mesh->load();
    return mesh;
}

// (b) Large grid (> 10000 triangles) using a 32-BIT index buffer in a single
// INTERLEAVED source-0 buffer holding both position AND UV. This single mesh
// exercises three uncovered paths at once:
//   - the >10000-tri budget hint,
//   - the idx32 (32-bit) read path,
//   - the sharedBuf==true interleaved-buffer branch (pos+UV share source 0).
// gridN columns/rows of quads -> (gridN*gridN*2) triangles. gridN=80 -> 12800 tris.
Ogre::MeshPtr createLargeInterleavedGrid32(const std::string& name, int gridN)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;

    // Interleaved: position (FLOAT3) + UV (FLOAT2) BOTH in source 0.
    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

    const int dim = gridN + 1;             // verts per side
    const size_t vertCount = static_cast<size_t>(dim) * dim;

    std::vector<float> verts;
    verts.reserve(vertCount * 5);          // 3 pos + 2 uv
    for (int y = 0; y < dim; ++y) {
        for (int x = 0; x < dim; ++x) {
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            verts.push_back(fx);           // px
            verts.push_back(fy);           // py
            verts.push_back(0.f);          // pz (all coplanar; harmless)
            verts.push_back(fx / gridN);   // u (finite, in [0,1])
            verts.push_back(fy / gridN);   // v
        }
    }

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), vertCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    vbuf->writeData(0, verts.size() * sizeof(float), verts.data());
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = vertCount;

    // 32-bit indices, two triangles per quad. Use a non-degenerate winding so no
    // triangle is zero-area (positions are distinct grid corners).
    std::vector<uint32_t> idx;
    idx.reserve(static_cast<size_t>(gridN) * gridN * 6);
    auto vid = [dim](int x, int y) { return static_cast<uint32_t>(y * dim + x); };
    for (int y = 0; y < gridN; ++y) {
        for (int x = 0; x < gridN; ++x) {
            const uint32_t a = vid(x, y);
            const uint32_t b = vid(x + 1, y);
            const uint32_t c = vid(x, y + 1);
            const uint32_t d = vid(x + 1, y + 1);
            idx.push_back(a); idx.push_back(c); idx.push_back(b);
            idx.push_back(b); idx.push_back(c); idx.push_back(d);
        }
    }

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_32BIT, idx.size(),
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    ibuf->writeData(0, idx.size() * sizeof(uint32_t), idx.data());
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = idx.size();

    mesh->_setBounds(Ogre::AxisAlignedBox(0, 0, 0, gridN, gridN, 0));
    mesh->_setBoundingSphereRadius(static_cast<Ogre::Real>(gridN) * 2.0f);
    mesh->load();
    return mesh;
}

// (c) Mesh whose index buffer contains an OUT-OF-RANGE index. The first triangle's
// index (3) >= vertexCount (3) so the validator's bounds check skips it without a
// crash; the remaining indices are valid. UV present (interleaved) so the UV-skip
// branch does NOT fire here.
Ogre::MeshPtr createOutOfRangeIndexMesh(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    const std::array<float, 15> verts{{
        0.f, 0.f, 0.f, 0.f, 0.f,
        1.f, 0.f, 0.f, 1.f, 0.f,
        0.f, 1.f, 0.f, 0.f, 1.f,
    }};
    vbuf->writeData(0, verts.size() * sizeof(float), verts.data());
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 3;

    // Two triangles: the first references vertex 3 (out of range, vertexCount==3),
    // the second is fully in range.
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 6, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    const std::array<uint16_t, 6> idx{{0, 1, 3 /* out of range */, 0, 1, 2}};
    ibuf->writeData(0, idx.size() * sizeof(uint16_t), idx.data());
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 6;

    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 1, 1, 1));
    mesh->_setBoundingSphereRadius(2.0f);
    mesh->load();
    return mesh;
}

} // namespace

class MeshValidatorChecklistCoverageTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    MeshValidator* validator = nullptr;

    void SetUp() override
    {
        MeshValidator::kill();
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(kSingletonSettleTimeMs);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        ASSERT_TRUE(canLoadMeshFiles());

        validator = MeshValidator::instance();
        ASSERT_NE(validator, nullptr);
    }

    void TearDown() override
    {
        if (Manager::getSingletonPtr())
            SelectionSet::getSingleton()->clear();

        MeshValidator::kill();
        SelectionSet::kill();
        Manager::kill();

        if (app)
            app->processEvents();
        QThread::msleep(kSingletonSettleTimeMs);
    }

    // Collect descriptions of every issue row so tests can scan for the prefix
    // they expect without caring about ordering.
    QStringList descriptions() const
    {
        QStringList out;
        for (const QVariant& v : validator->issues())
            out << v.toMap().value("description").toString();
        return out;
    }

    bool anyStartsWith(const QString& prefix) const
    {
        for (const QString& d : descriptions())
            if (d.startsWith(prefix))
                return true;
        return false;
    }

    bool anyContains(const QString& needle) const
    {
        for (const QString& d : descriptions())
            if (d.contains(needle))
                return true;
        return false;
    }
};

// (a) No-UV mesh -> the "UVs: ... skipped" info row (lines 335-341).
TEST_F(MeshValidatorChecklistCoverageTest, NoUvMeshEmitsSkippedUvInfoRow)
{
    auto mesh = createNoUvMesh("MVChecklistNoUvMesh");
    auto* entity = createEntityFromMesh("MVChecklistNoUvNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);
    validator->doValidate();

    EXPECT_TRUE(validator->validated());
    // The geometry check still runs (positions present), so a Geometry row exists.
    EXPECT_TRUE(anyStartsWith("Geometry:"));

    // The targeted branch: a UVs row that is the "skipped" variant.
    bool sawSkippedUv = false;
    for (const QVariant& v : validator->issues()) {
        const QVariantMap m = v.toMap();
        const QString desc = m.value("description").toString();
        if (desc.startsWith("UVs:") && desc.contains("skipped")) {
            sawSkippedUv = true;
            EXPECT_EQ(m.value("type").toString(), QStringLiteral("info"));
            EXPECT_FALSE(m.value("fixable").toBool());
        }
    }
    EXPECT_TRUE(sawSkippedUv);

    // The normal UV-ok / non-finite / extreme rows must NOT appear for a UV-less mesh.
    EXPECT_FALSE(anyContains("all finite"));
    EXPECT_FALSE(anyContains("non-finite"));
    EXPECT_FALSE(anyContains("extreme values"));
    EXPECT_FALSE(validator->hasFixableIssues());
}

// (b) >10000-tri grid with a 32-bit index buffer in a shared/interleaved buffer:
// exercises the tri-budget hint (lines 320-331), the idx32 path (lines 262-266),
// and the sharedBuf==true branch (lines 226-228) all at once.
TEST_F(MeshValidatorChecklistCoverageTest, LargeGrid32BitInterleavedTriBudgetHint)
{
    // gridN=80 -> 80*80*2 = 12800 triangles (> 10000), 81*81 = 6561 verts (< 65536,
    // but stored in a 32-bit buffer to drive the idx32 path regardless).
    auto mesh = createLargeInterleavedGrid32("MVChecklistBigGridMesh", 80);
    auto* entity = createEntityFromMesh("MVChecklistBigGridNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);
    validator->doValidate();

    EXPECT_TRUE(validator->validated());

    // Geometry row is "ok" (no degenerate triangles in a proper grid).
    bool sawGeometryOk = false;
    for (const QVariant& v : validator->issues()) {
        const QVariantMap m = v.toMap();
        const QString desc = m.value("description").toString();
        if (desc.startsWith("Geometry:") && m.value("type").toString() == "ok") {
            sawGeometryOk = true;
            // No "degenerate" wording on the ok row.
            EXPECT_FALSE(desc.contains("degenerate triangle"));
        }
    }
    EXPECT_TRUE(sawGeometryOk);

    // The targeted tri-budget hint row.
    bool sawTriBudget = false;
    for (const QVariant& v : validator->issues()) {
        const QVariantMap m = v.toMap();
        const QString desc = m.value("description").toString();
        if (desc.startsWith("Tri budget:")) {
            sawTriBudget = true;
            EXPECT_EQ(m.value("type").toString(), QStringLiteral("info"));
            EXPECT_FALSE(m.value("fixable").toBool());
            EXPECT_TRUE(desc.contains("consider decimating"));
            EXPECT_EQ(m.value("count").toInt(), 12800);
        }
    }
    EXPECT_TRUE(sawTriBudget);

    // UVs are present & finite (interleaved source-0 branch was taken) -> a UV row
    // exists and is NOT the "skipped" variant.
    EXPECT_TRUE(anyStartsWith("UVs:"));
    EXPECT_FALSE(anyContains("skipped"));
    EXPECT_FALSE(anyContains("non-finite"));
}

// (c) Out-of-range index -> the bounds-check skip path (lines 268-269). The mesh
// must still validate cleanly (no crash, no false degenerate from the skipped tri).
TEST_F(MeshValidatorChecklistCoverageTest, OutOfRangeIndexIsSkippedSafely)
{
    auto mesh = createOutOfRangeIndexMesh("MVChecklistOobIdxMesh");
    auto* entity = createEntityFromMesh("MVChecklistOobIdxNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);
    validator->doValidate();

    EXPECT_TRUE(validator->validated());

    // The out-of-range triangle is skipped, the in-range triangle is non-degenerate,
    // so geometry reports "ok" with zero degenerates.
    bool sawGeometryOk = false;
    for (const QVariant& v : validator->issues()) {
        const QVariantMap m = v.toMap();
        const QString desc = m.value("description").toString();
        if (desc.startsWith("Geometry:")) {
            EXPECT_EQ(m.value("type").toString(), QStringLiteral("ok"));
            EXPECT_FALSE(desc.contains("degenerate triangle"));
            sawGeometryOk = true;
        }
    }
    EXPECT_TRUE(sawGeometryOk);

    // No error/warning rows: the skip must not surface as a degenerate.
    EXPECT_FALSE(validator->hasFixableIssues());
    for (const QVariant& v : validator->issues()) {
        const QString type = v.toMap().value("type").toString();
        EXPECT_NE(type, QStringLiteral("error"));
        EXPECT_NE(type, QStringLiteral("warning"));
    }
}

// Multi-entity case: a UV-less mesh AND a UV-bearing mesh selected together means
// meshesWithUVs>0, so the "skipped" branch must NOT fire (guards the
// meshesWithUVs==0 condition from the no-UV test above).
TEST_F(MeshValidatorChecklistCoverageTest, MixedUvAndNoUvSelectionDoesNotSkipUvCheck)
{
    auto noUv = createNoUvMesh("MVChecklistMixNoUvMesh");
    auto withUv = createLargeInterleavedGrid32("MVChecklistMixUvMesh", 4); // small, finite UVs
    auto* e1 = createEntityFromMesh("MVChecklistMixNoUvNode", noUv);
    auto* e2 = createEntityFromMesh("MVChecklistMixUvNode", withUv);
    ASSERT_NE(e1, nullptr);
    ASSERT_NE(e2, nullptr);

    auto* sel = SelectionSet::getSingleton();
    sel->selectOne(e1);
    sel->append(e2);

    validator->doValidate();
    EXPECT_TRUE(validator->validated());

    // With at least one UV-bearing mesh in the selection the "skipped" row is gone;
    // instead a normal UV row (ok/finite) is present.
    EXPECT_FALSE(anyContains("no texture coordinates"));
    EXPECT_TRUE(anyStartsWith("UVs:"));
}
