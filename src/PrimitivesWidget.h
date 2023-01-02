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

private:
/*
    enum PrimitiveTypeTEMP
    {
        AP_NONE,
        AP_CUBE,
        AP_SPHERE,
        AP_PLAN,
        AP_CYLINDER,
        AP_CONE,
        AP_TORUS,
        AP_TUBE,
        AP_CAPSULE,
        AP_ICOSPHERE,
        AP_ROUNDEDBOX
    };
    struct PrimitiveObjectTEMP
    {
        PrimitiveObjectTEMP():type(AP_NONE),
            sizeX(1.0f),sizeY(1.0f),sizeZ(1.0f),
            radius(1.0f),radius2(0.5f),height(1.0f),
            numSegX(1),numSegY(1),numSegZ(1),
            uTile(1.0f),vTile(1.0f),switchUV(false){}

        PrimitiveTypeTEMP type;
        Ogre::Real  sizeX;
        Ogre::Real  sizeY;
        Ogre::Real  sizeZ;
        Ogre::Real  radius;
        Ogre::Real  radius2;
        Ogre::Real  height;
        int         numSegX;
        int         numSegY;
        int         numSegZ;
        Ogre::Real  uTile;
        Ogre::Real  vTile;
        bool        switchUV;
    };

*/
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

signals:
    void newPrimitiveCreated(const QString& name);
private:
    void setUiEmpty();
    void setUiMesh();
    void setUiCube();
    void setUiSphere();
    void setUiPlan();
    void setUiCylinder();
    void setUiCone();
    void setUiTorus();
    void setUiTube();
    void setUiCapsule();
    void setUiIcoSphere();
    void setUiRoundedBox();
    void updateUiFromParams();
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
    QLineEdit* mUiPrimitiveType;
    QList<PrimitiveObject*> mSelectedPrimitive;
};

#endif // PRIMITIVES_WIDGET_H
