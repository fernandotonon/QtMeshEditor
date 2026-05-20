#include "PS1RipInputSettingsDialog.h"
#include "PsxJoypadBindings.h"
#include "PsxJoypadState.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

QString keyDisplayName(Qt::Key key)
{
    if (key == Qt::Key_unknown)
        return QObject::tr("(none)");
    return QKeySequence(key).toString(QKeySequence::NativeText);
}

} // namespace

PS1RipInputSettingsDialog::PS1RipInputSettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("PS1 Input — Keyboard"));
    setModal(true);
    resize(420, 480);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        tr("Defaults: arrows = D-pad, Z/X = ○/✕, S/D = △/□, Enter/Shift = Start/Select.\n"
           "Click a key cell and press the new key (Esc cancels)."),
        this));

    m_table = new QTableWidget(this);
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({tr("Control"), tr("Key")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_table);

    auto *resetBtn = new QPushButton(tr("Reset to defaults"), this);
    connect(resetBtn, &QPushButton::clicked, this, &PS1RipInputSettingsDialog::onResetDefaults);
    layout->addWidget(resetBtn);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &PS1RipInputSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    rebuildRows();
}

void PS1RipInputSettingsDialog::rebuildRows()
{
    m_captureRow = -1;
    m_table->setRowCount(static_cast<int>(PsxJoypadState::kButtonCount));
    for (unsigned button = 0; button < PsxJoypadState::kButtonCount; ++button) {
        const int row = static_cast<int>(button);
        m_table->setItem(row, 0, new QTableWidgetItem(PsxJoypadBindings::buttonLabel(button)));

        auto *keyBtn = new QPushButton(keyDisplayName(PsxJoypadBindings::keyForButton(button)), m_table);
        connect(keyBtn, &QPushButton::clicked, this, [this, row]() { beginCapture(row); });
        m_table->setCellWidget(row, 1, keyBtn);
    }
}

void PS1RipInputSettingsDialog::beginCapture(int row)
{
    m_captureRow = row;
    if (auto *btn = qobject_cast<QPushButton *>(m_table->cellWidget(row, 1)))
        btn->setText(tr("Press a key…"));
    setFocus(Qt::OtherFocusReason);
}

void PS1RipInputSettingsDialog::keyPressEvent(QKeyEvent *event)
{
    if (m_captureRow < 0) {
        QDialog::keyPressEvent(event);
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        rebuildRows();
        event->accept();
        return;
    }

    const unsigned button = static_cast<unsigned>(m_captureRow);
    PsxJoypadBindings::setKeyForButton(button, static_cast<Qt::Key>(event->key()));
    if (auto *btn = qobject_cast<QPushButton *>(m_table->cellWidget(m_captureRow, 1)))
        btn->setText(keyDisplayName(static_cast<Qt::Key>(event->key())));

    m_captureRow = -1;
    event->accept();
}

void PS1RipInputSettingsDialog::onResetDefaults()
{
    PsxJoypadBindings::resetToDefaults();
    rebuildRows();
}

void PS1RipInputSettingsDialog::accept()
{
    PsxJoypadBindings::save();
    QDialog::accept();
}
