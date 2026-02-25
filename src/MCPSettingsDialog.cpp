#include "MCPSettingsDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QPushButton>

MCPSettingsDialog::MCPSettingsDialog(bool serverRunning, int currentPort, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("MCP Server Settings"));
    setMinimumWidth(350);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Server settings group
    QGroupBox *settingsGroup = new QGroupBox(tr("HTTP REST API"), this);
    QVBoxLayout *groupLayout = new QVBoxLayout(settingsGroup);

    m_enableCheckBox = new QCheckBox(tr("Enable MCP HTTP Server"), settingsGroup);
    m_enableCheckBox->setChecked(serverRunning);
    groupLayout->addWidget(m_enableCheckBox);

    QFormLayout *formLayout = new QFormLayout();
    m_portSpinBox = new QSpinBox(settingsGroup);
    m_portSpinBox->setRange(1024, 65535);
    m_portSpinBox->setValue(currentPort);
    m_portSpinBox->setEnabled(!serverRunning);
    formLayout->addRow(tr("Port:"), m_portSpinBox);
    groupLayout->addLayout(formLayout);

    // Status label
    m_statusLabel = new QLabel(settingsGroup);
    m_statusLabel->setWordWrap(true);
    groupLayout->addWidget(m_statusLabel);

    mainLayout->addWidget(settingsGroup);

    updateStatus(serverRunning);

    // Close button
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    QPushButton *closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    buttonLayout->addWidget(closeButton);
    mainLayout->addLayout(buttonLayout);

    // Toggle connection
    connect(m_enableCheckBox, &QCheckBox::toggled, this, &MCPSettingsDialog::onEnableToggled);
}

void MCPSettingsDialog::onEnableToggled(bool checked)
{
    if (checked) {
        int port = m_portSpinBox->value();
        bool ok = startCallback ? startCallback(port) : false;
        if (ok) {
            m_portSpinBox->setEnabled(false);
            updateStatus(true);
        } else {
            // Revert checkbox without re-triggering
            m_enableCheckBox->blockSignals(true);
            m_enableCheckBox->setChecked(false);
            m_enableCheckBox->blockSignals(false);
            m_statusLabel->setText(tr("Failed to start on port %1").arg(port));
            m_statusLabel->setStyleSheet("color: red;");
        }
    } else {
        if (stopCallback) stopCallback();
        m_portSpinBox->setEnabled(true);
        updateStatus(false);
    }
}

void MCPSettingsDialog::updateStatus(bool running)
{
    if (running) {
        m_statusLabel->setText(tr("Running on port %1").arg(m_portSpinBox->value()));
        m_statusLabel->setStyleSheet("color: green;");
    } else {
        m_statusLabel->setText(tr("Stopped"));
        m_statusLabel->setStyleSheet("color: gray;");
    }
}
