#include "ThemeManager.h"
#include "AppSettingsKeys.h"
#include <QApplication>
#include <QPalette>
#include <QSettings>

ThemeManager* ThemeManager::m_pSingleton = nullptr;

ThemeManager* ThemeManager::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new ThemeManager();
    return m_pSingleton;
}

ThemeManager* ThemeManager::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine);
    Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void ThemeManager::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

void ThemeManager::applyThemePreference(const QString& themeValue)
{
    const QString themeLower = themeValue.trimmed().toLower();
    if (themeLower == QStringLiteral("dark")) {
        QPalette dark;
        dark.setColor(QPalette::Window, QColor(53, 53, 53));
        dark.setColor(QPalette::WindowText, Qt::white);
        dark.setColor(QPalette::Base, QColor(35, 35, 35));
        dark.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
        dark.setColor(QPalette::ToolTipBase, QColor(25, 25, 25));
        dark.setColor(QPalette::ToolTipText, Qt::white);
        dark.setColor(QPalette::Text, Qt::white);
        dark.setColor(QPalette::Button, QColor(53, 53, 53));
        dark.setColor(QPalette::ButtonText, Qt::white);
        dark.setColor(QPalette::Link, QColor(42, 130, 218));
        dark.setColor(QPalette::Highlight, QColor(42, 130, 218));
        dark.setColor(QPalette::HighlightedText, Qt::black);
        QApplication::setPalette(dark);
        return;
    }
    if (themeLower == QStringLiteral("light")) {
        QApplication::setPalette(QColor(QStringLiteral("ghostwhite")));
        return;
    }
    if (themeLower == QStringLiteral("custom")) {
        QSettings settings;
        const QColor custom = settings.value(QStringLiteral("customPalette")).value<QColor>();
        if (custom.isValid()) {
            QApplication::setPalette(custom);
        }
    }
}

void ThemeManager::applySavedThemeFromSettings()
{
    QSettings settings;
    const QString appearanceTheme = settings.value(AppSettingsKeys::appearanceTheme()).toString().trimmed();
    const QString paletteTheme = settings.value(AppSettingsKeys::palette(), QStringLiteral("dark")).toString().trimmed();
    const QString paletteThemeLower = paletteTheme.toLower();
    const QString themeValue =
        paletteThemeLower == QStringLiteral("custom")
            ? paletteTheme
            : (appearanceTheme.isEmpty() ? paletteTheme : appearanceTheme);
    applyThemePreference(themeValue);
}

ThemeManager::ThemeManager() : QObject(nullptr) {}

void ThemeManager::refreshTheme()
{
    emit themeChanged();
}

QColor ThemeManager::windowColor() const
{
    return QApplication::palette().color(QPalette::Window);
}

QColor ThemeManager::panelColor() const
{
    return QApplication::palette().color(QPalette::Window);
}

QColor ThemeManager::headerColor() const
{
    return QApplication::palette().color(QPalette::Window).darker(110);
}

QColor ThemeManager::inputColor() const
{
    return QApplication::palette().color(QPalette::Base);
}

QColor ThemeManager::textColor() const
{
    return QApplication::palette().color(QPalette::WindowText);
}

QColor ThemeManager::disabledTextColor() const
{
    return QApplication::palette().color(QPalette::Disabled, QPalette::WindowText);
}

QColor ThemeManager::placeholderTextColor() const
{
    return QApplication::palette().color(QPalette::PlaceholderText);
}

QColor ThemeManager::highlightColor() const
{
    return QApplication::palette().color(QPalette::Highlight);
}

QColor ThemeManager::highlightedTextColor() const
{
    return QApplication::palette().color(QPalette::HighlightedText);
}

QColor ThemeManager::buttonColor() const
{
    return QApplication::palette().color(QPalette::Button);
}

QColor ThemeManager::buttonTextColor() const
{
    return QApplication::palette().color(QPalette::ButtonText);
}

QColor ThemeManager::borderColor() const
{
    return QApplication::palette().color(QPalette::Mid);
}

QColor ThemeManager::accentColor() const
{
    return QApplication::palette().color(QPalette::Highlight);
}

QString ThemeManager::themeName() const
{
    auto bg = QApplication::palette().color(QPalette::Window);
    return bg.lightness() > 128 ? "light" : "dark";
}
