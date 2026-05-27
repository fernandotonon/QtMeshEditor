#include "PS1CapturedAssets.h"
#include "PS1ExtractedAssetBrowser.h"

#include <gtest/gtest.h>

#include <QSignalSpy>

namespace {

// Helper that fabricates a one-prim + one-mesh capture so the tests
// don't depend on the heavy reconstruction path. Mirrors how
// `MeshReconstructor::reconstructDeduped` shapes the data.
CapturedAssetSet makeFixture()
{
    CaptureSnapshot snapshot;
    PrimRecord prim;
    prim.kind = PrimKind::TexturedTri;
    prim.vertexCount = 3;
    prim.tpage = 0x1234;
    prim.clut = 0x5678;
    prim.matrixId = 7;
    prim.semiTrans = 0;
    snapshot.prims.push_back(prim);

    PrimRecord prim2;
    prim2.kind = PrimKind::MonoTri;
    prim2.vertexCount = 3;
    prim2.matrixId = 8;
    snapshot.prims.push_back(prim2);

    ReconstructedCaptureSet rec;
    ReconstructedMesh mesh;
    mesh.meshName = QStringLiteral("ps1_unique_0");
    mesh.vertexCount = 3;
    mesh.triangleCount = 1;
    ReconstructedSubMesh sub;
    sub.materialName = QStringLiteral("PS1Rip_tpage_1234_clut_5678_st0_dm0");
    mesh.subMeshes.append(sub);
    rec.uniqueMeshes.append(mesh);

    ReconstructedMesh mesh2;
    mesh2.meshName = QStringLiteral("ps1_unique_1");
    mesh2.vertexCount = 3;
    mesh2.triangleCount = 1;
    ReconstructedSubMesh sub2;
    sub2.materialName = QStringLiteral("PS1Rip_color");
    mesh2.subMeshes.append(sub2);
    rec.uniqueMeshes.append(mesh2);

    rec.instances.append({0, 0.0f, 0.0f, 0.0f});
    rec.instances.append({1, 1.0f, 0.0f, 0.0f});

    QHash<QString, QImage> texImages;
    QImage tile(8, 8, QImage::Format_ARGB32);
    tile.fill(Qt::cyan);
    texImages.insert(QStringLiteral("PS1Rip_tpage_1234_clut_5678_st0_dm0"), tile);

    return PS1CapturedAssets::buildFromCapture(QStringLiteral("test123"), snapshot, rec, texImages);
}

} // namespace

TEST(PS1CapturedAssetsTest, BuildFromCapturePopulatesRowsAndCounts)
{
    const CapturedAssetSet set = makeFixture();
    EXPECT_EQ(set.captureId, QStringLiteral("test123"));
    ASSERT_EQ(set.rows.size(), 2);
    EXPECT_EQ(set.totalPrims, 2);
    EXPECT_EQ(set.totalTris, 2);
    EXPECT_EQ(set.uniqueMeshes.size(), 2);
    EXPECT_EQ(set.instances.size(), 2);

    const CapturedAssetRow &row0 = set.rows.at(0);
    EXPECT_EQ(row0.rowIndex, 1);
    EXPECT_TRUE(row0.textured);
    EXPECT_FALSE(row0.colored);
    EXPECT_EQ(row0.tpage, 0x1234);
    EXPECT_EQ(row0.uniqueMeshIndex, 0);
    EXPECT_EQ(row0.instanceIndex, 0);

    const CapturedAssetRow &row1 = set.rows.at(1);
    EXPECT_FALSE(row1.textured);
    EXPECT_TRUE(row1.colored);
    EXPECT_EQ(row1.uniqueMeshIndex, 1);
    EXPECT_EQ(row1.instanceIndex, 1);

    // One unique textured material identity = one unique texture id.
    EXPECT_EQ(set.uniqueTextureIds.size(), 1);
}

