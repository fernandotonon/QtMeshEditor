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

#include <QInputDialog>
#include <QMessageBox>

#include <OgreAnimationState.h>

#include "GlobalDefinitions.h"

#include "SkeletonTransform.h"
#include "SelectionSet.h"
#include "Manager.h"
#include "AnimationWidget.h"
#include "ui_animationwidget.h"

AnimationWidget::AnimationWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::AnimationWidget)/*,mAnimationState(0)*/
{
    ui->setupUi(this);

    ui->animTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    //ui->animTable->verticalHeader()->hide();
    ui->animTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    connect(SelectionSet::getSingleton(),SIGNAL(entitySelectionChanged()),this,SLOT(updateAnimationTable()));


    ui->skeletonTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->skeletonTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    connect(SelectionSet::getSingleton(),SIGNAL(entitySelectionChanged()),this,SLOT(updateSkeletonTable()));
}

AnimationWidget::~AnimationWidget()
{
    delete ui;

    //TODO delete    mShowSkeleton
}

void AnimationWidget::updateAnimationTable()
{
    while(ui->animTable->rowCount())
    {
        ui->animTable->removeRow(0);
    }

    if(!SelectionSet::getSingleton()->hasEntities())
        return;

    bool hasAnimationEnable = false;

    foreach(Ogre::Entity* entity, SelectionSet::getSingleton()->getEntitiesSelectionList())
    {
        //Animation
        Ogre::AnimationStateSet* set = entity->getAllAnimationStates();
        if(set)
        {
            Ogre::AnimationStateIterator iter=set->getAnimationStateIterator();
            while(iter.hasMoreElements())
            {
                QTableWidgetItem *entityItem = new QTableWidgetItem;
                entityItem->setText(entity->getName().data());
                entityItem->setData(ENTITY_DATA,qVariantFromValue((void *) entity));
                entityItem->setFlags(entityItem->flags() & ~Qt::ItemIsEditable);

                Ogre::AnimationState * animationState = iter.getNext();

                QString animationName = animationState->getAnimationName().c_str();

                QTableWidgetItem *animationItem = new QTableWidgetItem;
                animationItem->setText(animationName);
                animationItem->setFlags(animationItem->flags() & ~Qt::ItemIsEditable);

                QTableWidgetItem* enabledCB = new QTableWidgetItem(0);
                enabledCB->setCheckState(animationState->getEnabled()?Qt::Checked:Qt::Unchecked);
                enabledCB->setFlags(enabledCB->flags() & ~Qt::ItemIsEditable);
                hasAnimationEnable = hasAnimationEnable || animationState->getEnabled();

                QTableWidgetItem* loopCB = new QTableWidgetItem(0);
                loopCB->setCheckState(animationState->getLoop()?Qt::Checked:Qt::Unchecked);
                loopCB->setFlags(loopCB->flags() & ~Qt::ItemIsEditable);

                ui->animTable->insertRow(0);
                ui->animTable->setItem(0,0,entityItem);
                ui->animTable->setItem(0,1,animationItem);
                ui->animTable->setItem(0,2,enabledCB);
                ui->animTable->setItem(0,3,loopCB);
            }
        }
    }

    // update the ui animation state
    if(!hasAnimationEnable)
        setAnimationState(false);
}

void AnimationWidget::updateSkeletonTable()
{
    while(ui->skeletonTable->rowCount())
    {
        ui->skeletonTable->removeRow(0);
    }

    if(!SelectionSet::getSingleton()->hasEntities())
        return;

    foreach(Ogre::Entity* entity, SelectionSet::getSingleton()->getEntitiesSelectionList())
    {
        QString str = entity->getName().data();
        QTableWidgetItem *entityItem = new QTableWidgetItem;
        entityItem->setText(str);
        entityItem->setData(ENTITY_DATA,qVariantFromValue((void *) entity));
        entityItem->setFlags(entityItem->flags() & ~Qt::ItemIsEditable);

        QTableWidgetItem* showSkeletonCB = new QTableWidgetItem(0);
        showSkeletonCB->setCheckState(mShowSkeleton.contains(entity)?Qt::Checked:Qt::Unchecked);
        showSkeletonCB->setFlags(showSkeletonCB->flags() & ~Qt::ItemIsEditable);

        ui->skeletonTable->insertRow(0);
        ui->skeletonTable->setItem(0,0,entityItem);
        ui->skeletonTable->setItem(0,1,showSkeletonCB);
    }
}

void AnimationWidget::on_PlayPauseButton_toggled(bool checked)
{
    setAnimationState(checked);
}

void AnimationWidget::setAnimationState(bool playing)
{
    if(playing)
    {
        ui->PlayPauseButton->setIcon(QIcon(":/icones/pause.png"));
        emit changeAnimationState(true);
    }
    else
    {
        ui->PlayPauseButton->setIcon(QIcon(":/icones/play.png"));
        emit changeAnimationState(false);
    }
}

