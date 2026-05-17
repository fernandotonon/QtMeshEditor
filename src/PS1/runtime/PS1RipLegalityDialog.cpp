#include "PS1RipLegalityDialog.h"
#include "SentryReporter.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

namespace {
constexpr auto kSettingsGroup = "ps1Rip";
constexpr auto kAckKey = "acknowledged";
} // namespace

bool PS1RipLegalityDialog::isAcknowledged()
{
    return QSettings().value(QString::fromLatin1(kSettingsGroup) + QLatin1Char('/')
                             + QString::fromLatin1(kAckKey),
                         false)
        .toBool();
}

void PS1RipLegalityDialog::setAcknowledged(bool value)
{
    QSettings().setValue(QString::fromLatin1(kSettingsGroup) + QLatin1Char('/')
                           + QString::fromLatin1(kAckKey),
                       value);
}

PS1RipLegalityDialog::PS1RipLegalityDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("PS1 Runtime Ripper"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    auto *text = new QLabel(
        tr("QtMeshEditor's PS1 Runtime Ripper is intended for use with games and a BIOS "
           "you legally own. Provide your own BIOS file. No game data is bundled.\n\n"
           "Use only for personal preservation, modding, or educational purposes."),
        this);
    text->setWordWrap(true);
    layout->addWidget(text);

    m_ackCheck = new QCheckBox(tr("I understand"), this);
    layout->addWidget(m_ackCheck);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(m_ackCheck, &QCheckBox::toggled, buttons->button(QDialogButtonBox::Ok), &QWidget::setEnabled);
    layout->addWidget(buttons);
}

void PS1RipLegalityDialog::accept()
{
    if (m_ackCheck->isChecked()) {
        setAcknowledged(true);
        SentryReporter::addBreadcrumb(QStringLiteral("ps1.rip.dialog.acknowledged"),
                                    QStringLiteral("user acknowledged legality notice"));
    }
    QDialog::accept();
}
