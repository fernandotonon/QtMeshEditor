#ifndef BACKGROUND_REMOVER_H
#define BACKGROUND_REMOVER_H

#include <QImage>
#include <QString>

// AI background removal (epic #764 support): segment the foreground object out of
// a photo so image-to-3D (TripoSR) sees a clean, isolated subject. TripoSR is
// trained on background-removed images and produces garbage on busy backgrounds;
// this is the same pre-process rembg/TripoSR's own gradio demo run.
//
// The SIXTH ONNX consumer. Uses **U²-Net** (Qin et al., "U²-Net: Going Deeper
// with Nested U-Structure for Salient Object Detection") — the salient-object
// model rembg ships by default. **Apache-2.0** code + permissively-released
// weights, so it clears QtMeshEditor's permissive-redistribution bar (same reason
// TripoSR/UniRig passed). See THIRD_PARTY_AI_MODELS.md.
//
// Ogre-free + Qt-only (QImage in/out), same shape as PbrMapSynth so it unit-tests
// without a GL context. ENABLE_ONNX-guarded; without it isAvailable() is false and
// removeBackground() returns the input unchanged (with ok=false + a reason) so the
// caller can proceed on an already-clean image.
//
// Model contract (u2net.onnx): input float32 [1,3,320,320] (RGB, resized,
// per-channel ImageNet-normalized), output float32 [1,1,320,320] saliency in
// [0,1]. The mask is resized back to the source and applied as alpha; the result
// is composited over a solid background (white by default) since TripoSR wants an
// opaque isolated subject, not transparency.
class BackgroundRemover {
public:
    struct Options {
        Options();
        // Composite the cut-out subject over this solid color (TripoSR expects an
        // opaque image). White matches TripoSR's demo. Alpha ignored.
        int bgR = 255, bgG = 255, bgB = 255;
        // Saliency threshold [0..1]; pixels below are treated as background.
        float threshold = 0.5f;
        // Feather the mask edge (px) to avoid a hard cut halo.
        int feather = 2;
    };

    struct Result {
        bool ok = false;
        QString error;
        QImage image;      // the composited, background-removed RGB image
        bool usedModel = false;
    };

    // True only when built with ENABLE_ONNX.
    static bool isAvailable();

    // AppData/ai_models/rembg/u2net.onnx.
    static QString modelPath();
    static bool modelPresent();
    // Download the model on first use (blocks via a local event loop). Returns the
    // path when present, else empty. Honours QTMESH_REMBG_NO_DOWNLOAD + the
    // base-URL override (QTMESH_REMBG_MODEL_BASE_URL / QSettings ai/rembgModelBaseUrl).
    static QString ensureModelBlocking();

    // Run U²-Net on `image`, apply the mask as alpha, composite over the solid
    // background, and return the cleaned image. On any failure (no ONNX, missing
    // model, inference error) returns ok=false with the ORIGINAL image in
    // `image`, so the caller can fall back to using the input as-is.
    static Result removeBackground(const QImage& image,
                                   const QString& modelPath,
                                   const Options& opts = {});
};

#endif // BACKGROUND_REMOVER_H
