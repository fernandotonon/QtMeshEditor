/********************************************************************************
** Form generated from reading UI file 'mainwindow.ui'
**
** Created by: Qt User Interface Compiler version 5.2.1
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_MAINWINDOW_H
#define UI_MAINWINDOW_H

#include <QtCore/QVariant>
#include <QtWidgets/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_MainWindow
{
public:
    QAction *actionShow_Grid;
    QAction *actionImport;
    QAction *actionAdd_Primitive;
    QAction *actionMaterial_Editor;
    QAction *actionAbout;
    QAction *actionObjects_Toolbar;
    QAction *actionTools_Toolbar;
    QAction *actionMeshEditor;
    QAction *actionMove_Object;
    QAction *actionRotate_Object;
    QAction *actionSelect_Object;
    QAction *actionRemove_Object;
    QAction *actionExport_Selected;
    QWidget *centralWidget;
    QMenuBar *menuBar;
    QMenu *menuOp_es;
    QMenu *menuView;
    QMenu *menuFile;
    QMenu *menuHelp;
    QToolBar *objectsToolbar;
    QToolBar *toolToolbar;
    QDockWidget *meshEditorWidget;
    QWidget *dockWidgetContents;
    QVBoxLayout *verticalLayout;
    QTabWidget *tabWidget;
    QWidget *tab;
    QVBoxLayout *verticalLayout_2;
    QHBoxLayout *horizontalLayout;
    QLabel *label;
    QComboBox *MeshComboBox;
    QHBoxLayout *horizontalLayout_7;
    QLabel *label_15;
    QComboBox *SubMeshComboBox;
    QHBoxLayout *horizontalLayout_3;
    QLabel *label_2;
    QComboBox *MaterialComboBox;
    QHBoxLayout *horizontalLayout_4;
    QLabel *label_3;
    QLabel *label_4;
    QLineEdit *meshPositionX;
    QLabel *label_5;
    QLineEdit *meshPositionY;
    QLabel *label_6;
    QLineEdit *meshPositionZ;
    QSpacerItem *horizontalSpacer_3;
    QHBoxLayout *horizontalLayout_6;
    QLabel *label_11;
    QLabel *label_13;
    QLineEdit *meshRotateX;
    QLabel *label_12;
    QLineEdit *meshRotateY;
    QLabel *label_14;
    QLineEdit *meshRotateZ;
    QHBoxLayout *horizontalLayout_5;
    QLabel *label_7;
    QLabel *label_10;
    QLineEdit *meshSizeX;
    QLabel *label_9;
    QLineEdit *meshSizeY;
    QLabel *label_8;
    QLineEdit *meshSizeZ;
    QSpacerItem *verticalSpacer_2;
    QWidget *tab_2;
    QVBoxLayout *verticalLayout_3;
    QHBoxLayout *horizontalLayout_8;
    QLabel *label_16;
    QComboBox *AnimationComboBox;
    QPushButton *renameButton;
    QHBoxLayout *horizontalLayout_10;
    QCheckBox *checkBoxAnimationEnabled;
    QCheckBox *checkBoxAnimationLoop;
    QVBoxLayout *verticalLayout_4;
    QCheckBox *checkBoxDisplaySkeleton;
    QHBoxLayout *horizontalLayout_11;
    QSpacerItem *horizontalSpacer_4;
    QPushButton *PlayPauseButton;
    QSpacerItem *horizontalSpacer_5;
    QSpacerItem *verticalSpacer_3;
    QSpacerItem *verticalSpacer;

    void setupUi(QMainWindow *MainWindow)
    {
        if (MainWindow->objectName().isEmpty())
            MainWindow->setObjectName(QStringLiteral("MainWindow"));
        MainWindow->resize(800, 500);
        MainWindow->setMouseTracking(true);
        MainWindow->setContextMenuPolicy(Qt::NoContextMenu);
        MainWindow->setAcceptDrops(true);
        QIcon icon;
        icon.addFile(QStringLiteral(":/icones/icone.ico"), QSize(), QIcon::Normal, QIcon::Off);
        MainWindow->setWindowIcon(icon);
        MainWindow->setDocumentMode(false);
        actionShow_Grid = new QAction(MainWindow);
        actionShow_Grid->setObjectName(QStringLiteral("actionShow_Grid"));
        actionShow_Grid->setCheckable(true);
        actionShow_Grid->setChecked(true);
        actionImport = new QAction(MainWindow);
        actionImport->setObjectName(QStringLiteral("actionImport"));
        actionAdd_Primitive = new QAction(MainWindow);
        actionAdd_Primitive->setObjectName(QStringLiteral("actionAdd_Primitive"));
        QIcon icon1;
        icon1.addFile(QStringLiteral(":/icones/cube.png"), QSize(), QIcon::Normal, QIcon::Off);
        actionAdd_Primitive->setIcon(icon1);
        actionMaterial_Editor = new QAction(MainWindow);
        actionMaterial_Editor->setObjectName(QStringLiteral("actionMaterial_Editor"));
        actionAbout = new QAction(MainWindow);
        actionAbout->setObjectName(QStringLiteral("actionAbout"));
        actionObjects_Toolbar = new QAction(MainWindow);
        actionObjects_Toolbar->setObjectName(QStringLiteral("actionObjects_Toolbar"));
        actionObjects_Toolbar->setCheckable(true);
        actionObjects_Toolbar->setChecked(true);
        actionTools_Toolbar = new QAction(MainWindow);
        actionTools_Toolbar->setObjectName(QStringLiteral("actionTools_Toolbar"));
        actionTools_Toolbar->setCheckable(true);
        actionTools_Toolbar->setChecked(true);
        actionMeshEditor = new QAction(MainWindow);
        actionMeshEditor->setObjectName(QStringLiteral("actionMeshEditor"));
        actionMeshEditor->setCheckable(true);
        actionMeshEditor->setChecked(true);
        actionMove_Object = new QAction(MainWindow);
        actionMove_Object->setObjectName(QStringLiteral("actionMove_Object"));
        actionMove_Object->setCheckable(true);
        QIcon icon2;
        icon2.addFile(QStringLiteral(":/icones/move.png"), QSize(), QIcon::Normal, QIcon::Off);
        actionMove_Object->setIcon(icon2);
        actionMove_Object->setAutoRepeat(true);
        actionRotate_Object = new QAction(MainWindow);
        actionRotate_Object->setObjectName(QStringLiteral("actionRotate_Object"));
        actionRotate_Object->setCheckable(true);
        QIcon icon3;
        icon3.addFile(QStringLiteral(":/icones/rotacionar.png"), QSize(), QIcon::Normal, QIcon::Off);
        actionRotate_Object->setIcon(icon3);
        actionSelect_Object = new QAction(MainWindow);
        actionSelect_Object->setObjectName(QStringLiteral("actionSelect_Object"));
        actionSelect_Object->setCheckable(true);
        QIcon icon4;
        icon4.addFile(QStringLiteral(":/icones/selecionar.png"), QSize(), QIcon::Normal, QIcon::Off);
        actionSelect_Object->setIcon(icon4);
        actionRemove_Object = new QAction(MainWindow);
        actionRemove_Object->setObjectName(QStringLiteral("actionRemove_Object"));
        QIcon icon5;
        icon5.addFile(QStringLiteral(":/icones/remove.png"), QSize(), QIcon::Normal, QIcon::Off);
        actionRemove_Object->setIcon(icon5);
        actionExport_Selected = new QAction(MainWindow);
        actionExport_Selected->setObjectName(QStringLiteral("actionExport_Selected"));
        centralWidget = new QWidget(MainWindow);
        centralWidget->setObjectName(QStringLiteral("centralWidget"));
        centralWidget->setMouseTracking(true);
        centralWidget->setFocusPolicy(Qt::StrongFocus);
        centralWidget->setAcceptDrops(true);
        MainWindow->setCentralWidget(centralWidget);
        menuBar = new QMenuBar(MainWindow);
        menuBar->setObjectName(QStringLiteral("menuBar"));
        menuBar->setGeometry(QRect(0, 0, 800, 21));
        menuBar->setMouseTracking(false);
        menuOp_es = new QMenu(menuBar);
        menuOp_es->setObjectName(QStringLiteral("menuOp_es"));
        menuView = new QMenu(menuOp_es);
        menuView->setObjectName(QStringLiteral("menuView"));
        menuFile = new QMenu(menuBar);
        menuFile->setObjectName(QStringLiteral("menuFile"));
        menuHelp = new QMenu(menuBar);
        menuHelp->setObjectName(QStringLiteral("menuHelp"));
        MainWindow->setMenuBar(menuBar);
        objectsToolbar = new QToolBar(MainWindow);
        objectsToolbar->setObjectName(QStringLiteral("objectsToolbar"));
        objectsToolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        MainWindow->addToolBar(Qt::LeftToolBarArea, objectsToolbar);
        toolToolbar = new QToolBar(MainWindow);
        toolToolbar->setObjectName(QStringLiteral("toolToolbar"));
        MainWindow->addToolBar(Qt::TopToolBarArea, toolToolbar);
        meshEditorWidget = new QDockWidget(MainWindow);
        meshEditorWidget->setObjectName(QStringLiteral("meshEditorWidget"));
        QSizePolicy sizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        sizePolicy.setHorizontalStretch(0);
        sizePolicy.setVerticalStretch(0);
        sizePolicy.setHeightForWidth(meshEditorWidget->sizePolicy().hasHeightForWidth());
        meshEditorWidget->setSizePolicy(sizePolicy);
        meshEditorWidget->setMinimumSize(QSize(300, 315));
        meshEditorWidget->setMaximumSize(QSize(300, 524287));
        meshEditorWidget->setMouseTracking(false);
        meshEditorWidget->setFloating(false);
        meshEditorWidget->setFeatures(QDockWidget::DockWidgetFloatable);
        meshEditorWidget->setAllowedAreas(Qt::LeftDockWidgetArea|Qt::RightDockWidgetArea);
        dockWidgetContents = new QWidget();
        dockWidgetContents->setObjectName(QStringLiteral("dockWidgetContents"));
        sizePolicy.setHeightForWidth(dockWidgetContents->sizePolicy().hasHeightForWidth());
        dockWidgetContents->setSizePolicy(sizePolicy);
        dockWidgetContents->setMouseTracking(false);
        verticalLayout = new QVBoxLayout(dockWidgetContents);
        verticalLayout->setSpacing(6);
        verticalLayout->setContentsMargins(11, 11, 11, 11);
        verticalLayout->setObjectName(QStringLiteral("verticalLayout"));
        tabWidget = new QTabWidget(dockWidgetContents);
        tabWidget->setObjectName(QStringLiteral("tabWidget"));
        tab = new QWidget();
        tab->setObjectName(QStringLiteral("tab"));
        verticalLayout_2 = new QVBoxLayout(tab);
        verticalLayout_2->setSpacing(6);
        verticalLayout_2->setContentsMargins(11, 11, 11, 11);
        verticalLayout_2->setObjectName(QStringLiteral("verticalLayout_2"));
        horizontalLayout = new QHBoxLayout();
        horizontalLayout->setSpacing(6);
        horizontalLayout->setObjectName(QStringLiteral("horizontalLayout"));
        label = new QLabel(tab);
        label->setObjectName(QStringLiteral("label"));
        QSizePolicy sizePolicy1(QSizePolicy::Fixed, QSizePolicy::Fixed);
        sizePolicy1.setHorizontalStretch(0);
        sizePolicy1.setVerticalStretch(0);
        sizePolicy1.setHeightForWidth(label->sizePolicy().hasHeightForWidth());
        label->setSizePolicy(sizePolicy1);

        horizontalLayout->addWidget(label);

        MeshComboBox = new QComboBox(tab);
        MeshComboBox->setObjectName(QStringLiteral("MeshComboBox"));
        QSizePolicy sizePolicy2(QSizePolicy::Expanding, QSizePolicy::Fixed);
        sizePolicy2.setHorizontalStretch(0);
        sizePolicy2.setVerticalStretch(0);
        sizePolicy2.setHeightForWidth(MeshComboBox->sizePolicy().hasHeightForWidth());
        MeshComboBox->setSizePolicy(sizePolicy2);
        MeshComboBox->setBaseSize(QSize(100, 0));
        MeshComboBox->setFocusPolicy(Qt::NoFocus);
        MeshComboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        MeshComboBox->setDuplicatesEnabled(true);

        horizontalLayout->addWidget(MeshComboBox);


        verticalLayout_2->addLayout(horizontalLayout);

        horizontalLayout_7 = new QHBoxLayout();
        horizontalLayout_7->setSpacing(6);
        horizontalLayout_7->setObjectName(QStringLiteral("horizontalLayout_7"));
        label_15 = new QLabel(tab);
        label_15->setObjectName(QStringLiteral("label_15"));

        horizontalLayout_7->addWidget(label_15);

        SubMeshComboBox = new QComboBox(tab);
        SubMeshComboBox->setObjectName(QStringLiteral("SubMeshComboBox"));
        sizePolicy2.setHeightForWidth(SubMeshComboBox->sizePolicy().hasHeightForWidth());
        SubMeshComboBox->setSizePolicy(sizePolicy2);

        horizontalLayout_7->addWidget(SubMeshComboBox);


        verticalLayout_2->addLayout(horizontalLayout_7);

        horizontalLayout_3 = new QHBoxLayout();
        horizontalLayout_3->setSpacing(6);
        horizontalLayout_3->setObjectName(QStringLiteral("horizontalLayout_3"));
        label_2 = new QLabel(tab);
        label_2->setObjectName(QStringLiteral("label_2"));
        sizePolicy1.setHeightForWidth(label_2->sizePolicy().hasHeightForWidth());
        label_2->setSizePolicy(sizePolicy1);

        horizontalLayout_3->addWidget(label_2);

        MaterialComboBox = new QComboBox(tab);
        MaterialComboBox->setObjectName(QStringLiteral("MaterialComboBox"));
        MaterialComboBox->setEnabled(false);
        sizePolicy2.setHeightForWidth(MaterialComboBox->sizePolicy().hasHeightForWidth());
        MaterialComboBox->setSizePolicy(sizePolicy2);
        MaterialComboBox->setFocusPolicy(Qt::NoFocus);
        MaterialComboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        MaterialComboBox->setDuplicatesEnabled(true);

        horizontalLayout_3->addWidget(MaterialComboBox);


        verticalLayout_2->addLayout(horizontalLayout_3);

        horizontalLayout_4 = new QHBoxLayout();
        horizontalLayout_4->setSpacing(6);
        horizontalLayout_4->setObjectName(QStringLiteral("horizontalLayout_4"));
        label_3 = new QLabel(tab);
        label_3->setObjectName(QStringLiteral("label_3"));

        horizontalLayout_4->addWidget(label_3);

        label_4 = new QLabel(tab);
        label_4->setObjectName(QStringLiteral("label_4"));

        horizontalLayout_4->addWidget(label_4);

        meshPositionX = new QLineEdit(tab);
        meshPositionX->setObjectName(QStringLiteral("meshPositionX"));
        meshPositionX->setMaxLength(20);

        horizontalLayout_4->addWidget(meshPositionX);

        label_5 = new QLabel(tab);
        label_5->setObjectName(QStringLiteral("label_5"));

        horizontalLayout_4->addWidget(label_5);

        meshPositionY = new QLineEdit(tab);
        meshPositionY->setObjectName(QStringLiteral("meshPositionY"));
        meshPositionY->setMaxLength(20);

        horizontalLayout_4->addWidget(meshPositionY);

        label_6 = new QLabel(tab);
        label_6->setObjectName(QStringLiteral("label_6"));

        horizontalLayout_4->addWidget(label_6);

        meshPositionZ = new QLineEdit(tab);
        meshPositionZ->setObjectName(QStringLiteral("meshPositionZ"));
        meshPositionZ->setMaxLength(20);

        horizontalLayout_4->addWidget(meshPositionZ);

        horizontalSpacer_3 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_4->addItem(horizontalSpacer_3);


        verticalLayout_2->addLayout(horizontalLayout_4);

        horizontalLayout_6 = new QHBoxLayout();
        horizontalLayout_6->setSpacing(6);
        horizontalLayout_6->setObjectName(QStringLiteral("horizontalLayout_6"));
        label_11 = new QLabel(tab);
        label_11->setObjectName(QStringLiteral("label_11"));

        horizontalLayout_6->addWidget(label_11);

        label_13 = new QLabel(tab);
        label_13->setObjectName(QStringLiteral("label_13"));

        horizontalLayout_6->addWidget(label_13);

        meshRotateX = new QLineEdit(tab);
        meshRotateX->setObjectName(QStringLiteral("meshRotateX"));

        horizontalLayout_6->addWidget(meshRotateX);

        label_12 = new QLabel(tab);
        label_12->setObjectName(QStringLiteral("label_12"));

        horizontalLayout_6->addWidget(label_12);

        meshRotateY = new QLineEdit(tab);
        meshRotateY->setObjectName(QStringLiteral("meshRotateY"));

        horizontalLayout_6->addWidget(meshRotateY);

        label_14 = new QLabel(tab);
        label_14->setObjectName(QStringLiteral("label_14"));

        horizontalLayout_6->addWidget(label_14);

        meshRotateZ = new QLineEdit(tab);
        meshRotateZ->setObjectName(QStringLiteral("meshRotateZ"));

        horizontalLayout_6->addWidget(meshRotateZ);


        verticalLayout_2->addLayout(horizontalLayout_6);

        horizontalLayout_5 = new QHBoxLayout();
        horizontalLayout_5->setSpacing(6);
        horizontalLayout_5->setObjectName(QStringLiteral("horizontalLayout_5"));
        label_7 = new QLabel(tab);
        label_7->setObjectName(QStringLiteral("label_7"));

        horizontalLayout_5->addWidget(label_7);

        label_10 = new QLabel(tab);
        label_10->setObjectName(QStringLiteral("label_10"));

        horizontalLayout_5->addWidget(label_10);

        meshSizeX = new QLineEdit(tab);
        meshSizeX->setObjectName(QStringLiteral("meshSizeX"));
        meshSizeX->setMaxLength(20);

        horizontalLayout_5->addWidget(meshSizeX);

        label_9 = new QLabel(tab);
        label_9->setObjectName(QStringLiteral("label_9"));

        horizontalLayout_5->addWidget(label_9);

        meshSizeY = new QLineEdit(tab);
        meshSizeY->setObjectName(QStringLiteral("meshSizeY"));
        meshSizeY->setMaxLength(20);

        horizontalLayout_5->addWidget(meshSizeY);

        label_8 = new QLabel(tab);
        label_8->setObjectName(QStringLiteral("label_8"));

        horizontalLayout_5->addWidget(label_8);

        meshSizeZ = new QLineEdit(tab);
        meshSizeZ->setObjectName(QStringLiteral("meshSizeZ"));
        meshSizeZ->setMaxLength(20);

        horizontalLayout_5->addWidget(meshSizeZ);


        verticalLayout_2->addLayout(horizontalLayout_5);

        verticalSpacer_2 = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);

        verticalLayout_2->addItem(verticalSpacer_2);

        tabWidget->addTab(tab, QString());
        tab_2 = new QWidget();
        tab_2->setObjectName(QStringLiteral("tab_2"));
        verticalLayout_3 = new QVBoxLayout(tab_2);
        verticalLayout_3->setSpacing(6);
        verticalLayout_3->setContentsMargins(11, 11, 11, 11);
        verticalLayout_3->setObjectName(QStringLiteral("verticalLayout_3"));
        horizontalLayout_8 = new QHBoxLayout();
        horizontalLayout_8->setSpacing(6);
        horizontalLayout_8->setObjectName(QStringLiteral("horizontalLayout_8"));
        label_16 = new QLabel(tab_2);
        label_16->setObjectName(QStringLiteral("label_16"));
        sizePolicy1.setHeightForWidth(label_16->sizePolicy().hasHeightForWidth());
        label_16->setSizePolicy(sizePolicy1);

        horizontalLayout_8->addWidget(label_16);

        AnimationComboBox = new QComboBox(tab_2);
        AnimationComboBox->setObjectName(QStringLiteral("AnimationComboBox"));

        horizontalLayout_8->addWidget(AnimationComboBox);

        renameButton = new QPushButton(tab_2);
        renameButton->setObjectName(QStringLiteral("renameButton"));
        renameButton->setMaximumSize(QSize(50, 16777215));

        horizontalLayout_8->addWidget(renameButton);


        verticalLayout_3->addLayout(horizontalLayout_8);

        horizontalLayout_10 = new QHBoxLayout();
        horizontalLayout_10->setSpacing(6);
        horizontalLayout_10->setObjectName(QStringLiteral("horizontalLayout_10"));
        horizontalLayout_10->setContentsMargins(-1, 0, -1, -1);
        checkBoxAnimationEnabled = new QCheckBox(tab_2);
        checkBoxAnimationEnabled->setObjectName(QStringLiteral("checkBoxAnimationEnabled"));

        horizontalLayout_10->addWidget(checkBoxAnimationEnabled);

        checkBoxAnimationLoop = new QCheckBox(tab_2);
        checkBoxAnimationLoop->setObjectName(QStringLiteral("checkBoxAnimationLoop"));

        horizontalLayout_10->addWidget(checkBoxAnimationLoop);


        verticalLayout_3->addLayout(horizontalLayout_10);

        verticalLayout_4 = new QVBoxLayout();
        verticalLayout_4->setSpacing(6);
        verticalLayout_4->setObjectName(QStringLiteral("verticalLayout_4"));
        verticalLayout_4->setContentsMargins(-1, -1, -1, 0);
        checkBoxDisplaySkeleton = new QCheckBox(tab_2);
        checkBoxDisplaySkeleton->setObjectName(QStringLiteral("checkBoxDisplaySkeleton"));

        verticalLayout_4->addWidget(checkBoxDisplaySkeleton);


        verticalLayout_3->addLayout(verticalLayout_4);

        horizontalLayout_11 = new QHBoxLayout();
        horizontalLayout_11->setSpacing(6);
        horizontalLayout_11->setObjectName(QStringLiteral("horizontalLayout_11"));
        horizontalLayout_11->setContentsMargins(-1, 0, -1, -1);
        horizontalSpacer_4 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_11->addItem(horizontalSpacer_4);

        PlayPauseButton = new QPushButton(tab_2);
        PlayPauseButton->setObjectName(QStringLiteral("PlayPauseButton"));
        PlayPauseButton->setMinimumSize(QSize(100, 0));
        QIcon icon6;
        icon6.addFile(QStringLiteral(":/icones/play.png"), QSize(), QIcon::Normal, QIcon::Off);
        PlayPauseButton->setIcon(icon6);
        PlayPauseButton->setIconSize(QSize(20, 20));

        horizontalLayout_11->addWidget(PlayPauseButton);

        horizontalSpacer_5 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_11->addItem(horizontalSpacer_5);


        verticalLayout_3->addLayout(horizontalLayout_11);

        verticalSpacer_3 = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);

        verticalLayout_3->addItem(verticalSpacer_3);

        tabWidget->addTab(tab_2, QString());

        verticalLayout->addWidget(tabWidget);

        verticalSpacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);

        verticalLayout->addItem(verticalSpacer);

        meshEditorWidget->setWidget(dockWidgetContents);
        MainWindow->addDockWidget(static_cast<Qt::DockWidgetArea>(2), meshEditorWidget);

        menuBar->addAction(menuFile->menuAction());
        menuBar->addAction(menuOp_es->menuAction());
        menuBar->addAction(menuHelp->menuAction());
        menuOp_es->addAction(actionShow_Grid);
        menuOp_es->addAction(menuView->menuAction());
        menuView->addAction(actionObjects_Toolbar);
        menuView->addAction(actionTools_Toolbar);
        menuView->addAction(actionMeshEditor);
        menuFile->addAction(actionImport);
        menuFile->addAction(actionExport_Selected);
        menuHelp->addAction(actionAbout);
        objectsToolbar->addAction(actionAdd_Primitive);
        toolToolbar->addAction(actionMove_Object);
        toolToolbar->addAction(actionRotate_Object);
        toolToolbar->addAction(actionSelect_Object);
        toolToolbar->addAction(actionRemove_Object);
        toolToolbar->addAction(actionMaterial_Editor);

        retranslateUi(MainWindow);

        tabWidget->setCurrentIndex(0);
        MeshComboBox->setCurrentIndex(-1);


        QMetaObject::connectSlotsByName(MainWindow);
    } // setupUi

    void retranslateUi(QMainWindow *MainWindow)
    {
        MainWindow->setWindowTitle(QApplication::translate("MainWindow", "QtMeshEditor", 0));
        actionShow_Grid->setText(QApplication::translate("MainWindow", "Show Grid", 0));
        actionImport->setText(QApplication::translate("MainWindow", "Import Mesh", 0));
        actionAdd_Primitive->setText(QApplication::translate("MainWindow", "Add Primitive", 0));
#ifndef QT_NO_TOOLTIP
        actionAdd_Primitive->setToolTip(QApplication::translate("MainWindow", "Add Primitive", 0));
#endif // QT_NO_TOOLTIP
        actionMaterial_Editor->setText(QApplication::translate("MainWindow", "Material Editor", 0));
        actionAbout->setText(QApplication::translate("MainWindow", "About", 0));
        actionObjects_Toolbar->setText(QApplication::translate("MainWindow", "Objects", 0));
        actionTools_Toolbar->setText(QApplication::translate("MainWindow", "Tools", 0));
        actionMeshEditor->setText(QApplication::translate("MainWindow", "MeshEditor", 0));
        actionMove_Object->setText(QApplication::translate("MainWindow", "Move Object", 0));
#ifndef QT_NO_TOOLTIP
        actionMove_Object->setToolTip(QApplication::translate("MainWindow", "Move Object", 0));
#endif // QT_NO_TOOLTIP
        actionRotate_Object->setText(QApplication::translate("MainWindow", "Rotate Object", 0));
#ifndef QT_NO_TOOLTIP
        actionRotate_Object->setToolTip(QApplication::translate("MainWindow", "Rotate Object", 0));
#endif // QT_NO_TOOLTIP
        actionSelect_Object->setText(QApplication::translate("MainWindow", "Select Object", 0));
#ifndef QT_NO_TOOLTIP
        actionSelect_Object->setToolTip(QApplication::translate("MainWindow", "Select Object", 0));
#endif // QT_NO_TOOLTIP
        actionRemove_Object->setText(QApplication::translate("MainWindow", "Remove Object", 0));
#ifndef QT_NO_TOOLTIP
        actionRemove_Object->setToolTip(QApplication::translate("MainWindow", "Remove Object", 0));
#endif // QT_NO_TOOLTIP
        actionExport_Selected->setText(QApplication::translate("MainWindow", "Export Selected", 0));
        menuOp_es->setTitle(QApplication::translate("MainWindow", "Options", 0));
        menuView->setTitle(QApplication::translate("MainWindow", "View", 0));
        menuFile->setTitle(QApplication::translate("MainWindow", "File", 0));
        menuHelp->setTitle(QApplication::translate("MainWindow", "Help", 0));
        objectsToolbar->setWindowTitle(QApplication::translate("MainWindow", "Objects", 0));
        toolToolbar->setWindowTitle(QApplication::translate("MainWindow", "Tools", 0));
        meshEditorWidget->setWindowTitle(QApplication::translate("MainWindow", "MeshEditor", 0));
        label->setText(QApplication::translate("MainWindow", "Mesh:      ", 0));
        label_15->setText(QApplication::translate("MainWindow", "SubMesh:", 0));
        label_2->setText(QApplication::translate("MainWindow", "Material:  ", 0));
        label_3->setText(QApplication::translate("MainWindow", "Position:", 0));
        label_4->setText(QApplication::translate("MainWindow", "x", 0));
        meshPositionX->setInputMask(QString());
        label_5->setText(QApplication::translate("MainWindow", "y", 0));
        label_6->setText(QApplication::translate("MainWindow", "z", 0));
        label_11->setText(QApplication::translate("MainWindow", "Rotate:  ", 0));
        label_13->setText(QApplication::translate("MainWindow", "x", 0));
        label_12->setText(QApplication::translate("MainWindow", "y", 0));
        label_14->setText(QApplication::translate("MainWindow", "z", 0));
        label_7->setText(QApplication::translate("MainWindow", "Size:      ", 0));
        label_10->setText(QApplication::translate("MainWindow", "x", 0));
        label_9->setText(QApplication::translate("MainWindow", "y", 0));
        label_8->setText(QApplication::translate("MainWindow", "z", 0));
        tabWidget->setTabText(tabWidget->indexOf(tab), QApplication::translate("MainWindow", "Mesh", 0));
        label_16->setText(QApplication::translate("MainWindow", "Anim:", 0));
        renameButton->setText(QApplication::translate("MainWindow", "Rename", 0));
        checkBoxAnimationEnabled->setText(QApplication::translate("MainWindow", "Enabled", 0));
        checkBoxAnimationLoop->setText(QApplication::translate("MainWindow", "Loop", 0));
        checkBoxDisplaySkeleton->setText(QApplication::translate("MainWindow", "Display Skeleton", 0));
        PlayPauseButton->setText(QString());
        tabWidget->setTabText(tabWidget->indexOf(tab_2), QApplication::translate("MainWindow", "Animation", 0));
    } // retranslateUi

};

namespace Ui {
    class MainWindow: public Ui_MainWindow {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_MAINWINDOW_H
