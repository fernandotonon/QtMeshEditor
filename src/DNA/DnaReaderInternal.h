#ifndef DNA_DNAREADERINTERNAL_H
#define DNA_DNAREADERINTERNAL_H

// Library-facing helpers shared by DnaReader.cpp and DnaRigLogic.cpp. Only
// included from translation units that already see the OpenRigLogic headers.

#ifdef ENABLE_OPENRIGLOGIC

#include "DnaReader.h"

#include <dna/BinaryStreamReader.h>

namespace Dna {
namespace detail {

inline dna::Configuration makeConfig(const ReadOptions& opt)
{
    dna::Configuration cfg;
    dna::DataLayer layer = dna::DataLayer::Definition;
    if (opt.loadGeometry)
        layer = layer | dna::DataLayer::Geometry;
    if (opt.loadBehavior)
        layer = layer | dna::DataLayer::RBFBehavior | dna::DataLayer::MachineLearnedBehavior
                      | dna::DataLayer::TwistSwingBehavior;
    cfg.layer = layer;
    cfg.unknownLayerPolicy = dna::UnknownLayerPolicy::Ignore;
    if (opt.convertCoordinateSystem) {
        // Target: right-handed, Y up. The library is asked for the file's own system
        // (x=left,y=up,z=front: a MetaHuman faces +Z there, left on +X) — an identity
        // transform. The default FACING (-Z, what this editor's viewport and Ogre-style
        // assets expect) is applied afterwards by yawRig on the root joints and meshes:
        // RigLogic's outputs are joint-LOCAL deltas in the file's frame, so a library-side
        // axis change would leave them inconsistent with the converted skeleton, whereas a
        // root yaw leaves every non-root local transform untouched.
        cfg.coordinateSystemTransformPolicy = dna::CoordinateSystemTransformPolicy::Transform;
        cfg.coordinateSystem = {tdm::axis_dir::left, tdm::axis_dir::up, tdm::axis_dir::front};
        cfg.rotationSequence = dna::RotationSequence::xyz;
        cfg.rotationSign = {dna::RotationDirection::positive, dna::RotationDirection::positive,
                            dna::RotationDirection::positive};
        cfg.faceWindingOrder = dna::FaceWindingOrder::ccw;
    }
    return cfg;
}

}  // namespace detail
}  // namespace Dna

#endif  // ENABLE_OPENRIGLOGIC
#endif  // DNA_DNAREADERINTERNAL_H
