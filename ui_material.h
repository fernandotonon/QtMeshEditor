/********************************************************************************
** Form generated from reading UI file 'material.ui'
**
** Created by: Qt User Interface Compiler version 5.2.1
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_MATERIAL_H
#define UI_MATERIAL_H

#include <QtCore/QVariant>
#include <QtWidgets/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_Material
{
public:
    QWidget *centralwidget;
    QVBoxLayout *verticalLayout;
    QListWidget *listMaterial;
    QHBoxLayout *horizontalLayout;
    QSpacerItem *horizontalSpacer;
    QPushButton *buttonNew;
    QPushButton *pushButton;
    QPushButton *buttonEdit;
    QPushButton *buttonExport;
    QSpacerItem *horizontalSpacer_2;
    QStatusBar *statusbar;

    void setupUi(QMainWindow *Material)
    {
        if (Material->objectName().isEmpty())
            Material->setObjectName(QStringLiteral("Material"));
        Material->resize(600, 500);
        centralwidget = new QWidget(Material);
        centralwidget->setObjectName(QStringLiteral("centralwidget"));
        verticalLayout = new QVBoxLayout(centralwidget);
        verticalLayout->setObjectName(QStringLiteral("verticalLayout"));
        listMaterial = new QListWidget(centralwidget);
        listMaterial->setObjectName(QStringLiteral("listMaterial"));
        listMaterial->setSortingEnabled(true);

        verticalLayout->addWidget(listMaterial);

        horizontalLayout = new QHBoxLayout();
        horizontalLayout->setObjectName(QStringLiteral("horizontalLayout"));
        horizontalSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout->addItem(horizontalSpacer);

        buttonNew = new QPushButton(centralwidget);
        buttonNew->setObjectName(QStringLiteral("buttonNew"));

        horizontalLayout->addWidget(buttonNew);

        pushButton = new QPushButton(centralwidget);
        pushButton->setObjectName(QStringLiteral("pushButton"));

        horizontalLayout->addWidget(pushButton);

        buttonEdit = new QPushButton(centralwidget);
        buttonEdit->setObjectName(QStringLiteral("buttonEdit"));
        buttonEdit->setEnabled(false);

        horizontalLayout->addWidget(buttonEdit);

        buttonExport = new QPushButton(centralwidget);
        buttonExport->setObjectName(QStringLiteral("buttonExport"));
        buttonExport->setEnabled(false);

        horizontalLayout->addWidget(buttonExport);

        horizontalSpacer_2 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout->addItem(horizontalSpacer_2);


        verticalLayout->addLayout(horizontalLayout);

        Material->setCentralWidget(centralwidget);
        statusbar = new QStatusBar(Material);
        statusbar->setObjectName(QStringLiteral("statusbar"));
        Material->setStatusBar(statusbar);

        retranslateUi(Material);

        QMetaObject::connectSlotsByName(Material);
    } // setupUi

    void retranslateUi(QMainWindow *Material)
    {
        Material->setWindowTitle(QApplication::translate("Material", "MainWindow", 0));
        buttonNew->setText(QApplication::translate("Material", "New", 0));
        pushButton->setText(QApplication::translate("Material", "Import", 0));
        buttonEdit->setText(QApplication::translate("Material", "Edit", 0));
        buttonExport->setText(QApplication::translate("Material", "Export", 0));
    } // retranslateUi

};

namespace Ui {
    class Material: public Ui_Material {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_MATERIAL_H
