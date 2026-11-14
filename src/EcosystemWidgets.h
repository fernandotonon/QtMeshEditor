#ifndef ECOSYSTEM_WIDGETS_H
#define ECOSYSTEM_WIDGETS_H

#include "QtMeshEcosystem.h"

#include <QColor>
#include <QPalette>
#include <QSize>
#include <QString>

class QLabel;
class QPushButton;
class QToolButton;
class QWidget;

/**
 * @brief Small Qt Widgets building blocks shared by the Welcome and About
 *        dialogs, so both present the QtMesh ecosystem the same way.
 *
 * Everything is styled from the active QPalette (stylesheets use palette()
 * roles), so light and dark themes work without hardcoded colours.
 */
namespace EcosystemWidgets {

/// Small all-caps section heading ("MADE WITH QTMESHEDITOR").
QLabel* sectionHeader(const QString& text, QWidget* parent = nullptr);

/// A label drawn in a softer variant of the palette's text colour.
QLabel* mutedLabel(const QString& text, QWidget* parent = nullptr);

/// Rich-text label whose links open through QtMeshEcosystem::openUrl (so they
/// get a breadcrumb) and are reachable with the keyboard.
QLabel* linkLabel(const QString& html, const QString& surface, QWidget* parent = nullptr);

/// Clickable preview card for a showcase game (thumbnail, title, platform).
/// Focusable; Enter/Space opens the game.
QToolButton* gameCard(const QtMeshEcosystem::Game& game, const QString& surface,
                      const QSize& thumbnailSize, QWidget* parent = nullptr);

/// Flat text button that opens an ecosystem link ("Explore QtMesh Games →").
QPushButton* linkButton(const QString& text, QtMeshEcosystem::Link link,
                        const QString& surface, QWidget* parent = nullptr);

/// 1px horizontal rule in lineColor().
QWidget* separator(QWidget* parent = nullptr);

/// Accent for links, focus rings and the tagline: the palette highlight,
/// lightened on dark themes until it reads against the window colour (the
/// editor's dark palette has a deep-blue highlight and a grey Link role).
QColor accentColor(const QPalette& palette);
/// A hairline colour visible on both themes (Mid equals Window in dark mode).
QColor lineColor(const QPalette& palette);
/// Secondary text colour (65 % text over the window colour).
QColor mutedColor(const QPalette& palette);

/// Stylesheet for a primary (highlight-coloured) push button.
QString primaryButtonStyle(const QPalette& palette);
/// Stylesheet for a secondary (outlined) push button.
QString secondaryButtonStyle(const QPalette& palette);

} // namespace EcosystemWidgets

#endif // ECOSYSTEM_WIDGETS_H
