#include "BoneDisplay.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace BoneDisplay {
namespace {

bool finite3(const Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finiteColor(const Color& c)
{
    return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b) && std::isfinite(c.a);
}

float clamp01(float v)
{
    if (v < 0.f) return 0.f;
    if (v > 1.f) return 1.f;
    return v;
}

Color clampedColor(Color c)
{
    c.r = clamp01(c.r);
    c.g = clamp01(c.g);
    c.b = clamp01(c.b);
    c.a = clamp01(c.a);
    return c;
}

int hexNibble(QChar ch)
{
    const char c = ch.toLatin1();
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hexByte(const QString& s, int offset)
{
    const int hi = hexNibble(s.at(offset));
    const int lo = hexNibble(s.at(offset + 1));
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

QJsonArray vec3Json(const Vec3& v)
{
    return QJsonArray{v.x, v.y, v.z};
}

QJsonArray colorJson(const Color& c)
{
    return QJsonArray{c.r, c.g, c.b, c.a};
}

bool readVec3(const QJsonValue& value, Vec3* out, QString* error, const char* what)
{
    const QJsonArray a = value.toArray();
    if (a.size() != 3) {
        if (error) *error = QStringLiteral("%1 must be a 3-number array").arg(QLatin1String(what));
        return false;
    }
    Vec3 v;
    v.x = static_cast<float>(a.at(0).toDouble());
    v.y = static_cast<float>(a.at(1).toDouble());
    v.z = static_cast<float>(a.at(2).toDouble());
    if (!finite3(v)) {
        if (error) *error = QStringLiteral("%1 has a non-finite component").arg(QLatin1String(what));
        return false;
    }
    *out = v;
    return true;
}

bool readColor(const QJsonValue& value, Color* out, QString* error)
{
    const QJsonArray a = value.toArray();
    if (a.size() != 3 && a.size() != 4) {
        if (error) *error = QStringLiteral("color must be an RGB or RGBA array");
        return false;
    }
    Color c;
    c.r = static_cast<float>(a.at(0).toDouble());
    c.g = static_cast<float>(a.at(1).toDouble());
    c.b = static_cast<float>(a.at(2).toDouble());
    c.a = a.size() == 4 ? static_cast<float>(a.at(3).toDouble()) : 0.65f;
    if (!finiteColor(c)) {
        if (error) *error = QStringLiteral("color has a non-finite component");
        return false;
    }
    *out = clampedColor(c);
    return true;
}

Bone& ensureBone(Document* doc, const QString& name)
{
    return (*doc).bones[name];
}

void pruneBone(Document* doc, const QString& name)
{
    const auto it = doc->bones.constFind(name);
    if (it == doc->bones.constEnd()) return;
    if (it.value() == Bone{})
        doc->bones.remove(name);
}

bool knownGroup(const Document& doc, const QString& name)
{
    for (const Group& g : doc.groups)
        if (g.name == name) return true;
    return false;
}

QJsonObject boneToJson(const Bone& b)
{
    QJsonObject o;
    o.insert(QStringLiteral("shape"), shapeId(b.shape));
    o.insert(QStringLiteral("customMesh"), b.customMesh);
    o.insert(QStringLiteral("scale"), vec3Json(b.scale));
    o.insert(QStringLiteral("offset"), vec3Json(b.offset));
    o.insert(QStringLiteral("rotation"), vec3Json(b.rotationDeg));
    o.insert(QStringLiteral("color"), colorJson(b.color));
    o.insert(QStringLiteral("colorSet"), b.colorSet);
    o.insert(QStringLiteral("useGroupColor"), b.useGroupColor);
    o.insert(QStringLiteral("visible"), b.visible);
    o.insert(QStringLiteral("selectionLocked"), b.selectionLocked);
    o.insert(QStringLiteral("lockTranslation"), b.lockTranslation);
    o.insert(QStringLiteral("lockRotation"), b.lockRotation);
    o.insert(QStringLiteral("lockScale"), b.lockScale);
    o.insert(QStringLiteral("layers"), static_cast<double>(b.layers));
    QJsonArray groups;
    for (const QString& g : b.groups)
        groups.append(g);
    o.insert(QStringLiteral("groups"), groups);
    return o;
}

bool boneFromJson(const QJsonObject& o, Bone* out, QString* error)
{
    Bone b;
    if (o.contains(QStringLiteral("shape"))) {
        Shape shape;
        if (!shapeFromId(o.value(QStringLiteral("shape")).toString(), &shape)) {
            if (error) *error = QStringLiteral("unknown bone shape");
            return false;
        }
        b.shape = shape;
    }
    b.customMesh = o.value(QStringLiteral("customMesh")).toString();
    if (o.contains(QStringLiteral("scale"))) {
        if (!readVec3(o.value(QStringLiteral("scale")), &b.scale, error, "scale")) return false;
        if (b.scale.x == 0.f || b.scale.y == 0.f || b.scale.z == 0.f) {
            if (error) *error = QStringLiteral("scale must be non-zero");
            return false;
        }
    }
    if (o.contains(QStringLiteral("offset"))
        && !readVec3(o.value(QStringLiteral("offset")), &b.offset, error, "offset"))
        return false;
    if (o.contains(QStringLiteral("rotation"))
        && !readVec3(o.value(QStringLiteral("rotation")), &b.rotationDeg, error, "rotation"))
        return false;
    if (o.contains(QStringLiteral("color")) && !readColor(o.value(QStringLiteral("color")), &b.color, error))
        return false;
    b.colorSet = o.value(QStringLiteral("colorSet")).toBool(false);
    b.useGroupColor = o.value(QStringLiteral("useGroupColor")).toBool(false);
    b.visible = o.value(QStringLiteral("visible")).toBool(true);
    b.selectionLocked = o.value(QStringLiteral("selectionLocked")).toBool(false);
    b.lockTranslation = o.value(QStringLiteral("lockTranslation")).toBool(false);
    b.lockRotation = o.value(QStringLiteral("lockRotation")).toBool(false);
    b.lockScale = o.value(QStringLiteral("lockScale")).toBool(false);
    if (o.contains(QStringLiteral("layers"))) {
        const double layers = o.value(QStringLiteral("layers")).toDouble(-1.0);
        if (layers < 0.0 || layers > static_cast<double>(kAllLayers) || !std::isfinite(layers)) {
            if (error) *error = QStringLiteral("layers must be a 32-bit mask");
            return false;
        }
        b.layers = static_cast<std::uint32_t>(layers);
    }
    if (o.contains(QStringLiteral("groups"))) {
        const QJsonArray groups = o.value(QStringLiteral("groups")).toArray();
        for (const QJsonValue& g : groups) {
            const QString name = g.toString();
            if (name.isEmpty() || b.groups.contains(name)) continue;
            b.groups.append(name);
        }
    }
    *out = b;
    return true;
}

bool fillDocument(const QJsonObject& obj, Document* out, QString* error)
{
    Document doc;
    if (obj.contains(QStringLiteral("visibleLayers"))) {
        const double layers = obj.value(QStringLiteral("visibleLayers")).toDouble(-1.0);
        if (layers < 0.0 || layers > static_cast<double>(kAllLayers) || !std::isfinite(layers)) {
            if (error) *error = QStringLiteral("visibleLayers must be a 32-bit mask");
            return false;
        }
        doc.visibleLayers = static_cast<std::uint32_t>(layers);
    }
    doc.isolatedGroup = obj.value(QStringLiteral("isolatedGroup")).toString();
    const QJsonArray groups = obj.value(QStringLiteral("groups")).toArray();
    for (const QJsonValue& value : groups) {
        const QJsonObject g = value.toObject();
        const QString name = g.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            if (error) *error = QStringLiteral("group name is empty");
            return false;
        }
        if (knownGroup(doc, name)) {
            if (error) *error = QStringLiteral("duplicate group '%1'").arg(name);
            return false;
        }
        Color color {1.f, 0.85f, 0.2f, 0.7f};
        if (g.contains(QStringLiteral("color")) && !readColor(g.value(QStringLiteral("color")), &color, error))
            return false;
        doc.groups.append(Group{name, color});
    }
    const QJsonObject bones = obj.value(QStringLiteral("bones")).toObject();
    for (auto it = bones.begin(); it != bones.end(); ++it) {
        if (it.key().isEmpty()) {
            if (error) *error = QStringLiteral("bone name is empty");
            return false;
        }
        Bone bone;
        if (!boneFromJson(it.value().toObject(), &bone, error)) return false;
        if (!(bone == Bone{}))
            doc.bones.insert(it.key(), bone);
    }
    if (!doc.isolatedGroup.isEmpty() && !knownGroup(doc, doc.isolatedGroup)) {
        if (error) *error = QStringLiteral("isolated group is not in the group list");
        return false;
    }
    *out = doc;
    return true;
}

quint32 readU32(const QByteArray& bytes, int offset)
{
    return static_cast<quint32>(static_cast<quint8>(bytes.at(offset)))
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 1))) << 8)
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 2))) << 16)
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 3))) << 24);
}

