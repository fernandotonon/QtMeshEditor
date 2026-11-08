#ifndef IMAGE_SOURCE_CONTROLLER_H
#define IMAGE_SOURCE_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QString>

// The SOURCE IMAGE shared by every "from an image" action (Image -> 3D, Character from a photo ...): where the image
// comes from, not what is done with it.
//
//   * pick a file (selectImage) or generate / edit one from a text prompt (generateSourceImage: FLUX.2-klein or any
//     Stable Diffusion checkpoint, through SDManager),
//   * a preview thumbnail, a background SmolVLM caption, a full-size viewer + Save As,
//   * one selection for the whole app: pick it once and either action can use it.
//
// The consumers (MeshGenController::generateSelected, CharacterController::generateFromPhoto, ...) read selectedImagePath().
// While one of them is working it LOCKS the source (setLocked) so the image cannot be swapped under a running job.
// The QML side is qml/ImageSourcePicker.qml, loaded wherever an image is needed.
class ImageSourceController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // The selected image (empty until one is picked or generated).
    Q_PROPERTY(QString selectedImagePath READ selectedImagePath NOTIFY selectedImageChanged)
    // The same as a proper file URL (QUrl::fromLocalFile) for QML Image sources: hand-built "file:///" strings break on Windows.
    Q_PROPERTY(QString selectedImageUrl READ selectedImageUrl NOTIFY selectedImageChanged)
    // A small preview thumbnail as a data:image/png;base64 URL; empty when nothing is selected.
    Q_PROPERTY(QString previewSource READ previewSource NOTIFY selectedImageChanged)
    // SmolVLM caption of the selected image, computed on a worker thread as soon as it is picked (the AI texture pass reuses it).
    Q_PROPERTY(QString caption READ caption NOTIFY captionChanged)
    Q_PROPERTY(bool captioning READ captioning NOTIFY captionChanged)
    // A consumer is working with the image: it cannot be replaced or cleared.
    Q_PROPERTY(bool locked READ locked NOTIFY lockedChanged)

public:
    static ImageSourceController* instance();
    static ImageSourceController* create(QQmlEngine*, QJSEngine*);
    static void kill();

    QString selectedImagePath() const { return m_selectedImage; }
    QString selectedImageUrl() const;
    QString previewSource() const { return m_previewSource; }
    QString caption() const { return m_caption; }
    bool captioning() const { return m_captioning; }
    bool locked() const { return !m_lockOwners.isEmpty(); }

    /// A consumer starts / ends a job on the image (several can hold the lock at once).
    void setLocked(const QObject* owner, bool locked);
    /// Select `path` programmatically (empty = clear): preview, caption, signals.
    void setSelectedImage(const QString& path);

    /// A file dialog (non-native, like the other pickers); stores the choice. Does nothing else.
    Q_INVOKABLE void selectImage();
    Q_INVOKABLE void clearSelectedImage();

    /// Text -> image. generateSourceImage is async: it loads the model if needed, generates, then selects the result exactly as
    /// selectImage() would. With an image already selected the prompt EDITS it (FLUX.2 kontext-style: the prompt describes the
    /// change); with none it generates from scratch. Progress / result arrive through imageGenStatus / imageGenProgress.
    Q_INVOKABLE bool imageGenAvailable() const;
    Q_INVOKABLE QString imageGenModelName() const;
    Q_INVOKABLE void generateSourceImage(const QString& prompt);

    /// Copy the image to `destPath` (a plain path or a file:// URL). A generated image otherwise lives only in AppData under a
    /// timestamped name. False + `error` when nothing is selected or the copy fails.
    Q_INVOKABLE bool saveSelectedImageAs(const QString& destPath);
    /// Ask for a destination with a Save dialog and copy the image there; returns the saved path or empty when cancelled / failed.
    Q_INVOKABLE QString saveSelectedImageInteractive();
    /// A sensible default file name for the save dialog (from the caption / source name; filesystem-safe, ends in .png).
    Q_INVOKABLE QString suggestedImageFileName() const;

signals:
    void selectedImageChanged();
    void captionChanged();
    void lockedChanged();
    void statusMessage(const QString& message);
    void error(const QString& message);
    void imageGenStatus(const QString& message, bool isError);
    void imageGenProgress(int step, int total);

private slots:
    // SDManager signal handlers, gated on m_imageGenActive so an unrelated texture generation never hijacks the selection.
    void onImageGenModelLoaded();
    void onImageGenCompleted(const QString& outputPath);
    void onImageGenError(const QString& message);

private:
    explicit ImageSourceController(QObject* parent = nullptr);

    /// Store `path` as the selected image: preview thumbnail, breadcrumb, signals, background caption.
    void applySelectedImage(const QString& path);
    void startCaptioning(const QString& path);
    Q_INVOKABLE void setCaptionResult(const QString& caption, const QString& forPath);

    /// Native generation resolution for a checkpoint: klein and SDXL train at 1024², SD 1.x at 512².
    static int imageGenSizeForModel(const QString& model);
    /// file:// -> path, and a missing suffix becomes .png (shared so the path the interactive save returns is the one written).
    static QString normalisedSavePath(const QString& destPath);

    QSet<const QObject*> m_lockOwners;
    bool m_imageGenActive = false;   // text -> image in flight
    QString m_imageGenPrompt;        // the (suffixed) prompt being generated
    QString m_imageGenFileName;      // expected output file (signal correlation)
    QString m_imageGenRef;           // edit mode: the image being edited (else empty)
    bool m_imageGenWired = false;    // SDManager signal wiring done once
    int m_imageGenSize = 1024;       // 1024 for FLUX.2, 512 for SD checkpoints
    QString m_selectedImage;
    QString m_previewSource;
    QString m_caption;
    bool m_captioning = false;
    // A stale caption must not land after the image changed again: startCaptioning stamps the path, setCaptionResult drops mismatches.
    QString m_captionForPath;
    static ImageSourceController* s_instance;
};

#endif // IMAGE_SOURCE_CONTROLLER_H
