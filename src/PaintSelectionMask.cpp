#include "PaintSelectionMask.h"

#include "TexturePaintBuffer.h"

#include <algorithm>
#include <cmath>
#include <utility>

void PaintSelectionMask::resize(int width, int height)
{
    m_width = std::max(0, width);
    m_height = std::max(0, height);
    m_data.assign(static_cast<size_t>(m_width) * static_cast<size_t>(m_height), 0);
    m_setCount = 0;
    m_bbox = {};
}

bool PaintSelectionMask::isSelected(int x, int y) const
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) return false;
    return m_data[static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x)] != 0;
}

void PaintSelectionMask::setSelected(int x, int y, bool selected)
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) return;
    const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x);
    const bool was = m_data[idx] != 0;
    if (was == selected) return;
    m_data[idx] = selected ? 1 : 0;
    if (selected) {
        ++m_setCount;
        expandBBox(x, y);
    } else {
        --m_setCount;
        // Shrinking the bbox precisely is O(W*H); we let it stay loose
        // and rebuild on demand if it matters (rebuildSummary).
    }
}

void PaintSelectionMask::clear()
{
    if (m_setCount == 0 && m_bbox.empty()) return;
    std::fill(m_data.begin(), m_data.end(), 0);
    m_setCount = 0;
    m_bbox = {};
}

void PaintSelectionMask::selectAll()
{
    if (m_width <= 0 || m_height <= 0) return;
    std::fill(m_data.begin(), m_data.end(), 1);
    m_setCount = m_width * m_height;
    m_bbox = {0, 0, m_width, m_height};
}

void PaintSelectionMask::invert()
{
    if (m_width <= 0 || m_height <= 0) return;
    for (auto& b : m_data) b = b ? 0 : 1;
    rebuildSummary();
}

namespace {

inline bool colourWithinTol(const Ogre::ColourValue& a,
                            const Ogre::ColourValue& b,
                            float tol)
{
    return std::fabs(a.r - b.r) <= tol
        && std::fabs(a.g - b.g) <= tol
        && std::fabs(a.b - b.b) <= tol
        && std::fabs(a.a - b.a) <= tol;
}

} // namespace

int PaintSelectionMask::smartSelect(const TexturePaintBuffer& buf,
                                    int sx, int sy,
                                    float tolerance,
                                    CombineMode mode)
{
    if (buf.width() != m_width || buf.height() != m_height) return 0;
    if (m_width <= 0 || m_height <= 0) return 0;
    if (sx < 0 || sy < 0 || sx >= m_width || sy >= m_height) return 0;
    tolerance = std::clamp(tolerance, 0.0f, 1.0f);

    const Ogre::ColourValue seed = buf.pixel(sx, sy);

    if (mode == CombineMode::Replace)
        clear();

    // Flood fill: 4-connected. Visited bitmap prevents revisiting in
    // the Add/Sub modes where the mask state alone isn't enough (a
    // pixel that's already in the mask in Add mode would otherwise
    // block expansion through it).
    std::vector<uint8_t> visited(m_data.size(), 0);
    std::vector<std::pair<int,int>> stack;
    stack.reserve(64);
    stack.push_back({sx, sy});
    int affected = 0;

    auto applyAt = [&](int x, int y) {
        const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x);
        const bool was = m_data[idx] != 0;
        if (mode == CombineMode::Sub) {
            if (!was) return false;
            m_data[idx] = 0;
            --m_setCount;
            return true;
        }
        // Replace and Add both set the bit if it isn't already set.
        if (was) return false;
        m_data[idx] = 1;
        ++m_setCount;
        expandBBox(x, y);
        return true;
    };

    while (!stack.empty()) {
        auto [x, y] = stack.back();
        stack.pop_back();
        if (x < 0 || y < 0 || x >= m_width || y >= m_height) continue;
        const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x);
        if (visited[idx]) continue;
        const Ogre::ColourValue here = buf.pixel(x, y);
        if (!colourWithinTol(here, seed, tolerance)) continue;
        visited[idx] = 1;
        if (applyAt(x, y)) ++affected;
        stack.push_back({x + 1, y});
        stack.push_back({x - 1, y});
        stack.push_back({x, y + 1});
        stack.push_back({x, y - 1});
    }

    // Sub mode can shrink the bbox; rebuild for correctness.
    if (mode == CombineMode::Sub)
        rebuildSummary();
    return affected;
}

void PaintSelectionMask::rebuildSummary()
{
    m_setCount = 0;
    m_bbox = {};
    for (int y = 0; y < m_height; ++y) {
        for (int x = 0; x < m_width; ++x) {
            if (m_data[static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x)]) {
                ++m_setCount;
                expandBBox(x, y);
            }
        }
    }
}

void PaintSelectionMask::expandBBox(int x, int y)
{
    if (m_bbox.empty()) {
        m_bbox = {x, y, x + 1, y + 1};
        return;
    }
    m_bbox.x0 = std::min(m_bbox.x0, x);
    m_bbox.y0 = std::min(m_bbox.y0, y);
    m_bbox.x1 = std::max(m_bbox.x1, x + 1);
    m_bbox.y1 = std::max(m_bbox.y1, y + 1);
}
