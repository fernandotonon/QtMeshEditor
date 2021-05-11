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

#ifndef TRANSFORM_WIDGET_H
#define TRANSFORM_WIDGET_H

#include <QWidget>

namespace Ui {
class TransformWidget;
}

class QItemSelection;
class ObjectItemModel;

namespace Ogre {
class SceneNode;
class Vector3;
}
class TransformWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TransformWidget(QWidget *parent = 0);
    ~TransformWidget();

private slots:
    void updateTreeViewFromSelection();
    void treeWidgetSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected);

    void updatePosition(const Ogre::Vector3& newPosition);
    void updateNodeScale(const Ogre::Vector3& newScale);
    void updateNodeOrientation(const Ogre::Vector3& newOrientation);

    void onPositionEdited();
    void onNodeScaleEdited();
    void onNodeOrientationEdited();

signals:
    void selectionChanged(const QString& newSelection);
    void selectionChanged(Ogre::SceneNode* newNode);

private:
    Ui::TransformWidget* ui;
    ObjectItemModel*    m_pObjectTreeModel;

};

#endif // TRANSFORM_WIDGET_H
