#include "Leiden.h"

#include <algorithm>
#include <deque>
#include <numeric>
#include <random>
#include <unordered_map>

namespace GenericSeg {

namespace {

constexpr double kEps = 1e-12;

// Weighted undirected graph with explicit self-loop weights. K[i] is the
// node's degree in the ORIGINAL sense (a self-loop counts twice), so sum(K) is
// always 2m of the input graph, at every aggregation level.
struct Graph {
    int n = 0;
    std::vector<std::vector<std::pair<int, double>>> adj;   // no self-loops
    std::vector<double> self;
    std::vector<double> K;
    double twoM = 0;
    // Null model: the cost of putting i and j together is coef * W[i] * W[j].
    // Modularity: W = degree, coef = gamma / 2m. CPM: W = node count, coef = gamma.
    std::vector<double> W;
    double coef = 0;
};

void setQuality(Graph& g, const LeidenOptions& o)
{
    if (o.quality == LeidenQuality::Cpm) {
        g.W.assign(g.n, 1.0);
        g.coef = o.resolution;
    } else {
        g.W = g.K;
        g.coef = g.twoM > 0 ? o.resolution / g.twoM : 0.0;
    }
}

Graph buildGraph(int n, const std::vector<GraphEdge>& edges)
{
    Graph g;
    g.n = n;
    g.adj.resize(n);
    g.self.assign(n, 0.0);
    g.K.assign(n, 0.0);
    std::vector<std::unordered_map<int, double>> acc(n);
    for (const GraphEdge& e : edges) {
        if (e.a < 0 || e.b < 0 || e.a >= n || e.b >= n || !(e.weight > 0)) continue;
        if (e.a == e.b) {
            g.self[e.a] += e.weight;
            g.K[e.a] += 2.0 * e.weight;
        } else {
            acc[e.a][e.b] += e.weight;
            acc[e.b][e.a] += e.weight;
            g.K[e.a] += e.weight;
            g.K[e.b] += e.weight;
        }
    }
    for (int i = 0; i < n; ++i) {
        g.adj[i].assign(acc[i].begin(), acc[i].end());
        std::sort(g.adj[i].begin(), g.adj[i].end());   // deterministic order
    }
    g.twoM = std::accumulate(g.K.begin(), g.K.end(), 0.0);
    return g;
}

std::vector<int> shuffled(int n, std::mt19937& rng)  // NOSONAR - deterministic seed by design (not security)
{
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), rng);  // NOSONAR - deterministic seed by design (not security)
    return order;
}

// Fast local moving (Leiden's queue-based variant of Louvain's first phase).
// Returns true if any node changed community.
bool moveNodes(const Graph& g, std::vector<int>& comm, std::mt19937& rng)  // NOSONAR - deterministic seed by design (not security)
{
    const int n = g.n;
    std::vector<double> tot(n, 0.0);
    std::vector<int> size(n, 0);
    for (int i = 0; i < n; ++i) { tot[comm[i]] += g.W[i]; ++size[comm[i]]; }
    std::vector<int> empty;
    for (int c = n - 1; c >= 0; --c) if (size[c] == 0) empty.push_back(c);

    std::deque<int> queue;
    std::vector<char> inQueue(n, 1);
    for (int i : shuffled(n, rng)) queue.push_back(i);
    std::vector<double> w(n, 0.0);
    std::vector<int> touched;
    bool changed = false;

    while (!queue.empty()) {
        const int i = queue.front();
        queue.pop_front();
        inQueue[i] = 0;
        const int old = comm[i];
        tot[old] -= g.W[i];
        if (--size[old] == 0) empty.push_back(old);

        touched.clear();
        for (const auto& [j, wij] : g.adj[i]) {
            const int c = comm[j];
            if (w[c] == 0.0) touched.push_back(c);
            w[c] += wij;
        }
        const double scale = g.coef * g.W[i];
        int best = old;
        double bestGain = w[old] - scale * tot[old];
        for (int c : touched) {
            const double gain = w[c] - scale * tot[c];
            if (gain > bestGain + kEps) { bestGain = gain; best = c; }
        }
        if (bestGain < -kEps) {   // better alone than anywhere
            best = size[old] == 0 ? old : empty.back();
        }
        for (int c : touched) w[c] = 0.0;

        if (size[best] == 0) {
            auto it = std::find(empty.begin(), empty.end(), best);
            if (it != empty.end()) empty.erase(it);
        }
        comm[i] = best;
        tot[best] += g.W[i];
        ++size[best];
        if (best != old) {
            changed = true;
            for (const auto& [j, wij] : g.adj[i]) {
                (void)wij;
                if (!inQueue[j] && comm[j] != best) { inQueue[j] = 1; queue.push_back(j); }
            }
        }
    }
    return changed;
}

