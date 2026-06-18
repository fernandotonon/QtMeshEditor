#include "SceneTreeModel.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "GlobalDefinitions.h"
#include "UndoManager.h"
#include "commands/TransformCommands.h"
#include "SentryReporter.h"
#include <QTimer>
#include <Ogre.h>

// ---- SceneTreeItem ----

SceneTreeItem::SceneTreeItem(const QString& name, ItemType type, void* ogrePtr, SceneTreeItem* parent)
    : mName(name), mType(type), mOgrePtr(ogrePtr), mParent(parent)
{
}

SceneTreeItem::~SceneTreeItem()
{
    qDeleteAll(mChildren);
}

void SceneTreeItem::appendChild(SceneTreeItem* child)
{
    mChildren.append(child);
}

SceneTreeItem* SceneTreeItem::child(int row) const
{
    if (row < 0 || row >= mChildren.size()) return nullptr;
    return mChildren.at(row);
}

int SceneTreeItem::childCount() const { return mChildren.size(); }

int SceneTreeItem::row() const
{
    if (mParent)
        return mParent->mChildren.indexOf(const_cast<SceneTreeItem*>(this));
    return 0;
}

SceneTreeItem* SceneTreeItem::parentItem() const { return mParent; }

QString SceneTreeItem::typeLabel() const
{
    switch (mType) {
    case Root:      return "Scene";
    case Node: {
        // Show "Group" for empty scene nodes that have children (groups)
        auto* sn = static_cast<Ogre::SceneNode*>(mOgrePtr);
        if (sn && sn->numAttachedObjects() == 0 && sn->numChildren() > 0)
            return "Group";
        return "Node";
    }
    case Entity:    return "Mesh";
    case SubEntity: return "Submesh";
    }
    return "";
}

// ---- SceneTreeModel ----

SceneTreeModel::SceneTreeModel(QObject* parent)
    : QAbstractItemModel(parent)
{
    mRebuildTimer = new QTimer(this);
    mRebuildTimer->setSingleShot(true);
    mRebuildTimer->setInterval(50); // Debounce: coalesce rapid signals
    connect(mRebuildTimer, &QTimer::timeout, this, &SceneTreeModel::rebuild);

    rebuild();

    auto scheduleRebuild = [this]() { mRebuildTimer->start(); };
    connect(Manager::getSingleton(), &Manager::sceneNodeCreated, this, scheduleRebuild);
    connect(Manager::getSingleton(), &Manager::sceneNodeDestroyed, this, scheduleRebuild);
    connect(Manager::getSingleton(), &Manager::entityCreated, this, scheduleRebuild);
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged, this, &SceneTreeModel::updateSelection);
    // The user wants the material picker to refresh on selection change too, so
    // a freshly-selected entity's materials are immediately offered.
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &SceneTreeModel::materialsChanged);
}

SceneTreeModel::~SceneTreeModel()
{
    delete mRootItem;
}

void SceneTreeModel::rebuild()
{
    beginResetModel();
    delete mRootItem;

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* rootNode = sceneMgr->getRootSceneNode();

    mRootItem = new SceneTreeItem("Scene", SceneTreeItem::Root, rootNode, nullptr);
    buildChildren(rootNode, mRootItem);

    endResetModel();
    // A rebuild follows model loads / node + entity creation, any of which can
    // introduce new materials — tell QML pickers to refresh their lists.
    emit materialsChanged();
}

void SceneTreeModel::buildChildren(Ogre::SceneNode* sceneNode, SceneTreeItem* parentItem)
{
    auto children = sceneNode->getChildren();
    for (const auto& child : children)
    {
        auto* childNode = static_cast<Ogre::SceneNode*>(child);
        QString name = QString::fromStdString(childNode->getName());

        if (name.isEmpty() || Manager::getSingleton()->isForbiddenNodeName(name))
            continue;

        // Hide transient gizmo scaffolding that editor code creates as
        // children of the root scene node (currently: BevelGizmo's handle
        // rig). Matched on the exact names the gizmo creates — see
        // BevelGizmo.cpp — so a user mesh sharing the suffix doesn't
        // vanish from the tree. A future gizmo with different names
        // needs to add its own entries here.
        if (name == "BevelGizmo_Node"
         || name == "BevelGizmo_Shaft"
         || name == "BevelGizmo_Handle")
            continue;

        auto* nodeItem = new SceneTreeItem(name, SceneTreeItem::Node, childNode, parentItem);
        parentItem->appendChild(nodeItem);

        // Add entities
        for (int i = 0; i < childNode->numAttachedObjects(); ++i)
        {
            Ogre::MovableObject* obj = childNode->getAttachedObject(i);
            if (!obj || obj->getMovableType() != "Entity") continue;

            auto* entity = static_cast<Ogre::Entity*>(obj);
            QString entName = QString::fromStdString(entity->getName());
            auto* entItem = new SceneTreeItem(entName, SceneTreeItem::Entity, entity, nodeItem);
            nodeItem->appendChild(entItem);

            // Add sub-entities
            for (unsigned int s = 0; s < entity->getNumSubEntities(); ++s)
            {
                Ogre::SubEntity* sub = entity->getSubEntity(s);
                QString subName = QString::number(s);
                auto* subItem = new SceneTreeItem(subName, SceneTreeItem::SubEntity, sub, entItem);
                entItem->appendChild(subItem);
            }
        }

        // Recurse child nodes
        if (childNode->numChildren() > 0)
            buildChildren(childNode, nodeItem);
    }
}

