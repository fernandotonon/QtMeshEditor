#ifndef BODYPOSESTREAM_H
#define BODYPOSESTREAM_H
#ifdef ENABLE_MOCAP
#include "MocapLiveTypes.h"
#include "OneEuroFilter.h"
#include "PoseCapPredictor.h"
#include <algorithm>
#include <cmath>

// Shared camera / video / MCP input path. Smooth geometry once, then solve;
// independently smoothing quaternions and landmarks gave different poses.
class BodyPoseStream {
public:
    void setSmoothing(double cutoff, bool enabled = true)
    {
        m_smooth = enabled;
        OneEuroFilter::Params params;
        params.minCutoff = std::max(0.5, cutoff);
        params.beta = 0.5;
        params.dCutoff = 0.8;
        for (auto& lm : m_worldFilters)
            for (auto& axis : lm) axis = OneEuroFilter(params);
        for (auto& lm : m_imageFilters)
            for (auto& axis : lm) axis = OneEuroFilter(params);
    }

    void reset()
    {
        m_solver.reset();
        for (auto& lm : m_worldFilters)
            for (auto& axis : lm) axis.reset();
        for (auto& lm : m_imageFilters)
            for (auto& axis : lm) axis.reset();
    }

    BodyLiveFrame process(const PoseSample& sample, int width, int height)
    {
        BodyLiveFrame body;
        body.timeSec = sample.timeSec;
        body.imageWidth = width;
        body.imageHeight = height;
        if (!(sample.confidence > 0.f)) return body;
        body.valid = true;
        body.world = sample.world;
        body.screenCrop = sample.screenCrop;
        body.imageXy = sample.imageXy;
        body.visibility = sample.visibility;
        for (int lm = 0; lm < PoseIK::kLandmarkCount; ++lm) {
            for (int axis = 0; axis < 3; ++axis)
                if (!std::isfinite(body.world[lm * 3 + axis]))
                    body.visibility[lm] = 0.f;
            if (!(body.visibility[lm] >= 0.3f) || !m_smooth) continue;
            // Pose finger world landmarks barely move; hand inference uses
            // crop-space landmarks separately.
            if (lm < 17 || lm > 22)
                for (int axis = 0; axis < 3; ++axis)
                    body.world[lm * 3 + axis] = static_cast<float>(
                        m_worldFilters[lm][axis].filter(body.world[lm * 3 + axis], sample.timeSec));
            if (width > 0 && height > 0)
                for (int axis = 0; axis < 2; ++axis) {
                    const float dimension = static_cast<float>(axis == 0 ? width : height);
                    const float coordinate = body.imageXy[lm * 2 + axis];
                    if (std::isfinite(coordinate))
                        body.imageXy[lm * 2 + axis] = static_cast<float>(
                            m_imageFilters[lm][axis].filter(coordinate / dimension, sample.timeSec)) * dimension;
                }
        }
        const auto result = m_solver.solveFrame(body.world.data(), body.visibility.data());
        body.quats = result.quats;
        body.resolvedMask = result.resolvedMask;
        return body;
    }

private:
    bool m_smooth = true;
    PoseIK::Solver m_solver;
    std::array<std::array<OneEuroFilter, 3>, PoseIK::kLandmarkCount> m_worldFilters{};
    std::array<std::array<OneEuroFilter, 2>, PoseIK::kLandmarkCount> m_imageFilters{};
};
#endif
#endif
