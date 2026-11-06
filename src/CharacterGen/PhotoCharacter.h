// Photo -> character parameters: the whole pipeline behind `qtmesh character --photo` and the panel's "From a photo" block.
// SAM 3D Body gives the body, PhotoAppearance the colours / hair / clothes / face projection, BodyShape + Proportions the fit to
// SAM's neutral body. estimate() is a pure function of (photo, assets, options): run it on a worker thread after prepareModels()
// has fetched what it needs on the main thread (a model download needs the downloader's thread).
#pragma once

#include "CharacterModel.h"

#include <QImage>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace CharacterGen {
namespace PhotoCharacter {

struct Options {
    bool fitFace = false;           ///< experimental: refine the face modes against the photo's landmarks
    bool fitProportions = true;     ///< match SAM's neutral body (widths through the coefficients, lengths through the proportions)
    bool bundleHair = false;        ///< choose one of the bundled hairstyles instead of a Hair Studio preset
    QString debugName;              ///< with QTMESH_PHOTO_DEBUG_DIR: base name of the overlay / landmark dumps
};

struct Result {
    bool ok = false;
    QString error;
    QStringList notes;              ///< what was measured, one line each (also sent to the progress callback)
    std::vector<float> bodyShape;   ///< the MHR coefficients after the fit
    Proportions proportions;
    bool haveSkin = false, haveEye = false, haveHairColour = false, haveHairStyle = false, haveClothes = false;
    bool haveTopColour = false, haveBottomColour = false;
    Rgb skin{}, eye{}, hairColour{}, topColour{}, bottomColour{};
    QString hairStyle;              ///< "proc:<preset>" or a bundle style
    Hair::Params hairProcedural;
    QString topStyle, bottomStyle;
    std::vector<float> samBodyCm;   ///< SAM's neutral body (cm, the MHR vertex order), for comparisons
};

/// What the caller set explicitly and the photo must not overwrite.
struct Keep {
    bool skin = false, eye = false, hairColour = false, hairStyle = false, clothes = false, topColour = false, bottomColour = false, proportions = false;
};

/// Fetch every model the pipeline needs (SAM 3D Body, the matte, the face landmarks). MAIN THREAD; may download.
bool prepareModels(QString* error);

/// The pipeline. The models must be present (prepareModels). Safe on a worker thread.
Result estimate(const QImage& photo, const Assets& assets, const Options& options, const std::function<void(const QString&)>& note = {});

/// Copy the estimate into `p` (the body coefficients always; the rest unless `keep` says the caller chose it).
void apply(const Result& r, Params& p, const Keep& keep = {});

}  // namespace PhotoCharacter
}  // namespace CharacterGen
