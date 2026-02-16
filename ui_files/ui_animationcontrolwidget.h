/********************************************************************************
** Form generated from reading UI file 'animationcontrolwidget.ui'
**
** Created by: Qt User Interface Compiler version 6.9.1
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_ANIMATIONCONTROLWIDGET_H
#define UI_ANIMATIONCONTROLWIDGET_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include "animationcontrolslider.h"

QT_BEGIN_NAMESPACE

class Ui_AnimationControlWidget
{
public:
    QWidget *dockWidgetContents;
    QVBoxLayout *verticalLayout_3;
    QHBoxLayout *horizontalLayout;
    QVBoxLayout *verticalLayout_7;
    QLabel *label_2;
    QTreeWidget *treeWidget;
    QVBoxLayout *verticalLayout_6;
    QLabel *label;
    QListWidget *boneList;
    QVBoxLayout *verticalLayout_4;
    QHBoxLayout *horizontalLayout_2;
    QLabel *minSliderLabel;
    AnimationControlSlider *horizontalSlider;
    QLabel *maxSliderLabel;
    QHBoxLayout *lengthLayout;
    QLabel *lengthLabel;
    QDoubleSpinBox *lengthSpinBox;
    QSpacerItem *lengthSpacer;
    QHBoxLayout *keyframeButtonsLayout;
    QSpacerItem *keyframeSpacer;
    QPushButton *prevKeyframeButton;
    QPushButton *nextKeyframeButton;
    QPushButton *addKeyframeButton;
    QPushButton *deleteKeyframeButton;
    QSpacerItem *keyframeSpacer2;
    QHBoxLayout *horizontalLayout_3;
    QSpacerItem *horizontalSpacer;
    QTableWidget *tableWidget;
    QSpacerItem *horizontalSpacer_2;
    QSpacerItem *verticalSpacer;

    void setupUi(QDockWidget *AnimationControlWidget)
    {
        if (AnimationControlWidget->objectName().isEmpty())
            AnimationControlWidget->setObjectName("AnimationControlWidget");
        AnimationControlWidget->resize(963, 306);
        AnimationControlWidget->setFloating(false);
        dockWidgetContents = new QWidget();
        dockWidgetContents->setObjectName("dockWidgetContents");
        dockWidgetContents->setEnabled(true);
        verticalLayout_3 = new QVBoxLayout(dockWidgetContents);
        verticalLayout_3->setObjectName("verticalLayout_3");
        horizontalLayout = new QHBoxLayout();
        horizontalLayout->setObjectName("horizontalLayout");
        verticalLayout_7 = new QVBoxLayout();
        verticalLayout_7->setObjectName("verticalLayout_7");
        verticalLayout_7->setContentsMargins(-1, -1, 10, -1);
        label_2 = new QLabel(dockWidgetContents);
        label_2->setObjectName("label_2");

        verticalLayout_7->addWidget(label_2);

        treeWidget = new QTreeWidget(dockWidgetContents);
        QTreeWidgetItem *__qtreewidgetitem = new QTreeWidgetItem();
        __qtreewidgetitem->setText(1, QString::fromUtf8("2"));
        __qtreewidgetitem->setText(0, QString::fromUtf8("1"));
        treeWidget->setHeaderItem(__qtreewidgetitem);
        treeWidget->setObjectName("treeWidget");
        treeWidget->setMaximumSize(QSize(400, 16777215));
        treeWidget->setColumnCount(2);
        treeWidget->header()->setVisible(false);
        treeWidget->header()->setDefaultSectionSize(400);

        verticalLayout_7->addWidget(treeWidget);


        horizontalLayout->addLayout(verticalLayout_7);

        verticalLayout_6 = new QVBoxLayout();
        verticalLayout_6->setObjectName("verticalLayout_6");
        verticalLayout_6->setContentsMargins(-1, -1, 0, -1);
        label = new QLabel(dockWidgetContents);
        label->setObjectName("label");

        verticalLayout_6->addWidget(label);

        boneList = new QListWidget(dockWidgetContents);
        boneList->setObjectName("boneList");
        boneList->setModelColumn(0);

        verticalLayout_6->addWidget(boneList);


        horizontalLayout->addLayout(verticalLayout_6);

        verticalLayout_4 = new QVBoxLayout();
        verticalLayout_4->setObjectName("verticalLayout_4");
        horizontalLayout_2 = new QHBoxLayout();
        horizontalLayout_2->setObjectName("horizontalLayout_2");
        horizontalLayout_2->setContentsMargins(-1, 0, -1, -1);
        minSliderLabel = new QLabel(dockWidgetContents);
        minSliderLabel->setObjectName("minSliderLabel");

        horizontalLayout_2->addWidget(minSliderLabel);

        horizontalSlider = new AnimationControlSlider(dockWidgetContents);
        horizontalSlider->setObjectName("horizontalSlider");
        horizontalSlider->setEnabled(false);
        horizontalSlider->setTracking(true);
        horizontalSlider->setOrientation(Qt::Horizontal);
        horizontalSlider->setInvertedAppearance(false);
        horizontalSlider->setInvertedControls(false);
        horizontalSlider->setTickPosition(QSlider::TicksBelow);
        horizontalSlider->setTickInterval(1);

        horizontalLayout_2->addWidget(horizontalSlider);

        maxSliderLabel = new QLabel(dockWidgetContents);
        maxSliderLabel->setObjectName("maxSliderLabel");

        horizontalLayout_2->addWidget(maxSliderLabel);


        verticalLayout_4->addLayout(horizontalLayout_2);

        lengthLayout = new QHBoxLayout();
        lengthLayout->setObjectName("lengthLayout");
        lengthLabel = new QLabel(dockWidgetContents);
        lengthLabel->setObjectName("lengthLabel");

        lengthLayout->addWidget(lengthLabel);

        lengthSpinBox = new QDoubleSpinBox(dockWidgetContents);
        lengthSpinBox->setObjectName("lengthSpinBox");
        lengthSpinBox->setEnabled(false);
        lengthSpinBox->setDecimals(3);
        lengthSpinBox->setMinimum(0.010000000000000);
        lengthSpinBox->setMaximum(999.000000000000000);
        lengthSpinBox->setSingleStep(0.100000000000000);

        lengthLayout->addWidget(lengthSpinBox);

        lengthSpacer = new QSpacerItem(40, 20, QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Minimum);

        lengthLayout->addItem(lengthSpacer);


        verticalLayout_4->addLayout(lengthLayout);

        keyframeButtonsLayout = new QHBoxLayout();
        keyframeButtonsLayout->setObjectName("keyframeButtonsLayout");
        keyframeSpacer = new QSpacerItem(40, 20, QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Minimum);

        keyframeButtonsLayout->addItem(keyframeSpacer);

        prevKeyframeButton = new QPushButton(dockWidgetContents);
        prevKeyframeButton->setObjectName("prevKeyframeButton");
        prevKeyframeButton->setEnabled(false);

        keyframeButtonsLayout->addWidget(prevKeyframeButton);

        nextKeyframeButton = new QPushButton(dockWidgetContents);
        nextKeyframeButton->setObjectName("nextKeyframeButton");
        nextKeyframeButton->setEnabled(false);

        keyframeButtonsLayout->addWidget(nextKeyframeButton);

        addKeyframeButton = new QPushButton(dockWidgetContents);
        addKeyframeButton->setObjectName("addKeyframeButton");

        keyframeButtonsLayout->addWidget(addKeyframeButton);

        deleteKeyframeButton = new QPushButton(dockWidgetContents);
        deleteKeyframeButton->setObjectName("deleteKeyframeButton");
        deleteKeyframeButton->setEnabled(false);

        keyframeButtonsLayout->addWidget(deleteKeyframeButton);

        keyframeSpacer2 = new QSpacerItem(40, 20, QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Minimum);

        keyframeButtonsLayout->addItem(keyframeSpacer2);


        verticalLayout_4->addLayout(keyframeButtonsLayout);

        horizontalLayout_3 = new QHBoxLayout();
        horizontalLayout_3->setObjectName("horizontalLayout_3");
        horizontalSpacer = new QSpacerItem(40, 20, QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Minimum);

        horizontalLayout_3->addItem(horizontalSpacer);

        tableWidget = new QTableWidget(dockWidgetContents);
        if (tableWidget->columnCount() < 4)
            tableWidget->setColumnCount(4);
        QTableWidgetItem *__qtablewidgetitem = new QTableWidgetItem();
        tableWidget->setHorizontalHeaderItem(0, __qtablewidgetitem);
        QTableWidgetItem *__qtablewidgetitem1 = new QTableWidgetItem();
        tableWidget->setHorizontalHeaderItem(1, __qtablewidgetitem1);
        QTableWidgetItem *__qtablewidgetitem2 = new QTableWidgetItem();
        tableWidget->setHorizontalHeaderItem(2, __qtablewidgetitem2);
        QTableWidgetItem *__qtablewidgetitem3 = new QTableWidgetItem();
        tableWidget->setHorizontalHeaderItem(3, __qtablewidgetitem3);
        if (tableWidget->rowCount() < 3)
            tableWidget->setRowCount(3);
        QTableWidgetItem *__qtablewidgetitem4 = new QTableWidgetItem();
        tableWidget->setVerticalHeaderItem(0, __qtablewidgetitem4);
        QTableWidgetItem *__qtablewidgetitem5 = new QTableWidgetItem();
        tableWidget->setVerticalHeaderItem(1, __qtablewidgetitem5);
        QTableWidgetItem *__qtablewidgetitem6 = new QTableWidgetItem();
        tableWidget->setVerticalHeaderItem(2, __qtablewidgetitem6);
        QTableWidgetItem *__qtablewidgetitem7 = new QTableWidgetItem();
        __qtablewidgetitem7->setFlags(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsEnabled);
        tableWidget->setItem(0, 0, __qtablewidgetitem7);
        QTableWidgetItem *__qtablewidgetitem8 = new QTableWidgetItem();
        __qtablewidgetitem8->setFlags(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsEnabled);
        tableWidget->setItem(0, 1, __qtablewidgetitem8);
        QTableWidgetItem *__qtablewidgetitem9 = new QTableWidgetItem();
        __qtablewidgetitem9->setFlags(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsEnabled);
        tableWidget->setItem(0, 2, __qtablewidgetitem9);
        QTableWidgetItem *__qtablewidgetitem10 = new QTableWidgetItem();
        __qtablewidgetitem10->setFlags(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsEnabled);
        tableWidget->setItem(0, 3, __qtablewidgetitem10);
        QTableWidgetItem *__qtablewidgetitem11 = new QTableWidgetItem();
        __qtablewidgetitem11->setFlags(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsEnabled);
        tableWidget->setItem(1, 0, __qtablewidgetitem11);
        QTableWidgetItem *__qtablewidgetitem12 = new QTableWidgetItem();
        __qtablewidgetitem12->setFlags(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsEnabled);
        tableWidget->setItem(1, 1, __qtablewidgetitem12);
        QTableWidgetItem *__qtablewidgetitem13 = new QTableWidgetItem();
        __qtablewidgetitem13->setFlags(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsEnabled);
        tableWidget->setItem(1, 2, __qtablewidgetitem13);
        QTableWidgetItem *__qtablewidgetitem14 = new QTableWidgetItem();
        __qtablewidgetitem14->setFlags(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsEnabled);
        tableWidget->setItem(1, 3, __qtablewidgetitem14);
        QTableWidgetItem *__qtablewidgetitem15 = new QTableWidgetItem();
        __qtablewidgetitem15->setFlags(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsEnabled);
        tableWidget->setItem(2, 0, __qtablewidgetitem15);
        QTableWidgetItem *__qtablewidgetitem16 = new QTableWidgetItem();
        __qtablewidgetitem16->setFlags(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsEnabled);
        tableWidget->setItem(2, 1, __qtablewidgetitem16);
        QTableWidgetItem *__qtablewidgetitem17 = new QTableWidgetItem();
        __qtablewidgetitem17->setFlags(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsEnabled);
        tableWidget->setItem(2, 2, __qtablewidgetitem17);
        QTableWidgetItem *__qtablewidgetitem18 = new QTableWidgetItem();
        __qtablewidgetitem18->setFlags(Qt::ItemIsSelectable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsEnabled);
        tableWidget->setItem(2, 3, __qtablewidgetitem18);
        tableWidget->setObjectName("tableWidget");
        tableWidget->setEnabled(true);
        QSizePolicy sizePolicy(QSizePolicy::Policy::Fixed, QSizePolicy::Policy::Fixed);
        sizePolicy.setHorizontalStretch(0);
        sizePolicy.setVerticalStretch(0);
        sizePolicy.setHeightForWidth(tableWidget->sizePolicy().hasHeightForWidth());
        tableWidget->setSizePolicy(sizePolicy);
        tableWidget->setMinimumSize(QSize(430, 110));
        tableWidget->setMaximumSize(QSize(430, 110));
        tableWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        tableWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        tableWidget->setProperty("showDropIndicator", QVariant(false));
        tableWidget->setDragDropOverwriteMode(false);
        tableWidget->horizontalHeader()->setDefaultSectionSize(90);

        horizontalLayout_3->addWidget(tableWidget);

        horizontalSpacer_2 = new QSpacerItem(40, 20, QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Minimum);

        horizontalLayout_3->addItem(horizontalSpacer_2);


        verticalLayout_4->addLayout(horizontalLayout_3);

        verticalSpacer = new QSpacerItem(20, 40, QSizePolicy::Policy::Minimum, QSizePolicy::Policy::Expanding);

        verticalLayout_4->addItem(verticalSpacer);


        horizontalLayout->addLayout(verticalLayout_4);


        verticalLayout_3->addLayout(horizontalLayout);

        AnimationControlWidget->setWidget(dockWidgetContents);

        retranslateUi(AnimationControlWidget);

        QMetaObject::connectSlotsByName(AnimationControlWidget);
    } // setupUi

    void retranslateUi(QDockWidget *AnimationControlWidget)
    {
        AnimationControlWidget->setWindowTitle(QCoreApplication::translate("AnimationControlWidget", "Animation Control", nullptr));
        label_2->setText(QCoreApplication::translate("AnimationControlWidget", "Select Animation", nullptr));
        label->setText(QCoreApplication::translate("AnimationControlWidget", "Bones", nullptr));
        minSliderLabel->setText(QCoreApplication::translate("AnimationControlWidget", "0", nullptr));
        maxSliderLabel->setText(QCoreApplication::translate("AnimationControlWidget", "--", nullptr));
        lengthLabel->setText(QCoreApplication::translate("AnimationControlWidget", "Length:", nullptr));
        lengthSpinBox->setSuffix(QCoreApplication::translate("AnimationControlWidget", " s", nullptr));
        prevKeyframeButton->setText(QCoreApplication::translate("AnimationControlWidget", "|<", nullptr));
        nextKeyframeButton->setText(QCoreApplication::translate("AnimationControlWidget", ">|", nullptr));
        addKeyframeButton->setText(QCoreApplication::translate("AnimationControlWidget", "+ Keyframe", nullptr));
        deleteKeyframeButton->setText(QCoreApplication::translate("AnimationControlWidget", "- Keyframe", nullptr));
        QTableWidgetItem *___qtablewidgetitem = tableWidget->horizontalHeaderItem(0);
        ___qtablewidgetitem->setText(QCoreApplication::translate("AnimationControlWidget", "w", nullptr));
        QTableWidgetItem *___qtablewidgetitem1 = tableWidget->horizontalHeaderItem(1);
        ___qtablewidgetitem1->setText(QCoreApplication::translate("AnimationControlWidget", "x", nullptr));
        QTableWidgetItem *___qtablewidgetitem2 = tableWidget->horizontalHeaderItem(2);
        ___qtablewidgetitem2->setText(QCoreApplication::translate("AnimationControlWidget", "y", nullptr));
        QTableWidgetItem *___qtablewidgetitem3 = tableWidget->horizontalHeaderItem(3);
        ___qtablewidgetitem3->setText(QCoreApplication::translate("AnimationControlWidget", "z", nullptr));
        QTableWidgetItem *___qtablewidgetitem4 = tableWidget->verticalHeaderItem(0);
        ___qtablewidgetitem4->setText(QCoreApplication::translate("AnimationControlWidget", "Translation", nullptr));
        QTableWidgetItem *___qtablewidgetitem5 = tableWidget->verticalHeaderItem(1);
        ___qtablewidgetitem5->setText(QCoreApplication::translate("AnimationControlWidget", "Scale", nullptr));
        QTableWidgetItem *___qtablewidgetitem6 = tableWidget->verticalHeaderItem(2);
        ___qtablewidgetitem6->setText(QCoreApplication::translate("AnimationControlWidget", "Rotation", nullptr));

        const bool __sortingEnabled = tableWidget->isSortingEnabled();
        tableWidget->setSortingEnabled(false);
        QTableWidgetItem *___qtablewidgetitem7 = tableWidget->item(0, 0);
        ___qtablewidgetitem7->setText(QCoreApplication::translate("AnimationControlWidget", "-", nullptr));
        QTableWidgetItem *___qtablewidgetitem8 = tableWidget->item(0, 1);
        ___qtablewidgetitem8->setText(QCoreApplication::translate("AnimationControlWidget", "0", nullptr));
        QTableWidgetItem *___qtablewidgetitem9 = tableWidget->item(0, 2);
        ___qtablewidgetitem9->setText(QCoreApplication::translate("AnimationControlWidget", "0", nullptr));
        QTableWidgetItem *___qtablewidgetitem10 = tableWidget->item(0, 3);
        ___qtablewidgetitem10->setText(QCoreApplication::translate("AnimationControlWidget", "0", nullptr));
        QTableWidgetItem *___qtablewidgetitem11 = tableWidget->item(1, 0);
        ___qtablewidgetitem11->setText(QCoreApplication::translate("AnimationControlWidget", "-", nullptr));
        QTableWidgetItem *___qtablewidgetitem12 = tableWidget->item(1, 1);
        ___qtablewidgetitem12->setText(QCoreApplication::translate("AnimationControlWidget", "0", nullptr));
        QTableWidgetItem *___qtablewidgetitem13 = tableWidget->item(1, 2);
        ___qtablewidgetitem13->setText(QCoreApplication::translate("AnimationControlWidget", "0", nullptr));
        QTableWidgetItem *___qtablewidgetitem14 = tableWidget->item(1, 3);
        ___qtablewidgetitem14->setText(QCoreApplication::translate("AnimationControlWidget", "0", nullptr));
        QTableWidgetItem *___qtablewidgetitem15 = tableWidget->item(2, 0);
        ___qtablewidgetitem15->setText(QCoreApplication::translate("AnimationControlWidget", "0", nullptr));
        QTableWidgetItem *___qtablewidgetitem16 = tableWidget->item(2, 1);
        ___qtablewidgetitem16->setText(QCoreApplication::translate("AnimationControlWidget", "0", nullptr));
        QTableWidgetItem *___qtablewidgetitem17 = tableWidget->item(2, 2);
        ___qtablewidgetitem17->setText(QCoreApplication::translate("AnimationControlWidget", "0", nullptr));
        QTableWidgetItem *___qtablewidgetitem18 = tableWidget->item(2, 3);
        ___qtablewidgetitem18->setText(QCoreApplication::translate("AnimationControlWidget", "0", nullptr));
        tableWidget->setSortingEnabled(__sortingEnabled);

    } // retranslateUi

};

namespace Ui {
    class AnimationControlWidget: public Ui_AnimationControlWidget {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_ANIMATIONCONTROLWIDGET_H
