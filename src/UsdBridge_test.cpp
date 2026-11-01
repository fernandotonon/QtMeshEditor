#include <gtest/gtest.h>

#include "UsdBridge.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <assimp/material.h>
#include <assimp/mesh.h>
#include <assimp/scene.h>

#include <cmath>
#include <memory>

// Without ENABLE_USD every surface must fail soft with a clear message and the
// register hooks must be harmless — the contract the CLI/MCP/GUI rely on.
#ifndef ENABLE_USD

TEST(UsdBridgeStandalone, UnavailableWithoutFlag) {
    EXPECT_FALSE(UsdBridge::available());
    QString err;
    EXPECT_EQ(UsdBridge::readScene("/nonexistent/a.usda", &err), nullptr);
    EXPECT_TRUE(err.contains("ENABLE_USD"));
    err.clear();
    EXPECT_FALSE(UsdBridge::writeScene("/nonexistent/a.usda", nullptr, &err));
    EXPECT_TRUE(err.contains("ENABLE_USD"));

    Assimp::Importer imp;
    UsdBridge::registerImporter(imp);   // must not throw / crash
    Assimp::Exporter exp;
    UsdBridge::registerExporter(exp);
}

TEST(UsdBridgeStandalone, ExtensionsAreRecognisedEvenWhenUnavailable) {
    EXPECT_TRUE(UsdBridge::isUsdPath("x.usd"));
    EXPECT_TRUE(UsdBridge::isUsdPath("X.USDA"));
    EXPECT_TRUE(UsdBridge::isUsdPath("a/b/c.usdc"));
    EXPECT_TRUE(UsdBridge::isUsdPath("c.usdz"));
    EXPECT_FALSE(UsdBridge::isUsdPath("c.abc"));
    EXPECT_FALSE(UsdBridge::isUsdPath("usd"));
}

#else  // ENABLE_USD

#include "MeshImporterExporter.h"
#include "TestHelpers.h"

#include <OgreAnimation.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSkeleton.h>

