#ifndef PS1RIPINPUTSETTINGSDIALOG_H
#define PS1RIPINPUTSETTINGSDIALOG_H

#include <QDialog>

class QKeyEvent;
class QTableWidget;

/** Small keyboard-mapping editor for PS1 rip session (#417). */
class PS1RipInputSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PS1RipInputSettingsDialog(QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onResetDefaults();
    void accept() override;

private:
    void rebuildRows();
    void beginCapture(int row);

    QTableWidget *m_table = nullptr;
    int m_captureRow = -1;
};

#endif // PS1RIPINPUTSETTINGSDIALOG_H
