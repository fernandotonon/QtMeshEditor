#ifndef DNA_DNAFITCONTROLLER_H
#define DNA_DNAFITCONTROLLER_H

// Inspector action "Fit MetaHuman body to the selected character" (epic #1095, Phase 4).
//
// Select two entities: an imported MetaHuman BODY .dna and the character to fit it to. The fit runs
// (the same core as `qtmesh dna fit` / MCP fit_dna_body), the report is shown as the PREVIEW, and the
// fitted character is added to the scene as a NEW node next to the original — nothing existing is
// touched, so Ctrl+Z (AddFittedDnaCharacterCommand) fully reverts it.

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class DnaFitController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool canFit READ canFit NOTIFY changed)
    Q_PROPERTY(QString hint READ hint NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY changed)

public:
    static DnaFitController* instance();
    static DnaFitController* qmlInstance(QQmlEngine*, QJSEngine*);
    static void kill();

    bool canFit() const { return m_canFit; }
    /// Why fitting is not possible right now, or what will happen when it is.
    QString hint() const { return m_hint; }
    /// Result of the last fit (the preview report), or its error.
    QString status() const { return m_status; }
    /// A DNA character is in the scene at all (the section is shown only then).
    bool hasSelection() const { return m_hasDna; }

    /// mode: "auto" (joints when the target has a skeleton, else topology), "topology", "joints", "surface".
    Q_INVOKABLE bool fitSelected(const QString& mode = QStringLiteral("auto"), bool alignLimbs = true);
    Q_INVOKABLE void refresh();

signals:
    void changed();

private:
    explicit DnaFitController(QObject* parent = nullptr);
    static DnaFitController* m_pSingleton;
    bool m_canFit = false, m_hasDna = false;
    QString m_hint, m_status;
};

#endif
