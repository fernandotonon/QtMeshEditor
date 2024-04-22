#include "about.h"

About::About(QWidget *parent) :
    QDialog(parent)
{
    ui->setupUi(this);
    auto versionText = QString("Version: ")+QTMESHEDITOR_VERSION;
    ui->versionText->setText(versionText);
}

QString About::getVersionText() const {
    return ui->versionText->text();
}
