#ifndef VRAMVIEWERWIDGET_H
#define VRAMVIEWERWIDGET_H

#include "VramSnapshot.h"

#include <QImage>
#include <QLabel>
#include <QVector>
#include <QScrollArea>
#include <QWidget>

class QComboBox;

/** Interactive 1024×512 VRAM view for the PS1 rip session (#420). */
class VramViewerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VramViewerWidget(QWidget *parent = nullptr);

    void setVramData(const QVector<uint16_t> &cells, const QImage &nativePreview);
    void setViewMode(VramSnapshot::ViewMode mode);

public slots:
    void onViewModeChanged(int index);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refreshDisplay();
    void updateHover(const QPoint &pos);

    QVector<uint16_t> m_vramCells;
    QImage m_nativeImage;
    VramSnapshot::ViewMode m_mode = VramSnapshot::ViewMode::Native16;
    QComboBox *m_modeCombo = nullptr;
    QLabel *m_imageLabel = nullptr;
    QLabel *m_hoverLabel = nullptr;
    QScrollArea *m_scroll = nullptr;
};

#endif // VRAMVIEWERWIDGET_H