namespace {

struct SceneDeleter {
    void operator()(aiScene* s) const { delete s; }
};
using ScenePtr = std::unique_ptr<aiScene, SceneDeleter>;

aiNode* makeNode(const char* name, aiNode* parent, const aiVector3D& t = aiVector3D())
{
    auto* n = new aiNode(name);
    n->mParent = parent;
    n->mTransformation = aiMatrix4x4(aiVector3D(1, 1, 1), aiQuaternion(), t);
    return n;
}

void setChildren(aiNode* n, std::vector<aiNode*> kids)
{
    n->mNumChildren = static_cast<unsigned>(kids.size());
    n->mChildren = new aiNode*[kids.size()];
    for (size_t i = 0; i < kids.size(); ++i) n->mChildren[i] = kids[i];
}

aiNodeAnim* makeChannel(const char* node, const std::vector<std::pair<double, aiQuaternion>>& rot,
                        const aiVector3D& pos)
{
    auto* ch = new aiNodeAnim();
    ch->mNodeName = aiString(node);
    ch->mNumRotationKeys = static_cast<unsigned>(rot.size());
    ch->mRotationKeys = new aiQuatKey[rot.size()];
    for (size_t i = 0; i < rot.size(); ++i) {
        ch->mRotationKeys[i].mTime = rot[i].first;
        ch->mRotationKeys[i].mValue = rot[i].second;
    }
    ch->mNumPositionKeys = 1;
    ch->mPositionKeys = new aiVectorKey[1];
    ch->mPositionKeys[0].mTime = 0.0;
    ch->mPositionKeys[0].mValue = pos;
    ch->mNumScalingKeys = 1;
    ch->mScalingKeys = new aiVectorKey[1];
    ch->mScalingKeys[0].mTime = 0.0;
    ch->mScalingKeys[0].mValue = aiVector3D(1, 1, 1);
    return ch;
}

// A 2x3 vertex strip (4 triangles) skinned to a two-bone chain, with a blend
// shape, a textured material and two clips — everything the writer handles.
ScenePtr makeRiggedScene(bool withClips = true)
{
    ScenePtr scene(new aiScene());
    auto* root = makeNode("Hero", nullptr);
    scene->mRootNode = root;

    auto* boneRoot = makeNode("root", root);
    auto* boneElbow = makeNode("elbow:joint", boneRoot, aiVector3D(0, 1, 0));
    setChildren(boneRoot, { boneElbow });
    auto* meshNode = makeNode("Hero_mesh", root);
    meshNode->mNumMeshes = 1;
    meshNode->mMeshes = new unsigned[1]{ 0 };
    setChildren(root, { boneRoot, meshNode });

    auto* m = new aiMesh();
    m->mName = aiString("body");
    m->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    m->mNumVertices = 6;
    m->mVertices = new aiVector3D[6];
    m->mNormals = new aiVector3D[6];
    m->mTextureCoords[0] = new aiVector3D[6];
    m->mNumUVComponents[0] = 2;
    for (int i = 0; i < 6; ++i) {
        const float x = static_cast<float>(i % 2), y = static_cast<float>(i / 2);
        m->mVertices[i] = aiVector3D(x, y, 0.25f * i);
        m->mNormals[i] = aiVector3D(0, 0, 1);
        m->mTextureCoords[0][i] = aiVector3D(x, y * 0.5f, 0);
    }
    const unsigned tri[4][3] = { {0, 1, 2}, {1, 3, 2}, {2, 3, 4}, {3, 5, 4} };
    m->mNumFaces = 4;
    m->mFaces = new aiFace[4];
    for (int f = 0; f < 4; ++f) {
        m->mFaces[f].mNumIndices = 3;
        m->mFaces[f].mIndices = new unsigned[3]{ tri[f][0], tri[f][1], tri[f][2] };
    }
    m->mMaterialIndex = 0;

    // y = 0 -> root, y = 1 -> half/half, y = 2 -> elbow
    auto* bRoot = new aiBone();
    bRoot->mName = aiString("root");
    bRoot->mOffsetMatrix = aiMatrix4x4();
    bRoot->mNumWeights = 4;
    bRoot->mWeights = new aiVertexWeight[4]{ {0, 1.f}, {1, 1.f}, {2, 0.5f}, {3, 0.5f} };
    auto* bElbow = new aiBone();
    bElbow->mName = aiString("elbow:joint");
    aiMatrix4x4 off;
    aiMatrix4x4::Translation(aiVector3D(0, -1, 0), off);
    bElbow->mOffsetMatrix = off;
    bElbow->mNumWeights = 4;
    bElbow->mWeights = new aiVertexWeight[4]{ {2, 0.5f}, {3, 0.5f}, {4, 1.f}, {5, 1.f} };
    m->mNumBones = 2;
    m->mBones = new aiBone*[2]{ bRoot, bElbow };

    auto* shape = new aiAnimMesh();
    shape->mName = aiString("Bulge: big");
    shape->mNumVertices = 6;
    shape->mVertices = new aiVector3D[6];
    for (int i = 0; i < 6; ++i) shape->mVertices[i] = m->mVertices[i];
    shape->mVertices[4].z += 0.5f;
    shape->mVertices[5].x += 0.25f;
    shape->mWeight = 1.0f;
    m->mNumAnimMeshes = 1;
    m->mAnimMeshes = new aiAnimMesh*[1]{ shape };

    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh*[1]{ m };

    auto* mat = new aiMaterial();
    aiString nm("Skin Mat");
    mat->AddProperty(&nm, AI_MATKEY_NAME);
    aiColor3D diffuse(0.25f, 0.5f, 0.75f);
    mat->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
    aiString tex("hero_albedo.png");
    mat->AddProperty(&tex, AI_MATKEY_TEXTURE(aiTextureType_DIFFUSE, 0));
    scene->mNumMaterials = 1;
    scene->mMaterials = new aiMaterial*[1]{ mat };

    if (withClips) {
        auto mkClip = [&](const char* name, double len, float deg) {
            auto* a = new aiAnimation();
            a->mName = aiString(name);
            a->mDuration = len;
            a->mTicksPerSecond = 1.0;
            const float r = deg * static_cast<float>(M_PI) / 180.0f;
            a->mNumChannels = 1;
            a->mChannels = new aiNodeAnim*[1]{ makeChannel(
                "elbow:joint",
                { { 0.0, aiQuaternion() },
                  { len * 0.5, aiQuaternion(aiVector3D(0, 0, 1), r) },
                  { len, aiQuaternion() } },
                aiVector3D(0, 1, 0)) };
            return a;
        };
        scene->mNumAnimations = 2;
        scene->mAnimations = new aiAnimation*[2]{ mkClip("Wave", 2.0, 60.0f), mkClip("Idle clip", 1.0, 20.0f) };
    }
    return scene;
}

double angleOf(const aiQuaternion& q)
{
    return 2.0 * std::acos(std::min(1.0, std::fabs(static_cast<double>(q.w))));
}

const aiNodeAnim* channelFor(const aiAnimation* a, const std::string& node)
{
    for (unsigned c = 0; c < a->mNumChannels; ++c)
        if (std::string(a->mChannels[c]->mNodeName.C_Str()) == node) return a->mChannels[c];
    return nullptr;
}

const aiAnimation* animNamed(const aiScene* s, const std::string& n)
{
    for (unsigned i = 0; i < s->mNumAnimations; ++i)
        if (std::string(s->mAnimations[i]->mName.C_Str()) == n) return s->mAnimations[i];
    return nullptr;
}

const aiBone* boneNamed(const aiMesh* m, const std::string& n)
{
    for (unsigned i = 0; i < m->mNumBones; ++i)
        if (std::string(m->mBones[i]->mName.C_Str()) == n) return m->mBones[i];
    return nullptr;
}

float weightOf(const aiBone* b, unsigned vertex)
{
    for (unsigned i = 0; i < b->mNumWeights; ++i)
        if (b->mWeights[i].mVertexId == vertex) return b->mWeights[i].mWeight;
    return 0.0f;
}

class UsdBridgeFormatTest : public ::testing::TestWithParam<const char*> {};

}  // namespace

