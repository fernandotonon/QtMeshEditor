#include <QDebug>

#include <QInputDialog>
#include <QFileDialog>
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
    ui->animTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    ui->skeletonTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->skeletonTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    connect(SelectionSet::getSingleton(),&SelectionSet::entitySelectionChanged,this,[this]()
    {
        updateAnimationTable();
        updateSkeletonTable();
    });
}

AnimationWidget::~AnimationWidget()
{
    disableAllSkeletonDebug();

    delete ui;
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

    for(Ogre::Entity* entity : SelectionSet::getSingleton()->getEntitiesSelectionList())
    {
        //Animation
        Ogre::AnimationStateSet* set = entity->getAllAnimationStates();
        if(!set) continue;

        for (const auto &animationState:set->getAnimationStates())
        {
            QTableWidgetItem *entityItem = new QTableWidgetItem;
            entityItem->setText(entity->getName().data());
            entityItem->setData(ENTITY_DATA,QVariant::fromValue((void *) entity));
            entityItem->setFlags(entityItem->flags() & ~Qt::ItemIsEditable);

            QString animationName = animationState.second->getAnimationName().c_str();

            QTableWidgetItem *animationItem = new QTableWidgetItem;
            animationItem->setText(animationName);
            animationItem->setFlags(animationItem->flags() & ~Qt::ItemIsEditable);

            QTableWidgetItem* enabledCB = new QTableWidgetItem(0);
            enabledCB->setCheckState(animationState.second->getEnabled()?Qt::Checked:Qt::Unchecked);
            enabledCB->setFlags(enabledCB->flags() & ~Qt::ItemIsEditable);
            hasAnimationEnable = hasAnimationEnable || animationState.second->getEnabled();

            QTableWidgetItem* loopCB = new QTableWidgetItem(0);
            loopCB->setCheckState(animationState.second->getLoop()?Qt::Checked:Qt::Unchecked);
            loopCB->setFlags(loopCB->flags() & ~Qt::ItemIsEditable);

            ui->animTable->insertRow(0);
            ui->animTable->setItem(0,0,entityItem);
            ui->animTable->setItem(0,1,animationItem);
            ui->animTable->setItem(0,2,enabledCB);
            ui->animTable->setItem(0,3,loopCB);
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

    for(Ogre::Entity* entity : SelectionSet::getSingleton()->getEntitiesSelectionList())
    {
        QString str = entity->getName().data();
        QTableWidgetItem *entityItem = new QTableWidgetItem;
        entityItem->setText(str);
        entityItem->setData(ENTITY_DATA,QVariant::fromValue((void *) entity));
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
    auto icon = QIcon(playing?":/icones/pause.png":":/icones/play.png");

    ui->PlayPauseButton->setIcon(icon);

    emit changeAnimationState(playing);
}

void AnimationWidget::on_skeletonTable_clicked(const QModelIndex &index)
{
    if(index.column()!=1)
        return;

    auto entity = (Ogre::Entity*)ui->skeletonTable->model()->data(ui->skeletonTable->model()->index(index.row(),0), ENTITY_DATA).value<void *>();

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

    if(!ok) return;

    if(oldName == newName) return;

    if(!newName.size())
    {
        QMessageBox::warning(this,tr("Error when renaming the animation"),tr("The animation name couldn't be changed to empty\nPlease type a name."),QMessageBox::Ok);
        return;
    }

    Ogre::Entity* entity = nullptr;
    entity = (Ogre::Entity*)ui->animTable->model()->data(ui->animTable->model()->index(row,0), ENTITY_DATA).value<void *>();
    if(!entity) return;

    disableAllSkeletonDebug();

    if(Manager::getSingleton()->hasAnimationName(entity, newName))
    {
        QMessageBox::warning(this,tr("Error when renaming the animation"),tr("This name already exists."),QMessageBox::Ok);
        return;
    }

    setAnimationState(false);

    if(SkeletonTransform::renameAnimation(entity,oldName,newName))
    {
        updateAnimationTable();
        ui->animTable->sortItems(0);
        emit changeAnimationName(newName.toStdString());
    }
    else
        QMessageBox::warning(this,tr("Error when renaming the animation"),tr("The animation name couldn't be changed, look into the graphics log for details."),QMessageBox::Ok);

}

void AnimationWidget::disableEntityAnimations(Ogre::Entity* entity)
{
    if(auto set = entity->getAllAnimationStates(); set)
    {
        for (const auto &animationState : set->getAnimationStates())
        {
            animationState.second->setEnabled(false);
        }
    }
    updateAnimationTable();
}

void AnimationWidget::disableAllSelectedAnimations()
{
    if(!SelectionSet::getSingleton()->hasEntities())
        return;

    for(const Ogre::Entity* entity : SelectionSet::getSingleton()->getEntitiesSelectionList())
    {
        if(auto set = entity->getAllAnimationStates(); set)
        {
            for (const auto &animationState : set->getAnimationStates())
            {
                animationState.second->setEnabled(false);
            }
        }
    }
    updateAnimationTable();
}

void AnimationWidget::disableAllSkeletonDebug()
{
    for(SkeletonDebug *sd : mShowSkeleton.values())
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

    const Ogre::Entity* entity = (Ogre::Entity*)ui->animTable->model()->data(ui->animTable->model()->index(index.row(),0), ENTITY_DATA).value<void *>();
    if(!entity)
        return;

    Ogre::AnimationState* animationState = entity->getAnimationState(ui->animTable->item(index.row(),1)->text().toStdString().data());
    if(!animationState)
        return;

    switch(index.column())
    {
    case 2:
        animationState->setEnabled(index.data(Qt::CheckStateRole) == Qt::Checked);
        break;
    case 3:
        animationState->setLoop(index.data(Qt::CheckStateRole) == Qt::Checked);
        break;
    default:
        break;
    }
}


/**
 * @brief AnimationWidget::on_mergeButton_clicked
 * Merge a skeleton file into the current skeleton
 *
 */
void AnimationWidget::on_mergeButton_clicked()
{
    QStringList fileNames = QFileDialog::getOpenFileNames(this, tr("Select a skeleton file to import"),
                                                     "",
                                                     QString("Skeleton ( *.skeleton )"),
                                                     nullptr, QFileDialog::DontUseNativeDialog);

    if(fileNames.size() == 0)
        return;

    // Disable all animations
    disableAllSelectedAnimations();
    setAnimationState(false);

    for(const QString& fileName : fileNames)
    {
        std::ifstream file(fileName.toStdString().data(), std::ios::in | std::ios::binary);
        if(!file.is_open())
        {
            Ogre::LogManager::getSingleton().logMessage("Error when merging the skeleton - The file couldn't be opened");
            QMessageBox::warning(nullptr,tr("Error when merging the skeleton"),tr("The file %1 couldn't be opened").arg(fileName),QMessageBox::Ok);
            continue;
        }

        Ogre::DataStreamPtr stream(new Ogre::FileStreamDataStream(fileName.toStdString().data(), &file, false));
        Ogre::SkeletonPtr importedSkel;
        auto name = fileName.toStdString();
        unsigned int count = 0;
        while(true){
            try{
                importedSkel = Ogre::SkeletonManager::getSingleton().create(name.data(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
                break;
            } catch(Ogre::Exception& e){
                name = fileName.toStdString() + "_" + std::to_string(++count);
            }
        }

        Ogre::SkeletonSerializer serializer;
        serializer.importSkeleton(stream, importedSkel.get());

        for(Ogre::Entity* ent : SelectionSet::getSingleton()->getEntitiesSelectionList())
        {
            if(!ent->hasSkeleton())
                continue;

            // return to bind pose
            ent->getSkeleton()->reset();

            Ogre::Skeleton* skel = ent->getSkeleton();

            unsigned short numAnimations = importedSkel->getNumAnimations();
            for(unsigned short i=0; i<numAnimations; i++)
            {
                Ogre::Animation* anim = importedSkel->getAnimation(i);
                if(!anim)
                    continue;

                if(skel->hasAnimation("merged_"+anim->getName()))
                {
                    Ogre::LogManager::getSingleton().logMessage("Error when merging the skeleton - The skeleton already has an animation named "+anim->getName());
                    QMessageBox::warning(this,tr("Error when merging the skeleton"),tr("The skeleton already has an animation named %1").arg(anim->getName().c_str()),QMessageBox::Ok);
                    continue;
                }
                Ogre::Animation* newAnim = skel->createAnimation("merged_"+anim->getName(), anim->getLength());

                unsigned short numTracks = anim->getNumNodeTracks();
                for(unsigned short j=0; j<numTracks+1000; j++)
                {
                    if(!anim->hasNodeTrack(j)) continue;

                    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(j);
                    if(!track)
                        continue;

                    Ogre::NodeAnimationTrack* newTrack = newAnim->createNodeTrack(track->getHandle());
                    newTrack->setAssociatedNode(track->getAssociatedNode());

                    unsigned short numKeyFrames = track->getNumKeyFrames();
                    for(unsigned short k=0; k<numKeyFrames; k++)
                    {
                        Ogre::TransformKeyFrame* keyFrame = track->getNodeKeyFrame(k);
                        if(!keyFrame)
                            continue;

                        Ogre::TransformKeyFrame* newKeyFrame = newTrack->createNodeKeyFrame(keyFrame->getTime());
                        newKeyFrame->setTranslate(keyFrame->getTranslate());
                        newKeyFrame->setRotation(keyFrame->getRotation());
                        newKeyFrame->setScale(keyFrame->getScale());

                        // print the keyframe
                        Ogre::LogManager::getSingleton().logMessage("Animation: " + newAnim->getName());
                        Ogre::LogManager::getSingleton().logMessage("Keyframe: " + Ogre::StringConverter::toString(keyFrame->getTime()));
                        Ogre::LogManager::getSingleton().logMessage("Translate: " + Ogre::StringConverter::toString(keyFrame->getTranslate()));
                        Ogre::LogManager::getSingleton().logMessage("Rotation: " + Ogre::StringConverter::toString(keyFrame->getRotation()));
                        Ogre::LogManager::getSingleton().logMessage("Scale: " + Ogre::StringConverter::toString(keyFrame->getScale()));
                    }
                }
            }

            skel->setBindingPose();
            skel->setBlendMode(Ogre::ANIMBLEND_CUMULATIVE);

            ent->refreshAvailableAnimationState();
        }

        Ogre::SkeletonManager::getSingleton().remove(importedSkel->getHandle());

        file.close();

        //Update the animation table
        updateAnimationTable();

        //Update the skeleton table
        updateSkeletonTable();
    }
}