void appendU32(QByteArray* out, quint32 value)
{
    out->append(static_cast<char>(value & 0xffu));
    out->append(static_cast<char>((value >> 8) & 0xffu));
    out->append(static_cast<char>((value >> 16) & 0xffu));
    out->append(static_cast<char>((value >> 24) & 0xffu));
}

QByteArray pad4(QByteArray data, char pad)
{
    while (data.size() % 4 != 0)
        data.append(pad);
    return data;
}

bool readGlb(const QByteArray& whole, QByteArray* json, QByteArray* bin, QString* error)
{
    if (whole.size() < 12) {
        if (error) *error = QStringLiteral("glb is too small");
        return false;
    }
    if (readU32(whole, 0) != 0x46546C67u) {
        if (error) *error = QStringLiteral("glb magic mismatch");
        return false;
    }
    int offset = 12;
    bool sawJson = false;
    while (offset + 8 <= whole.size()) {
        const quint32 length = readU32(whole, offset);
        const quint32 type = readU32(whole, offset + 4);
        const int data = offset + 8;
        if (length > static_cast<quint32>(whole.size() - data)) {
            if (error) *error = QStringLiteral("glb chunk overruns the file");
            return false;
        }
        const QByteArray chunk = whole.mid(data, static_cast<int>(length));
        if (type == 0x4E4F534Au) {
            *json = chunk;
            sawJson = true;
        } else if (type == 0x004E4942u) {
            *bin = chunk;
        }
        offset = data + static_cast<int>(length);
    }
    if (!sawJson) {
        if (error) *error = QStringLiteral("glb has no JSON chunk");
        return false;
    }
    return true;
}

