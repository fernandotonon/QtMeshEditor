#ifndef CHARACTERGEN_CHARACTERCONTROLLER_H
#define CHARACTERGEN_CHARACTERCONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <QStringList>

#include <algorithm>
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
    Q_PROPERTY(QStringList hairNames READ hairNames NOTIFY hairNamesChanged)
    Q_PROPERTY(QString hairStyle READ hairStyle WRITE setHairStyle NOTIFY hairStyleChanged)
    Q_PROPERTY(QString hairColour READ hairColour WRITE setHairColour NOTIFY hairColourChanged)
    Q_PROPERTY(QVariantList hairColourPresets READ hairColourPresets CONSTANT)
    Q_PROPERTY(QStringList eyeNames READ eyeNames CONSTANT)
    Q_PROPERTY(bool openEyes READ openEyes WRITE setOpenEyes NOTIFY openEyesChanged)
    Q_PROPERTY(bool skinTexture READ skinTexture WRITE setSkinTexture NOTIFY skinTextureChanged)
    Q_PROPERTY(QStringList topStyles READ topStyles CONSTANT)
    Q_PROPERTY(QStringList bottomStyles READ bottomStyles CONSTANT)
    Q_PROPERTY(QVariantList clothColours READ clothColours CONSTANT)
    Q_PROPERTY(QString topStyle READ topStyle WRITE setTopStyle NOTIFY clothesChanged)
    Q_PROPERTY(QString bottomStyle READ bottomStyle WRITE setBottomStyle NOTIFY clothesChanged)
    Q_PROPERTY(QString topColour READ topColour WRITE setTopColour NOTIFY clothesChanged)
    Q_PROPERTY(QString bottomColour READ bottomColour WRITE setBottomColour NOTIFY clothesChanged)
    Q_PROPERTY(bool eyelashes READ eyelashes WRITE setEyelashes NOTIFY eyelashesChanged)
    Q_PROPERTY(double bodyHair READ bodyHair WRITE setBodyHair NOTIFY bodyHairChanged)

public:
    static CharacterController* instance();
    static CharacterController* qmlInstance(QQmlEngine*, QJSEngine*);
    static void kill();

    bool assetsPresent() const;
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QStringList topStyles() const;
    QStringList bottomStyles() const;
    QVariantList clothColours() const;   ///< [{name, colour}]
    QString topStyle() const { return m_topStyle; }
    QString bottomStyle() const { return m_bottomStyle; }
    QString topColour() const { return m_topColour; }
    QString bottomColour() const { return m_bottomColour; }
    void setTopStyle(const QString& v);
    void setBottomStyle(const QString& v);
    void setTopColour(const QString& v);       ///< a name or "#rrggbb"; anything else is ignored
    void setBottomColour(const QString& v);
    bool eyelashes() const { return m_eyelashes; }
    void setEyelashes(bool v) { if (v != m_eyelashes) { m_eyelashes = v; emit eyelashesChanged(); } }
    double bodyHair() const { return m_bodyHair; }
    void setBodyHair(double v) { v = std::clamp(v, 0.0, 1.0); if (v != m_bodyHair) { m_bodyHair = v; emit bodyHairChanged(); } }
    bool skinTexture() const { return m_skinTexture; }
    void setSkinTexture(bool v) { if (v != m_skinTexture) { m_skinTexture = v; emit skinTextureChanged(); } }
    bool openEyes() const { return m_openEyes; }
    void setOpenEyes(bool v) { if (v != m_openEyes) { m_openEyes = v; emit openEyesChanged(); } }
    /// The first `count` body-shape coefficients a seed produces (what the sliders start from).
    Q_INVOKABLE QVariantList seedBody(int seed, double variety) const;   // all 45 coefficients
    QStringList skinNames() const;
    /// "#rrggbb" of a named skin colour (what the picker starts from); empty for an unknown name.
    Q_INVOKABLE QString skinColourHex(const QString& name) const;
    // the same colour dialog as the brush colour (non-native: native pickers freeze against Ogre's GL surface); empty when cancelled
    Q_INVOKABLE QString pickColour(const QString& initial) const;
    QStringList hairNames() const;
    QString hairStyle() const { return m_hairStyle; }
    void setHairStyle(const QString& v) { if (v != m_hairStyle) { m_hairStyle = v; emit hairStyleChanged(); } }
    /// The hair colour as "#rrggbb" (a preset id is accepted too; anything else is ignored).
    QString hairColour() const { return m_hairColour; }
    void setHairColour(const QString& v);
    /// [{name, label, colour}] of the hair colour presets.
    QVariantList hairColourPresets() const;
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
    void hairNamesChanged();
    void hairStyleChanged();
    void hairColourChanged();
    void bodyHairChanged();
    void eyelashesChanged();
    void clothesChanged();

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
    QString m_hairStyle;
    QStringList m_hairNamesSeen;   ///< the hairstyles the panel was last told about (hairNamesChanged)
    QString m_hairColour = QStringLiteral("#3d2517");
    double m_bodyHair = 0.5;
    bool m_eyelashes = true;
    QString m_topStyle = QStringLiteral("none");
    QString m_bottomStyle = QStringLiteral("none");
    QString m_topColour = QStringLiteral("#cccbc7");
    QString m_bottomColour = QStringLiteral("#3d4f78");
    std::shared_ptr<CharacterGen::Assets> m_assets;
    static CharacterController* s_instance;
};

#endif
