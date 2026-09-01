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
        // Composite the cut-out subject over this solid color. TripoSR is trained
        // with the background filled to NEUTRAL GRAY 128 (run.py: (1-alpha)*0.5) —
        // NOT white. White gets reconstructed as a solid wall of geometry behind
        // the subject, so the default is gray.
        int bgR = 128, bgG = 128, bgB = 128;
        // Saliency threshold [0..1]; pixels below are treated as background.
        float threshold = 0.5f;
        // Feather the mask edge to avoid a hard cut halo: 0 = hard threshold,
        // otherwise the alpha ramps over a band of ±0.075*feather (in mask
        // saliency units) around `threshold`. Default 2 → ±0.15 band.
        int feather = 2;
        // Crop to the subject's bounding box and re-pad so the foreground fills
        // this fraction of the (square) output — TripoSR's resize_foreground step
        // (default 0.85). Centering + tight framing is what stops the leftover
        // margin being reconstructed as background geometry. 0 disables cropping.
        float foregroundRatio = 0.85f;
        // Emit an RGBA image carrying the segmentation as a real ALPHA MATTE
        // instead of compositing over the solid background. The TRELLIS.2
        // backend needs this: its pipeline consumes RGBA-with-alpha directly
        // (and a genuine matte keeps the upstream default remover — the
        // non-commercial briaai/RMBG-2.0 — from ever loading; see
        // docs/trellis2-dependencies.md). Default off preserves the TripoSR
        // behaviour.
        bool keepAlpha = false;
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
