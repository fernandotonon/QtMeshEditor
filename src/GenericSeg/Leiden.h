#pragma once
// Leiden community detection (Traag, Waltman & van Eck 2019) for generic
// segmentation (#1179): groups the 2D masks that cover the same faces across
// views into part instances. Our own implementation because igraph (what
// SAMesh uses) is GPL. Modularity with a resolution parameter; deterministic
// for a given seed. Pure data.

#include <cstdint>
#include <vector>

namespace GenericSeg {

struct GraphEdge {
    int a = 0, b = 0;
    float weight = 1.0f;   // > 0; self-loops and duplicates are allowed
};

enum class LeidenQuality {
    // Classic modularity. Has a resolution limit: in a graph with many
    // communities it tends to cut sparse ones in half.
    Modularity,
    // Constant Potts Model: a community is kept when its internal edge
    // density beats `resolution` (in [0, 1] for unit weights), independent of
    // the size of the whole graph. No resolution limit.
    Cpm,
};

struct LeidenOptions {
    LeidenQuality quality = LeidenQuality::Modularity;
    // Higher = more, smaller communities. 0 merges every connected component
    // into one community (both qualities).
    double resolution = 1.0;
    uint32_t seed = 42;
    int maxLevels = 32;
};

// Community per node, numbered 0..C-1 in order of first appearance.
// Isolated nodes get their own community.
std::vector<int> leiden(int nodeCount, const std::vector<GraphEdge>& edges,
                        const LeidenOptions& opts = {});

// Modularity of a partition (for tests and diagnostics).
double modularity(int nodeCount, const std::vector<GraphEdge>& edges,
                  const std::vector<int>& community, double resolution = 1.0);

} // namespace GenericSeg
