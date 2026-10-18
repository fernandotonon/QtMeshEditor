#include <gtest/gtest.h>

#include "DnaReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cmath>

#ifdef ENABLE_OPENRIGLOGIC
#include <dna/BinaryStreamWriter.h>
#include <dna/Writer.h>
#include <trio/streams/MemoryStream.h>
#endif

using namespace Dna;

namespace {
double dot4(const std::array<double, 4>& a, const std::array<double, 4>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}
}  // namespace

// ---- pure helpers (run in every build) ------------------------------------

TEST(DnaMath, EulerIdentity)
{
    const auto q = eulerToQuaternion({0, 0, 0}, "xyz", true);
    EXPECT_NEAR(q[0], 1.0, 1e-12);
    EXPECT_NEAR(q[1] + q[2] + q[3], 0.0, 1e-12);
}

TEST(DnaMath, SingleAxisNinetyDegrees)
{
    const auto q = eulerToQuaternion({0, 90, 0}, "xyz", true);
    EXPECT_NEAR(q[0], std::cos(M_PI / 4), 1e-9);
    EXPECT_NEAR(q[2], std::sin(M_PI / 4), 1e-9);
    // Radians path agrees with degrees path.
    const auto r = eulerToQuaternion({0, M_PI / 2, 0}, "xyz", false);
    EXPECT_NEAR(std::abs(dot4(q, r)), 1.0, 1e-9);
}

TEST(DnaMath, SignFlipsDirection)
{
    const auto p = eulerToQuaternion({0, 0, 30}, "xyz", true, {1, 1, 1});
    const auto n = eulerToQuaternion({0, 0, 30}, "xyz", true, {1, 1, -1});
    EXPECT_NEAR(p[3], -n[3], 1e-12);
}

TEST(DnaMath, FirstLetterIsAppliedFirst)
{
    // xyz: rotate about X first (+90: Y->Z), then about Y (+90: Z->X) => Y maps to +X.
    const auto q = eulerToQuaternion({90, 90, 0}, "xyz", true);
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    // rotate (0,1,0)
    const double vx = (1 - 2 * (y * y + z * z)) * 0 + 2 * (x * y - z * w) * 1;
    EXPECT_NEAR(vx, 1.0, 1e-9);
}

TEST(DnaMath, SequenceOrderMatters)
{
    const auto a = eulerToQuaternion({90, 90, 0}, "xyz", true);
    const auto b = eulerToQuaternion({90, 90, 0}, "yxz", true);
    EXPECT_LT(std::abs(dot4(a, b)), 0.99);
}

TEST(DnaMath, WorldToLocalAccumulatesBack)
{
    std::vector<Joint> j(3);
    j[0].parent = -1; j[0].translation = {0, 0, 0};
    j[1].parent = 0;  j[1].translation = {0, 1, 0};
    j[2].parent = 1;  j[2].translation = {0, 1.5, 0};
    ASSERT_TRUE(worldToLocal(j));
    EXPECT_NEAR(j[1].translation[1], 1.0, 1e-9);
    EXPECT_NEAR(j[2].translation[1], 0.5, 1e-9);
}

TEST(DnaMath, WorldToLocalRotatesOffsetsIntoParentFrame)
{
    std::vector<Joint> j(2);
    j[0].parent = -1;
    j[0].orientation = eulerToQuaternion({0, 0, 90}, "xyz", true);  // +90 about Z
    j[1].parent = 0;
    j[1].orientation = j[0].orientation;
    j[1].translation = {0, 1, 0};  // world offset straight up
    ASSERT_TRUE(worldToLocal(j));
    // In the parent's frame (rotated +90 about Z) world +Y is local +X... inverse: local = R^-1 * (0,1,0) = (1,0,0)
    EXPECT_NEAR(j[1].translation[0], 1.0, 1e-9);
    EXPECT_NEAR(j[1].translation[1], 0.0, 1e-9);
    EXPECT_NEAR(j[1].orientation[0], 1.0, 1e-9);  // same world rot => identity local
}

TEST(DnaMath, WorldToLocalAcceptsChildBeforeParent)
{
    std::vector<Joint> j(2);
    j[0].parent = 1; j[0].translation = {0, 3, 0};
    j[1].parent = -1; j[1].translation = {0, 1, 0};
    ASSERT_TRUE(worldToLocal(j));
    EXPECT_NEAR(j[0].translation[1], 2.0, 1e-9);
}