QModelIndex SceneTreeModel::index(int row, int column, const QModelIndex& parent) const
{
    if (!hasIndex(row, column, parent))
        return QModelIndex();

    SceneTreeItem* parentItem = parent.isValid()
        ? static_cast<SceneTreeItem*>(parent.internalPointer())
        : mRootItem;

    SceneTreeItem* childItem = parentItem->child(row);
    if (childItem)
        return createIndex(row, column, childItem);
    return QModelIndex();
}

QModelIndex SceneTreeModel::parent(const QModelIndex& child) const
{
    if (!child.isValid())
        return QModelIndex();

    auto* childItem = static_cast<SceneTreeItem*>(child.internalPointer());
    SceneTreeItem* parentItem = childItem->parentItem();

    if (!parentItem || parentItem == mRootItem)
        return QModelIndex();

    return createIndex(parentItem->row(), 0, parentItem);
}

int SceneTreeModel::rowCount(const QModelIndex& parent) const
{
    if (!mRootItem) return 0;

    SceneTreeItem* parentItem = parent.isValid()
        ? static_cast<SceneTreeItem*>(parent.internalPointer())
        : mRootItem;

    return parentItem->childCount();
}

int SceneTreeModel::columnCount(const QModelIndex&) const { return 1; }

QVariant SceneTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) return QVariant();

    auto* item = static_cast<SceneTreeItem*>(index.internalPointer());

    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return item->name();
    case TypeRole:
        return static_cast<int>(item->type());
    case TypeLabelRole:
        return item->typeLabel();
    case SelectedRole: {
        auto* sel = SelectionSet::getSingleton();
        switch (item->type()) {
        case SceneTreeItem::Node:
            return sel->contains(static_cast<Ogre::SceneNode*>(item->ogrePtr()));
        case SceneTreeItem::Entity:
            return sel->contains(static_cast<Ogre::Entity*>(item->ogrePtr()));
        case SceneTreeItem::SubEntity:
            return sel->contains(static_cast<Ogre::SubEntity*>(item->ogrePtr()));
        default:
            return false;
        }
    }
    case MaterialNameRole: {
        if (item->type() == SceneTreeItem::SubEntity) {
            auto* sub = static_cast<Ogre::SubEntity*>(item->ogrePtr());
            return QString::fromStdString(sub->getMaterialName());
        }
        if (item->type() == SceneTreeItem::Entity) {
            auto* ent = static_cast<Ogre::Entity*>(item->ogrePtr());
            if (ent->getNumSubEntities() > 0)
                return QString::fromStdString(ent->getSubEntity(0)->getMaterialName());
        }
        return QVariant();
    }
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> SceneTreeModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[NameRole] = "name";
    roles[TypeRole] = "itemType";
    roles[TypeLabelRole] = "typeLabel";
    roles[SelectedRole] = "isItemSelected";
    roles[MaterialNameRole] = "materialName";
    return roles;
}

SceneTreeItem* SceneTreeModel::itemFromIndex(const QModelIndex& index) const
{
    if (!index.isValid()) return nullptr;
    return static_cast<SceneTreeItem*>(index.internalPointer());
}

QModelIndex SceneTreeModel::rootIndex() const
{
    return QModelIndex();
}

void SceneTreeModel::selectItem(int row, const QModelIndex& parentIndex, bool multiSelect)
{
    QModelIndex idx = index(row, 0, parentIndex);
    auto* item = itemFromIndex(idx);
    if (!item) return;

    auto* sel = SelectionSet::getSingleton();

    if (!multiSelect)
        sel->clear();

    switch (item->type()) {
    case SceneTreeItem::Node: {
        auto* node = static_cast<Ogre::SceneNode*>(item->ogrePtr());
        if (multiSelect && sel->contains(node))
            sel->removeOne(node);
        else
            sel->append(node);
        break;
    }
    case SceneTreeItem::Entity: {
        auto* entity = static_cast<Ogre::Entity*>(item->ogrePtr());
        if (multiSelect && sel->contains(entity))
            sel->removeOne(entity);
        else
            sel->append(entity);
        break;
    }
    case SceneTreeItem::SubEntity: {
        auto* sub = static_cast<Ogre::SubEntity*>(item->ogrePtr());
        if (multiSelect && sel->contains(sub))
            sel->removeOne(sub);
        else
            sel->append(sub);
        break;
    }
    default:
        break;
    }
}

bool SceneTreeModel::isSelected(int row, const QModelIndex& parentIndex) const
{
    QModelIndex idx = index(row, 0, parentIndex);
    return data(idx, SelectedRole).toBool();
}

QString SceneTreeModel::materialName(int row, const QModelIndex& parentIndex) const
{
    QModelIndex idx = index(row, 0, parentIndex);
    return data(idx, MaterialNameRole).toString();
}

