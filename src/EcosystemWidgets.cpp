#include "EcosystemWidgets.h"

#include <QApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QToolButton>

#include <cmath>
#include <utility>

namespace EcosystemWidgets {

namespace {

QColor blend(const QColor& a, const QColor& b, double t)
{
    return QColor::fromRgbF(a.redF() * t + b.redF() * (1 - t),
                            a.greenF() * t + b.greenF() * (1 - t),
                            a.blueF() * t + b.blueF() * (1 - t));
}

double luminance(const QColor& c)
{
    auto lin = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}

double contrast(const QColor& a, const QColor& b)
{
    double la = luminance(a), lb = luminance(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

QString css(const QColor& c) { return c.name(QColor::HexRgb); }

QPixmap roundedThumbnail(const QString& path, const QSize& size, qreal dpr)
{
    const QSize device = size * dpr;
    QPixmap out(device);
    out.fill(Qt::transparent);
    QPixmap src(path);
    if (src.isNull())
        return QPixmap();

    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(device)), 6 * dpr, 6 * dpr);
    p.setClipPath(clip);
    // Cover-fit: fill the card, crop the overflow evenly.
    const QPixmap scaled = src.scaled(device, Qt::KeepAspectRatioByExpanding,
                                      Qt::SmoothTransformation);
    p.drawPixmap((device.width() - scaled.width()) / 2,
                 (device.height() - scaled.height()) / 2, scaled);
    p.end();
    out.setDevicePixelRatio(dpr);
    return out;
}

} // namespace

QColor mutedColor(const QPalette& pal)
{
    // 65 % text over the background: readable in both themes, clearly secondary.
    return blend(pal.color(QPalette::WindowText), pal.color(QPalette::Window), 0.65);
}

QColor lineColor(const QPalette& pal)
{
    return blend(pal.color(QPalette::WindowText), pal.color(QPalette::Window), 0.22);
}

QColor accentColor(const QPalette& pal)
{
    const QColor bg = pal.color(QPalette::Window);
    QColor accent = pal.color(QPalette::Highlight);
    const bool darkTheme = luminance(bg) < 0.2;
    // WCAG AA for text (4.5:1): step the accent away from the background.
    for (int i = 0; i < 12 && contrast(accent, bg) < 4.5; ++i)
        accent = darkTheme ? accent.lighter(118) : accent.darker(118);
    return accent;
}

QLabel* sectionHeader(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text.toUpper(), parent);
    QFont f = label->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 0.85);
    f.setLetterSpacing(QFont::PercentageSpacing, 108);
    label->setFont(f);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, mutedColor(pal));
    label->setPalette(pal);
    return label;
}

QLabel* mutedLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, mutedColor(pal));
    label->setPalette(pal);
    return label;
}

