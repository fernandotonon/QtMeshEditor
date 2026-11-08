#ifndef CHARACTERGEN_CHARACTERCONTROLLER_H
#define CHARACTERGEN_CHARACTERCONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QStringList>

#include <algorithm>
#include <array>
#include <memory>

namespace CharacterGen {
struct Assets;
}
#include "CharacterModel.h"
#include "BodyShape.h"
#include "PhotoCharacter.h"
#include <QImage>

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
    /// 0..1 while the first-use asset download is running; -1 when idle / unknown size.
    Q_PROPERTY(double downloadProgress READ downloadProgress NOTIFY downloadProgressChanged)
    Q_PROPERTY(QString downloadLabel READ downloadLabel NOTIFY downloadProgressChanged)
    Q_PROPERTY(qint64 downloadBytesReceived READ downloadBytesReceived NOTIFY downloadProgressChanged)
    Q_PROPERTY(qint64 downloadBytesTotal READ downloadBytesTotal NOTIFY downloadProgressChanged)
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
    Q_PROPERTY(QString hairSource READ hairSource NOTIFY hairStyleChanged)
    Q_PROPERTY(QVariantList hairParamSpecs READ hairParamSpecs CONSTANT)
    Q_PROPERTY(QVariantMap hairParams READ hairParams NOTIFY hairParamsChanged)
    Q_PROPERTY(QVariantMap hairFitParams READ hairFitParams NOTIFY hairParamsChanged)
    Q_PROPERTY(QStringList hairPresetIds READ hairPresetIds CONSTANT)
    Q_PROPERTY(QStringList savedHairPresets READ savedHairPresets NOTIFY hairLibraryChanged)
    Q_PROPERTY(QVariantList hairLibrary READ hairLibrary NOTIFY hairLibraryChanged)
    Q_PROPERTY(QString hairStudioStatus READ hairStudioStatus NOTIFY hairLibraryChanged)
    Q_PROPERTY(bool eyelashes READ eyelashes WRITE setEyelashes NOTIFY eyelashesChanged)
    Q_PROPERTY(double bodyHair READ bodyHair WRITE setBodyHair NOTIFY bodyHairChanged)

