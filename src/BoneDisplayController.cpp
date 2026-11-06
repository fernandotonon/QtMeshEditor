#include "BoneDisplayController.h"

#include "AnimationControlController.h"
#include "Manager.h"
#include "PropertiesPanelController.h"
#include "SentryReporter.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QQmlEngine>

#include <OgreEntity.h>
#include <OgreSkeleton.h>

namespace {
QStringList skeletonBoneNames(Ogre::Entity* entity)
{
    QStringList names;
    if (!entity || !entity->hasSkeleton() || !entity->getMesh()) return names;
    const Ogre::SkeletonPtr skeleton = entity->getMesh()->getSkeleton();
    if (!skeleton) return names;
    const unsigned short count = skeleton->getNumBones();
    names.reserve(count);
    for (unsigned short i = 0; i < count; ++i) {
        if (Ogre::Bone* bone = skeleton->getBone(i))
            names.append(QString::fromStdString(bone->getName()));
    }
    return names;
}

const BoneDisplay::Color kGroupPalette[] = {
    {1.f, 0.85f, 0.2f, 0.7f},
    {0.35f, 0.75f, 1.f, 0.7f},
    {0.45f, 0.9f, 0.45f, 0.7f},
    {1.f, 0.45f, 0.45f, 0.7f},
    {0.75f, 0.5f, 1.f, 0.7f},
    {1.f, 0.6f, 0.25f, 0.7f},
};
}

BoneDisplayController* BoneDisplayController::instance()
{
    static BoneDisplayController* controller = new BoneDisplayController();
    controller->bindSignals();
    return controller;
}

BoneDisplayController* BoneDisplayController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    auto* controller = instance();
    QQmlEngine::setObjectOwnership(controller, QQmlEngine::CppOwnership);
    Q_UNUSED(engine);
    return controller;
}

BoneDisplayController::BoneDisplayController(QObject* parent)
    : QObject(parent)
{
}

void BoneDisplayController::bindSignals()
{
    if (!m_mgrBound) {
        if (auto* mgr = Manager::getSingletonPtr()) {
            connect(mgr, &Manager::sceneNodeDestroyed, this, [this](Ogre::SceneNode* const& node) {
                forgetNode(node);
            });
            connect(mgr, &Manager::sceneClearing, this, [this]() {
                if (m_docs.isEmpty()) return;
                m_docs.clear();
                emit displayChanged();
            });
            m_mgrBound = true;
        }
    }
    if (!m_animBound) {
        if (auto* anim = AnimationControlController::instance()) {
            connect(anim, &AnimationControlController::boneListChanged,
                    this, &BoneDisplayController::displayChanged);
            m_animBound = true;
        }
    }
}

void BoneDisplayController::forgetNode(Ogre::SceneNode* node)
{
    if (!node) return;
    bool changed = false;
    for (auto* obj : node->getAttachedObjects()) {
        if (!obj || obj->getMovableType() != "Entity") continue;
        changed = m_docs.remove(static_cast<Ogre::Entity*>(obj)) > 0 || changed;
    }
    if (changed) emit displayChanged();
}

void BoneDisplayController::note(const QString& category, const QString& message) const
{
    SentryReporter::addBreadcrumb(category, message);
}

Ogre::Entity* BoneDisplayController::selectedEntity() const
{
    auto* anim = AnimationControlController::instance();
    return anim ? anim->selectedEntity() : nullptr;
}

QString BoneDisplayController::selectedBoneName() const
{
    auto* anim = AnimationControlController::instance();
    return anim ? anim->selectedBone() : QString();
}

BoneDisplay::Document BoneDisplayController::documentFor(Ogre::Entity* entity) const
{
    if (!entity) return {};
    return m_docs.value(entity);
}

bool BoneDisplayController::isBoneShown(Ogre::Entity* entity, const QString& bone) const
{
    return BoneDisplay::isVisible(documentFor(entity), bone);
}

bool BoneDisplayController::canSelectBone(Ogre::Entity* entity, const QString& bone) const
{
    return BoneDisplay::canSelect(documentFor(entity), bone);
}

bool BoneDisplayController::isChannelLocked(Ogre::Entity* entity, const QString& bone,
                                            BoneDisplay::Channel channel) const
{
    return BoneDisplay::channelLocked(documentFor(entity), bone, channel);
}