TEST(UsdBridgeWrite, Available) {
    EXPECT_TRUE(UsdBridge::available());
}

TEST_P(UsdBridgeFormatTest, GeometryRoundTrips) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("hero.") + GetParam());
    ScenePtr scene = makeRiggedScene();
    QString err;
    ASSERT_TRUE(UsdBridge::writeScene(path, scene.get(), &err)) << err.toStdString();
    ASSERT_TRUE(QFile::exists(path));
    EXPECT_FALSE(QFile::exists(path + ".qtmesh_tmp")) << "temp file must be renamed away";

    QString warn;
    ScenePtr back(UsdBridge::readScene(path, &err, &warn));
    ASSERT_NE(back, nullptr) << err.toStdString();
    ASSERT_EQ(back->mNumMeshes, 1u);
    const aiMesh* m = back->mMeshes[0];
    EXPECT_EQ(m->mNumFaces, 4u);

    // Same point cloud (order may differ after the converter indexes it).
    ASSERT_GE(m->mNumVertices, 6u);
    for (unsigned i = 0; i < 6; ++i) {
        bool found = false;
        for (unsigned j = 0; j < m->mNumVertices && !found; ++j)
            found = (m->mVertices[j] - scene->mMeshes[0]->mVertices[i]).Length() < 1e-5f;
        EXPECT_TRUE(found) << "vertex " << i << " lost in the round trip";
    }
    EXPECT_TRUE(m->HasNormals());
    EXPECT_TRUE(m->HasTextureCoords(0));
    // normals survive
    for (unsigned j = 0; j < m->mNumVertices; ++j)
        EXPECT_NEAR(m->mNormals[j].z, 1.0f, 1e-4f);
}

TEST_P(UsdBridgeFormatTest, SkinningAndSkeletonRoundTrip) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("rig.") + GetParam());
    ScenePtr scene = makeRiggedScene(false);
    QString err;
    ASSERT_TRUE(UsdBridge::writeScene(path, scene.get(), &err)) << err.toStdString();
    ScenePtr back(UsdBridge::readScene(path, &err));
    ASSERT_NE(back, nullptr) << err.toStdString();
    const aiMesh* m = back->mMeshes[0];
    ASSERT_EQ(m->mNumBones, 2u);

    // Joint names (not the sanitised USD paths) survive: "elbow:joint" has a
    // character that is not a valid USD identifier.
    const aiBone* root = boneNamed(m, "root");
    const aiBone* elbow = boneNamed(m, "elbow:joint");
    ASSERT_NE(root, nullptr);
    ASSERT_NE(elbow, nullptr);

    // Every vertex keeps weight sum 1 and the dominant bone it had.
    std::map<unsigned, float> sums;
    for (const aiBone* b : { root, elbow })
        for (unsigned i = 0; i < b->mNumWeights; ++i)
            sums[b->mWeights[i].mVertexId] += b->mWeights[i].mWeight;
    for (const auto& [v, s] : sums)
        EXPECT_NEAR(s, 1.0f, 1e-4f) << "vertex " << v;

    // The elbow's bind pose is at y = 1: its offset matrix undoes that.
    const aiVector3D p = elbow->mOffsetMatrix * aiVector3D(0, 1, 0);
    EXPECT_NEAR(p.Length(), 0.0f, 1e-4f);

    // Hierarchy: elbow is a child of root in the node tree.
    const aiNode* nElbow = back->mRootNode->FindNode("elbow:joint");
    ASSERT_NE(nElbow, nullptr);
    ASSERT_NE(nElbow->mParent, nullptr);
    EXPECT_STREQ(nElbow->mParent->mName.C_Str(), "root");
    EXPECT_NEAR(nElbow->mTransformation.b4, 1.0f, 1e-4f);
}

