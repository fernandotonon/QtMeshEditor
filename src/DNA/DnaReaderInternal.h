#ifndef DNA_DNAREADERINTERNAL_H
#define DNA_DNAREADERINTERNAL_H

// Library-facing helpers shared by DnaReader.cpp and DnaRigLogic.cpp. Only
// included from translation units that already see the OpenRigLogic headers.

#ifdef ENABLE_OPENRIGLOGIC

#include "DnaReader.h"

#include <dna/BinaryStreamReader.h>
#include <dna/BinaryStreamWriter.h>
#include <dna/JSONStreamReader.h>
#include <status/Status.h>
#include <trio/streams/FileStream.h>
#include <trio/streams/MemoryStream.h>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>

namespace Dna {
namespace detail {

/// True when the file is a JSON DNA (the library's text format): a `.json` extension, or a first
/// non-blank byte of '{'.
inline bool isJsonDnaFile(const QString& path)
{
    if (path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) return true;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(64);
    for (char c : head) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return c == '{';
    }
    return false;
}

/// Read a JSON DNA and re-serialise it as a binary DNA in `out` (positioned at 0). The library applies
/// its unit / coordinate-system / rotation conversions only when READING BINARY, so routing JSON through
/// a binary stream gives it exactly the same conventions as every other DNA with no second code path.
/// Returns an empty string on success, else a user-facing error.
inline QString jsonToBinaryStream(const QString& path, dna::MemoryStream& out)
{
    const QByteArray p = QFile::encodeName(QFileInfo(path).absoluteFilePath());
    auto in = pma::makeScoped<dna::FileStream>(p.constData(), dna::FileStream::AccessMode::Read,
                                               dna::FileStream::OpenMode::Binary);
    if (!in) return QStringLiteral("Could not open %1.").arg(QFileInfo(path).fileName());
    auto jr = pma::makeScoped<dna::JSONStreamReader>(in.get());
    if (!jr) return QStringLiteral("Could not create a JSON DNA reader.");
    jr->read();
    if (!sc::Status::isOk())
        return QStringLiteral("%1 is not a valid JSON DNA: %2").arg(QFileInfo(path).fileName(), QString::fromUtf8(sc::Status::get().message));
    auto bw = pma::makeScoped<dna::BinaryStreamWriter>(&out);
    if (!bw) return QStringLiteral("Could not create a DNA writer.");
    bw->setFrom(jr.get());
    bw->write();
    if (!sc::Status::isOk())
        return QStringLiteral("Could not convert %1 from JSON: %2").arg(QFileInfo(path).fileName(), QString::fromUtf8(sc::Status::get().message));
    out.seek(0);
    return QString();
}

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