void SceneTreeModel::setMaterial(int row, const QModelIndex& parentIndex, const QString& matName)
{
    QModelIndex idx = index(row, 0, parentIndex);
    auto* item = itemFromIndex(idx);
    if (!item) return;

    std::string stdName = matName.toStdString();

    if (item->type() == SceneTreeItem::SubEntity) {
        auto* sub = static_cast<Ogre::SubEntity*>(item->ogrePtr());
        sub->setMaterialName(stdName);
        emit dataChanged(idx, idx, {MaterialNameRole});
    }
    else if (item->type() == SceneTreeItem::Entity) {
        auto* ent = static_cast<Ogre::Entity*>(item->ogrePtr());
        ent->setMaterialName(stdName);
        emit dataChanged(idx, idx, {MaterialNameRole});
    }
}

QStringList SceneTreeModel::availableMaterials() const
{
    QStringList names;
    auto it = Ogre::MaterialManager::getSingleton().getResourceIterator();
    while (it.hasMoreElements())
    {
        auto res = it.getNext();
        QString name = QString::fromStdString(res->getName());
        // Skip Ogre's built-in materials.
        if (name.startsWith("Ogre/") || name.startsWith("BaseWhite") || name == "GUI_Material")
            continue;
        // Skip runtime paint-pipeline materials. These are created by
        // TexturePaintController (paint session, mask overlay, hover
        // ring) and aren't user-authored — they'd just clutter the
        // submesh material dropdown.
        if (name.startsWith("QMEPaintMaskOverlay_")
         || name.startsWith("QMEPaint_")
         || name.startsWith("TexturePaint/"))
            continue;
        names.append(name);
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool SceneTreeModel::canReparent(const QString& nodeName, const QString& newParentName) const
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return false;

    auto* sceneMgr = mgr->getSceneMgr();
    if (!sceneMgr) return false;

    // Node must exist
    if (!sceneMgr->hasSceneNode(nodeName.toStdString())) return false;

    Ogre::SceneNode* node = sceneMgr->getSceneNode(nodeName.toStdString());

    // Resolve target parent (empty or "root" means root scene node)
    Ogre::SceneNode* newParent = nullptr;
    if (newParentName.isEmpty() || newParentName == "root") {
        newParent = sceneMgr->getRootSceneNode();
    } else {
        if (!sceneMgr->hasSceneNode(newParentName.toStdString())) return false;
        newParent = sceneMgr->getSceneNode(newParentName.toStdString());
    }

    // Cannot reparent to self
    if (node == newParent) return false;

    // Already a child of the target — no-op
    if (node->getParent() == newParent) return false;

    // Cannot reparent into own subtree
    if (Manager::isDescendantOf(newParent, node)) return false;

    return true;
}

bool SceneTreeModel::reparentNode(const QString& nodeName, const QString& newParentName)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return false;

    auto* sceneMgr = mgr->getSceneMgr();
    if (!sceneMgr) return false;

    if (!canReparent(nodeName, newParentName)) return false;

    Ogre::SceneNode* node = sceneMgr->getSceneNode(nodeName.toStdString());

    // Resolve target parent
    Ogre::SceneNode* newParent = nullptr;
    if (newParentName.isEmpty() || newParentName == "root") {
        newParent = sceneMgr->getRootSceneNode();
    } else {
        newParent = sceneMgr->getSceneNode(newParentName.toStdString());
    }

    // Capture old state for undo
    Ogre::SceneNode* oldParent = static_cast<Ogre::SceneNode*>(node->getParent());
    QString oldParentName = (oldParent && oldParent != sceneMgr->getRootSceneNode())
        ? QString::fromStdString(oldParent->getName()) : QString();
    Ogre::Vector3 oldLocalPos = node->getPosition();
    Ogre::Quaternion oldLocalOrient = node->getOrientation();
    Ogre::Vector3 oldLocalScale = node->getScale();

    // Perform the reparent (preserves world transform)
    if (!mgr->reparentNode(node, newParent))
        return false;

    // Capture new local transform (set by reparentNode)
    Ogre::Vector3 newLocalPos = node->getPosition();
    Ogre::Quaternion newLocalOrient = node->getOrientation();
    Ogre::Vector3 newLocalScale = node->getScale();

    // Resolve new parent name for undo storage
    QString resolvedNewParentName = (newParent != sceneMgr->getRootSceneNode())
        ? QString::fromStdString(newParent->getName()) : QString();

    // Push undo command
    UndoManager::getSingleton()->push(new ReparentCommand(
        nodeName, oldParentName, resolvedNewParentName,
        oldLocalPos, oldLocalOrient, oldLocalScale,
        newLocalPos, newLocalOrient, newLocalScale));

    SentryReporter::addBreadcrumb("ui.action",
        QString("Reparent node '%1' under '%2'").arg(nodeName, newParentName.isEmpty() ? "root" : newParentName));

    return true;
}

void SceneTreeModel::updateSelection()
{
    emit dataChanged(QModelIndex(), QModelIndex(), {SelectedRole});
    emit selectionUpdated();
}
