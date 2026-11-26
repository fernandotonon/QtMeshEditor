#pragma once
// Multi-label graph cut for generic segmentation (#1179): alpha-expansion
// (Boykov, Veksler & Zabih 2001) over a Potts pairwise term, each expansion
// solved exactly by a max-flow (Dinic). Pure data.
//
// Used to turn the per-face part votes into a clean labelling: a face pays
// its unary cost for the label it gets, and every edge whose two ends get
// different labels pays its weight, so boundaries settle where they are
// cheap (creases) instead of following the vote noise.

#include "Leiden.h"   // GraphEdge

#include <vector>

namespace GenericSeg {

// Energy of a labelling: sum of unary[node * labelCount + label] plus the
// weight of every edge whose ends differ.
double labelingEnergy(int labelCount, const std::vector<float>& unary,
                      const std::vector<GraphEdge>& edges, const std::vector<int>& labels);

// Minimises the energy above, starting from `initial` (labels in
// [0, labelCount)). Each sweep tries every label once; stops when a sweep
// changes nothing or after `maxSweeps`. Never returns a labelling with a
// higher energy than the start. Edge weights must be >= 0.
//
// `candidates` (optional, one list per label) restricts each label's move to
// the listed nodes plus everything within `grow` edge hops of them or of the
// label's current region; other nodes keep their label for that move. With
// many labels on a large graph this is what keeps it fast: a part's move is
// solved around that part, not over the whole mesh.
std::vector<int> alphaExpansion(int nodeCount, int labelCount, const std::vector<float>& unary,
                                const std::vector<GraphEdge>& edges, std::vector<int> initial,
                                int maxSweeps = 4,
                                const std::vector<std::vector<int>>* candidates = nullptr,
                                int grow = 2);

} // namespace GenericSeg
