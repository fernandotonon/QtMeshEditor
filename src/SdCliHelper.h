#pragma once

#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

/// Out-of-process text-to-image through stable-diffusion.cpp's own `sd-cli`
/// (shipped as the `qtmesh_sdcli` helper next to the editor binary).
///
/// WHY a helper: FLUX.3 Image Klein needs a stable-diffusion.cpp newer than the
/// one linked into the editor, and the editor's sd.cpp shares llama.cpp's ggml
/// (SD_USE_UPSTREAM_GGML). Measured: newer sd.cpp on that shared ggml aborts in
/// ggml_backend_sched_split_graph (a stride-0 int32 input rejected by the
/// scheduler) on EVERY model, FLUX.2 included — while the same sd.cpp built
/// with its OWN ggml fork generates FLUX.3 fine. A separate process keeps each
/// side on the ggml it was built against, and a crash cannot take the editor
/// down (same reasoning as `qtmesh_quadriflow`).
///
/// Pure + Qt-only (no Ogre, no sd.cpp headers), so the argument building and
/// progress parsing are unit-tested in every build.
namespace SdCliHelper {

struct Request {
    QString diffusion;      ///< diffusion model (FLUX.3 GGUF)
    QString llm;            ///< text encoder (Qwen3-VL GGUF)
    QString vae;            ///< autoencoder
    QString prompt;
    QString outputPath;     ///< PNG written by the helper
    QString refImagePath;   ///< optional: edit this image (sd-cli -r)
    int width = 1024;
    int height = 1024;
    int steps = 16;         ///< FLUX.3 Klein distilled default
    float cfgScale = 1.0f;  ///< distilled: guidance baked in
    qint64 seed = -1;       ///< <0 = random
    int threads = 0;        ///< 0 = helper default
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    QString error;          ///< user-facing reason when !ok && !cancelled
};

/// `QTMESH_SDCLI_HELPER`, else `qtmesh_sdcli` next to the editor binary (the
/// macOS bundle's Contents/MacOS, Linux /usr/share/qtmesheditor). Empty when
/// the helper is not installed.
QString defaultHelperPath();

/// True when a usable helper binary exists.
bool helperAvailable();

/// The exact argv (without the program) for `sd-cli`. Always euler sampling and
/// the request's cfg/steps — the editor's SD-oriented settings (30 steps,
/// cfg 7, negative prompt) must never reach a distilled flow model.
QStringList buildArguments(const Request& req);

/// Parses one progress fragment from sd-cli's progress bar
/// ("  |=====>   | 12/16 - 1.23s/it"). Returns false for anything else.
bool parseProgress(const QByteArray& fragment, int* step, int* total);

/// Runs the helper to completion. `progress(step,total)` fires only for the
/// sampling bar (total == req.steps) — sd-cli also draws a tensor-loading bar
/// that must not be mistaken for sampling. Setting `stop` kills the process.
Result run(const Request& req,
           const QString& helperPath,
           const std::function<void(int, int)>& progress,
           const std::atomic<bool>& stop);

} // namespace SdCliHelper
