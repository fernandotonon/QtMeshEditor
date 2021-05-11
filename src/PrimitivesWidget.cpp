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

#include <QDebug>

#include <QInputDialog>
#include <QHBoxLayout>
#include <QLabel>

#include <OgreAny.h>
#include <OgreUserObjectBindings.h>

#include "ProceduralStableHeaders.h" ///REMOVE
#include "Procedural.h"             ///REMOVE

#include "SelectionSet.h"
#include "Manager.h"
#include "PrimitivesWidget.h"

#include "mainwindow.h"

// TODO add spring primitive

// TODO play with numTextureCoord (require to change the UI)

// TODO fix the bug in the num of segment for rounded box

// TODO switchUV doesn't seems to work

PrimitivesWidget::PrimitivesWidget(QWidget *parent)
    :QWidget(parent)
{
    setupUi(this);
    setUiEmpty();

    connect(edit_sizeX,SIGNAL(valueChanged(double)),this,SLOT(onEditSizeX()));
    connect(edit_sizeY,SIGNAL(valueChanged(double)),this,SLOT(onEditSizeY()));
    connect(edit_sizeZ,SIGNAL(valueChanged(double)),this,SLOT(onEditSizeZ()));
    connect(edit_radius,SIGNAL(valueChanged(double)),this,SLOT(onEditRadius()));
    connect(edit_radius2,SIGNAL(valueChanged(double)),this,SLOT(onEditRadius2()));
    connect(edit_height,SIGNAL(valueChanged(double)),this,SLOT(onEditHeight()));

    connect(edit_numSegX,SIGNAL(valueChanged(int)),this,SLOT(onEditNumSegX()));
    connect(edit_numSegY,SIGNAL(valueChanged(int)),this,SLOT(onEditNumSegY()));
    connect(edit_numSegZ,SIGNAL(valueChanged(int)),this,SLOT(onEditNumSegZ()));
    connect(edit_UTile,SIGNAL(valueChanged(double)),this,SLOT(onEditUTile()));
    connect(edit_VTile,SIGNAL(valueChanged(double)),this,SLOT(onEditVTile()));
    connect(pb_switchUV,SIGNAL(toggled(bool)),this,SLOT(onToggleSwitchUV()));

    connect(SelectionSet::getSingleton(),SIGNAL(nodeSelectionChanged()),this,SLOT(onSelectionChanged()));
}

PrimitivesWidget::~PrimitivesWidget()
{
}

void PrimitivesWidget::setUiEmpty()
{
    edit_type->setText(QString::null);

    gb_Geometry->hide();
    gb_Mesh->hide();

    label_sizeX->hide();
    label_sizeY->hide();
    label_sizeZ->hide();

    edit_sizeX->hide();
    edit_sizeY->hide();
    edit_sizeZ->hide();

    label_radius->hide();
    label_radius2->hide();
    label_height->hide();

    edit_radius->hide();
    edit_radius2->hide();
    edit_height->hide();

    label_numSegX->hide();
    label_numSegY->hide();
    label_numSegZ->hide();

    edit_numSegX->hide();
    edit_numSegY->hide();
    edit_numSegZ->hide();

    setUVTileVisible(false);
}

void PrimitivesWidget::setUiMesh()
{
    setUiEmpty();
    edit_type->setText(tr("Mesh"));
}
void PrimitivesWidget::setUVTileVisible(bool visible)
{
    label_UTile->setVisible(visible);
    label_VTile->setVisible(visible);

    edit_UTile->setVisible(visible);
    edit_VTile->setVisible(visible);
    pb_switchUV->setVisible(visible);
}