TEST(DnaMath, WorldToLocalDetectsCycle)
{
    std::vector<Joint> j(2);
    j[0].parent = 1;
    j[1].parent = 0;
    EXPECT_FALSE(worldToLocal(j));
}

TEST(DnaExpand, FanTriangulatesAndFlipsV)
{
    Mesh m;
    m.positions = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
    m.normals = {0,0,1};
    m.uvs = {0,0, 1,0, 1,1, 0,1};
    for (uint32_t i = 0; i < 4; ++i) m.layouts.push_back({i, i, 0});
    m.faces = {{0, 1, 2, 3}};
    const ExpandedMesh e = expand(m, true);
    EXPECT_EQ(e.indices.size(), 6u);
    EXPECT_EQ(e.positionOf.size(), 4u);
    EXPECT_FLOAT_EQ(e.uvs[1], 1.0f);  // v=0 -> 1
    EXPECT_FLOAT_EQ(e.uvs[3], 1.0f);
    EXPECT_FLOAT_EQ(e.uvs[5], 0.0f);
    EXPECT_EQ(expand(m, false).uvs[1], 0.0f);
}

TEST(DnaExpand, DropsFacesWithBadIndices)
{
    Mesh m;
    m.positions = {0,0,0, 1,0,0, 0,1,0};
    m.normals = {0,0,1};
    m.uvs = {0,0};
    for (uint32_t i = 0; i < 3; ++i) m.layouts.push_back({i, 0, 0});
    m.layouts.push_back({99, 0, 0});  // out-of-range position
    m.faces = {{0, 1, 2}, {0, 1, 3}, {0, 1}, {0, 1, 77}};
    const ExpandedMesh e = expand(m, false);
    EXPECT_EQ(e.indices.size(), 3u);
    EXPECT_EQ(e.skippedFaces, 3);
}

TEST(DnaExpand, SkinFollowsPosition)
{
    Mesh m;
    m.positions = {0,0,0, 1,0,0, 0,1,0};
    m.normals = {0,0,1};
    m.uvs = {0,0};
    m.layouts = {{0,0,0}, {1,0,0}, {2,0,0}, {1,0,0}};  // position 1 used twice
    m.faces = {{0,1,2}};
    m.skin = {{{0, 1.0f}}, {{1, 0.5f}, {0, 0.5f}}, {{2, 1.0f}}};
    const ExpandedMesh e = expand(m, false);
    ASSERT_EQ(e.skin.size(), 4u);
    EXPECT_EQ(e.skin[1].size(), 2u);
    EXPECT_EQ(e.skin[3].size(), 2u);
}

// ---- reader ---------------------------------------------------------------

TEST(DnaReader, MissingFileIsActionable)
{
    const ReadResult r = read("/nonexistent/head.dna");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
    EXPECT_EQ(r.code, available() ? ErrorCode::FileNotFound : ErrorCode::Unavailable);
}

#ifndef ENABLE_OPENRIGLOGIC
TEST(DnaReader, UnavailableBuildSaysHowToEnable)
{
    EXPECT_FALSE(available());
    const ReadResult r = readFromBuffer({1, 2, 3});
    EXPECT_EQ(r.code, ErrorCode::Unavailable);
    EXPECT_TRUE(r.error.contains("ENABLE_OPENRIGLOGIC"));
}
#else

