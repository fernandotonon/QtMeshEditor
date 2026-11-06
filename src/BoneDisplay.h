/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef BONEDISPLAY_H
#define BONEDISPLAY_H

// Bone display metadata (#561, epic #554 Slice G).
//
// Pure data: per-bone shape, custom-mesh proxy, display transform, colour,
// groups, 32 display layers, hide/show, selection lock, and per-channel
// transform locks. No Ogre and no scene, so membership, layer masks, and
// lock evaluation are unit-tested headless. SkeletonDebug reads a Document
// when it draws; BoneDisplayController stores one Document per entity and
// round-trips it through a `.bonedisplay.json` sidecar plus glTF scene extras.

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace BoneDisplay {

inline constexpr int kLayerCount = 32;
inline constexpr std::uint32_t kAllLayers = 0xFFFFFFFFu;
inline constexpr const char* kSchema = "qtmesh-bone-display-v1";
inline constexpr const char* kGltfExtrasKey = "qtmesh.bone.display";

enum class Shape {
    Octahedron = 0,
    Sphere,
    Cube,
    Arrow,
    Line,
    Circle,
    Ring,
    CustomMesh
};

enum class Channel { Translation, Rotation, Scale };

struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct Color {
    float r = 0.85f;
    float g = 0.85f;
    float b = 0.9f;
    float a = 0.65f;
    bool operator==(const Color& o) const
    {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
};

struct Bone {
    Shape shape = Shape::Octahedron;
    QString customMesh;
    Vec3 scale {1.f, 1.f, 1.f};
    Vec3 offset {0.f, 0.f, 0.f};
    Vec3 rotationDeg {0.f, 0.f, 0.f};
    Color color {};
    bool colorSet = false;
    bool useGroupColor = false;
    bool visible = true;
    bool selectionLocked = false;
    bool lockTranslation = false;
    bool lockRotation = false;
    bool lockScale = false;
    std::uint32_t layers = 1u; // bit 0 = layer 0
    QStringList groups;

    bool operator==(const Bone& o) const
    {
        return shape == o.shape && customMesh == o.customMesh && scale == o.scale
            && offset == o.offset && rotationDeg == o.rotationDeg && color == o.color
            && colorSet == o.colorSet && useGroupColor == o.useGroupColor
            && visible == o.visible && selectionLocked == o.selectionLocked
            && lockTranslation == o.lockTranslation && lockRotation == o.lockRotation
            && lockScale == o.lockScale && layers == o.layers && groups == o.groups;
    }
};

struct Group {
    QString name;
    Color color {1.f, 0.85f, 0.2f, 0.7f};
    bool operator==(const Group& o) const { return name == o.name && color == o.color; }
};

struct Document {
    QHash<QString, Bone> bones;
    QList<Group> groups;
    std::uint32_t visibleLayers = kAllLayers;
    QString isolatedGroup;
};

struct NamedDocument {
    QString node;
    Document document;
};

QString shapeId(Shape shape);
bool shapeFromId(const QString& id, Shape* out);
QStringList shapeIds();

QString colorToHex(const Color& c);
bool colorFromHex(const QString& text, Color* out);

bool layerBit(std::uint32_t mask, int index);
bool setLayerBit(std::uint32_t* mask, int index, bool on);

Bone boneOrDefault(const Document& doc, const QString& bone);
bool isDefault(const Document& doc);

bool isVisible(const Document& doc, const QString& bone);
bool canSelect(const Document& doc, const QString& bone);
bool channelLocked(const Document& doc, const QString& bone, Channel channel);
Color effectiveColor(const Document& doc, const QString& bone, bool* hasColor);

bool setShape(Document* doc, const QString& bone, Shape shape);
bool setCustomMesh(Document* doc, const QString& bone, const QString& entityName);
bool setScale(Document* doc, const QString& bone, const Vec3& scale);
bool setOffset(Document* doc, const QString& bone, const Vec3& offset);
bool setRotation(Document* doc, const QString& bone, const Vec3& rotationDeg);
bool setColor(Document* doc, const QString& bone, const Color& color);
bool setUseGroupColor(Document* doc, const QString& bone, bool on);
bool setVisible(Document* doc, const QString& bone, bool visible);
bool setSelectionLocked(Document* doc, const QString& bone, bool locked);
bool setChannelLocked(Document* doc, const QString& bone, Channel channel, bool locked);
bool setBoneLayer(Document* doc, const QString& bone, int index, bool on);
bool setVisibleLayer(Document* doc, int index, bool on);

bool addGroup(Document* doc, const QString& name, const Color& color, QString* error = nullptr);
bool removeGroup(Document* doc, const QString& name);
bool setGroupColor(Document* doc, const QString& name, const Color& color);
bool setMembership(Document* doc, const QString& bone, const QString& group, bool member);
bool setIsolatedGroup(Document* doc, const QString& name);

QJsonObject toJson(const Document& doc);
bool fromJson(const QJsonObject& obj, Document* out, QString* error = nullptr);

QJsonObject sceneToJson(const QList<NamedDocument>& entities);
bool sceneFromJson(const QJsonObject& obj, QList<NamedDocument>* out, QString* error = nullptr);

QString sidecarPath(const QString& meshPath);
bool writeSidecarFile(const QString& path, const QJsonObject& obj, QString* error = nullptr);
bool readSidecarFile(const QString& path, QJsonObject* out, QString* error = nullptr);

void mergeGltfExtras(QJsonObject* gltfRoot, const QJsonObject& payload);
QJsonObject gltfExtrasPayload(const QJsonObject& gltfRoot);
bool writeGltfExtras(const QString& path, const QJsonObject& payload, QString* error = nullptr);
bool readGltfExtras(const QString& path, QJsonObject* payload, QString* error = nullptr);

} // namespace BoneDisplay

#endif