void PrimitivesWidget::setUiCube()
{

    edit_type->setText(tr("Cube"));

    gb_Geometry->show();
    gb_Mesh->show();

    edit_sizeX->show();
    edit_sizeY->show();
    edit_sizeZ->show();

    label_sizeX->show();
    label_sizeY->show();
    label_sizeZ->show();

    label_radius->hide();
    label_radius2->hide();
    label_height->hide();

    edit_radius->hide();
    edit_radius2->hide();
    edit_height->hide();

    label_numSegX->show();
    label_numSegY->show();
    label_numSegZ->show();

    edit_numSegX->show();
    edit_numSegY->show();
    edit_numSegZ->show();

    setUVTileVisible(true);

    label_numSegX->setText(tr("Seg X"));
    label_numSegY->setText(tr("Seg Y"));
    label_numSegZ->setText(tr("Seg Z"));

}

void PrimitivesWidget::setUiSphere()
{
    edit_type->setText(tr("Sphere"));

    gb_Geometry->show();
    gb_Mesh->show();

    edit_sizeX->hide();
    edit_sizeY->hide();
    edit_sizeZ->hide();

    label_sizeX->hide();
    label_sizeY->hide();
    label_sizeZ->hide();

    label_radius->show();
    label_radius2->hide();
    label_height->hide();

    edit_radius->show();
    edit_radius2->hide();
    edit_height->hide();

    label_radius->setText(tr("Radius"));

    label_numSegX->show();
    label_numSegY->show();
    label_numSegZ->hide();

    edit_numSegX->show();
    edit_numSegY->show();
    edit_numSegZ->hide();

    setUVTileVisible(true);

    label_numSegX->setText(tr("Seg Ring"));
    label_numSegY->setText(tr("Seg Loop"));

}

void PrimitivesWidget::setUiPlan()
{
    edit_type->setText(tr("Plan"));

    gb_Geometry->show();
    gb_Mesh->show();

    edit_sizeX->show();
    edit_sizeY->show();
    edit_sizeZ->hide();

    label_sizeX->show();
    label_sizeY->show();
    label_sizeZ->hide();

    label_radius->hide();
    label_radius2->hide();
    label_height->hide();

    edit_radius->hide();
    edit_radius2->hide();
    edit_height->hide();

    label_numSegX->show();
    label_numSegY->show();
    label_numSegZ->hide();

    edit_numSegX->show();
    edit_numSegY->show();
    edit_numSegZ->hide();

    setUVTileVisible(true);

    label_numSegX->setText(tr("Seg X"));
    label_numSegY->setText(tr("Seg Y"));

}

void PrimitivesWidget::setUiCylinder()
{
    edit_type->setText(tr("Cylinder"));

    gb_Geometry->show();
    gb_Mesh->show();

    edit_sizeX->hide();
    edit_sizeY->hide();
    edit_sizeZ->hide();

    label_sizeX->hide();
    label_sizeY->hide();
    label_sizeZ->hide();

    label_radius->show();
    label_radius2->hide();
    label_height->show();

    edit_radius->show();
    edit_radius2->hide();
    edit_height->show();

    label_radius->setText(tr("Radius"));

    label_numSegX->show();
    label_numSegY->hide();
    label_numSegZ->show();

    edit_numSegX->show();
    edit_numSegY->hide();
    edit_numSegZ->show();

    setUVTileVisible(true);

    label_numSegX->setText(tr("Seg Base"));
    label_numSegZ->setText(tr("Seg Height"));
}

void PrimitivesWidget::setUiCone()
{
    edit_type->setText(tr("Cone"));

    gb_Geometry->show();
    gb_Mesh->show();

    edit_sizeX->hide();
    edit_sizeY->hide();
    edit_sizeZ->hide();

    label_sizeX->hide();
    label_sizeY->hide();
    label_sizeZ->hide();

    label_radius->show();
    label_radius2->hide();
    label_height->show();

    edit_radius->show();
    edit_radius2->hide();
    edit_height->show();

    label_radius->setText(tr("Radius"));

    label_numSegX->show();
    label_numSegY->hide();
    label_numSegZ->show();

    edit_numSegX->show();
    edit_numSegY->hide();
    edit_numSegZ->show();

    setUVTileVisible(true);

    label_numSegX->setText(tr("Seg Base"));
    label_numSegZ->setText(tr("Seg Height"));
}