QLabel* linkLabel(const QString& html, const QString& surface, QWidget* parent)
{
    // Colour the anchors inline: rich-text links otherwise take the palette's
    // Link role, which the editor's dark theme sets to a dim grey.
    const QPalette pal = parent ? parent->palette() : QApplication::palette();
    QString styled = html;
    styled.replace(QStringLiteral("<a href="),
                   QStringLiteral("<a style=\"color:%1\" href=").arg(css(accentColor(pal))));
    auto* label = new QLabel(styled, parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(false);
    label->setTextInteractionFlags(Qt::LinksAccessibleByMouse
                                   | Qt::LinksAccessibleByKeyboard);
    label->setFocusPolicy(Qt::StrongFocus);
    QObject::connect(label, &QLabel::linkActivated, label, [surface](const QString& link) {
        const QUrl target(link);
        QtMeshEcosystem::openUrl(target, target.host() + target.path(), surface);
    });
    return label;
}

QToolButton* gameCard(const QtMeshEcosystem::Game& game, const QString& surface,
                      const QSize& thumbnailSize, QWidget* parent)
{
    auto* card = new QToolButton(parent);
    card->setObjectName(QStringLiteral("ecosystemGameCard"));
    card->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    card->setAutoRaise(false);
    card->setFocusPolicy(Qt::StrongFocus);
    card->setCursor(Qt::PointingHandCursor);
    card->setText(game.title + QLatin1Char('\n') + game.platform);
    card->setToolTip(QStringLiteral("%1 (%2)\n%3").arg(game.title, game.platform, game.blurb));
    card->setAccessibleName(QStringLiteral("%1, %2 game").arg(game.title, game.platform));
    card->setAccessibleDescription(game.blurb);

    const qreal dpr = parent ? parent->devicePixelRatioF()
                             : (QGuiApplication::primaryScreen()
                                    ? QGuiApplication::primaryScreen()->devicePixelRatio()
                                    : 1.0);
    const QPixmap thumb = roundedThumbnail(game.thumbnail, thumbnailSize, qMax<qreal>(dpr, 1.0));
    if (!thumb.isNull()) {
        card->setIcon(QIcon(thumb));
        card->setIconSize(thumbnailSize);
    }
    const QPalette pal = parent ? parent->palette() : QApplication::palette();
    card->setStyleSheet(QStringLiteral(
        "QToolButton#ecosystemGameCard {"
        "  border: 1px solid %1; border-radius: 8px;"
        "  background: palette(alternate-base); color: palette(window-text); padding: 6px; }"
        "QToolButton#ecosystemGameCard:hover { border-color: %2; }"
        "QToolButton#ecosystemGameCard:focus { border: 2px solid %2; }")
        .arg(css(lineColor(pal)), css(accentColor(pal))));

    const QUrl target(game.url);
    const QString name = QStringLiteral("game:") + game.id;
    QObject::connect(card, &QToolButton::clicked, card, [target, name, surface]() {
        QtMeshEcosystem::openUrl(target, name, surface);
    });
    return card;
}

QPushButton* linkButton(const QString& text, QtMeshEcosystem::Link link,
                        const QString& surface, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setToolTip(QtMeshEcosystem::url(link).toString());
    const QPalette pal = parent ? parent->palette() : QApplication::palette();
    button->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; border: 1px solid transparent; padding: 4px 6px;"
        "  font-weight: 600; background: transparent; }"
        "QPushButton:hover { text-decoration: underline; }"
        "QPushButton:focus { border: 1px solid %1; border-radius: 4px; }")
        .arg(css(accentColor(pal))));
    QObject::connect(button, &QPushButton::clicked, button, [link, surface]() {
        QtMeshEcosystem::open(link, surface);
    });
    return button;
}

QWidget* separator(QWidget* parent)
{
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::NoFrame);
    line->setFixedHeight(1);
    const QPalette pal = parent ? parent->palette() : QApplication::palette();
    line->setStyleSheet(QStringLiteral("background: %1;").arg(css(lineColor(pal))));
    return line;
}

QString primaryButtonStyle(const QPalette& pal)
{
    return QStringLiteral(
        "QPushButton { background: palette(highlight); color: palette(highlighted-text);"
        "  border: 2px solid transparent; border-radius: 6px; padding: 0 18px; font-weight: 600; }"
        "QPushButton:hover { background: %1; }"
        "QPushButton:focus { border: 2px solid %2; }")
        .arg(css(pal.color(QPalette::Highlight).lighter(112)), css(accentColor(pal)));
}

QString secondaryButtonStyle(const QPalette& pal)
{
    return QStringLiteral(
        "QPushButton { background: palette(button); color: palette(button-text);"
        "  border: 1px solid %1; border-radius: 6px; padding: 0 18px;"
        "  font-weight: 600; }"
        "QPushButton:hover { border-color: %2; }"
        "QPushButton:focus { border: 2px solid %2; }")
        .arg(css(lineColor(pal)), css(accentColor(pal)));
}

} // namespace EcosystemWidgets
