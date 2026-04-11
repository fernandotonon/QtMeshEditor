#ifndef ASSET_BROWSER_CONTROLLER_H
#define ASSET_BROWSER_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QVariantList>
#include <QFileSystemWatcher>

/**
 * @brief QML_SINGLETON that drives the Asset Browser panel.
 *
 * Exposes a filtered file list from a configurable root directory, watches for
 * filesystem changes, and provides actions to load meshes or apply textures.
 * The last browsed directory is persisted via QSettings.
 */
class AssetBrowserController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString rootPath READ rootPath WRITE setRootPath NOTIFY rootPathChanged)
    Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)

public:
    static AssetBrowserController* instance();
    static AssetBrowserController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    QString rootPath() const;
    void setRootPath(const QString& path);

    QVariantList files() const;

    QString filter() const;
    void setFilter(const QString& filter);

    QString searchQuery() const;
    void setSearchQuery(const QString& query);

    /// Open a native directory picker and set rootPath to the chosen folder.
    Q_INVOKABLE void browseForDirectory();

    /// Load/import the given file (mesh import or texture apply).
    Q_INVOKABLE void openFile(const QString& path);

    /// Navigate into a subdirectory.
    Q_INVOKABLE void navigateToDirectory(const QString& path);

    /// Navigate up one directory level.
    Q_INVOKABLE void navigateUp();

    /// Returns the file-type category for a given file extension.
    Q_INVOKABLE QString fileTypeForPath(const QString& path) const;

    /// Returns a data-URI preview image for a material file, or "" if unavailable.
    Q_INVOKABLE QString materialPreview(const QString& filePath) const;

signals:
    void rootPathChanged();
    void filesChanged();
    void filterChanged();
    void searchQueryChanged();

    /// Emitted when a mesh file should be imported (handled by MainWindow).
    void importMeshRequested(const QStringList& paths);

    /// Emitted when the user clicks "Browse..." so MainWindow opens a native dialog.
    void browseRequested();

private:
    AssetBrowserController();
    ~AssetBrowserController() override;

    static AssetBrowserController* m_pSingleton;

    void refreshFiles();
    void setupWatcher();
    QString classifyExtension(const QString& ext) const;

    QString m_rootPath;
    QString m_filter = "all";  // "all", "meshes", "textures", "materials"
    QString m_searchQuery;
    QVariantList m_files;
    QFileSystemWatcher* m_watcher = nullptr;

    static const QStringList s_meshExtensions;
    static const QStringList s_textureExtensions;
    static const QStringList s_materialExtensions;
};

#endif // ASSET_BROWSER_CONTROLLER_H
