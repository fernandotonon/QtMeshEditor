// Alpha-expansion graph cut (#1179): pure data.

#include "GenericSeg/GraphCut.h"

#include <gtest/gtest.h>

#include <random>

using namespace GenericSeg;

namespace {

// Chain of n nodes, each linked to the next with weight w.
std::vector<GraphEdge> chain(int n, float w)
{
    std::vector<GraphEdge> e;
    for (int i = 0; i + 1 < n; ++i) e.push_back({i, i + 1, w});
    return e;
}

} // namespace

TEST(GenericSegGraphCut, SmoothsAnIsolatedNoisyNode)
{
    // 9 nodes prefer label 0, the middle one prefers 1 a little.
    const int n = 9, K = 2;
    std::vector<float> unary(n * K, 0.0f);
    for (int i = 0; i < n; ++i) unary[i * K + 1] = 1.0f;
    unary[4 * K + 0] = 0.5f;
    unary[4 * K + 1] = 0.0f;
    std::vector<int> init(n, 0);
    init[4] = 1;
    // Cutting twice costs 2 * 0.4 > 0.5: the middle node joins its neighbours.
    EXPECT_EQ(alphaExpansion(n, K, unary, chain(n, 0.4f), init), std::vector<int>(n, 0));
    // Cheap edges: the evidence wins.
    EXPECT_EQ(alphaExpansion(n, K, unary, chain(n, 0.1f), init), init);
}

TEST(GenericSegGraphCut, FindsTheOptimumOfSmallProblems)
{
    // Random 3-label problems on 7 nodes: compare with brute force. Alpha
    // expansion is not exact in general, but it must be within its factor-2
    // bound for Potts and never worse than the start.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const int n = 7, K = 3;
    for (int trial = 0; trial < 40; ++trial) {
        std::vector<float> unary(n * K);
        for (float& x : unary) x = u(rng);
        std::vector<GraphEdge> edges;
        for (int a = 0; a < n; ++a)
            for (int b = a + 1; b < n; ++b)
                if (u(rng) < 0.4f) edges.push_back({a, b, 0.5f * u(rng)});
        std::vector<int> init(n);
        for (int& l : init) l = int(rng() % K);
        const std::vector<int> out = alphaExpansion(n, K, unary, edges, init);
        double best = 1e30;
        std::vector<int> L(n, 0);
        for (int code = 0; code < 2187; ++code) {   // 3^7
            int c = code;
            for (int i = 0; i < n; ++i) { L[i] = c % K; c /= K; }
            best = std::min(best, labelingEnergy(K, unary, edges, L));
        }
        const double e = labelingEnergy(K, unary, edges, out);
        EXPECT_LE(e, labelingEnergy(K, unary, edges, init) + 1e-9);
        EXPECT_LE(e, 2.0 * best + 1e-6) << "trial " << trial;
        EXPECT_GE(e, best - 1e-6);
    }
}

TEST(GenericSegGraphCut, StraightensABoundaryOnAGrid)
{
    // 10x10 grid, left half prefers 0 and right half 1, with a jagged
    // vote along the middle. The result is a straight vertical cut.
    const int W = 10, n = W * W, K = 2;
    std::vector<float> unary(n * K);
    std::vector<int> init(n);
    for (int y = 0; y < W; ++y)
        for (int x = 0; x < W; ++x) {
            const int i = y * W + x;
            const int side = x < W / 2 ? 0 : 1;
            const bool jag = (x == W / 2 - 1 || x == W / 2) && (y % 2 == 0);
            const int voted = jag ? 1 - side : side;
            unary[i * K + voted] = 0.0f;
            unary[i * K + 1 - voted] = jag ? 0.3f : 2.0f;
            init[i] = voted;
        }
    std::vector<GraphEdge> edges;
    for (int y = 0; y < W; ++y)
        for (int x = 0; x < W; ++x) {
            if (x + 1 < W) edges.push_back({y * W + x, y * W + x + 1, 0.5f});
            if (y + 1 < W) edges.push_back({y * W + x, (y + 1) * W + x, 0.5f});
        }
    const std::vector<int> out = alphaExpansion(n, K, unary, edges, init);
    for (int y = 0; y < W; ++y)
        for (int x = 0; x < W; ++x) EXPECT_EQ(out[y * W + x], x < W / 2 ? 0 : 1) << x << "," << y;

    // Local moves (each label only around the nodes that voted for it) reach
    // the same straight cut.
    std::vector<std::vector<int>> candidates(K);
    for (int i = 0; i < n; ++i) candidates[init[i]].push_back(i);
    EXPECT_EQ(alphaExpansion(n, K, unary, edges, init, 4, &candidates, 2), out);
    // With no hops a label can only take its own candidates: nothing to gain.
    EXPECT_EQ(alphaExpansion(n, K, unary, edges, init, 4, &candidates, 0), init);
}

TEST(GenericSegGraphCut, BadInputIsReturnedUnchanged)
{
    const std::vector<int> init{0, 1};
    EXPECT_EQ(alphaExpansion(2, 2, {0.0f}, {}, init), init);           // wrong unary size
    EXPECT_EQ(alphaExpansion(2, 1, {0.0f, 0.0f}, {}, {0, 0}), (std::vector<int>{0, 0}));
}