namespace {

// A tiny but complete character: 3-joint spine (cm, world space, Y up), one
// quad mesh skinned to the joints, and one blend-shape channel.
std::vector<uint8_t> makeSyntheticDna(bool withGeometry = true, uint16_t lodCount = 1)
{
    auto stream = pma::makeScoped<dna::MemoryStream>();
    auto writer = pma::makeScoped<dna::BinaryStreamWriter>(stream.get());

    writer->setName("synthetic");
    writer->setArchetype(dna::Archetype::other);
    writer->setGender(dna::Gender::other);
    writer->setAge(30);
    writer->setTranslationUnit(dna::TranslationUnit::cm);
    writer->setRotationUnit(dna::RotationUnit::degrees);
    writer->setCoordinateSystem({tdm::axis_dir::right, tdm::axis_dir::up, tdm::axis_dir::back});
    writer->setRotationSequence(dna::RotationSequence::xyz);
    writer->setLODCount(lodCount);
    writer->setDBMaxLOD(0);
    writer->setDBComplexity("test");
    writer->setDBName("test");

    const char* names[] = {"root", "spine", "head"};
    for (uint16_t i = 0; i < 3; ++i) writer->setJointName(i, names[i]);
    const uint16_t parents[] = {0, 0, 1};  // root is its own parent
    writer->setJointHierarchy(parents, 3);
    const dna::Vector3 t[] = {{0, 0, 0}, {0, 100, 0}, {0, 150, 0}};
    const dna::Vector3 r[] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    writer->setNeutralJointTranslations(t, 3);
    writer->setNeutralJointRotations(r, 3);
    const uint16_t allJoints[] = {0, 1, 2};
    writer->setJointIndices(0, allJoints, 3);
    for (uint16_t l = 0; l < lodCount; ++l) writer->setLODJointMapping(l, 0);

    writer->setBlendShapeChannelName(0, "smile");
    const uint16_t bsIdx[] = {0};
    writer->setBlendShapeChannelIndices(0, bsIdx, 1);
    for (uint16_t l = 0; l < lodCount; ++l) writer->setLODBlendShapeChannelMapping(l, 0);

    writer->setMeshName(0, "body_lod0_mesh");
    const uint16_t meshIdx[] = {0};
    writer->setMeshIndices(0, meshIdx, 1);
    for (uint16_t l = 0; l < lodCount; ++l) writer->setLODMeshMapping(l, 0);
    writer->setMeshBlendShapeChannelMapping(0, 0, 0);

    if (withGeometry) {
        const dna::Position pos[] = {{-10, 0, 0}, {10, 0, 0}, {10, 150, 0}, {-10, 150, 0}};
        writer->setVertexPositions(0, pos, 4);
        const dna::TextureCoordinate uv[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        writer->setVertexTextureCoordinates(0, uv, 4);
        const dna::Normal nrm[] = {{0, 0, 1}};
        writer->setVertexNormals(0, nrm, 1);
        const dna::VertexLayout layouts[] = {{0, 0, 0}, {1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
        writer->setVertexLayouts(0, layouts, 4);
        const uint32_t face[] = {0, 1, 2, 3};
        writer->setFaceVertexLayoutIndices(0, 0, face, 4);
        writer->setMaximumInfluencePerVertex(0, 2);
        const float w0[] = {1.0f};
        const uint16_t j0[] = {0};
        const float w1[] = {0.5f, 0.5f};
        const uint16_t j1[] = {1, 2};
        writer->setSkinWeightsValues(0, 0, w0, 1);
        writer->setSkinWeightsJointIndices(0, 0, j0, 1);
        writer->setSkinWeightsValues(0, 1, w0, 1);
        writer->setSkinWeightsJointIndices(0, 1, j0, 1);
        writer->setSkinWeightsValues(0, 2, w1, 2);
        writer->setSkinWeightsJointIndices(0, 2, j1, 2);
        writer->setSkinWeightsValues(0, 3, w1, 2);
        writer->setSkinWeightsJointIndices(0, 3, j1, 2);
        writer->setBlendShapeChannelIndex(0, 0, 0);
        const dna::Delta d[] = {{0, 10, 0}, {0, 10, 0}};
        writer->setBlendShapeTargetDeltas(0, 0, d, 2);
        const uint32_t vi[] = {2, 3};
        writer->setBlendShapeTargetVertexIndices(0, 0, vi, 2);
    }
    writer->write();

    const uint64_t n = stream->size();
    std::vector<uint8_t> bytes(n);
    stream->seek(0);
    stream->read(reinterpret_cast<char*>(bytes.data()), n);
    return bytes;
}

}  // namespace

TEST(DnaReader, SyntheticRoundTripDescriptorAndJoints)
{
    ASSERT_TRUE(available());
    ReadOptions opt;
    opt.convertCoordinateSystem = false;
    opt.neutralSpace = NeutralSpace::World;  // the synthetic joints are authored as world positions
    const ReadResult r = readFromBuffer(makeSyntheticDna(), opt);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const Rig& g = r.rig;
    EXPECT_EQ(g.descriptor.name, "synthetic");
    EXPECT_EQ(g.descriptor.translationUnit, "cm");
    ASSERT_EQ(g.joints.size(), 3u);
    EXPECT_EQ(g.joints[0].parent, -1);   // root self-parent -> -1
    EXPECT_EQ(g.joints[2].parent, 1);
    // cm -> m (default) and world -> local: head sits 0.5 m above the spine.
    EXPECT_NEAR(g.joints[1].translation[1], 1.0, 1e-5);
    EXPECT_NEAR(g.joints[2].translation[1], 0.5, 1e-5);
    EXPECT_EQ(g.lodJointIndices.size(), 3u);
}

TEST(DnaReader, SyntheticGeometrySkinAndBlendShapes)
{
    ReadOptions opt;
    opt.convertCoordinateSystem = false;
    const ReadResult r = readFromBuffer(makeSyntheticDna(), opt);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.rig.meshes.size(), 1u);
    const Mesh& m = r.rig.meshes[0];
    EXPECT_EQ(m.name, "body_lod0_mesh");
    EXPECT_EQ(m.positions.size(), 12u);
    EXPECT_NEAR(m.positions[7], 1.5f, 1e-5f);  // 150 cm -> 1.5 m
    EXPECT_EQ(m.faces.size(), 1u);
    ASSERT_EQ(m.skin.size(), 4u);
    EXPECT_EQ(m.skin[2].size(), 2u);
    ASSERT_EQ(m.blendShapeTargets.size(), 1u);
    EXPECT_EQ(m.blendShapeTargets[0].channel, 0);
    EXPECT_EQ(r.rig.blendShapeChannels, QStringList{"smile"});
    EXPECT_NEAR(m.blendShapeTargets[0].deltas[1], 0.1f, 1e-5f);
    EXPECT_EQ(expand(m, true).indices.size(), 6u);
}

TEST(DnaReader, PreserveUnitsKeepsCentimetres)
{
    ReadOptions opt;
    opt.convertCoordinateSystem = false;
    opt.units = Units::Preserve;
    const ReadResult r = readFromBuffer(makeSyntheticDna(), opt);
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.rig.joints[1].translation[1], 100.0, 1e-3);  // parent-relative (default): taken verbatim
}

TEST(DnaReader, ParentRelativeIsTheDefaultAndNotAccumulated)
{
    ReadOptions opt;
    opt.convertCoordinateSystem = false;
    const ReadResult r = readFromBuffer(makeSyntheticDna(), opt);
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.rig.joints[2].translation[1], 1.5, 1e-5);  // taken verbatim
}

