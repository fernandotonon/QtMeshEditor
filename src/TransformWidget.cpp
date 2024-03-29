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

#include "GlobalDefinitions.h"

#include "TransformWidget.h"
#include "SelectionSet.h"
#include "TransformOperator.h"
#include "Manager.h"
#include "ObjectItemModel.h"
#include "ui_TransformWidget.h"

TransformWidget::TransformWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::TransformWidget), m_pObjectTreeModel(0)
{
    ui->setupUi(this);

    // Scene TreeView
    m_pObjectTreeModel = new ObjectItemModel(this);
    ui->treeView->setModel(m_pObjectTreeModel);
    connect(SelectionSet::getSingleton(),SIGNAL(selectionChanged()),this,SLOT(updateTreeViewFromSelection()));
    connect(ui->treeView->selectionModel(),SIGNAL(selectionChanged (const QItemSelection&, const QItemSelection&)),
            this,SLOT(treeWidgetSelectionChanged(const QItemSelection&, const QItemSelection&)));

    // Position edit boxes
    connect(TransformOperator::getSingleton(),SIGNAL(selectedPositionChanged(Ogre::Vector3)),this,SLOT(updatePosition(Ogre::Vector3)));
    connect(ui->positionX, SIGNAL(valueChanged(double)),this,SLOT(onPositionEdited()));
    connect(ui->positionY, SIGNAL(valueChanged(double)),this,SLOT(onPositionEdited()));
    connect(ui->positionZ, SIGNAL(valueChanged(double)),this,SLOT(onPositionEdited()));

    // Scale edit boxes
    connect(TransformOperator::getSingleton(),SIGNAL(selectedScaleChanged(Ogre::Vector3)),this,SLOT(updateNodeScale(Ogre::Vector3)));
    connect(ui->scaleX, SIGNAL(valueChanged(double)),this,SLOT(onNodeScaleEdited()));
    connect(ui->scaleY, SIGNAL(valueChanged(double)),this,SLOT(onNodeScaleEdited()));
    connect(ui->scaleZ, SIGNAL(valueChanged(double)),this,SLOT(onNodeScaleEdited()));

    // Rotation edit boxes
    connect(TransformOperator::getSingleton(),SIGNAL(selectedOrientationChanged(Ogre::Vector3)),this,SLOT(updateNodeOrientation(Ogre::Vector3)));
    connect(ui->rotationX, SIGNAL(valueChanged(double)),this,SLOT(onNodeOrientationEdited()));
    connect(ui->rotationY, SIGNAL(valueChanged(double)),this,SLOT(onNodeOrientationEdited()));
    connect(ui->rotationZ, SIGNAL(valueChanged(double)),this,SLOT(onNodeOrientationEdited()));
}

TransformWidget::~TransformWidget()
{
    delete ui;

    if(m_pObjectTreeModel)
    {
        delete m_pObjectTreeModel;
        m_pObjectTreeModel = 0;
    }

}

void TransformWidget::updateTreeViewFromSelection()
{
    ui->treeView->blockSignals(true);

    QModelIndex start =  m_pObjectTreeModel->getRootIndex();
    QModelIndexList index;
    Ogre::SceneNode* node = 0;
    Ogre::Entity*    entity = 0;
    Ogre::SubEntity* subEntity = 0;
    QItemSelection newSelection;

    foreach(node,SelectionSet::getSingleton()->getNodesSelectionList())
    {
        index = m_pObjectTreeModel->match(start, NODE_DATA,
                      QVariant::fromValue((void *) node),
                      1/*stop*/ ,Qt::MatchExactly|Qt::MatchRecursive);

        if(!index.isEmpty())
            if(index.at(0).isValid())
            {
                newSelection.select(index.at(0),index.at(0));
                ui->treeView->expand(m_pObjectTreeModel->parent(index.at(0)));
            }
    }

    foreach(entity,SelectionSet::getSingleton()->getEntitiesSelectionList())
    {
        index = m_pObjectTreeModel->match(start, ENTITY_DATA,
                      QVariant::fromValue((void *) entity),
                      1/*stop*/ ,Qt::MatchExactly|Qt::MatchRecursive);

        if(!index.isEmpty())
            if(index.at(0).isValid())
            {
                newSelection.select(index.at(0),index.at(0));
                ui->treeView->expand(m_pObjectTreeModel->parent(index.at(0)));
            }
    }

    foreach(subEntity,SelectionSet::getSingleton()->getSubEntitiesSelectionList())
    {
        index = m_pObjectTreeModel->match(start, SUBENTITY_DATA,
                      QVariant::fromValue((void *) subEntity),
                      1/*stop*/ ,Qt::MatchExactly|Qt::MatchRecursive);

        if(!index.isEmpty())
            if(index.at(0).isValid())
            {
                newSelection.select(index.at(0),index.at(0));
                ui->treeView->expand(m_pObjectTreeModel->parent(index.at(0)));
            }
    }
    ui->treeView->selectionModel()->select(newSelection, QItemSelectionModel::ClearAndSelect);

    // Update header Text
    int numSelected = SelectionSet::getSingleton()->getCount();
    if(numSelected == 0)
        m_pObjectTreeModel->setHeaderText(tr("No Selection"));
    else if(numSelected == 1)
        m_pObjectTreeModel->setHeaderText(tr("1 Object Selected"));
    else
        m_pObjectTreeModel->setHeaderText(QString::number(numSelected)+tr(" Objects Selected"));

    ui->treeView->blockSignals(false);
}

