#include "WeightPaintOps.h"

#include "SkinWeightsPost.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace WeightPaintOps {

namespace {

bool boneLocked(int boneHandle, const std::vector<std::uint8_t>& lockedBones)
{
    if (boneHandle < 0) return false;
    const size_t i = static_cast<size_t>(boneHandle);
    return i < lockedBones.size() && lockedBones[i] != 0;
}

/// Renormalise a row while holding every LOCKED bone at its current value: the
/// unlocked bones are scaled to fill whatever headroom is left. This is what
/// makes "lock bone" hold under any brush mode — a plain renormalise would
/// scale the locked bone too and quietly change it.
void normalizeRowRespectingLocks(SkinWeights::VertexWeights& vw,
                                 const std::vector<std::uint8_t>& lockedBones)
{
    if (vw.count <= 0) return;
    if (lockedBones.empty()) { normalizeRow(vw); return; }

    double lockedSum = 0.0;
    double freeSum = 0.0;
    for (int k = 0; k < vw.count; ++k) {
        if (boneLocked(vw.boneIndices[k], lockedBones)) lockedSum += vw.weights[k];
        else                                            freeSum   += vw.weights[k];
    }
    // Locked weights already fill (or overfill) the row: zero the rest rather
    // than scaling a locked value down to make room.
    const double headroom = 1.0 - lockedSum;
    if (headroom <= 1e-12) {
        for (int k = 0; k < vw.count; ++k)
            if (!boneLocked(vw.boneIndices[k], lockedBones)) vw.weights[k] = 0.0;
        return;
    }
    if (freeSum <= 1e-12) return;   // nothing unlocked to redistribute into
    const double scale = headroom / freeSum;
    for (int k = 0; k < vw.count; ++k)
        if (!boneLocked(vw.boneIndices[k], lockedBones)) vw.weights[k] *= scale;
}

/// Write `target` onto `boneHandle` and scale the OTHER (unlocked) bones into
/// the remaining headroom, so the row sums to 1 with the painted value INTACT.
///
/// This is the crux of weight painting. Writing the target and then calling a
/// plain renormalise does not work: painting bone B to 1.0 on a vertex already
/// weighted 1.0 to bone A gives {A:1, B:1}, which renormalises to {A:.5, B:.5}
/// — the normalise silently halves every stroke, so the brush appears to cap at
/// 50% and a Blur dab cannot move a saturated spike at all. Both were caught by
/// tests before this helper existed.
/// True when no influence carries meaningful weight. Such a row cannot be
/// normalised into anything meaningful, and handing its largest slot 1.0 (which
/// is what pruneAndRenormalize does) would invent an influence the user never
/// painted.
bool rowIsDegenerate(const SkinWeights::VertexWeights& vw)
{
    double sum = 0.0;
    for (int k = 0; k < vw.count; ++k) sum += vw.weights[k];
    return sum <= 1e-9;
}

} // namespace

void writeWeightHoldingTarget(SkinWeights::VertexWeights& vw,
                              int boneHandle,
                              double target,
                              const std::vector<std::uint8_t>& lockedBones,
                              int fallbackBoneHandle)
{
    const double t = std::clamp(target, 0.0, 1.0);

    // Locked bones are immovable, so the target can only claim what is left
    // after them. Clamp rather than stealing from a locked bone.
    double lockedSum = 0.0;
    for (int k = 0; k < vw.count; ++k) {
        if (vw.boneIndices[k] == boneHandle) continue;
        if (boneLocked(vw.boneIndices[k], lockedBones)) lockedSum += vw.weights[k];
    }
    const double allowed = std::clamp(t, 0.0, std::max(0.0, 1.0 - lockedSum));

    if (!setWeight(vw, boneHandle, allowed)) return;

    // Scale the remaining unlocked bones to fill exactly the leftover headroom.
    double othersSum = 0.0;
    for (int k = 0; k < vw.count; ++k) {
        if (vw.boneIndices[k] == boneHandle) continue;
        if (boneLocked(vw.boneIndices[k], lockedBones)) continue;
        othersSum += vw.weights[k];
    }
    const double headroom = std::max(0.0, 1.0 - allowed - lockedSum);
    if (othersSum <= 1e-12) {
        // Every OTHER unlocked influence is zero, so there is nothing to scale
        // into the headroom. Leaving the row summing to <1 is not an option:
        // the caller's pruneAndRenormalize would drop those zero influences and
        // renormalise the painted bone straight back to 1.0 — so painting a
        // weight DOWN would spring it back to FULL. (Observed: a Blur that
        // correctly computed 0.3 came out as 1.0.)
        //
        // Instead give the headroom to one of the zero-weight unlocked
        // siblings, which keeps the row summing to 1 with the painted value
        // intact. Preferring the sibling with the LOWEST bone handle keeps this
        // deterministic.
        if (headroom > 1e-12) {
            int sibling = -1;
            for (int k = 0; k < vw.count; ++k) {
                if (vw.boneIndices[k] == boneHandle) continue;
                if (boneLocked(vw.boneIndices[k], lockedBones)) continue;
                if (sibling < 0 || vw.boneIndices[k] < vw.boneIndices[sibling]) sibling = k;
            }
            if (sibling >= 0) {
                vw.weights[sibling] = headroom;
                return;
            }
            // No existing sibling. Introduce the caller-supplied fallback bone
            // (normally the painted bone's PARENT) to hold the freed weight.
            //
            // Handing the headroom back to the painted bone instead — which is
            // what this did originally — pinned any vertex that reached 1.0 at
            // 1.0 forever, because a sole influence must own the whole row to
            // keep it summing to 1. Adding a recipient is the only way to let
            // the value come down.
            const bool absorbed = fallbackBoneHandle >= 0
                               && fallbackBoneHandle != boneHandle
                               && !boneLocked(fallbackBoneHandle, lockedBones)
                               && setWeight(vw, fallbackBoneHandle, headroom);
            // Genuinely nowhere to put it: keep the row normalised.
            if (!absorbed)
                setWeight(vw, boneHandle, allowed + headroom);
        }
        return;
    }
    const double scale = headroom / othersSum;
    for (int k = 0; k < vw.count; ++k) {
        if (vw.boneIndices[k] == boneHandle) continue;
        if (boneLocked(vw.boneIndices[k], lockedBones)) continue;
        vw.weights[k] *= scale;
    }
}

