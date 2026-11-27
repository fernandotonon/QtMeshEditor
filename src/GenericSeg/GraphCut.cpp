#include "GraphCut.h"

#include <algorithm>
#include <deque>
#include <limits>

namespace GenericSeg {

namespace {

// Dinic max-flow on a graph with a source and a sink. Only what alpha
// expansion needs: build, run, read the source side of the min cut.
class MaxFlow {
public:
    explicit MaxFlow(int nodes) : m_head(nodes + 2, -1), m_source(nodes), m_sink(nodes + 1) {}

    int source() const { return m_source; }
    int sink() const { return m_sink; }

    void addEdge(int u, int v, double cap, double revCap = 0.0)
    {
        if (!(cap > 0) && !(revCap > 0)) return;
        m_to.push_back(v); m_cap.push_back(std::max(0.0, cap));
        m_next.push_back(m_head[u]); m_head[u] = int(m_to.size()) - 1;
        m_to.push_back(u); m_cap.push_back(std::max(0.0, revCap));
        m_next.push_back(m_head[v]); m_head[v] = int(m_to.size()) - 1;
    }

    void run()
    {
        const int n = int(m_head.size());
        m_level.assign(n, -1);
        m_iter.assign(n, -1);
        while (bfs()) {
            m_iter = m_head;
            while (augment() > kEps) {}
        }
    }

    // After run(): reachable from the source in the residual graph.
    std::vector<char> sourceSide() const
    {
        std::vector<char> seen(m_head.size(), 0);
        std::deque<int> q{m_source};
        seen[m_source] = 1;
        while (!q.empty()) {
            const int u = q.front();
            q.pop_front();
            for (int e = m_head[u]; e >= 0; e = m_next[e])
                if (m_cap[e] > kEps && !seen[m_to[e]]) { seen[m_to[e]] = 1; q.push_back(m_to[e]); }
        }
        return seen;
    }

private:
    static constexpr double kEps = 1e-12;

    bool bfs()
    {
        std::fill(m_level.begin(), m_level.end(), -1);
        std::deque<int> q{m_source};
        m_level[m_source] = 0;
        while (!q.empty()) {
            const int u = q.front();
            q.pop_front();
            for (int e = m_head[u]; e >= 0; e = m_next[e])
                if (m_cap[e] > kEps && m_level[m_to[e]] < 0) {
                    m_level[m_to[e]] = m_level[u] + 1;
                    q.push_back(m_to[e]);
                }
        }
        return m_level[m_sink] >= 0;
    }

    // One augmenting path through the level graph, found iteratively (an
    // explicit edge stack, no recursion: callers run on worker threads with
    // small stacks, and paths can get long once short ones saturate).
    double augment()
    {
        std::vector<int>& path = m_path;
        path.clear();
        int u = m_source;
        while (true) {
            if (u == m_sink) {
                double f = std::numeric_limits<double>::infinity();
                for (int e : path) f = std::min(f, m_cap[e]);
                for (int e : path) { m_cap[e] -= f; m_cap[e ^ 1] += f; }
                return f;
            }
            int& e = m_iter[u];
            while (e >= 0 && (m_cap[e] <= kEps || m_level[m_to[e]] != m_level[u] + 1)) e = m_next[e];
            if (e >= 0) {
                path.push_back(e);
                u = m_to[e];
                continue;
            }
            // Dead end: drop u from the level graph and back up.
            m_level[u] = -1;
            if (path.empty()) return 0.0;
            const int back = path.back();
            path.pop_back();
            u = m_to[back ^ 1];
            m_iter[u] = m_next[m_iter[u]];
        }
    }