// Leiden refinement: inside each community, start from singletons and merge
// a still-singleton node into a well-connected sub-community (greedy, the
// theta -> 0 limit of the paper's randomised choice). Guarantees every
// refined community is connected.
std::vector<int> refine(const Graph& g, const std::vector<int>& comm, std::mt19937& rng)  // NOSONAR - deterministic seed by design (not security)
{
    const int n = g.n;
    std::vector<int> refined(n);
    std::iota(refined.begin(), refined.end(), 0);
    std::vector<double> rtot(g.W);
    std::vector<int> rsize(n, 1);
    std::vector<double> commTot(n, 0.0);
    for (int i = 0; i < n; ++i) commTot[comm[i]] += g.W[i];
    // ext[T] = weight from refined community T to the rest of its community.
    std::vector<double> ext(n, 0.0);
    for (int i = 0; i < n; ++i)
        for (const auto& [j, wij] : g.adj[i])
            if (comm[j] == comm[i]) ext[i] += wij;

    std::vector<double> w(n, 0.0);
    std::vector<int> touched;
    for (int v : shuffled(n, rng)) {
        if (rsize[refined[v]] > 1) continue;
        const double S = commTot[comm[v]];
        // v itself must be well connected to its community.
        if (ext[v] < g.coef * g.W[v] * (S - g.W[v]) - kEps) continue;

        touched.clear();
        for (const auto& [u, wvu] : g.adj[v]) {
            if (comm[u] != comm[v]) continue;
            const int T = refined[u];
            if (w[T] == 0.0) touched.push_back(T);
            w[T] += wvu;
        }
        const double scale = g.coef * g.W[v];
        int best = -1;
        double bestGain = 0.0;
        for (int T : touched) {
            if (T == refined[v]) continue;
            const bool wellConnected = ext[T] >= g.coef * rtot[T] * (S - rtot[T]) - kEps;
            if (!wellConnected) continue;
            const double gain = w[T] - scale * rtot[T];
            if (gain >= -kEps && (best < 0 || gain > bestGain + kEps)) { best = T; bestGain = gain; }
        }
        if (best >= 0) {
            const int own = refined[v];
            ext[best] = ext[best] + ext[own] - 2.0 * w[best];
            rtot[best] += rtot[own];
            rsize[best] += 1;
            rsize[own] = 0;
            refined[v] = best;
        }
        for (int T : touched) w[T] = 0.0;
    }
    return refined;
}

