#ifndef ASSETSCANCONTROLLER_H
#define ASSETSCANCONTROLLER_H

#include <QObject>
#include <QVariantList>
#include <QQmlEngine>

class AssetScanController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QStringList profileIds READ profileIds NOTIFY profilesChanged)
    Q_PROPERTY(QStringList profileLabels READ profileLabels NOTIFY profilesChanged)
    Q_PROPERTY(QString selectedProfileId READ selectedProfileId WRITE setSelectedProfileId
               NOTIFY selectedProfileIdChanged)
    Q_PROPERTY(QString profileDescription READ profileDescription NOTIFY selectedProfileIdChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(bool hasResults READ hasResults NOTIFY resultsChanged)
    Q_PROPERTY(int summaryScanned READ summaryScanned NOTIFY resultsChanged)
    Q_PROPERTY(int summaryPassed READ summaryPassed NOTIFY resultsChanged)
    Q_PROPERTY(int summaryWarnings READ summaryWarnings NOTIFY resultsChanged)
    Q_PROPERTY(int summaryErrors READ summaryErrors NOTIFY resultsChanged)
    Q_PROPERTY(QVariantList findings READ findings NOTIFY resultsChanged)

public:
    static AssetScanController* instance();
    static AssetScanController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    QStringList profileIds() const { return m_profileIds; }
    QStringList profileLabels() const { return m_profileLabels; }
    QString selectedProfileId() const { return m_selectedProfileId; }
    QString profileDescription() const { return m_profileDescription; }
    bool scanning() const { return m_scanning; }
    bool hasResults() const { return m_hasResults; }
    int summaryScanned() const { return m_summaryScanned; }
    int summaryPassed() const { return m_summaryPassed; }
    int summaryWarnings() const { return m_summaryWarnings; }
    int summaryErrors() const { return m_summaryErrors; }
    QVariantList findings() const { return m_findings; }

    void setSelectedProfileId(const QString& id);

    /// Scan @p rootPath with the selected platform profile (mirrors `qtmesh scan --target`).
    Q_INVOKABLE void scanFolder(const QString& rootPath);

    /// Resolve the CLI binary used for isolated subprocess scans (test seam).
    static QString resolveCliBinaryForTest();

    /// Parse `--json` scan stdout into summary + findings (test seam).
    static bool parseScanJsonReport(const QByteArray& jsonBytes,
                                    int* scanned, int* passed, int* warnings, int* errors,
                                    QVariantList* findings, QString* errorOut);

#ifdef QTMESH_UNIT_TESTS
    /// Test seam for applyScanReport (summary/findings properties).
    void ingestScanReportJsonForTest(const QByteArray& jsonBytes);
    static QString sanitizeProfileLabelForTest(const QString& displayName, const QString& id);
#endif

private:
    explicit AssetScanController(QObject* parent = nullptr);
    ~AssetScanController() override = default;

    void reloadProfiles();
    void updateProfileDescription();
    void setScanning(bool scanning);
    void applyScanReport(const QByteArray& jsonBytes);

    static AssetScanController* m_pSingleton;

    QStringList m_profileIds;
    QStringList m_profileLabels;
    QString m_selectedProfileId;
    QString m_profileDescription;
    bool m_scanning = false;
    bool m_hasResults = false;
    int m_summaryScanned = 0;
    int m_summaryPassed = 0;
    int m_summaryWarnings = 0;
    int m_summaryErrors = 0;
    QVariantList m_findings;

signals:
    void profilesChanged();
    void selectedProfileIdChanged();
    void scanningChanged();
    void resultsChanged();
    void scanFinished(bool ok, const QString& message);
    void error(const QString& message);
};

#endif // ASSETSCANCONTROLLER_H
