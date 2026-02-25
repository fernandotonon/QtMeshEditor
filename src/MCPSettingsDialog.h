#ifndef MCPSETTINGSDIALOG_H
#define MCPSETTINGSDIALOG_H

#include <QDialog>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <functional>

class MCPSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MCPSettingsDialog(bool serverRunning, int currentPort, QWidget *parent = nullptr);

    // Callbacks set by the caller; startCallback returns true on success
    std::function<bool(int port)> startCallback;
    std::function<void()> stopCallback;

private:
    void onEnableToggled(bool checked);
    void updateStatus(bool running);

    QCheckBox *m_enableCheckBox;
    QSpinBox *m_portSpinBox;
    QLabel *m_statusLabel;
};

#endif // MCPSETTINGSDIALOG_H
