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
        // Target: right-handed, Y up, character facing +Z (the glTF / Mixamo
        // convention the turntable and exporters expect). DNA names the
        // into-screen direction "front", and a MetaHuman faces "front" in its
        // authored x=right,y=up,z=back system — i.e. toward -Z — so the target
        // is x=left,y=up,z=front: a proper 180-degree turn about Y, still
        // right-handed (x=right,y=up,z=front would be a mirror).
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
