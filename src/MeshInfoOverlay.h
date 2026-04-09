#ifndef MESHINFOOVERLAY_H
#define MESHINFOOVERLAY_H

#include <QObject>
#include <QList>
#include <QLabel>
#include <QPointer>
#include <Ogre.h>

class OgreWidget;
class QMainWindow;

class MeshInfoOverlay : public QObject
{
    Q_OBJECT
public:
    explicit MeshInfoOverlay(QMainWindow* parent);
    ~MeshInfoOverlay() override;

    bool isVisible() const { return mVisible; }

    /// Build overlay text from a list of entities (pure data, testable without GUI).
    static QString formatStats(const QList<Ogre::Entity*>& entities, bool isSelection);

signals:
    void visibilityChanged(bool visible);

public slots:
    void setVisible(bool visible);
    void refresh();
    void setActiveWidget(OgreWidget* widget);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void ensureLabel();
    void repositionLabel();
    QList<Ogre::Entity*> collectEntities(bool& isSelection) const;

    QMainWindow* mMainWindow;
    bool mVisible = false;
    QPointer<QLabel> mLabel;                  // top-level transparent overlay window
    QPointer<OgreWidget> mActiveWidget;       // currently focused viewport
};

#endif // MESHINFOOVERLAY_H
