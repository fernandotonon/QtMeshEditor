#ifndef CLOUD_ACCOUNT_MENU_BUTTON_H
#define CLOUD_ACCOUNT_MENU_BUTTON_H

#include <QWidget>

class QAction;
class QLabel;
class QMenu;
class QToolButton;
class QWidgetAction;

/// VS Code-style QtMesh Cloud account control: avatar button + popup menu.
/// Self-contained so MainWindow can host it on the objects toolbar today and
/// move it to a top bar later without rewiring auth logic.
class CloudAccountMenuButton : public QWidget {
    Q_OBJECT

public:
    explicit CloudAccountMenuButton(QWidget* parent = nullptr);

    QToolButton* toolButton() const { return m_button; }
    QMenu* menu() const { return m_menu; }

    /// Reads CloudCredentialStore / QSettings and updates button + menu visibility.
    void refresh();

    /// Exposed for unit tests.
    static QString initialsFromDisplayName(const QString& displayName);

signals:
    void signInRequested();
    void signOutRequested();
    void uploadFilesRequested();
    void openProjectsRequested();
    void feedbackRequested();

private:
    class AvatarButton;

    void buildMenu();
    void applyMenuStyle();
    void updateHeader(const QString& displayName, bool signedIn);

    QToolButton* m_button = nullptr;
    QMenu* m_menu = nullptr;
    QWidget* m_headerWidget = nullptr;
    QLabel* m_headerNameLabel = nullptr;
    QLabel* m_headerSubtitleLabel = nullptr;
    QWidgetAction* m_headerAction = nullptr;
    QAction* m_headerSeparator = nullptr;
    QAction* m_mainSeparator = nullptr;
    QAction* m_signInAction = nullptr;
    QAction* m_signOutAction = nullptr;
    QAction* m_uploadAction = nullptr;
    QAction* m_feedbackAction = nullptr;
    QAction* m_openProjectsAction = nullptr;
};

#endif
