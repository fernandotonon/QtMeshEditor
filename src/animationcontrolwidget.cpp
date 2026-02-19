#include "animationcontrolwidget.h"
#include "ui_animationcontrolwidget.h"
#include "SelectionSet.h"
#include "Manager.h"
#include <QTreeWidgetItem>
#include <QTimer>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <cmath>

// Returns entities from the current selection. If entities are directly selected,
// returns those. Otherwise resolves selected nodes to their attached entities.
static QList<Ogre::Entity*> getSelectedEntities()
{
    const auto* sel = SelectionSet::getSingleton();
    if (sel->hasEntities())
        return sel->getEntitiesSelectionList();

    QList<Ogre::Entity*> entities;
    if (!sel->hasNodes())
        return entities;

    const auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    for (const auto* node : sel->getNodesSelectionList())
    {
        if (sceneMgr->hasEntity(node->getName()))
            entities.append(sceneMgr->getEntity(node->getName()));
    }
    return entities;
}

AnimationControlWidget::AnimationControlWidget(QWidget *parent) :
    QDockWidget(parent),
    ui(new Ui::AnimationControlWidget)
{
    ui->setupUi(this);
    updateAnimationTree();

    connect(SelectionSet::getSingleton(),SIGNAL(entitySelectionChanged()),this,SLOT(updateAnimationTree()));
    connect(SelectionSet::getSingleton(),SIGNAL(nodeSelectionChanged()),this,SLOT(updateAnimationTree()));
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

                ui->lengthSpinBox->blockSignals(true);
                ui->lengthSpinBox->setValue(animation->getLength());
                ui->lengthSpinBox->blockSignals(false);
                ui->lengthSpinBox->setEnabled(true);

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
                    refreshSliderTicks();
                    bool hasKeyframes = m_selectedTrack->getNumKeyFrames() > 0;
                    ui->prevKeyframeButton->setEnabled(hasKeyframes);
                    ui->nextKeyframeButton->setEnabled(hasKeyframes);
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
            refreshSliderTicks();
            setAnimationFrame(ui->horizontalSlider->value());

            bool hasKeyframes = m_selectedTrack->getNumKeyFrames() > 0;
            ui->prevKeyframeButton->setEnabled(hasKeyframes);
            ui->nextKeyframeButton->setEnabled(hasKeyframes);
        }
    });
    connect(ui->horizontalSlider,SIGNAL(valueChanged(int)),this,SLOT(setAnimationFrame(int)));
    connect(ui->tableWidget, &QTableWidget::cellChanged, this, &AnimationControlWidget::onKeyframeValueChanged);
    connect(ui->addKeyframeButton, &QPushButton::clicked, this, &AnimationControlWidget::onAddKeyframe);
    connect(ui->deleteKeyframeButton, &QPushButton::clicked, this, &AnimationControlWidget::onDeleteKeyframe);
    connect(ui->prevKeyframeButton, &QPushButton::clicked, this, &AnimationControlWidget::onPrevKeyframe);
    connect(ui->nextKeyframeButton, &QPushButton::clicked, this, &AnimationControlWidget::onNextKeyframe);
    connect(ui->lengthSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &AnimationControlWidget::onAnimationLengthChanged);
    updateTableEditability(false);
    m_pTimer = new QTimer(this);
    connect(m_pTimer, &QTimer::timeout, this, [this](){
        if(!m_selectedEntity) return;
        if(!m_selectedEntity->hasAnimationState(m_selectedAnimation)) return;

        Ogre::AnimationState* state = m_selectedEntity->getAnimationState(m_selectedAnimation);
        int newValue = static_cast<int>(state->getTimePosition() * 1000);
        if (newValue != ui->horizontalSlider->value()) {
            ui->horizontalSlider->blockSignals(true);
            ui->horizontalSlider->setValue(newValue);
            ui->horizontalSlider->blockSignals(false);
            setAnimationFrame(newValue);
        }
    });
    m_pTimer->start(16);
}

AnimationControlWidget::~AnimationControlWidget()
{
    delete ui;
}

