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

#include "materialeditor.h"
#include "ui_materialeditor.h"
#include <stdio.h>
#include <QDebug>
#include <QInputDialog>
#include <QFileDialog>
#include "OgreLog.h"

#include "Manager.h"
#include "MaterialHighlighter.h"

MaterialEditor::MaterialEditor(QWidget *parent) :
    QDialog(parent)
  ,ui(new Ui::MaterialEditor)
  ,ambientColorDialog(new QColorDialog(this))
  ,difuseColorDialog(new QColorDialog(this))
  ,specularColorDialog(new QColorDialog(this))
  ,emissiveColorDialog(new QColorDialog(this))
  ,mSelectedPass(NULL)
  ,mSelectedTechnique(NULL)
  ,mSelectedTextureUnit(NULL)
{
    ui->setupUi(this);

    QObject::connect(ambientColorDialog,SIGNAL(colorSelected(const QColor &)),this,SLOT(on_Ambient_Color_Selected(QColor)));
    QObject::connect(difuseColorDialog,SIGNAL(colorSelected(const QColor &)),this,SLOT(on_Difuse_Color_Selected(QColor)));
    QObject::connect(specularColorDialog,SIGNAL(colorSelected(const QColor &)),this,SLOT(on_Specular_Color_Selected(QColor)));
    QObject::connect(emissiveColorDialog,SIGNAL(colorSelected(const QColor &)),this,SLOT(on_Emissive_Color_Selected(QColor)));

    mMaterialHighlighter = new MaterialHighlighter(ui->textMaterial);
}

MaterialEditor::~MaterialEditor()
{
    delete mMaterialHighlighter;
    delete ui;
}

void MaterialEditor::setMaterialText(const QString &_mat)
{
    Ogre::LogManager::getSingleton().logMessage("void MaterialEditor::setMaterialText(const QString &_mat)");
    //mMaterialHighlighter->highlightBlock(_mat);
    ui->textMaterial->setText(_mat);

    ui->scrollArea->setEnabled(true);
    ui->saveButton->setEnabled(false);
}


void MaterialEditor::setMaterial(const QString &_material)
{
    Ogre::LogManager::getSingleton().logMessage("void MaterialEditor::setMaterial(const QString &_material)");
    if(_material.size()==0)
    {
        setMaterialText("material material_name\n{\n}");
        mMaterialName = "material_name";
    }
    else
    {
        ui->techComboBox->clear();
        ui->techComboBox->addItem("");

        mMaterialName = _material;

        Ogre::MaterialPtr m = Ogre::MaterialManager::getSingleton().getByName(mMaterialName.toStdString().data()).staticCast<Ogre::Material>();

        Ogre::MaterialSerializer ms;
        ms.queueForExport(m,false,false,_material.toStdString().data());

        setMaterialText(ms.getQueuedAsString().data());

        Ogre::Material::TechniqueIterator it = m->getTechniqueIterator();
        int tcount=0;//technique
        int pcount=0;//pass
        QMap <int, Ogre::Pass*> passMap;
        QList <QString> passMapName;
        while (it.hasMoreElements())
        {
            pcount=0;

            Ogre::Technique* tech = it.getNext();
            Ogre::Technique::PassIterator itP = tech->getPassIterator();

            QString techname = tech->getName().size()?tech->getName().data():QString("technique%1").arg(tcount);

            ui->techComboBox->addItem(techname);

            while (itP.hasMoreElements())
            {


                Ogre::Pass* pass = itP.getNext();

                QString passname = pass->getName().size()?pass->getName().data():QString("pass%1").arg(pcount);

                passMap[pcount] = pass;
                passMapName.append(passname);

                ++pcount;
            }
            mTechMap[tcount] = passMap;
            mTechMapName[tcount] = passMapName;
            passMap.clear();
            passMapName.clear();
            ++tcount;
        }

        if(ui->techComboBox->count()>0)
            ui->techComboBox->setCurrentIndex(1);
    }
}

void MaterialEditor::on_buttonEditAmbientColor_clicked()
{
    ambientColorDialog->show();
}

void MaterialEditor::on_buttonEditDifuseColor_clicked()
{
    difuseColorDialog->show();
}

