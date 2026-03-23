#ifndef SCENE_TREE_MODEL_H
#define SCENE_TREE_MODEL_H

#include <QAbstractItemModel>
#include <QList>
#include <QString>

class QTimer;

namespace Ogre {
    class SceneNode;
    class Entity;
    class SubEntity;
}

class SceneTreeItem
{
public:
    enum ItemType { Root, Node, Entity, SubEntity };

    SceneTreeItem(const QString& name, ItemType type, void* ogrePtr, SceneTreeItem* parent = nullptr);
    ~SceneTreeItem();

    void appendChild(SceneTreeItem* child);
    SceneTreeItem* child(int row) const;
    int childCount() const;
    int row() const;
    SceneTreeItem* parentItem() const;

    QString name() const { return mName; }
    ItemType type() const { return mType; }
    void* ogrePtr() const { return mOgrePtr; }
    QString typeLabel() const;

private:
    QString mName;
    ItemType mType;
    void* mOgrePtr;
    SceneTreeItem* mParent;
    QList<SceneTreeItem*> mChildren;
};

class SceneTreeModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        TypeRole,
        TypeLabelRole,
        SelectedRole,
        MaterialNameRole,
    };

    explicit SceneTreeModel(QObject* parent = nullptr);
    ~SceneTreeModel() override;

    // QAbstractItemModel interface
    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void selectItem(int row, const QModelIndex& parentIndex, bool multiSelect);
    Q_INVOKABLE QModelIndex rootIndex() const;
    Q_INVOKABLE bool isSelected(int row, const QModelIndex& parentIndex) const;
    Q_INVOKABLE QString materialName(int row, const QModelIndex& parentIndex) const;
    Q_INVOKABLE void setMaterial(int row, const QModelIndex& parentIndex, const QString& materialName);
    Q_INVOKABLE QStringList availableMaterials() const;

public slots:
    void rebuild();
    void updateSelection();

signals:
    void selectionUpdated();

private:
    void buildChildren(Ogre::SceneNode* sceneNode, SceneTreeItem* parentItem);
    SceneTreeItem* itemFromIndex(const QModelIndex& index) const;

    SceneTreeItem* mRootItem = nullptr;
    QTimer* mRebuildTimer = nullptr;
};

#endif // SCENE_TREE_MODEL_H
