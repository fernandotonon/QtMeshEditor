#include "KimodoMath.h"

#include <QStringList>

#include <algorithm>
#include <cfenv>
#include <cmath>

namespace AIMotion::KimodoMath {

namespace {

bool isAlnum(QChar c) { return c.isLetterOrNumber(); }

// Python str.capitalize(): first character upper-cased, the rest lower-cased.
QString pyCapitalize(const QString& s)
{
    if (s.isEmpty()) return s;
    return s.left(1).toUpper() + s.mid(1).toLower();
}

} // namespace

QString sanitizeText(const QString& input)
{
    QString text = input.simplified();   // strip + collapse runs of whitespace into one space
    if (text.isEmpty()) return text;

    // Leading non-alphanumerics go. Python's loop leaves i on the LAST index when nothing is alphanumeric.
    int i = 0;
    for (i = 0; i < text.size(); ++i)
        if (isAlnum(text[i])) break;
    if (i == text.size()) i = int(text.size()) - 1;
    text = pyCapitalize(text.mid(i));

    static const QString kFinal = QStringLiteral(".!?\"])'");
    int j = int(text.size()) - 1;
    for (; j >= 0; --j)
        if (isAlnum(text[j]) || kFinal.contains(text[j])) break;
    if (j < 0) j = 0;
    text = text.left(j + 1);
    if (!QStringLiteral(".!?").contains(text.back())) text += QLatin1Char('.');

    for (const QChar brk : {QChar('.'), QChar('!'), QChar('?')}) {
        QStringList parts;
        for (const QString& x : text.split(brk)) {
            const QString y = x.trimmed();
            parts << (y.isEmpty() ? y : y.left(1).toUpper() + y.mid(1));
        }
        text = parts.join(QString(brk) + QLatin1Char(' ')).trimmed();
    }
    return text;
}

LlmPrompt llm2vecPrompt(const QString& sanitizedText)
{
    LlmPrompt p;
    const QString t = sanitizedText.trimmed();
    p.full = QStringLiteral("<|start_header_id|>user<|end_header_id|>\n\n") + t + QStringLiteral("<|eot_id|>");
    p.pooled = t + QStringLiteral("<|eot_id|>");
    return p;
}

DdimSchedule ddimSchedule(const std::vector<float>& base, int steps)
{
    DdimSchedule s;
    const int n = int(base.size());
    if (n < 2 || steps < 1) return s;
    // torch: arange(n) (int64) * python-float stride -> float32, round half-to-even, clamp to n-1.
    const float frac = float(double(n - 1) / double(std::max(1, steps - 1)));
    const int previousMode = std::fegetround();
    std::fesetround(FE_TONEAREST);
    std::vector<float> acp(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int64_t t = std::min<int64_t>(int64_t(std::nearbyint(float(i) * frac)), n - 1);
        if (i < steps) s.timesteps.push_back(t);
        acp[size_t(i)] = base[size_t(t)];
    }
    std::fesetround(previousMode);
    // calc_diffusion_vars: betas from consecutive ratios, then cumprod again (float32 throughout).
    float cum = 1.0f, last = 1.0f;
    s.alphasCumprod.resize(size_t(steps));
    s.alphasCumprodPrev.resize(size_t(steps));
    for (int i = 0; i < n && i < steps; ++i) {
        const float beta = 1.0f - acp[size_t(i)] / last;
        last = acp[size_t(i)];
        const float prev = cum;
        cum *= (1.0f - beta);
        s.alphasCumprodPrev[size_t(i)] = i == 0 ? 1.0f : std::max(prev, 1e-9f);
        s.alphasCumprod[size_t(i)] = std::max(cum, 1e-9f);
    }
    return s;
}

void ddimStep(const DdimSchedule& s, int i, const float* pred, float* x, size_t n)
{
    const float acp = s.alphasCumprod[size_t(i)];
    const float prev = s.alphasCumprodPrev[size_t(i)];
    const float sqrtRecip = 1.0f / std::sqrt(acp);
    const float sqrtRecipM1 = 1.0f / std::sqrt(acp / (1.0f - acp));
    const float a = std::sqrt(prev), b = std::sqrt(1.0f - prev);
    for (size_t k = 0; k < n; ++k) {
        const float eps = (sqrtRecip * x[k] - pred[k]) / sqrtRecipM1;
        x[k] = pred[k] * a + b * eps;
    }
}

namespace {

// TrajectorySmoother (pos_weight 0, no loop): M = AᵀA + ρI with A the second-difference operator on rows
// 1..n-2. M is symmetric positive definite and pentadiagonal, so a banded Cholesky replaces scipy's splu.
class Smoother {
public:
    Smoother(int n, float margin) : m_n(n), m_margin(margin)
    {
        // Band of AᵀA: d0 diagonal, d1/d2 first/second off-diagonals.
        std::vector<double> d0(static_cast<size_t>(n), 0.0), d1(static_cast<size_t>(n), 0.0), d2(static_cast<size_t>(n), 0.0);
        for (int r = 1; r <= n - 2; ++r) {
            const int c[3] = {r - 1, r, r + 1};
            const double v[3] = {-1.0, 2.0, -1.0};
            for (int a = 0; a < 3; ++a)
                for (int b = a; b < 3; ++b) {
                    const double p = v[a] * v[b];
                    if (a == b) d0[size_t(c[a])] += p;
                    else if (b - a == 1) d1[size_t(c[a])] += p;
                    else d2[size_t(c[a])] += p;
                }
        }
        double diagMax = 0.0;
        for (double d : d0) diagMax = std::max(diagMax, std::fabs(d));
        m_rho = 0.25 * std::sqrt(diagMax);
        for (double& d : d0) d += m_rho;
        // Banded LDLᵀ: L unit lower with two sub-diagonals l1, l2.
        m_D.assign(size_t(n), 0.0);
        m_l1.assign(size_t(n), 0.0);
        m_l2.assign(size_t(n), 0.0);
        for (int i = 0; i < n; ++i) {
            double d = d0[size_t(i)];
            if (i >= 1) d -= m_l1[size_t(i)] * m_l1[size_t(i)] * m_D[size_t(i - 1)];
            if (i >= 2) d -= m_l2[size_t(i)] * m_l2[size_t(i)] * m_D[size_t(i - 2)];
            m_D[size_t(i)] = d;
            if (i + 1 < n) {
                double v = d1[size_t(i)];
                if (i >= 1) v -= m_l2[size_t(i + 1)] * m_l1[size_t(i)] * m_D[size_t(i - 1)];
                m_l1[size_t(i + 1)] = v / d;
            }
            if (i + 2 < n) m_l2[size_t(i + 2)] = d2[size_t(i)] / d;
        }
    }