double falloffWeight(double distance, double radius, double falloff, BrushShape shape)
{
    if (radius <= 0.0) return 0.0;
    if (distance > radius) return 0.0;
    // Square mode is flat (constant strength) and Round uses
    // (1 - d/r)^(1 + falloff*5) — identical to
    // EditModeController::applyVertexColorBrush, so the weight brush and the
    // vertex-colour brush feel the same under the same settings.
    if (shape == BrushShape::Square) return 1.0;
    const double t = 1.0 - (distance / radius);
    const double exponent = 1.0 + falloff * 5.0;
    return std::pow(std::max(0.0, t), exponent);
}

double weightOf(const SkinWeights::VertexWeights& vw, int boneHandle)
{
    for (int k = 0; k < vw.count; ++k)
        if (vw.boneIndices[k] == boneHandle) return vw.weights[k];
    return 0.0;
}

bool setWeight(SkinWeights::VertexWeights& vw, int boneHandle, double weight)
{
    if (boneHandle < 0) return false;
    const double w = std::clamp(weight, 0.0, 1.0);
    for (int k = 0; k < vw.count; ++k) {
        if (vw.boneIndices[k] == boneHandle) { vw.weights[k] = w; return true; }
    }
    if (vw.count >= 8) return false;   // full row; caller may prune and retry
    vw.boneIndices[vw.count] = boneHandle;
    vw.weights[vw.count] = w;
    ++vw.count;
    return true;
}

void normalizeRow(SkinWeights::VertexWeights& vw)
{
    double sum = 0.0;
    for (int k = 0; k < vw.count; ++k) sum += vw.weights[k];
    // A row that sums to ~0 carries no information; inventing a distribution
    // would be worse than leaving it for the caller to notice.
    if (sum <= 1e-12) return;
    const double inv = 1.0 / sum;
    for (int k = 0; k < vw.count; ++k) vw.weights[k] *= inv;
}

