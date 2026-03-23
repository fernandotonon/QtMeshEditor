#ifndef BATCH_EXPORTER_H
#define BATCH_EXPORTER_H

#include <QObject>
#include <QStringList>

class BatchExporter : public QObject
{
    Q_OBJECT

public:
    explicit BatchExporter(QObject* parent = nullptr);

    void setInputFiles(const QStringList& files);
    void setOutputFormat(const QString& format);
    void setOutputDirectory(const QString& dir);

    Q_INVOKABLE void execute();

signals:
    void progressChanged(int current, int total, const QString& currentFile);
    void finished(int successCount, int failCount);
    void error(const QString& file, const QString& message);

private:
    QStringList mInputFiles;
    QString mOutputFormat;
    QString mOutputDir;
};

#endif // BATCH_EXPORTER_H
