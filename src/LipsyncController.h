#ifndef LIPSYNCCONTROLLER_H
#define LIPSYNCCONTROLLER_H

// Audio2Face (#1019): the GUI bridge for audio-driven lipsync.
//
// Mirrors FaceRigController: the heavy work (a ~320 MB first-use model
// download, then one ONNX inference plus a constrained solve per frame) runs
// on a WORKER thread so the window keeps painting, and the results are
// committed to Ogre on the main thread, because morph keyframes touch the
// scene graph.
//
// The commit is one undo macro — a take is a single Ctrl+Z, not 300 separate
// keyframe undos.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <memory>

class QQmlEngine;
class QJSEngine;

class LipsyncController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// A mesh is selected AND carries morph targets. Without targets there is
    /// nothing to drive, so the UI points at `facerig` instead of failing.
    Q_PROPERTY(bool hasRiggableSelection READ hasRiggableSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int progressTotal READ progressTotal NOTIFY progressChanged)
    /// Emotion names, in the order the model expects them.
    Q_PROPERTY(QStringList emotionNames READ emotionNames CONSTANT)

public:
    static LipsyncController* instance();
    static QObject* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasRiggableSelection() const;
    bool available() const;
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    int progress() const { return m_progress; }
    int progressTotal() const { return m_progressTotal; }
    QStringList emotionNames() const;

    /// Start a take on the selected entity. `audioPath` is a WAV; `emotion`
    /// is 0-10 values in `emotionNames` order. Returns false immediately if
    /// the preconditions fail (no selection, no targets, already running).
    Q_INVOKABLE bool generateAsync(const QString& audioPath,
                                   const QString& clipName = QStringLiteral("Lipsync"),
                                   double fps = 30.0,
                                   const QList<double>& emotion = {});
    Q_INVOKABLE void cancel();

    /// Ask MainWindow to raise a file dialog — QML cannot parent one, the
    /// same split HdrEnvironmentController uses.
    Q_INVOKABLE void browseForAudio();

signals:
    void selectionChanged();
    void busyChanged();
    void statusChanged();
    void progressChanged();
    /// Terminal: `ok` false means nothing was written and `message` says why.
    void finished(bool ok, const QString& message);
    /// MainWindow opens the dialog and calls generateAsync with the result.
    void browseRequested();

private:
    explicit LipsyncController(QObject* parent = nullptr);
    ~LipsyncController() override;

    void setStatus(const QString& s);
    void setBusy(bool b);
    void setProgress(int done, int total);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_busy = false;
    QString m_status;
    int m_progress = 0;
    int m_progressTotal = 0;
};

#endif  // LIPSYNCCONTROLLER_H
