#include "BatchExporter.h"
#include "CLIPipeline.h"
#include <QFileInfo>
#include <QDir>

BatchExporter::BatchExporter(QObject* parent) : QObject(parent) {}

void BatchExporter::setInputFiles(const QStringList& files) { mInputFiles = files; }
void BatchExporter::setOutputFormat(const QString& format) { mOutputFormat = format; }
void BatchExporter::setOutputDirectory(const QString& dir) { mOutputDir = dir; }

void BatchExporter::execute()
{
    int success = 0, fail = 0;

    for (int i = 0; i < mInputFiles.size(); ++i)
    {
        const QString& file = mInputFiles[i];
        emit progressChanged(i + 1, mInputFiles.size(), file);

        QFileInfo fi(file);
        QString outputPath = mOutputDir.isEmpty()
            ? fi.absolutePath() + "/" + fi.baseName() + "." + mOutputFormat
            : mOutputDir + "/" + fi.baseName() + "." + mOutputFormat;

        // Build argv for CLIPipeline::run()
        QStringList args = {"qtmesh", "convert", file, "-o", outputPath};
        std::vector<char*> argv;
        std::vector<std::string> storage;
        for (const auto& arg : args) {
            storage.push_back(arg.toStdString());
            argv.push_back(const_cast<char*>(storage.back().c_str()));
        }

        int result = CLIPipeline::run(static_cast<int>(argv.size()), argv.data());
        if (result == 0)
            ++success;
        else
        {
            ++fail;
            emit error(file, "Conversion failed");
        }
    }

    emit finished(success, fail);
}