public:
    static CharacterController* instance();
    static CharacterController* qmlInstance(QQmlEngine*, QJSEngine*);
    static void kill();

    bool assetsPresent() const;
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    double downloadProgress() const { return m_downloadProgress; }
    QString downloadLabel() const { return m_downloadLabel; }
    qint64 downloadBytesReceived() const { return m_downloadBytesReceived; }
    qint64 downloadBytesTotal() const { return m_downloadBytesTotal; }
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
    void setHairStyle(const QString& v) { if (v != m_hairStyle) { m_hairStyle = v; emit hairStyleChanged(); saveHairStudioSettings(); } }
    /// The hair colour as "#rrggbb" (a preset id is accepted too; anything else is ignored).
    QString hairColour() const { return m_hairColour; }
    void setHairColour(const QString& v);
    /// [{name, label, colour}] of the hair colour presets.
    QVariantList hairColourPresets() const;
    QStringList eyeNames() const;

    // ---- Hair Studio (ProceduralHair.h / HairFitting.h / HairLibrary.h) ----
    /// "none" | "bundle" | "procedural" | "library": where the current hairstyle comes from.
    QString hairSource() const;
    QVariantList hairParamSpecs() const;        ///< [{key,label,min,max,step}]
    QVariantMap hairParams() const;             ///< current procedural parameters by key
    QVariantMap hairFitParams() const;          ///< current fit adjustment (tx..rz, scale, snug, clearance, flip)
    QStringList hairPresetIds() const;
    QStringList savedHairPresets() const;
    QVariantList hairLibrary() const;           ///< [{id,name,length,texture,gender,style,license,author,collection,thumbnail}]
    QString hairStudioStatus() const { return m_hairStudioStatus; }
    /// The browser: built-in (bundle), procedural presets and imported styles in one list, filtered; [{key,kind,name,length,texture,gender,style,thumbnail,license,author,current}].
    /// An empty filter value (or "all") matches everything; `style` is a case-insensitive substring of the style/name.
    Q_INVOKABLE QVariantList hairBrowser(const QString& kind, const QString& length, const QString& texture, const QString& gender, const QString& style) const;
    /// Select a browser entry by its key ("proc:<id>", "lib:<id>" or a bundle style name).
    Q_INVOKABLE void selectHair(const QString& key);
    Q_INVOKABLE QString hairPresetLabel(const QString& id) const;
    /// Use a bundled ("short_straight" ...) or saved procedural preset: the hair becomes "proc:<id>".
    Q_INVOKABLE void useProceduralHair(const QString& presetId);
    Q_INVOKABLE void useLibraryHair(const QString& id);
    Q_INVOKABLE void clearHair();
    Q_INVOKABLE void setHairParam(const QString& key, double value);
    Q_INVOKABLE void setHairFitParam(const QString& key, double value);
    Q_INVOKABLE void resetHairParams();
    Q_INVOKABLE void resetHairFit();
    Q_INVOKABLE bool saveHairPreset(const QString& name);
    Q_INVOKABLE bool deleteHairPreset(const QString& name);
    /// Register hairstyles from a model file or a folder (FBX/OBJ/GLB/DAE...); returns a summary line.
    Q_INVOKABLE QString importHair(const QString& path, const QString& author, const QString& license, const QString& attribution);
    Q_INVOKABLE bool removeLibraryHair(const QString& id);
    /// Duplicate the current hairstyle under a new name (procedural: a saved preset; library: a copy of the entry).
    Q_INVOKABLE QString duplicateHair(const QString& name);
    Q_INVOKABLE QString hairFileFilter() const;
    /// Settings persistence (QSettings): the Hair Studio state of the last session.
    void saveHairStudioSettings() const;
    void loadHairStudioSettings();

    /// body/face are "index=value,..." lists; seed < 0 = the average character. Returns "" on success, else an error.
    /// replace = true: swap the character this panel made last (live preview; consecutive swaps merge into one undo step).
    Q_INVOKABLE QString generate(int seed, double variety, const QString& body, const QString& face, const QString& skin,
                                 const QString& eye, bool replace = false);
    /// A character made by this panel is still in the scene (so a slider change can edit it in place).
    Q_INVOKABLE bool hasCharacter() const;
    // ---- named shape controls (BodyShape.h, Proportions.h) ----
    /// The proportion factors [height, legs, arms, torso, shoulders, head] (1 = the mean body).
    Q_PROPERTY(QVariantList proportions READ proportions NOTIFY proportionsChanged)
    /// Bumped when the shape model (labels, measured sliders) becomes available or changes: bindings that call the measure functions depend on it.
    Q_PROPERTY(int shapeRevision READ shapeRevision NOTIFY shapeRevisionChanged)
    int shapeRevision() const { return m_shapeRevision; }
    QVariantList proportions() const;
    Q_INVOKABLE int proportionCount() const { return 6; }
    Q_INVOKABLE QString proportionLabel(int i) const;
    Q_INVOKABLE QString proportionHint(int i) const;
    Q_INVOKABLE void setProportion(int i, double value);
    Q_INVOKABLE void resetProportions();
    /// The measured sliders (chest, waist, hips, belly, bust): index -> label / hint / range in cm either side of the mean body.
    Q_INVOKABLE int measureCount() const { return 5; }
    Q_INVOKABLE QString measureLabel(int i) const;
    Q_INVOKABLE QString measureHint(int i) const;
    Q_INVOKABLE double measureRange(int i) const;
    /// Offset in cm of measurement `i` for these 45 coefficients, and the coefficients that put it at `target` cm without moving the others.
    Q_INVOKABLE double measureOffset(int i, const QVariantList& coefficients) const;
    Q_INVOKABLE QVariantList coefficientsWithMeasure(int i, double target, const QVariantList& coefficients) const;
    // ---- from an image (PhotoCharacter.h) ----
    /// Read the body, skin, hair, eyes and clothes from the image on a worker thread (the models download on first use); the status line
    /// reports the steps. The image comes from the shared source (ImageSourceController: a file, or one generated from a prompt).
    /// When it is done `photoFitted` carries the body coefficients and colours for the panel and the panel builds the character.
    Q_INVOKABLE void generateFromPhoto(const QString& path);
    /// A short human name for body-shape mode `mode` (what it mostly changes), and what it does in a sentence.
    Q_INVOKABLE QString shapeLabel(int mode) const;
    Q_INVOKABLE QString shapeHint(int mode) const;