void MaterialEditor::on_buttonEditSpecularColor_clicked()
{
    specularColorDialog->show();
}

void MaterialEditor::on_buttonEditEmissiveColor_clicked()
{
    emissiveColorDialog->show();
}

void MaterialEditor::setTechFields(const QMap<int, Ogre::Pass *> &_techMap, const QList <QString> &_passList)
{
    ui->passComboBox->clear();
    ui->passComboBox->addItem("");
    ui->passComboBox->addItems(_passList);

    mPassMap = _techMap;

    ui->passComboBox->setEnabled(true);
    ui->passNewButton->setEnabled(true);

    if(ui->passComboBox->count()>0)
        ui->passComboBox->setCurrentIndex(1);
}

void MaterialEditor::setPassFields(Ogre::Pass* _pass)
{
    mSelectedPass = _pass;
    ui->radioLightninOn->setEnabled(true);
    ui->radioLightninOff->setEnabled(true);
    ui->srcSceneBlendBox->setEnabled(true);
    ui->dstSceneBlendBox->setEnabled(true);
    ui->radioDepthWriteOn->setEnabled(true);
    ui->radioDepthWriteOff->setEnabled(true);
    ui->radioDepthCheckOn->setEnabled(true);
    ui->radioDepthCheckOff->setEnabled(true);
    ui->buttonEditAmbientColor->setEnabled(true);
    ui->buttonEditDifuseColor->setEnabled(true);
    ui->buttonEditSpecularColor->setEnabled(true);
    ui->buttonEditEmissiveColor->setEnabled(true);
    ui->ComboTextureUnit->setEnabled(true);
    ui->TextureUnitNewButton->setEnabled(true);
    ui->checkBoxUseVertexColorToAmbient->setEnabled(true);
    ui->checkBoxUseVertexColorToDifuse->setEnabled(true);
    ui->checkBoxUseVertexColorToSpecular->setEnabled(true);
    ui->checkBoxUseVertexColorToEmissive->setEnabled(true);
    ui->alphaDifuse->setEnabled(true);
    ui->alphaSpecular->setEnabled(true);
    ui->shineSpecular->setEnabled(true);

    ui->radioLightninOn->setChecked(_pass->getLightingEnabled());
    ui->radioLightninOff->setChecked(!_pass->getLightingEnabled());
    ui->srcSceneBlendBox->setCurrentIndex(_pass->getSourceBlendFactor()+6);
    ui->dstSceneBlendBox->setCurrentIndex(_pass->getDestBlendFactor()+1);
    ui->radioDepthWriteOn->setChecked(_pass->getDepthCheckEnabled());
    ui->radioDepthWriteOff->setChecked(!_pass->getDepthCheckEnabled());
    ui->radioDepthCheckOn->setChecked(_pass->getDepthWriteEnabled());
    ui->radioDepthCheckOff->setChecked(!_pass->getDepthWriteEnabled());
    ui->checkBoxUseVertexColorToAmbient->setChecked(_pass->getVertexColourTracking()&1);
    ui->checkBoxUseVertexColorToDifuse->setChecked(_pass->getVertexColourTracking()&2);
    ui->checkBoxUseVertexColorToSpecular->setChecked(_pass->getVertexColourTracking()&4);
    ui->checkBoxUseVertexColorToEmissive->setChecked(_pass->getVertexColourTracking()&8);
    ui->alphaDifuse->setValue(_pass->getDiffuse().a);
    ui->alphaSpecular->setValue(_pass->getSpecular().a);
    ui->shineSpecular->setValue(_pass->getShininess());

    QColor Color;
    QPalette Pal(palette());

    Color.setRgbF(_pass->getAmbient().r,_pass->getAmbient().g,_pass->getAmbient().b);
    Pal.setColor(QPalette::Background, Color);
    ui->ambientColorWidget->setPalette(Pal);
    ambientColorDialog->setCurrentColor(Color);

    Color.setRgbF(_pass->getDiffuse().r,_pass->getDiffuse().g,_pass->getDiffuse().b);
    Pal.setColor(QPalette::Background, Color);
    ui->difuseColorWidget->setPalette(Pal);
    difuseColorDialog->setCurrentColor(Color);

    Color.setRgbF(_pass->getSpecular().r,_pass->getSpecular().g,_pass->getSpecular().b);
    Pal.setColor(QPalette::Background, Color);
    ui->specularColorWidget->setPalette(Pal);
    specularColorDialog->setCurrentColor(Color);

    Color.setRgbF(_pass->getEmissive().r,_pass->getEmissive().g,_pass->getEmissive().b);
    Pal.setColor(QPalette::Background, Color);
    ui->emissiveColorWidget->setPalette(Pal);
    emissiveColorDialog->setCurrentColor(Color);

    Ogre::Pass::TextureUnitStateIterator itTU = _pass->getTextureUnitStateIterator();
    int tcount=0;
    while (itTU.hasMoreElements())
    {
        ++tcount;

        Ogre::TextureUnitState *textureUnit = itTU.getNext();
        QString TUName = textureUnit->getName().size()?textureUnit->getName().data():QString("Texture_Unit%1").arg(tcount);
        mTexUnitMap[TUName]=textureUnit;
    }
    ui->ComboTextureUnit->clear();
    ui->ComboTextureUnit->addItem("");
    ui->ComboTextureUnit->addItems(mTexUnitMap.keys());

    if(ui->ComboTextureUnit->count()>0)
        ui->ComboTextureUnit->setCurrentIndex(1);
}

