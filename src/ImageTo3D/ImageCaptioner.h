#ifndef IMAGE_CAPTIONER_H
#define IMAGE_CAPTIONER_H

#include <QImage>
#include <QString>

// Local image captioning for the image-to-3D "describe-then-generate" texture
// path (epic #764). Given a single RGB image of an isolated object, produces a
// short natural-language description (e.g. "a brown rabbit sitting") to feed
// Stable Diffusion + depth-ControlNet as the texture prompt — so ALL views
// (front + back) are generated from the caption and no photo projection is
// needed (which sidesteps the front/back orientation + registration issues).
//
// Backend: llama.cpp's multimodal `libmtmd` running a SMALL, permissively-
// licensed vision-language model — **SmolVLM-500M-Instruct (Apache-2.0)** by
// default (~550 MB: the GGUF + its mmproj projector). Reuses the existing
// llama.cpp integration (no new runtime; works on macOS/Windows-MinGW/Linux)
// and the ModelDownloader / HF-repo first-use download pattern.
//
// Guarded by ENABLE_LOCAL_LLM. Synchronous + blocking (like the SD CLI
// drivers) — caption() loads model + mmproj, runs one image, returns the text.
// Pure-data (no Ogre); the caption is fed to MaterialEditorQML's multi-view
// texture pass. When the build lacks llama.cpp or the model is unavailable,
// isAvailable()/ensureModelBlocking() report it and caption() returns empty so
// the caller can fall back to a manual/neutral prompt.
namespace ImageCaptioner {

// True only when built with ENABLE_LOCAL_LLM.
static constexpr const char* kDefaultPrompt =
    "Describe the main object in this image in a short phrase for a texture "
    "prompt: its type, colours, and materials. Answer with only the phrase.";

bool isAvailable();

// AppData/ai_models/caption/ paths for the two GGUF files.
QString modelPath();     // the SmolVLM instruct GGUF
QString mmprojPath();    // its vision projector (mmproj) GGUF
bool    modelsPresent();

// Ensure both files exist, downloading on first use (blocks via a local event
// loop). Returns modelPath() when present, else empty. Honours
// QTMESH_CAPTION_NO_DOWNLOAD and the base-URL override
// (QTMESH_CAPTION_MODEL_BASE_URL / QSettings ai/captionModelBaseUrl).
QString ensureModelBlocking();

// Caption `image` (RGB; background-removed subject works best). `prompt`
// overrides the default instruction. Returns a trimmed one-line description,
// or empty on any failure (no model, load error, etc.). Blocking; call off the
// UI thread if responsiveness matters.
QString caption(const QImage& image, const QString& prompt = QString());

} // namespace ImageCaptioner

#endif // IMAGE_CAPTIONER_H
