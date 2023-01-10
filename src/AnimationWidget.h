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
    explicit AnimationWidget(QWidget *parent = 0);
    ~AnimationWidget();
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

signals:
    void changeAnimationState(bool playing);

private:
    Ui::AnimationWidget*  ui;
    QMap<Ogre::Entity*,SkeletonDebug*> mShowSkeleton;
};

#endif // ANIMATIONWIDGET_H
