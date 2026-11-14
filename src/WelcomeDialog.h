#ifndef WELCOMEDIALOG_H
#define WELCOMEDIALOG_H

#include <QDialog>
#include <QFileDialog>
#include <QStringList>
#include <QVariantMap>

#include <functional>

class QVBoxLayout;

/**
 * @brief The startup Welcome screen (shown by main.cpp before MainWindow).
 *
 * This is the ACTIVE welcome implementation: it runs before the main window,
 * Ogre and the QML singletons exist, which is why it is Qt Widgets. The QML
 * overlay (qml/WelcomeScreen.qml) is a programmatic-only fallback that shares
 * the same ecosystem catalog (QtMeshEcosystem) so both say the same thing.
 */
class WelcomeDialog : public QDialog
{
    Q_OBJECT
public:
    enum Result { Dismissed, NewScene, OpenFile, OpenRecent };

    explicit WelcomeDialog(QWidget* parent = nullptr);

    Result userAction() const { return m_action; }
    QString selectedFile() const { return m_selectedFile; }

    /// Shows the "try this next" discovery card (GamificationManager::suggestion
    /// map: featureKey/title/blurb/personalized). An empty map hides it.
    /// @p onDismiss is called with the feature key when the user hides it.
    void setDiscoverySuggestion(const QVariantMap& suggestion,
                                std::function<void(const QString&)> onDismiss = {});

    /// Returns true if dialog should be shown (checks QSettings "WelcomeScreen/dontShowAgain")
    static bool shouldShow();
    static QFileDialog::Options openFileDialogOptions();

private:
    QWidget* buildStartColumn();
    QWidget* buildTipsColumn();
    QWidget* buildShowcase();

    Result m_action = Dismissed;
    QString m_selectedFile;
    QVBoxLayout* m_suggestionHost = nullptr;
};

#endif // WELCOMEDIALOG_H