QByteArray writeGlb(const QByteArray& json, const QByteArray& bin)
{
    const QByteArray jsonPadded = pad4(json, ' ');
    const QByteArray binPadded = bin.isEmpty() ? QByteArray() : pad4(bin, '\0');
    const quint32 total = 12u
        + 8u + static_cast<quint32>(jsonPadded.size())
        + (binPadded.isEmpty() ? 0u : 8u + static_cast<quint32>(binPadded.size()));
    QByteArray out;
    out.reserve(static_cast<int>(total));
    appendU32(&out, 0x46546C67u);
    appendU32(&out, 2u);
    appendU32(&out, total);
    appendU32(&out, static_cast<quint32>(jsonPadded.size()));
    appendU32(&out, 0x4E4F534Au);
    out.append(jsonPadded);
    if (!binPadded.isEmpty()) {
        appendU32(&out, static_cast<quint32>(binPadded.size()));
        appendU32(&out, 0x004E4942u);
        out.append(binPadded);
    }
    return out;
}

bool suffixIsGltf(const QString& path, bool* binary)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("gltf")) {
        *binary = false;
        return true;
    }
    if (ext == QLatin1String("glb") || ext == QLatin1String("vrm")) {
        *binary = true;
        return true;
    }
    return false;
}

} // namespace

QString shapeId(Shape shape)
{
    switch (shape) {
    case Shape::Octahedron: return QStringLiteral("octahedron");
    case Shape::Sphere: return QStringLiteral("sphere");
    case Shape::Cube: return QStringLiteral("cube");
    case Shape::Arrow: return QStringLiteral("arrow");
    case Shape::Line: return QStringLiteral("line");
    case Shape::Circle: return QStringLiteral("circle");
    case Shape::Ring: return QStringLiteral("ring");
    case Shape::CustomMesh: return QStringLiteral("custom-mesh");
    }
    return QStringLiteral("octahedron");
}

bool shapeFromId(const QString& id, Shape* out)
{
    const QString key = id.trimmed().toLower();
    if (key == QLatin1String("octahedron") || key == QLatin1String("octa")) *out = Shape::Octahedron;
    else if (key == QLatin1String("sphere")) *out = Shape::Sphere;
    else if (key == QLatin1String("cube") || key == QLatin1String("box")) *out = Shape::Cube;
    else if (key == QLatin1String("arrow")) *out = Shape::Arrow;
    else if (key == QLatin1String("line")) *out = Shape::Line;
    else if (key == QLatin1String("circle")) *out = Shape::Circle;
    else if (key == QLatin1String("ring")) *out = Shape::Ring;
    else if (key == QLatin1String("custom-mesh") || key == QLatin1String("custom")) *out = Shape::CustomMesh;
    else return false;
    return true;
}

