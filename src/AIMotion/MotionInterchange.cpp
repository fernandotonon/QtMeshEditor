#include "MotionInterchange.h"

#include "../AudioToFace/NpzReader.h"

#include <OgreMatrix3.h>

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

#include <algorithm>
#include <array>
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

namespace {

bool allFinite(const AudioToFace::NpyArray& a)
{
    return std::all_of(a.data.begin(), a.data.end(), [](float v) { return std::isfinite(v); });
}

bool checkVersion(AudioToFace::NpzReader& npz, QString* error)
{
    QString err;
    const auto version = npz.read(QStringLiteral("format_version"), &err);
    if (!version.valid()) return fail(error, QStringLiteral("not a motion interchange file (no format_version)"));
    if (!std::isfinite(version.data[0])) return fail(error, QStringLiteral("format_version is not a number"));
    if (int(version.data[0]) != kInterchangeVersion)
        return fail(error, QStringLiteral("motion interchange version %1 is not supported (expected %2)")
                               .arg(int(version.data[0])).arg(kInterchangeVersion));
    return true;
}

// The arrays every interchange file carries, with their shapes checked against each other.
struct RawArrays {
    QStringList names;
    AudioToFace::NpyArray parents;
    AudioToFace::NpyArray rest;
    AudioToFace::NpyArray rots;
    AudioToFace::NpyArray root;
    AudioToFace::NpyArray fps;
    int J = 0;
    int T = 0;
};

bool readArrays(AudioToFace::NpzReader& npz, RawArrays* a, QString* error)
{
    QString err;
    a->names = npz.readStrings(QStringLiteral("joint_names"), &err);
    if (a->names.isEmpty()) return fail(error, QStringLiteral("joint_names: %1").arg(err));
    a->J = int(a->names.size());
    a->parents = npz.read(QStringLiteral("joint_parents"), &err);
    a->rest = npz.read(QStringLiteral("rest_joints"), &err);
    a->rots = npz.read(QStringLiteral("local_rot_mats"), &err);
    a->root = npz.read(QStringLiteral("root_positions"), &err);
    a->fps = npz.read(QStringLiteral("fps"), &err);
    if (!a->parents.valid() || !a->rest.valid() || !a->rots.valid() || !a->root.valid() || !a->fps.valid())
        return fail(error, QStringLiteral("missing array: %1").arg(err));
    const int J = a->J;
    if (a->parents.elementCount() != J) return fail(error, QStringLiteral("joint_parents has %1 entries for %2 joints").arg(a->parents.elementCount()).arg(J));
    if (a->rest.shape != std::vector<int64_t>{J, 3}) return fail(error, QStringLiteral("rest_joints shape %1").arg(shapeText(a->rest)));
    if (a->rots.shape.size() != 4 || a->rots.shape[1] != J || a->rots.shape[2] != 3 || a->rots.shape[3] != 3)
        return fail(error, QStringLiteral("local_rot_mats shape %1, expected [T,%2,3,3]").arg(shapeText(a->rots)).arg(J));
    a->T = int(a->rots.shape[0]);
    if (a->T < 2) return fail(error, QStringLiteral("need at least 2 frames"));
    if (a->root.shape != std::vector<int64_t>{a->T, 3}) return fail(error, QStringLiteral("root_positions shape %1").arg(shapeText(a->root)));
    if (!(a->fps.data[0] > 0.0f) || a->fps.data[0] > 1000.0f) return fail(error, QStringLiteral("bad fps %1").arg(a->fps.data[0]));
    // Every value below reaches std::lround or the retargeter's root translation: reject NaN/inf up front.
    if (!allFinite(a->parents)) return fail(error, QStringLiteral("joint_parents has a non-finite value"));
    if (!allFinite(a->rest)) return fail(error, QStringLiteral("rest_joints has a non-finite value"));
    if (!allFinite(a->root)) return fail(error, QStringLiteral("root_positions has a non-finite value"));
    return true;
}

bool readSkeleton(const RawArrays& a, Motion* m, QString* error)
{
    m->fps = a.fps.data[0];
    for (int j = 0; j < a.J; ++j) {
        m->names.push_back(a.names[j].toStdString());
        const int p = int(std::lround(a.parents.data[size_t(j)]));
        if (p >= j || p < -1 || (p == -1 && j != 0))
            return fail(error, QStringLiteral("joint %1 (%2) has parent %3: parents must precede children and only joint 0 may be a root")
                                   .arg(j).arg(a.names[j]).arg(p));
        m->parents.push_back(p);
        m->restJoints.emplace_back(a.rest.data[size_t(j) * 3], a.rest.data[size_t(j) * 3 + 1], a.rest.data[size_t(j) * 3 + 2]);
    }
    return true;
}

// One 3x3 must be a finite rotation: det ~ +1 (a reflection or a zero matrix would orthonormalise into garbage).
bool checkRotation(const float* mat, int t, const QString& joint, QString* error)
{
    if (!std::all_of(mat, mat + 9, [](float v) { return std::isfinite(v); }))
        return fail(error, QStringLiteral("non-finite rotation at frame %1 joint %2").arg(t).arg(joint));
    const float det = mat[0] * (mat[4] * mat[8] - mat[5] * mat[7]) - mat[1] * (mat[3] * mat[8] - mat[5] * mat[6])
                    + mat[2] * (mat[3] * mat[7] - mat[4] * mat[6]);
    if (std::fabs(det - 1.0f) > 0.05f)
        return fail(error, QStringLiteral("frame %1 joint %2 is not a rotation (det %3)").arg(t).arg(joint).arg(det));
    return true;
}

bool readFrames(const RawArrays& a, Motion* m, QString* error)
{
    m->localRot.assign(size_t(a.T), std::vector<Ogre::Quaternion>(size_t(a.J)));
    for (int t = 0; t < a.T; ++t) {
        for (int j = 0; j < a.J; ++j) {
            const float* mat = &a.rots.data[(size_t(t) * a.J + j) * 9];
            if (!checkRotation(mat, t, a.names[j], error)) return false;
            m->localRot[size_t(t)][size_t(j)] = quatFromMatrix(mat);
        }
        m->rootPos.emplace_back(a.root.data[size_t(t) * 3], a.root.data[size_t(t) * 3 + 1], a.root.data[size_t(t) * 3 + 2]);
    }
    return true;
}

void readContacts(AudioToFace::NpzReader& npz, int T, Motion* m)
{
    if (!npz.contains(QStringLiteral("foot_contacts"))) return;
    QString err;
    const auto fc = npz.read(QStringLiteral("foot_contacts"), &err);
    if (!fc.valid() || fc.shape.size() != 2 || fc.shape[0] != T) return;
    const auto C = size_t(fc.shape[1]);
    for (size_t t = 0; t < size_t(T); ++t) m->contacts.emplace_back(fc.data.begin() + t * C, fc.data.begin() + (t + 1) * C);
}

} // namespace

