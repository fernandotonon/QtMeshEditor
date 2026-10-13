#ifndef BODYROOTMOTION_H
#define BODYROOTMOTION_H

#ifdef ENABLE_MOCAP
#include "MocapLiveTypes.h"
#include "OneEuroFilter.h"
#include <OgreVector3.h>
#include <array>
#include <cmath>

// Weak-perspective camera translation. MediaPipe's world landmarks are
// hip-centred: they contain articulation but no global displacement. Fit
// pixels/metre to the projected torso and use image-space hip position plus
// scale change to estimate movement. Depth is approximate without intrinsics.
class BodyRootMotion {
public:
    bool calibrate(const BodyLiveFrame& frame)
    {
        Ogre::Vector3 position;
        if (!cameraPosition(frame, position))
            return false;
        m_reference = position;
        m_last = Ogre::Vector3::ZERO;
        m_ready = true;
        m_filters = {};
        return true;
    }

    Ogre::Vector3 evaluate(const BodyLiveFrame& frame)
    {
        if (!m_ready) {
            calibrate(frame);
            return Ogre::Vector3::ZERO;
        }
        Ogre::Vector3 position;
        // Occlusion holds position; it must not snap the actor to origin.
        if (!cameraPosition(frame, position))
            return m_last;
        position -= m_reference;
        for (int axis = 0; axis < 3; ++axis)
            position[axis] = static_cast<float>(m_filters[axis].filter(position[axis], frame.timeSec));
        m_last = position;
        return position;
    }

private:
    static bool cameraPosition(const BodyLiveFrame& f, Ogre::Vector3& out)
    {
        if (!f.valid || f.imageWidth <= 0 || f.imageHeight <= 0)
            return false;
        for (int lm : {11, 12, 23, 24}) {
            if (!(f.visibility[lm] >= 0.3f)) return false;
            for (int axis = 0; axis < 3; ++axis)
                if (!std::isfinite(f.world[lm * 3 + axis])) return false;
            if (!std::isfinite(f.imageXy[lm * 2]) || !std::isfinite(f.imageXy[lm * 2 + 1]))
                return false;
        }
        double numerator = 0.0, denominator = 0.0;
        for (const auto& pair : {std::array<int, 2>{11, 23}, {12, 24}, {11, 12}, {23, 24}}) {
            for (int axis = 0; axis < 2; ++axis) {
                const double world = f.world[pair[0] * 3 + axis] - f.world[pair[1] * 3 + axis];
                const double image = f.imageXy[pair[0] * 2 + axis] - f.imageXy[pair[1] * 2 + axis];
                numerator += world * image;
                denominator += world * world;
            }
        }
        if (denominator < 1e-5) return false;
        const double scale = numerator / denominator;
        // Reject collapsed/mismatched projections, not valid body motion.
        if (!std::isfinite(scale) || scale < 10.0 || scale > 10000.0) return false;
        const double hipX = (f.imageXy[46] + f.imageXy[48]) * 0.5 - f.imageWidth * 0.5;
        const double hipY = (f.imageXy[47] + f.imageXy[49]) * 0.5 - f.imageHeight * 0.5;
        // A fixed nominal focal length is sufficient for relative movement;
        // calibrated camera intrinsics would improve metric depth accuracy.
        out = Ogre::Vector3(hipX / scale, -hipY / scale, -f.imageWidth / scale);
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }

    bool m_ready = false;
    Ogre::Vector3 m_reference = Ogre::Vector3::ZERO;
    Ogre::Vector3 m_last = Ogre::Vector3::ZERO;
    std::array<OneEuroFilter, 3> m_filters{};
};
#endif
#endif
