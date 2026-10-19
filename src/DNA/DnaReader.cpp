#include "DnaReader.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

#ifdef ENABLE_OPENRIGLOGIC
#include <dna/BinaryStreamReader.h>
#include <dna/Reader.h>
#include <status/Status.h>
#include <trio/streams/FileStream.h>
#include <trio/streams/MemoryStream.h>
#include "DnaReaderInternal.h"
#endif

namespace Dna {

// ---------------------------------------------------------------------------
// Pure helpers — always compiled
// ---------------------------------------------------------------------------

namespace {

using Quat = std::array<double, 4>;  // w, x, y, z

Quat axisQuat(char axis, double radians, int sign)
{
    const double h = 0.5 * radians * sign;
    const double s = std::sin(h), c = std::cos(h);
    switch (axis) {
    case 'x': return {c, s, 0, 0};
    case 'y': return {c, 0, s, 0};
    default:  return {c, 0, 0, s};
    }
}

Quat mul(const Quat& a, const Quat& b)
{
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
            a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
            a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}

Quat conj(const Quat& q) { return {q[0], -q[1], -q[2], -q[3]}; }

Quat normalised(Quat q)
{
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-12)
        return {1, 0, 0, 0};
    for (double& v : q) v /= n;
    return q;
}

std::array<double, 3> rotate(const Quat& q, const std::array<double, 3>& v)
{
    const Quat p{0, v[0], v[1], v[2]};
    const Quat r = mul(mul(q, p), conj(q));
    return {r[1], r[2], r[3]};
}

}  // namespace

std::array<double, 4> eulerToQuaternion(const std::array<double, 3>& angles,
                                        const QString& sequence,
                                        bool degrees,
                                        const std::array<int, 3>& signs)
{
    const double k = degrees ? 3.14159265358979323846 / 180.0 : 1.0;
    const QString seq = sequence.toLower();
    Quat q{1, 0, 0, 0};
    // Fall back to xyz for an unrecognised sequence: the caller surfaces the
    // authored string in the descriptor, and identity-with-wrong-order beats a
    // refusal to import a character at all.
    const QString order = (seq.size() == 3) ? seq : QStringLiteral("xyz");
    // Maya/RigLogic "xyz" means rotate about X first, then Y, then Z. With
    // column vectors that is Rz*Ry*Rx, so each later axis PRE-multiplies.
    for (const QChar ch : order) {
        const int a = (ch == 'x') ? 0 : (ch == 'y') ? 1 : 2;
        q = mul(axisQuat(ch.toLatin1(), angles[a] * k, signs[a]), q);
    }
    return normalised(q);
}

void yawRig(Rig& rig, double radians)
{
    const double c = std::cos(radians), sn = std::sin(radians);
    auto rot = [&](float* p) {
        const double x = p[0], z = p[2];
        p[0] = float(x * c + z * sn);
        p[2] = float(-x * sn + z * c);
    };
    const std::array<double, 4> qy{std::cos(radians / 2), 0, std::sin(radians / 2), 0};
    for (Joint& j : rig.joints) {
        if (j.parent >= 0) continue;
        const double x = j.translation[0], z = j.translation[2];
        j.translation[0] = x * c + z * sn;
        j.translation[2] = -x * sn + z * c;
        const auto& q = j.orientation;
        j.orientation = {qy[0] * q[0] - qy[1] * q[1] - qy[2] * q[2] - qy[3] * q[3], qy[0] * q[1] + qy[1] * q[0] + qy[2] * q[3] - qy[3] * q[2],
                         qy[0] * q[2] - qy[1] * q[3] + qy[2] * q[0] + qy[3] * q[1], qy[0] * q[3] + qy[1] * q[2] - qy[2] * q[1] + qy[3] * q[0]};
    }
    for (Mesh& m : rig.meshes) {
        for (size_t i = 0; i + 2 < m.positions.size(); i += 3) rot(&m.positions[i]);
        for (size_t i = 0; i + 2 < m.normals.size(); i += 3) rot(&m.normals[i]);
        for (auto& bs : m.blendShapeTargets)
            for (size_t i = 0; i + 2 < bs.deltas.size(); i += 3) rot(&bs.deltas[i]);
    }
}