    bool valid() const
    {
        for (double d : m_D)
            if (!(d > 0.0)) return false;
        return m_n >= 3;
    }

    void solve(std::vector<double>& r) const
    {
        const int n = m_n;
        for (int i = 1; i < n; ++i) {
            r[size_t(i)] -= m_l1[size_t(i)] * r[size_t(i - 1)];
            if (i >= 2) r[size_t(i)] -= m_l2[size_t(i)] * r[size_t(i - 2)];
        }
        for (int i = 0; i < n; ++i) r[size_t(i)] /= m_D[size_t(i)];
        for (int i = n - 2; i >= 0; --i) {
            r[size_t(i)] -= m_l1[size_t(i + 1)] * r[size_t(i + 1)];
            if (i + 2 < n) r[size_t(i)] -= m_l2[size_t(i + 2)] * r[size_t(i + 2)];
        }
    }

    // ADMM with over-relaxation; x/z/u float32 like the numpy arrays, the linear solve in double.
    std::vector<std::array<float, 2>> smooth(const std::vector<std::array<float, 2>>& target,
                                             std::vector<std::array<float, 2>> x) const
    {
        const int n = m_n;
        const float alpha = 1.8f;
        std::vector<std::array<float, 2>> z(static_cast<size_t>(n), {0.0f, 0.0f}), u(static_cast<size_t>(n), {0.0f, 0.0f});
        std::vector<double> r0(static_cast<size_t>(n)), r1(static_cast<size_t>(n));
        for (int it = 0; it < 500; ++it) {
            for (int i = 0; i < n; ++i) {
                float zx = x[size_t(i)][0] + u[size_t(i)][0] - target[size_t(i)][0];
                float zy = x[size_t(i)][1] + u[size_t(i)][1] - target[size_t(i)][1];
                const float norm = std::sqrt(zx * zx + zy * zy);
                if (double(norm) > double(m_margin)) {
                    const float scale = float(double(m_margin) / double(norm));
                    zx *= scale;
                    zy *= scale;
                }
                z[size_t(i)] = {zx + target[size_t(i)][0], zy + target[size_t(i)][1]};
            }
            for (int i = 0; i < n; ++i)
                for (int k = 0; k < 2; ++k) u[size_t(i)][size_t(k)] += alpha * (x[size_t(i)][size_t(k)] - z[size_t(i)][size_t(k)]);
            for (int i = 0; i < n; ++i) {
                r0[size_t(i)] = m_rho * double(z[size_t(i)][0] - u[size_t(i)][0]);
                r1[size_t(i)] = m_rho * double(z[size_t(i)][1] - u[size_t(i)][1]);
            }
            solve(r0);
            solve(r1);
            for (int i = 0; i < n; ++i) x[size_t(i)] = {float(r0[size_t(i)]), float(r1[size_t(i)])};
        }
        return x;
    }

private:
    int m_n;
    float m_margin;
    double m_rho = 0.0;
    std::vector<double> m_D, m_l1, m_l2;
};

} // namespace

std::vector<std::array<float, 2>> smoothSignal(const std::vector<std::array<float, 2>>& x, float margin)
{
    const int N = int(x.size());
    if (N < 3) return x;
    std::array<float, 2> mean = {0.0f, 0.0f};
    for (const auto& p : x) {
        mean[0] += p[0];
        mean[1] += p[1];
    }
    mean[0] /= float(N);
    mean[1] /= float(N);
    std::vector<std::array<float, 2>> xs(static_cast<size_t>(N), mean);

    int levels = int(std::floor(std::log2(double(N))));
    levels = std::max(levels - 4, 1);
    int step = 1 << levels;
    while (true) {
        std::vector<int> idx;
        for (int i = 0; i < N; i += step) idx.push_back(i);
        const int n = int(idx.size());
        Smoother smoother(n, margin);
        if (!smoother.valid()) return x;   // Kimodo's splu would raise here (fewer than 3 coarse samples)
        std::vector<std::array<float, 2>> tgt(static_cast<size_t>(n)), x0(static_cast<size_t>(n));
        for (int k = 0; k < n; ++k) {
            tgt[size_t(k)] = x[size_t(idx[size_t(k)])];
            x0[size_t(k)] = xs[size_t(idx[size_t(k)])];
        }
        const auto sm = smoother.smooth(tgt, x0);
        for (int k = 0; k < n; ++k) xs[size_t(idx[size_t(k)])] = sm[size_t(k)];

        // Interleave: midpoints between the solved samples; the last interleaved sample extrapolates when it has
        // no right neighbour. At step 1 the "interleaved" set is every sample (Python's x[0::1]), so the final
        // pass averages each sample with its successor — reproduced as written.
        const int next = step / 2;
        std::vector<int> inter;
        for (int i = next; i < N; i += step) inter.push_back(i);
        int numInter = int(inter.size());
        std::vector<std::array<float, 2>> coarse(static_cast<size_t>(n));
        for (int k = 0; k < n; ++k) coarse[size_t(k)] = xs[size_t(idx[size_t(k)])];
        if (numInter == n) {
            const auto& a = coarse[size_t(n - 1)];
            const auto& b = coarse[size_t(n - 2)];
            xs[size_t(inter.back())] = {a[0] + (a[0] - b[0]) / 2.0f, a[1] + (a[1] - b[1]) / 2.0f};
            --numInter;
        }
        // numpy evaluates the right-hand side (from the coarse samples, read above) before assigning.
        if (next == 0) {
            for (int k = 0; k < n; ++k) coarse[size_t(k)] = xs[size_t(idx[size_t(k)])];   // includes the extrapolated tail
        }
        for (int k = 0; k < numInter; ++k)
            xs[size_t(inter[size_t(k)])] = {(coarse[size_t(k)][0] + coarse[size_t(k + 1)][0]) / 2.0f,
                                            (coarse[size_t(k)][1] + coarse[size_t(k + 1)][1]) / 2.0f};
        if (step == 1) break;
        step /= 2;
    }
    return xs;
}

std::array<float, 4> quatFromMatrixWxyz(const float m[9])
{
    const float m00 = m[0], m01 = m[1], m02 = m[2], m10 = m[3], m11 = m[4], m12 = m[5], m20 = m[6], m21 = m[7], m22 = m[8];
    const float s[4] = {1.0f + m00 + m11 + m22, 1.0f + m00 - m11 - m22, 1.0f - m00 + m11 - m22, 1.0f - m00 - m11 + m22};
    float qa[4];
    for (int k = 0; k < 4; ++k) qa[k] = std::sqrt(s[k] > 0.0f ? s[k] : 0.0f);
    const float cand[4][4] = {
        {qa[0] * qa[0], m21 - m12, m02 - m20, m10 - m01},
        {m21 - m12, qa[1] * qa[1], m10 + m01, m02 + m20},
        {m02 - m20, m10 + m01, qa[2] * qa[2], m12 + m21},
        {m10 - m01, m20 + m02, m21 + m12, qa[3] * qa[3]},
    };
    int best = 0;   // torch.argmax: first maximum
    for (int k = 1; k < 4; ++k)
        if (qa[k] > qa[best]) best = k;
    const float denom = 2.0f * std::max(qa[best], 0.1f);
    return {cand[best][0] / denom, cand[best][1] / denom, cand[best][2] / denom, cand[best][3] / denom};
}

void matrixFromQuatWxyz(const std::array<float, 4>& q, float o[9])
{
    const float r = q[0], i = q[1], j = q[2], k = q[3];
    const float two_s = 2.0f / (r * r + i * i + j * j + k * k);
    o[0] = 1 - two_s * (j * j + k * k); o[1] = two_s * (i * j - k * r);     o[2] = two_s * (i * k + j * r);
    o[3] = two_s * (i * j + k * r);     o[4] = 1 - two_s * (i * i + k * k); o[5] = two_s * (j * k - i * r);
    o[6] = two_s * (i * k - j * r);     o[7] = two_s * (j * k + i * r);     o[8] = 1 - two_s * (i * i + j * j);
}

} // namespace AIMotion::KimodoMath