BoneDisplay::Bone BoneDisplayController::selectedBone() const
{
    return BoneDisplay::boneOrDefault(documentFor(selectedEntity()), selectedBoneName());
}

BoneDisplay::Document* BoneDisplayController::mutableSelected(bool* haveBone)
{
    Ogre::Entity* entity = selectedEntity();
    const QString bone = selectedBoneName();
    if (haveBone) *haveBone = entity && !bone.isEmpty();
    if (!entity) return nullptr;
    return &m_docs[entity];
}

void BoneDisplayController::commitSelected(bool rebuildShapes)
{
    emit displayChanged();
    if (!rebuildShapes) return;
    Ogre::Entity* entity = selectedEntity();
    if (!entity) return;
    if (auto* props = PropertiesPanelController::instance())
        props->refreshSkeletonOverlays(QString::fromStdString(entity->getName()));
}

QString BoneDisplayController::bits(std::uint32_t mask)
{
    QString out(BoneDisplay::kLayerCount, QLatin1Char('0'));
    for (int i = 0; i < BoneDisplay::kLayerCount; ++i)
        if (BoneDisplay::layerBit(mask, i))
            out[i] = QLatin1Char('1');
    return out;
}

bool BoneDisplayController::hasBone() const
{
    return selectedEntity() && !selectedBoneName().isEmpty();
}

int BoneDisplayController::shapeIndex() const
{
    return static_cast<int>(selectedBone().shape);
}

QString BoneDisplayController::customMesh() const
{
    return selectedBone().customMesh;
}

QStringList BoneDisplayController::proxyChoices() const
{
    QStringList names;
    names << QStringLiteral("(none)");
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return names;
    Ogre::Entity* self = selectedEntity();
    for (Ogre::Entity* entity : mgr->getEntities()) {
        if (!entity || entity == self || !entity->getMesh()) continue;
        const QString name = QString::fromStdString(entity->getName());
        if (name.isEmpty() || name.contains(QLatin1String("restGhost"))) continue;
        names << name;
    }
    names.removeDuplicates();
    return names;
}

int BoneDisplayController::customMeshIndex() const
{
    const QStringList choices = proxyChoices();
    const int index = choices.indexOf(customMesh());
    return index < 0 ? 0 : index;
}

double BoneDisplayController::scaleX() const { return selectedBone().scale.x; }
double BoneDisplayController::scaleY() const { return selectedBone().scale.y; }
double BoneDisplayController::scaleZ() const { return selectedBone().scale.z; }
double BoneDisplayController::offsetX() const { return selectedBone().offset.x; }
double BoneDisplayController::offsetY() const { return selectedBone().offset.y; }
double BoneDisplayController::offsetZ() const { return selectedBone().offset.z; }
double BoneDisplayController::rotationX() const { return selectedBone().rotationDeg.x; }
double BoneDisplayController::rotationY() const { return selectedBone().rotationDeg.y; }
double BoneDisplayController::rotationZ() const { return selectedBone().rotationDeg.z; }
bool BoneDisplayController::boneVisible() const { return selectedBone().visible; }
bool BoneDisplayController::selectionLocked() const { return selectedBone().selectionLocked; }
bool BoneDisplayController::lockTranslation() const { return selectedBone().lockTranslation; }
bool BoneDisplayController::lockRotation() const { return selectedBone().lockRotation; }
bool BoneDisplayController::lockScale() const { return selectedBone().lockScale; }
bool BoneDisplayController::useGroupColor() const { return selectedBone().useGroupColor; }

QString BoneDisplayController::colorHex() const
{
    const BoneDisplay::Bone bone = selectedBone();
    if (!bone.colorSet) return QStringLiteral("#d9d9e6a6");
    return BoneDisplay::colorToHex(bone.color);
}

QStringList BoneDisplayController::groupNames() const
{
    QStringList names;
    for (const BoneDisplay::Group& group : documentFor(selectedEntity()).groups)
        names << group.name;
    return names;
}

QStringList BoneDisplayController::groupColors() const
{
    QStringList colors;
    for (const BoneDisplay::Group& group : documentFor(selectedEntity()).groups)
        colors << BoneDisplay::colorToHex(group.color);
    return colors;
}

QStringList BoneDisplayController::boneGroups() const
{
    return selectedBone().groups;
}

QString BoneDisplayController::isolatedGroup() const
{
    return documentFor(selectedEntity()).isolatedGroup;
}

QString BoneDisplayController::boneLayerBits() const
{
    return bits(selectedBone().layers);
}