QStringList shapeIds()
{
    return {QStringLiteral("octahedron"), QStringLiteral("sphere"), QStringLiteral("cube"),
            QStringLiteral("arrow"), QStringLiteral("line"), QStringLiteral("circle"),
            QStringLiteral("ring"), QStringLiteral("custom-mesh")};
}

QString colorToHex(const Color& c)
{
    const auto byte = [](float v) {
        const int n = static_cast<int>(std::lround(clamp01(v) * 255.f));
        return QStringLiteral("%1").arg(n, 2, 16, QLatin1Char('0'));
    };
    return QStringLiteral("#") + byte(c.r) + byte(c.g) + byte(c.b) + byte(c.a);
}

bool colorFromHex(const QString& text, Color* out)
{
    QString s = text.trimmed();
    if (s.startsWith(QLatin1Char('#'))) s.remove(0, 1);
    if (s.size() != 6 && s.size() != 8) return false;
    const int r = hexByte(s, 0);
    const int g = hexByte(s, 2);
    const int b = hexByte(s, 4);
    if (r < 0 || g < 0 || b < 0) return false;
    int a = 166; // ~0.65, the transparent overlay default
    if (s.size() == 8) {
        a = hexByte(s, 6);
        if (a < 0) return false;
    }
    *out = Color{r / 255.f, g / 255.f, b / 255.f, a / 255.f};
    return true;
}

bool layerBit(std::uint32_t mask, int index)
{
    if (index < 0 || index >= kLayerCount) return false;
    return (mask & (1u << index)) != 0u;
}

bool setLayerBit(std::uint32_t* mask, int index, bool on)
{
    if (!mask || index < 0 || index >= kLayerCount) return false;
    const std::uint32_t bit = 1u << index;
    if (on) *mask |= bit;
    else *mask &= ~bit;
    return true;
}

Bone boneOrDefault(const Document& doc, const QString& bone)
{
    const auto it = doc.bones.constFind(bone);
    if (it == doc.bones.constEnd()) return Bone{};
    return it.value();
}

bool isDefault(const Document& doc)
{
    return doc.bones.isEmpty() && doc.groups.isEmpty()
        && doc.visibleLayers == kAllLayers && doc.isolatedGroup.isEmpty();
}

bool isVisible(const Document& doc, const QString& bone)
{
    const Bone b = boneOrDefault(doc, bone);
    if (!b.visible) return false;
    if ((b.layers & doc.visibleLayers) == 0u) return false;
    if (!doc.isolatedGroup.isEmpty() && !b.groups.contains(doc.isolatedGroup)) return false;
    return true;
}

bool canSelect(const Document& doc, const QString& bone)
{
    return isVisible(doc, bone) && !boneOrDefault(doc, bone).selectionLocked;
}

bool channelLocked(const Document& doc, const QString& bone, Channel channel)
{
    const Bone b = boneOrDefault(doc, bone);
    switch (channel) {
    case Channel::Translation: return b.lockTranslation;
    case Channel::Rotation: return b.lockRotation;
    case Channel::Scale: return b.lockScale;
    }
    return false;
}

Color effectiveColor(const Document& doc, const QString& bone, bool* hasColor)
{
    const Bone b = boneOrDefault(doc, bone);
    if (b.useGroupColor) {
        for (const QString& name : b.groups) {
            for (const Group& g : doc.groups) {
                if (g.name == name) {
                    if (hasColor) *hasColor = true;
                    return g.color;
                }
            }
        }
    }
    if (b.colorSet) {
        if (hasColor) *hasColor = true;
        return b.color;
    }
    if (hasColor) *hasColor = false;
    return Color{};
}

bool setShape(Document* doc, const QString& bone, Shape shape)
{
    if (!doc || bone.isEmpty()) return false;
    ensureBone(doc, bone).shape = shape;
    pruneBone(doc, bone);
    return true;
}

bool setCustomMesh(Document* doc, const QString& bone, const QString& entityName)
{
    if (!doc || bone.isEmpty()) return false;
    ensureBone(doc, bone).customMesh = entityName.trimmed();
    pruneBone(doc, bone);
    return true;
}

