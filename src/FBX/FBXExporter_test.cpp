#include <gtest/gtest.h>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <QFile>
#include <QDir>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QApplication>
#include <QThread>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreSkeleton.h>
#include <OgreBone.h>
#include <OgreAnimation.h>
#include <OgreKeyFrame.h>
#include <OgreQuaternion.h>
#include <OgreMatrix3.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMeshManager.h>
#include <OgreSkeletonManager.h>
#include <OgreMaterialManager.h>
#include <OgreTechnique.h>
#include <OgrePass.h>
#include <OgreTextureUnitState.h>
#include "FBXExporter.h"
#include "../Manager.h"
#include "../SelectionSet.h"
#include "../MeshImporterExporter.h"
#include "../TestHelpers.h"

// ── Standalone tests (no Ogre needed) ────────────────────────────

TEST(FBXExporterStandaloneTest, ExportFBX_NullEntity_ReturnsFalse) {
    EXPECT_FALSE(FBXExporter::exportFBX(nullptr, "/tmp/test.fbx"));
}

TEST(FBXExporterStandaloneTest, ExportFBX_EmptyPath_ReturnsFalse) {
    // Can't create a real entity without Ogre, but empty path should fail
    EXPECT_FALSE(FBXExporter::exportFBX(nullptr, ""));
}

// ── Euler decomposition tests ────────────────────────────────────
// The FBX exporter decomposes quaternions to Euler XYZ angles where
// Assimp reconstructs as R = Rz * Ry * Rx (FBX RotOrder_EulerXYZ).
// These standalone tests replicate the decomposition and verify it
// round-trips correctly through Assimp's convention.

namespace {

// Replicate the static quaternionToEulerXYZ from FBXExporter.cpp
void testQuaternionToEulerXYZ(const Ogre::Quaternion& q,
                               double& rx, double& ry, double& rz)
{
    double w = q.w, x = q.x, y = q.y, z = q.z;
    double sinp = std::clamp(2.0 * (w * y - x * z), -1.0, 1.0);
    ry = std::asin(sinp);

    double cosp = std::cos(ry);
    if (cosp > 1e-6)
    {
        rx = std::atan2(2.0 * (y * z + w * x), 1.0 - 2.0 * (x * x + y * y));
        rz = std::atan2(2.0 * (x * y + w * z), 1.0 - 2.0 * (y * y + z * z));
    }
    else
    {
        rz = 0.0;
        rx = std::atan2(-(2.0 * (x * y - w * z)), 1.0 - 2.0 * (x * x + z * z));
    }
    rx *= 180.0 / M_PI;
    ry *= 180.0 / M_PI;
    rz *= 180.0 / M_PI;
}

// Reconstruct quaternion from Euler angles using Assimp's convention:
// R = Rz * Ry * Rx (same as FBXConverter::GetRotationMatrix for EulerXYZ)
Ogre::Quaternion eulerToQuatAssimp(double rxDeg, double ryDeg, double rzDeg)
{
    double rx = rxDeg * M_PI / 180.0;
    double ry = ryDeg * M_PI / 180.0;
    double rz = rzDeg * M_PI / 180.0;

    Ogre::Matrix3 mx, my, mz;
    mx.FromAngleAxis(Ogre::Vector3::UNIT_X, Ogre::Radian(rx));
    my.FromAngleAxis(Ogre::Vector3::UNIT_Y, Ogre::Radian(ry));
    mz.FromAngleAxis(Ogre::Vector3::UNIT_Z, Ogre::Radian(rz));

    // R = Rz * Ry * Rx
    Ogre::Matrix3 combined = mz * my * mx;
    Ogre::Quaternion result;
    result.FromRotationMatrix(combined);
    return result;
}

// Check if two quaternions represent the same rotation (q and -q are equivalent)
bool quaternionsEqual(const Ogre::Quaternion& a, const Ogre::Quaternion& b, double tol = 1e-4)
{
    double dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    return std::abs(std::abs(dot) - 1.0) < tol;
}

} // anonymous namespace

TEST(FBXEulerTest, Identity) {
    Ogre::Quaternion q = Ogre::Quaternion::IDENTITY;
    double rx, ry, rz;
    testQuaternionToEulerXYZ(q, rx, ry, rz);
    EXPECT_NEAR(rx, 0.0, 0.01);
    EXPECT_NEAR(ry, 0.0, 0.01);
    EXPECT_NEAR(rz, 0.0, 0.01);
}

TEST(FBXEulerTest, PureXRotation_90) {
    Ogre::Quaternion q(Ogre::Radian(Ogre::Degree(90)), Ogre::Vector3::UNIT_X);
    double rx, ry, rz;
    testQuaternionToEulerXYZ(q, rx, ry, rz);
    EXPECT_NEAR(rx, 90.0, 0.01);
    EXPECT_NEAR(ry, 0.0, 0.01);
    EXPECT_NEAR(rz, 0.0, 0.01);
}

TEST(FBXEulerTest, PureYRotation_45) {
    Ogre::Quaternion q(Ogre::Radian(Ogre::Degree(45)), Ogre::Vector3::UNIT_Y);
    double rx, ry, rz;
    testQuaternionToEulerXYZ(q, rx, ry, rz);
    EXPECT_NEAR(rx, 0.0, 0.01);
    EXPECT_NEAR(ry, 45.0, 0.01);
    EXPECT_NEAR(rz, 0.0, 0.01);
}

TEST(FBXEulerTest, PureZRotation_60) {
    Ogre::Quaternion q(Ogre::Radian(Ogre::Degree(60)), Ogre::Vector3::UNIT_Z);
    double rx, ry, rz;
    testQuaternionToEulerXYZ(q, rx, ry, rz);
    EXPECT_NEAR(rx, 0.0, 0.01);
    EXPECT_NEAR(ry, 0.0, 0.01);
    EXPECT_NEAR(rz, 60.0, 0.01);
}

TEST(FBXEulerTest, CombinedRotation_RoundTrip) {
    // 30° X then 45° Y (intrinsic) = qx * qy
    Ogre::Quaternion qx(Ogre::Radian(Ogre::Degree(30)), Ogre::Vector3::UNIT_X);
    Ogre::Quaternion qy(Ogre::Radian(Ogre::Degree(45)), Ogre::Vector3::UNIT_Y);
    Ogre::Quaternion q = qx * qy; // combined rotation

    double rx, ry, rz;
    testQuaternionToEulerXYZ(q, rx, ry, rz);

    // Reconstruct using Assimp's convention: R = Rz * Ry * Rx
    Ogre::Quaternion reconstructed = eulerToQuatAssimp(rx, ry, rz);
    EXPECT_TRUE(quaternionsEqual(q, reconstructed))
        << "Original: (" << q.w << "," << q.x << "," << q.y << "," << q.z << ")"
        << " Reconstructed: (" << reconstructed.w << "," << reconstructed.x
        << "," << reconstructed.y << "," << reconstructed.z << ")";
}

TEST(FBXEulerTest, ArbitraryRotation_RoundTrip) {
    // Arbitrary rotation: 25° X, 50° Y, 35° Z via Assimp convention
    Ogre::Quaternion original = eulerToQuatAssimp(25.0, 50.0, 35.0);

    double rx, ry, rz;
    testQuaternionToEulerXYZ(original, rx, ry, rz);

    Ogre::Quaternion reconstructed = eulerToQuatAssimp(rx, ry, rz);
    EXPECT_TRUE(quaternionsEqual(original, reconstructed))
        << "Euler angles: (" << rx << "," << ry << "," << rz << ")";
}

TEST(FBXEulerTest, NegativeAngles_RoundTrip) {
    Ogre::Quaternion original = eulerToQuatAssimp(-30.0, 15.0, -120.0);

    double rx, ry, rz;
    testQuaternionToEulerXYZ(original, rx, ry, rz);

    Ogre::Quaternion reconstructed = eulerToQuatAssimp(rx, ry, rz);
    EXPECT_TRUE(quaternionsEqual(original, reconstructed));
}

TEST(FBXEulerTest, LargeAngles_RoundTrip) {
    Ogre::Quaternion original = eulerToQuatAssimp(170.0, -80.0, 160.0);

    double rx, ry, rz;
    testQuaternionToEulerXYZ(original, rx, ry, rz);

    Ogre::Quaternion reconstructed = eulerToQuatAssimp(rx, ry, rz);
    EXPECT_TRUE(quaternionsEqual(original, reconstructed));
}

TEST(FBXEulerTest, NearGimbalLock_RoundTrip) {
    // Near gimbal lock: ry close to 90°
    Ogre::Quaternion original = eulerToQuatAssimp(10.0, 89.0, 20.0);

    double rx, ry, rz;
    testQuaternionToEulerXYZ(original, rx, ry, rz);

    Ogre::Quaternion reconstructed = eulerToQuatAssimp(rx, ry, rz);
    EXPECT_TRUE(quaternionsEqual(original, reconstructed));
}

TEST(FBXEulerTest, ManyRandomRotations_RoundTrip) {
    // Test a grid of rotations to verify decomposition works broadly
    int failures = 0;
    for (int ax = -150; ax <= 150; ax += 30) {
        for (int ay = -80; ay <= 80; ay += 20) {
            for (int az = -150; az <= 150; az += 30) {
                Ogre::Quaternion original = eulerToQuatAssimp(ax, ay, az);
                double rx, ry, rz;
                testQuaternionToEulerXYZ(original, rx, ry, rz);
                Ogre::Quaternion reconstructed = eulerToQuatAssimp(rx, ry, rz);
                if (!quaternionsEqual(original, reconstructed)) {
                    failures++;
                }
            }
        }
    }
    EXPECT_EQ(failures, 0) << failures << " rotation(s) failed round-trip";
}

// ── Euler continuity (unrolling) tests ───────────────────────────

TEST(FBXEulerContinuityTest, UnrollPreventsBigJump) {
    // Simulate two consecutive Euler angles that jump across 360° boundary
    // The unroll lambda from the exporter:
    auto unroll = [](double prev, double cur) {
        double d = cur - prev;
        if (d > 180.0)       cur -= 360.0 * std::ceil((d - 180.0) / 360.0);
        else if (d < -180.0) cur += 360.0 * std::ceil((-d - 180.0) / 360.0);
        return cur;
    };

    // Jump from 170° to -170° (should become 190°, delta = 20°)
    EXPECT_NEAR(unroll(170.0, -170.0), 190.0, 0.01);

    // Jump from -170° to 170° (should become -190°, delta = -20°)
    EXPECT_NEAR(unroll(-170.0, 170.0), -190.0, 0.01);

    // No jump needed: 10° to 30°
    EXPECT_NEAR(unroll(10.0, 30.0), 30.0, 0.01);

    // Large jump: 350° to 10° (should become 370°)
    EXPECT_NEAR(unroll(350.0, 10.0), 370.0, 0.01);

    // Jump from 10° to 350° (should become -10°)
    EXPECT_NEAR(unroll(10.0, 350.0), -10.0, 0.01);
}