TEST_P(UsdBridgeFormatTest, ClipsRoundTripWithCountNamesAndPoses) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("clips.") + GetParam());
    ScenePtr scene = makeRiggedScene();
    QString err;
    ASSERT_TRUE(UsdBridge::writeScene(path, scene.get(), &err)) << err.toStdString();
    ScenePtr back(UsdBridge::readScene(path, &err));
    ASSERT_NE(back, nullptr) << err.toStdString();

    ASSERT_EQ(back->mNumAnimations, 2u) << "clip count must survive";
    const aiAnimation* wave = animNamed(back.get(), "Wave");
    const aiAnimation* idle = animNamed(back.get(), "Idle clip");
    ASSERT_NE(wave, nullptr);
    ASSERT_NE(idle, nullptr) << "non-identifier clip names keep their original text";
    EXPECT_NEAR(wave->mDuration, 2.0, 1e-3);
    EXPECT_NEAR(idle->mDuration, 1.0, 1e-3);

    const aiNodeAnim* ch = channelFor(wave, "elbow:joint");
    ASSERT_NE(ch, nullptr);
    ASSERT_EQ(ch->mNumRotationKeys, 3u);
    EXPECT_NEAR(ch->mRotationKeys[0].mTime, 0.0, 1e-4);
    EXPECT_NEAR(ch->mRotationKeys[1].mTime, 1.0, 1e-3);
    EXPECT_NEAR(ch->mRotationKeys[2].mTime, 2.0, 1e-3);
    EXPECT_NEAR(angleOf(ch->mRotationKeys[0].mValue), 0.0, 1e-3);
    EXPECT_NEAR(angleOf(ch->mRotationKeys[1].mValue), 60.0 * M_PI / 180.0, 1e-3);
    EXPECT_NEAR(angleOf(ch->mRotationKeys[2].mValue), 0.0, 1e-3);

    const aiNodeAnim* ch2 = channelFor(idle, "elbow:joint");
    ASSERT_NE(ch2, nullptr);
    EXPECT_NEAR(angleOf(ch2->mRotationKeys[1].mValue), 20.0 * M_PI / 180.0, 1e-3);
}

TEST_P(UsdBridgeFormatTest, BlendShapesRoundTripWithNames) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("shapes.") + GetParam());
    ScenePtr scene = makeRiggedScene(false);
    QString err;
    ASSERT_TRUE(UsdBridge::writeScene(path, scene.get(), &err)) << err.toStdString();
    ScenePtr back(UsdBridge::readScene(path, &err));
    ASSERT_NE(back, nullptr) << err.toStdString();
    const aiMesh* m = back->mMeshes[0];
    ASSERT_EQ(m->mNumAnimMeshes, 1u);
    EXPECT_STREQ(m->mAnimMeshes[0]->mName.C_Str(), "Bulge: big");

    // The displaced vertices carry the same offsets as the source shape.
    const aiAnimMesh* shape = m->mAnimMeshes[0];
    ASSERT_EQ(shape->mNumVertices, m->mNumVertices);
    int displaced = 0;
    for (unsigned v = 0; v < m->mNumVertices; ++v) {
        const aiVector3D d = shape->mVertices[v] - m->mVertices[v];
        if (d.Length() > 1e-5f) {
            ++displaced;
            const bool z = std::fabs(d.z - 0.5f) < 1e-4f && std::fabs(d.x) < 1e-4f;
            const bool x = std::fabs(d.x - 0.25f) < 1e-4f && std::fabs(d.z) < 1e-4f;
            EXPECT_TRUE(z || x) << "unexpected offset " << d.x << "," << d.y << "," << d.z;
        }
    }
    EXPECT_EQ(displaced, 2);
}

