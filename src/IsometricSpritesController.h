#ifndef ISOMETRIC_SPRITES_CONTROLLER_H
#define ISOMETRIC_SPRITES_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

/// QML-facing singleton for in-app isometric sprite export (#724).
/// Wraps `ModelIsometricRenderer` against the live scene selection.
class IsometricSpritesController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasExportableSelection READ hasExportableSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool hasSkinnedSelection READ hasSkinnedSelection NOTIFY selectionChanged)
    Q_PROPERTY(QStringList availableAnimations READ availableAnimations NOTIFY availableAnimationsChanged)
    Q_PROPERTY(bool isExporting READ isExporting NOTIFY isExportingChanged)

public:
    static IsometricSpritesController *instance();
    static IsometricSpritesController *qmlInstance(QQmlEngine *engine, QJSEngine *scriptEngine);
    static void kill();

    bool hasExportableSelection() const;
    bool hasSkinnedSelection() const;
    QStringList availableAnimations() const { return m_animations; }
    bool isExporting() const { return m_isExporting; }

    Q_INVOKABLE void refreshAnimations();

    /// Opens the save dialog via MainWindow (reliable parent for QQuickWidget-hosted QML windows).
    Q_INVOKABLE void requestOutputPathPick(const QString &startPath = QString());

    /// Direct save dialog (tests / headless). GUI should prefer requestOutputPathPick().
    Q_INVOKABLE QString chooseOutputPath(const QString &startPath = QString());

    /// Normalize a browse seed into a sensible default `.png` save path.
    static QString normalizedSaveSeed(const QString &startPath);

    /// Render the current selection to an isometric directions×frames PNG.
    /// Empty `animationName` → static mesh (one frame per direction).
    /// Returns `{ ok, outputPath, error, directions, frames, sheetWidth, sheetHeight }`.
    Q_INVOKABLE QVariantMap exportSelected(const QString &outputPath,
                                           const QString &animationName,
                                           int directions,
                                           int frames,
                                           int resolution,
                                           double elevation,
                                           double padding,
                                           double cameraDistance,
                                           double startAzimuth);

signals:
    void selectionChanged();
    void availableAnimationsChanged();
    void isExportingChanged();
    void exportFinished(bool ok, const QString &outputPath, const QString &error);
    void outputPathPickRequested(const QString &startPath);
    void outputPathPicked(const QString &path);

private:
    explicit IsometricSpritesController(QObject *parent = nullptr);
    ~IsometricSpritesController() override = default;

    void setExporting(bool exporting);

    QStringList m_animations;
    bool m_isExporting = false;

    static IsometricSpritesController *s_instance;
};

#endif // ISOMETRIC_SPRITES_CONTROLLER_H
