#ifndef WELCOMEDIALOG_H
#define WELCOMEDIALOG_H

#include <QDialog>
#include <QStringList>

class WelcomeDialog : public QDialog
{
    Q_OBJECT
public:
    enum Result { Dismissed, NewScene, OpenFile, OpenRecent };

    explicit WelcomeDialog(QWidget* parent = nullptr);

    Result userAction() const { return m_action; }
    QString selectedFile() const { return m_selectedFile; }

    /// Returns true if dialog should be shown (checks QSettings "WelcomeScreen/dontShowAgain")
    static bool shouldShow();

private:
    Result m_action = Dismissed;
    QString m_selectedFile;
};

#endif // WELCOMEDIALOG_H