TEST(DnaReader, BehaviourInventoryOnlyWhenRequested)
{
    ReadOptions opt;
    opt.convertCoordinateSystem = false;
    EXPECT_FALSE(readFromBuffer(makeSyntheticDna(), opt).rig.behavior.loaded);
    opt.loadBehavior = true;
    const ReadResult r = readFromBuffer(makeSyntheticDna(), opt);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.rig.behavior.loaded);
    EXPECT_EQ(r.rig.behavior.blendShapeChannels, 1);
}

TEST(DnaReader, CoordinateConversionKeepsYUpCharacterUpright)
{
    // Source is x=right y=up z=back (a MetaHuman faces -Z); the target faces +Z,
    // a 180-degree turn about Y: X flips, height is untouched.
    const ReadResult r = readFromBuffer(makeSyntheticDna());
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.rig.coordinateSystemConverted);
    EXPECT_NEAR(r.rig.meshes[0].positions[7], 1.5f, 1e-5f);
    EXPECT_NEAR(r.rig.meshes[0].positions[0], 0.1f, 1e-5f);  // was -10 cm
    EXPECT_NEAR(r.rig.meshes[0].positions[1 * 3], -0.1f, 1e-5f);
}

TEST(DnaReader, InvalidLodIsRejectedWithRange)
{
    ReadOptions opt;
    opt.lod = 3;
    const ReadResult r = readFromBuffer(makeSyntheticDna(), opt);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, ErrorCode::InvalidLod);
    EXPECT_TRUE(r.error.contains("LOD 3"));
}

TEST(DnaReader, GarbageIsNotDna)
{
    std::vector<uint8_t> junk(256, 0x41);
    const ReadResult r = readFromBuffer(junk);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.code == ErrorCode::NotDna || r.code == ErrorCode::Corrupt || r.code == ErrorCode::Other)
        << r.error.toStdString();
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(DnaReader, TruncatedFileIsRejected)
{
    std::vector<uint8_t> bytes = makeSyntheticDna();
    ASSERT_GT(bytes.size(), 64u);
    bytes.resize(bytes.size() / 2);
    const ReadResult r = readFromBuffer(bytes);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(DnaReader, ReadsFromDisk)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("c.dna");
    const std::vector<uint8_t> bytes = makeSyntheticDna();
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size()));
    f.close();
    ASSERT_EQ(QFileInfo(path).size(), static_cast<qint64>(bytes.size()));
    const ReadResult r = read(path);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.rig.joints.size(), 3u);
}

#endif  // ENABLE_OPENRIGLOGIC
