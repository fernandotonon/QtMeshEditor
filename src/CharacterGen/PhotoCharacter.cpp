#include "PhotoCharacter.h"

#include "BodyShape.h"
#include "PhotoAppearance.h"
#include "Proportions.h"
#include "../BodyFromPhoto/SamBodyPredictor.h"
#include "../FaceRig/FaceLandmarkDetector.h"
#include "../ImageTo3D/BackgroundRemover.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPen>

#include <algorithm>

namespace CharacterGen {
namespace PhotoCharacter {

namespace {
QString hex(const PhotoAppearance::ColourEstimate& c)
{
    return QColor::fromRgbF(double(c.colour[0]), double(c.colour[1]), double(c.colour[2])).name();
}

struct Sink {
    Result* r;
    const std::function<void(const QString&)>* cb;
    void operator()(const QString& s) const
    {
        r->notes << s;
        if (cb && *cb) (*cb)(s);
    }
};

void dumpDebug(const QImage& photo, const SamBody::Result& sr, const PhotoAppearance::FaceAppearance& fa, const QString& name, bool beforeFace)
{
    if (!qEnvironmentVariableIsSet("QTMESH_PHOTO_DEBUG_DIR") || name.isEmpty()) return;
    const QDir dir(qEnvironmentVariable("QTMESH_PHOTO_DEBUG_DIR"));
    if (beforeFace) {
        QImage dbg = photo.convertToFormat(QImage::Format_RGB888);
        QPainter pt(&dbg);
        pt.setPen(QPen(Qt::red, 4));
        pt.drawRect(PhotoAppearance::headBoxFromBody(sr.body.posedVertices, sr.body.camT, sr.body.focalPx, photo.width(), photo.height()));
        pt.setPen(QPen(Qt::green, 4));
        pt.drawRect(sr.bboxUsed);
        pt.end();
        dbg.save(dir.filePath(name + QStringLiteral("_head.png")));
    } else if (!fa.landmarks.empty()) {
        QFile lf(dir.filePath(name + QStringLiteral("_landmarks.f32")));
        if (lf.open(QIODevice::WriteOnly)) lf.write(reinterpret_cast<const char*>(fa.landmarks.data()), qint64(fa.landmarks.size() * sizeof(std::array<float, 3>)));
    }
}

void chooseHairFrom(const PhotoAppearance::FaceAppearance& fa, const Assets& assets, const Options& o, Result& r, const Sink& say)
{
    const bool native = !assets.mhrArkit.empty();
    if (!native) return;
    const PhotoAppearance::HairChoice hc = PhotoAppearance::chooseHair(fa);
    if (!hc.preset.isEmpty() && !o.bundleHair) {
        r.haveHairStyle = true;
        r.hairStyle = QStringLiteral("proc:") + hc.preset;
        r.hairProcedural = Hair::presetParams(hc.preset);
        if (hc.length >= 0.0f) Hair::setParam(r.hairProcedural, QStringLiteral("length"), double(hc.length));
        if (hc.volume >= 0.0f) Hair::setParam(r.hairProcedural, QStringLiteral("volume"), double(hc.volume));
        if (hc.curliness >= 0.0f) Hair::setParam(r.hairProcedural, QStringLiteral("curliness"), double(hc.curliness));
        if (hc.fringe >= 0.0f) Hair::setParam(r.hairProcedural, QStringLiteral("fringe"), double(hc.fringe));
        say(QStringLiteral("Photo hair: %1 (length %2 m, volume %3, curliness %4, fringe %5) from top %6, side %7, lowest %8, fringe %9, coherence %10")
                .arg(r.hairStyle).arg(double(r.hairProcedural.length), 0, 'f', 2).arg(double(r.hairProcedural.volume), 0, 'f', 2).arg(double(r.hairProcedural.curliness), 0, 'f', 2).arg(double(r.hairProcedural.fringe), 0, 'f', 2)
                .arg(double(fa.hairTop), 0, 'f', 2).arg(double(fa.hairSide), 0, 'f', 2).arg(double(fa.hairLowest), 0, 'f', 2).arg(double(fa.hairFringe), 0, 'f', 2).arg(double(fa.hairCoherence), 0, 'f', 2));
    } else {
        static const char* const styles[] = {"", "short02", "bob01", "long01"};
        const QString s = QString::fromLatin1(styles[std::clamp(fa.hairLength, 0, 3)]);
        if (!s.isEmpty() && assets.hairStyle(s)) { r.haveHairStyle = true; r.hairStyle = s; }
    }
}

void fitBody(const SamBody::Result& sr, const Assets& assets, const Options& o, Result& r, const Sink& say)
{
    std::vector<float> samCm(sr.body.neutralVertices);
    for (float& v : samCm) v *= 100.0f;
    r.samBodyCm = samCm;
    if (!o.fitProportions || qEnvironmentVariableIsSet("QTMESH_NO_PROPORTION_FIT")) return;
    BodyShape::Model bm;
    if (!bm.build(assets)) return;
    const auto before = bm.measure(r.bodyShape);
    const BodyShape::BodyFit bf = BodyShape::fitToBody(bm, assets, samCm, r.bodyShape);
    if (!bf.ok) return;
    r.bodyShape = bf.coefficients;
    r.proportions = bf.proportions;
    const auto sam = bm.measurePositions(samCm);
    const auto now = bm.measure(r.bodyShape);
    QString line = QStringLiteral("Body fit (cm) SAM / shape-only -> fitted:");
    for (int m = 0; m < BodyShape::MeasureCount; ++m)
        line += QStringLiteral(" %1 %2/%3->%4").arg(QLatin1String(BodyShape::info(BodyShape::Measure(m)).id)).arg(double(sam[size_t(m)]), 0, 'f', 1).arg(double(before[size_t(m)]), 0, 'f', 1).arg(double(now[size_t(m)]), 0, 'f', 1);
    say(line);
    const Proportions& pr = r.proportions;
    say(QStringLiteral("Proportions: height %1 legs %2 arms %3 torso %4 shoulders %5 head %6").arg(double(pr.height), 0, 'f', 3).arg(double(pr.legs), 0, 'f', 3).arg(double(pr.arms), 0, 'f', 3).arg(double(pr.torso), 0, 'f', 3).arg(double(pr.shoulders), 0, 'f', 3).arg(double(pr.head), 0, 'f', 3));
}
}  // namespace

bool prepareModels(QString* error)
{
    QString err;
    if (!SamBody::ensureModelsBlocking(&err)) {
        if (error) *error = QObject::tr("SAM 3D Body models unavailable: %1").arg(err);
        return false;
    }
    // (best effort: a missing matte or landmark model only costs the appearance estimates, never the body)
    BackgroundRemover::Quality q = BackgroundRemover::Quality::Fast;
    BackgroundRemover::resolveModelBlocking(BackgroundRemover::Quality::Best, &q);
    FaceRig::FaceLandmarkDetector::ensureModelBlocking();
    return true;
}

Result estimate(const QImage& photo, const Assets& assets, const Options& o, const std::function<void(const QString&)>& note)
{
    Result r;
    const Sink say{&r, &note};
    if (photo.isNull()) { r.error = QObject::tr("The image could not be read."); return r; }
    say(QObject::tr("Reading the body from the photo (SAM 3D Body)…"));
    SamBody::Options so;   // the neutral mesh + topology are needed for the clothing regions and the fit
    const SamBody::Result sr = SamBody::predict(photo, so);
    if (!sr.ok) { r.error = sr.error; return r; }
    if (!sr.warning.isEmpty()) say(QStringLiteral("Warning: ") + sr.warning);
    dumpDebug(photo, sr, {}, o.debugName, true);

    say(QObject::tr("Reading skin, hair, eyes and clothes…"));
    QImage matte;
    const QRectF headBox = PhotoAppearance::headBoxFromBody(sr.body.posedVertices, sr.body.camT, sr.body.focalPx, photo.width(), photo.height());
    const PhotoAppearance::FaceAppearance fa = PhotoAppearance::estimate(photo, sr.bboxUsed, headBox, &matte);
    dumpDebug(photo, sr, fa, o.debugName, false);
    if (fa.skin.ok) { r.haveSkin = true; r.skin = fa.skin.colour; }
    if (fa.hair.ok) {
        r.haveHairColour = true;
        r.hairColour = fa.hair.colour;
        chooseHairFrom(fa, assets, o, r, say);
    }
    if (fa.iris.ok) { r.haveEye = true; r.eye = fa.iris.colour; }
    say(QStringLiteral("Photo appearance: face %1, skin %2 (%3 px), hair %4 (%5 px, length %6, side %7), iris %8 %9")
            .arg(fa.faceFound ? QStringLiteral("found") : QStringLiteral("NOT found"), fa.skin.ok ? hex(fa.skin) : QStringLiteral("-")).arg(fa.skin.samples)
            .arg(fa.hair.ok ? hex(fa.hair) : QStringLiteral("-")).arg(fa.hair.samples).arg(fa.hairLength).arg(double(fa.sideHairFraction), 0, 'f', 2)
            .arg(fa.iris.ok ? hex(fa.iris) : QStringLiteral("-"), fa.note));
    if (fa.skin.ok) {
        const PhotoAppearance::ClothingEstimate ce = PhotoAppearance::estimateClothing(photo, matte, r.skin, sr.body.neutralVertices, sr.body.posedVertices, sr.body.triangles, sr.body.camT, sr.body.focalPx);
        if (ce.ok) {
            r.haveClothes = true;
            r.topStyle = ce.top;
            r.bottomStyle = ce.bottom;
            if (ce.topColour.ok) { r.haveTopColour = true; r.topColour = ce.topColour.colour; }
            if (ce.bottomColour.ok) { r.haveBottomColour = true; r.bottomColour = ce.bottomColour.colour; }
            say(QStringLiteral("Photo clothing: top %1 %2, bottom %3 %4").arg(ce.top, ce.topColour.ok ? hex(ce.topColour) : QStringLiteral("-"), ce.bottom, ce.bottomColour.ok ? hex(ce.bottomColour) : QStringLiteral("-")));
        }
    }
    say(QObject::tr("Fitting the body…"));
    r.bodyShape = sr.body.shape;
    fitBody(sr, assets, o, r, say);
    if (o.fitFace) {
        const PhotoAppearance::FaceFit ff = PhotoAppearance::fitFaceCoefficients(fa.landmarks, r.bodyShape);
        if (ff.ok) {
            r.bodyShape = ff.coefficients;
            say(QStringLiteral("Face fit: landmark error %1 -> %2 (of the inter-ocular distance, %3 modes)").arg(double(ff.errBefore), 0, 'f', 4).arg(double(ff.errAfter), 0, 'f', 4).arg(ff.modesUsed));
        } else say(QStringLiteral("Warning: face fit skipped: %1").arg(ff.note));
    }
    r.bodyShape.resize(size_t(std::max(assets.mhrShapes, 0)), 0.0f);
    r.ok = true;
    return r;
}

void apply(const Result& r, Params& p, const Keep& keep)
{
    p.bodyShape = r.bodyShape;
    if (!keep.proportions) p.proportions = r.proportions;
    if (r.haveSkin && !keep.skin) p.skin = r.skin;
    if (r.haveEye && !keep.eye) p.eye = r.eye;
    if (r.haveHairColour && !keep.hairColour) p.hairColour = r.hairColour;
    if (r.haveHairStyle && !keep.hairStyle) {
        p.hairStyle = r.hairStyle;
        if (r.hairStyle.startsWith(QLatin1String("proc:"))) p.hairProcedural = r.hairProcedural;
    }
    if (r.haveClothes && !keep.clothes) {
        p.topStyle = r.topStyle;
        p.bottomStyle = r.bottomStyle;
    }
    if (r.haveTopColour && !keep.clothes && !keep.topColour) p.topColour = r.topColour;
    if (r.haveBottomColour && !keep.clothes && !keep.bottomColour) p.bottomColour = r.bottomColour;
}

}  // namespace PhotoCharacter
}  // namespace CharacterGen
