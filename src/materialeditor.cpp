#include "materialeditor.h"
#include "ui_materialeditor.h"
#include <stdio.h>
#include <QDebug>
#include <QInputDialog>
#include <QFileDialog>
#include <QMessageBox>
#include "OgreLog.h"

#include "Manager.h"
#include "MaterialHighlighter.h"
#include <OgreScriptCompiler.h>
#include <OgreScriptTranslator.h>

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

    ambientColorDialog->setOption(QColorDialog::DontUseNativeDialog);
    difuseColorDialog->setOption(QColorDialog::DontUseNativeDialog);
    specularColorDialog->setOption(QColorDialog::DontUseNativeDialog);
    emissiveColorDialog->setOption(QColorDialog::DontUseNativeDialog);

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
    ui->textMaterial->setText(_mat);

    ui->scrollArea->setEnabled(true);
    ui->applyButton->setEnabled(false);
}

std::string MaterialEditor::getMaterialText() const
{
    return ui->textMaterial->toPlainText().toStdString();
}

void MaterialEditor::setMaterial(const QString &_material)
{
    if(_material.size()==0)
    {
        setMaterialText("material material_name\n{\n}");
        mMaterialName = "material_name";
        ui->scrollArea->setEnabled(false);
    }
    else
    {
        ui->techComboBox->clear();
        ui->techComboBox->addItem("");

        mMaterialName = _material;

        Ogre::MaterialPtr m = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().getByName(mMaterialName.toStdString().data()));

        Ogre::MaterialSerializer ms;
        ms.queueForExport(m,false,false,_material.toStdString().data());

        setMaterialText(ms.getQueuedAsString().data());

        auto techniques = m->getTechniques();
        int tcount=0;//technique
        int pcount=0;//pass
        QMap <int, Ogre::Pass*> passMap;
        QList <QString> passMapName;
        for (Ogre::Technique* tech : techniques)
        {
            pcount=0;

            QString techname = tech->getName().size()?tech->getName().data():QString("technique%1").arg(tcount);

            ui->techComboBox->addItem(techname);

            auto passes = tech->getPasses();
            for(Ogre::Pass* pass : passes)
            {
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

std::string MaterialEditor::getMaterialName() const
{
    return mMaterialName.toStdString();
}

bool MaterialEditor::isScrollAreaEnabled() const
{
    return ui->scrollArea->isEnabled();
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
    ui->scrollArea->setEnabled(true);

    ui->checkBoxLightning->setChecked(_pass->getLightingEnabled());
    ui->srcSceneBlendBox->setCurrentIndex(_pass->getSourceBlendFactor()+6);
    ui->dstSceneBlendBox->setCurrentIndex(_pass->getDestBlendFactor()+1);
    ui->checkBoxDepthWrite->setChecked(_pass->getDepthWriteEnabled());
    ui->checkBoxDepthCheck->setChecked(_pass->getDepthCheckEnabled());
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
    Pal.setColor(QPalette::Window, Color);
    ui->ambientColorWidget->setPalette(Pal);
    ambientColorDialog->setCurrentColor(Color);

    Color.setRgbF(_pass->getDiffuse().r,_pass->getDiffuse().g,_pass->getDiffuse().b);
    Pal.setColor(QPalette::Window, Color);
    ui->difuseColorWidget->setPalette(Pal);
    difuseColorDialog->setCurrentColor(Color);

    Color.setRgbF(_pass->getSpecular().r,_pass->getSpecular().g,_pass->getSpecular().b);
    Pal.setColor(QPalette::Window, Color);
    ui->specularColorWidget->setPalette(Pal);
    specularColorDialog->setCurrentColor(Color);

    Color.setRgbF(_pass->getEmissive().r,_pass->getEmissive().g,_pass->getEmissive().b);
    Pal.setColor(QPalette::Window, Color);
    ui->emissiveColorWidget->setPalette(Pal);
    emissiveColorDialog->setCurrentColor(Color);

    auto itTU = _pass->getTextureUnitStates();
    int tcount=0;
    for (Ogre::TextureUnitState *textureUnit : itTU)
    {
        ++tcount;

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

    Ogre::MaterialPtr m = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().getByName(mMaterialName.toStdString().data()));

    Ogre::MaterialSerializer ms;
    ms.queueForExport(m,false,false,mMaterialName.toStdString().data());

    setMaterialText(ms.getQueuedAsString().data());
}

bool MaterialEditor::validateScript(Ogre::DataStreamPtr& dataStream)
{

    try{
        class MyListener: public Ogre::ScriptCompilerListener
        {
            private:
                std::vector<Ogre::Exception> errors;
            public:
            virtual void handleError(Ogre::ScriptCompiler *compiler, Ogre::uint32 code, const Ogre::String &file, int line, const Ogre::String &msg){
                Ogre::LogManager::getSingleton().logError("Listener: "+msg);
                Ogre::Exception e{0,msg,"ScriptCompilerListener","error",file.c_str(),line};
                errors.push_back(e);
            }
                const std::vector<Ogre::Exception> &getErrors(){return errors;};
        };

        if(!Ogre::ResourceGroupManager::getSingleton().resourceGroupExists("Test_Script"))
            Ogre::ResourceGroupManager::getSingleton().createResourceGroup("Test_Script");
        if(Ogre::MaterialManager::getSingleton().resourceExists(mMaterialName.toStdString().data(),"Test_Script"))
            Ogre::MaterialManager::getSingleton().remove(mMaterialName.toStdString().data(),"Test_Script");

        MyListener *l = new MyListener();
        Ogre::ScriptCompilerManager::getSingleton().setListener(l);
        Ogre::ScriptCompilerManager::getSingleton().parseScript(dataStream,"Test_Script");
        Ogre::MaterialManager::getSingleton().remove(mMaterialName.toStdString().data(),"Test_Script");

        QString errorMessages;
        auto errors = l->getErrors();
        for(const auto &e : errors){
            errorMessages+="Error on line ("+QString::number(e.getLine())+"): "+e.getDescription().c_str()+"\n";
        }
        if(!l->getErrors().empty()){
            QMessageBox mBox;
            mBox.setText(errorMessages);
            mBox.exec();
        }
        return l->getErrors().empty();
    } catch(Ogre::Exception &e){
        QMessageBox mBox;
        mBox.setText(QString("Error: ")+e.what()+"\n");
        mBox.exec();
        return false;
    } catch(...){
        QMessageBox mBox;
        mBox.setText("Unknown error\n");
        mBox.exec();
        return false;
    }
    return true;
}

void MaterialEditor::on_techComboBox_currentIndexChanged(int index)
{
    if(index>0)
    {
        Ogre::MaterialPtr m = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().getByName(mMaterialName.toStdString().data()));

        mSelectedTechnique = m.get()->getTechnique(index-1);

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

     //   ui->scrollArea->setEnabled(false);

        ui->checkBoxLightning->setChecked(false);
        ui->srcSceneBlendBox->setCurrentIndex(0);
        ui->dstSceneBlendBox->setCurrentIndex(0);
        ui->checkBoxDepthWrite->setChecked(false);
        ui->checkBoxDepthCheck->setChecked(false);
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

        auto effects = mSelectedTextureUnit->getEffects();
        for(auto effectPair : effects){
            if(effectPair.first==Ogre::TextureUnitState::ET_UVSCROLL ||
                    effectPair.first==Ogre::TextureUnitState::ET_USCROLL)
                ui->scrollAnimUSpeed->setValue(effectPair.second.arg1);
            else if(effectPair.first==Ogre::TextureUnitState::ET_UVSCROLL ||
                    effectPair.first==Ogre::TextureUnitState::ET_VSCROLL)
                ui->scrollAnimVSpeed->setValue(effectPair.second.arg1);
        }
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
    Pal.setColor(QPalette::Window, arg1);
    ui->ambientColorWidget->setPalette(Pal);
    if(mSelectedPass)
        mSelectedPass->setAmbient(arg1.redF(),arg1.greenF(),arg1.blueF());
    updateMaterialText();
}

void MaterialEditor::on_Difuse_Color_Selected(const QColor &arg1)
{
    QPalette Pal(palette());
    Pal.setColor(QPalette::Window, arg1);
    ui->difuseColorWidget->setPalette(Pal);
    if(mSelectedPass)
        mSelectedPass->setDiffuse(arg1.redF(),arg1.greenF(),arg1.blueF(),ui->alphaDifuse->text().toFloat());
    updateMaterialText();
}

void MaterialEditor::on_Specular_Color_Selected(const QColor &arg1)
{
    QPalette Pal(palette());
    Pal.setColor(QPalette::Window, arg1);
    ui->specularColorWidget->setPalette(Pal);
    if(mSelectedPass)
        mSelectedPass->setSpecular(arg1.redF(),arg1.greenF(),arg1.blueF(),ui->alphaSpecular->text().toFloat());
    updateMaterialText();
}

void MaterialEditor::on_Emissive_Color_Selected(const QColor &arg1)
{
    QPalette Pal(palette());
    Pal.setColor(QPalette::Window, arg1);
    ui->emissiveColorWidget->setPalette(Pal);
    if(mSelectedPass)
        mSelectedPass->setEmissive(arg1.redF(),arg1.greenF(),arg1.blueF());
    updateMaterialText();
}

void MaterialEditor::on_textMaterial_textChanged()
{
    ui->applyButton->setEnabled(true);
}

void MaterialEditor::on_applyButton_clicked()
{
    Ogre::String script = ui->textMaterial->toPlainText().toStdString().data();
    Ogre::MemoryDataStream *memoryStream = new Ogre::MemoryDataStream((void*)script.c_str(), script.length() * sizeof(char));
    Ogre::DataStreamPtr dataStream(memoryStream);

    if(!validateScript(dataStream)){
        return;
    }

    if(Ogre::MaterialManager::getSingleton().resourceExists(mMaterialName.toStdString().data()))
        Ogre::MaterialManager::getSingleton().remove(mMaterialName.toStdString().data());

    Ogre::MaterialManager::getSingleton().parseScript(dataStream,Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    mMaterialName = ui->textMaterial->toPlainText();
    mMaterialName = mMaterialName.remove(0,mMaterialName.indexOf("material")+9);
    mMaterialName.remove(mMaterialName.indexOf("\n"),mMaterialName.size());

    Ogre::MaterialPtr material = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().getByName(mMaterialName.toStdString().data()));

    material->compile();

    Ogre::MaterialManager::getSingleton().reloadAll(true);
    Ogre::MeshManager::getSingleton().reloadAll(true);

    //Reaply all materials after reloading
    for(Ogre::SceneNode * sn : Manager::getSingleton()->getSceneNodes())
    {
        Ogre::LogManager::getSingleton().logMessage(sn->getName());
        if(!sn->getName().empty()&&!sn->getAttachedObjects().empty()) {
            Ogre::Entity *e = static_cast<Ogre::Entity *>(sn->getAttachedObject(0));
            e->setMaterialName(e->getSubEntity(0)->getMaterialName());
        }
    }

    setMaterial(mMaterialName);

    ui->scrollArea->setEnabled(true);
    ui->applyButton->setEnabled(false);
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
        Ogre::MaterialPtr m = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().getByName(mMaterialName.toStdString().data()));
        Ogre::Technique *t = m.get()->createTechnique();
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
                                                     tr("Image File (*.bmp *.jpg *.gif *.raw *.png *.tga *.dds)"),
                                                    nullptr, QFileDialog::DontUseNativeDialog);

    if(filePath.size()&&mSelectedTextureUnit)
    {

        QFileInfo file;
        file.setFile(filePath);

        try {
            Ogre::TextureManager::getSingleton().getByName(file.fileName().toStdString().data(),file.path().toStdString().data());
        } catch (...) {
            Ogre::ResourceGroupManager::getSingleton().addResourceLocation(file.path().toStdString().data(),"FileSystem",file.path().toStdString().data());
            Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();

            Ogre::Image i;
            i.load(file.fileName().toStdString().data(),file.path().toStdString().data());
            Ogre::TextureManager::getSingleton().loadImage(file.fileName().toStdString().data(),file.path().toStdString().data(),i);
        }
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

void MaterialEditor::on_checkBoxLightning_toggled(bool checked)
{
    if(mSelectedPass)
        mSelectedPass->setLightingEnabled(checked);
    updateMaterialText();
}


void MaterialEditor::on_checkBoxDepthWrite_toggled(bool checked)
{
    if(mSelectedPass)
        mSelectedPass->setDepthWriteEnabled(checked);
    updateMaterialText();
}


void MaterialEditor::on_checkBoxDepthCheck_toggled(bool checked)
{
    if(mSelectedPass)
        mSelectedPass->setDepthCheckEnabled(checked);
    updateMaterialText();
}

void MaterialEditor::on_comboPolygonMode_currentIndexChanged(int index)
{
    if(mSelectedPass)
        mSelectedPass->setPolygonMode(static_cast<Ogre::PolygonMode>(index+1));
    updateMaterialText();
}


void MaterialEditor::on_scrollAnimUSpeed_valueChanged(double arg1)
{
    mSelectedTextureUnit->setScrollAnimation(ui->scrollAnimUSpeed->value(),ui->scrollAnimVSpeed->value());
    updateMaterialText();
}


void MaterialEditor::on_scrollAnimVSpeed_valueChanged(double arg1)
{
    mSelectedTextureUnit->setScrollAnimation(ui->scrollAnimUSpeed->value(),ui->scrollAnimVSpeed->value());
    updateMaterialText();
}

