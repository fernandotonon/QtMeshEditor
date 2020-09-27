/********************************************************************************
** Form generated from reading UI file 'materialeditor.ui'
**
** Created by: Qt User Interface Compiler version 5.2.1
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_MATERIALEDITOR_H
#define UI_MATERIALEDITOR_H

#include <QtCore/QVariant>
#include <QtWidgets/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_MaterialEditor
{
public:
    QVBoxLayout *verticalLayout;
    QTextEdit *textMaterial;
    QHBoxLayout *horizontalLayout;
    QSpacerItem *horizontalSpacer;
    QPushButton *saveButton;
    QSpacerItem *horizontalSpacer_2;
    QScrollArea *scrollArea;
    QWidget *scrollAreaWidgetContents;
    QVBoxLayout *verticalLayout_2;
    QHBoxLayout *horizontalLayout_3;
    QLabel *label_3;
    QComboBox *techComboBox;
    QPushButton *newTechnique;
    QSpacerItem *horizontalSpacer_5;
    QHBoxLayout *horizontalLayout_4;
    QLabel *label_4;
    QComboBox *passComboBox;
    QPushButton *passNewButton;
    QSpacerItem *horizontalSpacer_6;
    QHBoxLayout *horizontalLayout_2;
    QLabel *label;
    QWidget *widget;
    QRadioButton *radioLightninOn;
    QRadioButton *radioLightninOff;
    QSpacerItem *horizontalSpacer_3;
    QHBoxLayout *horizontalLayout_6;
    QLabel *label_6;
    QWidget *widget_3;
    QRadioButton *radioDepthWriteOn;
    QRadioButton *radioDepthWriteOff;
    QSpacerItem *horizontalSpacer_8;
    QHBoxLayout *horizontalLayout_5;
    QLabel *label_5;
    QWidget *widget_2;
    QRadioButton *radioDepthCheckOn;
    QRadioButton *radioDepthCheckOff;
    QSpacerItem *horizontalSpacer_7;
    QHBoxLayout *horizontalLayout_8;
    QLabel *label_8;
    QComboBox *srcSceneBlendBox;
    QComboBox *dstSceneBlendBox;
    QSpacerItem *horizontalSpacer_9;
    QHBoxLayout *horizontalLayout_ColorAmbient;
    QLabel *label_2;
    QWidget *ambientColorWidget;
    QPushButton *buttonEditAmbientColor;
    QCheckBox *checkBoxUseVertexColorToAmbient;
    QSpacerItem *horizontalSpacer_4;
    QHBoxLayout *horizontalLayout_11;
    QLabel *label_10;
    QWidget *difuseColorWidget;
    QPushButton *buttonEditDifuseColor;
    QLabel *label_13;
    QDoubleSpinBox *alphaDifuse;
    QCheckBox *checkBoxUseVertexColorToDifuse;
    QSpacerItem *horizontalSpacer_11;
    QHBoxLayout *horizontalLayout_10;
    QLabel *label_11;
    QWidget *specularColorWidget;
    QPushButton *buttonEditSpecularColor;
    QLabel *label_14;
    QDoubleSpinBox *alphaSpecular;
    QLabel *label_9;
    QDoubleSpinBox *shineSpecular;
    QCheckBox *checkBoxUseVertexColorToSpecular;
    QSpacerItem *horizontalSpacer_12;
    QHBoxLayout *horizontalLayout_9;
    QLabel *label_12;
    QWidget *emissiveColorWidget;
    QPushButton *buttonEditEmissiveColor;
    QCheckBox *checkBoxUseVertexColorToEmissive;
    QSpacerItem *horizontalSpacer_13;
    QHBoxLayout *horizontalLayout_7;
    QLabel *label_7;
    QComboBox *ComboTextureUnit;
    QPushButton *TextureUnitNewButton;
    QSpacerItem *horizontalSpacer_10;
    QHBoxLayout *horizontalLayout_12;
    QLabel *label_15;
    QLabel *textureName;
    QPushButton *selectTexture;
    QPushButton *removeTexture;
    QSpacerItem *horizontalSpacer_14;
    QVBoxLayout *ogreLayout;
    QDialogButtonBox *buttonBox;

    void setupUi(QDialog *MaterialEditor)
    {
        if (MaterialEditor->objectName().isEmpty())
            MaterialEditor->setObjectName(QStringLiteral("MaterialEditor"));
        MaterialEditor->setWindowModality(Qt::WindowModal);
        MaterialEditor->resize(600, 500);
        MaterialEditor->setMinimumSize(QSize(600, 500));
        MaterialEditor->setSizeGripEnabled(false);
        MaterialEditor->setModal(false);
        verticalLayout = new QVBoxLayout(MaterialEditor);
        verticalLayout->setObjectName(QStringLiteral("verticalLayout"));
        textMaterial = new QTextEdit(MaterialEditor);
        textMaterial->setObjectName(QStringLiteral("textMaterial"));

        verticalLayout->addWidget(textMaterial);

        horizontalLayout = new QHBoxLayout();
        horizontalLayout->setObjectName(QStringLiteral("horizontalLayout"));
        horizontalSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout->addItem(horizontalSpacer);

        saveButton = new QPushButton(MaterialEditor);
        saveButton->setObjectName(QStringLiteral("saveButton"));
        saveButton->setEnabled(false);

        horizontalLayout->addWidget(saveButton);

        horizontalSpacer_2 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout->addItem(horizontalSpacer_2);


        verticalLayout->addLayout(horizontalLayout);

        scrollArea = new QScrollArea(MaterialEditor);
        scrollArea->setObjectName(QStringLiteral("scrollArea"));
        scrollArea->setMinimumSize(QSize(0, 200));
        scrollArea->setWidgetResizable(true);
        scrollAreaWidgetContents = new QWidget();
        scrollAreaWidgetContents->setObjectName(QStringLiteral("scrollAreaWidgetContents"));
        scrollAreaWidgetContents->setGeometry(QRect(0, 0, 563, 372));
        verticalLayout_2 = new QVBoxLayout(scrollAreaWidgetContents);
        verticalLayout_2->setObjectName(QStringLiteral("verticalLayout_2"));
        horizontalLayout_3 = new QHBoxLayout();
        horizontalLayout_3->setObjectName(QStringLiteral("horizontalLayout_3"));
        label_3 = new QLabel(scrollAreaWidgetContents);
        label_3->setObjectName(QStringLiteral("label_3"));

        horizontalLayout_3->addWidget(label_3);

        techComboBox = new QComboBox(scrollAreaWidgetContents);
        techComboBox->setObjectName(QStringLiteral("techComboBox"));
        QSizePolicy sizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
        sizePolicy.setHorizontalStretch(0);
        sizePolicy.setVerticalStretch(0);
        sizePolicy.setHeightForWidth(techComboBox->sizePolicy().hasHeightForWidth());
        techComboBox->setSizePolicy(sizePolicy);
        techComboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);

        horizontalLayout_3->addWidget(techComboBox);

        newTechnique = new QPushButton(scrollAreaWidgetContents);
        newTechnique->setObjectName(QStringLiteral("newTechnique"));

        horizontalLayout_3->addWidget(newTechnique);

        horizontalSpacer_5 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_3->addItem(horizontalSpacer_5);


        verticalLayout_2->addLayout(horizontalLayout_3);

        horizontalLayout_4 = new QHBoxLayout();
        horizontalLayout_4->setObjectName(QStringLiteral("horizontalLayout_4"));
        label_4 = new QLabel(scrollAreaWidgetContents);
        label_4->setObjectName(QStringLiteral("label_4"));

        horizontalLayout_4->addWidget(label_4);

        passComboBox = new QComboBox(scrollAreaWidgetContents);
        passComboBox->setObjectName(QStringLiteral("passComboBox"));
        passComboBox->setEnabled(false);
        sizePolicy.setHeightForWidth(passComboBox->sizePolicy().hasHeightForWidth());
        passComboBox->setSizePolicy(sizePolicy);
        passComboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);

        horizontalLayout_4->addWidget(passComboBox);

        passNewButton = new QPushButton(scrollAreaWidgetContents);
        passNewButton->setObjectName(QStringLiteral("passNewButton"));
        passNewButton->setEnabled(false);

        horizontalLayout_4->addWidget(passNewButton);

        horizontalSpacer_6 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_4->addItem(horizontalSpacer_6);


        verticalLayout_2->addLayout(horizontalLayout_4);

        horizontalLayout_2 = new QHBoxLayout();
        horizontalLayout_2->setObjectName(QStringLiteral("horizontalLayout_2"));
        label = new QLabel(scrollAreaWidgetContents);
        label->setObjectName(QStringLiteral("label"));

        horizontalLayout_2->addWidget(label);

        widget = new QWidget(scrollAreaWidgetContents);
        widget->setObjectName(QStringLiteral("widget"));
        widget->setMinimumSize(QSize(100, 0));
        radioLightninOn = new QRadioButton(widget);
        radioLightninOn->setObjectName(QStringLiteral("radioLightninOn"));
        radioLightninOn->setEnabled(false);
        radioLightninOn->setGeometry(QRect(10, 0, 37, 17));
        radioLightninOn->setAutoRepeat(false);
        radioLightninOn->setAutoExclusive(true);
        radioLightninOff = new QRadioButton(widget);
        radioLightninOff->setObjectName(QStringLiteral("radioLightninOff"));
        radioLightninOff->setEnabled(false);
        radioLightninOff->setGeometry(QRect(50, 0, 39, 17));

        horizontalLayout_2->addWidget(widget);

        horizontalSpacer_3 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_2->addItem(horizontalSpacer_3);


        verticalLayout_2->addLayout(horizontalLayout_2);

        horizontalLayout_6 = new QHBoxLayout();
        horizontalLayout_6->setObjectName(QStringLiteral("horizontalLayout_6"));
        label_6 = new QLabel(scrollAreaWidgetContents);
        label_6->setObjectName(QStringLiteral("label_6"));

        horizontalLayout_6->addWidget(label_6);

        widget_3 = new QWidget(scrollAreaWidgetContents);
        widget_3->setObjectName(QStringLiteral("widget_3"));
        widget_3->setMinimumSize(QSize(100, 0));
        radioDepthWriteOn = new QRadioButton(widget_3);
        radioDepthWriteOn->setObjectName(QStringLiteral("radioDepthWriteOn"));
        radioDepthWriteOn->setEnabled(false);
        radioDepthWriteOn->setGeometry(QRect(0, 0, 37, 17));
        radioDepthWriteOff = new QRadioButton(widget_3);
        radioDepthWriteOff->setObjectName(QStringLiteral("radioDepthWriteOff"));
        radioDepthWriteOff->setEnabled(false);
        radioDepthWriteOff->setGeometry(QRect(40, 0, 39, 17));

        horizontalLayout_6->addWidget(widget_3);

        horizontalSpacer_8 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_6->addItem(horizontalSpacer_8);


        verticalLayout_2->addLayout(horizontalLayout_6);

        horizontalLayout_5 = new QHBoxLayout();
        horizontalLayout_5->setObjectName(QStringLiteral("horizontalLayout_5"));
        label_5 = new QLabel(scrollAreaWidgetContents);
        label_5->setObjectName(QStringLiteral("label_5"));

        horizontalLayout_5->addWidget(label_5);

        widget_2 = new QWidget(scrollAreaWidgetContents);
        widget_2->setObjectName(QStringLiteral("widget_2"));
        widget_2->setMinimumSize(QSize(100, 0));
        radioDepthCheckOn = new QRadioButton(widget_2);
        radioDepthCheckOn->setObjectName(QStringLiteral("radioDepthCheckOn"));
        radioDepthCheckOn->setEnabled(false);
        radioDepthCheckOn->setGeometry(QRect(0, 0, 37, 17));
        radioDepthCheckOff = new QRadioButton(widget_2);
        radioDepthCheckOff->setObjectName(QStringLiteral("radioDepthCheckOff"));
        radioDepthCheckOff->setEnabled(false);
        radioDepthCheckOff->setGeometry(QRect(40, 0, 39, 17));

        horizontalLayout_5->addWidget(widget_2);

        horizontalSpacer_7 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_5->addItem(horizontalSpacer_7);


        verticalLayout_2->addLayout(horizontalLayout_5);

        horizontalLayout_8 = new QHBoxLayout();
        horizontalLayout_8->setObjectName(QStringLiteral("horizontalLayout_8"));
        label_8 = new QLabel(scrollAreaWidgetContents);
        label_8->setObjectName(QStringLiteral("label_8"));

        horizontalLayout_8->addWidget(label_8);

        srcSceneBlendBox = new QComboBox(scrollAreaWidgetContents);
        srcSceneBlendBox->setObjectName(QStringLiteral("srcSceneBlendBox"));
        srcSceneBlendBox->setEnabled(false);
        sizePolicy.setHeightForWidth(srcSceneBlendBox->sizePolicy().hasHeightForWidth());
        srcSceneBlendBox->setSizePolicy(sizePolicy);

        horizontalLayout_8->addWidget(srcSceneBlendBox);

        dstSceneBlendBox = new QComboBox(scrollAreaWidgetContents);
        dstSceneBlendBox->setObjectName(QStringLiteral("dstSceneBlendBox"));
        dstSceneBlendBox->setEnabled(false);
        sizePolicy.setHeightForWidth(dstSceneBlendBox->sizePolicy().hasHeightForWidth());
        dstSceneBlendBox->setSizePolicy(sizePolicy);

        horizontalLayout_8->addWidget(dstSceneBlendBox);

        horizontalSpacer_9 = new QSpacerItem(379, 15, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_8->addItem(horizontalSpacer_9);


        verticalLayout_2->addLayout(horizontalLayout_8);

        horizontalLayout_ColorAmbient = new QHBoxLayout();
        horizontalLayout_ColorAmbient->setObjectName(QStringLiteral("horizontalLayout_ColorAmbient"));
        label_2 = new QLabel(scrollAreaWidgetContents);
        label_2->setObjectName(QStringLiteral("label_2"));

        horizontalLayout_ColorAmbient->addWidget(label_2);

        ambientColorWidget = new QWidget(scrollAreaWidgetContents);
        ambientColorWidget->setObjectName(QStringLiteral("ambientColorWidget"));
        ambientColorWidget->setMinimumSize(QSize(50, 0));
        ambientColorWidget->setAutoFillBackground(true);

        horizontalLayout_ColorAmbient->addWidget(ambientColorWidget);

        buttonEditAmbientColor = new QPushButton(scrollAreaWidgetContents);
        buttonEditAmbientColor->setObjectName(QStringLiteral("buttonEditAmbientColor"));
        buttonEditAmbientColor->setEnabled(false);

        horizontalLayout_ColorAmbient->addWidget(buttonEditAmbientColor);

        checkBoxUseVertexColorToAmbient = new QCheckBox(scrollAreaWidgetContents);
        checkBoxUseVertexColorToAmbient->setObjectName(QStringLiteral("checkBoxUseVertexColorToAmbient"));
        checkBoxUseVertexColorToAmbient->setEnabled(false);

        horizontalLayout_ColorAmbient->addWidget(checkBoxUseVertexColorToAmbient);

        horizontalSpacer_4 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_ColorAmbient->addItem(horizontalSpacer_4);


        verticalLayout_2->addLayout(horizontalLayout_ColorAmbient);

        horizontalLayout_11 = new QHBoxLayout();
        horizontalLayout_11->setObjectName(QStringLiteral("horizontalLayout_11"));
        label_10 = new QLabel(scrollAreaWidgetContents);
        label_10->setObjectName(QStringLiteral("label_10"));

        horizontalLayout_11->addWidget(label_10);

        difuseColorWidget = new QWidget(scrollAreaWidgetContents);
        difuseColorWidget->setObjectName(QStringLiteral("difuseColorWidget"));
        difuseColorWidget->setMinimumSize(QSize(50, 0));
        difuseColorWidget->setAutoFillBackground(true);

        horizontalLayout_11->addWidget(difuseColorWidget);

        buttonEditDifuseColor = new QPushButton(scrollAreaWidgetContents);
        buttonEditDifuseColor->setObjectName(QStringLiteral("buttonEditDifuseColor"));
        buttonEditDifuseColor->setEnabled(false);

        horizontalLayout_11->addWidget(buttonEditDifuseColor);

        label_13 = new QLabel(scrollAreaWidgetContents);
        label_13->setObjectName(QStringLiteral("label_13"));

        horizontalLayout_11->addWidget(label_13);

        alphaDifuse = new QDoubleSpinBox(scrollAreaWidgetContents);
        alphaDifuse->setObjectName(QStringLiteral("alphaDifuse"));
        alphaDifuse->setEnabled(false);
        alphaDifuse->setDecimals(4);
        alphaDifuse->setMaximum(1);
        alphaDifuse->setSingleStep(0.1);

        horizontalLayout_11->addWidget(alphaDifuse);

        checkBoxUseVertexColorToDifuse = new QCheckBox(scrollAreaWidgetContents);
        checkBoxUseVertexColorToDifuse->setObjectName(QStringLiteral("checkBoxUseVertexColorToDifuse"));
        checkBoxUseVertexColorToDifuse->setEnabled(false);

        horizontalLayout_11->addWidget(checkBoxUseVertexColorToDifuse);

        horizontalSpacer_11 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_11->addItem(horizontalSpacer_11);


        verticalLayout_2->addLayout(horizontalLayout_11);

        horizontalLayout_10 = new QHBoxLayout();
        horizontalLayout_10->setObjectName(QStringLiteral("horizontalLayout_10"));
        label_11 = new QLabel(scrollAreaWidgetContents);
        label_11->setObjectName(QStringLiteral("label_11"));

        horizontalLayout_10->addWidget(label_11);

        specularColorWidget = new QWidget(scrollAreaWidgetContents);
        specularColorWidget->setObjectName(QStringLiteral("specularColorWidget"));
        specularColorWidget->setMinimumSize(QSize(50, 0));
        specularColorWidget->setAutoFillBackground(true);

        horizontalLayout_10->addWidget(specularColorWidget);

        buttonEditSpecularColor = new QPushButton(scrollAreaWidgetContents);
        buttonEditSpecularColor->setObjectName(QStringLiteral("buttonEditSpecularColor"));
        buttonEditSpecularColor->setEnabled(false);

        horizontalLayout_10->addWidget(buttonEditSpecularColor);

        label_14 = new QLabel(scrollAreaWidgetContents);
        label_14->setObjectName(QStringLiteral("label_14"));

        horizontalLayout_10->addWidget(label_14);

        alphaSpecular = new QDoubleSpinBox(scrollAreaWidgetContents);
        alphaSpecular->setObjectName(QStringLiteral("alphaSpecular"));
        alphaSpecular->setEnabled(false);
        alphaSpecular->setFrame(true);
        alphaSpecular->setDecimals(4);
        alphaSpecular->setMaximum(1);
        alphaSpecular->setSingleStep(0.1);

        horizontalLayout_10->addWidget(alphaSpecular);

        label_9 = new QLabel(scrollAreaWidgetContents);
        label_9->setObjectName(QStringLiteral("label_9"));

        horizontalLayout_10->addWidget(label_9);

        shineSpecular = new QDoubleSpinBox(scrollAreaWidgetContents);
        shineSpecular->setObjectName(QStringLiteral("shineSpecular"));
        shineSpecular->setEnabled(false);
        shineSpecular->setDecimals(4);
        shineSpecular->setMaximum(1);
        shineSpecular->setSingleStep(0.1);

        horizontalLayout_10->addWidget(shineSpecular);

        checkBoxUseVertexColorToSpecular = new QCheckBox(scrollAreaWidgetContents);
        checkBoxUseVertexColorToSpecular->setObjectName(QStringLiteral("checkBoxUseVertexColorToSpecular"));
        checkBoxUseVertexColorToSpecular->setEnabled(false);

        horizontalLayout_10->addWidget(checkBoxUseVertexColorToSpecular);

        horizontalSpacer_12 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_10->addItem(horizontalSpacer_12);


        verticalLayout_2->addLayout(horizontalLayout_10);

        horizontalLayout_9 = new QHBoxLayout();
        horizontalLayout_9->setObjectName(QStringLiteral("horizontalLayout_9"));
        label_12 = new QLabel(scrollAreaWidgetContents);
        label_12->setObjectName(QStringLiteral("label_12"));

        horizontalLayout_9->addWidget(label_12);

        emissiveColorWidget = new QWidget(scrollAreaWidgetContents);
        emissiveColorWidget->setObjectName(QStringLiteral("emissiveColorWidget"));
        emissiveColorWidget->setMinimumSize(QSize(50, 0));
        emissiveColorWidget->setAutoFillBackground(true);

        horizontalLayout_9->addWidget(emissiveColorWidget);

        buttonEditEmissiveColor = new QPushButton(scrollAreaWidgetContents);
        buttonEditEmissiveColor->setObjectName(QStringLiteral("buttonEditEmissiveColor"));
        buttonEditEmissiveColor->setEnabled(false);

        horizontalLayout_9->addWidget(buttonEditEmissiveColor);

        checkBoxUseVertexColorToEmissive = new QCheckBox(scrollAreaWidgetContents);
        checkBoxUseVertexColorToEmissive->setObjectName(QStringLiteral("checkBoxUseVertexColorToEmissive"));
        checkBoxUseVertexColorToEmissive->setEnabled(false);

        horizontalLayout_9->addWidget(checkBoxUseVertexColorToEmissive);

        horizontalSpacer_13 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_9->addItem(horizontalSpacer_13);


        verticalLayout_2->addLayout(horizontalLayout_9);

        horizontalLayout_7 = new QHBoxLayout();
        horizontalLayout_7->setObjectName(QStringLiteral("horizontalLayout_7"));
        label_7 = new QLabel(scrollAreaWidgetContents);
        label_7->setObjectName(QStringLiteral("label_7"));

        horizontalLayout_7->addWidget(label_7);

        ComboTextureUnit = new QComboBox(scrollAreaWidgetContents);
        ComboTextureUnit->setObjectName(QStringLiteral("ComboTextureUnit"));
        ComboTextureUnit->setEnabled(false);
        sizePolicy.setHeightForWidth(ComboTextureUnit->sizePolicy().hasHeightForWidth());
        ComboTextureUnit->setSizePolicy(sizePolicy);
        ComboTextureUnit->setSizeAdjustPolicy(QComboBox::AdjustToContents);

        horizontalLayout_7->addWidget(ComboTextureUnit);

        TextureUnitNewButton = new QPushButton(scrollAreaWidgetContents);
        TextureUnitNewButton->setObjectName(QStringLiteral("TextureUnitNewButton"));
        TextureUnitNewButton->setEnabled(false);

        horizontalLayout_7->addWidget(TextureUnitNewButton);

        horizontalSpacer_10 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_7->addItem(horizontalSpacer_10);


        verticalLayout_2->addLayout(horizontalLayout_7);

        horizontalLayout_12 = new QHBoxLayout();
        horizontalLayout_12->setObjectName(QStringLiteral("horizontalLayout_12"));
        label_15 = new QLabel(scrollAreaWidgetContents);
        label_15->setObjectName(QStringLiteral("label_15"));

        horizontalLayout_12->addWidget(label_15);

        textureName = new QLabel(scrollAreaWidgetContents);
        textureName->setObjectName(QStringLiteral("textureName"));
        QFont font;
        font.setItalic(true);
        textureName->setFont(font);

        horizontalLayout_12->addWidget(textureName);

        selectTexture = new QPushButton(scrollAreaWidgetContents);
        selectTexture->setObjectName(QStringLiteral("selectTexture"));
        selectTexture->setEnabled(false);

        horizontalLayout_12->addWidget(selectTexture);

        removeTexture = new QPushButton(scrollAreaWidgetContents);
        removeTexture->setObjectName(QStringLiteral("removeTexture"));
        removeTexture->setEnabled(false);

        horizontalLayout_12->addWidget(removeTexture);

        horizontalSpacer_14 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_12->addItem(horizontalSpacer_14);


        verticalLayout_2->addLayout(horizontalLayout_12);

        scrollArea->setWidget(scrollAreaWidgetContents);

        verticalLayout->addWidget(scrollArea);

        ogreLayout = new QVBoxLayout();
        ogreLayout->setObjectName(QStringLiteral("ogreLayout"));

        verticalLayout->addLayout(ogreLayout);

        buttonBox = new QDialogButtonBox(MaterialEditor);
        buttonBox->setObjectName(QStringLiteral("buttonBox"));
        buttonBox->setOrientation(Qt::Horizontal);
        buttonBox->setStandardButtons(QDialogButtonBox::Close);
        buttonBox->setCenterButtons(true);

        verticalLayout->addWidget(buttonBox);


        retranslateUi(MaterialEditor);
        QObject::connect(buttonBox, SIGNAL(accepted()), MaterialEditor, SLOT(accept()));
        QObject::connect(buttonBox, SIGNAL(rejected()), MaterialEditor, SLOT(reject()));

        QMetaObject::connectSlotsByName(MaterialEditor);
    } // setupUi

    void retranslateUi(QDialog *MaterialEditor)
    {
        MaterialEditor->setWindowTitle(QApplication::translate("MaterialEditor", "Material Editor", 0));
        textMaterial->setHtml(QApplication::translate("MaterialEditor", "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.0//EN\" \"http://www.w3.org/TR/REC-html40/strict.dtd\">\n"
"<html><head><meta name=\"qrichtext\" content=\"1\" /><style type=\"text/css\">\n"
"p, li { white-space: pre-wrap; }\n"
"</style></head><body style=\" font-family:'MS Shell Dlg 2'; font-size:8.25pt; font-weight:400; font-style:normal;\">\n"
"<p style=\" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;\"><span style=\" font-size:8pt;\"> </span></p></body></html>", 0));
        saveButton->setText(QApplication::translate("MaterialEditor", "Save", 0));
        label_3->setText(QApplication::translate("MaterialEditor", "Technique:", 0));
        techComboBox->clear();
        techComboBox->insertItems(0, QStringList()
         << QString()
        );
        newTechnique->setText(QApplication::translate("MaterialEditor", "New", 0));
        label_4->setText(QApplication::translate("MaterialEditor", "Pass:", 0));
        passNewButton->setText(QApplication::translate("MaterialEditor", "New", 0));
        label->setText(QApplication::translate("MaterialEditor", "Lighting:", 0));
        radioLightninOn->setText(QApplication::translate("MaterialEditor", "On", 0));
        radioLightninOff->setText(QApplication::translate("MaterialEditor", "Off", 0));
        label_6->setText(QApplication::translate("MaterialEditor", "Depth Write:", 0));
        radioDepthWriteOn->setText(QApplication::translate("MaterialEditor", "On", 0));
        radioDepthWriteOff->setText(QApplication::translate("MaterialEditor", "Off", 0));
        label_5->setText(QApplication::translate("MaterialEditor", "Depth Check:", 0));
        radioDepthCheckOn->setText(QApplication::translate("MaterialEditor", "On", 0));
        radioDepthCheckOff->setText(QApplication::translate("MaterialEditor", "Off", 0));
        label_8->setText(QApplication::translate("MaterialEditor", "Scene Blend:", 0));
        srcSceneBlendBox->clear();
        srcSceneBlendBox->insertItems(0, QStringList()
         << QString()
         << QApplication::translate("MaterialEditor", "alpha_blend", 0)
         << QApplication::translate("MaterialEditor", "colour_blend", 0)
         << QApplication::translate("MaterialEditor", "add", 0)
         << QApplication::translate("MaterialEditor", "modulate", 0)
         << QApplication::translate("MaterialEditor", "replace", 0)
         << QApplication::translate("MaterialEditor", "one", 0)
         << QApplication::translate("MaterialEditor", "zero", 0)
         << QApplication::translate("MaterialEditor", "dest_colour", 0)
         << QApplication::translate("MaterialEditor", "src_colour", 0)
         << QApplication::translate("MaterialEditor", "one_minus_dest_colour", 0)
         << QApplication::translate("MaterialEditor", "one_minus_src_colour", 0)
         << QApplication::translate("MaterialEditor", "dest_alpha", 0)
         << QApplication::translate("MaterialEditor", "src_alpha", 0)
         << QApplication::translate("MaterialEditor", "one_minus_dest_alpha", 0)
         << QApplication::translate("MaterialEditor", "one_minus_src_alpha", 0)
        );
        dstSceneBlendBox->clear();
        dstSceneBlendBox->insertItems(0, QStringList()
         << QString()
         << QApplication::translate("MaterialEditor", "one", 0)
         << QApplication::translate("MaterialEditor", "zero", 0)
         << QApplication::translate("MaterialEditor", "dest_colour", 0)
         << QApplication::translate("MaterialEditor", "src_colour", 0)
         << QApplication::translate("MaterialEditor", "one_minus_dest_colour", 0)
         << QApplication::translate("MaterialEditor", "one_minus_src_colour", 0)
         << QApplication::translate("MaterialEditor", "dest_alpha", 0)
         << QApplication::translate("MaterialEditor", "src_alpha", 0)
         << QApplication::translate("MaterialEditor", "one_minus_dest_alpha", 0)
         << QApplication::translate("MaterialEditor", "one_minus_src_alpha", 0)
        );
        label_2->setText(QApplication::translate("MaterialEditor", "Ambient Color:", 0));
        buttonEditAmbientColor->setText(QApplication::translate("MaterialEditor", "Edit", 0));
        checkBoxUseVertexColorToAmbient->setText(QApplication::translate("MaterialEditor", "Use Vertex Color", 0));
        label_10->setText(QApplication::translate("MaterialEditor", "Diffuse Color:", 0));
        buttonEditDifuseColor->setText(QApplication::translate("MaterialEditor", "Edit", 0));
        label_13->setText(QApplication::translate("MaterialEditor", "Alpha:", 0));
        checkBoxUseVertexColorToDifuse->setText(QApplication::translate("MaterialEditor", "Use Vertex Color", 0));
        label_11->setText(QApplication::translate("MaterialEditor", "Specular Color:", 0));
        buttonEditSpecularColor->setText(QApplication::translate("MaterialEditor", "Edit", 0));
        label_14->setText(QApplication::translate("MaterialEditor", "Alpha:", 0));
        label_9->setText(QApplication::translate("MaterialEditor", "Shine:", 0));
        checkBoxUseVertexColorToSpecular->setText(QApplication::translate("MaterialEditor", "Use Vertex Color", 0));
        label_12->setText(QApplication::translate("MaterialEditor", "Emissive Color:", 0));
        buttonEditEmissiveColor->setText(QApplication::translate("MaterialEditor", "Edit", 0));
        checkBoxUseVertexColorToEmissive->setText(QApplication::translate("MaterialEditor", "Use Vertex Color", 0));
        label_7->setText(QApplication::translate("MaterialEditor", "Texture Unit:", 0));
        TextureUnitNewButton->setText(QApplication::translate("MaterialEditor", "New", 0));
        label_15->setText(QApplication::translate("MaterialEditor", "Texture:", 0));
        textureName->setText(QApplication::translate("MaterialEditor", "*Select a texture*", 0));
        selectTexture->setText(QApplication::translate("MaterialEditor", "Select", 0));
        removeTexture->setText(QApplication::translate("MaterialEditor", "Remove", 0));
    } // retranslateUi

};

namespace Ui {
    class MaterialEditor: public Ui_MaterialEditor {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_MATERIALEDITOR_H