#ifdef QTMESH_UNIT_TESTS
    /// Test hook: drive the ModelDownloader progress path without a live fetch.
    void unitTestWatchDownloads(bool on)
    {
        m_watchingDownload = on;
        if (!on) clearDownloadProgress();
    }
    void unitTestOnDownloadProgress(const QString& name, qint64 received, qint64 total)
    {
        onModelDownloadProgress(name, received, total);
    }
#endif

signals:
    void statusChanged();
    void downloadProgressChanged();
    void openEyesChanged();
    void skinTextureChanged();
    void hairNamesChanged();
    void hairStyleChanged();
    void hairColourChanged();
    void hairParamsChanged();
    void hairLibraryChanged();
    void bodyHairChanged();
    void proportionsChanged();
    /// The photo was read: the 45 body coefficients and the skin / eye colours ("#rrggbb", empty = not found) to put in the panel.
    void photoFitted(const QVariantList& coefficients, const QString& skin, const QString& eye);
    void shapeRevisionChanged();
    void eyelashesChanged();
    void clothesChanged();

private:
    CharacterController() = default;
    void setStatus(const QString& s, bool busy);
    void clearDownloadProgress();
    void onModelDownloadProgress(const QString& name, qint64 received, qint64 total);

    bool m_busy = false;
    QString m_status;
    double m_downloadProgress = -1.0;
    QString m_downloadLabel;
    qint64 m_downloadBytesReceived = 0;
    qint64 m_downloadBytesTotal = 0;
    bool m_watchingDownload = false;
    QString m_currentNode;
    CharacterGen::Params m_currentParams;
    bool m_haveParams = false;
    bool m_openEyes = true;
    bool m_skinTexture = true;
    QString m_hairStyle;
    CharacterGen::Hair::Params m_hairParams;
    CharacterGen::Hair::FitAdjust m_hairFit;
    QString m_hairStudioStatus;
    void hairChanged();
    QStringList m_hairNamesSeen;   ///< the hairstyles the panel was last told about (hairNamesChanged)
    QString m_hairColour = QStringLiteral("#3d2517");
    double m_bodyHair = 0.5;
    std::array<double, 6> m_proportions{1, 1, 1, 1, 1, 1};
    mutable std::shared_ptr<CharacterGen::BodyShape::Model> m_shapeModel;   // built from the loaded assets on first use
    const CharacterGen::BodyShape::Model* shapeModel() const;
    int m_shapeRevision = 0;
    bool prepareAssets(QString* err);
    void finishPhoto(const std::shared_ptr<CharacterGen::PhotoCharacter::Result>& result);
    void bumpShapeRevision() { ++m_shapeRevision; emit shapeRevisionChanged(); }
    bool m_eyelashes = true;
    QString m_topStyle = QStringLiteral("none");
    QString m_bottomStyle = QStringLiteral("none");
    QString m_topColour = QStringLiteral("#cccbc7");
    QString m_bottomColour = QStringLiteral("#3d4f78");
    std::shared_ptr<CharacterGen::Assets> m_assets;
    static CharacterController* s_instance;
};

#endif
