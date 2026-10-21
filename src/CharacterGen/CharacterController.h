#ifndef CHARACTERGEN_CHARACTERCONTROLLER_H
#define CHARACTERGEN_CHARACTERCONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>

#include <memory>

namespace CharacterGen {
struct Assets;
}

/// Inspector "Character Generator" (Object mode): random / hand-tuned body + face, skin and eye colour, one undoable Generate.
/// Assets are fetched on first use (CharacterAssets); the heavy build runs on the main thread (Ogre) in ~1-2 s.
class CharacterController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool assetsPresent READ assetsPresent NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY statusChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QStringList skinNames READ skinNames CONSTANT)
    Q_PROPERTY(QStringList eyeNames READ eyeNames CONSTANT)

public:
    static CharacterController* instance();
    static CharacterController* qmlInstance(QQmlEngine*, QJSEngine*);
    static void kill();

    bool assetsPresent() const;
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QStringList skinNames() const;
    QStringList eyeNames() const;

    /// body/face are "index=value,..." lists; seed < 0 = the average character. Returns "" on success, else an error.
    Q_INVOKABLE QString generate(int seed, double variety, const QString& body, const QString& face, const QString& skin,
                                 const QString& eye);

signals:
    void statusChanged();

private:
    CharacterController() = default;
    void setStatus(const QString& s, bool busy);

    bool m_busy = false;
    QString m_status;
    std::shared_ptr<CharacterGen::Assets> m_assets;
    static CharacterController* s_instance;
};

#endif
