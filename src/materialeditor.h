#ifndef MATERIALEDITOR_H
#define MATERIALEDITOR_H

#include <QDialog>
#include <QColorDialog>
#include <QMap>
#include <QSyntaxHighlighter>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgreTextureManager.h>
#include <OgreMaterialSerializer.h>
#include <OgreTechnique.h>
#include <OgrePass.h>


namespace Ui {
class MaterialEditor;
}

class MaterialHighlighter;

class MaterialEditor : public QDialog
{
        Q_OBJECT
        
    public:
        explicit MaterialEditor(QWidget *parent = 0);
        virtual ~MaterialEditor();
        void setMaterialText(const QString &_mat);
        void setMaterial(const QString& _material);
        std::string getMaterialText() const;
        std::string getMaterialName() const;
        bool isScrollAreaEnabled() const;
                        
private slots:
    void on_buttonEditAmbientColor_clicked();

    void on_techComboBox_currentIndexChanged(const QString &arg1);

    void on_passComboBox_currentIndexChanged(const QString &arg1);

    void on_Ambient_Color_Selected(const QColor &arg1);

    void on_Difuse_Color_Selected(const QColor &arg1);

    void on_Specular_Color_Selected(const QColor &arg1);

    void on_Emissive_Color_Selected(const QColor &arg1);

    void on_textMaterial_textChanged();

    void on_applyButton_clicked();

    void on_buttonEditDifuseColor_clicked();

    void on_buttonEditSpecularColor_clicked();

    void on_buttonEditEmissiveColor_clicked();

    void on_srcSceneBlendBox_currentIndexChanged(int index);

    void on_dstSceneBlendBox_currentIndexChanged(int index);

    void on_checkBoxUseVertexColorToAmbient_toggled(bool checked);

    void on_checkBoxUseVertexColorToDifuse_toggled(bool checked);

    void on_checkBoxUseVertexColorToSpecular_toggled(bool checked);

    void on_checkBoxUseVertexColorToEmissive_toggled(bool checked);

    void on_alphaDifuse_valueChanged(double arg1);

    void on_alphaSpecular_valueChanged(double arg1);

    void on_shineSpecular_valueChanged(double arg1);

    void on_newTechnique_clicked();

    void on_passNewButton_clicked();

    void on_techComboBox_currentIndexChanged(int index);

    void on_TextureUnitNewButton_clicked();

    void on_passComboBox_currentIndexChanged(int index);

    void on_selectTexture_clicked();

    void on_ComboTextureUnit_currentIndexChanged(int index);

    void on_removeTexture_clicked();

    void on_checkBoxLightning_toggled(bool checked);

    void on_checkBoxDepthWrite_toggled(bool checked);

    void on_checkBoxDepthCheck_toggled(bool checked);

    void on_comboPolygonMode_currentIndexChanged(int index);

    void on_scrollAnimUSpeed_valueChanged(double arg1);

    void on_scrollAnimVSpeed_valueChanged(double arg1);

private:
    Ui::MaterialEditor *ui;

    QString mMaterialName;

    QColorDialog *ambientColorDialog;
    QColorDialog *difuseColorDialog;
    QColorDialog *specularColorDialog;
    QColorDialog *emissiveColorDialog;

    QMap <int, QMap <int, Ogre::Pass*> > mTechMap;
    QMap <int, QList <QString> > mTechMapName;
    QMap <int, Ogre::Pass*> mPassMap;
    Ogre::Pass* mSelectedPass;
    Ogre::TextureUnitState* mSelectedTextureUnit;
    Ogre::Technique* mSelectedTechnique;
    QMap <QString, Ogre::TextureUnitState*> mTexUnitMap;
    void setTechFields(const QMap<int, Ogre::Pass *> &_techMap, const QList<QString> &_passList);
    void setPassFields(Ogre::Pass *_pass);
    void updateMaterialText();
    bool validateScript(Ogre::DataStreamPtr &dataStream);

    MaterialHighlighter* mMaterialHighlighter;
};

#endif // MATERIALEDITOR_H