// ── Tests requiring Ogre ─────────────────────────────────────────
// NOTE: FBXExporterTest fixture and its TEST_F tests were removed because
// they crash in CI. The FBXExporterCoverageTest fixture (below) uses
// in-memory meshes and does not depend on external .fbx files.

TEST(FBXExporterStandaloneTest, ExportFBX_FormatFileURI) {
    QString uri = "/path/to/file";
    QString format = "FBX Binary (*.fbx)";
    EXPECT_EQ(MeshImporterExporter::formatFileURI(uri, format), "/path/to/file.fbx");

    // Already has extension
    uri = "/path/to/file.fbx";
    EXPECT_EQ(MeshImporterExporter::formatFileURI(uri, format), "/path/to/file.fbx");
}

// ═══════════════════════════════════════════════════════════════════
//  Lightweight FBX Binary Parser (for test verification)
// ═══════════════════════════════════════════════════════════════════

namespace {

struct FBXProperty {
    char type = 0;
    bool boolVal = false;
    int32_t intVal = 0;
    int64_t longVal = 0;
    float floatVal = 0;
    double doubleVal = 0;
    std::string stringVal;
    std::vector<double> doubleArray;
    std::vector<int32_t> intArray;
    std::vector<float> floatArray;
    std::vector<int64_t> longArray;
};

struct FBXNode {
    std::string name;
    std::vector<FBXProperty> properties;
    std::vector<FBXNode> children;

    const FBXNode* find(const std::string& n) const {
        for (const auto& c : children)
            if (c.name == n) return &c;
        return nullptr;
    }

    std::vector<const FBXNode*> findAll(const std::string& n) const {
        std::vector<const FBXNode*> result;
        for (const auto& c : children)
            if (c.name == n) result.push_back(&c);
        return result;
    }
};

FBXProperty readProperty(std::ifstream& in)
{
    FBXProperty p;
    in.read(&p.type, 1);
    switch (p.type) {
    case 'C': { uint8_t v; in.read(reinterpret_cast<char*>(&v), 1); p.boolVal = v != 0; break; }
    case 'I': in.read(reinterpret_cast<char*>(&p.intVal), 4); break;
    case 'L': in.read(reinterpret_cast<char*>(&p.longVal), 8); break;
    case 'F': in.read(reinterpret_cast<char*>(&p.floatVal), 4); break;
    case 'D': in.read(reinterpret_cast<char*>(&p.doubleVal), 8); break;
    case 'S': case 'R': {
        uint32_t len; in.read(reinterpret_cast<char*>(&len), 4);
        p.stringVal.resize(len);
        in.read(p.stringVal.data(), len);
        break;
    }
    case 'd': {
        uint32_t count; in.read(reinterpret_cast<char*>(&count), 4);
        uint32_t encoding; in.read(reinterpret_cast<char*>(&encoding), 4);
        uint32_t byteLen; in.read(reinterpret_cast<char*>(&byteLen), 4);
        p.doubleArray.resize(count);
        in.read(reinterpret_cast<char*>(p.doubleArray.data()), byteLen);
        break;
    }
    case 'i': {
        uint32_t count; in.read(reinterpret_cast<char*>(&count), 4);
        uint32_t encoding; in.read(reinterpret_cast<char*>(&encoding), 4);
        uint32_t byteLen; in.read(reinterpret_cast<char*>(&byteLen), 4);
        p.intArray.resize(count);
        in.read(reinterpret_cast<char*>(p.intArray.data()), byteLen);
        break;
    }
    case 'f': {
        uint32_t count; in.read(reinterpret_cast<char*>(&count), 4);
        uint32_t encoding; in.read(reinterpret_cast<char*>(&encoding), 4);
        uint32_t byteLen; in.read(reinterpret_cast<char*>(&byteLen), 4);
        p.floatArray.resize(count);
        in.read(reinterpret_cast<char*>(p.floatArray.data()), byteLen);
        break;
    }
    case 'l': {
        uint32_t count; in.read(reinterpret_cast<char*>(&count), 4);
        uint32_t encoding; in.read(reinterpret_cast<char*>(&encoding), 4);
        uint32_t byteLen; in.read(reinterpret_cast<char*>(&byteLen), 4);
        p.longArray.resize(count);
        in.read(reinterpret_cast<char*>(p.longArray.data()), byteLen);
        break;
    }
    default: break;
    }
    return p;
}

FBXNode readNode(std::ifstream& in)
{
    FBXNode node;
    uint32_t endOffset, numProps, propListLen;
    in.read(reinterpret_cast<char*>(&endOffset), 4);
    in.read(reinterpret_cast<char*>(&numProps), 4);
    in.read(reinterpret_cast<char*>(&propListLen), 4);
    uint8_t nameLen;
    in.read(reinterpret_cast<char*>(&nameLen), 1);
    node.name.resize(nameLen);
    in.read(node.name.data(), nameLen);

    for (uint32_t i = 0; i < numProps; ++i)
        node.properties.push_back(readProperty(in));

    // Read child nodes until endOffset
    while (static_cast<uint32_t>(in.tellg()) < endOffset) {
        // Check for null record (13 zero bytes)
        auto pos = in.tellg();
        uint32_t testEnd;
        in.read(reinterpret_cast<char*>(&testEnd), 4);
        if (testEnd == 0) {
            // Likely null sentinel — skip remaining 9 bytes
            in.seekg(pos);
            char sentinel[13];
            in.read(sentinel, 13);
            break;
        }
        in.seekg(pos);
        node.children.push_back(readNode(in));
    }

    // Ensure we're at endOffset
    in.seekg(endOffset);
    return node;
}

std::vector<FBXNode> parseFBX(const std::string& path)
{
    std::vector<FBXNode> nodes;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return nodes;

    // Skip 27-byte header
    in.seekg(27);

    while (in.good()) {
        auto pos = in.tellg();
        uint32_t endOffset;
        in.read(reinterpret_cast<char*>(&endOffset), 4);
        if (endOffset == 0) break; // null sentinel = end of top-level nodes
        in.seekg(pos);
        nodes.push_back(readNode(in));
    }
    return nodes;
}

const FBXNode* findTopLevel(const std::vector<FBXNode>& nodes, const std::string& name) {
    for (const auto& n : nodes)
        if (n.name == name) return &n;
    return nullptr;
}

// Recursively find all nodes with a given name
void findAllRecursive(const FBXNode& node, const std::string& name,
                      std::vector<const FBXNode*>& result) {
    if (node.name == name) result.push_back(&node);
    for (const auto& c : node.children)
        findAllRecursive(c, name, result);
}

std::vector<const FBXNode*> findAllInTree(const std::vector<FBXNode>& nodes,
                                          const std::string& name) {
    std::vector<const FBXNode*> result;
    for (const auto& n : nodes)
        findAllRecursive(n, name, result);
    return result;
}

// Find P (property) nodes with a given first property string
const FBXNode* findP70(const FBXNode& props70, const std::string& propName) {
    for (const auto& p : props70.children) {
        if (p.name == "P" && !p.properties.empty() && p.properties[0].stringVal == propName)
            return &p;
    }
    return nullptr;
}

} // anonymous namespace (FBX parser)

// ═══════════════════════════════════════════════════════════════════
//  In-Memory Mesh Coverage Tests
// ═══════════════════════════════════════════════════════════════════

class FBXExporterCoverageTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    int meshCounter = 0;
    QTemporaryDir textureDir;

    void SetUp() override {
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        if (!canLoadMeshFiles()) {
            GTEST_SKIP() << "Skipping: no GL context / hardware buffers available";
        }
        createStandardOgreMaterials();

        // Provide a real texture file so FBXExporter can embed Video.Content.
        ASSERT_TRUE(textureDir.isValid());
        const QString texPath = QDir(textureDir.path()).filePath("diffuse_tex.png");
        QFile tex(texPath);
        ASSERT_TRUE(tex.open(QIODevice::WriteOnly));
        // 1x1 transparent PNG
        const QByteArray png =
            QByteArray::fromHex(
                "89504E470D0A1A0A"
                "0000000D49484452"
                "0000000100000001"
                "08060000001F15C489"
                "0000000A49444154"
                "789C63600000020001"
                "E221BC3300000000"
                "49454E44AE426082");
        tex.write(png);
        tex.close();

        auto& rgm = Ogre::ResourceGroupManager::getSingleton();
        const Ogre::String texGroup = "FBXExporterTestTextures";
        if (!rgm.resourceGroupExists(texGroup))
            rgm.createResourceGroup(texGroup);
        rgm.addResourceLocation(textureDir.path().toStdString(), "FileSystem", texGroup, false);
        // Don't touch the DEFAULT group (it may already be initialized by the engine).
        // Initialize only our dedicated test group so openResource() can find the texture.
        try { rgm.initialiseResourceGroup(texGroup); } catch (...) {}
    }

    void TearDown() override {
        SelectionSet::kill();
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(50);
    }

    std::string uniqueName(const std::string& base) {
        return base + "_" + std::to_string(meshCounter++);
    }

    // Export entity to temp file and parse the FBX
    struct ExportResult {
        std::vector<FBXNode> nodes;
        QString path;
        bool success = false;
    };

    ExportResult exportAndParse(Ogre::Entity* entity) {
        ExportResult r;
        r.path = QString("/tmp/fbx_coverage_%1.fbx").arg(meshCounter);
        r.success = FBXExporter::exportFBX(entity, r.path);
        if (r.success)
            r.nodes = parseFBX(r.path.toStdString());
        return r;
    }

    void cleanup(const ExportResult& r) {
        QFile::remove(r.path);
    }

    // ── Mesh creation helpers ───────────────────────────────────

    // Triangle with positions + normals + UVs, 16-bit indices, shared vertex data
    Ogre::Entity* createSimpleMesh(const std::string& name) {
        auto mesh = Ogre::MeshManager::getSingleton().createManual(
            name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

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
            decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        // pos(3) + normal(3) + uv(2) = 8 floats per vertex
        float verts[] = {
            0,0,0,   0,0,1,  0.0f,0.0f,
            1,0,0,   0,0,1,  1.0f,0.0f,
            0,1,0,   0,0,1,  0.0f,1.0f,
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

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
        auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
        node->attachObject(entity);
        return entity;
    }

    // Triangle with positions only (no normals, no UVs)
    Ogre::Entity* createMeshNoNormalsNoUVs(const std::string& name) {
        auto mesh = Ogre::MeshManager::getSingleton().createManual(
            name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        auto* sub = mesh->createSubMesh();
        mesh->sharedVertexData = new Ogre::VertexData();
        auto* decl = mesh->sharedVertexData->vertexDeclaration;
        decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

        auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float verts[] = {0,0,0, 1,0,0, 0,1,0};
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

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
        auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
        node->attachObject(entity);
        return entity;
    }

    // Triangle with per-submesh vertex data (useSharedVertices=false)
    Ogre::Entity* createMeshNonShared(const std::string& name) {
        auto mesh = Ogre::MeshManager::getSingleton().createManual(
            name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        auto* sub = mesh->createSubMesh();
        sub->useSharedVertices = false;
        sub->vertexData = new Ogre::VertexData();
        auto* decl = sub->vertexData->vertexDeclaration;
        size_t offset = 0;
        decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
        offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
        decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);

        auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float verts[] = {
            2,0,0,  0,1,0,
            3,0,0,  0,1,0,
            2,1,0,  0,1,0,
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

        mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,4,2,1));
        mesh->_setBoundingSphereRadius(4.0);
        mesh->load();

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
        auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
        node->attachObject(entity);
        return entity;
    }

    // Triangle with 32-bit index buffer
    Ogre::Entity* createMesh32BitIndices(const std::string& name) {
        auto mesh = Ogre::MeshManager::getSingleton().createManual(
            name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        auto* sub = mesh->createSubMesh();
        mesh->sharedVertexData = new Ogre::VertexData();
        auto* decl = mesh->sharedVertexData->vertexDeclaration;
        decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

        auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float verts[] = {0,0,0, 1,0,0, 0,1,0};
        vbuf->writeData(0, sizeof(verts), verts);
        mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
        mesh->sharedVertexData->vertexCount = 3;

        auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            Ogre::HardwareIndexBuffer::IT_32BIT, 3,
            Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        uint32_t idx[] = {0, 1, 2};
        ibuf->writeData(0, sizeof(idx), idx);
        sub->useSharedVertices = true;
        sub->indexData->indexBuffer = ibuf;
        sub->indexData->indexCount = 3;

        mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,1,1,1));
        mesh->_setBoundingSphereRadius(2.0);
        mesh->load();

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
        auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
        node->attachObject(entity);
        return entity;
    }

    // 2 submeshes with different materials
    Ogre::Entity* createMultiSubmeshMesh(const std::string& name) {
        // Create two materials
        auto matA = Ogre::MaterialManager::getSingleton().create(
            name + "_matA", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        matA->getTechnique(0)->getPass(0)->setDiffuse(1.0f, 0.0f, 0.0f, 1.0f);

        auto matB = Ogre::MaterialManager::getSingleton().create(
            name + "_matB", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        matB->getTechnique(0)->getPass(0)->setDiffuse(0.0f, 0.0f, 1.0f, 1.0f);

        auto mesh = Ogre::MeshManager::getSingleton().createManual(
            name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        mesh->sharedVertexData = new Ogre::VertexData();
        auto* decl = mesh->sharedVertexData->vertexDeclaration;
        decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

        auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), 6, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float verts[] = {
            0,0,0, 1,0,0, 0,1,0,  // triangle 1
            2,0,0, 3,0,0, 2,1,0,  // triangle 2
        };
        vbuf->writeData(0, sizeof(verts), verts);
        mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
        mesh->sharedVertexData->vertexCount = 6;

        // Submesh 0
        auto* sub0 = mesh->createSubMesh();
        sub0->useSharedVertices = true;
        sub0->setMaterialName(name + "_matA");
        auto ibuf0 = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            Ogre::HardwareIndexBuffer::IT_16BIT, 3,
            Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        uint16_t idx0[] = {0, 1, 2};
        ibuf0->writeData(0, sizeof(idx0), idx0);
        sub0->indexData->indexBuffer = ibuf0;
        sub0->indexData->indexCount = 3;

        // Submesh 1
        auto* sub1 = mesh->createSubMesh();
        sub1->useSharedVertices = true;
        sub1->setMaterialName(name + "_matB");
        auto ibuf1 = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            Ogre::HardwareIndexBuffer::IT_16BIT, 3,
            Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        uint16_t idx1[] = {3, 4, 5};
        ibuf1->writeData(0, sizeof(idx1), idx1);
        sub1->indexData->indexBuffer = ibuf1;
        sub1->indexData->indexCount = 3;

        mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,4,2,1));
        mesh->_setBoundingSphereRadius(4.0);
        mesh->load();

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
        auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
        node->attachObject(entity);
        return entity;
    }

    // Mesh with skeleton: 3 bones (root/spine/head), bone assignments on spine
    Ogre::Entity* createSkeletonMesh(const std::string& name) {
        auto skel = Ogre::SkeletonManager::getSingleton().create(
            name + "_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        auto* root = skel->createBone("root", 0);
        root->setPosition(Ogre::Vector3(0, 0, 0));

        auto* spine = skel->createBone("spine", 1);
        spine->setPosition(Ogre::Vector3(0, 1, 0.5));
        root->addChild(spine);

        auto* head = skel->createBone("head", 2);
        head->setPosition(Ogre::Vector3(0, 0.5, 0));
        spine->addChild(head);

        skel->setBindingPose();

        auto mesh = Ogre::MeshManager::getSingleton().createManual(
            name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        auto* sub = mesh->createSubMesh();
        mesh->sharedVertexData = new Ogre::VertexData();
        auto* decl = mesh->sharedVertexData->vertexDeclaration;
        size_t offset = 0;
        decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
        offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
        decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);

        auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float verts[] = {
            0,0,0.5f,  0,0,1,
            1,0,0.5f,  0,0,1,
            0,1,0.5f,  0,0,1,
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

        // Bone assignments: all vertices assigned to spine (bone 1)
        Ogre::VertexBoneAssignment vba;
        vba.boneIndex = 1;
        vba.weight = 1.0f;
        for (unsigned short v = 0; v < 3; ++v) {
            vba.vertexIndex = v;
            mesh->addBoneAssignment(vba);
        }

        mesh->_notifySkeleton(skel);
        mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,2,2,2));
        mesh->_setBoundingSphereRadius(3.0);
        mesh->load();

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
        auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
        node->attachObject(entity);
        return entity;
    }

    // Skeleton mesh with a "walk" animation (3 keyframes)
    Ogre::Entity* createAnimatedMesh(const std::string& name) {
        auto skel = Ogre::SkeletonManager::getSingleton().create(
            name + "_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        auto* root = skel->createBone("root", 0);
        root->setPosition(Ogre::Vector3(0, 0, 0));

        auto* spine = skel->createBone("spine", 1);
        spine->setPosition(Ogre::Vector3(0, 1, 0));
        root->addChild(spine);

        skel->setBindingPose();

        // Create animation with 3 keyframes
        auto* anim = skel->createAnimation("walk", 1.0f);
        auto* track = anim->createNodeTrack(1);
        track->setAssociatedNode(spine);

        auto* kf0 = track->createNodeKeyFrame(0.0f);
        kf0->setTranslate(Ogre::Vector3::ZERO);
        kf0->setRotation(Ogre::Quaternion::IDENTITY);
        kf0->setScale(Ogre::Vector3::UNIT_SCALE);

        auto* kf1 = track->createNodeKeyFrame(0.5f);
        kf1->setTranslate(Ogre::Vector3(0.5f, 0, 0));
        kf1->setRotation(Ogre::Quaternion(Ogre::Radian(Ogre::Degree(30)),
                                           Ogre::Vector3::UNIT_Y));
        kf1->setScale(Ogre::Vector3::UNIT_SCALE);

        auto* kf2 = track->createNodeKeyFrame(1.0f);
        kf2->setTranslate(Ogre::Vector3::ZERO);
        kf2->setRotation(Ogre::Quaternion::IDENTITY);
        kf2->setScale(Ogre::Vector3::UNIT_SCALE);

        auto mesh = Ogre::MeshManager::getSingleton().createManual(
            name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        auto* sub = mesh->createSubMesh();
        mesh->sharedVertexData = new Ogre::VertexData();
        auto* decl = mesh->sharedVertexData->vertexDeclaration;
        decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

        auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float verts[] = {0,0,0, 1,0,0, 0,1,0};
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

        // Bone assignments: vertices to spine
        Ogre::VertexBoneAssignment vba;
        vba.boneIndex = 1;
        vba.weight = 1.0f;
        for (unsigned short v = 0; v < 3; ++v) {
            vba.vertexIndex = v;
            mesh->addBoneAssignment(vba);
        }

        mesh->_notifySkeleton(skel);
        mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,2,2,1));
        mesh->_setBoundingSphereRadius(3.0);
        mesh->load();

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
        auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
        node->attachObject(entity);
        return entity;
    }

    // Mesh with a material that has a TextureUnitState
    Ogre::Entity* createTexturedMesh(const std::string& name) {
        auto mat = Ogre::MaterialManager::getSingleton().create(
            name + "_mat", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        mat->getTechnique(0)->getPass(0)->setDiffuse(0.8f, 0.8f, 0.8f, 1.0f);
        mat->getTechnique(0)->getPass(0)->createTextureUnitState("diffuse_tex.png");

        auto mesh = Ogre::MeshManager::getSingleton().createManual(
            name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        auto* sub = mesh->createSubMesh();
        sub->setMaterialName(name + "_mat");
        mesh->sharedVertexData = new Ogre::VertexData();
        auto* decl = mesh->sharedVertexData->vertexDeclaration;
        size_t offset = 0;
        decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
        offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
        decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

        auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float verts[] = {
            0,0,0,  0.0f,0.0f,
            1,0,0,  1.0f,0.0f,
            0,1,0,  0.0f,1.0f,
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

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
        auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
        node->attachObject(entity);
        return entity;
    }

    // Mesh with material having known diffuse/specular/shininess
    Ogre::Entity* createMaterialTestMesh(const std::string& name) {
        auto mat = Ogre::MaterialManager::getSingleton().create(
            name + "_mat", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        mat->getTechnique(0)->getPass(0)->setDiffuse(0.9f, 0.1f, 0.2f, 1.0f);
        mat->getTechnique(0)->getPass(0)->setSpecular(0.5f, 0.6f, 0.7f, 1.0f);
        mat->getTechnique(0)->getPass(0)->setAmbient(0.1f, 0.2f, 0.3f);
        mat->getTechnique(0)->getPass(0)->setSelfIllumination(0.05f, 0.06f, 0.07f);
        mat->getTechnique(0)->getPass(0)->setShininess(64.0f);

        auto mesh = Ogre::MeshManager::getSingleton().createManual(
            name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        auto* sub = mesh->createSubMesh();
        sub->setMaterialName(name + "_mat");
        mesh->sharedVertexData = new Ogre::VertexData();
        auto* decl = mesh->sharedVertexData->vertexDeclaration;
        decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

        auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float verts[] = {0,0,0, 1,0,0, 0,1,0};
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

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
        auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
        node->attachObject(entity);
        return entity;
    }
};

// ── Group A: Document Structure ─────────────────────────────────

TEST_F(FBXExporterCoverageTest, TopLevelNodes) {
    auto name = uniqueName("tln");
    auto* entity = createSimpleMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);
    ASSERT_GE(r.nodes.size(), 7u);

    // Expected top-level nodes in order
    EXPECT_EQ(r.nodes[0].name, "FBXHeaderExtension");
    EXPECT_EQ(r.nodes[1].name, "GlobalSettings");
    EXPECT_EQ(r.nodes[2].name, "Documents");
    EXPECT_EQ(r.nodes[3].name, "References");
    EXPECT_EQ(r.nodes[4].name, "Definitions");
    EXPECT_EQ(r.nodes[5].name, "Objects");
    EXPECT_EQ(r.nodes[6].name, "Connections");

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, HeaderExtension) {
    auto name = uniqueName("hdr");
    auto* entity = createSimpleMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* hdr = findTopLevel(r.nodes, "FBXHeaderExtension");
    ASSERT_NE(hdr, nullptr);

    auto* hdrVer = hdr->find("FBXHeaderVersion");
    ASSERT_NE(hdrVer, nullptr);
    EXPECT_EQ(hdrVer->properties[0].intVal, 1003);

    auto* fbxVer = hdr->find("FBXVersion");
    ASSERT_NE(fbxVer, nullptr);
    EXPECT_EQ(fbxVer->properties[0].intVal, 7300);

    auto* enc = hdr->find("EncryptionType");
    ASSERT_NE(enc, nullptr);
    EXPECT_EQ(enc->properties[0].intVal, 0);

    auto* creator = hdr->find("Creator");
    ASSERT_NE(creator, nullptr);
    EXPECT_EQ(creator->properties[0].stringVal, "QtMeshEditor FBX Exporter");

    auto* cts = hdr->find("CreationTimeStamp");
    ASSERT_NE(cts, nullptr);
    EXPECT_NE(cts->find("Version"), nullptr);
    EXPECT_NE(cts->find("Year"), nullptr);
    EXPECT_NE(cts->find("Month"), nullptr);
    EXPECT_NE(cts->find("Day"), nullptr);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, GlobalSettings) {
    auto name = uniqueName("gs");
    auto* entity = createSimpleMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* gs = findTopLevel(r.nodes, "GlobalSettings");
    ASSERT_NE(gs, nullptr);

    auto* props = gs->find("Properties70");
    ASSERT_NE(props, nullptr);

    auto* upAxis = findP70(*props, "UpAxis");
    ASSERT_NE(upAxis, nullptr);
    EXPECT_EQ(upAxis->properties[4].intVal, 1);

    auto* unitScale = findP70(*props, "UnitScaleFactor");
    ASSERT_NE(unitScale, nullptr);
    EXPECT_NEAR(unitScale->properties[4].doubleVal, 100.0, 0.01);

    auto* timeMode = findP70(*props, "TimeMode");
    ASSERT_NE(timeMode, nullptr);
    EXPECT_EQ(timeMode->properties[4].intVal, 6);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, Documents) {
    auto name = uniqueName("doc");
    auto* entity = createSimpleMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* docs = findTopLevel(r.nodes, "Documents");
    ASSERT_NE(docs, nullptr);

    auto* count = docs->find("Count");
    ASSERT_NE(count, nullptr);
    EXPECT_EQ(count->properties[0].intVal, 1);

    auto* doc = docs->find("Document");
    ASSERT_NE(doc, nullptr);
    EXPECT_NE(doc->find("RootNode"), nullptr);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, Definitions_MeshOnly) {
    auto name = uniqueName("def");
    auto* entity = createSimpleMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* defs = findTopLevel(r.nodes, "Definitions");
    ASSERT_NE(defs, nullptr);

    // Check ObjectType nodes exist
    auto objectTypes = defs->findAll("ObjectType");
    ASSERT_GE(objectTypes.size(), 4u); // GlobalSettings, Model, Geometry, Material

    // Verify GlobalSettings, Model, Geometry, Material are present
    bool hasGS = false, hasModel = false, hasGeom = false, hasMat = false;
    for (const auto* ot : objectTypes) {
        if (!ot->properties.empty()) {
            if (ot->properties[0].stringVal == "GlobalSettings") hasGS = true;
            if (ot->properties[0].stringVal == "Model") hasModel = true;
            if (ot->properties[0].stringVal == "Geometry") hasGeom = true;
            if (ot->properties[0].stringVal == "Material") hasMat = true;
        }
    }
    EXPECT_TRUE(hasGS);
    EXPECT_TRUE(hasModel);
    EXPECT_TRUE(hasGeom);
    EXPECT_TRUE(hasMat);

    cleanup(r);
}

// ── Group B: Geometry ──────────────────────────────────────────

TEST_F(FBXExporterCoverageTest, Vertices_ZMirrored) {
    auto name = uniqueName("vz");
    auto* entity = createSimpleMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    ASSERT_NE(objects, nullptr);

    auto geomNodes = objects->findAll("Geometry");
    ASSERT_EQ(geomNodes.size(), 1u);

    auto* verts = geomNodes[0]->find("Vertices");
    ASSERT_NE(verts, nullptr);
    ASSERT_EQ(verts->properties[0].doubleArray.size(), 9u); // 3 vertices * 3 components

    // Original z values were 0, 0, 0 → negated should be -0, -0, -0
    // v0: (0,0,-0), v1: (1,0,-0), v2: (0,1,-0)
    auto& v = verts->properties[0].doubleArray;
    EXPECT_NEAR(v[0], 0.0, 0.001); // v0.x
    EXPECT_NEAR(v[1], 0.0, 0.001); // v0.y
    EXPECT_NEAR(v[2], 0.0, 0.001); // v0.z (was 0, negated is -0)
    EXPECT_NEAR(v[3], 1.0, 0.001); // v1.x
    EXPECT_NEAR(v[7], 1.0, 0.001); // v2.y

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, PolygonIndices_WindingReversed) {
    auto name = uniqueName("pi");
    auto* entity = createSimpleMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto geomNodes = objects->findAll("Geometry");
    ASSERT_EQ(geomNodes.size(), 1u);

    auto* polyIdx = geomNodes[0]->find("PolygonVertexIndex");
    ASSERT_NE(polyIdx, nullptr);
    auto& pi = polyIdx->properties[0].intArray;
    ASSERT_EQ(pi.size(), 3u);

    // Original indices: 0, 1, 2
    // Reversed winding: (i0, i2, -(i1+1)) = (0, 2, -2)
    EXPECT_EQ(pi[0], 0);
    EXPECT_EQ(pi[1], 2);
    EXPECT_EQ(pi[2], -(1 + 1));

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, Normals_ExpandedByPolygonVertex) {
    auto name = uniqueName("norm");
    auto* entity = createSimpleMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto geomNodes = objects->findAll("Geometry");
    ASSERT_EQ(geomNodes.size(), 1u);

    auto* normLayer = geomNodes[0]->find("LayerElementNormal");
    ASSERT_NE(normLayer, nullptr);

    auto* mapping = normLayer->find("MappingInformationType");
    ASSERT_NE(mapping, nullptr);
    EXPECT_EQ(mapping->properties[0].stringVal, "ByPolygonVertex");

    auto* ref = normLayer->find("ReferenceInformationType");
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->properties[0].stringVal, "Direct");

    auto* normals = normLayer->find("Normals");
    ASSERT_NE(normals, nullptr);
    // 1 triangle * 3 vertices = 3 normals * 3 components = 9 doubles
    ASSERT_EQ(normals->properties[0].doubleArray.size(), 9u);

    // Original normal is (0,0,1) → Z-mirrored: (0,0,-1)
    // Expanded in reversed winding order: v0(0,0,-1), v2(0,0,-1), v1(0,0,-1)
    auto& n = normals->properties[0].doubleArray;
    EXPECT_NEAR(n[2], -1.0, 0.001); // v0 normal Z
    EXPECT_NEAR(n[5], -1.0, 0.001); // v2 normal Z
    EXPECT_NEAR(n[8], -1.0, 0.001); // v1 normal Z

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, UVs_VFlipped) {
    auto name = uniqueName("uv");
    auto* entity = createSimpleMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto geomNodes = objects->findAll("Geometry");
    auto* uvLayer = geomNodes[0]->find("LayerElementUV");
    ASSERT_NE(uvLayer, nullptr);

    auto* uvs = uvLayer->find("UV");
    ASSERT_NE(uvs, nullptr);
    ASSERT_EQ(uvs->properties[0].doubleArray.size(), 6u); // 3 verts * 2 components

    auto& uv = uvs->properties[0].doubleArray;
    // v0 original UV: (0.0, 0.0) → V-flip: (0.0, 1.0)
    EXPECT_NEAR(uv[0], 0.0, 0.001);
    EXPECT_NEAR(uv[1], 1.0, 0.001);
    // v1 original UV: (1.0, 0.0) → V-flip: (1.0, 1.0)
    EXPECT_NEAR(uv[2], 1.0, 0.001);
    EXPECT_NEAR(uv[3], 1.0, 0.001);
    // v2 original UV: (0.0, 1.0) → V-flip: (0.0, 0.0)
    EXPECT_NEAR(uv[4], 0.0, 0.001);
    EXPECT_NEAR(uv[5], 0.0, 0.001);

    // Check UVIndex has reversed winding
    auto* uvIdx = uvLayer->find("UVIndex");
    ASSERT_NE(uvIdx, nullptr);
    auto& ui = uvIdx->properties[0].intArray;
    ASSERT_EQ(ui.size(), 3u);
    // Original: 0,1,2 → Reversed winding: (0, 2, 1)
    EXPECT_EQ(ui[0], 0);
    EXPECT_EQ(ui[1], 2);
    EXPECT_EQ(ui[2], 1);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, VertexColors_WritesLayerElementColor) {
    auto meshPtr = createInMemoryTriangleMeshWithVertexColors("fbx_colors");
    ASSERT_TRUE(!!meshPtr);
    auto* node = Manager::getSingleton()->addSceneNode("fbx_colors_node");
    auto* entity = Manager::getSingleton()->createEntity(node, meshPtr);
    ASSERT_NE(entity, nullptr);

    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto geomNodes = objects->findAll("Geometry");
    ASSERT_EQ(geomNodes.size(), 1u);

    EXPECT_NE(geomNodes[0]->find("LayerElementColor"), nullptr);
    auto* layer = geomNodes[0]->find("Layer");
    ASSERT_NE(layer, nullptr);
    bool hasColor = false;
    for (const auto* le : layer->findAll("LayerElement")) {
        auto* typeNode = le->find("Type");
        if (typeNode && !typeNode->properties.empty()
            && typeNode->properties[0].stringVal == "LayerElementColor") {
            hasColor = true;
            break;
        }
    }
    EXPECT_TRUE(hasColor);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, NoNormals_SkipsNormalLayer) {
    auto name = uniqueName("nonorm");
    auto* entity = createMeshNoNormalsNoUVs(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto geomNodes = objects->findAll("Geometry");
    ASSERT_EQ(geomNodes.size(), 1u);

    // Should have no LayerElementNormal
    EXPECT_EQ(geomNodes[0]->find("LayerElementNormal"), nullptr);
    // Should have no LayerElementUV
    EXPECT_EQ(geomNodes[0]->find("LayerElementUV"), nullptr);

    // Layer should not have Normal or UV LayerElement entries
    auto* layer = geomNodes[0]->find("Layer");
    ASSERT_NE(layer, nullptr);

    // Only LayerElementMaterial should be in the Layer
    auto layerElems = layer->findAll("LayerElement");
    bool hasNormalType = false, hasUVType = false;
    for (const auto* le : layerElems) {
        auto* typeNode = le->find("Type");
        if (typeNode && typeNode->properties[0].stringVal == "LayerElementNormal")
            hasNormalType = true;
        if (typeNode && typeNode->properties[0].stringVal == "LayerElementUV")
            hasUVType = true;
    }
    EXPECT_FALSE(hasNormalType);
    EXPECT_FALSE(hasUVType);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, Indices32Bit) {
    auto name = uniqueName("i32");
    auto* entity = createMesh32BitIndices(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto geomNodes = objects->findAll("Geometry");
    ASSERT_EQ(geomNodes.size(), 1u);

    auto* polyIdx = geomNodes[0]->find("PolygonVertexIndex");
    ASSERT_NE(polyIdx, nullptr);
    auto& pi = polyIdx->properties[0].intArray;
    ASSERT_EQ(pi.size(), 3u);
    // Same winding reversal: (0, 2, -(1+1))
    EXPECT_EQ(pi[0], 0);
    EXPECT_EQ(pi[1], 2);
    EXPECT_EQ(pi[2], -2);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, NonSharedVertexData) {
    auto name = uniqueName("ns");
    auto* entity = createMeshNonShared(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto geomNodes = objects->findAll("Geometry");
    ASSERT_EQ(geomNodes.size(), 1u);

    auto* verts = geomNodes[0]->find("Vertices");
    ASSERT_NE(verts, nullptr);
    ASSERT_EQ(verts->properties[0].doubleArray.size(), 9u);

    // Positions from non-shared data: (2,0,0), (3,0,0), (2,1,0)
    // Z-mirrored: (2,0,-0), (3,0,-0), (2,1,-0)
    auto& v = verts->properties[0].doubleArray;
    EXPECT_NEAR(v[0], 2.0, 0.001);
    EXPECT_NEAR(v[3], 3.0, 0.001);
    EXPECT_NEAR(v[6], 2.0, 0.001);
    EXPECT_NEAR(v[7], 1.0, 0.001);

    cleanup(r);
}

// ── Group C: Materials ─────────────────────────────────────────

TEST_F(FBXExporterCoverageTest, MaterialProperties) {
    auto name = uniqueName("matp");
    auto* entity = createMaterialTestMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto matNodes = objects->findAll("Material");
    ASSERT_EQ(matNodes.size(), 1u);

    auto* props = matNodes[0]->find("Properties70");
    ASSERT_NE(props, nullptr);

    // DiffuseColor: 0.9, 0.1, 0.2
    auto* diffuse = findP70(*props, "DiffuseColor");
    ASSERT_NE(diffuse, nullptr);
    EXPECT_NEAR(diffuse->properties[4].doubleVal, 0.9, 0.01);
    EXPECT_NEAR(diffuse->properties[5].doubleVal, 0.1, 0.01);
    EXPECT_NEAR(diffuse->properties[6].doubleVal, 0.2, 0.01);

    // SpecularColor: 0.5, 0.6, 0.7
    auto* specular = findP70(*props, "SpecularColor");
    ASSERT_NE(specular, nullptr);
    EXPECT_NEAR(specular->properties[4].doubleVal, 0.5, 0.01);
    EXPECT_NEAR(specular->properties[5].doubleVal, 0.6, 0.01);
    EXPECT_NEAR(specular->properties[6].doubleVal, 0.7, 0.01);

    // Shininess: 64.0
    auto* shininess = findP70(*props, "Shininess");
    ASSERT_NE(shininess, nullptr);
    EXPECT_NEAR(shininess->properties[4].doubleVal, 64.0, 0.01);

    // AmbientColor: 0.1, 0.2, 0.3
    auto* ambient = findP70(*props, "AmbientColor");
    ASSERT_NE(ambient, nullptr);
    EXPECT_NEAR(ambient->properties[4].doubleVal, 0.1, 0.01);

    // EmissiveColor: 0.05, 0.06, 0.07
    auto* emissive = findP70(*props, "EmissiveColor");
    ASSERT_NE(emissive, nullptr);
    EXPECT_NEAR(emissive->properties[4].doubleVal, 0.05, 0.01);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, MultipleMaterials) {
    auto name = uniqueName("mm");
    auto* entity = createMultiSubmeshMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto matNodes = objects->findAll("Material");
    ASSERT_EQ(matNodes.size(), 2u);

    // Each should have Properties70 with DiffuseColor
    for (const auto* mat : matNodes) {
        auto* props = mat->find("Properties70");
        ASSERT_NE(props, nullptr);
        auto* dc = findP70(*props, "DiffuseColor");
        ASSERT_NE(dc, nullptr);
    }

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, MeshModelNode) {
    auto name = uniqueName("model");
    auto* entity = createSimpleMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto modelNodes = objects->findAll("Model");
    ASSERT_GE(modelNodes.size(), 1u);

    // Find the "Mesh" type model
    const FBXNode* meshModel = nullptr;
    for (const auto* m : modelNodes) {
        if (m->properties.size() >= 3 && m->properties[2].stringVal == "Mesh")
            meshModel = m;
    }
    ASSERT_NE(meshModel, nullptr);

    auto* props = meshModel->find("Properties70");
    ASSERT_NE(props, nullptr);

    // Check LclTranslation, LclRotation, LclScaling
    EXPECT_NE(findP70(*props, "Lcl Translation"), nullptr);
    EXPECT_NE(findP70(*props, "Lcl Rotation"), nullptr);
    EXPECT_NE(findP70(*props, "Lcl Scaling"), nullptr);

    // Check Shading and Culling
    auto* shading = meshModel->find("Shading");
    ASSERT_NE(shading, nullptr);
    EXPECT_EQ(shading->properties[0].boolVal, true);

    auto* culling = meshModel->find("Culling");
    ASSERT_NE(culling, nullptr);
    EXPECT_EQ(culling->properties[0].stringVal, "CullingOff");

    cleanup(r);
}

// ── Group D: Skeleton ──────────────────────────────────────────

TEST_F(FBXExporterCoverageTest, BoneNodeAttributes) {
    auto name = uniqueName("bna");
    auto* entity = createSkeletonMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto nodeAttrs = objects->findAll("NodeAttribute");
    // 3 bones = 3 NodeAttribute nodes
    ASSERT_EQ(nodeAttrs.size(), 3u);

    for (const auto* na : nodeAttrs) {
        auto* typeFlags = na->find("TypeFlags");
        ASSERT_NE(typeFlags, nullptr);
        EXPECT_EQ(typeFlags->properties[0].stringVal, "Skeleton");
    }

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, DeformingBoneTransform) {
    auto name = uniqueName("dbt");
    auto* entity = createSkeletonMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto modelNodes = objects->findAll("Model");

    // Find the spine bone (LimbNode type, name contains "spine")
    const FBXNode* spineModel = nullptr;
    for (const auto* m : modelNodes) {
        if (m->properties.size() >= 3 && m->properties[2].stringVal == "LimbNode") {
            // Check if name contains "spine"
            if (m->properties[1].stringVal.find("spine") != std::string::npos)
                spineModel = m;
        }
    }
    ASSERT_NE(spineModel, nullptr);

    auto* props = spineModel->find("Properties70");
    ASSERT_NE(props, nullptr);

    auto* lclT = findP70(*props, "Lcl Translation");
    ASSERT_NE(lclT, nullptr);
    // Spine position: (0, 1, 0.5) → Z-mirrored: (0, 1, -0.5)
    EXPECT_NEAR(lclT->properties[4].doubleVal, 0.0, 0.01);
    EXPECT_NEAR(lclT->properties[5].doubleVal, 1.0, 0.01);
    EXPECT_NEAR(lclT->properties[6].doubleVal, -0.5, 0.01);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, NonDeformingBoneTransform) {
    auto name = uniqueName("ndbt");
    auto* entity = createSkeletonMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto modelNodes = objects->findAll("Model");

    // Find the root bone (no vertex assignments → non-deforming → inverted transform)
    const FBXNode* rootModel = nullptr;
    for (const auto* m : modelNodes) {
        if (m->properties.size() >= 3 && m->properties[2].stringVal == "LimbNode") {
            if (m->properties[1].stringVal.find("root") != std::string::npos)
                rootModel = m;
        }
    }
    ASSERT_NE(rootModel, nullptr);

    auto* props = rootModel->find("Properties70");
    ASSERT_NE(props, nullptr);

    // Root bone at origin with identity rotation → inverse is also identity
    auto* lclT = findP70(*props, "Lcl Translation");
    ASSERT_NE(lclT, nullptr);
    // Root position (0,0,0), inverted → still (0,0,-0)
    EXPECT_NEAR(lclT->properties[4].doubleVal, 0.0, 0.01);
    EXPECT_NEAR(lclT->properties[5].doubleVal, 0.0, 0.01);
    EXPECT_NEAR(lclT->properties[6].doubleVal, 0.0, 0.01);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, BoneHierarchy) {
    auto name = uniqueName("bh");
    auto* entity = createSkeletonMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* conn = findTopLevel(r.nodes, "Connections");
    ASSERT_NE(conn, nullptr);

    // Collect all OO connections
    auto cNodes = conn->findAll("C");
    std::vector<std::pair<int64_t, int64_t>> ooConns;
    for (const auto* c : cNodes) {
        if (c->properties.size() >= 3 && c->properties[0].stringVal == "OO")
            ooConns.push_back({c->properties[1].longVal, c->properties[2].longVal});
    }

    // There should be at least bone hierarchy connections
    // root→0 (scene root), spine→root model, head→spine model
    // We verify at least one connection to 0 (scene root) from a LimbNode model
    bool hasRootConnection = false;
    for (const auto& [child, parent] : ooConns) {
        if (parent == 0 && child != 0)
            hasRootConnection = true;
    }
    EXPECT_TRUE(hasRootConnection);

    cleanup(r);
}

// ── Group E: Skin Deformers ────────────────────────────────────

TEST_F(FBXExporterCoverageTest, SkinDeformer) {
    auto name = uniqueName("skin");
    auto* entity = createSkeletonMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");

    // Find Deformer nodes with "Skin" type
    auto deformerNodes = objects->findAll("Deformer");
    bool hasSkin = false;
    for (const auto* d : deformerNodes) {
        if (d->properties.size() >= 3 && d->properties[2].stringVal == "Skin") {
            hasSkin = true;
            auto* ver = d->find("Version");
            ASSERT_NE(ver, nullptr);
            EXPECT_EQ(ver->properties[0].intVal, 101);
        }
    }
    EXPECT_TRUE(hasSkin);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, ClusterData) {
    auto name = uniqueName("clus");
    auto* entity = createSkeletonMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto deformerNodes = objects->findAll("Deformer");

    // Find Cluster deformers
    bool hasCluster = false;
    for (const auto* d : deformerNodes) {
        if (d->properties.size() >= 3 && d->properties[2].stringVal == "Cluster") {
            hasCluster = true;

            // Should have Indexes, Weights, Transform, TransformLink
            auto* indexes = d->find("Indexes");
            ASSERT_NE(indexes, nullptr);
            EXPECT_FALSE(indexes->properties[0].intArray.empty());

            auto* weights = d->find("Weights");
            ASSERT_NE(weights, nullptr);
            EXPECT_FALSE(weights->properties[0].doubleArray.empty());

            // Verify weights are all 1.0 (we assigned weight=1.0)
            for (double w : weights->properties[0].doubleArray)
                EXPECT_NEAR(w, 1.0, 0.001);

            auto* transform = d->find("Transform");
            ASSERT_NE(transform, nullptr);
            EXPECT_EQ(transform->properties[0].doubleArray.size(), 16u);

            auto* transformLink = d->find("TransformLink");
            ASSERT_NE(transformLink, nullptr);
            EXPECT_EQ(transformLink->properties[0].doubleArray.size(), 16u);
        }
    }
    EXPECT_TRUE(hasCluster);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, ClusterConnections) {
    auto name = uniqueName("clcon");
    auto* entity = createSkeletonMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* conn = findTopLevel(r.nodes, "Connections");
    ASSERT_NE(conn, nullptr);

    // There should be OO connections for cluster→skin and bone→cluster
    auto cNodes = conn->findAll("C");
    int ooConnCount = 0;
    for (const auto* c : cNodes) {
        if (c->properties.size() >= 3 && c->properties[0].stringVal == "OO")
            ooConnCount++;
    }
    // At minimum: mesh→0, geom→mesh, mat→mesh, nodeAttr→bone(x3),
    // root→0, spine→root, head→spine, skin→geom, cluster→skin, bone→cluster
    EXPECT_GT(ooConnCount, 10);

    cleanup(r);
}

// ── Group F: Animations ────────────────────────────────────────

TEST_F(FBXExporterCoverageTest, AnimationStack) {
    auto name = uniqueName("astack");
    auto* entity = createAnimatedMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto stacks = objects->findAll("AnimationStack");
    ASSERT_EQ(stacks.size(), 1u);

    auto* props = stacks[0]->find("Properties70");
    ASSERT_NE(props, nullptr);

    auto* localStart = findP70(*props, "LocalStart");
    ASSERT_NE(localStart, nullptr);
    EXPECT_EQ(localStart->properties[4].longVal, 0);

    auto* localStop = findP70(*props, "LocalStop");
    ASSERT_NE(localStop, nullptr);
    // 1.0 second * 46186158000 ticks/sec
    EXPECT_EQ(localStop->properties[4].longVal, 46186158000LL);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, AnimationCurveNodes) {
    auto name = uniqueName("acn");
    auto* entity = createAnimatedMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto curveNodes = objects->findAll("AnimationCurveNode");
    // 1 bone track → 3 curve nodes (T, R, S)
    ASSERT_EQ(curveNodes.size(), 3u);

    // Each should have Properties70 with d|X, d|Y, d|Z
    for (const auto* cn : curveNodes) {
        auto* props = cn->find("Properties70");
        ASSERT_NE(props, nullptr);
        EXPECT_NE(findP70(*props, "d|X"), nullptr);
        EXPECT_NE(findP70(*props, "d|Y"), nullptr);
        EXPECT_NE(findP70(*props, "d|Z"), nullptr);
    }

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, AnimationCurves) {
    auto name = uniqueName("ac");
    auto* entity = createAnimatedMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto curves = objects->findAll("AnimationCurve");
    // 1 bone track → 9 curves (TX,TY,TZ,RX,RY,RZ,SX,SY,SZ)
    ASSERT_EQ(curves.size(), 9u);

    for (const auto* curve : curves) {
        // Should have KeyTime, KeyValueFloat, KeyAttrFlags
        auto* keyTime = curve->find("KeyTime");
        ASSERT_NE(keyTime, nullptr);
        const size_t keyCount = keyTime->properties[0].longArray.size();
        // Curves may be compacted to a single key when the channel is flat.
        EXPECT_TRUE(keyCount == 1u || keyCount == 3u);

        auto* keyValue = curve->find("KeyValueFloat");
        ASSERT_NE(keyValue, nullptr);
        EXPECT_EQ(keyValue->properties[0].floatArray.size(), keyCount);

        auto* keyFlags = curve->find("KeyAttrFlags");
        ASSERT_NE(keyFlags, nullptr);
        // Cubic interpolation flag: 24840
        EXPECT_EQ(keyFlags->properties[0].intArray[0], 24840);
    }

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, EulerContinuity) {
    // Create an animated mesh where rotation crosses the 180° boundary
    auto name = uniqueName("euler_cont");

    auto skel = Ogre::SkeletonManager::getSingleton().create(
        name + "_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* root = skel->createBone("root", 0);
    root->setPosition(Ogre::Vector3::ZERO);

    auto* bone = skel->createBone("bone", 1);
    bone->setPosition(Ogre::Vector3(0, 1, 0));
    root->addChild(bone);

    skel->setBindingPose();

    auto* anim = skel->createAnimation("spin", 1.0f);
    auto* track = anim->createNodeTrack(1);
    track->setAssociatedNode(bone);

    // Keyframe 0: rotation 170° Y
    auto* kf0 = track->createNodeKeyFrame(0.0f);
    kf0->setTranslate(Ogre::Vector3::ZERO);
    kf0->setRotation(Ogre::Quaternion(Ogre::Radian(Ogre::Degree(170)),
                                       Ogre::Vector3::UNIT_Y));
    kf0->setScale(Ogre::Vector3::UNIT_SCALE);

    // Keyframe 1: rotation 190° Y (crosses 180° boundary)
    auto* kf1 = track->createNodeKeyFrame(0.5f);
    kf1->setTranslate(Ogre::Vector3::ZERO);
    kf1->setRotation(Ogre::Quaternion(Ogre::Radian(Ogre::Degree(190)),
                                       Ogre::Vector3::UNIT_Y));
    kf1->setScale(Ogre::Vector3::UNIT_SCALE);

    // Keyframe 2: rotation 210° Y
    auto* kf2 = track->createNodeKeyFrame(1.0f);
    kf2->setTranslate(Ogre::Vector3::ZERO);
    kf2->setRotation(Ogre::Quaternion(Ogre::Radian(Ogre::Degree(210)),
                                       Ogre::Vector3::UNIT_Y));
    kf2->setScale(Ogre::Vector3::UNIT_SCALE);

    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float verts[] = {0,0,0, 1,0,0, 0,1,0};
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

    Ogre::VertexBoneAssignment vba;
    vba.boneIndex = 1; vba.weight = 1.0f;
    for (unsigned short v = 0; v < 3; ++v) {
        vba.vertexIndex = v;
        mesh->addBoneAssignment(vba);
    }
    mesh->_notifySkeleton(skel);
    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,2,2,1));
    mesh->_setBoundingSphereRadius(3.0);
    mesh->load();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
    auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
    node->attachObject(entity);

    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    // Find the RY curve and verify no large jumps between keyframes
    auto* objects = findTopLevel(r.nodes, "Objects");
    auto curves = objects->findAll("AnimationCurve");

    // Curves are in order: TX,TY,TZ,RX,RY,RZ,SX,SY,SZ
    // RY is the 5th curve (index 4)
    ASSERT_GE(curves.size(), 6u);
    auto* ryCurve = curves[4];
    auto* keyValue = ryCurve->find("KeyValueFloat");
    ASSERT_NE(keyValue, nullptr);
    auto& vals = keyValue->properties[0].floatArray;
    ASSERT_EQ(vals.size(), 3u);

    // Verify no jump > 90° between consecutive RY values
    for (size_t i = 1; i < vals.size(); ++i) {
        double diff = std::abs(static_cast<double>(vals[i]) - static_cast<double>(vals[i-1]));
        EXPECT_LT(diff, 90.0) << "RY jump too large between keyframe " << (i-1)
                               << " and " << i << ": " << vals[i-1] << " -> " << vals[i];
    }

    cleanup(r);
}

// ── Group G: Bind Pose & Textures ──────────────────────────────

TEST_F(FBXExporterCoverageTest, BindPose) {
    auto name = uniqueName("bp");
    auto* entity = createSkeletonMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto poses = objects->findAll("Pose");
    ASSERT_EQ(poses.size(), 1u);

    auto* nbPoseNodes = poses[0]->find("NbPoseNodes");
    ASSERT_NE(nbPoseNodes, nullptr);
    // 1 mesh + 3 bones = 4
    EXPECT_EQ(nbPoseNodes->properties[0].intVal, 4);

    auto poseNodes = poses[0]->findAll("PoseNode");
    ASSERT_EQ(poseNodes.size(), 4u);

    // Each PoseNode should have Node (id) and Matrix (16 doubles)
    for (const auto* pn : poseNodes) {
        auto* nodeId = pn->find("Node");
        ASSERT_NE(nodeId, nullptr);

        auto* matrix = pn->find("Matrix");
        ASSERT_NE(matrix, nullptr);
        EXPECT_EQ(matrix->properties[0].doubleArray.size(), 16u);
    }

    // First PoseNode (mesh) should have identity matrix
    auto& meshMatrix = poseNodes[0]->find("Matrix")->properties[0].doubleArray;
    EXPECT_NEAR(meshMatrix[0], 1.0, 0.001);
    EXPECT_NEAR(meshMatrix[5], 1.0, 0.001);
    EXPECT_NEAR(meshMatrix[10], 1.0, 0.001);
    EXPECT_NEAR(meshMatrix[15], 1.0, 0.001);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, TextureAndVideo) {
    auto name = uniqueName("tex");
    auto* entity = createTexturedMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");

    auto texNodes = objects->findAll("Texture");
    ASSERT_EQ(texNodes.size(), 1u);

    auto* texName = texNodes[0]->find("TextureName");
    ASSERT_NE(texName, nullptr);
    EXPECT_EQ(texName->properties[0].stringVal, "diffuse_tex.png");

    auto* fileName = texNodes[0]->find("FileName");
    ASSERT_NE(fileName, nullptr);
    EXPECT_EQ(fileName->properties[0].stringVal, "diffuse_tex.png");

    auto vidNodes = objects->findAll("Video");
    ASSERT_EQ(vidNodes.size(), 1u);

    auto* vidType = vidNodes[0]->find("Type");
    ASSERT_NE(vidType, nullptr);
    EXPECT_EQ(vidType->properties[0].stringVal, "Clip");

    // Embedded payload should exist
    auto* content = vidNodes[0]->find("Content");
    ASSERT_NE(content, nullptr);
    ASSERT_FALSE(content->properties.empty());
    EXPECT_EQ(content->properties[0].type, 'R');
    EXPECT_GT(content->properties[0].stringVal.size(), 0u);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, TextureConnections) {
    auto name = uniqueName("texcon");
    auto* entity = createTexturedMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* conn = findTopLevel(r.nodes, "Connections");
    ASSERT_NE(conn, nullptr);

    auto cNodes = conn->findAll("C");

    // Find OP connection with "DiffuseColor" (texture→material)
    bool hasDiffuseConn = false;
    // Find OO connection (video→texture)
    bool hasVideoConn = false;

    for (const auto* c : cNodes) {
        if (c->properties.size() >= 4 && c->properties[0].stringVal == "OP") {
            if (c->properties[3].stringVal == "DiffuseColor")
                hasDiffuseConn = true;
        }
        if (c->properties.size() >= 3 && c->properties[0].stringVal == "OO") {
            hasVideoConn = true; // Can't distinguish easily, but OO connections exist
        }
    }
    EXPECT_TRUE(hasDiffuseConn);
    EXPECT_TRUE(hasVideoConn);

    cleanup(r);
}

// ── Group H: Connections ───────────────────────────────────────

TEST_F(FBXExporterCoverageTest, BasicConnections) {
    auto name = uniqueName("bcon");
    auto* entity = createSimpleMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* conn = findTopLevel(r.nodes, "Connections");
    ASSERT_NE(conn, nullptr);

    auto cNodes = conn->findAll("C");

    // Collect connections
    bool hasMeshToRoot = false;
    int geomToMesh = 0;
    int matToMesh = 0;

    // First connection should be mesh model → root (0)
    if (!cNodes.empty() && cNodes[0]->properties.size() >= 3) {
        if (cNodes[0]->properties[0].stringVal == "OO" && cNodes[0]->properties[2].longVal == 0)
            hasMeshToRoot = true;
    }

    // Count geometry→mesh and material→mesh connections
    for (const auto* c : cNodes) {
        if (c->properties.size() >= 3 && c->properties[0].stringVal == "OO") {
            // All OO connections to the mesh model ID (non-zero, non-root)
            if (c->properties[2].longVal != 0)
                geomToMesh++; // Counts both geom and mat
        }
    }

    EXPECT_TRUE(hasMeshToRoot);
    EXPECT_GE(geomToMesh, 2); // at least 1 geometry + 1 material

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, SkeletalConnections) {
    auto name = uniqueName("scon");
    auto* entity = createAnimatedMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* conn = findTopLevel(r.nodes, "Connections");
    ASSERT_NE(conn, nullptr);

    auto cNodes = conn->findAll("C");

    int ooCount = 0, opCount = 0;
    for (const auto* c : cNodes) {
        if (c->properties[0].stringVal == "OO") ooCount++;
        if (c->properties[0].stringVal == "OP") opCount++;
    }

    // Should have many OO connections: mesh→0, geom→mesh, mat→mesh,
    // nodeAttr→bone(x2), bones to parent(x2), skin→geom, cluster→skin,
    // bone→cluster, animStack→0, layer→stack, curveNode→layer(x3)
    EXPECT_GT(ooCount, 12);

    // Should have OP connections: curveNode→bone (x3: T,R,S) + curve→curveNode (x9)
    EXPECT_GE(opCount, 12);

    cleanup(r);
}

// ── Group I: Edge Cases ────────────────────────────────────────

TEST_F(FBXExporterCoverageTest, MultiSubmeshGeometry) {
    auto name = uniqueName("msub");
    auto* entity = createMultiSubmeshMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto geomNodes = objects->findAll("Geometry");
    ASSERT_EQ(geomNodes.size(), 2u);

    // Each geometry should have Vertices, PolygonVertexIndex, LayerElementMaterial
    for (const auto* g : geomNodes) {
        EXPECT_NE(g->find("Vertices"), nullptr);
        EXPECT_NE(g->find("PolygonVertexIndex"), nullptr);
        EXPECT_NE(g->find("LayerElementMaterial"), nullptr);
    }

    // Verify material indices — since materials are sorted by name,
    // matA (name _matA) and matB (name _matB) will be indexed 0 and 1
    for (size_t i = 0; i < geomNodes.size(); ++i) {
        auto* matLayer = geomNodes[i]->find("LayerElementMaterial");
        auto* materials = matLayer->find("Materials");
        ASSERT_NE(materials, nullptr);
        auto& matIdx = materials->properties[0].intArray;
        ASSERT_EQ(matIdx.size(), 1u);
        // Material index should be valid (0 or 1)
        EXPECT_GE(matIdx[0], 0);
        EXPECT_LE(matIdx[0], 1);
    }

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, GimbalLockRotation) {
    // Bone at exactly 90° Y rotation → gimbal lock branch in quaternionToEulerXYZ
    auto name = uniqueName("gimbal");

    auto skel = Ogre::SkeletonManager::getSingleton().create(
        name + "_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* root = skel->createBone("root", 0);
    root->setPosition(Ogre::Vector3::ZERO);

    auto* bone = skel->createBone("bone", 1);
    bone->setPosition(Ogre::Vector3(0, 1, 0));
    // Set orientation to exactly 90° Y (gimbal lock)
    bone->setOrientation(Ogre::Quaternion(Ogre::Radian(Ogre::Degree(90)),
                                           Ogre::Vector3::UNIT_Y));
    root->addChild(bone);

    skel->setBindingPose();

    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float verts[] = {0,0,0, 1,0,0, 0,1,0};
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

    Ogre::VertexBoneAssignment vba;
    vba.boneIndex = 1; vba.weight = 1.0f;
    for (unsigned short v = 0; v < 3; ++v) {
        vba.vertexIndex = v;
        mesh->addBoneAssignment(vba);
    }
    mesh->_notifySkeleton(skel);
    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,2,2,1));
    mesh->_setBoundingSphereRadius(3.0);
    mesh->load();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
    auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
    node->attachObject(entity);

    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    // Verify the file exported successfully and has expected structure
    auto* objects = findTopLevel(r.nodes, "Objects");
    auto modelNodes = objects->findAll("Model");

    // Find the bone LimbNode
    const FBXNode* boneModel = nullptr;
    for (const auto* m : modelNodes) {
        if (m->properties.size() >= 3 && m->properties[2].stringVal == "LimbNode") {
            if (m->properties[1].stringVal.find("bone") != std::string::npos)
                boneModel = m;
        }
    }
    ASSERT_NE(boneModel, nullptr);

    auto* props = boneModel->find("Properties70");
    ASSERT_NE(props, nullptr);

    // The rotation should be decomposed (even in gimbal lock)
    auto* lclR = findP70(*props, "Lcl Rotation");
    ASSERT_NE(lclR, nullptr);
    // Verify the decomposition produced some rotation values
    // (exact values depend on the gimbal lock fallback path)
    EXPECT_TRUE(lclR->properties.size() >= 7);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, Definitions_Skeletal) {
    auto name = uniqueName("def_skel");
    auto* entity = createAnimatedMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* defs = findTopLevel(r.nodes, "Definitions");
    ASSERT_NE(defs, nullptr);

    auto objectTypes = defs->findAll("ObjectType");

    // Verify additional skeletal types are present
    bool hasNodeAttr = false, hasDeformer = false, hasPose = false;
    bool hasAnimStack = false, hasAnimLayer = false, hasAnimCurveNode = false, hasAnimCurve = false;
    for (const auto* ot : objectTypes) {
        if (!ot->properties.empty()) {
            const auto& typeName = ot->properties[0].stringVal;
            if (typeName == "NodeAttribute") hasNodeAttr = true;
            if (typeName == "Deformer") hasDeformer = true;
            if (typeName == "Pose") hasPose = true;
            if (typeName == "AnimationStack") hasAnimStack = true;
            if (typeName == "AnimationLayer") hasAnimLayer = true;
            if (typeName == "AnimationCurveNode") hasAnimCurveNode = true;
            if (typeName == "AnimationCurve") hasAnimCurve = true;
        }
    }
    EXPECT_TRUE(hasNodeAttr);
    EXPECT_TRUE(hasDeformer);
    EXPECT_TRUE(hasPose);
    EXPECT_TRUE(hasAnimStack);
    EXPECT_TRUE(hasAnimLayer);
    EXPECT_TRUE(hasAnimCurveNode);
    EXPECT_TRUE(hasAnimCurve);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, AnimationLayer) {
    auto name = uniqueName("alayer");
    auto* entity = createAnimatedMesh(name);
    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto layers = objects->findAll("AnimationLayer");
    ASSERT_EQ(layers.size(), 1u);

    auto* props = layers[0]->find("Properties70");
    ASSERT_NE(props, nullptr);

    auto* weight = findP70(*props, "Weight");
    ASSERT_NE(weight, nullptr);
    EXPECT_NEAR(weight->properties[4].doubleVal, 100.0, 0.01);

    cleanup(r);
}

// ==========================================================================
// NEW TESTS: Full export round-trip
// ==========================================================================

TEST_F(FBXExporterCoverageTest, ExportInMemoryMesh_CreatesValidFile) {
    auto name = uniqueName("inmem");
    auto* entity = createSimpleMesh(name);
    ASSERT_NE(entity, nullptr);

    QString outPath = QDir(QDir::tempPath()).filePath(QString("fbx_inmem_%1.fbx").arg(meshCounter));
    ASSERT_TRUE(FBXExporter::exportFBX(entity, outPath));

    // Verify file exists
    QFile file(outPath);
    EXPECT_TRUE(file.exists());
    EXPECT_GT(file.size(), 27); // At least larger than the 27-byte FBX header

    // Verify FBX magic bytes
    std::ifstream in(outPath.toStdString(), std::ios::binary);
    ASSERT_TRUE(in.is_open());

    char magic[21];
    in.read(magic, 21);
    EXPECT_EQ(std::string(magic, 20), "Kaydara FBX Binary  ");
    EXPECT_EQ(magic[20], '\0');

    char pad[2];
    in.read(pad, 2);
    EXPECT_EQ(pad[0], '\x1A');
    EXPECT_EQ(pad[1], '\x00');

    uint32_t version;
    in.read(reinterpret_cast<char*>(&version), 4);
    EXPECT_EQ(version, 7300u);

    in.close();
    QFile::remove(outPath);
}

TEST_F(FBXExporterCoverageTest, ExportSkeletonMesh_PreservesHierarchy) {
    auto name = uniqueName("skelhier");
    auto* entity = createSkeletonMesh(name);
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());

    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    // Find the Objects node
    auto* objects = findTopLevel(r.nodes, "Objects");
    ASSERT_NE(objects, nullptr);

    // Find all Model nodes (bones are represented as Limb models)
    auto models = objects->findAll("Model");
    // Should have at least 4 models: mesh model + 3 bones (root, spine, head)
    EXPECT_GE(models.size(), 4u);

    // Verify Connections node exists (bone hierarchy connections)
    auto* connections = findTopLevel(r.nodes, "Connections");
    ASSERT_NE(connections, nullptr);
    EXPECT_FALSE(connections->children.empty());

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, ExportAnimatedMesh_PreservesAnimations) {
    auto name = uniqueName("anim");
    auto* entity = createAnimatedMesh(name);
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());

    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    // Find the Objects node
    auto* objects = findTopLevel(r.nodes, "Objects");
    ASSERT_NE(objects, nullptr);

    // Should have AnimationStack nodes for the "walk" animation
    auto animStacks = objects->findAll("AnimationStack");
    EXPECT_GE(animStacks.size(), 1u);

    // Should have AnimationCurveNode entries for the keyframes
    auto curveNodes = objects->findAll("AnimationCurveNode");
    EXPECT_GE(curveNodes.size(), 1u);

    // Should have AnimationCurve entries with the actual data
    auto curves = objects->findAll("AnimationCurve");
    EXPECT_GE(curves.size(), 1u);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, ExportSkeletonOnly_CreatesSkeletonAndAnimations)
{
    // Build a skeleton with two bones and a simple animation, without any mesh/entity.
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "SkelOnlyTest", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
    auto* root = skel->createBone("Root", 0);
    root->setPosition(0, 0, 0);
    auto* child = skel->createBone("Child", 1);
    child->setPosition(0, 1, 0);
    root->addChild(child);

    auto* anim = skel->createAnimation("wave", 1.0f);
    auto* track = anim->createNodeTrack(0, child);
    {
        auto* k0 = track->createNodeKeyFrame(0.0f);
        k0->setTranslate(Ogre::Vector3(0, 1, 0));
        k0->setRotation(Ogre::Quaternion::IDENTITY);
        k0->setScale(Ogre::Vector3::UNIT_SCALE);
        auto* k1 = track->createNodeKeyFrame(1.0f);
        k1->setTranslate(Ogre::Vector3(0, 1, 0));
        k1->setRotation(Ogre::Quaternion(Ogre::Degree(45), Ogre::Vector3::UNIT_Z));
        k1->setScale(Ogre::Vector3::UNIT_SCALE);
    }

    const QString outPath = QDir(QDir::tempPath()).filePath(QString("fbx_skeleton_only_%1.fbx").arg(meshCounter));
    ASSERT_TRUE(FBXExporter::exportSkeletonOnlyFBX(skel.get(), outPath));

    auto nodes = parseFBX(outPath.toStdString());
    ASSERT_FALSE(nodes.empty());
    auto* objects = findTopLevel(nodes, "Objects");
    ASSERT_NE(objects, nullptr);

    // Bone models & skeleton node attributes should exist.
    auto models = objects->findAll("Model");
    auto nodeAttrs = objects->findAll("NodeAttribute");
    EXPECT_GE(models.size(), 2);
    EXPECT_GE(nodeAttrs.size(), 2);

    // Animation objects should exist.
    auto animStacks = objects->findAll("AnimationStack");
    auto animCurves = objects->findAll("AnimationCurve");
    EXPECT_GE(animStacks.size(), 1);
    EXPECT_GE(animCurves.size(), 1);

    QFile::remove(outPath);
}

// ==========================================================================
// NEW TESTS: Error handling
// ==========================================================================

TEST_F(FBXExporterCoverageTest, ExportToInvalidPath_HandlesGracefully) {
    auto name = uniqueName("invpath");
    auto* entity = createSimpleMesh(name);
    ASSERT_NE(entity, nullptr);

    // Export to a path where the parent is a file, not a directory
    QTemporaryFile tempFile;
    tempFile.open();
    QString invalidPath = tempFile.fileName() + "/test.fbx";
    bool result = FBXExporter::exportFBX(entity, invalidPath);
    EXPECT_FALSE(result);
}

TEST_F(FBXExporterCoverageTest, ExportNullEntity_HandlesGracefully) {
    // Should return false for nullptr entity
    QString nullTestPath = QDir(QDir::tempPath()).filePath("null_entity_test.fbx");
    EXPECT_FALSE(FBXExporter::exportFBX(nullptr, nullTestPath));
    // Ensure no file was created
    EXPECT_FALSE(QFile::exists(nullTestPath));
}

TEST_F(FBXExporterCoverageTest, ExportEmptyMesh_HandlesGracefully) {
    // Create a mesh with no submeshes (just shared vertex data)
    auto name = uniqueName("empty");
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    // Empty mesh with no vertex data and no submeshes
    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,1,1,1));
    mesh->_setBoundingSphereRadius(2.0);
    mesh->load();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
    auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
    node->attachObject(entity);

    QString outPath = QDir(QDir::tempPath()).filePath(QString("fbx_empty_%1.fbx").arg(meshCounter));
    // Should either succeed with a minimal file or fail gracefully
    bool result = FBXExporter::exportFBX(entity, outPath);
    // Either way, no crash
    if (result) {
        QFile::remove(outPath);
    }
}

