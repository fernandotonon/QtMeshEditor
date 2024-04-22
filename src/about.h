#ifndef ABOUT_H
#define ABOUT_H

#include <QDialog>
#include "ui_about.h"

class About : public QDialog
{
    Q_OBJECT
    
public:
    explicit About(QWidget *parent = nullptr);

    QString getVersionText() const;
    
private:
    std::unique_ptr<Ui::About> ui = std::make_unique<Ui::About>();
};

#endif // ABOUT_H