void PrimitivesWidget::setUiTorus()
{
    edit_type->setText(tr("Torus"));

    gb_Geometry->show();
    gb_Mesh->show();

    edit_sizeX->hide();
    edit_sizeY->hide();
    edit_sizeZ->hide();

    label_sizeX->hide();
    label_sizeY->hide();
    label_sizeZ->hide();

    label_radius->show();
    label_radius2->show();
    label_height->hide();

    edit_radius->show();
    edit_radius2->show();
    edit_height->hide();

    label_radius->setText(tr("Radius"));
    label_radius2->setText(tr("Section Radius"));

    label_numSegX->show();
    label_numSegY->show();
    label_numSegZ->hide();

    edit_numSegX->show();
    edit_numSegY->show();
    edit_numSegZ->hide();

    setUVTileVisible(true);

    label_numSegX->setText(tr("Seg Circle"));
    label_numSegY->setText(tr("Seg Section"));
}

void PrimitivesWidget::setUiTube()
{
    edit_type->setText(tr("Tube"));

    gb_Geometry->show();
    gb_Mesh->show();

    edit_sizeX->hide();
    edit_sizeY->hide();
    edit_sizeZ->hide();

    label_sizeX->hide();
    label_sizeY->hide();
    label_sizeZ->hide();

    label_radius->show();
    label_radius2->show();
    label_height->show();

    edit_radius->show();
    edit_radius2->show();
    edit_height->show();

    label_radius->setText(tr("Outer Radius"));
    label_radius2->setText(tr("Inner Radius"));

    label_numSegX->show();
    label_numSegY->hide();
    label_numSegZ->show();

    edit_numSegX->show();
    edit_numSegY->hide();
    edit_numSegZ->show();

    setUVTileVisible(true);

    label_numSegX->setText(tr("Seg Base"));
    label_numSegZ->setText(tr("Seg Height"));
}

void PrimitivesWidget::setUiCapsule()
{
    edit_type->setText(tr("Capsule"));

    gb_Geometry->show();
    gb_Mesh->show();

    edit_sizeX->hide();
    edit_sizeY->hide();
    edit_sizeZ->hide();

    label_sizeX->hide();
    label_sizeY->hide();
    label_sizeZ->hide();

    label_radius->show();
    label_radius2->hide();
    label_height->show();

    edit_radius->show();
    edit_radius2->hide();
    edit_height->show();

    label_radius->setText(tr("Radius"));

    label_numSegX->show();
    label_numSegY->show();
    label_numSegZ->show();

    edit_numSegX->show();
    edit_numSegY->show();
    edit_numSegZ->show();

    setUVTileVisible(true);

    label_numSegX->setText(tr("Seg Ring"));
    label_numSegY->setText(tr("Seg Loop"));
    label_numSegZ->setText(tr("Seg Height"));
}

void PrimitivesWidget::setUiIcoSphere()
{
    edit_type->setText(tr("IcoSphere"));

    gb_Geometry->show();
    gb_Mesh->show();

    edit_sizeX->hide();
    edit_sizeY->hide();
    edit_sizeZ->hide();

    label_sizeX->hide();
    label_sizeY->hide();
    label_sizeZ->hide();

    label_radius->show();
    label_radius2->hide();
    label_height->hide();

    edit_radius->show();
    edit_radius2->hide();
    edit_height->hide();

    label_radius->setText(tr("Radius"));

    label_numSegX->show();
    label_numSegY->hide();
    label_numSegZ->hide();

    edit_numSegX->show();
    edit_numSegY->hide();
    edit_numSegZ->hide();

    setUVTileVisible(true);

    label_numSegX->setText(tr("Iterations"));
}

