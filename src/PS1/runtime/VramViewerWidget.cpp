#include "VramViewerWidget.h"

#include "PsxVramColor.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QVBoxLayout>

VramViewerWidget::VramViewerWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *toolbar = new QHBoxLayout();
    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItem(tr("16-bit (RGB555)"), static_cast<int>(VramSnapshot::ViewMode::Native16));
    m_modeCombo->addItem(tr("4bpp index"), static_cast<int>(VramSnapshot::ViewMode::As4bpp));
    m_modeCombo->addItem(tr("8bpp index"), static_cast<int>(VramSnapshot::ViewMode::As8bpp));
    m_modeCombo->addItem(tr("CLUT preview"), static_cast<int>(VramSnapshot::ViewMode::ClutPreview));
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &VramViewerWidget::onViewModeChanged);
    toolbar->addWidget(new QLabel(tr("View:"), this));
    toolbar->addWidget(m_modeCombo, 1);
    layout->addLayout(toolbar);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_imageLabel = new QLabel(m_scroll);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setMinimumSize(256, 128);
    m_imageLabel->setMouseTracking(true);
    m_imageLabel->installEventFilter(this);
    m_scroll->setWidget(m_imageLabel);
    layout->addWidget(m_scroll, 1);

    m_hoverLabel = new QLabel(this);
    m_hoverLabel->setText(tr("Hover over VRAM to inspect pixels"));
    layout->addWidget(m_hoverLabel);
}

void VramViewerWidget::setVramData(const QVector<uint16_t> &cells, const QImage &nativePreview)
{
    m_vramCells = cells;
    m_nativeImage = nativePreview;
    refreshDisplay();
}

void VramViewerWidget::setViewMode(VramSnapshot::ViewMode mode)
{
    m_mode = mode;
    const int idx = m_modeCombo->findData(static_cast<int>(mode));
    if (idx >= 0)
        m_modeCombo->setCurrentIndex(idx);
    refreshDisplay();
}

void VramViewerWidget::onViewModeChanged(int index)
{
    m_mode = static_cast<VramSnapshot::ViewMode>(m_modeCombo->itemData(index).toInt());
    refreshDisplay();
}

void VramViewerWidget::refreshDisplay()
{
    if (m_vramCells.size() != VramSnapshot::kWidth * VramSnapshot::kHeight) {
        m_imageLabel->clear();
        return;
    }

    VramSnapshot snap;
    snap.mutablePixels() = m_vramCells;
    const QImage shown = snap.toImage(m_mode, 0, 0);
    m_imageLabel->setPixmap(
        QPixmap::fromImage(shown.scaled(shown.size() * 2, Qt::KeepAspectRatio, Qt::FastTransformation)));
}

bool VramViewerWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_imageLabel && event->type() == QEvent::MouseMove) {
        const auto *me = static_cast<QMouseEvent *>(event);
        const QPixmap pix = m_imageLabel->pixmap(Qt::ReturnByValue);
        if (!pix.isNull() && m_vramCells.size() == VramSnapshot::kWidth * VramSnapshot::kHeight) {
            const QPoint labelPos = me->pos();
            const int x = (labelPos.x() * VramSnapshot::kWidth) / qMax(1, pix.width());
            const int y = (labelPos.y() * VramSnapshot::kHeight) / qMax(1, pix.height());
            updateHover(QPoint(x, y));
        }
    }
    return QWidget::eventFilter(watched, event);
}

void VramViewerWidget::updateHover(const QPoint &pos)
{
    if (m_vramCells.size() != VramSnapshot::kWidth * VramSnapshot::kHeight
        || !QRect(0, 0, VramSnapshot::kWidth, VramSnapshot::kHeight).contains(pos)) {
        m_hoverLabel->setText(tr("Hover over VRAM to inspect pixels"));
        return;
    }

    const uint16_t cell = m_vramCells[pos.y() * VramSnapshot::kWidth + pos.x()];
    const int tpageX = (pos.x() / 64) * 64;
    const int tpageY = (pos.y() / 256) * 256;
    m_hoverLabel->setText(tr("VRAM (%1,%2) TPAGE≈(%3,%4) RGB555=0x%5")
                              .arg(pos.x())
                              .arg(pos.y())
                              .arg(tpageX)
                              .arg(tpageY)
                              .arg(cell, 4, 16, QChar('0')));
}