void AnimationControlWidget::updateAnimationTree()
{
    ui->treeWidget->clear();
    for(Ogre::Entity* entity : getSelectedEntities())
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
    if (!m_selectedEntity) return;
    if(!m_selectedEntity->hasAnimationState(m_selectedAnimation)) return;

    Ogre::AnimationState* state = m_selectedEntity->getAnimationState(m_selectedAnimation);
    state->setTimePosition(time/1000.0f);

    if(m_selectedTrack && m_selectedTrack->getNumKeyFrames() > 0){
        Ogre::KeyFrame* keyframe1 = nullptr;
        Ogre::KeyFrame* keyframe2 = nullptr;
        m_selectedTrack->getKeyFramesAtTime(time/1000.0f, &keyframe1, &keyframe2);

        // Pick the closest keyframe between kf1 and kf2
        Ogre::TransformKeyFrame* closest = nullptr;
        if (keyframe1 && keyframe2) {
            float dist1 = std::fabs(keyframe1->getTime() - time/1000.0f);
            float dist2 = std::fabs(keyframe2->getTime() - time/1000.0f);
            closest = static_cast<Ogre::TransformKeyFrame*>(dist1 <= dist2 ? keyframe1 : keyframe2);
        } else if (keyframe1) {
            closest = static_cast<Ogre::TransformKeyFrame*>(keyframe1);
        } else if (keyframe2) {
            closest = static_cast<Ogre::TransformKeyFrame*>(keyframe2);
        }

        if (closest && closest != m_currentKeyframe) {
            m_updatingTable = true;  // Guard: setBackground in updateTableEditability triggers cellChanged
            m_currentKeyframe = closest;
            updateTableEditability(true);
            ui->horizontalSlider->setSelectedTick(static_cast<int>(m_currentKeyframe->getTime() * 1000));
            m_updatingTable = false;
        }

        if (closest) {
            Ogre::Vector3 translation = closest->getTranslate();
            Ogre::Quaternion rotation = closest->getRotation();
            Ogre::Vector3 scale = closest->getScale();

            m_updatingTable = true;
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
            m_updatingTable = false;
        }
    } else {
        // No keyframes — clear selection
        if (m_currentKeyframe) {
            m_updatingTable = true;
            m_currentKeyframe = nullptr;
            updateTableEditability(false);
            ui->horizontalSlider->setSelectedTick(-1);
            m_updatingTable = false;
        }
    }
}

void AnimationControlWidget::onKeyframeValueChanged(int row, int col)
{
    if (m_updatingTable || !m_currentKeyframe) return;

    bool ok;
    float value = ui->tableWidget->item(row, col)->text().toFloat(&ok);
    if (!ok) {
        // Revert to current keyframe value on invalid input
        m_updatingTable = true;
        if (row == 0) {
            Ogre::Vector3 t = m_currentKeyframe->getTranslate();
            float vals[] = {0, t.x, t.y, t.z};
            ui->tableWidget->item(row, col)->setText(col == 0 ? "-" : QString::number(vals[col]));
        } else if (row == 1) {
            Ogre::Vector3 s = m_currentKeyframe->getScale();
            float vals[] = {0, s.x, s.y, s.z};
            ui->tableWidget->item(row, col)->setText(col == 0 ? "-" : QString::number(vals[col]));
        } else if (row == 2) {
            Ogre::Quaternion r = m_currentKeyframe->getRotation();
            float vals[] = {r.w, r.x, r.y, r.z};
            ui->tableWidget->item(row, col)->setText(QString::number(vals[col]));
        }
        m_updatingTable = false;
        return;
    }

    if (row == 0) {
        // Translation (col 0 is "-", not editable)
        Ogre::Vector3 t = m_currentKeyframe->getTranslate();
        if (col == 1) t.x = value;
        else if (col == 2) t.y = value;
        else if (col == 3) t.z = value;
        m_currentKeyframe->setTranslate(t);
    } else if (row == 1) {
        // Scale (col 0 is "-", not editable)
        Ogre::Vector3 s = m_currentKeyframe->getScale();
        if (col == 1) s.x = value;
        else if (col == 2) s.y = value;
        else if (col == 3) s.z = value;
        m_currentKeyframe->setScale(s);
    } else if (row == 2) {
        // Rotation
        Ogre::Quaternion r = m_currentKeyframe->getRotation();
        if (col == 0) r.w = value;
        else if (col == 1) r.x = value;
        else if (col == 2) r.y = value;
        else if (col == 3) r.z = value;
        m_currentKeyframe->setRotation(r);
    }

    // Refresh animation to show changes in viewport
    m_selectedEntity->getAllAnimationStates()->_notifyDirty();
    Ogre::AnimationState* state = m_selectedEntity->getAnimationState(m_selectedAnimation);
    state->setTimePosition(state->getTimePosition());
}

void AnimationControlWidget::onAddKeyframe()
{
    if (!m_selectedTrack || !m_selectedEntity) return;

    float time = ui->horizontalSlider->value() / 1000.0f;
    Ogre::TransformKeyFrame* newKf = m_selectedTrack->createNodeKeyFrame(time);

    // Get interpolated values at this time
    Ogre::TransformKeyFrame interpKf(nullptr, time);
    m_selectedTrack->getInterpolatedKeyFrame(m_selectedEntity->getAnimationState(m_selectedAnimation)->getTimePosition(), &interpKf);
    newKf->setTranslate(interpKf.getTranslate());
    newKf->setRotation(interpKf.getRotation());
    newKf->setScale(interpKf.getScale());

    refreshSliderTicks();
    // Update to recognize we're now on a keyframe
    setAnimationFrame(ui->horizontalSlider->value());
}