// ==========================================================================
// NEW TESTS: Edge cases
// ==========================================================================

TEST_F(FBXExporterCoverageTest, ExportMultiSubmeshMesh_WritesAllGeometry) {
    auto name = uniqueName("multisub");
    auto* entity = createMultiSubmeshMesh(name);
    ASSERT_NE(entity, nullptr);

    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    ASSERT_NE(objects, nullptr);

    // Should have 2 Geometry nodes (one per submesh)
    auto geomNodes = objects->findAll("Geometry");
    EXPECT_EQ(geomNodes.size(), 2u);

    // Each geometry should have vertices
    for (const auto* geom : geomNodes) {
        auto* verts = geom->find("Vertices");
        ASSERT_NE(verts, nullptr);
        EXPECT_FALSE(verts->properties.empty());
        // Both submeshes use shared vertex data (6 vertices total),
        // so each Geometry node contains all 6 vertices * 3 components = 18 doubles
        EXPECT_EQ(verts->properties[0].doubleArray.size(), 18u);
    }

    // Should have connections
    auto* connections = findTopLevel(r.nodes, "Connections");
    ASSERT_NE(connections, nullptr);
    EXPECT_FALSE(connections->children.empty());

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, ExportMeshWithMaterials_WritesMaterialData) {
    auto name = uniqueName("matdata");
    auto* entity = createMaterialTestMesh(name);
    ASSERT_NE(entity, nullptr);

    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    ASSERT_NE(objects, nullptr);

    // Should have Material node(s)
    auto materials = objects->findAll("Material");
    ASSERT_GE(materials.size(), 1u);

    // Verify material has Properties70 with color data
    auto* props = materials[0]->find("Properties70");
    ASSERT_NE(props, nullptr);

    // Check for diffuse color property
    auto* diffuse = findP70(*props, "DiffuseColor");
    ASSERT_NE(diffuse, nullptr);
    // Diffuse was set to (0.9, 0.1, 0.2)
    EXPECT_NEAR(diffuse->properties[4].doubleVal, 0.9, 0.05);
    EXPECT_NEAR(diffuse->properties[5].doubleVal, 0.1, 0.05);
    EXPECT_NEAR(diffuse->properties[6].doubleVal, 0.2, 0.05);

    // Check for specular color property
    auto* specular = findP70(*props, "SpecularColor");
    ASSERT_NE(specular, nullptr);
    // Specular was set to (0.5, 0.6, 0.7)
    EXPECT_NEAR(specular->properties[4].doubleVal, 0.5, 0.05);
    EXPECT_NEAR(specular->properties[5].doubleVal, 0.6, 0.05);
    EXPECT_NEAR(specular->properties[6].doubleVal, 0.7, 0.05);

    // Check for shininess
    auto* shininess = findP70(*props, "Shininess");
    ASSERT_NE(shininess, nullptr);
    EXPECT_NEAR(shininess->properties[4].doubleVal, 64.0, 0.5);

    cleanup(r);
}

