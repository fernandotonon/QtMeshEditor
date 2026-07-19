#include <QDebug>

#include <QInputDialog>
#include <QFileDialog>
#include <QMessageBox>
#include <QPointer>

#include <OgreAnimationState.h>

#include "GlobalDefinitions.h"

#include "SkeletonTransform.h"
#include "SelectionSet.h"
#include "Manager.h"
#include "AnimationWidget.h"
#include "SentryReporter.h"

AnimationWidget::AnimationWidget(QWidget *parent) :
    QWidget(parent)
{
    ui->setupUi(this);

    ui->animTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->animTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    ui->skeletonTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->skeletonTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    auto updateTables = [this]() {
        updateAnimationTable();
        updateSkeletonTable();
    };
    connect(SelectionSet::getSingleton(),&SelectionSet::entitySelectionChanged,this,updateTables);
    connect(SelectionSet::getSingleton(),&SelectionSet::nodeSelectionChanged,this,updateTables);

    // Clean up ALL skeleton debug/weight overlays before scene is cleared.
    // This must happen before any nodes are destroyed so that SkeletonDebug
    // timers don't fire with dangling entity pointers during teardown.
    connect(Manager::getSingleton(), &Manager::sceneClearing, this, [this]() {
        disableAllSkeletonDebug();
    });

    connect(Manager::getSingleton(), &Manager::sceneNodeDestroyed, this, [this](Ogre::SceneNode* const& node) {
        // Clean up any remaining SkeletonDebug and BoneWeightOverlay instances
        // for entities attached to this node (e.g. single-node deletion).
        QList<Ogre::Entity*> entities;
        for(auto* obj : node->getAttachedObjects())
        {
            if(obj->getMovableType() == "Entity")
                entities.append(static_cast<Ogre::Entity*>(obj));
        }
        for(auto* entity : entities)
        {
            delete mWeightOverlays.take(entity);
            delete mShowSkeleton.take(entity);
        }
    });

    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout, this, &AnimationWidget::pollAnimationState);
    m_pollTimer->start(200);
}

AnimationWidget::~AnimationWidget()
{
    disableAllSkeletonDebug();
}

bool AnimationWidget::isSkeletonShown(Ogre::Entity * entity) const
{
    return mShowSkeleton.contains(entity) && mShowSkeleton.find(entity).value()->bonesShown();
}

bool AnimationWidget::isSkeletonDebugActive(Ogre::Entity* entity) const
{
    // "Active" means the user-visible skeleton overlay (bones), not a
    // rest-pose ghost host that happens to share SkeletonDebug.
    return isSkeletonShown(entity);
}

bool AnimationWidget::isBoneWeightsShown(Ogre::Entity* entity) const
{
    return mWeightOverlays.contains(entity);
}

bool AnimationWidget::toggleSkeletonDebug(Ogre::Entity* entity, bool show)
{
    if (!entity || !entity->hasSkeleton())
        return false;

    SkeletonDebug* sd;
    if (mShowSkeleton.contains(entity))
        sd = mShowSkeleton.value(entity);
    else
    {
        sd = new SkeletonDebug(entity, Manager::getSingleton()->getSceneMgr(), 0.1f, 0.01f);
        mShowSkeleton.insert(entity, sd);
    }

    sd->showBones(show);

    if (!show && mShowSkeleton.contains(entity))
    {
        sd->showAxes(false);
        sd->showNames(false);
        // Keep the host alive when rest-pose ghost still needs it.
        if (!sd->restGhostShown()) {
            mShowSkeleton.remove(entity);
            // QMap of raw pointers doesn't own — delete explicitly. Without
            // this, the SkeletonDebug + its QTimer (which fires every tick
            // touching mBoneEntities) leaks. On the next enable, a *new*
            // SkeletonDebug attaches new visuals, the leaked one's timer
            // races with the new attachments via attachObjectToBone, and
            // touches dangling Ogre::Entity pointers → SIGSEGV at 0xf8.
            delete sd; // NOSONAR — manual delete needed since QMap doesn't own
        }
    }
    else if (show && mWeightOverlays.contains(entity))
    {
        // Reconnect to existing bone weight overlay so bone clicks update it
        auto* overlay = mWeightOverlays.value(entity);
        connect(sd, &SkeletonDebug::boneSelected, overlay, &BoneWeightOverlay::setSelectedBone);
        if (sd->selectedBoneIndex() >= 0)
            overlay->setSelectedBone(static_cast<unsigned short>(sd->selectedBoneIndex()));
    }

    updateSkeletonTable();
    return true;
}

