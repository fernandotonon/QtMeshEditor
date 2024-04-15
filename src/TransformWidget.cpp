#include <QDebug>
#include <QItemSelection>

#include "GlobalDefinitions.h"

#include "TransformWidget.h"
#include "SelectionSet.h"
#include "TransformOperator.h"
#include "ObjectItemModel.h"

TransformWidget::TransformWidget(QWidget *parent) :
    QWidget(parent), m_pObjectTreeModel(nullptr)
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
        m_pObjectTreeModel = nullptr;
    }
}

void TransformWidget::updateTreeViewFromSelection()
{
    ui->treeView->blockSignals(true);

    QModelIndex start =  m_pObjectTreeModel->getRootIndex();
    QModelIndexList index;
    Ogre::SceneNode* node = nullptr;
    Ogre::Entity*    entity = nullptr;
    Ogre::SubEntity* subEntity = nullptr;
    QItemSelection newSelection;

    foreach(node, SelectionSet::getSingleton()->getNodesSelectionList())
    {
        index = m_pObjectTreeModel->match(start, NODE_DATA,
                      QVariant::fromValue((void *) node),
                      1/*stop*/ ,Qt::MatchExactly|Qt::MatchRecursive);

        if(index.isEmpty() || !index.at(0).isValid()) continue;

        newSelection.select(index.at(0),index.at(0));
        ui->treeView->expand(m_pObjectTreeModel->parent(index.at(0)));
    }

    foreach(entity,SelectionSet::getSingleton()->getEntitiesSelectionList())
    {
        index = m_pObjectTreeModel->match(start, ENTITY_DATA,
                      QVariant::fromValue((void *) entity),
                      1/*stop*/ ,Qt::MatchExactly|Qt::MatchRecursive);

        if(index.isEmpty() || !index.at(0).isValid()) continue;

        newSelection.select(index.at(0),index.at(0));
        ui->treeView->expand(m_pObjectTreeModel->parent(index.at(0)));
    }

    foreach(subEntity,SelectionSet::getSingleton()->getSubEntitiesSelectionList())
    {
        index = m_pObjectTreeModel->match(start, SUBENTITY_DATA,
                      QVariant::fromValue((void *) subEntity),
                      1/*stop*/ ,Qt::MatchExactly|Qt::MatchRecursive);

        if(index.isEmpty() || !index.at(0).isValid()) continue;

        newSelection.select(index.at(0),index.at(0));
        ui->treeView->expand(m_pObjectTreeModel->parent(index.at(0)));
    }

    ui->treeView->selectionModel()->select(newSelection, QItemSelectionModel::ClearAndSelect);

    // Update header Text
    switch (int numSelected = SelectionSet::getSingleton()->getCount(); numSelected)
    {
    case 0:
        m_pObjectTreeModel->setHeaderText(tr("No object selected"));
        break;
    case 1:
        m_pObjectTreeModel->setHeaderText(tr("1 object selected"));
        break;
    default:
        m_pObjectTreeModel->setHeaderText(QString::number(numSelected) + tr(" objects selected"));
    }

    ui->treeView->blockSignals(false);
}

void TransformWidget::treeWidgetSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
    QItemSelectionRange range;
    QModelIndex index;
    QStandardItem*      currentItem = nullptr;
    Ogre::SceneNode*    node = nullptr;
    Ogre::Entity*       entity = nullptr;
    Ogre::SubEntity*    subEntity = nullptr;

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
