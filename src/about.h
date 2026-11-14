#ifndef ABOUT_H
#define ABOUT_H

#include <QDialog>

class QLabel;
class QTabWidget;
class QWidget;

/**
 * @brief Help → About: what QtMeshEditor is, the QtMesh ecosystem, and how
 *        to support development. Three tabs, built from the offline
 *        QtMeshEcosystem catalog and styled from the active palette.
 */
class About : public QDialog
{
    Q_OBJECT

public:
    explicit About(QWidget *parent = nullptr);
    ~About() override;

    QString getVersionText() const;

private:
    QWidget* buildAboutTab();
    QWidget* buildEcosystemTab();
    QWidget* buildSupportTab();

    QLabel* m_versionText = nullptr;
    QTabWidget* m_tabs = nullptr;
};

#endif // ABOUT_H
