#include "GamificationToast.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

namespace {
constexpr int kAutoHideMs = 6000;
constexpr int kMargin = 16;

QPointer<GamificationToast> g_activeToast;
}  // namespace

void GamificationToast::showAchievements(QWidget* parentWindow, const QVariantList& achievements)
{
    if (!parentWindow || achievements.isEmpty())
        return;
    if (!g_activeToast)
        g_activeToast = new GamificationToast(parentWindow);
    g_activeToast->appendAchievements(achievements);
}

GamificationToast::GamificationToast(QWidget* parentWindow)
    : QWidget(parentWindow)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);
    setCursor(Qt::PointingHandCursor);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    m_label = new QLabel(this);
    m_label->setStyleSheet(QStringLiteral(
        "color: #ececec; font-size: 12px; background: transparent;"));
    m_label->setTextFormat(Qt::RichText);
    layout->addWidget(m_label);

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    connect(m_hideTimer, &QTimer::timeout, this, &QWidget::close);
}

void GamificationToast::appendAchievements(const QVariantList& achievements)
{
    for (const QVariant& v : achievements) {
        const QVariantMap a = v.toMap();
        const QString title = a.value(QStringLiteral("title")).toString();
        if (title.isEmpty() || m_titles.contains(title))
            continue;
        m_titles.append(title);
        m_totalXp += a.value(QStringLiteral("xp")).toInt();
    }
    if (m_titles.isEmpty()) {
        close();
        return;
    }
    updateText();
    adjustSize();
    reposition();
    show();
    raise();
    m_hideTimer->start(kAutoHideMs);
}

void GamificationToast::updateText()
{
    QString text = QStringLiteral("🏆 <b>%1</b>").arg(m_titles.first().toHtmlEscaped());
    if (m_titles.size() > 1)
        text += tr(" and %n more achievement(s)", nullptr, m_titles.size() - 1);
    if (m_totalXp > 0)
        text += QStringLiteral("  <span style='color:#9fd49f'>+%1 XP</span>").arg(m_totalXp);
    m_label->setText(tr("Achievement unlocked — %1").arg(text));
}

void GamificationToast::reposition()
{
    QWidget* p = parentWidget();
    if (!p)
        return;
    move(p->width() - width() - kMargin, kMargin + 40);
}

void GamificationToast::mousePressEvent(QMouseEvent* event)
{
    Q_UNUSED(event)
    close();
}

void GamificationToast::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);
    painter.fillPath(path, QColor(0x2b, 0x2b, 0x2b, 0xf0));
    painter.setPen(QColor(0x4a, 0x7a, 0xa8));
    painter.drawPath(path);
}