QString BoneDisplayController::visibleLayerBits() const
{
    return bits(documentFor(selectedEntity()).visibleLayers);
}

void BoneDisplayController::setShapeIndex(int index)
{
    const QStringList ids = shapeIds();
    if (index < 0 || index >= ids.size()) return;
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have) return;
    BoneDisplay::Shape shape;
    if (!BoneDisplay::shapeFromId(ids.at(index), &shape)) return;
    if (!BoneDisplay::setShape(doc, selectedBoneName(), shape)) return;
    note(QStringLiteral("scene.skel.display.shape"),
         QStringLiteral("%1 → %2").arg(selectedBoneName(), ids.at(index)));
    commitSelected(true);
}

void BoneDisplayController::setCustomMeshIndex(int index)
{
    const QStringList choices = proxyChoices();
    if (index < 0 || index >= choices.size()) return;
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have) return;
    const QString name = index == 0 ? QString() : choices.at(index);
    if (!BoneDisplay::setCustomMesh(doc, selectedBoneName(), name)) return;
    note(QStringLiteral("scene.skel.display.shape"),
         QStringLiteral("custom mesh %1 on %2").arg(name.isEmpty() ? QStringLiteral("(none)") : name,
                                                    selectedBoneName()));
    commitSelected(true);
}

void BoneDisplayController::setScaleAxis(int axis, double value)
{
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have || axis < 0 || axis > 2) return;
    BoneDisplay::Vec3 scale = selectedBone().scale;
    if (axis == 0) scale.x = static_cast<float>(value);
    else if (axis == 1) scale.y = static_cast<float>(value);
    else scale.z = static_cast<float>(value);
    if (!BoneDisplay::setScale(doc, selectedBoneName(), scale)) return;
    note(QStringLiteral("scene.skel.display.shape"),
         QStringLiteral("scale %1").arg(selectedBoneName()));
    commitSelected(true);
}

void BoneDisplayController::setOffsetAxis(int axis, double value)
{
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have || axis < 0 || axis > 2) return;
    BoneDisplay::Vec3 offset = selectedBone().offset;
    if (axis == 0) offset.x = static_cast<float>(value);
    else if (axis == 1) offset.y = static_cast<float>(value);
    else offset.z = static_cast<float>(value);
    if (!BoneDisplay::setOffset(doc, selectedBoneName(), offset)) return;
    note(QStringLiteral("scene.skel.display.shape"),
         QStringLiteral("offset %1").arg(selectedBoneName()));
    commitSelected(true);
}

void BoneDisplayController::setRotationAxis(int axis, double value)
{
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have || axis < 0 || axis > 2) return;
    BoneDisplay::Vec3 rotation = selectedBone().rotationDeg;
    if (axis == 0) rotation.x = static_cast<float>(value);
    else if (axis == 1) rotation.y = static_cast<float>(value);
    else rotation.z = static_cast<float>(value);
    if (!BoneDisplay::setRotation(doc, selectedBoneName(), rotation)) return;
    note(QStringLiteral("scene.skel.display.shape"),
         QStringLiteral("rotation %1").arg(selectedBoneName()));
    commitSelected(true);
}

void BoneDisplayController::setColorHex(const QString& hex)
{
    BoneDisplay::Color color;
    if (!BoneDisplay::colorFromHex(hex, &color)) return;
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have) return;
    if (!BoneDisplay::setColor(doc, selectedBoneName(), color)) return;
    note(QStringLiteral("scene.skel.display.color"),
         QStringLiteral("%1 %2").arg(selectedBoneName(), BoneDisplay::colorToHex(color)));
    commitSelected(false);
}

void BoneDisplayController::setUseGroupColor(bool on)
{
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have) return;
    if (!BoneDisplay::setUseGroupColor(doc, selectedBoneName(), on)) return;
    note(QStringLiteral("scene.skel.display.color"),
         QStringLiteral("%1 group color %2").arg(selectedBoneName(), on ? QStringLiteral("on")
                                                                        : QStringLiteral("off")));
    commitSelected(false);
}

void BoneDisplayController::setBoneVisible(bool visible)
{
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have) return;
    if (!BoneDisplay::setVisible(doc, selectedBoneName(), visible)) return;
    note(QStringLiteral("scene.skel.display.visibility"),
         QStringLiteral("%1 %2").arg(selectedBoneName(), visible ? QStringLiteral("shown")
                                                                : QStringLiteral("hidden")));
    commitSelected(false);
}

