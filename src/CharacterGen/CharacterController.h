#ifndef CHARACTERGEN_CHARACTERCONTROLLER_H
#define CHARACTERGEN_CHARACTERCONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <QStringList>

#include <memory>

namespace CharacterGen {
struct Assets;
}
#include "CharacterModel.h"

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
    Q_PROPERTY(bool openEyes READ openEyes WRITE setOpenEyes NOTIFY openEyesChanged)
    Q_PROPERTY(bool skinTexture READ skinTexture WRITE setSkinTexture NOTIFY skinTextureChanged)

public:
    static CharacterController* instance();
    static CharacterController* qmlInstance(QQmlEngine*, QJSEngine*);
    static void kill();

    bool assetsPresent() const;
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    bool skinTexture() const { return m_skinTexture; }
    void setSkinTexture(bool v) { if (v != m_skinTexture) { m_skinTexture = v; emit skinTextureChanged(); } }
    bool openEyes() const { return m_openEyes; }
    void setOpenEyes(bool v) { if (v != m_openEyes) { m_openEyes = v; emit openEyesChanged(); } }
    /// The first `count` body-shape coefficients a seed produces (what the sliders start from).
    Q_INVOKABLE QVariantList seedBody(int seed, double variety) const;   // all 45 coefficients
    QStringList skinNames() const;
    QStringList eyeNames() const;

    /// body/face are "index=value,..." lists; seed < 0 = the average character. Returns "" on success, else an error.
    /// replace = true: swap the character this panel made last (live preview; consecutive swaps merge into one undo step).
    Q_INVOKABLE QString generate(int seed, double variety, const QString& body, const QString& face, const QString& skin,
                                 const QString& eye, bool replace = false);
    /// A character made by this panel is still in the scene (so a slider change can edit it in place).
    Q_INVOKABLE bool hasCharacter() const;
    /// A short human name for body-shape mode `mode` (what it mostly changes), and what it does in a sentence.
    Q_INVOKABLE QString shapeLabel(int mode) const;
    Q_INVOKABLE QString shapeHint(int mode) const;

signals:
    void statusChanged();
    void openEyesChanged();
    void skinTextureChanged();

private:
    CharacterController() = default;
    void setStatus(const QString& s, bool busy);

    bool m_busy = false;
    QString m_status;
    QString m_currentNode;
    CharacterGen::Params m_currentParams;
    bool m_haveParams = false;
    bool m_openEyes = true;
    bool m_skinTexture = true;
    std::shared_ptr<CharacterGen::Assets> m_assets;
    static CharacterController* s_instance;
};

#endif
