#include "DnaFit.h"
#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

#ifdef ENABLE_OPENRIGLOGIC
#include "DnaReaderInternal.h"

#include <dna/BinaryStreamWriter.h>
#include <dna/Writer.h>
#include <trio/streams/FileStream.h>

#include <QFile>
#include <QFileInfo>

#include <cmath>
#endif

namespace Dna {
namespace Fit {

#ifndef ENABLE_OPENRIGLOGIC

WriteResult writeFittedDna(const QString&, const Rig&, const Rig&, const Warp&, const QString&)
{
    WriteResult r;
    r.error = QStringLiteral("MetaHuman DNA support is not compiled into this build. "
                             "Rebuild QtMeshEditor with -DENABLE_OPENRIGLOGIC=ON.");
    return r;
}

#else

namespace {

// converted = s * native, per axis, derived from matching data (never assumed). The
// importer's frame differs from the file's by a sign flip per axis and the unit scale.
struct AxisMap {
    std::array<double, 3> s{1, 1, 1};
    double residual = 0;   // worst relative disagreement after applying the map
    bool ok = false;
    std::array<bool, 3> assumed{false, false, false};   // axis carried no data: scale assumed, sign +
};

// `fallbackScale` is used for an axis whose native data is all zero (nothing to learn a
// sign from) — real DNA carries data on every axis, tiny synthetic ones may not.
AxisMap deriveAxisMap(const std::vector<std::array<double, 3>>& converted, const std::vector<std::array<double, 3>>& native,
                      double fallbackScale = 0.0)
{
    AxisMap m;
    if (converted.size() != native.size() || converted.empty()) return m;
    for (int a = 0; a < 3; ++a) {
        double num = 0, den = 0;
        for (size_t i = 0; i < converted.size(); ++i) {
            num += converted[i][size_t(a)] * native[i][size_t(a)];
            den += native[i][size_t(a)] * native[i][size_t(a)];
        }
        if (den < 1e-18) {
            if (fallbackScale <= 0) return m;
            m.s[size_t(a)] = fallbackScale;
            m.assumed[size_t(a)] = true;
            continue;
        }
        m.s[size_t(a)] = num / den;
    }
    double worst = 0, scale = 0;
    for (size_t i = 0; i < converted.size(); ++i)
        for (int a = 0; a < 3; ++a) {
            worst = std::max(worst, std::abs(converted[i][size_t(a)] - m.s[size_t(a)] * native[i][size_t(a)]));
            scale = std::max(scale, std::abs(converted[i][size_t(a)]));
        }
    m.residual = scale > 0 ? worst / scale : 0.0;
    m.ok = m.residual < 1e-4;
    return m;
}

}  // namespace

WriteResult writeFittedDna(const QString& sourcePath, const Rig& original, const Rig& fitted, const Warp& warp,
                           const QString& outPath)
{
    WriteResult r;
    auto fail = [&](const QString& m) { r.error = m; return r; };
    if (original.joints.size() != fitted.joints.size() || original.meshes.size() != fitted.meshes.size())
        return fail(QStringLiteral("the fitted rig does not match the original (joint / mesh counts differ)"));
    if (!warp.valid) return fail(QStringLiteral("no valid warp"));
    const QFileInfo src(sourcePath);
    if (!src.exists()) return fail(QStringLiteral("source DNA not found: %1").arg(sourcePath));
    {
        // same file by identity, not by spelling: symlinks, hard links, and case-only aliases
        const QFileInfo dst(outPath);
        const bool sameSpelling = dst.absoluteFilePath() == src.absoluteFilePath();
        const bool sameCanonical = dst.exists() && dst.canonicalFilePath() == src.canonicalFilePath();
        bool sameInode = false;
#ifndef Q_OS_WIN
        struct stat a{}, b{};
        const QByteArray sa = QFile::encodeName(src.absoluteFilePath()), sb = QFile::encodeName(dst.absoluteFilePath());
        sameInode = dst.exists() && ::stat(sa.constData(), &a) == 0 && ::stat(sb.constData(), &b) == 0 &&
                    a.st_dev == b.st_dev && a.st_ino == b.st_ino;
#endif
        if (sameSpelling || sameCanonical || sameInode)
            return fail(QStringLiteral("refusing to overwrite the source DNA — choose another output path"));
    }

    // The source, in its OWN frame and units (no coordinate transform).
    const QByteArray sp = QFile::encodeName(src.absoluteFilePath());
    auto sstream = pma::makeScoped<dna::FileStream>(sp.constData(), dna::FileStream::AccessMode::Read,
                                                    dna::FileStream::OpenMode::Binary);
    dna::Configuration cfg;
    cfg.layer = dna::DataLayer::All;
    cfg.unknownLayerPolicy = dna::UnknownLayerPolicy::Preserve;
    auto reader = pma::makeScoped<dna::BinaryStreamReader>(sstream.get(), cfg);
    reader->read();
    if (!sc::Status::isOk())
        return fail(QStringLiteral("could not read %1: %2").arg(src.fileName(), QString::fromUtf8(sc::Status::get().message)));

    // ---- joints: derive converted<->native per axis from the neutral translations ----
    const std::uint16_t jc = reader->getJointCount();
    if (size_t(jc) != original.joints.size())
        return fail(QStringLiteral("the source has %1 joints, the rig has %2").arg(jc).arg(original.joints.size()));
    std::vector<std::array<double, 3>> jConv, jNat;
    for (std::uint16_t j = 0; j < jc; ++j) {
        const dna::Vector3 t = reader->getNeutralJointTranslation(j);
        jNat.push_back({t.x, t.y, t.z});
        jConv.push_back(original.joints[j].translation);
    }
    const AxisMap jm = deriveAxisMap(jConv, jNat, fitted.unitScale[0]);
    if (!jm.ok)
        return fail(QStringLiteral("cannot map the importer's joint frame back to the file's (residual %1) — an unsupported "
                                   "coordinate convention").arg(jm.residual));
    if (jm.assumed[0] || jm.assumed[1] || jm.assumed[2])
        r.notes << QStringLiteral("some joint-translation axes carry no data in this DNA; their sign was assumed positive");
    std::vector<dna::Vector3> newT(jc);
    for (std::uint16_t j = 0; j < jc; ++j) {
        const auto& f = fitted.joints[j].translation;
        newT[j] = {float(f[0] / jm.s[0]), float(f[1] / jm.s[1]), float(f[2] / jm.s[2])};
    }

    // ---- meshes: every mesh of the file, every LOD ----
    const std::uint16_t mc = reader->getMeshCount();
    std::vector<std::vector<dna::Position>> newPos(mc);
    std::vector<char> rewritten(mc, 0);
    for (size_t m = 0; m < original.meshes.size(); ++m) {
        const int k = original.meshes[m].dnaIndex;
        if (k < 0 || k >= mc) continue;
        const auto xs = reader->getVertexPositionXs(std::uint16_t(k));
        const auto ys = reader->getVertexPositionYs(std::uint16_t(k));
        const auto zs = reader->getVertexPositionZs(std::uint16_t(k));
        std::vector<std::array<double, 3>> nat, conv;
        for (size_t i = 0; i < xs.size() && (i + 1) * 3 <= original.meshes[m].positions.size(); ++i) {
            nat.push_back({xs[i], ys[i], zs[i]});
            conv.push_back({original.meshes[m].positions[i * 3], original.meshes[m].positions[i * 3 + 1],
                            original.meshes[m].positions[i * 3 + 2]});
        }
        const AxisMap pm = deriveAxisMap(conv, nat, fitted.unitScale[0]);
        if (!pm.ok)
            return fail(QStringLiteral("cannot map mesh '%1' back to the file's frame (residual %2)")
                            .arg(original.meshes[m].name).arg(pm.residual));
        if (nat.size() != fitted.meshes[m].positions.size() / 3)
            return fail(QStringLiteral("mesh '%1' changed vertex count").arg(original.meshes[m].name));
        auto& out = newPos[size_t(k)];
        for (size_t i = 0; i < nat.size(); ++i) {
            const float* f = &fitted.meshes[m].positions[i * 3];
            out.push_back({float(f[0] / pm.s[0]), float(f[1] / pm.s[1]), float(f[2] / pm.s[2])});
        }
        rewritten[size_t(k)] = 1;
        ++r.meshesWritten;
    }
    // Meshes the importer did not load (lower LODs): the SAME warp moves them. Their
    // converted frame uses the mapping learnt from LOD 0.
    AxisMap frame;
    for (size_t m = 0; m < original.meshes.size() && !frame.ok; ++m) {
        const int k = original.meshes[m].dnaIndex;
        if (k < 0 || k >= mc) continue;
        const auto xs = reader->getVertexPositionXs(std::uint16_t(k));
        const auto ys = reader->getVertexPositionYs(std::uint16_t(k));
        const auto zs = reader->getVertexPositionZs(std::uint16_t(k));
        std::vector<std::array<double, 3>> nat, conv;
        for (size_t i = 0; i < xs.size(); ++i) {
            nat.push_back({xs[i], ys[i], zs[i]});
            conv.push_back({original.meshes[m].positions[i * 3], original.meshes[m].positions[i * 3 + 1],
                            original.meshes[m].positions[i * 3 + 2]});
        }
        frame = deriveAxisMap(conv, nat, fitted.unitScale[0]);
    }
    for (std::uint16_t k = 0; k < mc; ++k) {
        if (rewritten[k]) continue;
        if (!frame.ok) continue;
        const auto xs = reader->getVertexPositionXs(k);
        const auto ys = reader->getVertexPositionYs(k);
        const auto zs = reader->getVertexPositionZs(k);
        auto& out = newPos[k];
        for (size_t i = 0; i < xs.size(); ++i) {
            const std::array<double, 3> c{xs[i] * frame.s[0], ys[i] * frame.s[1], zs[i] * frame.s[2]};
            const auto d = warp.displacement(c);
            out.push_back({float((c[0] + d[0]) / frame.s[0]), float((c[1] + d[1]) / frame.s[1]),
                           float((c[2] + d[2]) / frame.s[2])});
        }
        rewritten[k] = 1;
        ++r.meshesWritten;
        ++r.lodMeshesWarped;
    }

    // ---- write ----
    const QByteArray op = QFile::encodeName(QFileInfo(outPath).absoluteFilePath());
    auto ostream = pma::makeScoped<dna::FileStream>(op.constData(), dna::FileStream::AccessMode::Write,
                                                    dna::FileStream::OpenMode::Binary);
    auto writer = pma::makeScoped<dna::BinaryStreamWriter>(ostream.get());
    writer->setFrom(reader.get());
    writer->setNeutralJointTranslations(newT.data(), jc);
    for (std::uint16_t k = 0; k < mc; ++k)
        if (rewritten[k] && !newPos[k].empty())
            writer->setVertexPositions(k, newPos[k].data(), std::uint32_t(newPos[k].size()));
    writer->write();
    if (!sc::Status::isOk())
        return fail(QStringLiteral("writing %1 failed: %2").arg(outPath, QString::fromUtf8(sc::Status::get().message)));

    r.notes << QStringLiteral("Serialised the fitted skeleton and bind geometry; behaviour layers (RBF, joint groups, "
                              "neural nets) are copied unchanged — this is NOT a recalibration, so corrective data "
                              "still assumes the original proportions.")
            << QStringLiteral("Vertex normals are copied unchanged from the source.");
    r.ok = true;
    return r;
}

#endif  // ENABLE_OPENRIGLOGIC

}  // namespace Fit
}  // namespace Dna
