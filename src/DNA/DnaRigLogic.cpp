#include "DnaRigLogic.h"

#ifdef ENABLE_OPENRIGLOGIC
#include "DnaReaderInternal.h"

#include <riglogic/RigLogic.h>
#include <trio/streams/FileStream.h>

#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <chrono>
#include <cmath>
#endif

namespace Dna {

#ifndef ENABLE_OPENRIGLOGIC

struct RigEvaluator::Impl {};
RigEvaluator::RigEvaluator() = default;
RigEvaluator::~RigEvaluator() = default;

std::unique_ptr<RigEvaluator> RigEvaluator::create(const QString&, const ReadOptions&, QString* error)
{
    if (error)
        *error = QStringLiteral("MetaHuman DNA support is not compiled into this build. "
                                "Rebuild QtMeshEditor with -DENABLE_OPENRIGLOGIC=ON.");
    return nullptr;
}
int RigEvaluator::jointCount() const { return 0; }
QString RigEvaluator::jointName(int) const { return {}; }
int RigEvaluator::jointIndex(const QString&) const { return -1; }
int RigEvaluator::rawControlCount() const { return 0; }
QString RigEvaluator::rawControlName(int) const { return {}; }
int RigEvaluator::rawControlIndex(const QString&) const { return -1; }
QStringList RigEvaluator::driverJoints() const { return {}; }
const std::vector<int>& RigEvaluator::drivenJoints() const { static const std::vector<int> none; return none; }
int RigEvaluator::rbfSolverCount() const { return 0; }
int RigEvaluator::rbfPoseCount() const { return 0; }
void RigEvaluator::resetControls() {}
void RigEvaluator::setRawControl(int, float) {}
bool RigEvaluator::setDriverRotation(const QString&, const std::array<double, 4>&) { return false; }
double RigEvaluator::calculate() { return 0.0; }
std::vector<JointPose> RigEvaluator::drivenPoses() const { return {}; }
JointPose RigEvaluator::jointPose(int) const { return {}; }
std::vector<float> RigEvaluator::jointDeltas() const { return {}; }

#else  // ENABLE_OPENRIGLOGIC

namespace {
constexpr int kAttr = 9;  // tx ty tz rx ry rz sx sy sz
}

struct RigEvaluator::Impl {
    pma::ScopedPtr<trio::FileStream> stream;
    pma::ScopedPtr<dna::MemoryStream> memStream;   // a JSON DNA, re-serialised as binary
    pma::ScopedPtr<dna::BinaryStreamReader> reader;
    pma::ScopedPtr<rl4::RigLogic> logic;
    pma::ScopedPtr<rl4::RigInstance> instance;

    QStringList jointNames;
    QHash<QString, int> jointByName;
    QStringList rawNames;
    QHash<QString, int> rawByName;
    QHash<QString, int> driverFirstRaw;   // joint name -> index of its ".qx" control
    QStringList drivers;
    std::vector<int> driven;
    std::vector<float> neutral;           // jointCount * 9, DNA units/degrees
    double unitScale = 1.0;
    QString rotationSequence = QStringLiteral("xyz");
    bool degrees = true;
    int rbfSolvers = 0, rbfPoses = 0;
    std::vector<char> isRoot;             // joints whose parent is themselves
    double rootYaw = 0.0;                 // radians about +Y: the reader's default facing turn
};

RigEvaluator::RigEvaluator() : d(new Impl) {}
RigEvaluator::~RigEvaluator() = default;

std::unique_ptr<RigEvaluator> RigEvaluator::create(const QString& path, const ReadOptions& options,
                                                   QString* error)
{
    auto fail = [&](const QString& m) -> std::unique_ptr<RigEvaluator> {
        if (error) *error = m;
        return nullptr;
    };
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile())
        return fail(QStringLiteral("DNA file not found: %1").arg(path));

    std::unique_ptr<RigEvaluator> ev(new RigEvaluator);
    Impl& d = *ev->d;
    const bool json = detail::isJsonDnaFile(fi.absoluteFilePath());
    if (json) {
        d.memStream = pma::makeScoped<dna::MemoryStream>();
        if (!d.memStream) return fail(QStringLiteral("Could not allocate a memory stream."));
        const QString jerr = detail::jsonToBinaryStream(fi.absoluteFilePath(), *d.memStream);
        if (!jerr.isEmpty()) return fail(jerr);
    } else {
        const QByteArray p = QFile::encodeName(fi.absoluteFilePath());
        d.stream = pma::makeScoped<dna::FileStream>(p.constData(), dna::FileStream::AccessMode::Read,
                                                    dna::FileStream::OpenMode::Binary);
        if (!d.stream)
            return fail(QStringLiteral("Could not open %1.").arg(fi.fileName()));
    }

