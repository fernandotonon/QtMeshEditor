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

#include <QDebug>

#include <QStringList>
#include <QStandardItem>

#include <Ogre.h>

#include "GlobalDefinitions.h"

#include "Manager.h"
#include "ObjectItemModel.h"


// TODO check for memory leak when removing rows,...
// TODO add nicer icons in the tree view

ObjectItemModel::ObjectItemModel(QObject *parent)
    : QStandardItemModel(parent), m_pRootNode(0),mRootItem(0)
{
    reloadSceneNode();

    connect(Manager::getSingleton(),SIGNAL(sceneNodeCreated(Ogre::SceneNode* const&)),this,SLOT(newObjectNode(Ogre::SceneNode* const&)));
    connect(Manager::getSingleton(),SIGNAL(sceneNodeDestroyed(Ogre::SceneNode* const&)),this,SLOT(objectNodeRemoved(Ogre::SceneNode* const&)));
    //// DEBUG =>find the correct location of entity
    connect(Manager::getSingleton(),SIGNAL(entityCreated(Ogre::Entity* const&)),this,SLOT(reloadSceneNode()));
}

ObjectItemModel::~ObjectItemModel()
{
    delete mRootItem;
    m_pRootNode =0;
}

const QModelIndex ObjectItemModel::getRootIndex() const
{    return indexFromItem(mRootItem);   }

void ObjectItemModel::setHeaderText(const QString& text)
{
   setHorizontalHeaderLabels(QStringList(text));
}

void ObjectItemModel::reloadSceneNode()
{
    clear();

    setHorizontalHeaderLabels(QStringList(tr("No Selection")));

    m_pRootNode = Manager::getSingleton()->getSceneMgr()->getRootSceneNode();
    mRootItem = new QStandardItem(QIcon(":/icones/square.png"),QString(tr("Scene")));
    mRootItem->setData(QVariant::fromValue((void*)m_pRootNode));
    mRootItem->setSelectable(false);        //TODO set this to true to allow the entire Scene selection
    invisibleRootItem()->appendRow(mRootItem);

    appendAllChildFromParent(m_pRootNode, mRootItem);
}

void ObjectItemModel::appendAllChildFromParent(Ogre::SceneNode* const& parentNode, QStandardItem* const& parentItem)
{
    Ogre::Node::ChildNodeIterator nodeIterator = parentNode->getChildIterator();

    while (nodeIterator.hasMoreElements())
    {
        Ogre::SceneNode* pSN = static_cast<Ogre::SceneNode*>(nodeIterator.getNext());
        QString name = pSN->getName().data();

        if(name.length() && !(Manager::getSingleton()->isForbidenNodeName(name)))
        {
            Ogre::SceneNode* currentNode = pSN;
            QStandardItem* newItem = new QStandardItem(QIcon(":/icones/cube.png"),currentNode->getName().data()+tr(" (Node)"));
            newItem->setData(QVariant::fromValue((void*)currentNode), NODE_DATA);
            parentItem->appendRow(newItem);

            //Adding entities
            appendEntitiesFromNode(currentNode, newItem);

            if(currentNode->numChildren()>0)    //recursive call
                appendAllChildFromParent(currentNode,newItem);

        }
    }
}

void ObjectItemModel::appendEntitiesFromNode(Ogre::SceneNode* const& parentNode, QStandardItem* const& parentItem)
{
    for(int entIndex = 0;  entIndex < parentNode->numAttachedObjects();entIndex++)
    {
        mOgreEntity = static_cast<Ogre::Entity*>(parentNode->getAttachedObject(entIndex));

        QStandardItem* newEntity = new QStandardItem(QIcon(":/icones/cylinder.png"),mOgreEntity->getName().data()+tr(" (Mesh)"));
        newEntity->setData(QVariant::fromValue((void*)mOgreEntity), ENTITY_DATA);
        parentItem->appendRow(newEntity);
        // adding subentities
        appendSubEntitiesFromEntity(mOgreEntity, newEntity);
    }

}

void ObjectItemModel::appendSubEntitiesFromEntity(Ogre::Entity* const& parentEntity, QStandardItem* const& parentItem)
{

    for(unsigned int subEntIndex = 0; parentEntity && subEntIndex < parentEntity->getNumSubEntities();subEntIndex++)
    {
        QStandardItem* newSubEntity = new QStandardItem(QIcon(":/icones/torus.png"),QString::number(subEntIndex)+tr(" (Submesh)"));
        newSubEntity->setData(QVariant::fromValue((void*)parentEntity->getSubEntity(subEntIndex)),SUBENTITY_DATA); //TODO add data here !!!
        parentItem->appendRow(newSubEntity);
    }
}


void ObjectItemModel::newObjectNode(Ogre::SceneNode* const& newNode)
{
    QStandardItem* newItem = new QStandardItem(QIcon(":/icones/cube.png"),newNode->getName().data()+tr(" (Node)"));
    newItem->setData(QVariant::fromValue((void*)newNode),NODE_DATA);
    mRootItem->appendRow(newItem);
    appendAllChildFromParent(newNode, newItem);
}

void ObjectItemModel::objectNodeRemoved(Ogre::SceneNode* const& node)
{
    QModelIndex start = getRootIndex();
    QModelIndexList index;

    index = match(start, Qt::UserRole + 1,
                  QVariant::fromValue((void*)node),
                  1/*stop*/ ,Qt::MatchExactly|Qt::MatchRecursive);

    if(!index.isEmpty())
        if(index.at(0).isValid())
        {
           removeRow(index.at(0).row(),parent(index.at(0)));
        }
}
