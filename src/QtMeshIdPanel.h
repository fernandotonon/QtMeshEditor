#ifndef QTMESH_ID_PANEL_H
#define QTMESH_ID_PANEL_H

#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

/**
 * @brief Compact QtMesh ID account block for the Welcome dialog header.
 *
 * Shows the real QtMesh ID state (QtMeshId) and follows it live:
 *  - signed out / expired: what the account is for, that it is optional,
 *    "Sign in with QtMesh ID" and "Create a free account";
 *  - connecting: an unobtrusive status line (sign-in itself runs in the
 *    existing browser device-code prompt);
 *  - signed in: avatar initials, "Connected as <name>", Manage Account and
 *    Sign out.
 * No credential ever passes through this widget.
 */
class QtMeshIdPanel : public QWidget
{
    Q_OBJECT
public:
    /// @p headerNote is shown right-aligned on the "QtMesh ID" heading row
    /// (the Welcome dialog puts the version there to save a line).
    explicit QtMeshIdPanel(const QString& surface, QWidget* parent = nullptr,
                           const QString& headerNote = QString());

    /// Rebuilds the contents for the current QtMeshId state.
    void rebuild();

private:
    void clear();
    QPushButton* textButton(const QString& text, const QString& objectName);

    QString m_surface;
    QString m_headerNote;
    QVBoxLayout* m_layout = nullptr;
};

#endif // QTMESH_ID_PANEL_H