TEST(PS1CapturedAssetsTest, InstanceNodeNamesMatchMeshBuilderNaming)
{
    const CapturedAssetSet set = makeFixture();
    ASSERT_EQ(set.instanceNodeNames.size(), 2);
    // Naming scheme is `PS1Capture_<captureId>_inst<ordinal>` where the
    // ordinal walks per unique mesh in order. Matches PS1RipMeshBuilder's
    // attachCaptureSetToScene loop.
    EXPECT_EQ(set.instanceNodeNames.value(0), QStringLiteral("PS1Capture_test123_inst0"));
    EXPECT_EQ(set.instanceNodeNames.value(1), QStringLiteral("PS1Capture_test123_inst1"));
}

TEST(PS1CapturedAssetsTest, SetCaptureSetEmitsSignalAndUpdatesAccessors)
{
    PS1CapturedAssets *store = PS1CapturedAssets::getSingleton();
    ASSERT_NE(store, nullptr);
    store->clear();

    QSignalSpy spy(store, &PS1CapturedAssets::captureSetChanged);
    store->setCaptureSet(makeFixture());

    EXPECT_EQ(spy.count(), 1);
    EXPECT_TRUE(store->hasCapture());
    EXPECT_EQ(store->captureSet().rows.size(), 2);
}

TEST(PS1CapturedAssetsTest, RowMutationsFireRowChangedAndNoOpWhenIdle)
{
    PS1CapturedAssets *store = PS1CapturedAssets::getSingleton();
    ASSERT_NE(store, nullptr);
    store->clear();
    store->setCaptureSet(makeFixture());

    QSignalSpy rowSpy(store, &PS1CapturedAssets::rowChanged);
    EXPECT_TRUE(store->setRowHidden(1, true));
    EXPECT_TRUE(store->setRowDiscarded(2, true));
    EXPECT_FALSE(store->setRowHidden(1, true));  // already hidden
    EXPECT_FALSE(store->setRowHidden(99, true)); // out of range
    EXPECT_EQ(rowSpy.count(), 2);

    EXPECT_TRUE(store->captureSet().rows.at(0).hidden);
    EXPECT_TRUE(store->captureSet().rows.at(1).discarded);
}

TEST(PS1ExtractedAssetBrowserTest, BuildTilesForKindMesh)
{
    const CapturedAssetSet set = makeFixture();
    const auto tiles =
        PS1ExtractedAssetBrowser::buildTilesForKind(set, ExtractedAssetKind::Mesh);
    ASSERT_EQ(tiles.size(), 2);
    EXPECT_TRUE(tiles.at(0).textured);
    EXPECT_FALSE(tiles.at(0).coloredOnly);
    EXPECT_EQ(tiles.at(0).meshIndex, 0);
    EXPECT_EQ(tiles.at(0).instanceCount, 1);
    EXPECT_FALSE(tiles.at(0).thumbnail.isNull());
    EXPECT_TRUE(tiles.at(1).coloredOnly);
    EXPECT_FALSE(tiles.at(1).textured);
}

TEST(PS1ExtractedAssetBrowserTest, BuildTilesForKindTextureUsesDecodedImages)
{
    const CapturedAssetSet set = makeFixture();
    const auto tiles =
        PS1ExtractedAssetBrowser::buildTilesForKind(set, ExtractedAssetKind::Texture);
    ASSERT_EQ(tiles.size(), 1);
    EXPECT_EQ(tiles.at(0).assetId, QStringLiteral("PS1Rip_tpage_1234_clut_5678_st0_dm0"));
    EXPECT_FALSE(tiles.at(0).thumbnail.isNull());
    EXPECT_TRUE(tiles.at(0).textured);
}

TEST(PS1ExtractedAssetBrowserTest, BuildTilesForKindMaterialIncludesColorMaterial)
{
    const CapturedAssetSet set = makeFixture();
    const auto tiles =
        PS1ExtractedAssetBrowser::buildTilesForKind(set, ExtractedAssetKind::Material);
    ASSERT_EQ(tiles.size(), 2);
    EXPECT_TRUE(tiles.at(0).textured);
    EXPECT_EQ(tiles.at(0).assetId, QStringLiteral("PS1Rip_tpage_1234_clut_5678_st0_dm0"));
    EXPECT_TRUE(tiles.at(1).coloredOnly);
    EXPECT_EQ(tiles.at(1).assetId, QStringLiteral("PS1Rip_color"));
}