void MaterialEditor::updateMaterialText()
{
    Ogre::LogManager::getSingleton().logMessage("void MaterialEditor::updateMaterialText()");

    Ogre::MaterialPtr m = Ogre::MaterialManager::getSingleton().getByName(mMaterialName.toStdString().data()).staticCast<Ogre::Material>();

    Ogre::MaterialSerializer ms;
    ms.queueForExport(m,false,false,mMaterialName.toStdString().data());

    setMaterialText(ms.getQueuedAsString().data());
}

void MaterialEditor::on_techComboBox_currentIndexChanged(int index)
{
    if(index>0)
    {
        Ogre::MaterialPtr m = Ogre::MaterialManager::getSingleton().getByName(mMaterialName.toStdString().data()).staticCast<Ogre::Material>();

        mSelectedTechnique = m.getPointer()->getTechnique(index-1);

        setTechFields(mTechMap[index-1],mTechMapName[index-1]);
    }
    else
    {
        ui->passComboBox->clear();
        ui->passComboBox->setEnabled(false);
        ui->passNewButton->setEnabled(false);
        mSelectedTechnique = NULL;
    }
}

void MaterialEditor::on_techComboBox_currentIndexChanged(const QString &arg1)
{

}


void MaterialEditor::on_passComboBox_currentIndexChanged(int index)
{
    if(index>0)
    {
        setPassFields(mPassMap[index-1]);
    }
    else
    {
        mSelectedPass = NULL;

        ui->radioLightninOn->setEnabled(false);
        ui->radioLightninOff->setEnabled(false);
        ui->srcSceneBlendBox->setEnabled(false);
        ui->dstSceneBlendBox->setEnabled(false);
        ui->radioDepthWriteOn->setEnabled(false);
        ui->radioDepthWriteOff->setEnabled(false);
        ui->radioDepthCheckOn->setEnabled(false);
        ui->radioDepthCheckOff->setEnabled(false);
        ui->buttonEditAmbientColor->setEnabled(false);
        ui->buttonEditDifuseColor->setEnabled(false);
        ui->buttonEditSpecularColor->setEnabled(false);
        ui->buttonEditEmissiveColor->setEnabled(false);
        ui->ComboTextureUnit->setEnabled(false);
        ui->TextureUnitNewButton->setEnabled(false);
        ui->checkBoxUseVertexColorToAmbient->setEnabled(false);
        ui->checkBoxUseVertexColorToDifuse->setEnabled(false);
        ui->checkBoxUseVertexColorToSpecular->setEnabled(false);
        ui->checkBoxUseVertexColorToEmissive->setEnabled(false);
        ui->alphaDifuse->setEnabled(false);
        ui->alphaSpecular->setEnabled(false);
        ui->shineSpecular->setEnabled(false);
        ui->selectTexture->setEnabled(false);
        ui->removeTexture->setEnabled(false);

        ui->radioLightninOn->setChecked(false);
        ui->radioLightninOff->setChecked(false);
        ui->srcSceneBlendBox->setCurrentIndex(0);
        ui->dstSceneBlendBox->setCurrentIndex(0);
        ui->radioDepthWriteOn->setChecked(false);
        ui->radioDepthWriteOff->setChecked(false);
        ui->radioDepthCheckOn->setChecked(false);
        ui->radioDepthCheckOff->setChecked(false);
        ui->checkBoxUseVertexColorToAmbient->setChecked(false);
        ui->checkBoxUseVertexColorToDifuse->setChecked(false);
        ui->checkBoxUseVertexColorToSpecular->setChecked(false);
        ui->checkBoxUseVertexColorToEmissive->setChecked(false);
        ui->ComboTextureUnit->clear();
        ui->alphaDifuse->clear();
        ui->alphaSpecular->clear();
        ui->shineSpecular->clear();
        ui->textureName->setText("*Select a texture*");
    }
}

