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

#include <QHeaderView>
#include <OgreEntity.h>
#include <OgreSubEntity.h>

#include "GlobalDefinitions.h"

#include "MaterialWidget.h"
#include "MaterialComboDelegate.h"
#include "SelectionSet.h"

MaterialWidget::MaterialWidget(QWidget *parent) :
    QTableWidget(parent)
{
    setColumnCount(3);

    QStringList headerLabels;
    headerLabels<< tr("Entity")<<tr("Sub")<<tr("Material");
    setHorizontalHeaderLabels(headerLabels);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    verticalHeader()->hide();

    MaterialComboDelegate* pDelegate = new MaterialComboDelegate(this);
    setItemDelegateForColumn(2,pDelegate); //TODO find a method to avoid reloading at each selection change

    setEditTriggers(QAbstractItemView::AllEditTriggers);

    connect(SelectionSet::getSingleton(),SIGNAL(nodeSelectionChanged()),this,SLOT(onNodeSelected()));
    connect(SelectionSet::getSingleton(),SIGNAL(entitySelectionChanged()),this,SLOT(onEntitySelected()));
    connect(SelectionSet::getSingleton(),SIGNAL(subEntitySelectionChanged()),this,SLOT(onSubEntitySelected()));
    connect(this,SIGNAL(cellChanged(int,int)),this,SLOT(onMaterialChanged(int,int)));

}

MaterialWidget::~MaterialWidget()
{

}

void MaterialWidget::onNodeSelected()
{
    blockSignals(true);
    clearContents();
    setRowCount(0);
    if (SelectionSet::getSingleton()->hasNodes())
    {
        foreach (Ogre::SceneNode* node, SelectionSet::getSingleton()->getNodesSelectionList())
        {
            if(!node->getAttachedObjects().empty()){
                Ogre::Entity* entity = static_cast<Ogre::Entity*>(node->getAttachedObject(0));
                populateTableWithEntities({entity});
            }
        }
        horizontalHeader()->setStretchLastSection(true);
        horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    }
    blockSignals(false);
}

void MaterialWidget::onEntitySelected()
{
    blockSignals(true);
    clearContents();
    setRowCount(0);
    if (SelectionSet::getSingleton()->hasEntities())
    {
        populateTableWithEntities(SelectionSet::getSingleton()->getEntitiesSelectionList());
        horizontalHeader()->setStretchLastSection(true);
        horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    }
    blockSignals(false);
}

void MaterialWidget::onSubEntitySelected()
{
    blockSignals(true);
    clearContents();
    setRowCount(0);
    if (SelectionSet::getSingleton()->hasSubEntities())
    {
        populateTableWithSubEntities(SelectionSet::getSingleton()->getSubEntitiesSelectionList());
        horizontalHeader()->setStretchLastSection(true);
        horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    }
    blockSignals(false);
}

void MaterialWidget::populateTableWithEntities(const QList<Ogre::Entity*>& entities)
{
    foreach (Ogre::Entity* entity, entities)
    {
        Ogre::Entity::SubEntityList subEntities = entity->getSubEntities();
        QList<Ogre::SubEntity*> subEntityList;
        for (auto subEntity : subEntities) {
            subEntityList.append(subEntity);
        }
        populateTableWithSubEntities(subEntityList);
    }
}

void MaterialWidget::populateTableWithSubEntities(const QList<Ogre::SubEntity*>& subEntities)
{
    foreach (Ogre::SubEntity* subEntity, subEntities)
    {
        int row = rowCount();
        setRowCount(row + 1);

        unsigned int subIndex = 0;
        while ((subEntity->getParent()->getSubEntity(subIndex) != subEntity) &&
               (subIndex <= subEntity->getParent()->getNumSubEntities()))
            subIndex++;

        QTableWidgetItem* newItem = new QTableWidgetItem(subEntity->getParent()->getName().data());
        newItem->setFlags(newItem->flags() & ~Qt::ItemIsEditable);
        setItem(row, 0, newItem);

        newItem = new QTableWidgetItem(QString::number(subIndex));
        newItem->setFlags(newItem->flags() & ~Qt::ItemIsEditable);
        newItem->setData(SUBENTITY_DATA, QVariant::fromValue((void*)subEntity));
        setItem(row, 1, newItem);
    }
}

void MaterialWidget::onMaterialChanged(int row, int column)
{
    if(column!=2)
        return;

    QString newMaterialName = model()->data(model()->index(row,column)).toString();
    Ogre::SubEntity* currentSubEntity;
    currentSubEntity = (Ogre::SubEntity*)model()->data(model()->index(row,1), SUBENTITY_DATA).value<void *>();
    currentSubEntity->setMaterialName(newMaterialName.toStdString().data());

}
