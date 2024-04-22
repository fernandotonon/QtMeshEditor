#include "about.h"
#include "ui_about.h"

About::About(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::About)
{
    ui->setupUi(this);
    ui->versionText->setText(QString("Version: ")+QTMESHEDITOR_VERSION);
}

About::~About()
{
    delete ui;
}

QString About::getVersionText() const {
    return ui->versionText->text();
}