void MaterialEditor::on_passComboBox_currentIndexChanged(const QString &arg1)
{

}

void MaterialEditor::on_ComboTextureUnit_currentIndexChanged(int index)
{
    if(index>0&&mSelectedPass)
    {
        mSelectedTextureUnit=mSelectedPass->getTextureUnitState(index-1);
        QString TN = mSelectedTextureUnit->getTextureName().data();
        if(TN.size())
        {
            ui->textureName->setText(TN);
        }
        else
        {
            ui->textureName->setText("*Select a texture*");
        }
        ui->selectTexture->setEnabled(true);
        ui->removeTexture->setEnabled(true);
    }
    else
    {
        mSelectedTextureUnit = NULL;
        ui->textureName->setText("*Select a texture*");
        ui->selectTexture->setEnabled(false);
        ui->removeTexture->setEnabled(false);
    }
}

void MaterialEditor::on_Ambient_Color_Selected(const QColor &arg1)
{
    QPalette Pal(palette());
    Pal.setColor(QPalette::Background, arg1);
    ui->ambientColorWidget->setPalette(Pal);
    if(mSelectedPass)
        mSelectedPass->setAmbient(arg1.redF(),arg1.greenF(),arg1.blueF());
    updateMaterialText();
}

void MaterialEditor::on_Difuse_Color_Selected(const QColor &arg1)
{
    QPalette Pal(palette());
    Pal.setColor(QPalette::Background, arg1);
    ui->difuseColorWidget->setPalette(Pal);
    if(mSelectedPass)
        mSelectedPass->setDiffuse(arg1.redF(),arg1.greenF(),arg1.blueF(),ui->alphaDifuse->text().toFloat());
    updateMaterialText();
}

void MaterialEditor::on_Specular_Color_Selected(const QColor &arg1)
{
    QPalette Pal(palette());
    Pal.setColor(QPalette::Background, arg1);
    ui->specularColorWidget->setPalette(Pal);
    if(mSelectedPass)
        mSelectedPass->setSpecular(arg1.redF(),arg1.greenF(),arg1.blueF(),ui->alphaSpecular->text().toFloat());
    updateMaterialText();
}

void MaterialEditor::on_Emissive_Color_Selected(const QColor &arg1)
{
    QPalette Pal(palette());
    Pal.setColor(QPalette::Background, arg1);
    ui->emissiveColorWidget->setPalette(Pal);
    if(mSelectedPass)
        mSelectedPass->setEmissive(arg1.redF(),arg1.greenF(),arg1.blueF());
    updateMaterialText();
}

void MaterialEditor::on_textMaterial_textChanged()
{
    Ogre::LogManager::getSingleton().logMessage("void MaterialEditor::on_textMaterial_textChanged()");
    ui->scrollArea->setEnabled(false);
    ui->saveButton->setEnabled(true);
}