void PrimitivesWidget::setUiRoundedBox()
{
    edit_type->setText(tr("Rounded Box"));

    gb_Geometry->show();
    gb_Mesh->show();

    edit_sizeX->show();
    edit_sizeY->show();
    edit_sizeZ->show();

    label_sizeX->show();
    label_sizeY->show();
    label_sizeZ->show();

    label_radius->show();
    label_radius2->hide();
    label_height->hide();

    edit_radius->show();
    edit_radius2->hide();
    edit_height->hide();

    label_radius->setText(tr("Chamfer"));

    label_numSegX->show();
    label_numSegY->show();
    label_numSegZ->show();

    edit_numSegX->show();
    edit_numSegY->show();
    edit_numSegZ->show();

    setUVTileVisible(true);

    label_numSegX->setText(tr("Seg X"));
    label_numSegY->setText(tr("Seg Y"));
    label_numSegZ->setText(tr("Seg Z"));

}

void PrimitivesWidget::updateUiFromParams()
{
    blockEditSignals(true);

    if(mSelectedPrimitive.count() == 1)
    {
        PrimitiveObject* primitive = mSelectedPrimitive.at(0);

        edit_sizeX->setSpecialValueText(QString::null);
        edit_sizeY->setSpecialValueText(QString::null);
        edit_sizeZ->setSpecialValueText(QString::null);

        edit_radius->setSpecialValueText(QString::null);
        edit_radius2->setSpecialValueText(QString::null);
        edit_height->setSpecialValueText(QString::null);

        edit_numSegX->setSpecialValueText(QString::null);
        edit_numSegY->setSpecialValueText(QString::null);
        edit_numSegZ->setSpecialValueText(QString::null);

        edit_UTile->setSpecialValueText(QString::null);
        edit_VTile->setSpecialValueText(QString::null);

        edit_sizeX->setValue(primitive->getSizeX());
        edit_sizeY->setValue(primitive->getSizeY());
        edit_sizeZ->setValue(primitive->getSizeZ());

        edit_radius->setValue(primitive->getRadius());
        edit_radius2->setValue(primitive->getInnerRadius());
        edit_height->setValue(primitive->getHeight());

        edit_numSegX->setValue(primitive->getNumSegX());
        edit_numSegY->setValue(primitive->getNumSegY());
        edit_numSegZ->setValue(primitive->getNumSegZ());

        edit_UTile->setValue(primitive->getUTile());
        edit_VTile->setValue(primitive->getVTile());
        pb_switchUV->setChecked(primitive->hasUVSwitched());
    }
    else
    {
        edit_sizeX->setSpecialValueText(tr("-"));
        edit_sizeY->setSpecialValueText(tr("-"));
        edit_sizeZ->setSpecialValueText(tr("-"));

        edit_radius->setSpecialValueText(tr("-"));
        edit_radius2->setSpecialValueText(tr("-"));
        edit_height->setSpecialValueText(tr("-"));

        edit_numSegX->setSpecialValueText(tr("-"));
        edit_numSegY->setSpecialValueText(tr("-"));
        edit_numSegZ->setSpecialValueText(tr("-"));

        edit_UTile->setSpecialValueText(tr("-"));
        edit_VTile->setSpecialValueText(tr("-"));

        edit_sizeX->setValue(edit_sizeX->minimum());
        edit_sizeY->setValue(edit_sizeY->minimum());
        edit_sizeZ->setValue(edit_sizeZ->minimum());

        edit_radius->setValue(edit_radius->minimum());
        edit_radius2->setValue(edit_radius2->minimum());
        edit_height->setValue(edit_height->minimum());

        edit_numSegX->setValue(edit_numSegX->minimum());
        edit_numSegY->setValue(edit_numSegY->minimum());
        edit_numSegZ->setValue(edit_numSegZ->minimum());

        edit_UTile->setValue(edit_UTile->minimum());
        edit_VTile->setValue(edit_VTile->minimum());
        pb_switchUV->setChecked(false);
    }



    blockEditSignals(false);
}
void PrimitivesWidget::blockEditSignals(bool block)
{
    edit_sizeX->blockSignals(block);
    edit_sizeY->blockSignals(block);
    edit_sizeZ->blockSignals(block);

    edit_radius->blockSignals(block);
    edit_radius2->blockSignals(block);
    edit_height->blockSignals(block);

    edit_numSegX->blockSignals(block);
    edit_numSegY->blockSignals(block);
    edit_numSegZ->blockSignals(block);

    edit_UTile->blockSignals(block);
    edit_VTile->blockSignals(block);
    pb_switchUV->blockSignals(block);
}

