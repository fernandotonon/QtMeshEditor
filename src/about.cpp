#include "about.h"

About::About(QWidget *parent) :
    QDialog(parent)
{
    ui->setupUi(this);
    ui->versionText->setText(QString("Version: ")+QTMESHEDITOR_VERSION);
}

QString About::getVersionText() const {
    return ui->versionText->text();
}
