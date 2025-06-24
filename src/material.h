#ifndef MATERIAL_H
#define MATERIAL_H

#include "QtWidgets/qlistwidget.h"
#include <QMainWindow>

namespace Ui {
class Material;
}

class Material : public QMainWindow
{
    Q_OBJECT
    
public:
    explicit Material(QWidget *parent = nullptr);
    virtual ~Material();
    void SetMaterialList(const QStringList &_list);
    
private slots:
    void on_listMaterial_itemSelectionChanged();

    void on_buttonEdit_clicked();
    
    void on_buttonEditQML_clicked();

    void on_buttonExport_clicked();

    void on_buttonNew_clicked();

    void on_pushButton_clicked();

    void on_listMaterial_itemDoubleClicked(QListWidgetItem *item);

private:
    Ui::Material *ui;

    void UpdateMaterialList();
};

#endif // MATERIAL_H