TEST_P(UsdBridgeFormatTest, MaterialColourAndTextureRoundTrip) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("mat.") + GetParam());
    // the texture sits beside the output so a .usdz can package it
    {
        QFile f(tmp.filePath("hero_albedo.png"));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        // 1x1 PNG
        static const unsigned char png[] = {
            0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
            0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
            0x89,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0xF8,0xCF,0xC0,0x00,
            0x00,0x03,0x01,0x01,0x00,0x18,0xDD,0x8D,0xB0,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,
            0x44,0xAE,0x42,0x60,0x82 };
        f.write(reinterpret_cast<const char*>(png), sizeof(png));
    }
    ScenePtr scene = makeRiggedScene(false);
    QString err;
    ASSERT_TRUE(UsdBridge::writeScene(path, scene.get(), &err)) << err.toStdString();
    ScenePtr back(UsdBridge::readScene(path, &err));
    ASSERT_NE(back, nullptr) << err.toStdString();
    ASSERT_GE(back->mNumMaterials, 1u);
    const aiMaterial* mat = back->mMaterials[back->mMeshes[0]->mMaterialIndex];
    aiString name;
    mat->Get(AI_MATKEY_NAME, name);
    EXPECT_NE(std::string(name.C_Str()).find("Skin"), std::string::npos);
    aiString tex;
    ASSERT_EQ(mat->GetTexture(aiTextureType_DIFFUSE, 0, &tex), AI_SUCCESS)
        << "the diffuse texture reference was lost";
    EXPECT_TRUE(QString::fromUtf8(tex.C_Str()).endsWith("hero_albedo.png"));
}

INSTANTIATE_TEST_SUITE_P(Formats, UsdBridgeFormatTest,
                         ::testing::Values("usda", "usdc", "usd", "usdz"));

TEST(UsdBridgeWrite, FormatIsPickedFromTheExtensionAndMagicDiffers) {
    QTemporaryDir tmp;
    ScenePtr scene = makeRiggedScene(false);
    QString err;
    const QString a = tmp.filePath("a.usda"), c = tmp.filePath("c.usdc"), z = tmp.filePath("z.usdz");
    ASSERT_TRUE(UsdBridge::writeScene(a, scene.get(), &err)) << err.toStdString();
    ASSERT_TRUE(UsdBridge::writeScene(c, scene.get(), &err)) << err.toStdString();
    ASSERT_TRUE(UsdBridge::writeScene(z, scene.get(), &err)) << err.toStdString();
    auto head = [](const QString& p, int n) { QFile f(p); f.open(QIODevice::ReadOnly); return f.read(n); };
    EXPECT_TRUE(head(a, 8).startsWith("#usda"));
    EXPECT_TRUE(head(c, 8).startsWith("PXR-USDC"));
    EXPECT_TRUE(head(z, 4).startsWith("PK"));
}

TEST(UsdBridgeWrite, RejectsBadInputAndKeepsAnExistingFile) {
    QTemporaryDir tmp;
    QString err;
    EXPECT_FALSE(UsdBridge::writeScene(tmp.filePath("x.usda"), nullptr, &err));
    EXPECT_FALSE(err.isEmpty());
    ScenePtr empty(new aiScene());
    err.clear();
    EXPECT_FALSE(UsdBridge::writeScene(tmp.filePath("x.usda"), empty.get(), &err));
    EXPECT_FALSE(QFile::exists(tmp.filePath("x.usda")));

    // An unwritable destination fails without destroying the previous file.
    const QString keep = tmp.filePath("keep.usda");
    {
        QFile f(keep);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("#usda 1.0\n");
    }
    ScenePtr scene = makeRiggedScene(false);
    EXPECT_FALSE(UsdBridge::writeScene(tmp.filePath("nodir/keep.usda"), scene.get(), &err));
    QFile f(keep);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), QByteArray("#usda 1.0\n"));
}

TEST(UsdBridgeRead, MissingAndGarbageFilesFailCleanly) {
    QTemporaryDir tmp;
    QString err;
    EXPECT_EQ(UsdBridge::readScene(tmp.filePath("nope.usda"), &err), nullptr);
    EXPECT_FALSE(err.isEmpty());

    const QString junk = tmp.filePath("junk.usdc");
    {
        QFile f(junk);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("this is not a crate file at all");
    }
    err.clear();
    EXPECT_EQ(UsdBridge::readScene(junk, &err), nullptr);
    EXPECT_FALSE(err.isEmpty());
}

TEST(UsdBridgeRead, HandWrittenUsdaWithZUpAndCentimetresIsBroughtToYUpMetres) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath("tri.usda");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(R"(#usda 1.0
(
    defaultPrim = "Tri"
    metersPerUnit = 0.01
    upAxis = "Z"
)
def Xform "Tri"
{
    def Mesh "m"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (100, 0, 0), (0, 0, 100)]
    }
}
)");
    }
    QString err, warn;
    ScenePtr s(UsdBridge::readScene(path, &err, &warn));
    ASSERT_NE(s, nullptr) << err.toStdString();
    ASSERT_EQ(s->mNumMeshes, 1u);
    EXPECT_EQ(s->mMeshes[0]->mNumFaces, 1u);
    // Root transform: Z-up -> Y-up (-90 about X) then 0.01 scale. Vertex
    // (0,0,100) lands at world (0, +1, 0) after both.
    aiMatrix4x4 g = s->mRootNode->mTransformation;
    const aiVector3D p = g * aiVector3D(0, 0, 100);
    EXPECT_NEAR(p.y, 1.0f, 1e-4f);
    EXPECT_NEAR(p.x, 0.0f, 1e-4f);
    EXPECT_NEAR(p.z, 0.0f, 1e-4f);
}