    ReadOptions o = options;
    o.loadGeometry = false;   // evaluation needs no vertices
    o.loadBehavior = true;
    d.reader = json ? pma::makeScoped<dna::BinaryStreamReader>(d.memStream.get(), detail::makeConfig(o))
               : pma::makeScoped<dna::BinaryStreamReader>(d.stream.get(), detail::makeConfig(o));
    d.reader->read();
    if (!sc::Status::isOk())
        return fail(QStringLiteral("Failed to read %1 for rig evaluation: %2")
                        .arg(fi.fileName(), QString::fromUtf8(sc::Status::get().message)));

    if (d.reader->getRawControlCount() == 0 && d.reader->getGUIControlCount() == 0)
        return fail(QStringLiteral("%1 carries no rig-logic controls (no behaviour layer), so there is "
                                   "nothing to evaluate.").arg(fi.fileName()));

    d.logic = pma::makeScoped<rl4::RigLogic>(d.reader.get());
    if (!sc::Status::isOk() || !d.logic)
        return fail(QStringLiteral("RigLogic could not be created from %1: %2")
                        .arg(fi.fileName(), QString::fromUtf8(sc::Status::get().message)));
    d.instance = pma::makeScoped<rl4::RigInstance>(d.logic.get());
    if (!d.instance)
        return fail(QStringLiteral("RigLogic could not create a rig instance."));
    d.instance->setLOD(static_cast<std::uint16_t>(o.lod));

    const std::uint16_t jc = d.reader->getJointCount();
    for (std::uint16_t j = 0; j < jc; ++j) {
        const QString n = QString::fromUtf8(d.reader->getJointName(j).c_str());
        d.jointNames << n;
        d.jointByName.insert(n, j);
    }
    const std::uint16_t rc = d.reader->getRawControlCount();
    for (std::uint16_t i = 0; i < rc; ++i) {
        const QString n = QString::fromUtf8(d.reader->getRawControlName(i).c_str());
        d.rawNames << n;
        d.rawByName.insert(n, i);
        if (n.endsWith(QStringLiteral(".qx"))) {
            const QString joint = n.left(n.size() - 3);
            // The four components must be contiguous x,y,z,w for setDriverRotation.
            if (i + 3 < rc
                && d.reader->getRawControlName(i + 3).c_str() == (joint + QStringLiteral(".qw")).toStdString()) {
                d.driverFirstRaw.insert(joint, i);
                d.drivers << joint;
            }
        }
    }
    const auto neutral = d.logic->getNeutralJointValues();
    d.neutral.assign(neutral.begin(), neutral.end());
    // The reader turns the rig 180 degrees about Y by default (yawRig on the ROOT joints only);
    // RigLogic's outputs are in the file's frame, so the roots must be turned the same way here.
    d.rootYaw = (o.convertCoordinateSystem && !o.facePlusZ) ? 3.14159265358979323846 : 0.0;
    d.isRoot.assign(d.reader->getJointCount(), 0);
    for (std::uint16_t j = 0; j < d.reader->getJointCount(); ++j)
        d.isRoot[j] = d.reader->getJointParentIndex(j) == j ? 1 : 0;
    d.degrees = d.reader->getRotationUnit() == dna::RotationUnit::degrees;
    {
        const auto seq = d.reader->getRotationSequence();
        static const char* names[] = {"xyz", "xzy", "yxz", "yzx", "zxy", "zyx"};
        d.rotationSequence = QString::fromLatin1(names[static_cast<int>(seq)]);
    }
    const bool cm = d.reader->getTranslationUnit() == dna::TranslationUnit::cm;
    d.unitScale = (o.units == Units::Meters && cm) ? 0.01 : 1.0;
    d.rbfSolvers = d.reader->getRBFSolverCount();
    d.rbfPoses = d.reader->getRBFPoseCount();

    // Driven joints = every joint that owns at least one output attribute in any
    // joint group.
    std::vector<char> isDriven(jc, 0);
    for (std::uint16_t g = 0; g < d.reader->getJointGroupCount(); ++g)
        for (const std::uint16_t out : d.reader->getJointGroupOutputIndices(g))
            if (out / kAttr < jc) isDriven[out / kAttr] = 1;
    // Twist and swing setups drive joints too (thigh_twist_*, upperarm_twist_* …) — RigLogic writes
    // their outputs like any other, and leaving them out of the driven set left the twist joints
    // at rest under a bent limb (the largest error against the full rig).
    for (std::uint16_t t = 0; t < d.reader->getTwistCount(); ++t)
        for (const std::uint16_t out : d.reader->getTwistOutputJointIndices(t))
            if (out < jc) isDriven[out] = 1;
    for (std::uint16_t t = 0; t < d.reader->getSwingCount(); ++t)
        for (const std::uint16_t out : d.reader->getSwingOutputJointIndices(t))
            if (out < jc) isDriven[out] = 1;
    for (std::uint16_t j = 0; j < jc; ++j)
        if (isDriven[j]) d.driven.push_back(j);

    // RigLogic joint outputs are in the DNA's coordinate system after the
    // reader's transform; nothing more to convert here.
    return ev;
}

