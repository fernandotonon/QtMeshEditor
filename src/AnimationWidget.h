#ifndef ANIMATIONWIDGET_H
#define ANIMATIONWIDGET_H

#include <QWidget>
#include <QMap>
#include <QScopedPointer>
#include <QTimer>
#include "SkeletonDebug.h"
#include "BoneWeightOverlay.h"
#include "ui_animationwidget.h"

namespace Ogre{
    class AnimationState;
}

class AnimationWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AnimationWidget(QWidget *parent = nullptr);
    ~AnimationWidget() override;
    bool isSkeletonShown(Ogre::Entity*entity) const;
    bool isBoneWeightsShown(Ogre::Entity* entity) const;
    bool toggleSkeletonDebug(Ogre::Entity* entity, bool show);
    bool toggleBoneWeights(Ogre::Entity* entity, bool show);
    SkeletonDebug* getSkeletonDebug(Ogre::Entity* entity) const;
    BoneWeightOverlay* getBoneWeightOverlay(Ogre::Entity* entity) const;

private:
    void setAnimationState(bool playing);
    void disableAllSelectedAnimations();
    void disableAllSkeletonDebug();
    void disableEntityAnimations(Ogre::Entity* entity);

private slots:
    void updateAnimationTable();
    void updateSkeletonTable();
    void on_PlayPauseButton_toggled(bool checked);
    void on_animTable_cellDoubleClicked(int row, int column);
    void on_animTable_clicked(const QModelIndex &index) const;
    void on_skeletonTable_clicked(const QModelIndex &index);

    void on_mergeButton_clicked();
    void pollAnimationState();

signals:
    void changeAnimationState(bool playing);
    void changeAnimationName(const std::string &newName);

private:
    QScopedPointer<Ui::AnimationWidget> ui { new Ui::AnimationWidget };
    QMap<Ogre::Entity*,SkeletonDebug*> mShowSkeleton;
    QMap<Ogre::Entity*, BoneWeightOverlay*> mWeightOverlays;
    QTimer* m_pollTimer = nullptr;
    bool m_lastPollAnimEnabled = false;
};

#endif // ANIMATIONWIDGET_H