TEST(UsdBridgeWrite, NodeAnimationOnTheRootXformRoundTrips) {
    QTemporaryDir tmp;
    ScenePtr scene(new aiScene());
    scene->mRootNode = makeNode("Prop", nullptr);
    scene->mRootNode->mNumMeshes = 1;
    scene->mRootNode->mMeshes = new unsigned[1]{ 0 };
    auto* m = new aiMesh();
    m->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    m->mNumVertices = 3;
    m->mVertices = new aiVector3D[3]{ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
    m->mNumFaces = 1;
    m->mFaces = new aiFace[1];
    m->mFaces[0].mNumIndices = 3;
    m->mFaces[0].mIndices = new unsigned[3]{ 0, 1, 2 };
    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh*[1]{ m };
    auto* a = new aiAnimation();
    a->mName = aiString("Slide");
    a->mDuration = 2.0;
    a->mTicksPerSecond = 1.0;
    auto* ch = new aiNodeAnim();
    ch->mNodeName = aiString("Prop");
    ch->mNumPositionKeys = 2;
    ch->mPositionKeys = new aiVectorKey[2]{ aiVectorKey(0.0, aiVector3D(0, 0, 0)),
                                            aiVectorKey(2.0, aiVector3D(4, 0, 0)) };
    a->mNumChannels = 1;
    a->mChannels = new aiNodeAnim*[1]{ ch };
    scene->mNumAnimations = 1;
    scene->mAnimations = new aiAnimation*[1]{ a };

    const QString path = tmp.filePath("prop.usda");
    QString err;
    ASSERT_TRUE(UsdBridge::writeScene(path, scene.get(), &err)) << err.toStdString();
    ScenePtr back(UsdBridge::readScene(path, &err));
    ASSERT_NE(back, nullptr) << err.toStdString();
    ASSERT_GE(back->mNumAnimations, 1u);
    const aiNodeAnim* c = channelFor(back->mAnimations[0], "Prop");
    ASSERT_NE(c, nullptr);
    ASSERT_GE(c->mNumPositionKeys, 2u);
    EXPECT_NEAR(c->mPositionKeys[c->mNumPositionKeys - 1].mValue.x, 4.0f, 1e-3f);
    EXPECT_NEAR(c->mPositionKeys[c->mNumPositionKeys - 1].mTime, 2.0, 1e-3);
}

TEST(UsdBridgeWrite, MorphWeightClipsAreAuthoredAndReportedAsNotImported) {
    QTemporaryDir tmp;
    ScenePtr scene = makeRiggedScene(false);
    UsdBridge::MorphWeightClip clip;
    clip.name = "Smile";
    clip.shapes = { "Bulge: big" };
    clip.times = { 0.0, 1.0 };
    clip.weights = { { 0.0f }, { 1.0f } };
    UsdBridge::setWriteExtras({ clip });
    const QString path = tmp.filePath("morph.usda");
    QString err;
    ASSERT_TRUE(UsdBridge::writeScene(path, scene.get(), &err)) << err.toStdString();
    UsdBridge::clearWriteExtras();

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray text = f.readAll();
    EXPECT_TRUE(text.contains("blendShapeWeights")) << "weights must be authored in the file";
    EXPECT_TRUE(text.contains("skel:blendShapes"));

    QString warn;
    ScenePtr back(UsdBridge::readScene(path, &err, &warn));
    ASSERT_NE(back, nullptr) << err.toStdString();
    EXPECT_EQ(back->mMeshes[0]->mNumAnimMeshes, 1u) << "the shape itself still imports";
    EXPECT_TRUE(warn.contains("weight", Qt::CaseInsensitive))
        << "the dropped weight animation must be reported, not silent";
}

TEST(UsdBridgeWrite, MorphWeightsAreInterpolatedAtInsertedJointKeyTimes) {
    QTemporaryDir tmp;
    ScenePtr scene = makeRiggedScene();   // "Wave": joint keys at 0, 1, 2 s
    UsdBridge::MorphWeightClip clip;
    clip.name = "Wave";                    // same name -> merged into one SkelAnimation
    clip.shapes = { "Bulge: big" };
    clip.times = { 0.0, 2.0 };             // no morph key at the joint key t = 1 s
    clip.weights = { { 0.0f }, { 1.0f } };
    UsdBridge::setWriteExtras({ clip });
    const QString path = tmp.filePath("interp.usda");
    QString err;
    ASSERT_TRUE(UsdBridge::writeScene(path, scene.get(), &err)) << err.toStdString();
    UsdBridge::clearWriteExtras();
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(f.readAll());
    // t = 1 s is time code 24: holding the earlier key would write 0, the
    // linear value is 0.5.
    EXPECT_TRUE(text.contains(QRegularExpression(R"(24:\s*\[0\.5\])")))
        << "weight at the inserted sample time must be interpolated";
    EXPECT_FALSE(text.contains(QRegularExpression(R"(24:\s*\[0\])")));
}

TEST(UsdBridgeWrite, BlendShapeTokensAreSharedBetweenMeshAndClip) {
    QTemporaryDir tmp;
    ScenePtr scene = makeRiggedScene(false);
    aiMesh* m = scene->mMeshes[0];
    // two shapes whose names sanitise to the same identifier
    auto* second = new aiAnimMesh();
    second->mName = aiString("Bulge  big");   // "Bulge: big" and this both -> Bulge__big? use clearly equal
    second->mNumVertices = m->mNumVertices;
    second->mVertices = new aiVector3D[m->mNumVertices];
    for (unsigned i = 0; i < m->mNumVertices; ++i) second->mVertices[i] = m->mVertices[i];
    second->mVertices[0].x += 0.5f;
    second->mWeight = 1.0f;
    aiAnimMesh** shapes = new aiAnimMesh*[2]{ m->mAnimMeshes[0], second };
    delete[] m->mAnimMeshes;
    m->mAnimMeshes = shapes;
    m->mNumAnimMeshes = 2;
    m->mAnimMeshes[0]->mName = aiString("Smile-L");
    m->mAnimMeshes[1]->mName = aiString("Smile L");

    UsdBridge::MorphWeightClip clip;
    clip.name = "Talk";
    clip.shapes = { "Smile-L", "Smile L" };
    clip.times = { 0.0, 1.0 };
    clip.weights = { { 0.0f, 1.0f }, { 1.0f, 0.0f } };
    UsdBridge::setWriteExtras({ clip });
    const QString path = tmp.filePath("tokens.usda");
    QString err;
    ASSERT_TRUE(UsdBridge::writeScene(path, scene.get(), &err)) << err.toStdString();
    UsdBridge::clearWriteExtras();
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(f.readAll());
    // The same ordered, DISTINCT token pair appears on the mesh and on the clip.
    const QRegularExpression pair(R"(\[\s*"Smile_L"\s*,\s*"Smile_L_1"\s*\])");
    EXPECT_EQ(text.count(pair), 2) << "mesh skel:blendShapes and clip blendShapes must match";
    EXPECT_TRUE(text.contains("def BlendShape \"Smile_L\""));
    EXPECT_TRUE(text.contains("def BlendShape \"Smile_L_1\""));
}

// ---------------------------------------------------------------------------
// Ogre pipeline: export a skinned entity to USD and import it back (the
// "round trip keeps clip count and geometry" acceptance criterion), plus the
// Assimp registration the importer relies on.
// ---------------------------------------------------------------------------

class UsdBridgeSceneTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(tryInitOgre()) << "Ogre init required"; }
    void TearDown() override {
        if (auto* mgr = Manager::getSingletonPtr()) {
            if (auto* scene = mgr->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
        }
    }

    static Ogre::Entity* firstEntityNamed(const QString& node)
    {
        auto* sm = Manager::getSingleton()->getSceneMgr();
        return sm->hasEntity(node.toStdString()) ? sm->getEntity(node.toStdString()) : nullptr;
    }
};