void AnimationControlWidget::onDeleteKeyframe()
{
    if (!m_selectedTrack || !m_currentKeyframe) return;

    float currentTime = m_currentKeyframe->getTime();
    // Find the index of this keyframe
    for (unsigned short i = 0; i < m_selectedTrack->getNumKeyFrames(); i++) {
        if (std::fabs(m_selectedTrack->getKeyFrame(i)->getTime() - currentTime) < 0.001f) {
            m_selectedTrack->removeKeyFrame(i);
            break;
        }
    }

    m_currentKeyframe = nullptr;
    refreshSliderTicks();
    setAnimationFrame(ui->horizontalSlider->value());
}

void AnimationControlWidget::refreshSliderTicks()
{
    ui->horizontalSlider->clearTicks();
    if (!m_selectedTrack) return;
    for (unsigned short i = 0; i < m_selectedTrack->getNumKeyFrames(); i++) {
        Ogre::KeyFrame* kf = m_selectedTrack->getKeyFrame(i);
        ui->horizontalSlider->addTick(kf->getTime() * 1000, QColor("yellow"));
    }
    // Restore selected tick indicator from current keyframe
    if (m_currentKeyframe) {
        ui->horizontalSlider->setSelectedTick(static_cast<int>(m_currentKeyframe->getTime() * 1000));
    }
    ui->horizontalSlider->update();
}

void AnimationControlWidget::onPrevKeyframe()
{
    if (!m_selectedTrack || m_selectedTrack->getNumKeyFrames() == 0) return;

    float time = ui->horizontalSlider->value() / 1000.0f;
    Ogre::TransformKeyFrame* target = nullptr;

    for (int i = static_cast<int>(m_selectedTrack->getNumKeyFrames()) - 1; i >= 0; i--) {
        Ogre::TransformKeyFrame* kf = static_cast<Ogre::TransformKeyFrame*>(m_selectedTrack->getKeyFrame(i));
        if (kf->getTime() < time - 0.001f) {
            target = kf;
            break;
        }
    }

    if (!target)
        target = static_cast<Ogre::TransformKeyFrame*>(m_selectedTrack->getKeyFrame(0));

    ui->horizontalSlider->setValue(static_cast<int>(target->getTime() * 1000));
}

void AnimationControlWidget::onNextKeyframe()
{
    if (!m_selectedTrack || m_selectedTrack->getNumKeyFrames() == 0) return;

    float time = ui->horizontalSlider->value() / 1000.0f;
    Ogre::TransformKeyFrame* target = nullptr;

    for (unsigned short i = 0; i < m_selectedTrack->getNumKeyFrames(); i++) {
        Ogre::TransformKeyFrame* kf = static_cast<Ogre::TransformKeyFrame*>(m_selectedTrack->getKeyFrame(i));
        if (kf->getTime() > time + 0.001f) {
            target = kf;
            break;
        }
    }

    if (!target)
        target = static_cast<Ogre::TransformKeyFrame*>(m_selectedTrack->getKeyFrame(m_selectedTrack->getNumKeyFrames() - 1));

    ui->horizontalSlider->setValue(static_cast<int>(target->getTime() * 1000));
}

void AnimationControlWidget::onAnimationLengthChanged(double length)
{
    if (!m_selectedSkeleton || !m_selectedEntity) return;

    Ogre::Animation* animation = m_selectedSkeleton->getAnimation(m_selectedAnimation);
    if (!animation) return;

    animation->setLength(static_cast<float>(length));

    // Update slider maximum and label
    ui->horizontalSlider->setMaximum(static_cast<int>(length * 1000));
    ui->maxSliderLabel->setText(QString::number(length));

    // Clamp slider if current position exceeds new length
    if (ui->horizontalSlider->value() > ui->horizontalSlider->maximum()) {
        ui->horizontalSlider->setValue(ui->horizontalSlider->maximum());
    }

    // Refresh the animation state
    m_selectedEntity->getAllAnimationStates()->_notifyDirty();
    if (m_selectedEntity->hasAnimationState(m_selectedAnimation)) {
        Ogre::AnimationState* state = m_selectedEntity->getAnimationState(m_selectedAnimation);
        state->setLength(static_cast<float>(length));
        state->setTimePosition(std::min(state->getTimePosition(), static_cast<float>(length)));
    }

    refreshSliderTicks();
}

void AnimationControlWidget::updateTableEditability(bool onKeyframe)
{
    QColor bgColor = onKeyframe ? Qt::white : QColor(220, 220, 220);
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 4; col++) {
            QTableWidgetItem* item = ui->tableWidget->item(row, col);
            if (!item) continue;

            // Translation/Scale row col 0 ("-") always non-editable
            bool editable = onKeyframe && !(row < 2 && col == 0);
            Qt::ItemFlags flags = item->flags();
            if (editable)
                flags |= Qt::ItemIsEditable;
            else
                flags &= ~Qt::ItemIsEditable;
            item->setFlags(flags);
            item->setBackground(bgColor);
        }
    }
    ui->deleteKeyframeButton->setEnabled(onKeyframe);
}
