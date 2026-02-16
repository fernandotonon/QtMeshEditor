#ifndef ANIMATIONCONTROLWIDGET_H
#define ANIMATIONCONTROLWIDGET_H

#include <QDockWidget>

class QTimer;

namespace Ui {
class AnimationControlWidget;
}

namespace Ogre {
    class Entity;
    class NodeAnimationTrack;
    class SkeletonInstance;
    class TransformKeyFrame;
}

class AnimationControlWidget : public QDockWidget
{
    Q_OBJECT

public:
    explicit AnimationControlWidget(QWidget *parent = nullptr);
    ~AnimationControlWidget();

public slots:
    void updateAnimationTree();

private slots:
    void setAnimationFrame(int time);
    void onKeyframeValueChanged(int row, int col);
    void onAddKeyframe();
    void onDeleteKeyframe();
    void onPrevKeyframe();
    void onNextKeyframe();
    void onAnimationLengthChanged(double length);

private:
    void refreshSliderTicks();
    void updateTableEditability(bool onKeyframe);

    Ui::AnimationControlWidget *ui;
    QTimer* m_pTimer = nullptr;
    std::string m_selectedAnimation="";
    Ogre::NodeAnimationTrack* m_selectedTrack=nullptr;
    Ogre::SkeletonInstance* m_selectedSkeleton=nullptr;
    Ogre::Entity* m_selectedEntity=nullptr;
    Ogre::TransformKeyFrame* m_currentKeyframe = nullptr;
    float m_time = 0.0f;
    bool m_updatingTable = false;
};

#endif // ANIMATIONCONTROLWIDGET_H
