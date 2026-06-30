#pragma once

#include "HDR/HdrIblPrecompute.h"
#include "HDR/HdrEquirectLoader.h"

#include <QObject>
#include <QString>

/// Worker-thread IBL precompute (#468). Mirrors SDWorker's moveToThread pattern.
class HdrPrecomputeWorker : public QObject
{
    Q_OBJECT

public:
    explicit HdrPrecomputeWorker(QObject* parent = nullptr);

public slots:
    void precomputeIbl(const QString& cacheKey,
                       HdrEquirect::CubemapFaces environmentFaces,
                       quint64 generation);

signals:
    void precomputeCompleted(HdrIbl::IblBakeResult result,
                             bool fromDiskCache,
                             qint64 elapsedMs,
                             quint64 generation);
    void precomputeError(QString error, quint64 generation);

private:
};
