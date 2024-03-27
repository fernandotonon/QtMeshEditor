#ifndef PRIMITIVES_WIDGET_H
#define PRIMITIVES_WIDGET_H

#include <QWidget>
#include <OgrePrerequisites.h>
#include "PrimitiveObject.h"

#include "ui_PrimitivesWidget.h"

class PrimitivesWidget : public QWidget,private Ui::PrimitivesWidget
{
    Q_OBJECT
    
public:
    explicit PrimitivesWidget(QWidget *parent = 0);
    ~PrimitivesWidget();
    void updateUiFromParams();

public slots:
    void onSelectionChanged();
    void createCube();
    void createSphere();
    void createPlane();
    void createCylinder();
    void createCone();
    void createTorus();
    void createTube();
    void createCapsule();
    void createIcoSphere();
    void createRoundedBox();
    void createSpring();

protected:
    void setUiEmpty();
    void setUiMesh();
    void setUiCube();
    void setUiSphere();
    void setUiPlane();
    void setUiCylinder();
    void setUiCone();
    void setUiTorus();
    void setUiTube();
    void setUiCapsule();
    void setUiIcoSphere();
    void setUiRoundedBox();
    void setUiSpring();
    void createPrimitive(PrimitiveObject::PrimitiveType newPrimitive, const QString& name);
    PrimitiveObject::PrimitiveType getSelectedPrimitive();
    void blockEditSignals(bool block);
    void setUVTileVisible(bool visible = true);

private slots:

    void onEditSizeX();
    void onEditSizeY();
    void onEditSizeZ();

    void onEditRadius();
    void onEditRadius2();
    void onEditHeight();

    void onEditNumSegX();
    void onEditNumSegY();
    void onEditNumSegZ();

    void onEditUTile();
    void onEditVTile();
    void onToggleSwitchUV();

private:
    QList<PrimitiveObject*> mSelectedPrimitive;
};

#endif // PRIMITIVES_WIDGET_H
