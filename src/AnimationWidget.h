#ifndef ANIMATIONWIDGET_H
#define ANIMATIONWIDGET_H

#include <QWidget>
#include <QMap>
#include "SkeletonDebug.h"

namespace Ui {
class AnimationWidget;
}
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
    void on_animTable_clicked(const QModelIndex &index);
    void on_skeletonTable_clicked(const QModelIndex &index);

    void on_mergeButton_clicked();

signals:
    void changeAnimationState(bool playing);
    void changeAnimationName(const std::string &newName);

private:
    Ui::AnimationWidget*  ui;
    QMap<Ogre::Entity*,SkeletonDebug*> mShowSkeleton;
};

#endif // ANIMATIONWIDGET_H