bool worldToLocal(std::vector<Joint>& joints)
{
    const size_t n = joints.size();
    // Process parents before children without assuming file order.
    std::vector<int> state(n, 0);  // 0 unvisited, 1 visiting, 2 done
    std::vector<size_t> order;
    order.reserve(n);
    bool cycle = false;
    std::function<void(size_t)> visit = [&](size_t i) {
        if (state[i] == 2 || cycle) return;
        if (state[i] == 1) { cycle = true; return; }
        state[i] = 1;
        const int p = joints[i].parent;
        if (p >= 0 && static_cast<size_t>(p) < n && static_cast<size_t>(p) != i)
            visit(static_cast<size_t>(p));
        state[i] = 2;
        order.push_back(i);
    };
    for (size_t i = 0; i < n; ++i) visit(i);
    if (cycle)
        return false;

    std::vector<std::array<double, 3>> wt(n);
    std::vector<Quat> wq(n);
    for (size_t i = 0; i < n; ++i) {
        wt[i] = joints[i].translation;
        wq[i] = normalised(joints[i].orientation);
    }
    for (const size_t i : order) {
        const int p = joints[i].parent;
        if (p < 0 || static_cast<size_t>(p) >= n || static_cast<size_t>(p) == i) {
            joints[i].translation = wt[i];
            joints[i].orientation = wq[i];
            continue;
        }
        const size_t pi = static_cast<size_t>(p);
        const Quat pInv = conj(wq[pi]);
        const std::array<double, 3> d{wt[i][0] - wt[pi][0], wt[i][1] - wt[pi][1], wt[i][2] - wt[pi][2]};
        joints[i].translation = rotate(pInv, d);
        joints[i].orientation = normalised(mul(pInv, wq[i]));
    }
    return true;
}

