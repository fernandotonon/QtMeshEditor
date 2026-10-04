/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "AnimConstraints.h"

#include <QJsonArray>

#include <OgreMatrix3.h>

#include <algorithm>
#include <cmath>

namespace AnimCon {

namespace {

constexpr float kEps = 1e-6f;

struct TypeRow { Type type; const char* id; const char* label; };
const TypeRow kTypes[] = {
    {Type::LookAt, "look-at", "Look at"},
    {Type::IK, "ik", "IK (2-bone)"},
    {Type::ParentOf, "parent-of", "Parent of"},
    {Type::CopyRotation, "copy-rotation", "Copy rotation"},
    {Type::CopyPosition, "copy-position", "Copy position"},
    {Type::LimitRotation, "limit-rotation", "Limit rotation"},
};

struct AxisRow { Axis axis; const char* id; };
const AxisRow kAxes[] = {
    {Axis::X, "x"}, {Axis::Y, "y"}, {Axis::Z, "z"},
    {Axis::NegX, "-x"}, {Axis::NegY, "-y"}, {Axis::NegZ, "-z"},
};

Ogre::Vector3 anyPerpendicular(const Ogre::Vector3& v)
{
    Ogre::Vector3 p = v.crossProduct(Ogre::Vector3::UNIT_Y);
    if (p.squaredLength() < 1e-8f) p = v.crossProduct(Ogre::Vector3::UNIT_X);
    return p.normalisedCopy();
}

QJsonArray vec(const Ogre::Vector3& v) { return QJsonArray{v.x, v.y, v.z}; }
QJsonArray quat(const Ogre::Quaternion& q) { return QJsonArray{q.w, q.x, q.y, q.z}; }

bool readVec(const QJsonValue& v, Ogre::Vector3* out)
{
    const QJsonArray a = v.toArray();
    if (a.size() != 3) return false;
    for (const auto& x : a) if (!x.isDouble() || !std::isfinite(x.toDouble())) return false;
    *out = Ogre::Vector3(float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble()));
    return true;
}

bool readQuat(const QJsonValue& v, Ogre::Quaternion* out)
{
    const QJsonArray a = v.toArray();
    if (a.size() != 4) return false;
    for (const auto& x : a) if (!x.isDouble() || !std::isfinite(x.toDouble())) return false;
    *out = Ogre::Quaternion(float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble()),
                            float(a[3].toDouble()));
    out->normalise();
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Ids
// ---------------------------------------------------------------------------

QString typeId(Type t)
{
    for (const auto& r : kTypes) if (r.type == t) return QString::fromLatin1(r.id);
    return QStringLiteral("look-at");
}

bool typeFromId(const QString& id, Type* out)
{
    const QString k = id.trimmed().toLower().replace(QLatin1Char('_'), QLatin1Char('-'));
    for (const auto& r : kTypes)
        if (k == QLatin1String(r.id)) { if (out) *out = r.type; return true; }
    struct Alias { const char* a; Type t; };
    static const Alias aliases[] = {
        {"aim", Type::LookAt}, {"lookat", Type::LookAt}, {"ik2", Type::IK}, {"two-bone-ik", Type::IK},
        {"child-of", Type::ParentOf}, {"parent", Type::ParentOf}, {"copy-rot", Type::CopyRotation},
        {"copy-pos", Type::CopyPosition}, {"copy-location", Type::CopyPosition}, {"limit", Type::LimitRotation},
    };
    for (const auto& a : aliases)
        if (k == QLatin1String(a.a)) { if (out) *out = a.t; return true; }
    return false;
}

QStringList typeIds()
{
    QStringList l;
    for (const auto& r : kTypes) l << QString::fromLatin1(r.id);
    return l;
}

QString typeLabel(Type t)
{
    for (const auto& r : kTypes) if (r.type == t) return QString::fromLatin1(r.label);
    return QStringLiteral("Look at");
}

bool needsTarget(Type t) { return t != Type::LimitRotation; }
bool nodeCanOwn(Type t) { return t != Type::IK; }

QString axisId(Axis a)
{
    for (const auto& r : kAxes) if (r.axis == a) return QString::fromLatin1(r.id);
    return QStringLiteral("z");
}

bool axisFromId(const QString& id, Axis* out)
{
    const QString k = id.trimmed().toLower().replace(QLatin1Char('+'), QString());
    for (const auto& r : kAxes)
        if (k == QLatin1String(r.id)) { if (out) *out = r.axis; return true; }
    return false;
}

Ogre::Vector3 axisVector(Axis a)
{
    switch (a) {
    case Axis::X: return Ogre::Vector3::UNIT_X;
    case Axis::Y: return Ogre::Vector3::UNIT_Y;
    case Axis::Z: return Ogre::Vector3::UNIT_Z;
    case Axis::NegX: return Ogre::Vector3::NEGATIVE_UNIT_X;
    case Axis::NegY: return Ogre::Vector3::NEGATIVE_UNIT_Y;
    case Axis::NegZ: return Ogre::Vector3::NEGATIVE_UNIT_Z;
    }
    return Ogre::Vector3::UNIT_Z;
}

// ---------------------------------------------------------------------------
// Refs
// ---------------------------------------------------------------------------

QString formatRef(const Ref& r)
{
    if (r.isEmpty()) return {};
    return r.isBone() ? QStringLiteral("bone:%1/%2").arg(r.object, r.bone) : QStringLiteral("node:%1").arg(r.object);
}

bool parseRef(const QString& text, Ref* out, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    const QString s = text.trimmed();
    if (s.isEmpty()) { if (out) *out = Ref{}; return true; }
    const int colon = s.indexOf(QLatin1Char(':'));
    if (colon <= 0) return fail(QStringLiteral("'%1' must be node:Name or bone:Entity/Bone").arg(text));
    const QString kind = s.left(colon).toLower();
    const QString rest = s.mid(colon + 1);
    Ref r;
    if (kind == QLatin1String("node")) {
        r.object = rest.trimmed();
    } else if (kind == QLatin1String("bone")) {
        const int slash = rest.indexOf(QLatin1Char('/'));
        if (slash <= 0 || slash == rest.size() - 1)
            return fail(QStringLiteral("'%1': a bone reference is bone:Entity/Bone").arg(text));
        r.object = rest.left(slash).trimmed();
        r.bone = rest.mid(slash + 1).trimmed();
    } else {
        return fail(QStringLiteral("'%1': kind must be node or bone").arg(text));
    }
    if (r.object.isEmpty()) return fail(QStringLiteral("'%1' has no object name").arg(text));
    if (out) *out = r;
    return true;
}

QString displayName(const Constraint& c)
{
    if (!c.name.isEmpty()) return c.name;
    QString s = typeLabel(c.type);
    if (!c.target.isEmpty()) s += QStringLiteral(" → ") + (c.target.isBone() ? c.target.bone : c.target.object);
    return s;
}

// ---------------------------------------------------------------------------
// Transforms
// ---------------------------------------------------------------------------

Transform compose(const Transform& a, const Transform& b)
{
    Transform r;
    r.position = a.position + a.rotation * (a.scale * b.position);
    r.rotation = a.rotation * b.rotation;
    r.scale = a.scale * b.scale;
    return r;
}

Transform inverse(const Transform& t)
{
    Transform r;
    r.rotation = t.rotation.Inverse();
    r.scale = Ogre::Vector3(t.scale.x != 0 ? 1.0f / t.scale.x : 0.0f, t.scale.y != 0 ? 1.0f / t.scale.y : 0.0f,
                            t.scale.z != 0 ? 1.0f / t.scale.z : 0.0f);
    r.position = r.scale * (r.rotation * -t.position);
    return r;
}

// ---------------------------------------------------------------------------
// Solvers
// ---------------------------------------------------------------------------

Ogre::Quaternion aimRotation(const Ogre::Vector3& from, const Ogre::Vector3& to, Axis aim, Axis up,
                             const Ogre::Vector3& worldUp, const Ogre::Quaternion& current)
{
    Ogre::Vector3 f = to - from;
    if (f.squaredLength() < kEps * kEps) return current;
    f.normalise();
    const Ogre::Vector3 a = axisVector(aim);
    Ogre::Vector3 u = axisVector(up);
    if (std::abs(a.dotProduct(u)) > 0.99f) u = anyPerpendicular(a); // up parallel to aim
    u = (u - a * a.dotProduct(u)).normalisedCopy();
    // Desired world up: the hint made perpendicular to the aim direction.
    Ogre::Vector3 wu = worldUp - f * f.dotProduct(worldUp);
    if (wu.squaredLength() < 1e-8f) wu = anyPerpendicular(f);
    wu.normalise();
    // Map the local frame (a, u, a×u) onto the world frame (f, wu, f×wu).
    Ogre::Matrix3 L, W;
    L.SetColumn(0, a); L.SetColumn(1, u); L.SetColumn(2, a.crossProduct(u));
    W.SetColumn(0, f); W.SetColumn(1, wu); W.SetColumn(2, f.crossProduct(wu));
    Ogre::Quaternion q(W * L.Transpose());
    q.normalise();
    return q;
}

IkSolution solveTwoBone(const Ogre::Vector3& A, const Ogre::Vector3& B, const Ogre::Vector3& C,
                        const Ogre::Vector3& T, const Ogre::Vector3& P, bool hasPole)
{
    IkSolution s;
    const float a = (B - A).length();
    const float b = (C - B).length();
    if (a < kEps || b < kEps) return s;
    Ogre::Vector3 dvec = T - A;
    float dist = dvec.length();
    Ogre::Vector3 dir = dist > kEps ? dvec / dist : (C - A).normalisedCopy();
    const float lo = std::abs(a - b) + 1e-4f, hi = a + b - 1e-4f;
    s.reached = dist >= std::abs(a - b) - 1e-4f && dist <= a + b + 1e-4f;
    const float d = std::clamp(dist, lo, hi);

    // Bend plane: the pole (or the current elbow) made perpendicular to the
    // reach direction.
    Ogre::Vector3 ref = hasPole ? (P - A) : (B - A);
    Ogre::Vector3 bend = ref - dir * ref.dotProduct(dir);
    if (bend.squaredLength() < 1e-8f) bend = anyPerpendicular(dir);
    bend.normalise();

    float cosA = (a * a + d * d - b * b) / (2.0f * a * d);
    cosA = std::clamp(cosA, -1.0f, 1.0f);
    const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
    s.newMid = A + (dir * cosA + bend * sinA) * a;
    s.newEnd = A + dir * d;

    s.rootDelta = (B - A).getRotationTo(s.newMid - A);
    const Ogre::Vector3 lowerAfterRoot = s.rootDelta * (C - B);
    s.midDelta = lowerAfterRoot.getRotationTo(s.newEnd - s.newMid);
    s.valid = true;
    return s;
}

Ogre::Quaternion limitRotation(const Ogre::Quaternion& local, const Constraint& c)
{
    Ogre::Matrix3 m;
    local.ToRotationMatrix(m);
    Ogre::Radian x, y, z;
    m.ToEulerAnglesXYZ(x, y, z);
    auto clampDeg = [](Ogre::Radian v, float lo, float hi) {
        const float deg = v.valueDegrees();
        return Ogre::Radian(Ogre::Degree(std::clamp(deg, std::min(lo, hi), std::max(lo, hi))));
    };
    if (c.limitX) x = clampDeg(x, c.minDeg.x, c.maxDeg.x);
    if (c.limitY) y = clampDeg(y, c.minDeg.y, c.maxDeg.y);
    if (c.limitZ) z = clampDeg(z, c.minDeg.z, c.maxDeg.z);
    m.FromEulerAnglesXYZ(x, y, z);
    Ogre::Quaternion q(m);
    q.normalise();
    return q;
}

Ogre::Vector3 blend(const Ogre::Vector3& from, const Ogre::Vector3& to, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return from + (to - from) * float(t);
}

Ogre::Quaternion blend(const Ogre::Quaternion& from, const Ogre::Quaternion& to, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    if (t >= 1.0) return to;
    if (t <= 0.0) return from;
    return Ogre::Quaternion::Slerp(float(t), from, to, true);
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

QString schemaId() { return QStringLiteral("qtmesh-anim-constraints-v1"); }

QJsonObject toJson(const Constraint& c)
{
    QJsonObject o;
    o[QStringLiteral("id")] = c.id;
    if (!c.name.isEmpty()) o[QStringLiteral("name")] = c.name;
    o[QStringLiteral("type")] = typeId(c.type);
    o[QStringLiteral("owner")] = formatRef(c.owner);
    if (!c.target.isEmpty()) o[QStringLiteral("target")] = formatRef(c.target);
    if (!c.pole.isEmpty()) o[QStringLiteral("pole")] = formatRef(c.pole);
    o[QStringLiteral("enabled")] = c.enabled;
    o[QStringLiteral("influence")] = c.influence;
    switch (c.type) {
    case Type::LookAt:
        o[QStringLiteral("aim")] = axisId(c.aimAxis);
        o[QStringLiteral("up")] = axisId(c.upAxis);
        break;
    case Type::CopyPosition:
        o[QStringLiteral("axes")] = QJsonArray{c.useX, c.useY, c.useZ};
        break;
    case Type::ParentOf:
        if (c.hasOffset)
            o[QStringLiteral("offset")] = QJsonObject{{QStringLiteral("p"), vec(c.offset.position)},
                                                      {QStringLiteral("r"), quat(c.offset.rotation)},
                                                      {QStringLiteral("s"), vec(c.offset.scale)}};
        break;
    case Type::LimitRotation:
        o[QStringLiteral("limit")] = QJsonArray{c.limitX, c.limitY, c.limitZ};
        o[QStringLiteral("min")] = vec(c.minDeg);
        o[QStringLiteral("max")] = vec(c.maxDeg);
        break;
    default:
        break;
    }
    return o;
}

bool fromJson(const QJsonObject& o, Constraint* out, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    Constraint c;
    c.id = o.value(QStringLiteral("id")).toString();
    if (c.id.isEmpty()) return fail(QStringLiteral("constraint without an id"));
    c.name = o.value(QStringLiteral("name")).toString();
    if (!typeFromId(o.value(QStringLiteral("type")).toString(), &c.type))
        return fail(QStringLiteral("constraint %1: unknown type '%2'").arg(c.id, o.value(QStringLiteral("type")).toString()));
    QString err;
    if (!parseRef(o.value(QStringLiteral("owner")).toString(), &c.owner, &err) || c.owner.isEmpty())
        return fail(QStringLiteral("constraint %1: bad owner %2").arg(c.id, err));
    if (!parseRef(o.value(QStringLiteral("target")).toString(), &c.target, &err))
        return fail(QStringLiteral("constraint %1: %2").arg(c.id, err));
    if (!parseRef(o.value(QStringLiteral("pole")).toString(), &c.pole, &err))
        return fail(QStringLiteral("constraint %1: %2").arg(c.id, err));
    if (needsTarget(c.type) && c.target.isEmpty())
        return fail(QStringLiteral("constraint %1: a %2 constraint needs a target").arg(c.id, typeId(c.type)));
    c.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    const QJsonValue inf = o.value(QStringLiteral("influence"));
    if (!inf.isUndefined()) {
        if (!inf.isDouble() || !std::isfinite(inf.toDouble()))
            return fail(QStringLiteral("constraint %1: influence must be a number").arg(c.id));
        c.influence = std::clamp(inf.toDouble(), 0.0, 1.0);
    }
    if (o.contains(QStringLiteral("aim")) && !axisFromId(o.value(QStringLiteral("aim")).toString(), &c.aimAxis))
        return fail(QStringLiteral("constraint %1: bad aim axis").arg(c.id));
    if (o.contains(QStringLiteral("up")) && !axisFromId(o.value(QStringLiteral("up")).toString(), &c.upAxis))
        return fail(QStringLiteral("constraint %1: bad up axis").arg(c.id));
    if (o.contains(QStringLiteral("axes"))) {
        const QJsonArray a = o.value(QStringLiteral("axes")).toArray();
        if (a.size() == 3) { c.useX = a[0].toBool(); c.useY = a[1].toBool(); c.useZ = a[2].toBool(); }
    }
    if (o.contains(QStringLiteral("offset"))) {
        const QJsonObject off = o.value(QStringLiteral("offset")).toObject();
        if (!readVec(off.value(QStringLiteral("p")), &c.offset.position)
            || !readQuat(off.value(QStringLiteral("r")), &c.offset.rotation)
            || !readVec(off.value(QStringLiteral("s")), &c.offset.scale))
            return fail(QStringLiteral("constraint %1: malformed offset").arg(c.id));
        c.hasOffset = true;
    }
    if (o.contains(QStringLiteral("limit"))) {
        const QJsonArray a = o.value(QStringLiteral("limit")).toArray();
        if (a.size() == 3) { c.limitX = a[0].toBool(); c.limitY = a[1].toBool(); c.limitZ = a[2].toBool(); }
    }
    if (o.contains(QStringLiteral("min")) && !readVec(o.value(QStringLiteral("min")), &c.minDeg))
        return fail(QStringLiteral("constraint %1: min must be [x, y, z] degrees").arg(c.id));
    if (o.contains(QStringLiteral("max")) && !readVec(o.value(QStringLiteral("max")), &c.maxDeg))
        return fail(QStringLiteral("constraint %1: max must be [x, y, z] degrees").arg(c.id));
    if (out) *out = c;
    return true;
}

QJsonObject toDocument(const std::vector<Constraint>& cs)
{
    QJsonArray arr;
    for (const auto& c : cs) arr.append(toJson(c));
    return QJsonObject{{QStringLiteral("schema"), schemaId()}, {QStringLiteral("constraints"), arr}};
}

bool fromDocument(const QJsonObject& doc, std::vector<Constraint>* out, QString* error)
{
    if (doc.value(QStringLiteral("schema")).toString() != schemaId()) {
        if (error) *error = QStringLiteral("not a %1 document").arg(schemaId());
        return false;
    }
    std::vector<Constraint> cs;
    QStringList seen;
    for (const QJsonValue& v : doc.value(QStringLiteral("constraints")).toArray()) {
        Constraint c;
        if (!fromJson(v.toObject(), &c, error)) return false;
        if (seen.contains(c.id)) {
            if (error) *error = QStringLiteral("duplicate constraint id '%1'").arg(c.id);
            return false;
        }
        seen << c.id;
        cs.push_back(std::move(c));
    }
    if (out) *out = std::move(cs);
    return true;
}

bool applyParam(Constraint* c, const QString& keyIn, const QString& valueIn, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    const QString key = keyIn.trimmed().toLower().replace(QLatin1Char('-'), QLatin1Char('_'));
    const QString value = valueIn.trimmed();
    auto number = [&](float* dst) -> bool {
        bool ok = false;
        const double d = value.toDouble(&ok);
        if (!ok || !std::isfinite(d)) return fail(QStringLiteral("'%1' needs a number, got '%2'").arg(keyIn, valueIn));
        *dst = float(d);
        return true;
    };
    auto boolean = [&](bool* dst) -> bool {
        const QString v = value.toLower();
        if (v == QLatin1String("1") || v == QLatin1String("true") || v == QLatin1String("yes") || v == QLatin1String("on")) { *dst = true; return true; }
        if (v == QLatin1String("0") || v == QLatin1String("false") || v == QLatin1String("no") || v == QLatin1String("off")) { *dst = false; return true; }
        return fail(QStringLiteral("'%1' needs true/false, got '%2'").arg(keyIn, valueIn));
    };
    if (key == QLatin1String("influence") || key == QLatin1String("weight")) {
        float f = 0;
        if (!number(&f)) return false;
        c->influence = std::clamp(double(f), 0.0, 1.0);
        return true;
    }
    if (key == QLatin1String("enabled")) return boolean(&c->enabled);
    if (key == QLatin1String("aim") || key == QLatin1String("aim_axis")) {
        if (!axisFromId(value, &c->aimAxis)) return fail(QStringLiteral("aim must be x|y|z|-x|-y|-z"));
        return true;
    }
    if (key == QLatin1String("up") || key == QLatin1String("up_axis")) {
        if (!axisFromId(value, &c->upAxis)) return fail(QStringLiteral("up must be x|y|z|-x|-y|-z"));
        return true;
    }
    if (key == QLatin1String("x")) return boolean(&c->useX);
    if (key == QLatin1String("y")) return boolean(&c->useY);
    if (key == QLatin1String("z")) return boolean(&c->useZ);
    if (key == QLatin1String("limit_x")) return boolean(&c->limitX);
    if (key == QLatin1String("limit_y")) return boolean(&c->limitY);
    if (key == QLatin1String("limit_z")) return boolean(&c->limitZ);
    if (key == QLatin1String("min_x")) return number(&c->minDeg.x);
    if (key == QLatin1String("min_y")) return number(&c->minDeg.y);
    if (key == QLatin1String("min_z")) return number(&c->minDeg.z);
    if (key == QLatin1String("max_x")) return number(&c->maxDeg.x);
    if (key == QLatin1String("max_y")) return number(&c->maxDeg.y);
    if (key == QLatin1String("max_z")) return number(&c->maxDeg.z);
    if (key == QLatin1String("target")) {
        Ref r;
        QString e;
        if (!parseRef(value, &r, &e)) return fail(e);
        c->target = r;
        c->hasOffset = false; // a new parent-of target re-captures its offset
        return true;
    }
    if (key == QLatin1String("pole")) {
        Ref r;
        QString e;
        if (!parseRef(value, &r, &e)) return fail(e);
        c->pole = r;
        return true;
    }
    if (key == QLatin1String("name")) { c->name = value; return true; }
    return fail(QStringLiteral("unknown constraint parameter '%1'").arg(keyIn));
}

QString uniqueId(const std::vector<Constraint>& existing)
{
    for (int n = int(existing.size()) + 1;; ++n) {
        const QString id = QStringLiteral("con_%1").arg(n);
        bool used = false;
        for (const auto& c : existing) if (c.id == id) { used = true; break; }
        if (!used) return id;
    }
}

} // namespace AnimCon