bool setScale(Document* doc, const QString& bone, const Vec3& scale)
{
    if (!doc || bone.isEmpty() || !finite3(scale) || scale.x == 0.f || scale.y == 0.f || scale.z == 0.f)
        return false;
    ensureBone(doc, bone).scale = scale;
    pruneBone(doc, bone);
    return true;
}

bool setOffset(Document* doc, const QString& bone, const Vec3& offset)
{
    if (!doc || bone.isEmpty() || !finite3(offset)) return false;
    ensureBone(doc, bone).offset = offset;
    pruneBone(doc, bone);
    return true;
}

bool setRotation(Document* doc, const QString& bone, const Vec3& rotationDeg)
{
    if (!doc || bone.isEmpty() || !finite3(rotationDeg)) return false;
    ensureBone(doc, bone).rotationDeg = rotationDeg;
    pruneBone(doc, bone);
    return true;
}

bool setColor(Document* doc, const QString& bone, const Color& color)
{
    if (!doc || bone.isEmpty() || !finiteColor(color)) return false;
    Bone& b = ensureBone(doc, bone);
    b.color = clampedColor(color);
    b.colorSet = true;
    pruneBone(doc, bone);
    return true;
}

bool setUseGroupColor(Document* doc, const QString& bone, bool on)
{
    if (!doc || bone.isEmpty()) return false;
    ensureBone(doc, bone).useGroupColor = on;
    pruneBone(doc, bone);
    return true;
}

bool setVisible(Document* doc, const QString& bone, bool visible)
{
    if (!doc || bone.isEmpty()) return false;
    ensureBone(doc, bone).visible = visible;
    pruneBone(doc, bone);
    return true;
}

bool setSelectionLocked(Document* doc, const QString& bone, bool locked)
{
    if (!doc || bone.isEmpty()) return false;
    ensureBone(doc, bone).selectionLocked = locked;
    pruneBone(doc, bone);
    return true;
}

bool setChannelLocked(Document* doc, const QString& bone, Channel channel, bool locked)
{
    if (!doc || bone.isEmpty()) return false;
    Bone& b = ensureBone(doc, bone);
    switch (channel) {
    case Channel::Translation: b.lockTranslation = locked; break;
    case Channel::Rotation: b.lockRotation = locked; break;
    case Channel::Scale: b.lockScale = locked; break;
    }
    pruneBone(doc, bone);
    return true;
}

bool setBoneLayer(Document* doc, const QString& bone, int index, bool on)
{
    if (!doc || bone.isEmpty()) return false;
    Bone& b = ensureBone(doc, bone);
    if (!setLayerBit(&b.layers, index, on)) return false;
    pruneBone(doc, bone);
    return true;
}

bool setVisibleLayer(Document* doc, int index, bool on)
{
    if (!doc) return false;
    return setLayerBit(&doc->visibleLayers, index, on);
}

bool addGroup(Document* doc, const QString& name, const Color& color, QString* error)
{
    if (!doc) return false;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        if (error) *error = QStringLiteral("group name is empty");
        return false;
    }
    if (knownGroup(*doc, trimmed)) {
        if (error) *error = QStringLiteral("group '%1' already exists").arg(trimmed);
        return false;
    }
    if (!finiteColor(color)) {
        if (error) *error = QStringLiteral("group color is not finite");
        return false;
    }
    doc->groups.append(Group{trimmed, clampedColor(color)});
    return true;
}

bool removeGroup(Document* doc, const QString& name)
{
    if (!doc || !knownGroup(*doc, name)) return false;
    for (auto it = doc->groups.begin(); it != doc->groups.end(); ++it) {
        if (it->name == name) {
            doc->groups.erase(it);
            break;
        }
    }
    for (auto it = doc->bones.begin(); it != doc->bones.end(); ++it)
        it->groups.removeAll(name);
    if (doc->isolatedGroup == name)
        doc->isolatedGroup.clear();
    const QStringList bones = doc->bones.keys();
    for (const QString& bone : bones)
        pruneBone(doc, bone);
    return true;
}

bool setGroupColor(Document* doc, const QString& name, const Color& color)
{
    if (!doc || !finiteColor(color)) return false;
    for (Group& g : doc->groups) {
        if (g.name == name) {
            g.color = clampedColor(color);
            return true;
        }
    }
    return false;
}