PrimitiveObject::PrimitiveType PrimitivesWidget::getSelectedPrimitive()
{
    mSelectedPrimitive.clear();
    QList<Ogre::SceneNode*>::const_iterator nodeIter = SelectionSet::getSingleton()->getNodesSelectionList().begin();
    while ((nodeIter != SelectionSet::getSingleton()->getNodesSelectionList().end())
            &&(!PrimitiveObject::isPrimitive(*nodeIter)))
    {
           ++nodeIter;
    }

    if((nodeIter) == SelectionSet::getSingleton()->getNodesSelectionList().end())
        return PrimitiveObject::AP_NONE; //No Primitive at all

    //count = 1;
    mSelectedPrimitive.append(PrimitiveObject::getPrimitiveFromSceneNode(*nodeIter));
    PrimitiveObject::PrimitiveType firstType = mSelectedPrimitive.at(0)->getType();
    PrimitiveObject::PrimitiveType currentType = PrimitiveObject::AP_NONE;

    ++nodeIter;
    while (nodeIter != SelectionSet::getSingleton()->getNodesSelectionList().end())
    {
        if(PrimitiveObject::isPrimitive(*nodeIter))
        {
            currentType = PrimitiveObject::getPrimitiveFromSceneNode(*nodeIter)->getType();
            if(currentType != firstType)
                return PrimitiveObject::AP_NONE;
            else
                mSelectedPrimitive.append(PrimitiveObject::getPrimitiveFromSceneNode(*nodeIter));
        }
        ++nodeIter;
    }

    return firstType;
}


void PrimitivesWidget::onSelectionChanged()
{
    if(SelectionSet::getSingleton()->hasNodes())
    {
        switch (getSelectedPrimitive())
        {
            case PrimitiveObject::AP_NONE:
                    setUiEmpty();
                break;
            case PrimitiveObject::AP_CUBE:
                        updateUiFromParams(); setUiCube();
                    break;
            case PrimitiveObject::AP_SPHERE:
                        updateUiFromParams();setUiSphere();
                    break;
            case PrimitiveObject::AP_PLANE:
                        updateUiFromParams();setUiPlan();
                    break;
            case PrimitiveObject::AP_CYLINDER:
                        updateUiFromParams();setUiCylinder();
                    break;
            case PrimitiveObject::AP_CONE:
                        updateUiFromParams();setUiCone();
                    break;
            case PrimitiveObject::AP_TORUS:
                        updateUiFromParams();setUiTorus();
                    break;
            case PrimitiveObject::AP_TUBE:
                        updateUiFromParams();setUiTube();
                    break;
            case PrimitiveObject::AP_CAPSULE:
                        updateUiFromParams();setUiCapsule();
                    break;
            case PrimitiveObject::AP_ICOSPHERE:
                        updateUiFromParams();setUiIcoSphere();
                    break;
            case PrimitiveObject::AP_ROUNDEDBOX:
                        updateUiFromParams();setUiRoundedBox();
                    break;
            default:
                        setUiMesh();
            break;

        }//switch
    }//if selection has node
    else
        setUiEmpty();
}

////////////////////////
///
///
void PrimitivesWidget::onEditSizeX()
{
    if(mSelectedPrimitive.count()==0)
        return;

    foreach(PrimitiveObject* primitive, mSelectedPrimitive)
        primitive->setSizeX(edit_sizeX->value());

}

