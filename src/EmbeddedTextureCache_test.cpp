#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "EmbeddedTextureCache.h"

TEST(EmbeddedTextureCacheStandaloneTest, StoreAndRetrieveRoundtrip)
{
    EmbeddedTextureCache::clear();
    const std::vector<uint8_t> bytes{0x89, 'P', 'N', 'G', 1, 2, 3};
    EmbeddedTextureCache::store("Boss_normal.png", bytes);
    const auto got = EmbeddedTextureCache::retrieve("Boss_normal.png");
    EXPECT_EQ(got, bytes);
}

TEST(EmbeddedTextureCacheStandaloneTest, MissingKeyReturnsEmpty)
{
    EmbeddedTextureCache::clear();
    EXPECT_TRUE(EmbeddedTextureCache::retrieve("nothing.png").empty());
}

TEST(EmbeddedTextureCacheStandaloneTest, StoreReplacesPriorEntry)
{
    EmbeddedTextureCache::clear();
    EmbeddedTextureCache::store("x.png", {1, 2, 3});
    EmbeddedTextureCache::store("x.png", {4, 5});
    const auto got = EmbeddedTextureCache::retrieve("x.png");
    EXPECT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], 4);
    EXPECT_EQ(got[1], 5);
}

// CodeRabbit nitpick on #514: the original test name implied validation /
// rejection but the actual contract is "silent no-op." Renamed so the
// behavior matches the test name.
TEST(EmbeddedTextureCacheStandaloneTest, EmptyKeyOrEmptyBytesResultInEmptyRetrieval)
{
    EmbeddedTextureCache::clear();
    EmbeddedTextureCache::store("", {1, 2, 3});
    EmbeddedTextureCache::store("y.png", {});
    EXPECT_TRUE(EmbeddedTextureCache::retrieve("").empty());
    EXPECT_TRUE(EmbeddedTextureCache::retrieve("y.png").empty());
}

TEST(EmbeddedTextureCacheStandaloneTest, ClearWipesEverything)
{
    EmbeddedTextureCache::store("a.png", {1});
    EmbeddedTextureCache::store("b.png", {2});
    EmbeddedTextureCache::clear();
    EXPECT_TRUE(EmbeddedTextureCache::retrieve("a.png").empty());
    EXPECT_TRUE(EmbeddedTextureCache::retrieve("b.png").empty());
}

// Coverage for the raw-buffer overload added on PR #514. MaterialProcessor
// uses this path to stash bytes straight from `aiTexel*` without an
// intermediate std::vector copy at the call site.
TEST(EmbeddedTextureCacheStandaloneTest, RawBufferOverloadCopiesBytes)
{
    EmbeddedTextureCache::clear();
    const std::byte raw[] = {
        std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
        std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A},
    };
    EmbeddedTextureCache::store("raw.png", raw, sizeof(raw));
    const auto got = EmbeddedTextureCache::retrieve("raw.png");
    ASSERT_EQ(got.size(), sizeof(raw));
    EXPECT_EQ(got.front(), 0x89);          // first PNG signature byte
    EXPECT_EQ(got.back(),  0x0A);          // last PNG signature byte
}

TEST(EmbeddedTextureCacheStandaloneTest, RawBufferOverloadRejectsNullOrZeroLength)
{
    EmbeddedTextureCache::clear();
    EmbeddedTextureCache::store("null.png", nullptr, 0);
    EmbeddedTextureCache::store("zero.png", reinterpret_cast<const std::byte*>("x"), 0);
    EXPECT_TRUE(EmbeddedTextureCache::retrieve("null.png").empty());
    EXPECT_TRUE(EmbeddedTextureCache::retrieve("zero.png").empty());
}

// Smoke test for the cache's internal mutex. CodeRabbit asked for a
// concurrency check on PR #514 — the cache claims thread safety, this
// asserts the simplest version: N threads each store a unique key, and
// every key is independently retrievable afterwards.
TEST(EmbeddedTextureCacheStandaloneTest, ConcurrentStoresAllRetrievable)
{
    EmbeddedTextureCache::clear();
    constexpr int kThreads = 8;
    constexpr int kPerThread = 32;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([t]() {
            for (int i = 0; i < kPerThread; ++i) {
                const std::string key =
                    "t" + std::to_string(t) + "_" + std::to_string(i) + ".png";
                const std::vector<uint8_t> bytes{
                    static_cast<uint8_t>(t), static_cast<uint8_t>(i), 0xAA, 0xBB};
                EmbeddedTextureCache::store(key, bytes);
            }
        });
    }
    for (auto& th : ts) th.join();

    int found = 0;
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            const std::string key =
                "t" + std::to_string(t) + "_" + std::to_string(i) + ".png";
            const auto got = EmbeddedTextureCache::retrieve(key);
            if (got.size() == 4 &&
                got[0] == static_cast<uint8_t>(t) &&
                got[1] == static_cast<uint8_t>(i))
                ++found;
        }
    }
    EXPECT_EQ(found, kThreads * kPerThread);
}
