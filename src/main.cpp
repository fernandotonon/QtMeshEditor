#include <QApplication>
#include <QPalette>
#include <QDebug>
#include <QTimer>
#include <QStyleFactory>
#include <QSettings>
#include <QtQml/qqmlengine.h>
#include <QtQml/qjsengine.h>
#include "mainwindow.h"
#include "MaterialEditorQML.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QCoreApplication::setOrganizationName("QtMeshEditor");
    QCoreApplication::setOrganizationDomain("none");
    QCoreApplication::setApplicationName("QtMeshEditor");
    QCoreApplication::setApplicationVersion(QTMESHEDITOR_VERSION);

    a.setStyle(QStyleFactory::create("Fusion"));

    // Register QML types
    qmlRegisterSingletonType<MaterialEditorQML>("MaterialEditorQML", 1, 0, "MaterialEditorQML", 
        [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
            return MaterialEditorQML::qmlInstance(engine, scriptEngine);
        });

    MainWindow w;
    w.show();
    
    return a.exec();
}