void PrimitivesWidget::onEditSizeY()
{
    if(mSelectedPrimitive.count()==0)
        return;

    foreach(PrimitiveObject* primitive, mSelectedPrimitive)
        primitive->setSizeY(edit_sizeY->value());

}

void PrimitivesWidget::onEditSizeZ()
{
    if(mSelectedPrimitive.count()==0)
        return;

    foreach(PrimitiveObject* primitive, mSelectedPrimitive)
        primitive->setSizeZ(edit_sizeZ->value());

}

void PrimitivesWidget::onEditRadius()
{
    if(mSelectedPrimitive.count()==0)
        return;

    foreach(PrimitiveObject* primitive, mSelectedPrimitive)
        primitive->setRadius(edit_radius->value());

}

void PrimitivesWidget::onEditRadius2()
{
    if(mSelectedPrimitive.count()==0)
        return;

    foreach(PrimitiveObject* primitive, mSelectedPrimitive)
        primitive->setInnerRadius(edit_radius2->value());

}

void PrimitivesWidget::onEditHeight()
{
    if(mSelectedPrimitive.count()==0)
        return;

    foreach(PrimitiveObject* primitive, mSelectedPrimitive)
        primitive->setHeight(edit_height->value());

}

void PrimitivesWidget::onEditNumSegX()
{
    if(mSelectedPrimitive.count()==0)
        return;

    foreach(PrimitiveObject* primitive, mSelectedPrimitive)
        primitive->setNumSegX(edit_numSegX->value());

}

void PrimitivesWidget::onEditNumSegY()
{
    if(mSelectedPrimitive.count()==0)
        return;

    foreach(PrimitiveObject* primitive, mSelectedPrimitive)
        primitive->setNumSegY(edit_numSegY->value());

}

void PrimitivesWidget::onEditNumSegZ()
{
    if(mSelectedPrimitive.count()==0)
        return;

    foreach(PrimitiveObject* primitive, mSelectedPrimitive)
        primitive->setNumSegZ(edit_numSegZ->value());

}

void PrimitivesWidget::onEditUTile()
{
    if(mSelectedPrimitive.count()==0)
        return;

    foreach(PrimitiveObject* primitive, mSelectedPrimitive)
        primitive->setUTile(edit_UTile->value());

}

void PrimitivesWidget::onEditVTile()
{
    if(mSelectedPrimitive.count()==0)
        return;

    foreach(PrimitiveObject* primitive, mSelectedPrimitive)
        primitive->setVTile(edit_VTile->value());

}

void PrimitivesWidget::onToggleSwitchUV()
{
    if(mSelectedPrimitive.count()==0)
        return;

    foreach(PrimitiveObject* primitive, mSelectedPrimitive)
        primitive->setUVSwitch(pb_switchUV->isChecked());

}
////////////////////////
///
///

void PrimitivesWidget::createPrimitive(PrimitiveObject::PrimitiveType newPrimitive, const QString& name)
{
   PrimitiveObject* primitive = new PrimitiveObject(name, newPrimitive);
   primitive->createPrimitive();
}
void PrimitivesWidget::createCube()
{
    bool ok;
    QString name = QInputDialog::getText(Manager::getSingleton()->getMainWindow(), tr("New Cube"),
                                         tr("Cube name:"), QLineEdit::Normal,
                                         "", &ok);
    name=(ok&&name.size())?name:tr("Cube");

    if(!ok)
        return;

    PrimitiveObject::createCube(name);
    //createPrimitive(PrimitiveObject::AP_CUBE, name);
}

void PrimitivesWidget::createSphere()
{
    bool ok;
    QString name = QInputDialog::getText(Manager::getSingleton()->getMainWindow(), tr("New Sphere"),
                                         tr("Sphere name:"), QLineEdit::Normal,
                                         "", &ok);
    name=(ok&&name.size())?name:tr("Sphere");

    if(!ok)
        return;

    PrimitiveObject::createSphere(name);
    //createPrimitive(PrimitiveObject::AP_SPHERE, name);
}