bool AnimationWidget::toggleBoneWeights(Ogre::Entity* entity, bool show)
{
    if (!entity || !entity->hasSkeleton())
        return false;

    if (show)
    {
        if (mWeightOverlays.contains(entity))
            return true; // already shown

        auto* overlay = new BoneWeightOverlay(entity, Manager::getSingleton()->getSceneMgr());
        mWeightOverlays.insert(entity, overlay);

        if (mShowSkeleton.contains(entity))
        {
            auto* sd = mShowSkeleton.value(entity);
            connect(sd, &SkeletonDebug::boneSelected, overlay, &BoneWeightOverlay::setSelectedBone);
            if (sd->selectedBoneIndex() >= 0)
                overlay->setSelectedBone(static_cast<unsigned short>(sd->selectedBoneIndex()));
        }

        overlay->setVisible(true);
    }
    else
    {
        if (mWeightOverlays.contains(entity))
        {
            delete mWeightOverlays.value(entity);
            mWeightOverlays.remove(entity);
        }
    }

    updateSkeletonTable();
    return true;
}

void AnimationWidget::rebuildSkeletonOverlays(Ogre::Entity* entity)
{
    if (!entity || !entity->hasSkeleton())
        return;

    if (mShowSkeleton.contains(entity))
        mShowSkeleton.value(entity)->rebuildVisuals();
    if (mWeightOverlays.contains(entity))
        mWeightOverlays.value(entity)->rebuildVisuals();

    updateSkeletonTable();
}

void AnimationWidget::setRestPoseGhostVisible(bool show)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return;

    if (show) {
        // Create a SkeletonDebug host when needed, but do NOT enable bone
        // visuals — rest ghost is independent of the Skeleton checkbox.
        for (Ogre::Entity* ent : mgr->getEntities()) {
            if (!ent || ent->getMovableType() != "Entity" || !ent->hasSkeleton())
                continue;
            SkeletonDebug* sd = nullptr;
            if (mShowSkeleton.contains(ent)) {
                sd = mShowSkeleton.value(ent);
            } else {
                sd = new SkeletonDebug(ent, mgr->getSceneMgr(), 0.1f, 0.01f);
                mShowSkeleton.insert(ent, sd);
            }
            sd->showRestGhost(true);
        }
        return;
    }

    // Hide ghost only. Destroy hosts that exist solely for the ghost
    // (bones never shown) so we don't leave invisible SkeletonDebug timers.
    QList<Ogre::Entity*> toRemove;
    for (auto it = mShowSkeleton.begin(); it != mShowSkeleton.end(); ++it) {
        SkeletonDebug* sd = it.value();
        if (!sd) continue;
        sd->showRestGhost(false);
        if (!sd->bonesShown())
            toRemove.append(it.key());
    }
    for (Ogre::Entity* ent : toRemove) {
        SkeletonDebug* sd = mShowSkeleton.take(ent);
        delete sd; // NOSONAR — QMap doesn't own
    }
}

SkeletonDebug* AnimationWidget::getSkeletonDebug(Ogre::Entity* entity) const
{
    if (mShowSkeleton.contains(entity))
        return mShowSkeleton.value(entity);
    return nullptr;
}

BoneWeightOverlay* AnimationWidget::getBoneWeightOverlay(Ogre::Entity* entity) const
{
    if (mWeightOverlays.contains(entity))
        return mWeightOverlays.value(entity);
    return nullptr;
}

void AnimationWidget::updateAnimationTable()
{
    while(ui->animTable->rowCount())
    {
        ui->animTable->removeRow(0);
    }

    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    if (entities.isEmpty())
        return;

    bool hasAnimationEnable = false;

    for(Ogre::Entity* entity : entities)
    {
        //Animation
        const Ogre::AnimationStateSet* set = entity->getAllAnimationStates();
        if(!set) continue;

        for (const auto &animationState:set->getAnimationStates())
        {
            auto entityItem = new QTableWidgetItem;
            entityItem->setText(entity->getName().data());
            entityItem->setData(ENTITY_DATA,QVariant::fromValue((void *) entity));
            entityItem->setFlags(entityItem->flags() & ~Qt::ItemIsEditable);

            const QString animationName = animationState.second->getAnimationName().c_str();

            auto animationItem = new QTableWidgetItem;
            animationItem->setText(animationName);
            animationItem->setFlags(animationItem->flags() & ~Qt::ItemIsEditable);

            auto enabledCB = new QTableWidgetItem(0);
            enabledCB->setCheckState(animationState.second->getEnabled()?Qt::Checked:Qt::Unchecked);
            enabledCB->setFlags(enabledCB->flags() & ~Qt::ItemIsEditable);
            hasAnimationEnable = hasAnimationEnable || animationState.second->getEnabled();

            auto loopCB = new QTableWidgetItem(0);
            loopCB->setCheckState(animationState.second->getLoop()?Qt::Checked:Qt::Unchecked);
            loopCB->setFlags(loopCB->flags() & ~Qt::ItemIsEditable);

            ui->animTable->insertRow(0);
            ui->animTable->setItem(0,0,entityItem);
            ui->animTable->setItem(0,1,animationItem);
            ui->animTable->setItem(0,2,enabledCB);
            ui->animTable->setItem(0,3,loopCB);
        }
    }

}

