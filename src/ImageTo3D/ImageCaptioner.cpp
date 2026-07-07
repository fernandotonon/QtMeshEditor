#include "ImageCaptioner.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#ifdef ENABLE_MTMD
#include "ModelDownloader.h"
#include <QEventLoop>
#include <QObject>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include <llama.h>
#include <mtmd.h>
#include <mtmd-helper.h>

#include <string>
#include <vector>
#endif

namespace ImageCaptioner {

namespace {
// SmolVLM-500M-Instruct (Apache-2.0), the model + its vision projector. Hosted
// on the QtMeshEditor models repo under caption/ (mirrored from ggml-org).
// It's the smallest/fastest tier — captions are terse ("a rabbit"). For richer
// descriptions a future "quality" tier can swap in Moondream2 (~1.7 GB) or
// Qwen2-VL-2B (~1.5 GB), both Apache-2.0 + llama.cpp libmtmd-supported (see the
// #764 captioner research); the detail-seeking prompt in the header pushes the
// 500M model as far as it goes without the extra download.
constexpr const char* kModelFile  = "SmolVLM-500M-Instruct-Q8_0.gguf";
constexpr const char* kMmprojFile = "mmproj-SmolVLM-500M-Instruct-Q8_0.gguf";
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/caption/";
constexpr const char* kBaseUrlSettingsKey = "ai/captionModelBaseUrl";

QString modelDir()
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("ai_models/caption/"));
}
} // namespace

QString modelPath()  { return QDir(modelDir()).filePath(QString::fromLatin1(kModelFile)); }
QString mmprojPath() { return QDir(modelDir()).filePath(QString::fromLatin1(kMmprojFile)); }

#ifdef ENABLE_MTMD

bool isAvailable() { return true; }

bool modelsPresent()
{
    return QFileInfo::exists(modelPath()) && QFileInfo::exists(mmprojPath());
}

QString ensureModelBlocking()
{
    if (modelsPresent())
        return modelPath();
    if (!qEnvironmentVariableIsEmpty("QTMESH_CAPTION_NO_DOWNLOAD"))
        return {};

    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_CAPTION_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty()) return {};
    if (!base.endsWith('/')) base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl) return {};

    auto downloadOne = [&](const char* fileName, const QString& dest) -> bool {
        QDir().mkpath(QFileInfo(dest).absolutePath());
        const QString label =
            QStringLiteral("Caption %1").arg(QString::fromLatin1(fileName));
        const QString url = base + QString::fromLatin1(fileName);
        QEventLoop loop;
        bool ok = false, timedOut = false;
        auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) { ok = true; loop.quit(); }
            });
        auto onErr = QObject::connect(dl, &ModelDownloader::downloadError, &loop,
            [&](const QString& name, const QString&) {
                if (name == label) { ok = false; loop.quit(); }
            });
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop,
            [&]() { timedOut = true; loop.quit(); });
        timeout.start(1800000);   // 30 min — the pair is < 600 MB
        dl->startDownload(url, dest, label);
        loop.exec();
        QObject::disconnect(onDone);
        QObject::disconnect(onErr);
        if (timedOut) dl->cancelDownload();
        return ok && !timedOut && QFileInfo::exists(dest);
    };

    if (!QFileInfo::exists(modelPath())  && !downloadOne(kModelFile,  modelPath()))  return {};
    if (!QFileInfo::exists(mmprojPath()) && !downloadOne(kMmprojFile, mmprojPath())) return {};
    return modelsPresent() ? modelPath() : QString();
}