void BoneDisplayController::setSelectionLocked(bool locked)
{
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have) return;
    if (!BoneDisplay::setSelectionLocked(doc, selectedBoneName(), locked)) return;
    note(QStringLiteral("scene.skel.display.lock"),
         QStringLiteral("%1 selection %2").arg(selectedBoneName(), locked ? QStringLiteral("locked")
                                                                         : QStringLiteral("unlocked")));
    commitSelected(false);
}

void BoneDisplayController::setLockTranslation(bool locked)
{
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have) return;
    if (!BoneDisplay::setChannelLocked(doc, selectedBoneName(), BoneDisplay::Channel::Translation, locked))
        return;
    note(QStringLiteral("scene.skel.display.lock"),
         QStringLiteral("%1 translation %2").arg(selectedBoneName(), locked ? QStringLiteral("locked")
                                                                           : QStringLiteral("unlocked")));
    commitSelected(false);
}

void BoneDisplayController::setLockRotation(bool locked)
{
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have) return;
    if (!BoneDisplay::setChannelLocked(doc, selectedBoneName(), BoneDisplay::Channel::Rotation, locked))
        return;
    note(QStringLiteral("scene.skel.display.lock"),
         QStringLiteral("%1 rotation %2").arg(selectedBoneName(), locked ? QStringLiteral("locked")
                                                                        : QStringLiteral("unlocked")));
    commitSelected(false);
}

void BoneDisplayController::setLockScale(bool locked)
{
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have) return;
    if (!BoneDisplay::setChannelLocked(doc, selectedBoneName(), BoneDisplay::Channel::Scale, locked))
        return;
    note(QStringLiteral("scene.skel.display.lock"),
         QStringLiteral("%1 scale %2").arg(selectedBoneName(), locked ? QStringLiteral("locked")
                                                                     : QStringLiteral("unlocked")));
    commitSelected(false);
}

void BoneDisplayController::setBoneOnLayer(int index, bool on)
{
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have) return;
    if (!BoneDisplay::setBoneLayer(doc, selectedBoneName(), index, on)) return;
    note(QStringLiteral("scene.skel.display.layer"),
         QStringLiteral("%1 layer %2 %3").arg(selectedBoneName()).arg(index).arg(on ? QStringLiteral("on")
                                                                                   : QStringLiteral("off")));
    commitSelected(false);
}

void BoneDisplayController::setLayerVisible(int index, bool on)
{
    Ogre::Entity* entity = selectedEntity();
    if (!entity) return;
    if (!BoneDisplay::setVisibleLayer(&m_docs[entity], index, on)) return;
    note(QStringLiteral("scene.skel.display.layer"),
         QStringLiteral("visible layer %1 %2").arg(index).arg(on ? QStringLiteral("on")
                                                                : QStringLiteral("off")));
    commitSelected(false);
}

QString BoneDisplayController::addGroup(const QString& name)
{
    Ogre::Entity* entity = selectedEntity();
    if (!entity) {
        m_groupError = QStringLiteral("Select a bone before adding a group");
        emit displayChanged();
        return m_groupError;
    }
    const BoneDisplay::Color color = kGroupPalette[m_docs[entity].groups.size() % 6];
    QString error;
    if (!BoneDisplay::addGroup(&m_docs[entity], name, color, &error)) {
        m_groupError = error.isEmpty() ? QStringLiteral("Could not add the group") : error;
        emit displayChanged();
        return m_groupError;
    }
    m_groupError.clear();
    note(QStringLiteral("scene.skel.display.group"),
         QStringLiteral("add %1").arg(name.trimmed()));
    commitSelected(false);
    return {};
}

void BoneDisplayController::rekeyBone(Ogre::Entity* entity, const QString& from, const QString& to)
{
    if (!entity) return;
    auto it = m_docs.find(entity);
    if (it == m_docs.end()) return;
    if (!BoneDisplay::renameBoneKey(&it.value(), from, to)) return;
    emit displayChanged();
}

void BoneDisplayController::removeGroup(const QString& name)
{
    Ogre::Entity* entity = selectedEntity();
    if (!entity) return;
    if (!BoneDisplay::removeGroup(&m_docs[entity], name)) return;
    note(QStringLiteral("scene.skel.display.group"),
         QStringLiteral("remove %1").arg(name));
    commitSelected(false);
}

