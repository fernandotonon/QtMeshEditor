#include "HDR/HdrPrecomputeWorker.h"

#include "HDR/HdrCache.h"

#include <QElapsedTimer>

HdrPrecomputeWorker::HdrPrecomputeWorker(QObject* parent)
    : QObject(parent)
{
}

void HdrPrecomputeWorker::precomputeIbl(const QString& cacheKey,
                                        HdrEquirect::CubemapFaces environmentFaces,
                                        quint64 generation)
{
    QElapsedTimer timer;
    timer.start();

    HdrIbl::IblBakeResult result;
    QString error;

    if (HdrCache::isValid(cacheKey)) {
        if (HdrCache::load(cacheKey, result, error)) {
            emit precomputeCompleted(result, true, timer.elapsed(), generation);
            return;
        }
        HdrCache::invalidate(cacheKey);
    }

    if (!HdrIbl::bakeAll(environmentFaces, result, error)) {
        emit precomputeError(error, generation);
        return;
    }

    QString saveError;
    if (!HdrCache::save(cacheKey, result, saveError)) {
        emit precomputeError(saveError, generation);
        return;
    }

    emit precomputeCompleted(result, false, timer.elapsed(), generation);
}