// Collapse nodes by `part` (compacted ids). Returns the aggregated graph and
// fills `newIndex` (old node -> new node).
Graph aggregate(const Graph& g, const std::vector<int>& part, std::vector<int>& newIndex)
{
    std::unordered_map<int, int> ids;
    newIndex.assign(g.n, 0);
    for (int i = 0; i < g.n; ++i)
        newIndex[i] = ids.emplace(part[i], int(ids.size())).first->second;
    Graph a;
    a.n = int(ids.size());
    a.adj.resize(a.n);
    a.self.assign(a.n, 0.0);
    a.K.assign(a.n, 0.0);
    a.W.assign(a.n, 0.0);
    a.twoM = g.twoM;
    a.coef = g.coef;
    std::vector<std::unordered_map<int, double>> acc(a.n);
    for (int i = 0; i < g.n; ++i) {
        const int I = newIndex[i];
        a.K[I] += g.K[i];
        a.W[I] += g.W[i];
        a.self[I] += g.self[i];
        for (const auto& [j, wij] : g.adj[i]) {
            const int J = newIndex[j];
            if (I == J) {
                if (i < j) a.self[I] += wij;   // each internal edge once
            } else {
                acc[I][J] += wij;
            }
        }
    }
    for (int I = 0; I < a.n; ++I) {
        a.adj[I].assign(acc[I].begin(), acc[I].end());
        std::sort(a.adj[I].begin(), a.adj[I].end());
    }
    return a;
}

int countDistinct(const std::vector<int>& v)
{
    std::vector<int> s(v);
    std::sort(s.begin(), s.end());
    return int(std::unique(s.begin(), s.end()) - s.begin());
}

} // namespace

std::vector<int> leiden(int nodeCount, const std::vector<GraphEdge>& edges, const LeidenOptions& opts)
{
    if (nodeCount <= 0) return {};
    Graph g = buildGraph(nodeCount, edges);
    std::vector<int> result(nodeCount);
    std::iota(result.begin(), result.end(), 0);
    if (!(g.twoM > 0)) return result;   // no edges: all singletons
    setQuality(g, opts);

    std::mt19937 rng(opts.seed);  // NOSONAR - deterministic seed by design (not security)
    std::vector<int> nodeOf(nodeCount);   // original node -> current node
    std::iota(nodeOf.begin(), nodeOf.end(), 0);
    std::vector<int> comm(g.n);
    std::iota(comm.begin(), comm.end(), 0);

    for (int level = 0; level < opts.maxLevels; ++level) {
        moveNodes(g, comm, rng);
        if (countDistinct(comm) == g.n) break;   // every node alone: converged

        std::vector<int> refined = refine(g, comm, rng);
        // If refinement merged nothing, aggregate by the moved partition
        // (Louvain step) so the loop still makes progress.
        const bool refinedShrinks = countDistinct(refined) < g.n;
        std::vector<int> newIndex;
        Graph next = aggregate(g, refinedShrinks ? refined : comm, newIndex);
        std::vector<int> nextComm(next.n, 0);
        for (int i = 0; i < g.n; ++i) nextComm[newIndex[i]] = comm[i];
        // Compact community ids into [0, next.n).
        std::unordered_map<int, int> cid;
        for (int& c : nextComm) c = cid.emplace(c, int(cid.size())).first->second;
        for (int& v : nodeOf) v = newIndex[v];
        g = std::move(next);
        comm = std::move(nextComm);
    }
    std::unordered_map<int, int> out;
    for (int i = 0; i < nodeCount; ++i)
        result[i] = out.emplace(comm[nodeOf[i]], int(out.size())).first->second;
    return result;
}

double modularity(int nodeCount, const std::vector<GraphEdge>& edges,
                  const std::vector<int>& community, double resolution)
{
    if (nodeCount <= 0 || int(community.size()) != nodeCount) return 0.0;
    double twoM = 0;
    std::unordered_map<int, double> inside, degree;
    for (const GraphEdge& e : edges) {
        if (e.a < 0 || e.b < 0 || e.a >= nodeCount || e.b >= nodeCount || !(e.weight > 0)) continue;
        twoM += 2.0 * e.weight;
        degree[community[e.a]] += e.weight;
        degree[community[e.b]] += e.weight;
        if (community[e.a] == community[e.b]) inside[community[e.a]] += e.weight;
    }
    if (!(twoM > 0)) return 0.0;
    double q = 0;
    for (const auto& [c, d] : degree)
        q += 2.0 * inside[c] / twoM - resolution * (d / twoM) * (d / twoM);
    return q;
}

} // namespace GenericSeg