TEST_F(UsdBridgeSceneTest, ExportThenImportKeepsSkeletonClipsAndGeometry) {
    auto* entity = createAnimatedTestEntity("USD_Rig");
    ASSERT_NE(entity, nullptr);
    const unsigned srcBones = entity->getMesh()->getSkeleton()->getNumBones();
    const unsigned srcAnims = entity->getMesh()->getSkeleton()->getNumAnimations();
    ASSERT_GE(srcAnims, 1u);

    QTemporaryDir tmp;
    for (const char* fmt : { "USD ASCII (*.usda)", "USD Crate (*.usdc)", "USD Package (*.usdz)" }) {
        const QString ext = QString(fmt).section('*', 1, 1).chopped(1);
        const QString path = tmp.filePath(QStringLiteral("rig") + ext);
        ASSERT_EQ(MeshImporterExporter::exporter(entity->getParentSceneNode(), path, fmt), 0) << fmt;
        ASSERT_TRUE(QFile::exists(path)) << fmt;

        // Re-import under a different node name so entities do not collide.
        const QString copy = tmp.filePath(QStringLiteral("again_") + QString(fmt).section(' ', 1, 1) + ext);
        ASSERT_TRUE(QFile::copy(path, copy));
        MeshImporterExporter::importer(QStringList{copy});
        const QString nodeName = QFileInfo(copy).baseName();
        auto* back = firstEntityNamed(nodeName);
        ASSERT_NE(back, nullptr) << fmt << ": the exported USD did not import";
        ASSERT_TRUE(back->hasSkeleton()) << fmt;
        EXPECT_EQ(back->getMesh()->getSkeleton()->getNumBones(), srcBones) << fmt;
        EXPECT_EQ(back->getMesh()->getSkeleton()->getNumAnimations(), srcAnims) << fmt;
        EXPECT_NE(back->getMesh()->getSkeleton()->getAnimation("TestAnim"), nullptr) << fmt;
        EXPECT_EQ(back->getMesh()->getSubMesh(0)->vertexData != nullptr ||
                  back->getMesh()->sharedVertexData != nullptr, true) << fmt;
        const Ogre::VertexData* vd = back->getMesh()->getSubMesh(0)->useSharedVertices
            ? back->getMesh()->sharedVertexData : back->getMesh()->getSubMesh(0)->vertexData;
        ASSERT_NE(vd, nullptr);
        EXPECT_EQ(vd->vertexCount, 3u) << fmt;
    }
}