QString caption(const QImage& image, const QString& prompt)
{
    if (image.isNull() || !modelsPresent())
        return {};

    // ---- Load the text model + its mtmd (vision projector) context ----------
    llama_model_params mparams = llama_model_default_params();
    // Vision projector runs on CPU/accelerator via ggml; the small LM can put a
    // few layers on GPU where available. Keep it modest — captioning is one-shot.
    mparams.n_gpu_layers = 0;
    llama_model* model = llama_model_load_from_file(
        modelPath().toUtf8().constData(), mparams);
    if (!model) return {};

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 4096;   // ample for image tokens + a short caption
    int threads = QThread::idealThreadCount();
    if (threads <= 0) threads = 4;
    if (threads > 16) threads = 16;
    cparams.n_threads = threads;
    cparams.n_threads_batch = threads;
    llama_context* lctx = llama_init_from_model(model, cparams);
    if (!lctx) { llama_model_free(model); return {}; }

    mtmd_context_params mparams2 = mtmd_context_params_default();
    mparams2.use_gpu = false;   // small projector; CPU is fine + portable
    mparams2.print_timings = false;
    mparams2.n_threads = threads;
    mtmd_context* mctx = mtmd_init_from_file(
        mmprojPath().toUtf8().constData(), model, mparams2);
    if (!mctx || !mtmd_support_vision(mctx)) {
        if (mctx) mtmd_free(mctx);
        llama_free(lctx);
        llama_model_free(model);
        return {};
    }

    QString result;
    do {
        // ---- Wrap the RGB image as an mtmd_bitmap (RGBRGB… bytes) -----------
        const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
        std::vector<unsigned char> pix(static_cast<size_t>(rgb.width())
                                       * rgb.height() * 3);
        for (int y = 0; y < rgb.height(); ++y) {
            const uchar* line = rgb.constScanLine(y);
            std::copy(line, line + rgb.width() * 3,
                      pix.data() + static_cast<size_t>(y) * rgb.width() * 3);
        }
        mtmd_bitmap* bmp = mtmd_bitmap_init(
            static_cast<uint32_t>(rgb.width()),
            static_cast<uint32_t>(rgb.height()), pix.data());
        if (!bmp) break;

        // ---- Build the prompt with the media marker + tokenize --------------
        const QString instruction = prompt.isEmpty()
            ? QString::fromLatin1(kDefaultPrompt) : prompt;
        // SmolVLM (idefics3) chat format; the media marker is replaced by the
        // image chunk during tokenization.
        const std::string text =
            std::string("<|im_start|>User: ") + mtmd_default_marker()
            + instruction.toStdString() + "<end_of_utterance>\nAssistant:";

        mtmd_input_text itext;
        itext.text = text.c_str();
        itext.add_special = true;
        itext.parse_special = true;

        mtmd_input_chunks* chunks = mtmd_input_chunks_init();
        const mtmd_bitmap* bmps[1] = { bmp };
        const int32_t trc = mtmd_tokenize(mctx, chunks, &itext, bmps, 1);
        if (trc != 0) {
            mtmd_input_chunks_free(chunks);
            mtmd_bitmap_free(bmp);
            break;
        }

        // ---- Evaluate text + image chunks (runs the vision encoder) ---------
        llama_pos n_past = 0;
        const int32_t erc = mtmd_helper_eval_chunks(
            mctx, lctx, chunks, /*n_past=*/0, /*seq_id=*/0,
            /*n_batch=*/2048, /*logits_last=*/true, &n_past);
        mtmd_input_chunks_free(chunks);
        mtmd_bitmap_free(bmp);
        if (erc != 0) break;

        // ---- Greedy decode the caption --------------------------------------
        const llama_vocab* vocab = llama_model_get_vocab(model);
        llama_sampler* smpl = llama_sampler_chain_init(
            llama_sampler_chain_default_params());
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

        std::string out;
        for (int i = 0; i < 96; ++i) {   // room for a detailed comma-list
            const llama_token tok = llama_sampler_sample(smpl, lctx, -1);
            if (llama_vocab_is_eog(vocab, tok)) break;
            char buf[256];
            const int n = llama_token_to_piece(vocab, tok, buf, sizeof(buf), 0, true);
            if (n > 0) out.append(buf, n);
            llama_batch nb = llama_batch_get_one(const_cast<llama_token*>(&tok), 1);
            if (llama_decode(lctx, nb) != 0) break;
        }
        llama_sampler_free(smpl);

        result = QString::fromStdString(out).trimmed();
        // Strip any stray end-marker / role tokens the model may echo.
        result.remove(QStringLiteral("<end_of_utterance>"));
        result = result.trimmed();
    } while (false);

    mtmd_free(mctx);
    llama_free(lctx);
    llama_model_free(model);
    return result;
}

#else  // !ENABLE_MTMD

bool    isAvailable()        { return false; }
bool    modelsPresent()      { return false; }
QString ensureModelBlocking(){ return {}; }
QString caption(const QImage&, const QString&) { return {}; }

#endif // ENABLE_MTMD

} // namespace ImageCaptioner