    std::vector<int> m_head, m_to, m_next, m_level, m_iter, m_path;
    std::vector<double> m_cap;
    int m_source, m_sink;
};

} // namespace

double labelingEnergy(int labelCount, const std::vector<float>& unary,
                      const std::vector<GraphEdge>& edges, const std::vector<int>& labels)
{
    double e = 0.0;
    for (size_t i = 0; i < labels.size(); ++i) e += unary[i * labelCount + labels[i]];
    for (const GraphEdge& g : edges)
        if (labels[g.a] != labels[g.b]) e += g.weight;
    return e;
}

std::vector<int> alphaExpansion(int nodeCount, int labelCount, const std::vector<float>& unary,
                                const std::vector<GraphEdge>& edges, std::vector<int> labels,
                                int maxSweeps, const std::vector<std::vector<int>>* candidates,
                                int grow)
{
    if (nodeCount <= 0 || labelCount <= 1 || int(labels.size()) != nodeCount
        || unary.size() != size_t(nodeCount) * labelCount
        || (candidates && int(candidates->size()) != labelCount))
        return labels;
    // Incident edges per node.
    std::vector<std::vector<int>> incident(nodeCount);
    for (int e = 0; e < int(edges.size()); ++e) {
        if (edges[e].a == edges[e].b) continue;
        incident[edges[e].a].push_back(e);
        incident[edges[e].b].push_back(e);
    }
    std::vector<int> local(nodeCount, -1);   // node -> index in the move's graph
    std::vector<int> mark(nodeCount, -1);    // BFS stamp
    std::vector<int> edgeStamp(edges.size(), -1);
    int stamp = 0;

    for (int sweep = 0; sweep < maxSweeps; ++sweep) {
        bool changed = false;
        for (int alpha = 0; alpha < labelCount; ++alpha) {
            ++stamp;
            // The nodes that may switch: the candidates for alpha (all nodes
            // without a list) plus everything within `grow` hops of them or of
            // alpha's current region. The rest keep their label (a restricted
            // move: still never raises the energy).
            std::vector<int> active, frontier;
            auto visit = [&](int i) {
                if (mark[i] == stamp) return;
                mark[i] = stamp;
                frontier.push_back(i);
            };
            if (candidates) {
                for (int i : (*candidates)[alpha]) if (i >= 0 && i < nodeCount) visit(i);
                for (int i = 0; i < nodeCount; ++i) if (labels[i] == alpha) visit(i);
                std::vector<int> all = frontier;
                for (int hop = 0; hop < grow; ++hop) {
                    std::vector<int> next;
                    std::swap(next, frontier);
                    for (int i : next)
                        for (int e : incident[i]) visit(edges[e].a == i ? edges[e].b : edges[e].a);
                    all.insert(all.end(), frontier.begin(), frontier.end());
                }
                for (int i : all) if (labels[i] != alpha) active.push_back(i);
            } else {
                for (int i = 0; i < nodeCount; ++i) if (labels[i] != alpha) active.push_back(i);
            }
            if (active.empty()) continue;
            const int n = int(active.size());
            for (int k = 0; k < n; ++k) local[active[k]] = k;

            // Binary move per active node: x = 0 keeps its label, x = 1 takes
            // alpha. Source side = keep.
            MaxFlow g(n);
            std::vector<double> c0(n), c1(n);   // cost of keep / switch
            for (int k = 0; k < n; ++k) {
                c0[k] = unary[size_t(active[k]) * labelCount + labels[active[k]]];
                c1[k] = unary[size_t(active[k]) * labelCount + alpha];
            }
            for (int k = 0; k < n; ++k) {
                for (int e : incident[active[k]]) {
                    if (edgeStamp[e] == stamp) continue;
                    edgeStamp[e] = stamp;
                    const int a = edges[e].a, b = edges[e].b;
                    const double w = edges[e].weight;
                    const int la = local[a], lb = local[b];
                    if (la >= 0 && lb >= 0) {
                        // Potts pair, written as A + (C-A) x_a + (D-C) x_b + (B+C-A-D)(1-x_a) x_b
                        // with A..D the costs of (keep,keep), (keep,switch), (switch,keep), (switch,switch).
                        const double A = labels[a] != labels[b] ? w : 0.0;
                        const double B = labels[a] != alpha ? w : 0.0;
                        const double C = labels[b] != alpha ? w : 0.0;
                        c1[la] += C - A;
                        c1[lb] += -C;
                        g.addEdge(la, lb, B + C - A);   // cut when a keeps and b switches
                    } else {
                        // One end fixed at its label.
                        const int act = la >= 0 ? la : lb;
                        const int actNode = la >= 0 ? a : b;
                        const int fixedLabel = labels[la >= 0 ? b : a];
                        c0[act] += labels[actNode] != fixedLabel ? w : 0.0;
                        c1[act] += alpha != fixedLabel ? w : 0.0;
                    }
                }
            }
            for (int k = 0; k < n; ++k) {
                const double m = std::min(c0[k], c1[k]);
                // Paying c1 means ending on the sink side: cut source->k.
                g.addEdge(g.source(), k, c1[k] - m);
                g.addEdge(k, g.sink(), c0[k] - m);
            }
            g.run();
            const std::vector<char> keep = g.sourceSide();
            // Energy change of the move, from the switched nodes only.
            std::vector<int> switched;
            for (int k = 0; k < n; ++k) if (!keep[k]) switched.push_back(active[k]);
            double delta = 0.0;
            ++stamp;
            for (int i : switched) {
                delta += unary[size_t(i) * labelCount + alpha] - unary[size_t(i) * labelCount + labels[i]];
                mark[i] = stamp;   // reuse as "switched" flag
            }
            for (int i : switched)
                for (int e : incident[i]) {
                    if (edgeStamp[e] == stamp) continue;
                    edgeStamp[e] = stamp;
                    const int a = edges[e].a, b = edges[e].b;
                    const int na = mark[a] == stamp ? alpha : labels[a];
                    const int nb = mark[b] == stamp ? alpha : labels[b];
                    delta += ((na != nb) - (labels[a] != labels[b])) * double(edges[e].weight);
                }
            for (int i : active) local[i] = -1;
            if (delta < -1e-12) {
                for (int i : switched) labels[i] = alpha;
                changed = true;
            }
        }
        if (!changed) break;
    }
    return labels;
}

} // namespace GenericSeg