bool setMembership(Document* doc, const QString& bone, const QString& group, bool member)
{
    if (!doc || bone.isEmpty() || !knownGroup(*doc, group)) return false;
    Bone& b = ensureBone(doc, bone);
    if (member) {
        if (!b.groups.contains(group)) b.groups.append(group);
    } else {
        b.groups.removeAll(group);
    }
    pruneBone(doc, bone);
    return true;
}

bool setIsolatedGroup(Document* doc, const QString& name)
{
    if (!doc) return false;
    if (name.isEmpty()) {
        doc->isolatedGroup.clear();
        return true;
    }
    if (!knownGroup(*doc, name)) return false;
    doc->isolatedGroup = name;
    return true;
}

QJsonObject toJson(const Document& doc)
{
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QLatin1String(kSchema));
    root.insert(QStringLiteral("visibleLayers"), static_cast<double>(doc.visibleLayers));
    root.insert(QStringLiteral("isolatedGroup"), doc.isolatedGroup);
    QJsonArray groups;
    for (const Group& g : doc.groups) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), g.name);
        o.insert(QStringLiteral("color"), colorJson(g.color));
        groups.append(o);
    }
    root.insert(QStringLiteral("groups"), groups);
    QJsonObject bones;
    const QStringList names = doc.bones.keys();
    QStringList sorted = names;
    sorted.sort();
    for (const QString& name : sorted)
        bones.insert(name, boneToJson(doc.bones.value(name)));
    root.insert(QStringLiteral("bones"), bones);
    return root;
}

bool fromJson(const QJsonObject& obj, Document* out, QString* error)
{
    if (!out) return false;
    if (obj.value(QStringLiteral("schema")).toString() != QLatin1String(kSchema)) {
        if (error) *error = QStringLiteral("bone display schema mismatch");
        return false;
    }
    if (obj.contains(QStringLiteral("entities"))) {
        if (error) *error = QStringLiteral("scene bone-display documents use sceneFromJson");
        return false;
    }
    return fillDocument(obj, out, error);
}

Document prefixBoneNames(const Document& doc, const QString& prefix)
{
    if (prefix.isEmpty() || doc.bones.isEmpty()) return doc;
    Document out = doc;
    out.bones.clear();
    for (auto it = doc.bones.cbegin(); it != doc.bones.cend(); ++it)
        out.bones.insert(prefix + it.key(), it.value());
    return out;
}

Document matchImportedBoneNames(const Document& doc, const QString& prefix,
                                const QStringList& skeletonBones)
{
    if (prefix.isEmpty() || doc.bones.isEmpty() || skeletonBones.isEmpty()) return doc;
    const QSet<QString> names(skeletonBones.cbegin(), skeletonBones.cend());
    Document out = doc;
    out.bones.clear();
    for (auto it = doc.bones.cbegin(); it != doc.bones.cend(); ++it) {
        QString key = it.key();
        if (!names.contains(key)) {
            const QString prefixed = prefix + key;
            if (names.contains(prefixed))
                key = prefixed;
        }
        out.bones.insert(key, it.value());
    }
    return out;
}

QJsonObject sceneToJson(const QList<NamedDocument>& entities)
{
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QLatin1String(kSchema));
    QList<NamedDocument> sorted = entities;
    std::sort(sorted.begin(), sorted.end(), [](const NamedDocument& a, const NamedDocument& b) {
        return a.node < b.node;
    });
    QJsonArray arr;
    for (const NamedDocument& item : sorted) {
        if (item.node.isEmpty() || isDefault(item.document)) continue;
        QJsonObject entry = toJson(item.document);
        entry.remove(QStringLiteral("schema"));
        entry.insert(QStringLiteral("node"), item.node);
        arr.append(entry);
    }
    root.insert(QStringLiteral("entities"), arr);
    return root;
}

bool sceneFromJson(const QJsonObject& obj, QList<NamedDocument>* out, QString* error)
{
    if (!out) return false;
    if (obj.value(QStringLiteral("schema")).toString() != QLatin1String(kSchema)) {
        if (error) *error = QStringLiteral("bone display schema mismatch");
        return false;
    }
    if (!obj.contains(QStringLiteral("entities"))) {
        if (error) *error = QStringLiteral("scene bone-display document has no entities");
        return false;
    }
    QList<NamedDocument> loaded;
    const QJsonArray arr = obj.value(QStringLiteral("entities")).toArray();
    for (const QJsonValue& value : arr) {
        QJsonObject entry = value.toObject();
        const QString node = entry.value(QStringLiteral("node")).toString();
        if (node.isEmpty()) {
            if (error) *error = QStringLiteral("scene entity is missing a node name");
            return false;
        }
        entry.insert(QStringLiteral("schema"), QLatin1String(kSchema));
        entry.remove(QStringLiteral("entities"));
        Document doc;
        if (!fromJson(entry, &doc, error)) return false;
        loaded.append(NamedDocument{node, doc});
    }
    *out = loaded;
    return true;
}

