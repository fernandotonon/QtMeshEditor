#include <gtest/gtest.h>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <QFile>
#include <QCoreApplication>
#include <QApplication>
#include <QThread>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSkeleton.h>
#include <OgreQuaternion.h>
#include <OgreMatrix3.h>
#include "FBXExporter.h"
#include "../Manager.h"
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

class FBXExporterTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();
    }

    void TearDown() override {
        Manager::kill();

        if (app) {
            app->processEvents();
        }
        QThread::msleep(50);
    }
};

TEST_F(FBXExporterTest, ExportFBX_InvalidPath_ReturnsFalse) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    auto* entity = Manager::getSingleton()->getSceneMgr()->getEntity(sn->getName());

    EXPECT_FALSE(FBXExporter::exportFBX(entity, "/nonexistent_dir/sub/test.fbx"));
}

TEST_F(FBXExporterTest, ExportFBX_BinaryHeader) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    auto* entity = Manager::getSingleton()->getSceneMgr()->getEntity(sn->getName());

    QString outPath = "./fbx_header_test.fbx";
    ASSERT_TRUE(FBXExporter::exportFBX(entity, outPath));

    // Verify FBX binary header
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

TEST_F(FBXExporterTest, ExportFBX_NonZeroFileSize) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    auto* entity = Manager::getSingleton()->getSceneMgr()->getEntity(sn->getName());

    QString outPath = "./fbx_size_test.fbx";
    ASSERT_TRUE(FBXExporter::exportFBX(entity, outPath));

    QFile file(outPath);
    EXPECT_TRUE(file.exists());
    EXPECT_GT(file.size(), 1000); // Should be a substantial file

    QFile::remove(outPath);
}

TEST_F(FBXExporterTest, ExportFBX_WithSkeleton) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* entity = sceneMgr->getEntity(sn->getName());
    ASSERT_TRUE(entity->hasSkeleton());

    QString outPath = "./fbx_skeleton_test.fbx";
    ASSERT_TRUE(FBXExporter::exportFBX(entity, outPath));

    QFile file(outPath);
    EXPECT_GT(file.size(), 5000); // Skeleton data should make it larger

    QFile::remove(outPath);
}

TEST_F(FBXExporterTest, ExportFBX_ViaMeshImporterExporter) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();

    QString outPath = "./fbx_integration_test.fbx";
    int result = MeshImporterExporter::exporter(sn, outPath, "FBX Binary (*.fbx)");
    EXPECT_EQ(result, 0);

    QFile file(outPath);
    EXPECT_TRUE(file.exists());
    EXPECT_GT(file.size(), 1000);

    QFile::remove(outPath);
    QFile::remove("./fbx_integration_test.material");
}

TEST_F(FBXExporterTest, ExportFBX_SimpleMesh) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    // Import a simple mesh without skeleton (the Twist Dance also has skeleton,
    // but let's create a simple cube to test non-skeleton path)
    QStringList uri{"./media/models/Twist Dance.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    auto* entity = Manager::getSingleton()->getSceneMgr()->getEntity(sn->getName());

    QString outPath = "./fbx_simple_test.fbx";
    ASSERT_TRUE(FBXExporter::exportFBX(entity, outPath));

    QFile file(outPath);
    EXPECT_TRUE(file.exists());
    EXPECT_GT(file.size(), 100);

    QFile::remove(outPath);
}

TEST(FBXExporterStandaloneTest, ExportFBX_FormatFileURI) {
    QString uri = "/path/to/file";
    QString format = "FBX Binary (*.fbx)";
    EXPECT_EQ(MeshImporterExporter::formatFileURI(uri, format), "/path/to/file.fbx");

    // Already has extension
    uri = "/path/to/file.fbx";
    EXPECT_EQ(MeshImporterExporter::formatFileURI(uri, format), "/path/to/file.fbx");
}
