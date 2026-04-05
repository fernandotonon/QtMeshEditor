#ifndef MESHVALIDATOR_H
#define MESHVALIDATOR_H

#include <QObject>
#include <QVariantList>
#include <QQmlEngine>
#include <OgreFrameListener.h>

class MeshValidator : public QObject, public Ogre::FrameListener
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList issues READ issues NOTIFY issuesChanged)
    Q_PROPERTY(bool hasFixableIssues READ hasFixableIssues NOTIFY issuesChanged)
    Q_PROPERTY(bool validated READ validated NOTIFY issuesChanged)

public:
    static MeshValidator* instance();
    static MeshValidator* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasSelection() const;
    QVariantList issues() const { return m_issues; }
    bool hasFixableIssues() const;
    bool validated() const { return m_validated; }

    Q_INVOKABLE void validate();
    // Re-imports the mesh with Assimp cleanup flags to fix degenerate/invalid geometry.
    // Creates a new cleaned entity alongside the original — delete the original manually.
    Q_INVOKABLE void fixAll();

signals:
    void selectionChanged();
    void issuesChanged();
    void fixApplied(const QString& message);
    void error(const QString& message);

private:
    MeshValidator();
    ~MeshValidator() override;

    // Ogre::FrameListener — runs doValidate() the next time the GL context is current.
    // This avoids glMapBufferRange crashes on Linux when validate() is called
    // between render frames (i.e. without an active OpenGL context).
    bool frameStarted(const Ogre::FrameEvent& evt) override;
    void doValidate();

    static MeshValidator* m_pSingleton;
    QVariantList m_issues;
    bool m_validated = false;
    bool m_pendingValidate = false;
    bool m_frameListenerRegistered = false;
};

#endif // MESHVALIDATOR_H