int applyDab(const float* positions, int vertexCount,
             std::vector<SkinWeights::VertexWeights>& weights,
             const double center[3],
             int activeBoneHandle,
             const DabOptions& options,
             const std::vector<std::uint8_t>& lockedBones,
             const std::vector<std::vector<int>>& adjacency)
{
    if (!positions || !center || vertexCount <= 0 || activeBoneHandle < 0) return 0;
    if (weights.size() != static_cast<size_t>(vertexCount)) return 0;
    if (options.radius <= 0.0) return 0;
    // Painting a locked bone must do nothing at all — silently painting and
    // then having the renormalise undo it would look like a broken brush.
    if (boneLocked(activeBoneHandle, lockedBones)) return 0;
    // Blur needs the 1-ring; without it, fall through to a no-op rather than
    // behaving like Add (which would be a surprising substitution).
    if (options.mode == BrushMode::Blur
        && adjacency.size() != static_cast<size_t>(vertexCount)) return 0;

    int modified = 0;
    for (int v = 0; v < vertexCount; ++v) {
        const double dx = positions[v * 3 + 0] - center[0];
        const double dy = positions[v * 3 + 1] - center[1];
        const double dz = positions[v * 3 + 2] - center[2];

        double dist;
        if (options.shape == BrushShape::Square) {
            // Axis-aligned cube, matching the vertex-colour brush's square mode.
            if (std::abs(dx) > options.radius || std::abs(dy) > options.radius
                || std::abs(dz) > options.radius) continue;
            dist = 0.0;
        } else {
            dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > options.radius) continue;
        }

        const double fall = falloffWeight(dist, options.radius, options.falloff,
                                          options.shape);
        const double amount = options.strength * fall;
        if (amount <= 0.0) continue;

        SkinWeights::VertexWeights& vw = weights[static_cast<size_t>(v)];
        const double current = weightOf(vw, activeBoneHandle);
        double target = current;
        switch (options.mode) {
        case BrushMode::Add:
            target = current + amount * (1.0 - current);   // eases toward 1
            break;
        case BrushMode::Subtract:
            target = current - amount * current;           // eases toward 0
            break;
        case BrushMode::Blur: {
            const auto& ring = adjacency[static_cast<size_t>(v)];
            if (ring.empty()) continue;
            double mean = 0.0;
            for (const int n : ring) {
                if (n < 0 || static_cast<size_t>(n) >= weights.size()) continue;
                mean += weightOf(weights[static_cast<size_t>(n)], activeBoneHandle);
            }
            mean /= static_cast<double>(ring.size());
            target = current + amount * (mean - current);
            break;
        }
        }

        if (std::abs(target - current) < 1e-9) continue;
        // Ensure the row has a slot for the bone before writing the target.
        if (weightOf(vw, activeBoneHandle) == 0.0 && vw.count >= 8
            && !setWeight(vw, activeBoneHandle, target)) {
            // Row full (8 influences) and the active bone absent. Drop the
            // SMALLEST unlocked influence to make room, so a saturated vertex
            // does not silently refuse paint. Locked bones are never evicted.
            int worst = -1;
            double worstW = 0.0;
            for (int k = 0; k < vw.count; ++k) {
                if (boneLocked(vw.boneIndices[k], lockedBones)) continue;
                if (worst < 0 || vw.weights[k] < worstW) { worst = k; worstW = vw.weights[k]; }
            }
            if (worst < 0) continue;               // every influence is locked
            vw.boneIndices[worst] = vw.boneIndices[vw.count - 1];
            vw.weights[worst]     = vw.weights[vw.count - 1];
            --vw.count;
        }
        const SkinWeights::VertexWeights beforeWrite = vw;
        writeWeightHoldingTarget(vw, activeBoneHandle, target, lockedBones,
                                 options.fallbackBoneHandle);
        if (rowIsDegenerate(vw)) {
            // Every influence reached zero. Renormalising (or pruning) such a
            // row hands its largest slot 1.0 — so painting a weight DOWN to
            // zero would spring it back to FULL, the exact opposite of the
            // request. Keep the pre-dab row: a vertex must retain some
            // influence or it detaches from the skeleton entirely.
            vw = beforeWrite;
            continue;
        }
        ++modified;
    }

    if (modified > 0 && options.maxInfluences > 0)
        SkinWeightsPost::pruneAndRenormalize(weights, options.maxInfluences);
    return modified;
}

int fillConnected(std::vector<SkinWeights::VertexWeights>& weights,
                  const std::vector<std::vector<int>>& adjacency,
                  int seedVertex,
                  int activeBoneHandle,
                  double strength,
                  double falloff,
                  int maxHops,
                  const std::vector<std::uint8_t>& lockedBones)
{
    if (activeBoneHandle < 0 || seedVertex < 0) return 0;
    if (adjacency.size() != weights.size()) return 0;
    if (static_cast<size_t>(seedVertex) >= weights.size()) return 0;
    if (boneLocked(activeBoneHandle, lockedBones)) return 0;

    // BFS by hop count: geodesic spread follows the surface, so the fill cannot
    // leak onto a nearby-but-disconnected piece the way a radius test would.
    std::vector<int> hop(weights.size(), -1);
    std::queue<int> q;
    hop[static_cast<size_t>(seedVertex)] = 0;
    q.push(seedVertex);
    int reached = 0;
    while (!q.empty()) {
        const int v = q.front();
        q.pop();
        const int h = hop[static_cast<size_t>(v)];
        if (maxHops > 0 && h > maxHops) continue;

        // Falloff by hop fraction when bounded; unbounded fills flat, since
        // there is no distance to normalise against.
        const double t = (maxHops > 0) ? (1.0 - static_cast<double>(h) / maxHops) : 1.0;
        const double amount = strength * std::pow(std::max(0.0, t), 1.0 + falloff * 5.0);
        if (amount > 0.0) {
            SkinWeights::VertexWeights& vw = weights[static_cast<size_t>(v)];
            const double current = weightOf(vw, activeBoneHandle);
            const double target = current + amount * (1.0 - current);
            if (std::abs(target - current) > 1e-9) {
                // fillConnected only ever raises weight toward the active bone,
                // so it never needs a recipient for freed weight.
                writeWeightHoldingTarget(vw, activeBoneHandle, target, lockedBones,
                                         -1);
                ++reached;
            }
        }
        for (const int n : adjacency[static_cast<size_t>(v)]) {
            if (n < 0 || static_cast<size_t>(n) >= weights.size()) continue;
            if (hop[static_cast<size_t>(n)] >= 0) continue;
            hop[static_cast<size_t>(n)] = h + 1;
            q.push(n);
        }
    }
    return reached;
}