void BoneDisplayController::setGroupMember(const QString& name, bool member)
{
    bool have = false;
    BoneDisplay::Document* doc = mutableSelected(&have);
    if (!have) return;
    if (!BoneDisplay::setMembership(doc, selectedBoneName(), name, member)) return;
    note(QStringLiteral("scene.skel.display.group"),
         QStringLiteral("%1 %2 %3").arg(selectedBoneName(), member ? QStringLiteral("joins")
                                                                  : QStringLiteral("leaves"),
                                        name));
    commitSelected(false);
}

void BoneDisplayController::setGroupColorHex(const QString& name, const QString& hex)
{
    Ogre::Entity* entity = selectedEntity();
    if (!entity) return;
    BoneDisplay::Color color;
    if (!BoneDisplay::colorFromHex(hex, &color)) return;
    if (!BoneDisplay::setGroupColor(&m_docs[entity], name, color)) return;
    note(QStringLiteral("scene.skel.display.color"),
         QStringLiteral("group %1 %2").arg(name, BoneDisplay::colorToHex(color)));
    commitSelected(false);
}

void BoneDisplayController::isolateGroup(const QString& name)
{
    Ogre::Entity* entity = selectedEntity();
    if (!entity) return;
    const QString next = documentFor(entity).isolatedGroup == name ? QString() : name;
    if (!BoneDisplay::setIsolatedGroup(&m_docs[entity], next)) return;
    note(QStringLiteral("scene.skel.display.isolate"),
         next.isEmpty() ? QStringLiteral("cleared") : QStringLiteral("isolate %1").arg(next));
    commitSelected(false);
}

bool BoneDisplayController::saveEntity(Ogre::Entity* entity, const QString& meshPath)
{
    if (!entity || meshPath.isEmpty()) return false;
    const BoneDisplay::Document doc = documentFor(entity);
    const QString path = BoneDisplay::sidecarPath(meshPath);
    if (BoneDisplay::isDefault(doc)) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            note(QStringLiteral("scene.skel.display.save"),
                 QStringLiteral("could not remove %1").arg(path));
            return false;
        }
    } else {
        QString error;
        if (!BoneDisplay::writeSidecarFile(path, BoneDisplay::toJson(doc), &error)) {
            note(QStringLiteral("scene.skel.display.save"), error);
            return false;
        }
        const QString ext = QFileInfo(meshPath).suffix().toLower();
        if (ext == QLatin1String("gltf") || ext == QLatin1String("glb") || ext == QLatin1String("vrm")) {
            if (!BoneDisplay::writeGltfExtras(meshPath, BoneDisplay::toJson(doc), &error)) {
                note(QStringLiteral("scene.skel.display.save"), error);
                return false;
            }
        }
    }
    note(QStringLiteral("scene.skel.display.save"), path);
    return true;
}

bool BoneDisplayController::loadEntity(Ogre::Entity* entity, const QString& meshPath)
{
    if (!entity || meshPath.isEmpty()) return false;
    BoneDisplay::Document doc;
    const QString path = BoneDisplay::sidecarPath(meshPath);
    bool loaded = false;
    if (QFileInfo::exists(path)) {
        QJsonObject obj;
        QString error;
        if (!BoneDisplay::readSidecarFile(path, &obj, &error) || !BoneDisplay::fromJson(obj, &doc, &error)) {
            note(QStringLiteral("scene.skel.display.load"), error);
            return false;
        }
        loaded = true;
    } else {
        QJsonObject extras;
        if (!BoneDisplay::readGltfExtras(meshPath, &extras, nullptr)) return true;
        if (extras.contains(QStringLiteral("entities"))) {
            QList<BoneDisplay::NamedDocument> scene;
            QString error;
            if (!BoneDisplay::sceneFromJson(extras, &scene, &error)) {
                note(QStringLiteral("scene.skel.display.load"), error);
                return false;
            }
            const QString node = entity->getParentSceneNode()
                ? QString::fromStdString(entity->getParentSceneNode()->getName()) : QString();
            const QString entName = QString::fromStdString(entity->getName());
            for (const BoneDisplay::NamedDocument& item : scene) {
                if (item.node == node || item.node == entName) {
                    doc = item.document;
                    loaded = true;
                    break;
                }
            }
        } else {
            QString error;
            if (!BoneDisplay::fromJson(extras, &doc, &error)) {
                note(QStringLiteral("scene.skel.display.load"), error);
                return false;
            }
            loaded = true;
        }
    }
    if (!loaded) return true;
    if (BoneDisplay::isDefault(doc)) m_docs.remove(entity);
    else m_docs.insert(entity, doc);
    note(QStringLiteral("scene.skel.display.load"), path);
    emit displayChanged();
    return true;
}

