#ifndef ANIMATIONCONTROLWIDGET_H
#define ANIMATIONCONTROLWIDGET_H

#include <QDockWidget>

namespace Ui {
class AnimationControlWidget;
}

namespace Ogre {
    class Entity;
    class NodeAnimationTrack;
    class SkeletonInstance;
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

private:
    Ui::AnimationControlWidget *ui;
    std::string m_selectedAnimation="";
    Ogre::NodeAnimationTrack* m_selectedTrack=nullptr;
    Ogre::SkeletonInstance* m_selectedSkeleton=nullptr;
    Ogre::Entity* m_selectedEntity=nullptr;
    float m_time = 0.0f;
};

#endif // ANIMATIONCONTROLWIDGET_H
