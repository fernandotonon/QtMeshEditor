/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef VATBAKERCONTROLLER_H
#define VATBAKERCONTROLLER_H

#include "VATBaker.h"

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

/**
 * @brief QML_SINGLETON that wraps `VATBaker::bake()` for the Inspector UI.
 *
 * Surface contract:
 *   - The Inspector binds `availableAnimations` to populate the anim
 *     dropdown for the current selection. `refreshAnimations()` walks
 *     `SelectionSet` and fills the list. The Inspector calls this when
 *     selection changes (via the existing `selectionChanged` signal
 *     chain) and once on panel mount.
 *   - `bake(...)` runs the bake synchronously in slice 4. Sampling an
 *     Ogre animation state off-thread races with `Manager`'s per-frame
 *     updates, so a true producer/consumer split has to wait for a
 *     dedicated slice. `bakeFinished(bool ok, QString posTexture,
 *     QString error)` is emitted exactly once per `bake()` call —
 *     including the validation-failure paths, *except* the "already
 *     baking" guard which returns false without emitting (so the
 *     in-flight bake's `bakeFinished` is the only one observable).
 *   - `bakeProgress(int done, int total)` fires twice in slice 4:
 *     once at start (`done=total=0`) and once at end (both set to the
 *     frame count). Future slices add per-frame progress.
 *   - `isBaking` flips to `true` at the start of `bake()` and back to
 *     `false` after `bakeFinished` is emitted — QML uses it to gate
 *     the Bake button.
 *
 * The baker itself is pure-data (`VATBaker::bake`); this class handles
 * argument validation, signal emission, and QML exposure.
 */
class VATBakerController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QStringList availableAnimations READ availableAnimations NOTIFY availableAnimationsChanged)
    Q_PROPERTY(bool isBaking READ isBaking NOTIFY isBakingChanged)
    Q_PROPERTY(int progressDone READ progressDone NOTIFY bakeProgress)
    Q_PROPERTY(int progressTotal READ progressTotal NOTIFY bakeProgress)

public:
    static VATBakerController* instance();
    static VATBakerController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    QStringList availableAnimations() const { return m_animations; }
    bool isBaking() const { return m_isBaking; }
    int progressDone()  const { return m_progressDone; }
    int progressTotal() const { return m_progressTotal; }

    /// Repopulate `availableAnimations` from the first selected entity.
    /// QML calls this on mount and after selection changes.
    Q_INVOKABLE void refreshAnimations();

    /// Run an OpenVAT bake. Returns true if validation passed and
    /// `VATBaker::bake` was invoked; `bakeFinished(ok, posTexture, error)`
    /// is then emitted before this returns with the bake outcome.
    /// Returns false on a refused-to-start condition; in that case
    /// `bakeFinished(false, ..., reason)` is also emitted (so QML
    /// callers can rely on a single observable channel), except for
    /// the "already baking" guard where the in-flight bake will emit
    /// its own `bakeFinished` separately.
    Q_INVOKABLE bool bake(const QString& animationName,
                          double fps,
                          const QString& outputDir,
                          const QString& basename = QString());

    /// Open a native folder picker. Returns the selected absolute path
    /// or an empty string if the user cancelled. Used by the Inspector
    /// "Browse…" button on the output-dir row.
    Q_INVOKABLE QString chooseOutputDir(const QString& startDir = QString());

signals:
    void availableAnimationsChanged();
    void isBakingChanged();
    void bakeProgress(int done, int total);
    void bakeFinished(bool ok, const QString& posTexture, const QString& error);

private:
    explicit VATBakerController(QObject* parent = nullptr);
    ~VATBakerController() override;

    void setIsBaking(bool b);

    QStringList m_animations;
    bool m_isBaking = false;
    int  m_progressDone  = 0;
    int  m_progressTotal = 0;

    static VATBakerController* s_instance;
};

#endif // VATBAKERCONTROLLER_H
