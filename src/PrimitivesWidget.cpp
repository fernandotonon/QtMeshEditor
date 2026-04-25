#include <QInputDialog>
#include <QHBoxLayout>
#include <QLabel>

#include <array>

#include <OgreAny.h>
#include <OgreUserObjectBindings.h>

#include "SelectionSet.h"
#include "Manager.h"
#include "PrimitivesWidget.h"
#include "SentryReporter.h"

#include "mainwindow.h"

// TODO play with numTextureCoord (require to change the UI)

// TODO fix the bug in the num of segment for rounded box

// TODO switchUV doesn't seems to work

namespace {

template <typename Setter>
void applyToSelectedPrimitives(const QList<PrimitiveObject*>& selected, Setter&& setter)
{
    for (PrimitiveObject* primitive : selected) {
        setter(primitive);
    }
}

template <typename Value, typename Setter>
void applyPrimitiveValue(const QList<PrimitiveObject*>& selected, const Value& value, Setter setter)
{
    applyToSelectedPrimitives(selected, [value, setter](PrimitiveObject* primitive) {
        (primitive->*setter)(value);
    });
}

template <typename Widget, typename Setter>
void applyWidgetValue(const QList<PrimitiveObject*>& selected, const Widget* widget, Setter setter)
{
    applyPrimitiveValue(selected, widget->value(), setter);
}

template <typename Creator>
void promptAndCreatePrimitive(const QString& dialogTitle,
                              const QString& fieldLabel,
                              const QString& defaultName,
                              const QString& breadcrumbName,
                              Creator&& creator)
{
    bool ok = false;
    QString name = QInputDialog::getText(
        Manager::getSingleton()->getMainWindow(),
        dialogTitle,
        fieldLabel,
        QLineEdit::Normal,
        "",
        &ok);
    name = (ok && !name.isEmpty()) ? name : defaultName;

    if (!ok) {
        return;
    }

    SentryReporter::addBreadcrumb("ui.action", QString("Create primitive: %1").arg(breadcrumbName));
    creator(name);
}

} // namespace

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

