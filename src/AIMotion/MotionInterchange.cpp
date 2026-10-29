#include "MotionInterchange.h"

#include "../AudioToFace/NpzReader.h"

#include <OgreMatrix3.h>

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>

namespace AIMotion {

int Motion::indexOf(const std::string& name) const
{
    const auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? -1 : int(it - names.begin());
}

Ogre::Quaternion quatFromMatrix(const float m[9])
{
    Ogre::Matrix3 r(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
    r.Orthonormalize();
    Ogre::Quaternion q;
    q.FromRotationMatrix(r);
    q.normalise();
    return q;
}

namespace {

bool fail(QString* error, const QString& msg)
{
    if (error) *error = msg;
    return false;
}

QString shapeText(const AudioToFace::NpyArray& a)
{
    QStringList dims;
    for (int64_t d : a.shape) dims << QString::number(d);
    return QLatin1Char('[') + dims.join(QLatin1Char(',')) + QLatin1Char(']');
}

} // namespace

bool loadInterchange(const QString& npzPath, Motion* out, QString* error)
{
    if (!out) return fail(error, QStringLiteral("no output"));
    AudioToFace::NpzReader npz;
    QString err;
    if (!npz.open(npzPath, &err)) return fail(error, err);

    const auto version = npz.read(QStringLiteral("format_version"), &err);
    if (!version.valid()) return fail(error, QStringLiteral("not a motion interchange file (no format_version)"));
    if (int(version.data[0]) != kInterchangeVersion)
        return fail(error, QStringLiteral("motion interchange version %1 is not supported (expected %2)")
                               .arg(int(version.data[0])).arg(kInterchangeVersion));

    const QStringList names = npz.readStrings(QStringLiteral("joint_names"), &err);
    if (names.isEmpty()) return fail(error, QStringLiteral("joint_names: %1").arg(err));
    const int J = int(names.size());

    const auto parents = npz.read(QStringLiteral("joint_parents"), &err);
    const auto rest = npz.read(QStringLiteral("rest_joints"), &err);
    const auto rots = npz.read(QStringLiteral("local_rot_mats"), &err);
    const auto root = npz.read(QStringLiteral("root_positions"), &err);
    const auto fps = npz.read(QStringLiteral("fps"), &err);
    if (!parents.valid() || !rest.valid() || !rots.valid() || !root.valid() || !fps.valid())
        return fail(error, QStringLiteral("missing array: %1").arg(err));

    if (parents.elementCount() != J) return fail(error, QStringLiteral("joint_parents has %1 entries for %2 joints").arg(parents.elementCount()).arg(J));
    if (rest.shape != std::vector<int64_t>{J, 3}) return fail(error, QStringLiteral("rest_joints shape %1").arg(shapeText(rest)));
    if (rots.shape.size() != 4 || rots.shape[1] != J || rots.shape[2] != 3 || rots.shape[3] != 3)
        return fail(error, QStringLiteral("local_rot_mats shape %1, expected [T,%2,3,3]").arg(shapeText(rots)).arg(J));
    const int T = int(rots.shape[0]);
    if (T < 2) return fail(error, QStringLiteral("need at least 2 frames"));
    if (root.shape != std::vector<int64_t>{T, 3}) return fail(error, QStringLiteral("root_positions shape %1").arg(shapeText(root)));
    if (!(fps.data[0] > 0.0f) || fps.data[0] > 1000.0f) return fail(error, QStringLiteral("bad fps %1").arg(fps.data[0]));

    Motion m;
    m.fps = fps.data[0];
    for (int j = 0; j < J; ++j) {
        m.names.push_back(names[j].toStdString());
        const int p = int(std::lround(parents.data[size_t(j)]));
        if (p >= j || p < -1 || (p == -1 && j != 0))
            return fail(error, QStringLiteral("joint %1 (%2) has parent %3: parents must precede children and only joint 0 may be a root")
                                   .arg(j).arg(names[j]).arg(p));
        m.parents.push_back(p);
        m.restJoints.emplace_back(rest.data[size_t(j) * 3], rest.data[size_t(j) * 3 + 1], rest.data[size_t(j) * 3 + 2]);
    }

    m.localRot.assign(size_t(T), std::vector<Ogre::Quaternion>(size_t(J)));
    for (int t = 0; t < T; ++t) {
        for (int j = 0; j < J; ++j) {
            const float* mat = &rots.data[(size_t(t) * J + j) * 9];
            for (int k = 0; k < 9; ++k)
                if (!std::isfinite(mat[k])) return fail(error, QStringLiteral("non-finite rotation at frame %1 joint %2").arg(t).arg(names[j]));
            // A real rotation has det ~ +1; a reflection or a zero matrix would orthonormalise into garbage.
            const float det = mat[0] * (mat[4] * mat[8] - mat[5] * mat[7]) - mat[1] * (mat[3] * mat[8] - mat[5] * mat[6])
                            + mat[2] * (mat[3] * mat[7] - mat[4] * mat[6]);
            if (std::fabs(det - 1.0f) > 0.05f)
                return fail(error, QStringLiteral("frame %1 joint %2 is not a rotation (det %3)").arg(t).arg(names[j]).arg(det));
            m.localRot[size_t(t)][size_t(j)] = quatFromMatrix(mat);
        }
        m.rootPos.emplace_back(root.data[size_t(t) * 3], root.data[size_t(t) * 3 + 1], root.data[size_t(t) * 3 + 2]);
    }

    if (npz.contains(QStringLiteral("foot_contacts"))) {
        const auto fc = npz.read(QStringLiteral("foot_contacts"), &err);
        if (fc.valid() && fc.shape.size() == 2 && fc.shape[0] == T) {
            const int C = int(fc.shape[1]);
            for (int t = 0; t < T; ++t)
                m.contacts.emplace_back(fc.data.begin() + size_t(t) * C, fc.data.begin() + size_t(t + 1) * C);
        }
    }

    // result.json beside the file: provenance (model, seed, prompts). Optional.
    QFile manifest(QFileInfo(npzPath).dir().filePath(QStringLiteral("result.json")));
    if (manifest.open(QIODevice::ReadOnly)) m.meta = QJsonDocument::fromJson(manifest.readAll()).object();

    *out = std::move(m);
    return true;
}

Retarget::RigDesc rigDesc(const Motion& m)
{
    Retarget::RigDesc rig;
    rig.names = m.names;
    rig.parent = m.parents;
    for (int j = 0; j < m.jointCount(); ++j) {
        const int p = m.parents[size_t(j)];
        rig.bindPos.push_back(p < 0 ? m.restJoints[size_t(j)] : m.restJoints[size_t(j)] - m.restJoints[size_t(p)]);
        rig.bindRot.push_back(Ogre::Quaternion::IDENTITY);
    }
    return rig;
}

std::vector<Retarget::LocalPose> localPoses(const Motion& m)
{
    const Retarget::RigDesc rig = rigDesc(m);
    std::vector<Retarget::LocalPose> frames;
    frames.reserve(size_t(m.frameCount()));
    for (int t = 0; t < m.frameCount(); ++t) {
        Retarget::LocalPose pose;
        pose.rot = m.localRot[size_t(t)];
        pose.pos = rig.bindPos;
        pose.pos[0] = m.rootPos[size_t(t)];
        frames.push_back(std::move(pose));
    }
    return frames;
}

Motion resample(const Motion& m, float fps)
{
    if (!(fps > 0.0f) || m.frameCount() < 2 || std::fabs(fps - m.fps) < 1e-4f) return m;
    Motion r = m;
    r.fps = fps;
    const float dur = m.duration();
    const int T = std::max(2, int(std::lround(dur * fps)) + 1);
    r.localRot.assign(size_t(T), {});
    r.rootPos.assign(size_t(T), Ogre::Vector3::ZERO);
    if (!m.contacts.empty()) r.contacts.assign(size_t(T), {});
    for (int t = 0; t < T; ++t) {
        // Map onto the source timeline; the last frame lands exactly on the source's last frame.
        const float s = t == T - 1 ? float(m.frameCount() - 1) : float(t) * dur / float(T - 1) * m.fps;
        const int a = std::min(int(std::floor(s)), m.frameCount() - 1);
        const int b = std::min(a + 1, m.frameCount() - 1);
        const float w = s - float(a);
        auto& rot = r.localRot[size_t(t)];
        rot.resize(size_t(m.jointCount()));
        for (int j = 0; j < m.jointCount(); ++j)
            rot[size_t(j)] = Ogre::Quaternion::Slerp(w, m.localRot[size_t(a)][size_t(j)], m.localRot[size_t(b)][size_t(j)], true);
        r.rootPos[size_t(t)] = m.rootPos[size_t(a)] * (1.0f - w) + m.rootPos[size_t(b)] * w;
        if (!m.contacts.empty()) {
            auto& c = r.contacts[size_t(t)];
            c.resize(m.contacts[size_t(a)].size());
            for (size_t k = 0; k < c.size(); ++k)
                c[k] = m.contacts[size_t(a)][k] * (1.0f - w) + m.contacts[size_t(b)][k] * w;
        }
    }
    return r;
}

} // namespace AIMotion
