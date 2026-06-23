#include "CloudUploadProgress.h"

#include "SentryReporter.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>

CloudUploadProgress::CloudUploadProgress(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 2, 8, 2);

    m_label = new QLabel(this);
    m_bar = new QProgressBar(this);
    m_bar->setTextVisible(false);
    m_bar->setMaximumWidth(220);
    m_cancelButton = new QPushButton(tr("Cancel"), this);

    layout->addWidget(m_label, 1);
    layout->addWidget(m_bar);
    layout->addWidget(m_cancelButton);

    connect(m_cancelButton, &QPushButton::clicked, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud upload progress: Cancel"));
        emit cancelRequested();
    });
    hide();
}

void CloudUploadProgress::start(const QString& label, int total)
{
    m_label->setText(label);
    m_bar->setRange(0, qMax(1, total));
    m_bar->setValue(0);
    show();
}

void CloudUploadProgress::updateProgress(int current, int total, const QString& fileName)
{
    m_bar->setMaximum(qMax(1, total));
    m_bar->setValue(current);
    if (fileName.isEmpty())
        return;
    if (fileName.endsWith(QChar(0x2026)) || fileName.endsWith(QLatin1Char('.')))
        m_label->setText(fileName);
    else
        m_label->setText(tr("Uploading %1…").arg(fileName));
}

void CloudUploadProgress::finish(bool ok, const QString& message)
{
    m_label->setText(message);
    m_bar->setValue(m_bar->maximum());
    m_cancelButton->setEnabled(false);
    if (!ok)
        m_label->setStyleSheet(QStringLiteral("color: #ffb4b4;"));
}

void CloudUploadProgress::hideProgress()
{
    m_cancelButton->setEnabled(true);
    m_label->setStyleSheet({});
    hide();
}
