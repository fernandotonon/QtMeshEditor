/*/////////////////////////////////////////////////////////////////////////////////
/// A QtMeshEditor file
///
/// Copyright (c) HogPog Team (www.hogpog.com.br)
///
/// The MIT License
///
/// Permission is hereby granted, free of charge, to any person obtaining a copy
/// of this software and associated documentation files (the "Software"), to deal
/// in the Software without restriction, including without limitation the rights
/// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
/// copies of the Software, and to permit persons to whom the Software is
/// furnished to do so, subject to the following conditions:
///
/// The above copyright notice and this permission notice shall be included in
/// all copies or substantial portions of the Software.
///
/// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
/// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
/// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
/// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
/// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
/// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
/// THE SOFTWARE.
////////////////////////////////////////////////////////////////////////////////*/

#ifndef OBJECT_ITEM_MODEL_H
#define OBJECT_ITEM_MODEL_H

#include <QStandardItemModel>

#include <QVariant>

namespace Ogre{
    class SceneNode;
    class Entity;
}

class ObjectItemModel : public QStandardItemModel
{
    Q_OBJECT

public:
    explicit ObjectItemModel(QObject *parent = 0);
    ~ObjectItemModel();


    const QModelIndex getRootIndex() const;
    void setHeaderText(const QString& text);
private:
    void appendAllChildFromParent(Ogre::SceneNode* const& parentNode, QStandardItem* const& parentItem); 
    void appendEntitiesFromNode(Ogre::SceneNode* const& parentNode, QStandardItem* const& parentItem);
    void appendSubEntitiesFromEntity(Ogre::Entity* const& parentEntity, QStandardItem* const& parentItem);

private slots:
    void newObjectNode(Ogre::SceneNode* const& newNode);
    void objectNodeRemoved(Ogre::SceneNode* const& node);
    void reloadSceneNode();

private:
    Ogre::SceneNode* m_pRootNode;
    QStandardItem* mRootItem;
    Ogre::Entity* mOgreEntity;

};

#endif // OBJECT_ITEM_MODEL_H
