#include "A2FCoefficients.h"

namespace AudioToFace {

std::vector<float> reconstructPca(const PcaBasis& basis,
                                  const std::vector<float>& coefficients,
                                  int offset)
{
    if (!basis.valid() || offset < 0) return {};
    // The caller may hand us the whole 301-vector; it only has to contain the
    // block we were asked for. A short read would deform part of the face and
    // leave the rest at the mean, which looks like a rig fault rather than a
    // data fault — so refuse instead.
    if (coefficients.size() < size_t(offset) + size_t(basis.shapeCount)) return {};

    std::vector<float> out = basis.mean;   // NOT zero: the mean is a real face
    for (int s = 0; s < basis.shapeCount; ++s) {
        const float c = coefficients[size_t(offset) + size_t(s)];
        if (c == 0.0f) continue;           // common early in a take; skips a
                                           // full pass over ~184k floats
        const float* row = &basis.matrix[size_t(s) * basis.valueCount];
        for (size_t k = 0; k < basis.valueCount; ++k) out[k] += c * row[k];
    }
    return out;
}

}  // namespace AudioToFace