void AnimationWidget::on_skeletonTable_clicked(const QModelIndex &index)
{
    if(index.column()!=1)
        return;

    Ogre::Entity* entity = 0;
    entity = (Ogre::Entity*)ui->skeletonTable->model()->data(ui->skeletonTable->model()->index(index.row(),0), ENTITY_DATA).value<void *>();

    if(entity && entity->hasSkeleton())
    {
        bool checked =(index.data(Qt::CheckStateRole) == Qt::Checked);

        SkeletonDebug* sd;
        if(mShowSkeleton.contains(entity))
            sd = mShowSkeleton.find(entity).value();
        else
        {   // TODO go further in skeletonDebug to clean this cam (for overlay purpose at the get-go but remove before)
            // TODO improve that : we don't need to create a sd if it is not checked
            sd = new SkeletonDebug(entity, Manager::getSingleton()->getSceneMgr(), 0.1f, 0.01f);
            mShowSkeleton.insert(entity, sd);
        }

        sd->showAxes(checked);
        sd->showBones(checked);

        if(!checked && mShowSkeleton.contains(entity))
        {
            delete sd;
            mShowSkeleton.remove(entity);
        }
    }

}

void AnimationWidget::on_animTable_cellDoubleClicked(int row, int column)
{
    if(column != 1)
        return;

    bool ok;
    QString oldName = ui->animTable->item(row,column)->text();
    QString newName = QInputDialog::getText(this, tr("Change Animation Name"),
                                             tr("New name:"), QLineEdit::Normal,
                                             oldName, &ok);

    if(ok)
    {
        if(oldName == newName)
            return;

        if(!newName.size())
        {
            QMessageBox::warning(this,tr("Error when renaming the animation"),tr("The animation name couldn't be changed to empty\nPlease type a name."),QMessageBox::Ok);
            return;
        }

        if(Manager::getSingleton()->hasAnimationName(newName))
        {
            QMessageBox::warning(this,tr("Error when renaming the animation"),tr("This name already exists."),QMessageBox::Ok);
            return;
        }

        Ogre::Entity* entity = 0;
        entity = (Ogre::Entity*)ui->animTable->model()->data(ui->animTable->model()->index(row,0), ENTITY_DATA).value<void *>();

        disableAllSkeletonDebug();
        if(entity)
        {
            if(SkeletonTransform::renameAnimation(entity,oldName,newName))
            {
                updateAnimationTable();
                ui->animTable->sortItems(0);
            }
            else
                QMessageBox::warning(this,tr("Error when renaming the animation"),tr("The animation name couldn't be changed, look into the graphics log for details."),QMessageBox::Ok);
        }
    }

}

void AnimationWidget::disableEntityAnimations(Ogre::Entity* entity)
{
    Ogre::AnimationStateSet* set = entity->getAllAnimationStates();
    if(set)
    {
        Ogre::AnimationStateIterator iter=set->getAnimationStateIterator();
        while(iter.hasMoreElements())
        {
            Ogre::String str = iter.getNext()->getAnimationName();
            Ogre::AnimationState* animationState = entity->getAnimationState(str);
            animationState->setEnabled(false);
        }
    }
    updateAnimationTable();
}

void AnimationWidget::disableAllSelectedAnimations()
{
    if(!SelectionSet::getSingleton()->hasEntities())
        return;

    foreach(Ogre::Entity* entity, SelectionSet::getSingleton()->getEntitiesSelectionList())
    {
        Ogre::AnimationStateSet* set = entity->getAllAnimationStates();
        if(set)
        {
            Ogre::AnimationStateIterator iter=set->getAnimationStateIterator();
            while(iter.hasMoreElements())
            {
                Ogre::String str = iter.getNext()->getAnimationName();
                Ogre::AnimationState* animationState = entity->getAnimationState(str);
                animationState->setEnabled(false);
            }
        }
    }
    updateAnimationTable();
}

void AnimationWidget::disableAllSkeletonDebug()
{
    foreach(SkeletonDebug *sd, mShowSkeleton.values())
    {
        sd->showAxes(false);
        sd->showBones(false);
        delete sd;
    }

    mShowSkeleton.clear();
    updateSkeletonTable();
}

void AnimationWidget::on_animTable_clicked(const QModelIndex &index)
{
    if(index.column()<2)
        return;

    Ogre::Entity* entity = 0;
    entity = (Ogre::Entity*)ui->animTable->model()->data(ui->animTable->model()->index(index.row(),0), ENTITY_DATA).value<void *>();

    if(entity)  //Should always be true
    {

        Ogre::AnimationState* animationState = entity->getAnimationState(ui->animTable->item(index.row(),1)->text().toStdString().data());
        if(animationState)
        {
            switch(index.column())
            {
            case 2:
                animationState->setEnabled(index.data(Qt::CheckStateRole) == Qt::Checked);
                break;
            case 3:
                animationState->setLoop(index.data(Qt::CheckStateRole) == Qt::Checked);
                break;
            }
        }
    }

}