bool loadInterchange(const QString& npzPath, Motion* out, QString* error)
{
    if (!out) return fail(error, QStringLiteral("no output"));
    AudioToFace::NpzReader npz;
    if (QString err; !npz.open(npzPath, &err)) return fail(error, err);
    if (!checkVersion(npz, error)) return false;
    RawArrays a;
    if (!readArrays(npz, &a, error)) return false;
    Motion m;
    if (!readSkeleton(a, &m, error) || !readFrames(a, &m, error)) return false;
    readContacts(npz, a.T, &m);
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
    // The root path is in FLOOR space (y = 0 under the lowest joint), but Kimodo's rest joints are root-relative (hips at
    // the origin, feet ~1 m below). Stand the rest on the same floor, or the retarget's (root - rest root) delta lifts
    // every frame by the hip height.
    float floorY = 0.0f;
    for (const Ogre::Vector3& r : m.restJoints) floorY = std::min(floorY, r.y);
    for (int j = 0; j < m.jointCount(); ++j) {
        const int p = m.parents[size_t(j)];
        rig.bindPos.push_back(p < 0 ? m.restJoints[size_t(j)] - Ogre::Vector3(0.0f, floorY, 0.0f)
                                    : m.restJoints[size_t(j)] - m.restJoints[size_t(p)]);
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

namespace {

std::array<uint32_t, 256> crcTable()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    return table;
}

uint32_t crc32Of(const QByteArray& data)
{
    static const std::array<uint32_t, 256> table = crcTable();
    uint32_t c = 0xFFFFFFFFu;
    for (const char ch : data) c = table[(c ^ static_cast<unsigned char>(ch)) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put16(QByteArray& b, uint16_t v) { b.append(char(v & 0xFF)).append(char(v >> 8)); }
void put32(QByteArray& b, uint32_t v) { for (int k = 0; k < 4; ++k) b.append(char((v >> (8 * k)) & 0xFF)); }

// .npy version 1.0: magic, version, little-endian header length, a dict padded with spaces to 64 bytes.
QByteArray npy(const QByteArray& descr, const std::vector<int64_t>& shape, const QByteArray& payload)
{
    // numpy's tuple repr: "(J,)" for one dimension, "(T, J, 3)" otherwise.
    QByteArrayList parts;
    for (const int64_t d : shape) parts << QByteArray::number(qlonglong(d));
    QByteArray dims = parts.join(", ");
    if (shape.size() == 1) dims += ',';
    QByteArray header = "{'descr': '" + descr + "', 'fortran_order': False, 'shape': (" + dims + "), }";
    const int total = 10 + int(header.size()) + 1;
    header += QByteArray((64 - total % 64) % 64, ' ') + '\n';
    QByteArray out("\x93NUMPY\x01\x00", 8);
    put16(out, uint16_t(header.size()));
    return out + header + payload;
}

QByteArray floats(const std::vector<float>& v)
{
    return QByteArray(reinterpret_cast<const char*>(v.data()), int(v.size() * sizeof(float)));
}

} // namespace

bool saveInterchange(const QString& npzPath, const InterchangeArrays& a, QString* error)
{
    const size_t J = a.names.size();
    if (J == 0 || a.parents.size() != J || a.restJoints.size() != J * 3)
        return fail(error, QStringLiteral("interchange: skeleton arrays do not match"));
    if (a.localRotMats.size() % (J * 9) != 0) return fail(error, QStringLiteral("interchange: rotation array size"));
    const size_t T = a.localRotMats.size() / (J * 9);
    if (T < 2 || a.rootPositions.size() != T * 3) return fail(error, QStringLiteral("interchange: frame arrays do not match"));
    const bool hasContacts = a.contactChannels > 0 && a.footContacts.size() == T * size_t(a.contactChannels);

    size_t width = 1;
    for (const std::string& n : a.names) width = std::max(width, n.size());
    QByteArray names;
    for (const std::string& n : a.names) {
        QByteArray e = QByteArray::fromStdString(n);
        e.append(QByteArray(int(width - n.size()), '\0'));
        names += e;
    }
    std::vector<float> parents(a.parents.begin(), a.parents.end());
    const int64_t Ji = int64_t(J), Ti = int64_t(T);
    std::vector<std::pair<QByteArray, QByteArray>> entries = {
        {"local_rot_mats", npy("<f4", {Ti, Ji, 3, 3}, floats(a.localRotMats))},
        {"root_positions", npy("<f4", {Ti, 3}, floats(a.rootPositions))},
        {"rest_joints", npy("<f4", {Ji, 3}, floats(a.restJoints))},
        {"joint_parents", npy("<f4", {Ji}, floats(parents))},
        {"joint_names", npy("|S" + QByteArray::number(qulonglong(width)), {Ji}, names)},
        {"fps", npy("<f4", {1}, floats({a.fps}))},
        {"format_version", npy("<f4", {1}, floats({float(kInterchangeVersion)}))},
    };
    if (hasContacts)
        entries.insert(entries.begin() + 2, {"foot_contacts", npy("<f4", {Ti, int64_t(a.contactChannels)}, floats(a.footContacts))});

    QByteArray zip, central;
    for (const auto& [name, data] : entries) {
        const QByteArray file = name + ".npy";
        const uint32_t crc = crc32Of(data);
        const uint32_t offset = uint32_t(zip.size());
        put32(zip, 0x04034b50u); put16(zip, 20); put16(zip, 0); put16(zip, 0); put16(zip, 0); put16(zip, 0x21);
        put32(zip, crc); put32(zip, uint32_t(data.size())); put32(zip, uint32_t(data.size()));
        put16(zip, uint16_t(file.size())); put16(zip, 0);
        zip += file;
        zip += data;
        put32(central, 0x02014b50u); put16(central, 20); put16(central, 20); put16(central, 0); put16(central, 0);
        put16(central, 0); put16(central, 0x21); put32(central, crc); put32(central, uint32_t(data.size()));
        put32(central, uint32_t(data.size())); put16(central, uint16_t(file.size())); put16(central, 0); put16(central, 0);
        put16(central, 0); put16(central, 0); put32(central, 0); put32(central, offset);
        central += file;
    }
    const uint32_t cdOffset = uint32_t(zip.size());
    zip += central;
    put32(zip, 0x06054b50u); put16(zip, 0); put16(zip, 0); put16(zip, uint16_t(entries.size()));
    put16(zip, uint16_t(entries.size())); put32(zip, uint32_t(central.size())); put32(zip, cdOffset); put16(zip, 0);

    const QString part = npzPath + QStringLiteral(".part");
    QFile f(part);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(zip) != zip.size()) {
        QFile::remove(part);
        return fail(error, QStringLiteral("could not write %1").arg(npzPath));
    }
    f.close();
    QFile::remove(npzPath);
    if (!QFile::rename(part, npzPath)) return fail(error, QStringLiteral("could not rename %1").arg(part));
    return true;
}

} // namespace AIMotion
