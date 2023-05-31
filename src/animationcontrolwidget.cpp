#include "animationcontrolwidget.h"
#include "ui_animationcontrolwidget.h"
#include "SelectionSet.h"
#include "Manager.h"
#include <QTreeWidgetItem>
#include <QTimer>

AnimationControlWidget::AnimationControlWidget(QWidget *parent) :
    QDockWidget(parent),
    ui(new Ui::AnimationControlWidget)
{
    ui->setupUi(this);
    updateAnimationTree();

    connect(SelectionSet::getSingleton(),SIGNAL(entitySelectionChanged()),this,SLOT(updateAnimationTree()));
    connect(ui->treeWidget, &QTreeWidget::itemSelectionChanged, this, [=](){
        ui->horizontalSlider->setValue(0);
        ui->horizontalSlider->setEnabled(true);
        if(ui->treeWidget->selectedItems().size() > 0)
        {
            QTreeWidgetItem* item = ui->treeWidget->selectedItems().at(0);
            auto selected = item->text(0).toStdString();

            m_selectedAnimation = selected.substr(6);
            auto data = item->data(0,Qt::UserRole).value<std::tuple<Ogre::Entity*,Ogre::SkeletonInstance*,Ogre::AnimationState*>>();
            m_selectedEntity = std::get<0>(data);
            m_selectedSkeleton = std::get<1>(data);

            Ogre::Animation* animation = m_selectedSkeleton->getAnimation(m_selectedAnimation);
            if(animation)
            {
                ui->horizontalSlider->setMaximum(animation->getLength()*1000);
                ui->maxSliderLabel->setText(QString::number(animation->getLength()));

                ui->boneList->clear();
                auto trackList = animation->_getNodeTrackList();
                for(const auto &track:trackList)
                {
                    // add to bone list
                    QListWidgetItem* boneItem = new QListWidgetItem(QString::fromStdString(track.second->getAssociatedNode()->getName()));
                    boneItem->setData(Qt::UserRole,QVariant::fromValue(track.second));
                    ui->boneList->addItem(boneItem);
                }
                if(trackList.size() > 0)
                {
                    m_selectedTrack = trackList.begin()->second;
                    ui->boneList->setCurrentRow(0);
                    //Add track marks to the slider
                    for (int i = 0; i < m_selectedTrack->getNumKeyFrames(); i++)
                    {
                        Ogre::KeyFrame* keyFrame = m_selectedTrack->getKeyFrame(i);
                        ui->horizontalSlider->addTick(keyFrame->getTime()*1000, QColor("yellow"));
                    }
                }
            }
        }
    });
    connect(ui->boneList, &QListWidget::itemSelectionChanged, this, [=](){
        //clear selected from all bones
        for(int i = 0; i < m_selectedSkeleton->getNumBones(); i++)
        {
            m_selectedSkeleton->getBone(i)->getUserObjectBindings().setUserAny("selected",Ogre::Any(false));
        }
        if(ui->boneList->selectedItems().size() > 0)
        {
            auto selected = ui->boneList->selectedItems().at(0);
            m_selectedTrack = selected->data(Qt::UserRole).value<Ogre::NodeAnimationTrack*>();

            //set selected to bone user data
            m_selectedSkeleton->getBone(m_selectedTrack->getAssociatedNode()->getName())->getUserObjectBindings().setUserAny("selected",Ogre::Any(true));
        }
    });
    connect(ui->horizontalSlider,SIGNAL(valueChanged(int)),this,SLOT(setAnimationFrame(int)));
    QTimer *m_pTimer = new QTimer(this);
    connect(m_pTimer, &QTimer::timeout, this, [this](){
        if(m_selectedEntity)
        {
            // Updates the slider
            Ogre::AnimationState* state = m_selectedEntity->getAnimationState(m_selectedAnimation);
            ui->horizontalSlider->setValue(state->getTimePosition()*1000);
        }
    });
    m_pTimer->start(0);
}

AnimationControlWidget::~AnimationControlWidget()
{
    delete ui;
}

void AnimationControlWidget::updateAnimationTree()
{
    ui->treeWidget->clear();
    for(Ogre::Entity* entity : SelectionSet::getSingleton()->getEntitiesSelectionList())
    {
        // get skeleton
        Ogre::SkeletonInstance* skeleton = entity->getSkeleton();
        QTreeWidgetItem* item = new QTreeWidgetItem(ui->treeWidget);
        // only allow expanding, not selecting
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        item->setText(0,QString::fromStdString("mesh: "+entity->getName()));
        //Animation
        Ogre::AnimationStateSet* set = entity->getAllAnimationStates();
        if(set)
        {
            for (const auto &animationState:set->getAnimationStates())
            {
                QTreeWidgetItem* child = new QTreeWidgetItem(item);
                child->setText(0,QString::fromStdString("anim: "+animationState.first));
                //add skeleton and animation state to item data
                child->setData(0,Qt::UserRole,QVariant::fromValue(std::make_tuple(entity,skeleton,animationState.second)));
            }
        }
    }

}

void AnimationControlWidget::setAnimationFrame(int time)
{
    if (m_selectedEntity)
    {
        Ogre::AnimationState* state = m_selectedEntity->getAnimationState(m_selectedAnimation);
        state->setTimePosition(time/1000.0f);

        if(m_selectedTrack){
            Ogre::KeyFrame* keyframe1;
            Ogre::KeyFrame* keyframe2;
            m_selectedTrack->getKeyFramesAtTime(time/1000.0f, &keyframe1, &keyframe2);
            if(keyframe1)
            {
                Ogre::TransformKeyFrame* keyframe = static_cast<Ogre::TransformKeyFrame*>(keyframe1);

                Ogre::Vector3 translation = keyframe->getTranslate();
                Ogre::Quaternion rotation = keyframe->getRotation();
                Ogre::Vector3 scale = keyframe->getScale();

                ui->tableWidget->item(0,1)->setText(QString::number(translation.x));
                ui->tableWidget->item(0,2)->setText(QString::number(translation.y));
                ui->tableWidget->item(0,3)->setText(QString::number(translation.z));
                ui->tableWidget->item(1,1)->setText(QString::number(scale.x));
                ui->tableWidget->item(1,2)->setText(QString::number(scale.y));
                ui->tableWidget->item(1,3)->setText(QString::number(scale.z));
                ui->tableWidget->item(2,0)->setText(QString::number(rotation.w));
                ui->tableWidget->item(2,1)->setText(QString::number(rotation.x));
                ui->tableWidget->item(2,2)->setText(QString::number(rotation.y));
                ui->tableWidget->item(2,3)->setText(QString::number(rotation.z));
            }
        }
    }

}