bool BoneDisplayController::saveScene(const QHash<QString, Ogre::Entity*>& nodesToEntities,
                                      const QString& scenePath)
{
    if (scenePath.isEmpty()) return false;
    QList<BoneDisplay::NamedDocument> entities;
    // buildSceneAiScene prefixes every bone with "<node>_" once a scene has
    // more than one entity. Write those exported names so a reload finds them.
    const bool prefixBones = nodesToEntities.size() > 1;
    for (auto it = nodesToEntities.cbegin(); it != nodesToEntities.cend(); ++it) {
        if (!it.value()) continue;
        BoneDisplay::Document doc = documentFor(it.value());
        if (prefixBones && it.value()->hasSkeleton())
            doc = BoneDisplay::prefixBoneNames(doc, it.key() + QLatin1Char('_'));
        entities.append(BoneDisplay::NamedDocument{it.key(), doc});
    }
    const QJsonObject json = BoneDisplay::sceneToJson(entities);
    const QString path = BoneDisplay::sidecarPath(scenePath);
    const QJsonArray stored = json.value(QStringLiteral("entities")).toArray();
    if (stored.isEmpty()) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            note(QStringLiteral("scene.skel.display.save"),
                 QStringLiteral("could not remove %1").arg(path));
            return false;
        }
    } else {
        QString error;
        if (!BoneDisplay::writeSidecarFile(path, json, &error)) {
            note(QStringLiteral("scene.skel.display.save"), error);
            return false;
        }
        const QString ext = QFileInfo(scenePath).suffix().toLower();
        if (ext == QLatin1String("gltf") || ext == QLatin1String("glb") || ext == QLatin1String("vrm")) {
            if (!BoneDisplay::writeGltfExtras(scenePath, json, &error)) {
                note(QStringLiteral("scene.skel.display.save"), error);
                return false;
            }
        }
    }
    note(QStringLiteral("scene.skel.display.save"), path);
    return true;
}

bool BoneDisplayController::loadScene(const QHash<QString, Ogre::Entity*>& nodesToEntities,
                                      const QString& scenePath)
{
    if (scenePath.isEmpty() || nodesToEntities.isEmpty()) return false;
    QList<BoneDisplay::NamedDocument> scene;
    const QString path = BoneDisplay::sidecarPath(scenePath);
    bool fromFile = false;
    if (QFileInfo::exists(path)) {
        QJsonObject obj;
        QString error;
        if (!BoneDisplay::readSidecarFile(path, &obj, &error)
            || !BoneDisplay::sceneFromJson(obj, &scene, &error)) {
            note(QStringLiteral("scene.skel.display.load"), error);
            return false;
        }
        fromFile = true;
    } else {
        QJsonObject extras;
        if (!BoneDisplay::readGltfExtras(scenePath, &extras, nullptr)) return true;
        QString error;
        if (!BoneDisplay::sceneFromJson(extras, &scene, &error)) {
            BoneDisplay::Document single;
            if (nodesToEntities.size() == 1 && !extras.contains(QStringLiteral("entities"))
                && BoneDisplay::fromJson(extras, &single, &error)) {
                scene.append(BoneDisplay::NamedDocument{nodesToEntities.constBegin().key(), single});
            } else {
                note(QStringLiteral("scene.skel.display.load"), error);
                return false;
            }
        }
        fromFile = true;
    }
    if (!fromFile) return true;
    for (const BoneDisplay::NamedDocument& item : scene) {
        Ogre::Entity* entity = nodesToEntities.value(item.node);
        if (!entity) continue;
        BoneDisplay::Document doc = item.document;
        if (nodesToEntities.size() > 1) {
            doc = BoneDisplay::matchImportedBoneNames(
                doc, item.node + QLatin1Char('_'), skeletonBoneNames(entity));
        }
        if (BoneDisplay::isDefault(doc)) m_docs.remove(entity);
        else m_docs.insert(entity, doc);
    }
    note(QStringLiteral("scene.skel.display.load"), path);
    emit displayChanged();
    return true;
}
