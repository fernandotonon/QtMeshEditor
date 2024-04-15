#ifndef TRANSFORM_WIDGET_H
#define TRANSFORM_WIDGET_H

#include <QWidget>
#include <Ogre.h>
#include "ui_TransformWidget.h"

class QItemSelection;
class ObjectItemModel;

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
    Ui::TransformWidget* ui = new Ui::TransformWidget;
    ObjectItemModel*    m_pObjectTreeModel;
};

#endif // TRANSFORM_WIDGET_H