void TransformWidget::treeWidgetSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{

    QItemSelectionRange range;
    QModelIndex index;
    QStandardItem*      currentItem = 0;
    Ogre::SceneNode*    node = 0;
    Ogre::Entity*       entity = 0;
    Ogre::SubEntity*    subEntity = 0;

    foreach(range, deselected)
    {
        foreach(index,range.indexes())
        {
            currentItem = m_pObjectTreeModel->itemFromIndex(index);
            node = (Ogre::SceneNode*) currentItem->data(NODE_DATA).value<void *>();
            entity = (Ogre::Entity*) currentItem->data(ENTITY_DATA).value<void *>();
            subEntity = (Ogre::SubEntity*) currentItem->data(SUBENTITY_DATA).value<void *>();
            if(node)
                 SelectionSet::getSingleton()->removeOne(node);
            else if(entity)
                SelectionSet::getSingleton()->removeOne(entity);
            else if(subEntity)
                SelectionSet::getSingleton()->removeOne(subEntity);

        }
    }

    foreach(range, selected)
    {
        foreach(index,range.indexes())
        {
            currentItem = m_pObjectTreeModel->itemFromIndex(index);
            node = (Ogre::SceneNode*) currentItem->data(NODE_DATA).value<void *>();
            entity = (Ogre::Entity*) currentItem->data(ENTITY_DATA).value<void *>();
            subEntity = (Ogre::SubEntity*) currentItem->data(SUBENTITY_DATA).value<void *>();
            if(node)
                SelectionSet::getSingleton()->append(node);
            else if(entity)
                SelectionSet::getSingleton()->append(entity);
            else if(subEntity)
                SelectionSet::getSingleton()->append(subEntity);
        }
    }

}

void TransformWidget::updatePosition(const Ogre::Vector3& newPosition)
{
    ui->positionX->blockSignals(true);
    ui->positionY->blockSignals(true);
    ui->positionZ->blockSignals(true);

    ui->positionX->setValue(newPosition.x);
    ui->positionY->setValue(newPosition.y);
    ui->positionZ->setValue(newPosition.z);

    ui->positionX->blockSignals(false);
    ui->positionY->blockSignals(false);
    ui->positionZ->blockSignals(false);
}

void TransformWidget::onPositionEdited()
{
    Ogre::Vector3 position = Ogre::Vector3::ZERO;
    position.x=ui->positionX->value();
    position.y=ui->positionY->value();
    position.z=ui->positionZ->value();
    TransformOperator::getSingleton()->setSelectedPosition(position);
}

// TODO link this to the GUI scaling method
void TransformWidget::updateNodeScale(const Ogre::Vector3& newScale)
{
    ui->scaleX->blockSignals(true);
    ui->scaleY->blockSignals(true);
    ui->scaleZ->blockSignals(true);

    ui->scaleX->setValue(newScale.x);
    ui->scaleY->setValue(newScale.y);
    ui->scaleZ->setValue(newScale.z);

    ui->scaleX->blockSignals(false);
    ui->scaleY->blockSignals(false);
    ui->scaleZ->blockSignals(false);
}

void TransformWidget::onNodeScaleEdited()
{
    Ogre::Vector3 scale = Ogre::Vector3::UNIT_SCALE;
    scale.x=ui->scaleX->value();
    scale.y=ui->scaleY->value();
    scale.z=ui->scaleZ->value();
    TransformOperator::getSingleton()->setSelectedScale(scale);
}

void TransformWidget::updateNodeOrientation(const Ogre::Vector3& newOrientation)
{
    ui->rotationX->blockSignals(true);
    ui->rotationY->blockSignals(true);
    ui->rotationZ->blockSignals(true);

    ui->rotationX->setValue(newOrientation.x);
    ui->rotationY->setValue(newOrientation.y);
    ui->rotationZ->setValue(newOrientation.z);

    ui->rotationX->blockSignals(false);
    ui->rotationY->blockSignals(false);
    ui->rotationZ->blockSignals(false);
}
// TODO add a control between 0 360° or -180 180°
void TransformWidget::onNodeOrientationEdited()
{
    Ogre::Vector3 eulerOrientation = Ogre::Vector3::UNIT_SCALE;
    eulerOrientation.x=ui->rotationX->value();
    eulerOrientation.y=ui->rotationY->value();
    eulerOrientation.z=ui->rotationZ->value();
    TransformOperator::getSingleton()->setSelectedOrientation(eulerOrientation);
}