int RigEvaluator::jointCount() const { return d->jointNames.size(); }
QString RigEvaluator::jointName(int j) const { return (j >= 0 && j < d->jointNames.size()) ? d->jointNames[j] : QString(); }
int RigEvaluator::jointIndex(const QString& n) const { return d->jointByName.value(n, -1); }
int RigEvaluator::rawControlCount() const { return d->rawNames.size(); }
QString RigEvaluator::rawControlName(int i) const { return (i >= 0 && i < d->rawNames.size()) ? d->rawNames[i] : QString(); }
int RigEvaluator::rawControlIndex(const QString& n) const { return d->rawByName.value(n, -1); }
QStringList RigEvaluator::driverJoints() const { return d->drivers; }
const std::vector<int>& RigEvaluator::drivenJoints() const { return d->driven; }
int RigEvaluator::rbfSolverCount() const { return d->rbfSolvers; }
int RigEvaluator::rbfPoseCount() const { return d->rbfPoses; }

void RigEvaluator::resetControls()
{
    for (int i = 0; i < d->rawNames.size(); ++i)
        d->instance->setRawControl(static_cast<std::uint16_t>(i), 0.0f);
    // A quaternion control at rest is identity (w = 1), not all-zero.
    for (auto it = d->driverFirstRaw.cbegin(); it != d->driverFirstRaw.cend(); ++it)
        d->instance->setRawControl(static_cast<std::uint16_t>(it.value() + 3), 1.0f);
}

void RigEvaluator::setRawControl(int index, float value)
{
    if (index >= 0 && index < d->rawNames.size())
        d->instance->setRawControl(static_cast<std::uint16_t>(index), value);
}

bool RigEvaluator::setDriverRotation(const QString& joint, const std::array<double, 4>& q)
{
    const auto it = d->driverFirstRaw.constFind(joint);
    if (it == d->driverFirstRaw.constEnd())
        return false;
    double w = q[0], x = q[1], y = q[2], z = q[3];
    const double n = std::sqrt(w * w + x * x + y * y + z * z);
    if (n < 1e-12) { w = 1; x = y = z = 0; } else { w /= n; x /= n; y /= n; z /= n; }
    if (w < 0) { w = -w; x = -x; y = -y; z = -z; }
    const int base = it.value();
    d->instance->setRawControl(static_cast<std::uint16_t>(base), static_cast<float>(x));
    d->instance->setRawControl(static_cast<std::uint16_t>(base + 1), static_cast<float>(y));
    d->instance->setRawControl(static_cast<std::uint16_t>(base + 2), static_cast<float>(z));
    d->instance->setRawControl(static_cast<std::uint16_t>(base + 3), static_cast<float>(w));
    return true;
}

double RigEvaluator::calculate()
{
    const auto t0 = std::chrono::steady_clock::now();
    d->logic->calculate(d->instance.get());
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

JointPose RigEvaluator::jointPose(int joint) const
{
    JointPose p;
    p.joint = joint;
    if (joint < 0 || joint >= d->jointNames.size())
        return p;
    const auto out = d->instance->getJointOutputs();
    const size_t b = static_cast<size_t>(joint) * kAttr;
    if (b + kAttr > out.size() || b + kAttr > d->neutral.size())
        return p;
    auto v = [&](int a) { return static_cast<double>(d->neutral[b + a]) + static_cast<double>(out[b + a]); };
    p.translation = {v(0) * d->unitScale, v(1) * d->unitScale, v(2) * d->unitScale};
    p.orientation = eulerToQuaternion({v(3), v(4), v(5)}, d->rotationSequence, d->degrees);
    p.scale = {v(6), v(7), v(8)};
    if (d->rootYaw != 0.0 && size_t(joint) < d->isRoot.size() && d->isRoot[size_t(joint)]) {
        const double c = std::cos(d->rootYaw), sn = std::sin(d->rootYaw);
        const double x = p.translation[0], z = p.translation[2];
        p.translation[0] = x * c + z * sn;
        p.translation[2] = -x * sn + z * c;
        const std::array<double, 4> qy{std::cos(d->rootYaw / 2), 0, std::sin(d->rootYaw / 2), 0};
        const auto q = p.orientation;
        p.orientation = {qy[0] * q[0] - qy[1] * q[1] - qy[2] * q[2] - qy[3] * q[3], qy[0] * q[1] + qy[1] * q[0] + qy[2] * q[3] - qy[3] * q[2],
                         qy[0] * q[2] - qy[1] * q[3] + qy[2] * q[0] + qy[3] * q[1], qy[0] * q[3] + qy[1] * q[2] - qy[2] * q[1] + qy[3] * q[0]};
    }
    return p;
}

std::vector<JointPose> RigEvaluator::drivenPoses() const
{
    std::vector<JointPose> poses;
    poses.reserve(d->driven.size());
    for (const int j : d->driven)
        poses.push_back(jointPose(j));
    return poses;
}

std::vector<float> RigEvaluator::jointDeltas() const
{
    const auto out = d->instance->getJointOutputs();
    return std::vector<float>(out.begin(), out.end());
}

#endif  // ENABLE_OPENRIGLOGIC

}  // namespace Dna
