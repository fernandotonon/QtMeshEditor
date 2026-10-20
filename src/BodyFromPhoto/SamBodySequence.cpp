#include "SamBodySequence.h"

#include <algorithm>

namespace SamBody {

QImage downscaledForCapture(const QImage& image, int maxEdge)
{
    if (image.isNull() || std::max(image.width(), image.height()) <= maxEdge) return image;
    return image.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

MhrSequence solveSequence(Runner& runner, const std::vector<QImage>& images, const std::vector<double>& times,
                          const SequenceProgress& progress, const QRectF& box, const QString& matteModel)
{
    MhrSequence seq;
    if (images.size() != times.size() || images.size() < 2) {
        seq.error = QStringLiteral("need at least 2 frames");
        return seq;
    }
    if (!isAvailable()) {
        seq.error = QStringLiteral("SAM 3D Body needs an ONNX build (rebuild with -DENABLE_ONNX)");
        return seq;
    }
    QString restError;
    if (!runner.restPose(seq.restPos, seq.restQuat, &restError)) {
        seq.error = restError.isEmpty() ? QStringLiteral("the MHR body model is unavailable") : restError;
        return seq;
    }

    const int total = static_cast<int>(images.size());
    QStringList lastProblem;
    for (int i = 0; i < total; ++i) {
        if (progress && !progress(i, total, QStringLiteral("frame %1 / %2").arg(i + 1).arg(total))) {
            seq.error = QStringLiteral("cancelled");
            return seq;
        }
        Options o;
        o.wantMesh = false;
        o.bbox = box;
        o.matteModel = matteModel;
        const Result r = runner.predict(images[size_t(i)], o);
        if (r.ok && r.personFound) {
            seq.jointQuat.push_back(r.body.jointQuat);
            seq.jointPos.push_back(r.body.jointPos);
            seq.times.push_back(times[size_t(i)]);
            ++seq.solved;
        } else {
            ++seq.dropouts;
            // A dropout holds the previous pose, so the frame keeps its slot and the take its duration and timing. Before the
            // first solved frame there is nothing to hold: skip it.
            if (!seq.jointQuat.empty()) {
                seq.jointQuat.push_back(seq.jointQuat.back());
                seq.jointPos.push_back(seq.jointPos.back());
                seq.times.push_back(times[size_t(i)]);
            }
        }
    }
    if (progress) progress(total, total, QStringLiteral("done"));
    if (seq.solved < 2)
        seq.error = QStringLiteral("a person was found in only %1 of %2 frames (need at least 2)").arg(seq.solved).arg(total);
    return seq;
}

} // namespace SamBody