void AnimationWidget::updateSkeletonTable()
{
    while(ui->skeletonTable->rowCount())
    {
        ui->skeletonTable->removeRow(0);
    }

    auto entities = SelectionSet::getSingleton()->getResolvedEntities();
    if (entities.isEmpty())
        return;

    for(Ogre::Entity* entity : entities)
    {
        QString str = entity->getName().data();
        auto entityItem = new QTableWidgetItem;
        entityItem->setText(str);
        entityItem->setData(ENTITY_DATA,QVariant::fromValue((void *) entity));
        entityItem->setFlags(entityItem->flags() & ~Qt::ItemIsEditable);

        auto showSkeletonCB = new QTableWidgetItem(0);
        showSkeletonCB->setCheckState(mShowSkeleton.contains(entity)?Qt::Checked:Qt::Unchecked);
        showSkeletonCB->setFlags(showSkeletonCB->flags() & ~Qt::ItemIsEditable);

        auto showWeightsCB = new QTableWidgetItem(0);
        showWeightsCB->setCheckState(mWeightOverlays.contains(entity)?Qt::Checked:Qt::Unchecked);
        showWeightsCB->setFlags(showWeightsCB->flags() & ~Qt::ItemIsEditable);
        if (!entity->hasSkeleton())
            showWeightsCB->setFlags(showWeightsCB->flags() & ~Qt::ItemIsEnabled);

        ui->skeletonTable->insertRow(0);
        ui->skeletonTable->setItem(0,0,entityItem);
        ui->skeletonTable->setItem(0,1,showSkeletonCB);
        ui->skeletonTable->setItem(0,2,showWeightsCB);
    }
}

void AnimationWidget::on_PlayPauseButton_toggled(bool checked)
{
    SentryReporter::addBreadcrumb("ui.animation", "Toggle animation playback");
    setAnimationState(checked);
}

void AnimationWidget::setAnimationState(bool playing)
{
    auto icon = QIcon(playing?":/icones/pause.png":":/icones/play.png");

    ui->PlayPauseButton->setIcon(icon);

    emit changeAnimationState(playing);
}

void AnimationWidget::pollAnimationState()
{
    // Refresh table checkboxes to match actual Ogre animation state
    // (e.g. changed externally via MCP), but do NOT touch Play/Pause button
    for(int row = 0; row < ui->animTable->rowCount(); ++row)
    {
        auto* entityItem = ui->animTable->item(row, 0);
        auto* enabledItem = ui->animTable->item(row, 2);
        if(!entityItem || !enabledItem) continue;

        auto* entity = static_cast<Ogre::Entity*>(entityItem->data(ENTITY_DATA).value<void*>());
        if(!entity) continue;

        auto* animNameItem = ui->animTable->item(row, 1);
        if(!animNameItem) continue;

        Ogre::AnimationState* animState = entity->getAnimationState(animNameItem->text().toStdString());
        if(!animState) continue;

        Qt::CheckState expected = animState->getEnabled() ? Qt::Checked : Qt::Unchecked;
        if(enabledItem->checkState() != expected)
            enabledItem->setCheckState(expected);
    }
}

void AnimationWidget::on_skeletonTable_clicked(const QModelIndex &index)
{
    if(index.column() != 1 && index.column() != 2)
        return;

    auto entity = (Ogre::Entity*)ui->skeletonTable->model()->data(ui->skeletonTable->model()->index(index.row(),0), ENTITY_DATA).value<void *>();

    if(!entity || !entity->hasSkeleton())
        return;

    if(index.column() == 1)
    {
        bool checked = (index.data(Qt::CheckStateRole) == Qt::Checked);
        toggleSkeletonDebug(entity, checked);
        // Also toggle axes when using GUI (original behavior)
        if(checked)
        {
            auto* sd = getSkeletonDebug(entity);
            if(sd) sd->showAxes(true);
        }
    }
    else if(index.column() == 2)
    {
        bool checked = (index.data(Qt::CheckStateRole) == Qt::Checked);
        toggleBoneWeights(entity, checked);
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
        for (const auto &[key, value] : set->getAnimationStates())
        {
            value->setEnabled(false);
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
            for (const auto &[key, value] : set->getAnimationStates())
            {
                value->setEnabled(false);
            }
        }
    }
    updateAnimationTable();
}

void AnimationWidget::disableAllSkeletonDebug()
{
    for(BoneWeightOverlay *overlay : mWeightOverlays.values())
    {
        delete overlay;
    }
    mWeightOverlays.clear();

    for(SkeletonDebug *sd : mShowSkeleton.values())
    {
        sd->showAxes(false);
        sd->showBones(false);
        delete sd;
    }

    mShowSkeleton.clear();
    updateSkeletonTable();
}

void AnimationWidget::on_animTable_clicked(const QModelIndex &index) const
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