QString sidecarPath(const QString& meshPath)
{
    const QFileInfo fi(meshPath);
    return fi.absoluteDir().filePath(fi.completeBaseName() + QStringLiteral(".bonedisplay.json"));
}

bool writeSidecarFile(const QString& path, const QJsonObject& obj, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("could not open %1").arg(path);
        return false;
    }
    const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("could not write %1").arg(path);
        return false;
    }
    return true;
}

bool readSidecarFile(const QString& path, QJsonObject* out, QString* error)
{
    if (!out) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("could not read %1").arg(path);
        return false;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("bone display sidecar is not JSON");
        return false;
    }
    *out = doc.object();
    return true;
}

void mergeGltfExtras(QJsonObject* gltfRoot, const QJsonObject& payload)
{
    if (!gltfRoot) return;
    QJsonArray scenes = gltfRoot->value(QStringLiteral("scenes")).toArray();
    QJsonObject scene = scenes.isEmpty() ? QJsonObject() : scenes.at(0).toObject();
    QJsonObject extras = scene.value(QStringLiteral("extras")).toObject();
    extras.insert(QLatin1String(kGltfExtrasKey), payload);
    scene.insert(QStringLiteral("extras"), extras);
    if (scenes.isEmpty()) scenes.append(scene);
    else scenes.replace(0, scene);
    gltfRoot->insert(QStringLiteral("scenes"), scenes);
}

QJsonObject gltfExtrasPayload(const QJsonObject& gltfRoot)
{
    const QJsonArray scenes = gltfRoot.value(QStringLiteral("scenes")).toArray();
    if (scenes.isEmpty()) return {};
    const QJsonObject extras = scenes.at(0).toObject().value(QStringLiteral("extras")).toObject();
    const QJsonValue value = extras.value(QLatin1String(kGltfExtrasKey));
    if (value.isObject()) return value.toObject();
    if (value.isString()) {
        const QJsonDocument doc = QJsonDocument::fromJson(value.toString().toUtf8());
        if (doc.isObject()) return doc.object();
    }
    return {};
}

bool writeGltfExtras(const QString& path, const QJsonObject& payload, QString* error)
{
    bool binary = false;
    if (!suffixIsGltf(path, &binary)) {
        if (error) *error = QStringLiteral("not a glTF file");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("could not read %1").arg(path);
        return false;
    }
    const QByteArray whole = file.readAll();
    file.close();

    QByteArray jsonBytes;
    QByteArray bin;
    if (binary) {
        if (!readGlb(whole, &jsonBytes, &bin, error)) return false;
    } else {
        jsonBytes = whole;
    }
    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("glTF JSON did not parse");
        return false;
    }
    QJsonObject root = doc.object();
    mergeGltfExtras(&root, payload);
    const QByteArray rewritten = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("could not open %1").arg(path);
        return false;
    }
    const QByteArray bytes = binary ? writeGlb(rewritten, bin) : rewritten;
    if (out.write(bytes) != bytes.size() || !out.commit()) {
        if (error) *error = QStringLiteral("could not write %1").arg(path);
        return false;
    }
    return true;
}

bool readGltfExtras(const QString& path, QJsonObject* payload, QString* error)
{
    if (!payload) return false;
    bool binary = false;
    if (!suffixIsGltf(path, &binary)) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("could not read %1").arg(path);
        return false;
    }
    const QByteArray whole = file.readAll();
    QByteArray jsonBytes;
    QByteArray bin;
    if (binary) {
        if (!readGlb(whole, &jsonBytes, &bin, error)) return false;
    } else {
        jsonBytes = whole;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("glTF JSON did not parse");
        return false;
    }
    const QJsonObject found = gltfExtrasPayload(doc.object());
    if (found.isEmpty()) return false;
    *payload = found;
    return true;
}

} // namespace BoneDisplay