void MaterialEditor::on_saveButton_clicked()
{
    ui->scrollArea->setEnabled(true);
    ui->saveButton->setEnabled(false);

    Ogre::String script = ui->textMaterial->toPlainText().toStdString().data();
    Ogre::MemoryDataStream *memoryStream = new Ogre::MemoryDataStream((void*)script.c_str(), script.length() * sizeof(char));
    Ogre::DataStreamPtr dataStream(memoryStream);

    Ogre::MaterialManager::getSingleton().remove(mMaterialName.toStdString().data());

    Ogre::MaterialSerializer ms;
    ms.parseScript(dataStream,Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    mMaterialName = ui->textMaterial->toPlainText();
    mMaterialName = mMaterialName.remove(0,mMaterialName.indexOf("material")+9);
    mMaterialName.remove(mMaterialName.indexOf("\n"),mMaterialName.size());

    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().getByName(mMaterialName.toStdString().data()).staticCast<Ogre::Material>();

    material->compile();

    Ogre::MaterialManager::getSingleton().reloadAll(true);
    Ogre::MeshManager::getSingleton().reloadAll(true);

    foreach(Ogre::SceneNode * sn, Manager::getSingleton()->getSceneNodes())
    {
        Ogre::LogManager::getSingleton().logMessage(sn->getName());
        Ogre::Entity *e = static_cast<Ogre::Entity *>(sn->getAttachedObject(0));
        e->setMaterialName(e->getSubEntity(0)->getMaterialName());
    }

    setMaterial(mMaterialName);
}

void MaterialEditor::on_radioLightninOn_toggled(bool checked)
{
    if(mSelectedPass)
        mSelectedPass->setLightingEnabled(checked);
    updateMaterialText();
}


void MaterialEditor::on_radioDepthWriteOn_toggled(bool checked)
{
    if(mSelectedPass)
        mSelectedPass->setDepthWriteEnabled(checked);
    updateMaterialText();
}

void MaterialEditor::on_radioDepthCheckOn_toggled(bool checked)
{
    if(mSelectedPass)
        mSelectedPass->setDepthCheckEnabled(checked);
    updateMaterialText();
}

void MaterialEditor::on_srcSceneBlendBox_currentIndexChanged(int index)
{
    if(mSelectedPass)
    {
        if(index<6)
        {
            if(index>0)
            {
                mSelectedPass->setSceneBlending((Ogre::SceneBlendType)--index);
            }
            ui->dstSceneBlendBox->setCurrentIndex(0);
        }
        else
        {
            mSelectedPass->setSceneBlending((Ogre::SceneBlendFactor)(index-6),mSelectedPass->getDestBlendFactor());
        }
    }
    updateMaterialText();
}

void MaterialEditor::on_dstSceneBlendBox_currentIndexChanged(int index)
{
    if(mSelectedPass)
    {
        if(index>0)
        {
            mSelectedPass->setSceneBlending(mSelectedPass->getSourceBlendFactor(),(Ogre::SceneBlendFactor)--index);
        }
    }
    updateMaterialText();
}

void MaterialEditor::on_checkBoxUseVertexColorToAmbient_toggled(bool checked)
{
    if(mSelectedPass)
    {
        mSelectedPass->setVertexColourTracking(
                    checked
                    ?mSelectedPass->getVertexColourTracking()|1
                    :mSelectedPass->getVertexColourTracking()&0xE);//0b1110 not supported by msvsc
        updateMaterialText();
    }
}

void MaterialEditor::on_checkBoxUseVertexColorToDifuse_toggled(bool checked)
{
    if(mSelectedPass)
    {
        mSelectedPass->setVertexColourTracking(
                    checked
                    ?mSelectedPass->getVertexColourTracking()|2
                    :mSelectedPass->getVertexColourTracking()&0xD);//0b1101 not supported by msvsc
        updateMaterialText();
    }
}

void MaterialEditor::on_checkBoxUseVertexColorToSpecular_toggled(bool checked)
{
    if(mSelectedPass)
    {
        mSelectedPass->setVertexColourTracking(
                    checked
                    ?mSelectedPass->getVertexColourTracking()|4
                    :mSelectedPass->getVertexColourTracking()&0xB);//0b1011 not supported by msvsc
        updateMaterialText();
    }
}

void MaterialEditor::on_checkBoxUseVertexColorToEmissive_toggled(bool checked)
{
    if(mSelectedPass)
    {
        mSelectedPass->setVertexColourTracking(
                    checked
                    ?mSelectedPass->getVertexColourTracking()|8
                    :mSelectedPass->getVertexColourTracking()&0x7);//0b0111 not supported by msvsc
        updateMaterialText();
    }
}

void MaterialEditor::on_alphaDifuse_valueChanged(double arg1)
{
    if(mSelectedPass)
    {
        mSelectedPass->setDiffuse(
                    mSelectedPass->getDiffuse().r
                    ,mSelectedPass->getDiffuse().g
                    ,mSelectedPass->getDiffuse().b
                    ,arg1);
        updateMaterialText();
    }
}

void MaterialEditor::on_alphaSpecular_valueChanged(double arg1)
{
    if(mSelectedPass)
    {
        mSelectedPass->setSpecular(
                    mSelectedPass->getSpecular().r
                    ,mSelectedPass->getSpecular().g
                    ,mSelectedPass->getSpecular().b
                    ,arg1);
        updateMaterialText();
    }
}

void MaterialEditor::on_shineSpecular_valueChanged(double arg1)
{
    if(mSelectedPass)
    {
        mSelectedPass->setShininess(arg1);
        updateMaterialText();
    }
}

void MaterialEditor::on_newTechnique_clicked()
{
    bool ok;
    QString text = QInputDialog::getText(this, tr("New Technique"),
                                         tr("Technique name:"), QLineEdit::Normal,
                                         "", &ok);
    if (ok)
    {
#if (OGRE_VERSION < ((1 << 16) | (9 << 8) | 0))
        Ogre::MaterialPtr m = Ogre::MaterialManager::getSingleton().getByName(mMaterialName.toStdString().data());
#else
        Ogre::MaterialPtr m = Ogre::MaterialManager::getSingleton().getByName(mMaterialName.toStdString().data()).staticCast<Ogre::Material>();
#endif
        Ogre::Technique *t = m.getPointer()->createTechnique();
        t->setName(text.toStdString().data());

        setMaterial(mMaterialName);
    }
}

void MaterialEditor::on_passNewButton_clicked()
{
    bool ok;
    QString text = QInputDialog::getText(this, tr("New Pass"),
                                         tr("Pass name:"), QLineEdit::Normal,
                                         "", &ok);
    if (ok && mSelectedTechnique)
    {
        Ogre::Pass *p = mSelectedTechnique->createPass();
        p->setName(text.toStdString().data());

        mPassMap.insert(mPassMap.size(),p);

        ui->passComboBox->addItem(text.toStdString().data());

        updateMaterialText();
    }
}


void MaterialEditor::on_TextureUnitNewButton_clicked()
{
    bool ok;
    QString text = QInputDialog::getText(this, tr("New Texture Unit"),
                                         tr("Texture unit name:"), QLineEdit::Normal,
                                         "", &ok);
    if (ok && mSelectedPass)
    {
        Ogre::TextureUnitState *t = mSelectedPass->createTextureUnitState();
        t->setName(text.toStdString().data());

        ui->ComboTextureUnit->addItem(text.toStdString().data());

        updateMaterialText();
    }
}


void MaterialEditor::on_selectTexture_clicked()
{
    QString filePath = QFileDialog::getOpenFileName(this, tr("Select a texture"),
                                                     "",
                                                     tr("Image File (*.bmp *.jpg *.gif *.raw *.png *.tga *.dds)"));

    if(filePath.size()&&mSelectedTextureUnit)
    {

        QFileInfo file;
        file.setFile(filePath);

        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(file.filePath().toStdString().data(),"FileSystem",file.filePath().toStdString().data(),false);
        Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();

        Ogre::Image i;
        i.load(filePath.toStdString().data(),file.filePath().toStdString().data());
        Ogre::TextureManager::getSingleton().loadImage(file.fileName().toStdString().data(),file.filePath().toStdString().data(),i);

        ui->textureName->setText(file.fileName());
        mSelectedTextureUnit->setTextureName(file.fileName().toStdString().data());
    }
}

void MaterialEditor::on_removeTexture_clicked()
{
    if(mSelectedTextureUnit)
    {
        mSelectedTextureUnit->setTextureName("");
        ui->textureName->setText("*Select a texture*");
    }
}
