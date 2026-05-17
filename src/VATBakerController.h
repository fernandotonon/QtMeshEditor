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
 *   - `bake(...)` kicks off the bake on a worker QThread so the UI
 *     doesn't freeze on long anims. Progress is emitted as
 *     `bakeProgress(int done, int total)` after each frame; the
 *     terminal signal is `bakeFinished(bool ok, QString posTexture,
 *     QString error)`.
 *   - `isBaking` is `true` between the bake call and `bakeFinished`,
 *     so the QML button can disable itself.
 *
 * The baker itself is pure-data (`VATBaker::bake`); this class only
 * handles thread orchestration + QML exposure.
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

    /// Start a bake on a worker thread. Returns false synchronously if
    /// the bake can't even start (no selection, no animation, bake
    /// already in progress); in that case `bakeFinished` is NOT
    /// emitted. Returns true if the worker was kicked off; the result
    /// arrives via `bakeFinished`.
    ///
    /// `encoding` and `target` accept the same string values as the
    /// `qtmesh vat` CLI subcommand.
    Q_INVOKABLE bool bake(const QString& animationName,
                          double fps,
                          const QString& encoding,
                          const QString& target,
                          bool bakeNormals,
                          const QString& outputDir,
                          const QString& basename = QString());

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
