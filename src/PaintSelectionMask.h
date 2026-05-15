#ifndef PAINTSELECTIONMASK_H
#define PAINTSELECTIONMASK_H

#include <OgreColourValue.h>
#include <OgreVector.h>

#include <cstddef>
#include <cstdint>
#include <vector>

class TexturePaintBuffer;

/**
 * @brief Per-pixel boolean selection mask for the texture paint canvas.
 *
 * Paired 1:1 in size with a TexturePaintBuffer. A pixel is either
 * "selected" (1) or "not selected" (0). Fill / erase / delete actions
 * applied through TexturePaintController are restricted to the selected
 * pixels when the mask is non-empty — matches the Photoshop / GIMP
 * "marching ants" model where a selection scopes subsequent ops.
 *
 * Owns a flat `std::vector<uint8_t>` of size width*height. Also tracks a
 * tight AABB (bbox) of currently-set pixels so we can render the
 * marching-ants overlay without scanning the whole buffer every frame.
 *
 * Pure data — no Qt or Ogre runtime dependencies beyond Vector2 /
 * ColourValue used by the smart-select helper.
 */
class PaintSelectionMask
{
public:
    /// AABB of the current selection in pixel coords. [x0..x1) × [y0..y1).
    struct BBox {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        bool empty() const { return x1 <= x0 || y1 <= y0; }
        int width() const { return empty() ? 0 : x1 - x0; }
        int height() const { return empty() ? 0 : y1 - y0; }
    };

    PaintSelectionMask() = default;

    /// Match the buffer's size and clear all selection bits.
    void resize(int width, int height);

    int width() const { return m_width; }
    int height() const { return m_height; }
    /// True iff every pixel is unselected.
    bool isEmpty() const { return m_setCount == 0; }
    /// Count of selected pixels (cached; O(1)).
    int selectedCount() const { return m_setCount; }
    /// Tight bbox of currently-set pixels. Empty when isEmpty().
    const BBox& bbox() const { return m_bbox; }
    /// Raw mask byte buffer (row-major, top-left origin, 0 = unselected,
    /// 1 = selected).
    const std::vector<uint8_t>& data() const { return m_data; }
    std::vector<uint8_t>& data() { return m_data; }

    /// True if the pixel at (x, y) is in the selection. Out-of-bounds
    /// returns false.
    bool isSelected(int x, int y) const;

    /// Set a single pixel's selection state. No-op on OOB.
    void setSelected(int x, int y, bool selected);

    /// Clear the entire selection.
    void clear();

    /// Mark every pixel as selected. Equivalent to "select all".
    void selectAll();

    /// Invert the selection — every set bit flips.
    void invert();

    /**
     * @brief Magic-wand / fuzzy select: flood-fill the selection
     * starting at (sx, sy), adding pixels whose color is within
     * `tolerance` of the seed color (per-channel, in [0..1] RGB +
     * alpha space).
     *
     * @param buf       The pixel buffer to sample. Mask is grown over
     *                  pixels of `buf` whose colour is similar to
     *                  the seed's.
     * @param sx, sy    Seed pixel.
     * @param tolerance Per-channel L∞ tolerance, [0..1]. 0 = exact
     *                  match only; 1 = match everything.
     * @param mode      kReplace = wipe then grow new region;
     *                  kAdd     = OR new region into current mask;
     *                  kSub     = remove the matched region from
     *                              the current mask.
     * @return number of pixels added/removed (depending on mode).
     */
    enum class CombineMode { Replace = 0, Add = 1, Sub = 2 };
    int smartSelect(const TexturePaintBuffer& buf,
                    int sx, int sy,
                    float tolerance,
                    CombineMode mode = CombineMode::Replace);

private:
    /// Recompute bbox + count from scratch. Called after invert/erase ops
    /// that can't update them incrementally without measurable extra work.
    void rebuildSummary();
    /// Grow the bbox to include (x, y).
    void expandBBox(int x, int y);

    int m_width = 0;
    int m_height = 0;
    /// 0 = unselected, 1 = selected.
    std::vector<uint8_t> m_data;
    /// Number of pixels currently set to 1.
    int m_setCount = 0;
    BBox m_bbox;
};

#endif // PAINTSELECTIONMASK_H
