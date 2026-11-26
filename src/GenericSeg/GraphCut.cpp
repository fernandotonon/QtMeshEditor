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
                                int maxSweeps)
{
    if (nodeCount <= 0 || labelCount <= 1 || int(labels.size()) != nodeCount
        || unary.size() != size_t(nodeCount) * labelCount)
        return labels;
    double energy = labelingEnergy(labelCount, unary, edges, labels);
    for (int sweep = 0; sweep < maxSweeps; ++sweep) {
        bool changed = false;
        for (int alpha = 0; alpha < labelCount; ++alpha) {
            // Binary move per node: x = 0 keeps its label, x = 1 takes alpha.
            // Source side = keep. A node already labelled alpha has nothing to
            // decide (equal costs).
            MaxFlow g(nodeCount);
            std::vector<double> c0(nodeCount), c1(nodeCount);   // cost of keep / switch
            for (int i = 0; i < nodeCount; ++i) {
                c0[i] = unary[size_t(i) * labelCount + labels[i]];
                c1[i] = unary[size_t(i) * labelCount + alpha];
            }
            // Potts pair, written as A + (C-A) x_a + (D-C) x_b + (B+C-A-D)(1-x_a) x_b
            // with A..D the costs of (keep,keep), (keep,switch), (switch,keep), (switch,switch).
            for (const GraphEdge& e : edges) {
                const int a = e.a, b = e.b;
                if (a == b) continue;
                const double w = e.weight;
                const double A = labels[a] != labels[b] ? w : 0.0;
                const double B = labels[a] != alpha ? w : 0.0;
                const double C = labels[b] != alpha ? w : 0.0;
                const double D = 0.0;
                c1[a] += C - A;
                c1[b] += D - C;
                // Cut a->b when a keeps and b switches.
                g.addEdge(a, b, B + C - A - D);
            }
            for (int i = 0; i < nodeCount; ++i) {
                if (labels[i] == alpha) {   // forced keep
                    g.addEdge(g.source(), i, 0.0);
                    g.addEdge(i, g.sink(), 0.0);
                    c1[i] = c0[i] + 1e30;
                }
                const double m = std::min(c0[i], c1[i]);
                // Paying c1 means ending on the sink side: cut source->i.
                g.addEdge(g.source(), i, c1[i] - m);
                g.addEdge(i, g.sink(), c0[i] - m);
            }
            g.run();
            const std::vector<char> keep = g.sourceSide();
            std::vector<int> next = labels;
            for (int i = 0; i < nodeCount; ++i)
                if (!keep[i]) next[i] = alpha;
            const double e2 = labelingEnergy(labelCount, unary, edges, next);
            if (e2 < energy - 1e-9 * std::max(1.0, std::abs(energy))) {
                labels.swap(next);
                energy = e2;
                changed = true;
            }
        }
        if (!changed) break;
    }
    return labels;
}

} // namespace GenericSeg
