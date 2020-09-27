/********************************************************************************
** Form generated from reading UI file 'addprimitives.ui'
**
** Created by: Qt User Interface Compiler version 5.2.1
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_ADDPRIMITIVES_H
#define UI_ADDPRIMITIVES_H

#include <QtCore/QVariant>
#include <QtWidgets/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

QT_BEGIN_NAMESPACE

class Ui_AddPrimitives
{
public:
    QVBoxLayout *verticalLayout;
    QHBoxLayout *horizontalLayout_2;
    QPushButton *Cube;
    QPushButton *Sphere;
    QPushButton *Plane;
    QHBoxLayout *horizontalLayout_3;
    QDialogButtonBox *buttonBox;

    void setupUi(QDialog *AddPrimitives)
    {
        if (AddPrimitives->objectName().isEmpty())
            AddPrimitives->setObjectName(QStringLiteral("AddPrimitives"));
        AddPrimitives->setWindowModality(Qt::WindowModal);
        AddPrimitives->resize(257, 74);
        AddPrimitives->setModal(true);
        verticalLayout = new QVBoxLayout(AddPrimitives);
        verticalLayout->setObjectName(QStringLiteral("verticalLayout"));
        horizontalLayout_2 = new QHBoxLayout();
        horizontalLayout_2->setObjectName(QStringLiteral("horizontalLayout_2"));
        Cube = new QPushButton(AddPrimitives);
        Cube->setObjectName(QStringLiteral("Cube"));
        QIcon icon;
        icon.addFile(QStringLiteral(":/icones/cube.png"), QSize(), QIcon::Normal, QIcon::Off);
        Cube->setIcon(icon);

        horizontalLayout_2->addWidget(Cube);

        Sphere = new QPushButton(AddPrimitives);
        Sphere->setObjectName(QStringLiteral("Sphere"));
        QIcon icon1;
        icon1.addFile(QStringLiteral(":/icones/sphere.png"), QSize(), QIcon::Normal, QIcon::Off);
        Sphere->setIcon(icon1);

        horizontalLayout_2->addWidget(Sphere);

        Plane = new QPushButton(AddPrimitives);
        Plane->setObjectName(QStringLiteral("Plane"));
        QIcon icon2;
        icon2.addFile(QStringLiteral(":/icones/square.png"), QSize(), QIcon::Normal, QIcon::Off);
        Plane->setIcon(icon2);

        horizontalLayout_2->addWidget(Plane);


        verticalLayout->addLayout(horizontalLayout_2);

        horizontalLayout_3 = new QHBoxLayout();
        horizontalLayout_3->setObjectName(QStringLiteral("horizontalLayout_3"));
        buttonBox = new QDialogButtonBox(AddPrimitives);
        buttonBox->setObjectName(QStringLiteral("buttonBox"));
        buttonBox->setStandardButtons(QDialogButtonBox::Cancel);
        buttonBox->setCenterButtons(true);

        horizontalLayout_3->addWidget(buttonBox);


        verticalLayout->addLayout(horizontalLayout_3);


        retranslateUi(AddPrimitives);

        QMetaObject::connectSlotsByName(AddPrimitives);
    } // setupUi

    void retranslateUi(QDialog *AddPrimitives)
    {
        AddPrimitives->setWindowTitle(QApplication::translate("AddPrimitives", "Add Primitives", 0));
        Cube->setText(QApplication::translate("AddPrimitives", "Cube", 0));
        Sphere->setText(QApplication::translate("AddPrimitives", "Sphere", 0));
        Plane->setText(QApplication::translate("AddPrimitives", "Plane", 0));
    } // retranslateUi

};

namespace Ui {
    class AddPrimitives: public Ui_AddPrimitives {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_ADDPRIMITIVES_H