void normalize(std::vector<SkinWeights::VertexWeights>& weights)
{
    // Threshold 0 + the current max influence count: renormalise only, never
    // drop an influence (that is what limitInfluences is for).
    int maxCount = 1;
    for (const auto& vw : weights) maxCount = std::max(maxCount, vw.count);
    SkinWeightsPost::pruneAndRenormalize(weights, maxCount, /*threshold=*/0.0);
}

void smooth(std::vector<SkinWeights::VertexWeights>& weights,
            const std::vector<std::vector<int>>& adjacency,
            int iterations,
            const std::vector<std::uint8_t>& lockedVertices)
{
    SkinWeightsPost::laplacianSmooth(weights, adjacency, iterations, lockedVertices);
}

void limitInfluences(std::vector<SkinWeights::VertexWeights>& weights,
                     int maxInfluences)
{
    SkinWeightsPost::pruneAndRenormalize(weights, maxInfluences);
}

int mirrorByPosition(const float* positions, int vertexCount,
                     std::vector<SkinWeights::VertexWeights>& weights,
                     int axis,
                     double pivot,
                     double tolerance,
                     const std::vector<std::uint8_t>& lockedBones)
{
    if (!positions || vertexCount <= 0 || axis < 0 || axis > 2) return 0;
    if (weights.size() != static_cast<size_t>(vertexCount)) return 0;
    if (tolerance <= 0.0) return 0;

    // Read from a snapshot so a vertex mirrored early cannot become the source
    // for its own partner — otherwise the second half of the mesh would copy
    // already-mirrored values back.
    const std::vector<SkinWeights::VertexWeights> source = weights;
    const double tol2 = tolerance * tolerance;

    int modified = 0;
    for (int v = 0; v < vertexCount; ++v) {
        double target[3] = {positions[v * 3 + 0], positions[v * 3 + 1],
                            positions[v * 3 + 2]};
        target[axis] = 2.0 * pivot - target[axis];      // reflect

        // Nearest vertex to the reflected position. Brute force, matching the
        // vertex-colour brush's own O(n) scan; a spatial index is a later
        // optimisation, not a correctness concern.
        int best = -1;
        double bestD2 = tol2;
        for (int w = 0; w < vertexCount; ++w) {
            const double dx = positions[w * 3 + 0] - target[0];
            const double dy = positions[w * 3 + 1] - target[1];
            const double dz = positions[w * 3 + 2] - target[2];
            const double d2 = dx * dx + dy * dy + dz * dz;
            if (d2 <= bestD2) { bestD2 = d2; best = w; }
        }
        // No partner within tolerance: leave the vertex alone rather than
        // copying a wrong partner's weights onto it.
        if (best < 0) continue;

        SkinWeights::VertexWeights next = source[static_cast<size_t>(best)];
        if (!lockedBones.empty()) {
            // Preserve this vertex's locked-bone values, then let the mirrored
            // (unlocked) weights fill the remaining headroom.
            const SkinWeights::VertexWeights& before = source[static_cast<size_t>(v)];
            for (int k = 0; k < before.count; ++k) {
                if (!boneLocked(before.boneIndices[k], lockedBones)) continue;
                setWeight(next, before.boneIndices[k], before.weights[k]);
            }
            normalizeRowRespectingLocks(next, lockedBones);
        }
        weights[static_cast<size_t>(v)] = next;
        ++modified;
    }
    return modified;
}

double totalWeightOnBone(const std::vector<SkinWeights::VertexWeights>& weights,
                         int boneHandle)
{
    double sum = 0.0;
    for (const auto& vw : weights) sum += weightOf(vw, boneHandle);
    return sum;
}

} // namespace WeightPaintOps