void PrimitivesWidget::setUiEmpty()
{
    edit_type->setText("");

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

void PrimitivesWidget::applyPrimitiveUiConfig(const QString &type,
                                              bool sizeX, bool sizeY, bool sizeZ,
                                              bool radius, bool radius2, bool height,
                                              const QString &radiusText, const QString &radius2Text,
                                              bool segX, bool segY, bool segZ,
                                              const QString &segXText, const QString &segYText, const QString &segZText,
                                              bool uvTileVisible, bool switchUvVisible)
{
    edit_type->setText(type);

    gb_Geometry->show();
    gb_Mesh->show();

    edit_sizeX->setVisible(sizeX);
    edit_sizeY->setVisible(sizeY);
    edit_sizeZ->setVisible(sizeZ);

    label_sizeX->setVisible(sizeX);
    label_sizeY->setVisible(sizeY);
    label_sizeZ->setVisible(sizeZ);

    label_radius->setVisible(radius);
    label_radius2->setVisible(radius2);
    label_height->setVisible(height);

    edit_radius->setVisible(radius);
    edit_radius2->setVisible(radius2);
    edit_height->setVisible(height);

    if(!radiusText.isEmpty())  label_radius->setText(radiusText);
    if(!radius2Text.isEmpty()) label_radius2->setText(radius2Text);

    label_numSegX->setVisible(segX);
    label_numSegY->setVisible(segY);
    label_numSegZ->setVisible(segZ);

    edit_numSegX->setVisible(segX);
    edit_numSegY->setVisible(segY);
    edit_numSegZ->setVisible(segZ);

    setUVTileVisible(uvTileVisible);
    pb_switchUV->setVisible(switchUvVisible && uvTileVisible);

    if(!segXText.isEmpty()) label_numSegX->setText(segXText);
    if(!segYText.isEmpty()) label_numSegY->setText(segYText);
    if(!segZText.isEmpty()) label_numSegZ->setText(segZText);
}

void PrimitivesWidget::setUiCube()
{
    applyPrimitiveUiConfig(tr("Cube"),
                           true, true, true,
                           false, false, false,
                           QString(), QString(),
                           true, true, true,
                           tr("Seg X"), tr("Seg Y"), tr("Seg Z"),
                           true, false);
}

void PrimitivesWidget::setUiSphere()
{
    applyPrimitiveUiConfig(tr("Sphere"),
                           false, false, false,
                           true, false, false,
                           tr("Radius"), QString(),
                           true, true, false,
                           tr("Seg Ring"), tr("Seg Loop"), QString(),
                           true);
}

void PrimitivesWidget::setUiPlane()
{
    applyPrimitiveUiConfig(tr("Plane"),
                           true, true, false,
                           false, false, false,
                           QString(), QString(),
                           true, true, false,
                           tr("Seg X"), tr("Seg Y"), QString(),
                           true);
}

void PrimitivesWidget::setUiCylinder()
{
    applyPrimitiveUiConfig(tr("Cylinder"),
                           false, false, false,
                           true, false, true,
                           tr("Radius"), QString(),
                           true, false, true,
                           tr("Seg Base"), QString(), tr("Seg Height"),
                           true);
}

void PrimitivesWidget::setUiCone()
{
    applyPrimitiveUiConfig(tr("Cone"),
                           false, false, false,
                           true, false, true,
                           tr("Radius"), QString(),
                           true, false, true,
                           tr("Seg Base"), QString(), tr("Seg Height"),
                           true);
}

void PrimitivesWidget::setUiTorus()
{
    applyPrimitiveUiConfig(tr("Torus"),
                           false, false, false,
                           true, true, false,
                           tr("Radius"), tr("Section Radius"),
                           true, true, false,
                           tr("Seg Circle"), tr("Seg Section"), QString(),
                           true);
}

void PrimitivesWidget::setUiTube()
{
    applyPrimitiveUiConfig(tr("Tube"),
                           false, false, false,
                           true, true, true,
                           tr("Outer Radius"), tr("Inner Radius"),
                           true, false, true,
                           tr("Seg Base"), QString(), tr("Seg Height"),
                           true);
}

void PrimitivesWidget::setUiCapsule()
{
    applyPrimitiveUiConfig(tr("Capsule"),
                           false, false, false,
                           true, false, true,
                           tr("Radius"), QString(),
                           true, true, true,
                           tr("Seg Ring"), tr("Seg Loop"), tr("Seg Height"),
                           true);
}

void PrimitivesWidget::setUiIcoSphere()
{
    applyPrimitiveUiConfig(tr("IcoSphere"),
                           false, false, false,
                           true, false, false,
                           tr("Radius"), QString(),
                           true, false, false,
                           tr("Iterations"), QString(), QString(),
                           true);
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

void PrimitivesWidget::setUiSpring()
{
    edit_type->setText(tr("Spring"));

    gb_Mesh->show();

    label_numSegX->show();
    label_numSegY->show();

    edit_numSegX->show();
    edit_numSegY->show();

    label_numSegX->setText(tr("Circle Segments"));
    label_numSegY->setText(tr("Path Segments"));
}

void PrimitivesWidget::updateUiFromParams()
{
    blockEditSignals(true);

    if(mSelectedPrimitive.count() == 1)
    {
        PrimitiveObject* primitive = mSelectedPrimitive.at(0);

        edit_sizeX->setSpecialValueText(QString());
        edit_sizeY->setSpecialValueText(QString());
        edit_sizeZ->setSpecialValueText(QString());

        edit_radius->setSpecialValueText(QString());
        edit_radius2->setSpecialValueText(QString());
        edit_height->setSpecialValueText(QString());

        edit_numSegX->setSpecialValueText(QString());
        edit_numSegY->setSpecialValueText(QString());
        edit_numSegZ->setSpecialValueText(QString());

        edit_UTile->setSpecialValueText(QString());
        edit_VTile->setSpecialValueText(QString());

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
        edit_sizeX->setSpecialValueText("-");
        edit_sizeY->setSpecialValueText("-");
        edit_sizeZ->setSpecialValueText("-");

        edit_radius->setSpecialValueText("-");
        edit_radius2->setSpecialValueText("-");
        edit_height->setSpecialValueText("-");

        edit_numSegX->setSpecialValueText("-");
        edit_numSegY->setSpecialValueText("-");
        edit_numSegZ->setSpecialValueText("-");

        edit_UTile->setSpecialValueText("-");
        edit_VTile->setSpecialValueText("-");

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

const QList<PrimitiveObject *> &PrimitivesWidget::getSelectedPrimitiveList()
{
    return mSelectedPrimitive;
}

void PrimitivesWidget::blockEditSignals(bool block)
{
    const std::array<QObject*, 12> editors = {
        edit_sizeX, edit_sizeY, edit_sizeZ,
        edit_radius, edit_radius2, edit_height,
        edit_numSegX, edit_numSegY, edit_numSegZ,
        edit_UTile, edit_VTile, pb_switchUV
    };

    for (QObject* editor : editors) {
        editor->blockSignals(block);
    }
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
    if (!SelectionSet::getSingleton()->hasNodes()) {
        mSelectedPrimitive.clear();
        setUiEmpty();
        return;
    }

    const PrimitiveObject::PrimitiveType selectedPrimitive = getSelectedPrimitive();
    if (selectedPrimitive == PrimitiveObject::AP_NONE) {
        mSelectedPrimitive.clear();
        setUiEmpty();
        return;
    }

    void (PrimitivesWidget::*setUiForPrimitive)() = nullptr;
    switch (selectedPrimitive) {
    case PrimitiveObject::AP_CUBE:
        setUiForPrimitive = &PrimitivesWidget::setUiCube;
        break;
    case PrimitiveObject::AP_SPHERE:
        setUiForPrimitive = &PrimitivesWidget::setUiSphere;
        break;
    case PrimitiveObject::AP_PLANE:
        setUiForPrimitive = &PrimitivesWidget::setUiPlane;
        break;
    case PrimitiveObject::AP_CYLINDER:
        setUiForPrimitive = &PrimitivesWidget::setUiCylinder;
        break;
    case PrimitiveObject::AP_CONE:
        setUiForPrimitive = &PrimitivesWidget::setUiCone;
        break;
    case PrimitiveObject::AP_TORUS:
        setUiForPrimitive = &PrimitivesWidget::setUiTorus;
        break;
    case PrimitiveObject::AP_TUBE:
        setUiForPrimitive = &PrimitivesWidget::setUiTube;
        break;
    case PrimitiveObject::AP_CAPSULE:
        setUiForPrimitive = &PrimitivesWidget::setUiCapsule;
        break;
    case PrimitiveObject::AP_ICOSPHERE:
        setUiForPrimitive = &PrimitivesWidget::setUiIcoSphere;
        break;
    case PrimitiveObject::AP_ROUNDEDBOX:
        setUiForPrimitive = &PrimitivesWidget::setUiRoundedBox;
        break;
    case PrimitiveObject::AP_SPRING:
        setUiForPrimitive = &PrimitivesWidget::setUiSpring;
        break;
    default:
        setUiMesh();
        return;
    }

    setUiEmpty();
    (this->*setUiForPrimitive)();
    updateUiFromParams();
}

void PrimitivesWidget::onEditSizeX()
{
    applyWidgetValue(mSelectedPrimitive, edit_sizeX, &PrimitiveObject::setSizeX);
}

void PrimitivesWidget::onEditSizeY()
{
    applyWidgetValue(mSelectedPrimitive, edit_sizeY, &PrimitiveObject::setSizeY);
}

void PrimitivesWidget::onEditSizeZ()
{
    applyWidgetValue(mSelectedPrimitive, edit_sizeZ, &PrimitiveObject::setSizeZ);
}

void PrimitivesWidget::onEditRadius()
{
    applyWidgetValue(mSelectedPrimitive, edit_radius, &PrimitiveObject::setRadius);
}

void PrimitivesWidget::onEditRadius2()
{
    applyWidgetValue(mSelectedPrimitive, edit_radius2, &PrimitiveObject::setInnerRadius);
}

void PrimitivesWidget::onEditHeight()
{
    applyWidgetValue(mSelectedPrimitive, edit_height, &PrimitiveObject::setHeight);
}

void PrimitivesWidget::onEditNumSegX()
{
    applyWidgetValue(mSelectedPrimitive, edit_numSegX, &PrimitiveObject::setNumSegX);
}

void PrimitivesWidget::onEditNumSegY()
{
    applyWidgetValue(mSelectedPrimitive, edit_numSegY, &PrimitiveObject::setNumSegY);
}

void PrimitivesWidget::onEditNumSegZ()
{
    applyWidgetValue(mSelectedPrimitive, edit_numSegZ, &PrimitiveObject::setNumSegZ);
}

void PrimitivesWidget::onEditUTile()
{
    applyWidgetValue(mSelectedPrimitive, edit_UTile, &PrimitiveObject::setUTile);
}

void PrimitivesWidget::onEditVTile()
{
    applyWidgetValue(mSelectedPrimitive, edit_VTile, &PrimitiveObject::setVTile);
}

void PrimitivesWidget::onToggleSwitchUV()
{
    applyPrimitiveValue(mSelectedPrimitive, pb_switchUV->isChecked(), &PrimitiveObject::setUVSwitch);
}

void PrimitivesWidget::createCube()
{
    promptAndCreatePrimitive(tr("New Cube"), tr("Cube name:"), tr("Cube"), "Cube",
                             [](const QString& name) { PrimitiveObject::createCube(name); });
}

void PrimitivesWidget::createSphere()
{
    promptAndCreatePrimitive(tr("New Sphere"), tr("Sphere name:"), tr("Sphere"), "Sphere",
                             [](const QString& name) { PrimitiveObject::createSphere(name); });
}

void PrimitivesWidget::createPlane()
{
    promptAndCreatePrimitive(tr("New Plane"), tr("Plane name:"), tr("Plane"), "Plane",
                             [](const QString& name) { PrimitiveObject::createPlane(name); });
}

void PrimitivesWidget::createCylinder()
{
    promptAndCreatePrimitive(tr("New Cylinder"), tr("Cylinder name:"), tr("Cylinder"), "Cylinder",
                             [](const QString& name) { PrimitiveObject::createCylinder(name); });
}
void PrimitivesWidget::createCone()
{
    promptAndCreatePrimitive(tr("New Cone"), tr("Cone name:"), tr("Cone"), "Cone",
                             [](const QString& name) { PrimitiveObject::createCone(name); });
}
void PrimitivesWidget::createTorus()
{
    promptAndCreatePrimitive(tr("New Torus"), tr("Torus name:"), tr("Torus"), "Torus",
                             [](const QString& name) { PrimitiveObject::createTorus(name); });
}
void PrimitivesWidget::createTube()
{
    promptAndCreatePrimitive(tr("New Tube"), tr("Tube name:"), tr("Tube"), "Tube",
                             [](const QString& name) { PrimitiveObject::createTube(name); });
}
void PrimitivesWidget::createCapsule()
{
    promptAndCreatePrimitive(tr("New Capsule"), tr("Capsule name:"), tr("Capsule"), "Capsule",
                             [](const QString& name) { PrimitiveObject::createCapsule(name); });
}
void PrimitivesWidget::createIcoSphere()
{
    promptAndCreatePrimitive(tr("New IcoSphere"), tr("IcoSphere name:"), tr("IcoSphere"), "IcoSphere",
                             [](const QString& name) { PrimitiveObject::createIcoSphere(name); });
}
void PrimitivesWidget::createRoundedBox()
{
    promptAndCreatePrimitive(tr("New RoundedBox"), tr("RoundedBox name:"), tr("RoundedBox"), "RoundedBox",
                             [](const QString& name) { PrimitiveObject::createRoundedBox(name); });
}
void PrimitivesWidget::createSpring()
{
    promptAndCreatePrimitive(tr("New Spring"), tr("Spring name:"), tr("Spring"), "Spring",
                             [](const QString& name) { PrimitiveObject::createSpring(name); });
}
