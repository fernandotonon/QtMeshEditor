// Named body measurements for the 45 MHR identity modes. The modes themselves are anonymous PCA axes; this module measures the
// shaped body (height, leg length, waist ...) and linearises those measurements over the modes, so the UI can offer sliders that
// change ONE named thing (and leave the others alone) and the raw modes can be labelled by what they really do.
// Pure data: it reads the Assets (mean body, shape vectors, skinning) and nothing else.
#pragma once

#include "CharacterModel.h"

#include <array>
#include <vector>

namespace CharacterGen {
namespace BodyShape {

enum Measure {
    Height = 0,
    LegLength,      ///< floor to crotch
    ArmSpan,        ///< hand to hand
    ShoulderWidth,  ///< between the collar bones' outer ends
    ChestWidth,
    WaistWidth,
    HipsWidth,
    ChestDepth,     ///< front to back at the bust
    BellyDepth,     ///< front to back at the waist
    NeckLength,
    HeadHeight,
    HeadWidth,
    MeasureCount
};

struct Info {
    const char* id;      ///< stable key (CLI / MCP)
    const char* label;   ///< slider label
    const char* hint;    ///< tooltip
    float range;         ///< cm each side of the mean offered by the slider
};
const Info& info(Measure m);

class Model {
public:
    /// Classify the vertices by their dominant joint and linearise the measurements over the shape modes. False when the assets lack
    /// what is needed (then every other call returns zeros / the input unchanged).
    bool build(const Assets& a);
    bool valid() const { return m_valid; }

    /// The measurements (cm) of the body shaped by `coefficients` (exact, from the vertices).
    std::array<float, MeasureCount> measure(const std::vector<float>& coefficients) const;
    /// The measurements (cm) of arbitrary body positions (3 floats per vertex, cm, the MHR vertex order).
    std::array<float, MeasureCount> measurePositions(const std::vector<float>& positions) const { return m_valid && positions.size() == m_base.size() ? measureShaped(positions) : std::array<float, MeasureCount>{}; }
    /// Offset (cm) of one measurement from the mean body under the LINEAR model: what the sliders show.
    float offset(Measure m, const std::vector<float>& coefficients) const;
    /// Coefficients whose offset of `m` is `target` cm while every other measurement keeps its current offset: the smallest change
    /// of the coefficients (ridge-regularised, each clamped to +-limit) that does it.
    std::vector<float> withOffset(Measure m, float target, const std::vector<float>& coefficients, float limit = 3.0f) const;

    /// Linearised effect (cm per unit coefficient) of mode `s` on measurement `m`.
    float effect(Measure m, int mode) const { return m_valid ? m_J[size_t(m) * size_t(m_shapes) + size_t(mode)] : 0.0f; }
    int shapes() const { return m_shapes; }
    const std::array<float, MeasureCount>& mean() const { return m_mean; }

private:
    bool m_valid = false;
    int m_shapes = 0, m_vertices = 0;
    const Assets* m_assets = nullptr;
    std::vector<float> m_base;
    std::vector<float> m_vectors;
    std::vector<uint8_t> m_class;   ///< per vertex: 0 torso/pelvis, 1 leg, 2 arm, 3 collar bone, 4 neck, 5 head
    std::array<float, MeasureCount> m_mean{};
    std::vector<float> m_J;         ///< MeasureCount x shapes
    std::array<float, MeasureCount> measureShaped(const std::vector<float>& positions) const;
};

/// Fit the generator to a target body (SAM 3D Body's neutral mesh, cm, the MHR vertex order): the widths that the identity modes can
/// reach (chest, waist, hips, belly, bust) are matched by changing the coefficients as little as possible, and what they cannot (height,
/// leg / arm / torso length, shoulders, head size) goes into the proportions. Factors are limited to `maxChange` around 1.
struct BodyFit {
    bool ok = false;
    std::vector<float> coefficients;
    Proportions proportions;
};
BodyFit fitToBody(const Model& model, const Assets& assets, const std::vector<float>& targetCm, const std::vector<float>& coefficients, float maxChange = 0.15f);

/// Root-mean-square distance (cm) between two vertex arrays of the same layout (3 floats per vertex); -1 when the sizes differ.
float rmsDistance(const std::vector<float>& a, const std::vector<float>& b);

}  // namespace BodyShape
}  // namespace CharacterGen
