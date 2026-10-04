#ifndef AUDIOTOFACE_A2FCOEFFICIENTS_H
#define AUDIOTOFACE_A2FCOEFFICIENTS_H

// Audio2Face (#1019): turn the network's raw output into a mesh deformation.
//
// The network emits 301 numbers per frame, and the layout is not obvious from
// the tensor alone. From the shipped `network_info.json` for the v2.3
// character:
//
//     [  0 .. 271]  272 skin PCA coefficients
//     [272 .. 281]   10 tongue PCA coefficients
//     [282 .. 296]   15 jaw values   (5 joints x xyz)
//     [297 .. 300]    4 eye values
//     -------------------------------------------------
//                    301
//
// Only the skin block feeds the blendshape solve. Reconstruction is a plain
// affine combination against the shipped basis:
//
//     vertexDeltas = shapesMean + Σ coeff[i] * shapesMatrix[i]
//
// `shapesMean` is load-bearing and easy to miss: it is the mean FACE, not
// zero, so dropping it leaves every frame offset by the average expression —
// a face that never returns to neutral between words.
//
// Pure data — no Ogre, no ONNX, no Qt. The basis arrives as plain float
// arrays, so the loader that unpacks NVIDIA's `.npz` stays separate and this
// is testable with a tiny synthetic basis.

#include <cstddef>
#include <vector>

namespace AudioToFace {

/// Sizes for one character's network, read from its `network_info.json`
/// rather than hardcoded — the diffusion model and the per-actor regression
/// models do not agree on these.
struct CoefficientLayout {
    int skinShapes   = 272;
    int tongueShapes = 10;
    int jawValues    = 15;
    int eyeValues    = 4;

    int total() const { return skinShapes + tongueShapes + jawValues + eyeValues; }
};

/// The PCA basis for one region (skin or tongue).
///
/// `matrix` is shapeCount consecutive blocks, each `valueCount` long
/// (vertexCount * 3, xyz interleaved) — the same order NVIDIA stores in
/// `model_data.npz`.
struct PcaBasis {
    int shapeCount = 0;
    size_t valueCount = 0;
    std::vector<float> mean;     ///< valueCount
    std::vector<float> matrix;   ///< shapeCount * valueCount

    bool valid() const {
        return shapeCount > 0 && valueCount > 0 &&
               mean.size() == valueCount &&
               matrix.size() == size_t(shapeCount) * valueCount;
    }
};

/// Reconstruct `mean + Σ coeff[i] * basis[i]`.
///
/// `coefficients` may be longer than `basis.shapeCount` — the caller passes
/// the whole 301-vector and an offset, so the skin and tongue blocks are read
/// in place without copying.
///
/// Returns empty on any size mismatch rather than a partial result: a short
/// read here would silently deform part of the face.
std::vector<float> reconstructPca(const PcaBasis& basis,
                                  const std::vector<float>& coefficients,
                                  int offset = 0);

/// Convenience: the skin block of a full network output.
inline std::vector<float> reconstructSkin(const PcaBasis& skin,
                                          const std::vector<float>& networkOutput,
                                          const CoefficientLayout& layout = {})
{
    if (int(networkOutput.size()) < layout.total()) return {};
    return reconstructPca(skin, networkOutput, 0);
}

/// Convenience: the tongue block, which starts after the skin coefficients.
inline std::vector<float> reconstructTongue(const PcaBasis& tongue,
                                            const std::vector<float>& networkOutput,
                                            const CoefficientLayout& layout = {})
{
    if (int(networkOutput.size()) < layout.total()) return {};
    return reconstructPca(tongue, networkOutput, layout.skinShapes);
}

}  // namespace AudioToFace

#endif  // AUDIOTOFACE_A2FCOEFFICIENTS_H