void PrimitivesWidget::createPlane()
{
    bool ok;
    QString name = QInputDialog::getText(Manager::getSingleton()->getMainWindow(), tr("New Plane"),
                                         tr("Plane name:"), QLineEdit::Normal,
                                         "", &ok);
    name=(ok&&name.size())?name:tr("Plane");

    if(!ok)
        return;

    PrimitiveObject::createPlane(name);
    //createPrimitive(PrimitiveObject::AP_PLAN, name);
}

void PrimitivesWidget::createCylinder()
{
    bool ok;
    QString name = QInputDialog::getText(Manager::getSingleton()->getMainWindow(), tr("New Cylinder"),
                                         tr("Cylinder name:"), QLineEdit::Normal,
                                         "", &ok);
    name=(ok&&name.size())?name:tr("Cylinder");

    if(!ok)
        return;

    PrimitiveObject::createCylinder(name);
    //createPrimitive(PrimitiveObject::AP_CYLINDER, name);
}
void PrimitivesWidget::createCone()
{
    bool ok;
    QString name = QInputDialog::getText(Manager::getSingleton()->getMainWindow(), tr("New Cone"),
                                         tr("Cone name:"), QLineEdit::Normal,
                                         "", &ok);
    name=(ok&&name.size())?name:tr("Cone");

    if(!ok)
        return;

    PrimitiveObject::createCone(name);
    //createPrimitive(PrimitiveObject::AP_CONE, name);
}
void PrimitivesWidget::createTorus()
{
    bool ok;
    QString name = QInputDialog::getText(Manager::getSingleton()->getMainWindow(), tr("New Torus"),
                                         tr("Torus name:"), QLineEdit::Normal,
                                         "", &ok);
    name=(ok&&name.size())?name:tr("Torus");

    if(!ok)
        return;

    PrimitiveObject::createTorus(name);
    //createPrimitive(PrimitiveObject::AP_TORUS, name);
}
void PrimitivesWidget::createTube()
{
    bool ok;
    QString name = QInputDialog::getText(Manager::getSingleton()->getMainWindow(), tr("New Tube"),
                                         tr("Tube name:"), QLineEdit::Normal,
                                         "", &ok);
    name=(ok&&name.size())?name:tr("Tube");

    if(!ok)
        return;

    PrimitiveObject::createTube(name);
    //createPrimitive(PrimitiveObject::AP_TUBE, name);
}
void PrimitivesWidget::createCapsule()
{
    bool ok;
    QString name = QInputDialog::getText(Manager::getSingleton()->getMainWindow(), tr("New Capsule"),
                                         tr("Capsule name:"), QLineEdit::Normal,
                                         "", &ok);
    name=(ok&&name.size())?name:tr("Capsule");

    if(!ok)
        return;

    PrimitiveObject::createCapsule(name);
    //createPrimitive(PrimitiveObject::AP_CAPSULE, name);
}
void PrimitivesWidget::createIcoSphere()
{
    bool ok;
    QString name = QInputDialog::getText(Manager::getSingleton()->getMainWindow(), tr("New IcoSphere"),
                                         tr("IcoSphere name:"), QLineEdit::Normal,
                                         "", &ok);
    name=(ok&&name.size())?name:tr("IcoSphere");

    if(!ok)
        return;

    PrimitiveObject::createIcoSphere(name);
    //createPrimitive(PrimitiveObject::AP_ICOSPHERE, name);
}
void PrimitivesWidget::createRoundedBox()
{
    bool ok;
    QString name = QInputDialog::getText(Manager::getSingleton()->getMainWindow(), tr("New RoundedBox"),
                                         tr("RoundedBox name:"), QLineEdit::Normal,
                                         "", &ok);
    name=(ok&&name.size())?name:tr("RoundedBox");

    if(!ok)
        return;

    PrimitiveObject::createRoundedBox(name);
    //createPrimitive(PrimitiveObject::AP_ROUNDEDBOX, name);
}


