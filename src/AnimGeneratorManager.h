/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef ANIMGENERATORMANAGER_H
#define ANIMGENERATORMANAGER_H

// Scene side of procedural animation generators (#524). The formulas live in
// AnimGenerators.h; this binds them to the live scene.
//
// TWO BINDINGS, one model (`property = base + Σ active generators`):
//
//  * TRACK-BACKED targets (bone, node, morph) are MATERIALISED into the real
//    Ogre animation track: the track's original keyframes are snapshotted as
//    the BASE, and the track is rewritten as base keys outside the generator
//    window plus dense samples of base⊕generators inside it. Playback,
//    scrubbing, the dope sheet and every exporter therefore see the motion
//    with no new playback code. Muting restores the base; editing a
//    parameter re-materialises from the base (never from the previous
//    result, so nothing accumulates). Generators on the SAME track share one
//    base, so muting one never wipes another's effect.
//  * RUNTIME targets (pose weight, light, material) have no Ogre track, so
//    `tick()` evaluates `base(t) + Σ generators(t)` every frame on the
//    generator clock (advances while playing, follows the timeline slider
//    while paused).
//
// BAKE folds one generator into its base: the base becomes the generator's
// keyframes (a real track for bone/node/morph, a keyed curve for runtime
// targets) and the generator stays attached but inactive.
//
// UNDO: every mutation is one QUndoCommand that swaps two DOCUMENTS
// (`qtmesh-anim-generators-v1` + the base snapshots). Applying a document
// restores every track that leaves it and re-materialises every track in it,
// so undo/redo replays exactly. The same document is the
// `<file>.generators.json` sidecar written next to an export.
//
// Invariant: a base snapshot exists exactly as long as some generator
// (active, muted or baked) references its track.

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

#include "AnimGenerators.h"

#include <vector>

class OgreWidget;
class QQmlEngine;
class QJSEngine;
namespace Ogre { class Entity; class SceneNode; class ManualObject; class Camera; }

class AnimGeneratorManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QVariantList generators READ generatorRows NOTIFY generatorsChanged)
    Q_PROPERTY(int count READ count NOTIFY generatorsChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool lastOk READ lastOk NOTIFY statusChanged)
    Q_PROPERTY(QString pathEditId READ pathEditId NOTIFY pathEditChanged)
    Q_PROPERTY(int selectedPathPoint READ selectedPathPoint NOTIFY pathEditChanged)
    Q_PROPERTY(QStringList typeIds READ typeIds CONSTANT)
    Q_PROPERTY(QStringList kindIds READ kindIds CONSTANT)

public:
    static AnimGeneratorManager* instance();
    /// The instance if it exists — the render loop must not resurrect it
    /// during teardown.
    static AnimGeneratorManager* peek() { return m_pSingleton; }
    static AnimGeneratorManager* qmlInstance(QQmlEngine*, QJSEngine*);
    static void kill();

    // ---- C++ API (CLI / MCP / tests) -----------------------------------
    struct Result {
        bool ok = false;
        QString id;      ///< the generator acted on
        QString error;
    };

    /// Add a generator. Resolves a missing clip (bone: the selected / first
    /// skeletal clip; node: "Generators", created when absent; morph: the
    /// weight clip), validates that the target exists, then materialises it.
    /// One undo step when `undoable`.
    Result add(AnimGen::Generator g, bool undoable = true);
    Result remove(const QString& id, bool undoable = true);
    /// Apply `key=value` overrides (AnimGen::applyParam keys) as ONE step.
    Result setParams(const QString& id, const QList<QPair<QString, QString>>& params, bool undoable = true);
    Result setEnabled(const QString& id, bool enabled, bool undoable = true);
    Result bake(const QString& id, bool undoable = true);
    Result setPathPoints(const QString& id, const std::vector<Ogre::Vector3>& pts, bool undoable = true);

    const std::vector<AnimGen::Generator>& generators() const { return m_gens; }
    const AnimGen::Generator* find(const QString& id) const;
    /// True when the generator's target was found and its base captured.
    bool isBound(const QString& id) const;

    /// Full state: generators + base snapshots.
    QJsonObject document() const;
    /// Replace the state with `doc` (restores tracks that leave, materialises
    /// the rest). Used by undo/redo. Not itself undoable.
    bool applyDocument(const QJsonObject& doc, QString* error = nullptr);
    /// Remove every generator, restoring every base. Not undoable (scene reset).
    void clear();

    /// Sidecar I/O. `writeSidecar` keeps only generators whose target object
    /// is in `objects` (empty = all); returns false and removes a stale file
    /// when there is nothing to write. `loadSidecar` APPENDS the file's
    /// generators (ids re-assigned on collision), renaming target objects via
    /// `rename` (old → new name, for an asset re-imported under another
    /// name). The file's tracks already hold the materialised motion, so the
    /// load only re-adopts the bases.
    static QString sidecarPath(const QString& assetPath);
    bool writeSidecar(const QString& assetPath, const QStringList& objects = {},
                      const QJsonObject& meta = {}) const;
    /// The `meta` object a sidecar was written with (e.g. the exported
    /// entity / node names, so a re-import can rename targets).
    static QJsonObject sidecarMeta(const QString& assetPath);
    int loadSidecar(const QString& assetPath, const QHash<QString, QString>& rename = {},
                    QString* error = nullptr);

    /// Per-frame update from the render loop: advances the generator clock
    /// (`dt` while playing; follows the timeline slider while paused) and
    /// drives the runtime targets. Cheap when there are none.
    void tick(double dt, bool playing);
    double clock() const { return m_clock; }
    void setClock(double seconds) { m_clock = seconds; }

    /// Value a RUNTIME target receives at clock time `t` (base + active
    /// generators). For tests and the bake check.
    double runtimeValue(const AnimGen::Target& target, double t) const;

    // ---- Viewport path editing (TransformOperator routes the mouse) ------
    bool pathEditActive() const { return !m_pathEditId.isEmpty(); }
    bool beginDrag(OgreWidget* widget, const QPoint& screenPos);
    void updateDrag(OgreWidget* widget, const QPoint& screenPos);
    void endDrag();
    bool dragActive() const { return m_dragActive; }
    void updateHover(OgreWidget* widget, const QPoint& screenPos);

    // ---- QML ------------------------------------------------------------
    QVariantList generatorRows() const;
    int count() const { return int(m_gens.size()); }
    QString status() const { return m_status; }
    bool lastOk() const { return m_lastOk; }
    QString pathEditId() const { return m_pathEditId; }
    int selectedPathPoint() const { return m_selectedPoint; }
    QStringList typeIds() const { return AnimGen::typeIds(); }
    QStringList kindIds() const { return AnimGen::kindIds(); }

    Q_INVOKABLE QString typeLabel(const QString& typeId) const;
    Q_INVOKABLE QStringList objectsFor(const QString& kind) const;
    Q_INVOKABLE QStringList subsFor(const QString& kind, const QString& object) const;
    Q_INVOKABLE QStringList clipsFor(const QString& kind, const QString& object) const;
    Q_INVOKABLE QStringList channelsFor(const QString& kind, const QString& typeId) const;
    /// Add from the panel: `target` is the string form; `params` key → value.
    Q_INVOKABLE bool addFromUi(const QString& typeId, const QString& target, const QVariantMap& params);
    Q_INVOKABLE bool removeFromUi(const QString& id);
    Q_INVOKABLE bool setEnabledFromUi(const QString& id, bool enabled);
    Q_INVOKABLE bool bakeFromUi(const QString& id);
    Q_INVOKABLE bool setParamFromUi(const QString& id, const QString& key, const QString& value);
    Q_INVOKABLE QVariantMap details(const QString& id) const;

    Q_INVOKABLE bool beginPathEdit(const QString& id);
    Q_INVOKABLE void endPathEdit();
    Q_INVOKABLE bool addPathPoint(const QString& id);
    Q_INVOKABLE bool removePathPoint(const QString& id, int index);
    Q_INVOKABLE bool setPathPoint(const QString& id, int index, double x, double y, double z);
    Q_INVOKABLE void selectPathPoint(int index);

    /// Track key a target belongs to (one base per key).
    static QString trackKey(const AnimGen::Target& t);