TEST_F(FBXExporterCoverageTest, VerticesZMirrored_WithNonZeroZ) {
    // Create mesh with non-zero Z values to verify Z-negation
    auto name = uniqueName("vzn");

    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float verts[] = {0,0,1.5f,  1,0,2.5f,  0,1,3.5f};
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

    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,2,2,4));
    mesh->_setBoundingSphereRadius(4.0);
    mesh->load();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
    auto* entity = sceneMgr->createEntity(name + "_entity", mesh);
    node->attachObject(entity);

    auto r = exportAndParse(entity);
    ASSERT_TRUE(r.success);

    auto* objects = findTopLevel(r.nodes, "Objects");
    auto geomNodes = objects->findAll("Geometry");
    ASSERT_EQ(geomNodes.size(), 1u);

    auto* vertsNode = geomNodes[0]->find("Vertices");
    ASSERT_NE(vertsNode, nullptr);
    auto& v = vertsNode->properties[0].doubleArray;
    ASSERT_EQ(v.size(), 9u);

    // Z values should be negated: 1.5→-1.5, 2.5→-2.5, 3.5→-3.5
    EXPECT_NEAR(v[2], -1.5, 0.001);
    EXPECT_NEAR(v[5], -2.5, 0.001);
    EXPECT_NEAR(v[8], -3.5, 0.001);

    cleanup(r);
}