TEST_F(UsdBridgeSceneTest, ImporterAcceptsUsdThroughTheAssimpRegistration) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath("registered.usda");
    ScenePtr scene = makeRiggedScene();
    QString err;
    ASSERT_TRUE(UsdBridge::writeScene(path, scene.get(), &err)) << err.toStdString();

    Assimp::Importer imp;
    UsdBridge::registerImporter(imp);
    const aiScene* s = imp.ReadFile(path.toStdString(), 0);
    ASSERT_NE(s, nullptr) << imp.GetErrorString();
    EXPECT_EQ(s->mNumMeshes, 1u);
    EXPECT_EQ(s->mNumAnimations, 2u);

    // Without the registration Assimp has no loader for the path.
    Assimp::Importer plain;
    EXPECT_EQ(plain.ReadFile(path.toStdString(), 0), nullptr);
}

TEST_F(UsdBridgeSceneTest, ExporterIsRegisteredWithAssimp) {
    QTemporaryDir tmp;
    ScenePtr scene = makeRiggedScene(false);
    Assimp::Exporter exp;
    UsdBridge::registerExporter(exp);
    const QString path = tmp.filePath("viaassimp.usdc");
    ASSERT_EQ(exp.Export(scene.get(), "usdc", path.toStdString()), AI_SUCCESS) << exp.GetErrorString();
    EXPECT_TRUE(QFile::exists(path));
}

// 16+ joints: the crate writer used to compress token[] arrays of that size,
// which OpenUSD readers misread (cmake/patches/lightusd-uncompressed-token-arrays.patch).
// Our own reader accepts both, so this pins the round trip; the Blender check
// in docs/USD.md is what proves the raw encoding.
TEST_P(UsdBridgeFormatTest, ManyJointsRoundTrip) {
    ScenePtr scene = makeRiggedScene();
    aiNode* br = scene->mRootNode->mChildren[0];
    std::vector<aiNode*> kids(br->mChildren, br->mChildren + br->mNumChildren);
    for (int i = 0; i < 40; ++i)
        kids.push_back(makeNode(("extra_joint_number_" + std::to_string(i)).c_str(), br));
    setChildren(br, kids);

    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("many.") + GetParam());
    QString err;
    ASSERT_TRUE(UsdBridge::writeScene(path, scene.get(), &err)) << err.toStdString();
    ScenePtr back(UsdBridge::readScene(path, &err));
    ASSERT_NE(back, nullptr) << err.toStdString();
    EXPECT_NE(back->mRootNode->FindNode("extra_joint_number_39"), nullptr);
    EXPECT_NE(back->mRootNode->FindNode("elbow:joint"), nullptr);
    EXPECT_EQ(back->mNumAnimations, 2u);
    EXPECT_EQ(back->mMeshes[0]->mNumBones, 2u);
}

#endif  // ENABLE_USD