signals:
    void generatorsChanged();
    void statusChanged();
    void pathEditChanged();

private:
    AnimGeneratorManager();
    ~AnimGeneratorManager() override;

    struct Doc {
        std::vector<AnimGen::Generator> gens;
        QHash<QString, QJsonObject> bases;
    };
    static QJsonObject makeDocument(const Doc& d);
    static bool parseDocument(const QJsonObject& doc, Doc* out, QString* error);
    Doc current() const { return Doc{m_gens, m_bases}; }

    /// Apply `next` and push one undo step from the current state.
    Result commit(const Doc& next, const QString& label, const QString& id, bool undoable);
    void setStatus(const QString& text, bool ok);
    AnimGen::Generator* findMutable(const QString& id);

    // Resolution / validation against the live scene.
    bool resolveTarget(AnimGen::Target* t, QString* error) const;

    // Track-backed binding.
    bool captureBase(const QString& key, const AnimGen::Target& t, QJsonObject* base, QString* error);
    bool restoreBase(const QString& key, const QJsonObject& base);
    bool materialise(const QString& key, QJsonObject* base, const std::vector<const AnimGen::Generator*>& gens);
    QJsonObject bakedBase(const QString& key, const QJsonObject& base, const AnimGen::Generator& g);
    double clipLengthFor(const AnimGen::Target& t) const;

    // Runtime binding.
    bool readRuntime(const AnimGen::Target& t, double* v) const;
    void writeRuntime(const AnimGen::Target& t, double v);
    double runtimeSpan(const QString& key) const;
    static double evalRuntimeBase(const QJsonObject& base, double t);

    std::vector<const AnimGen::Generator*> activeFor(const QString& key) const;
    QStringList referencedKeys(const std::vector<AnimGen::Generator>& gens) const;

    // Path overlay.
    const AnimGen::Generator* pathGen() const;
    bool pathSpace(const AnimGen::Target& t, Ogre::Vector3* pos, Ogre::Quaternion* rot, Ogre::Vector3* scale) const;
    void refreshPathOverlay();
    void destroyPathOverlay();
    int hitTestPathPoint(OgreWidget* widget, const QPoint& screenPos) const;

    static AnimGeneratorManager* m_pSingleton;

    std::vector<AnimGen::Generator> m_gens;
    QHash<QString, QJsonObject> m_bases;
    QString m_status;
    bool m_lastOk = true;
    double m_clock = 0.0;
    int m_lastSliderMs = -1;

    QString m_pathEditId;
    int m_selectedPoint = -1;
    int m_hoverPoint = -1;
    bool m_dragActive = false;
    bool m_dragMoved = false;
    QJsonObject m_dragBefore;
    Ogre::Vector3 m_dragPlaneNormal;
    Ogre::Vector3 m_dragAnchorWorld;
    Ogre::Vector3 m_dragStartPoint;
    Ogre::SceneNode* m_overlayNode = nullptr;
    Ogre::ManualObject* m_overlayObj = nullptr;
};

#endif // ANIMGENERATORMANAGER_H