ExpandedMesh expand(const Mesh& mesh, bool flipV)
{
    ExpandedMesh out;
    const size_t nPos = mesh.positions.size() / 3;
    const size_t nNrm = mesh.normals.size() / 3;
    const size_t nUv = mesh.uvs.size() / 2;

    auto layoutValid = [&](const Mesh::Layout& l) {
        return l.position < nPos && (nNrm == 0 || l.normal < nNrm) && (nUv == 0 || l.uv < nUv);
    };

    out.positions.reserve(mesh.layouts.size() * 3);
    out.normals.reserve(mesh.layouts.size() * 3);
    out.uvs.reserve(mesh.layouts.size() * 2);
    out.positionOf.reserve(mesh.layouts.size());
    for (const Mesh::Layout& l : mesh.layouts) {
        // An out-of-range layout becomes a degenerate vertex rather than UB;
        // faces that reference it are dropped below.
        const bool ok = layoutValid(l);
        const size_t p = ok ? l.position : 0;
        for (int c = 0; c < 3; ++c)
            out.positions.push_back(nPos ? mesh.positions[p * 3 + c] : 0.0f);
        if (nNrm && ok) {
            for (int c = 0; c < 3; ++c) out.normals.push_back(mesh.normals[l.normal * 3 + c]);
        } else {
            out.normals.insert(out.normals.end(), {0.0f, 1.0f, 0.0f});
        }
        if (nUv && ok) {
            const float u = mesh.uvs[l.uv * 2], v = mesh.uvs[l.uv * 2 + 1];
            out.uvs.push_back(u);
            out.uvs.push_back(flipV ? 1.0f - v : v);
        } else {
            out.uvs.insert(out.uvs.end(), {0.0f, 0.0f});
        }
        out.positionOf.push_back(static_cast<uint32_t>(p));
    }

    if (!mesh.skin.empty()) {
        out.skin.resize(mesh.layouts.size());
        for (size_t v = 0; v < mesh.layouts.size(); ++v) {
            const uint32_t p = out.positionOf[v];
            if (p < mesh.skin.size())
                out.skin[v] = mesh.skin[p];
        }
    }

    const size_t nLayouts = mesh.layouts.size();
    for (const auto& face : mesh.faces) {
        bool bad = face.size() < 3;
        for (const uint32_t idx : face) {
            if (idx >= nLayouts || !layoutValid(mesh.layouts[idx])) { bad = true; break; }
        }
        if (bad) { ++out.skippedFaces; continue; }
        for (size_t i = 1; i + 1 < face.size(); ++i) {
            out.indices.push_back(face[0]);
            out.indices.push_back(face[i]);
            out.indices.push_back(face[i + 1]);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

#ifndef ENABLE_OPENRIGLOGIC

bool available() { return false; }

static ReadResult unavailable()
{
    ReadResult r;
    r.code = ErrorCode::Unavailable;
    r.error = QStringLiteral(
        "MetaHuman DNA support is not compiled into this build. "
        "Rebuild QtMeshEditor with -DENABLE_OPENRIGLOGIC=ON.");
    return r;
}

ReadResult read(const QString&, const ReadOptions&) { return unavailable(); }
ReadResult readFromBuffer(const std::vector<uint8_t>&, const ReadOptions&) { return unavailable(); }

#else  // ENABLE_OPENRIGLOGIC

bool available() { return true; }

namespace {

QString sv(const av::StringView& s) { return QString::fromUtf8(s.c_str()); }

QString axisName(tdm::axis_dir d)
{
    switch (d) {
    case tdm::axis_dir::left:  return QStringLiteral("left");
    case tdm::axis_dir::right: return QStringLiteral("right");
    case tdm::axis_dir::up:    return QStringLiteral("up");
    case tdm::axis_dir::down:  return QStringLiteral("down");
    case tdm::axis_dir::front: return QStringLiteral("front");
    case tdm::axis_dir::back:  return QStringLiteral("back");
    }
    return QStringLiteral("?");
}

QString seqName(tdm::rot_seq s)
{
    switch (s) {
    case tdm::rot_seq::xyz: return QStringLiteral("xyz");
    case tdm::rot_seq::xzy: return QStringLiteral("xzy");
    case tdm::rot_seq::yxz: return QStringLiteral("yxz");
    case tdm::rot_seq::yzx: return QStringLiteral("yzx");
    case tdm::rot_seq::zxy: return QStringLiteral("zxy");
    case tdm::rot_seq::zyx: return QStringLiteral("zyx");
    }
    return QStringLiteral("xyz");
}

QString archetypeName(dna::Archetype a)
{
    switch (a) {
    case dna::Archetype::asian:     return QStringLiteral("asian");
    case dna::Archetype::black:     return QStringLiteral("black");
    case dna::Archetype::caucasian: return QStringLiteral("caucasian");
    case dna::Archetype::hispanic:  return QStringLiteral("hispanic");
    case dna::Archetype::alien:     return QStringLiteral("alien");
    case dna::Archetype::other:     return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

QString genderName(dna::Gender g)
{
    switch (g) {
    case dna::Gender::male:   return QStringLiteral("male");
    case dna::Gender::female: return QStringLiteral("female");
    case dna::Gender::other:  return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

ReadResult failure(ErrorCode code, const QString& msg)
{
    ReadResult r;
    r.code = code;
    r.error = msg;
    return r;
}

// Turn the library's terse status into something a user can act on.
ReadResult statusFailure(const QString& what)
{
    const sc::StatusCode st = sc::Status::get();
    const QString detail = QString::fromUtf8(st.message ? st.message : "");
    if (st == dna::StreamReader::SignatureMismatchError)
        return failure(ErrorCode::NotDna,
            QStringLiteral("%1 is not a binary MetaHuman DNA file (signature mismatch). "
                           "Export the character with MetaHuman DCC Export and pick its "
                           "head.dna / body.dna — JSON DNA files are not supported yet.").arg(what));
    if (st == dna::StreamReader::VersionMismatchError)
        return failure(ErrorCode::UnsupportedVersion,
            QStringLiteral("%1 was written with a DNA format version this build of "
                           "OpenRigLogic cannot read (%2). Re-export it from a current "
                           "MetaHuman/OpenRigLogic release, or rebuild with a newer "
                           "QTMESH_OPENRIGLOGIC_TAG.").arg(what, detail));
    if (st == dna::StreamReader::InvalidDataError)
        return failure(ErrorCode::Corrupt,
            QStringLiteral("%1 is corrupt or truncated (%2).").arg(what, detail));
    if (st == dna::BoundedIOStream::OpenError || st == dna::BoundedIOStream::ReadError)
        return failure(ErrorCode::FileNotFound,
            QStringLiteral("Could not read %1 (%2).").arg(what, detail));
    return failure(ErrorCode::Other,
        QStringLiteral("Failed to read %1: %2").arg(what, detail));
}

ReadResult build(dna::BinaryStreamReader* reader, const ReadOptions& opt, const QString& what)
{
    reader->read();
    if (!sc::Status::isOk())
        return statusFailure(what);

    ReadResult res;
    Rig& rig = res.rig;

    // ---- descriptor -------------------------------------------------------
    Descriptor& d = rig.descriptor;
    d.name = sv(reader->getName());
    d.archetype = archetypeName(reader->getArchetype());
    d.gender = genderName(reader->getGender());
    d.age = reader->getAge();
    const bool cm = reader->getTranslationUnit() == dna::TranslationUnit::cm;
    d.translationUnit = cm ? QStringLiteral("cm") : QStringLiteral("m");
    const bool deg = reader->getRotationUnit() == dna::RotationUnit::degrees;
    d.rotationUnit = deg ? QStringLiteral("degrees") : QStringLiteral("radians");
    const tdm::coord_sys cs = reader->getCoordinateSystem();
    d.coordinateSystem = QStringLiteral("x=%1 y=%2 z=%3")
        .arg(axisName(cs.x), axisName(cs.y), axisName(cs.z));
    const tdm::rot_seq rseq = reader->getRotationSequence();
    d.rotationSequence = seqName(rseq);
    d.faceWindingOrder = reader->getFaceWindingOrder() == dna::FaceWindingOrder::cw
        ? QStringLiteral("cw") : QStringLiteral("ccw");
    d.lodCount = reader->getLODCount();
    d.dbName = sv(reader->getDBName());
    d.dbComplexity = sv(reader->getDBComplexity());
    for (std::uint32_t i = 0; i < reader->getMetaDataCount(); ++i) {
        const av::StringView key = reader->getMetaDataKey(i);
        d.metadata << QStringLiteral("%1=%2").arg(sv(key), sv(reader->getMetaDataValue(key.c_str())));
    }

    if (opt.lod < 0 || opt.lod >= std::max(1, d.lodCount))
        return failure(ErrorCode::InvalidLod,
            QStringLiteral("LOD %1 requested but %2 contains %3 LOD(s) (0..%4).")
                .arg(opt.lod).arg(what).arg(d.lodCount).arg(std::max(0, d.lodCount - 1)));
    rig.lod = opt.lod;
    rig.flipV = opt.flipV;
    rig.coordinateSystemConverted = opt.convertCoordinateSystem;
    const double scale = (opt.units == Units::Meters && cm) ? 0.01 : 1.0;
    rig.unitScale = {scale, scale, scale};

    // ---- definition: joints, channels, controls ---------------------------
    const std::uint16_t jointCount = reader->getJointCount();
    rig.joints.resize(jointCount);
    const std::array<int, 3> signs{1, 1, 1};  // reader normalises sign under Transform policy
    for (std::uint16_t j = 0; j < jointCount; ++j) {
        Joint& jt = rig.joints[j];
        jt.name = sv(reader->getJointName(j));
        const std::uint16_t parent = reader->getJointParentIndex(j);
        jt.parent = (parent == j) ? -1 : static_cast<int>(parent);  // DNA: root is its own parent
        const dna::Vector3 t = reader->getNeutralJointTranslation(j);
        jt.translation = {t.x * scale, t.y * scale, t.z * scale};
        const dna::Vector3 r = reader->getNeutralJointRotation(j);
        jt.orientation = eulerToQuaternion({r.x, r.y, r.z}, d.rotationSequence, deg, signs);
    }
    if (opt.neutralSpace == NeutralSpace::World && !worldToLocal(rig.joints))
        return failure(ErrorCode::Corrupt,
            QStringLiteral("%1 has a cyclic joint hierarchy.").arg(what));

    for (const std::uint16_t j : reader->getJointIndicesForLOD(static_cast<std::uint16_t>(opt.lod)))
        rig.lodJointIndices.push_back(j);

    for (std::uint16_t i = 0; i < reader->getBlendShapeChannelCount(); ++i)
        rig.blendShapeChannels << sv(reader->getBlendShapeChannelName(i));
    for (std::uint16_t i = 0; i < reader->getGUIControlCount(); ++i)
        rig.guiControls << sv(reader->getGUIControlName(i));
    for (std::uint16_t i = 0; i < reader->getRawControlCount(); ++i)
        rig.rawControls << sv(reader->getRawControlName(i));
    for (std::uint16_t i = 0; i < reader->getAnimatedMapCount(); ++i)
        rig.animatedMaps << sv(reader->getAnimatedMapName(i));

    // ---- behaviour inventory (the Phase 2 starting point) -----------------
    if (opt.loadBehavior) {
        BehaviorSummary& b = rig.behavior;
        b.loaded = true;
        b.guiControls = reader->getGUIControlCount();
        b.rawControls = reader->getRawControlCount();
        b.psdCount = reader->getPSDCount();
        b.jointGroups = reader->getJointGroupCount();
        b.blendShapeChannels = reader->getBlendShapeChannelCount();
        b.animatedMaps = reader->getAnimatedMapCount();
        b.rbfPoses = reader->getRBFPoseCount();
        b.neuralNetworks = reader->getNeuralNetworkCount();
        b.twists = reader->getTwistCount();
        b.swings = reader->getSwingCount();
    }

    // ---- geometry ---------------------------------------------------------
    if (opt.loadGeometry) {
        const auto meshIdx = reader->getMeshIndicesForLOD(static_cast<std::uint16_t>(opt.lod));
        if (meshIdx.size() == 0 && reader->getMeshCount() == 0)
            return failure(ErrorCode::MissingGeometry,
                QStringLiteral("%1 has no geometry layer (it is a rig-behaviour-only DNA). "
                               "Import the matching head/body DNA that carries the meshes.").arg(what));

        for (const std::uint16_t mi : meshIdx) {
            Mesh m;
            m.dnaIndex = mi;
            m.name = sv(reader->getMeshName(mi));
            const auto xs = reader->getVertexPositionXs(mi);
            const auto ys = reader->getVertexPositionYs(mi);
            const auto zs = reader->getVertexPositionZs(mi);
            m.positions.reserve(xs.size() * 3);
            for (size_t i = 0; i < xs.size(); ++i) {
                m.positions.push_back(static_cast<float>(xs[i] * scale));
                m.positions.push_back(static_cast<float>(ys[i] * scale));
                m.positions.push_back(static_cast<float>(zs[i] * scale));
            }
            const auto nx = reader->getVertexNormalXs(mi);
            const auto ny = reader->getVertexNormalYs(mi);
            const auto nz = reader->getVertexNormalZs(mi);
            for (size_t i = 0; i < nx.size(); ++i) {
                m.normals.push_back(nx[i]);
                m.normals.push_back(ny[i]);
                m.normals.push_back(nz[i]);
            }
            const auto us = reader->getVertexTextureCoordinateUs(mi);
            const auto vs = reader->getVertexTextureCoordinateVs(mi);
            for (size_t i = 0; i < us.size(); ++i) {
                m.uvs.push_back(us[i]);
                m.uvs.push_back(vs[i]);
            }
            const auto lp = reader->getVertexLayoutPositionIndices(mi);
            const auto lt = reader->getVertexLayoutTextureCoordinateIndices(mi);
            const auto ln = reader->getVertexLayoutNormalIndices(mi);
            m.layouts.resize(lp.size());
            for (size_t i = 0; i < lp.size(); ++i) {
                m.layouts[i].position = lp[i];
                m.layouts[i].uv = i < lt.size() ? lt[i] : 0;
                m.layouts[i].normal = i < ln.size() ? ln[i] : 0;
            }
            const std::uint32_t faceCount = reader->getFaceCount(mi);
            m.faces.resize(faceCount);
            for (std::uint32_t f = 0; f < faceCount; ++f) {
                const auto fl = reader->getFaceVertexLayoutIndices(mi, f);
                m.faces[f].assign(fl.begin(), fl.end());
            }

            const std::uint32_t skinCount = reader->getSkinWeightsCount(mi);
            if (skinCount > 0) {
                m.skin.resize(skinCount);
                for (std::uint32_t v = 0; v < skinCount; ++v) {
                    const auto w = reader->getSkinWeightsValues(mi, v);
                    const auto j = reader->getSkinWeightsJointIndices(mi, v);
                    const size_t n = std::min(w.size(), j.size());
                    m.skin[v].reserve(n);
                    for (size_t k = 0; k < n; ++k)
                        m.skin[v].push_back({j[k], w[k]});
                }
            }

            const std::uint16_t targets = reader->getBlendShapeTargetCount(mi);
            for (std::uint16_t t = 0; t < targets; ++t) {
                BlendShapeTarget bt;
                bt.channel = reader->getBlendShapeChannelIndex(mi, t);
                const auto idx = reader->getBlendShapeTargetVertexIndices(mi, t);
                const auto dx = reader->getBlendShapeTargetDeltaXs(mi, t);
                const auto dy = reader->getBlendShapeTargetDeltaYs(mi, t);
                const auto dz = reader->getBlendShapeTargetDeltaZs(mi, t);
                const size_t n = std::min({idx.size(), dx.size(), dy.size(), dz.size()});
                bt.vertexIndices.assign(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(n));
                bt.deltas.reserve(n * 3);
                for (size_t k = 0; k < n; ++k) {
                    bt.deltas.push_back(static_cast<float>(dx[k] * scale));
                    bt.deltas.push_back(static_cast<float>(dy[k] * scale));
                    bt.deltas.push_back(static_cast<float>(dz[k] * scale));
                }
                m.blendShapeTargets.push_back(std::move(bt));
            }
            rig.meshes.push_back(std::move(m));
        }

        const int totalMeshes = reader->getMeshCount();
        if (static_cast<int>(rig.meshes.size()) < totalMeshes)
            rig.warnings << QStringLiteral("%1 mesh(es) belong only to other LODs and were not imported "
                                           "(importing LOD %2 of %3).")
                                .arg(totalMeshes - static_cast<int>(rig.meshes.size()))
                                .arg(opt.lod).arg(d.lodCount);
    }

    // Default facing: -Z (see DnaReaderInternal.h). The file itself faces +Z.
    if (opt.convertCoordinateSystem && !opt.facePlusZ) {
        yawRig(rig, 3.14159265358979323846);
        rig.facingTurned = true;
    }
    res.ok = true;
    return res;
}

}  // namespace

ReadResult read(const QString& path, const ReadOptions& options)
{
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile())
        return failure(ErrorCode::FileNotFound,
            QStringLiteral("DNA file not found: %1").arg(path));

    if (detail::isJsonDnaFile(fi.absoluteFilePath())) {
        auto mem = pma::makeScoped<dna::MemoryStream>();
        if (!mem) return failure(ErrorCode::Other, QStringLiteral("Could not allocate a memory stream."));
        const QString jerr = detail::jsonToBinaryStream(fi.absoluteFilePath(), *mem);
        if (!jerr.isEmpty()) return failure(ErrorCode::NotDna, jerr);
        auto jreader = pma::makeScoped<dna::BinaryStreamReader>(mem.get(), detail::makeConfig(options));
        if (!jreader) return failure(ErrorCode::Other, QStringLiteral("Could not create a DNA reader."));
        return build(jreader.get(), options, fi.fileName());
    }
    const QByteArray p = QFile::encodeName(fi.absoluteFilePath());
    auto stream = pma::makeScoped<dna::FileStream>(p.constData(), dna::FileStream::AccessMode::Read,
                                                   dna::FileStream::OpenMode::Binary);
    // sc::Status is process-global and sticky: a failed earlier read leaves it
    // non-OK, and create() never resets it, so only the reader's own read()
    // result is trustworthy (it resets the status first).
    if (!stream)
        return failure(ErrorCode::FileNotFound, QStringLiteral("Could not open %1.").arg(fi.fileName()));
    auto reader = pma::makeScoped<dna::BinaryStreamReader>(stream.get(), detail::makeConfig(options));
    if (!reader)
        return failure(ErrorCode::Other, QStringLiteral("Could not create a DNA reader."));
    return build(reader.get(), options, fi.fileName());
}

ReadResult readFromBuffer(const std::vector<uint8_t>& bytes, const ReadOptions& options)
{
    auto stream = pma::makeScoped<dna::MemoryStream>();
    if (!stream)
        return failure(ErrorCode::Other, QStringLiteral("Could not allocate a memory stream."));
    if (!bytes.empty())
        stream->write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    stream->seek(0);
    auto reader = pma::makeScoped<dna::BinaryStreamReader>(stream.get(), detail::makeConfig(options));
    if (!reader)
        return failure(ErrorCode::Other, QStringLiteral("Could not create a DNA reader."));
    return build(reader.get(), options, QStringLiteral("the DNA buffer"));
}

#endif  // ENABLE_OPENRIGLOGIC

}  // namespace Dna
