#include "Gp0CaptureStats.h"

QString Gp0CaptureStats::primarySourceLabel() const
{
    switch (primarySource) {
    case Gp0CaptureSource::InCoreHook:
        return QStringLiteral("gp0_incore");
    case Gp0CaptureSource::DirectHook:
        return QStringLiteral("gp0_hook");
    case Gp0CaptureSource::RamOrderingTable:
        return QStringLiteral("ram_ot");
    case Gp0CaptureSource::RamLinear:
        return QStringLiteral("ram_linear");
    case Gp0CaptureSource::RamChainRoot:
        return QStringLiteral("ram_chain_root");
    case Gp0CaptureSource::RamModelMesh:
        return QStringLiteral("ram_model_mesh");
    case Gp0CaptureSource::None:
    default:
        return QStringLiteral("none");
    }
}
