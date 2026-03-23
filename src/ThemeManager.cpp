#include "ThemeManager.h"
#include <QApplication>
#include <QPalette>

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
