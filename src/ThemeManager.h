#ifndef THEME_MANAGER_H
#define THEME_MANAGER_H

#include <QObject>
#include <QColor>
#include <QQmlEngine>

class ThemeManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Panel colors
    Q_PROPERTY(QColor windowColor READ windowColor NOTIFY themeChanged)
    Q_PROPERTY(QColor panelColor READ panelColor NOTIFY themeChanged)
    Q_PROPERTY(QColor headerColor READ headerColor NOTIFY themeChanged)
    Q_PROPERTY(QColor inputColor READ inputColor NOTIFY themeChanged)

    // Text colors
    Q_PROPERTY(QColor textColor READ textColor NOTIFY themeChanged)
    Q_PROPERTY(QColor disabledTextColor READ disabledTextColor NOTIFY themeChanged)
    Q_PROPERTY(QColor placeholderTextColor READ placeholderTextColor NOTIFY themeChanged)

    // Interactive colors
    Q_PROPERTY(QColor highlightColor READ highlightColor NOTIFY themeChanged)
    Q_PROPERTY(QColor highlightedTextColor READ highlightedTextColor NOTIFY themeChanged)
    Q_PROPERTY(QColor buttonColor READ buttonColor NOTIFY themeChanged)
    Q_PROPERTY(QColor buttonTextColor READ buttonTextColor NOTIFY themeChanged)

    // Border and accent
    Q_PROPERTY(QColor borderColor READ borderColor NOTIFY themeChanged)
    Q_PROPERTY(QColor accentColor READ accentColor NOTIFY themeChanged)

    // Theme name
    Q_PROPERTY(QString themeName READ themeName NOTIFY themeChanged)

public:
    static ThemeManager* instance();
    static ThemeManager* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();
    static void applyThemePreference(const QString& themeValue);
    static void applySavedThemeFromSettings();

    QColor windowColor() const;
    QColor panelColor() const;
    QColor headerColor() const;
    QColor inputColor() const;
    QColor textColor() const;
    QColor disabledTextColor() const;
    QColor placeholderTextColor() const;
    QColor highlightColor() const;
    QColor highlightedTextColor() const;
    QColor buttonColor() const;
    QColor buttonTextColor() const;
    QColor borderColor() const;
    QColor accentColor() const;
    QString themeName() const;

public slots:
    void refreshTheme();

signals:
    void themeChanged();

private:
    ThemeManager();
    ~ThemeManager() override = default;

    static ThemeManager* m_pSingleton;
};

#endif // THEME_MANAGER_H
