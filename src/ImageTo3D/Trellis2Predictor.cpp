#include "Trellis2Predictor.h"

#include "BackgroundRemover.h"
#include "Trellis2Bake.h"
#include "Trellis2Interchange.h"

#include <QCoreApplication>
#include <QThread>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

constexpr const char* kEnvDirVar     = "QTMESH_TRELLIS2_ENV";
constexpr const char* kEnvPythonVar  = "QTMESH_TRELLIS2_PYTHON";
constexpr const char* kDirSettingsKey    = "ai/trellis2Env";
constexpr const char* kPythonSettingsKey = "ai/trellis2Python";
constexpr const char* kEnvCliVar        = "QTMESH_TRELLIS2_CLI";
constexpr const char* kEnvCliModelsVar  = "QTMESH_TRELLIS2_CLI_MODELS";
constexpr const char* kCliSettingsKey       = "ai/trellis2Cli";
constexpr const char* kCliModelsSettingsKey = "ai/trellis2CliModels";

QJsonObject runtimeManifest(const QString& dir)
{
    QFile f(QDir(dir).filePath(QStringLiteral("runtime.json")));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

MeshGenPredictor::Result failResult(const QString& message)
{
    MeshGenPredictor::Result r;
    r.error = message;
    return r;
}

} // namespace

Trellis2Predictor::Options::Options() = default;

bool Trellis2Predictor::isAvailable()
{
    return true;   // no ONNX requirement; the runtime probe is what gates it
}

QString Trellis2Predictor::runtimeDir()
{
    QString dir = qEnvironmentVariable(kEnvDirVar);
    if (dir.isEmpty())
        dir = QSettings().value(QLatin1String(kDirSettingsKey)).toString();
    if (dir.isEmpty()) {
        const QString base =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        dir = QDir(base).filePath(QStringLiteral("trellis2"));
    }
    return QDir(dir).exists() ? dir : QString();
}

QString Trellis2Predictor::resolvePython(const QString& dir)
{
    QString py = qEnvironmentVariable(kEnvPythonVar);
    if (py.isEmpty())
        py = QSettings().value(QLatin1String(kPythonSettingsKey)).toString();
    if (!py.isEmpty())
        return QFileInfo::exists(py) ? py : QString();
    if (dir.isEmpty())
        return {};
    const QJsonObject manifest = runtimeManifest(dir);
    py = manifest.value(QStringLiteral("python")).toString();
    if (!py.isEmpty() && QFileInfo::exists(py))
        return py;
#ifdef Q_OS_WIN
    py = QDir(dir).filePath(QStringLiteral("env/Scripts/python.exe"));
#else
    py = QDir(dir).filePath(QStringLiteral("env/bin/python"));
#endif
    return QFileInfo::exists(py) ? py : QString();
}

QString Trellis2Predictor::pythonPath()
{
    return resolvePython(runtimeDir());
}

QString Trellis2Predictor::generateScriptPath()
{
    const QString dir = runtimeDir();
    if (dir.isEmpty())
        return {};
    const QJsonObject manifest = runtimeManifest(dir);
    QString gen = manifest.value(QStringLiteral("generate")).toString();
    if (!gen.isEmpty() && QFileInfo::exists(gen))
        return gen;
    gen = QDir(dir).filePath(QStringLiteral("generate.py"));
    return QFileInfo::exists(gen) ? gen : QString();
}

QString Trellis2Predictor::trellisCliPath()
{
    QString cli = qEnvironmentVariable(kEnvCliVar);
    if (cli.isEmpty())
        cli = QSettings().value(QLatin1String(kCliSettingsKey)).toString();
    if (!cli.isEmpty())
        return QFileInfo::exists(cli) ? cli : QString();
    return QStandardPaths::findExecutable(QStringLiteral("trellis-cli"));
}

QString Trellis2Predictor::trellisCliModelsDir()
{
    QString dir = qEnvironmentVariable(kEnvCliModelsVar);
    if (dir.isEmpty())
        dir = QSettings().value(QLatin1String(kCliModelsSettingsKey)).toString();
    // An EXPLICIT dir (env var / QSettings) is authoritative — returned as-is
    // so error messages point at what the user configured. The IMPLICIT
    // candidates below are only accepted when they actually hold the
    // required model set: a bare/partial <cli>/models directory must not
    // shadow weights downloaded through AI Model Settings.
    if (!dir.isEmpty())
        return QDir(dir).exists() ? dir : QString();

    const auto hasRequiredGgufs = [](const QString& d) {
        const QDir q(d);
        return q.exists(QStringLiteral("ss_flow.gguf"))
            && q.exists(QStringLiteral("shape_flow_512.gguf"))
            && q.exists(QStringLiteral("tex_flow_512.gguf"))
            && q.exists(QStringLiteral("shape_dec.gguf"))
            && q.exists(QStringLiteral("tex_dec.gguf"))
            && q.exists(QStringLiteral("ss_dec.gguf"))
            && q.exists(QStringLiteral("dinov3.gguf"));
    };

    const QString cli = trellisCliPath();
    if (!cli.isEmpty()) {
        const QString sibling = QDir(QFileInfo(cli).absolutePath())
                                    .filePath(QStringLiteral("models"));
        if (hasRequiredGgufs(sibling))
            return sibling;
    }
    // AI Model Settings download location (AIModelCatalog "trellis2-gguf"):
    // models fetched from the Settings dialog land here, so a user who
    // pre-downloaded them only has to install/point at the trellis-cli
    // binary.
    const QString catalogDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("ai_models/trellis2"));
    if (QDir(catalogDir).exists())
        return catalogDir;
    return QString();
}

bool Trellis2Predictor::trellisCliAvailable()
{
    const QString cli = trellisCliPath();
    const QString models = trellisCliModelsDir();
    if (cli.isEmpty() || models.isEmpty())
        return false;
    // Minimum 512-pipeline set (the cascade models are optional extras).
    const QDir d(models);
    return d.exists(QStringLiteral("ss_flow.gguf"))
        && d.exists(QStringLiteral("shape_flow_512.gguf"))
        && d.exists(QStringLiteral("tex_flow_512.gguf"))
        && d.exists(QStringLiteral("shape_dec.gguf"))
        && d.exists(QStringLiteral("tex_dec.gguf"))
        && d.exists(QStringLiteral("ss_dec.gguf"))
        && d.exists(QStringLiteral("dinov3.gguf"));
}

Trellis2Predictor::RuntimeKind Trellis2Predictor::runtimeKind()
{
    // trellis.cpp preferred: no Python, quantized weights, Metal/Vulkan/CUDA.
    if (trellisCliAvailable())
        return RuntimeKind::TrellisCpp;
    if (!pythonPath().isEmpty() && !generateScriptPath().isEmpty())
        return RuntimeKind::PythonSidecar;
    return RuntimeKind::None;
}

bool Trellis2Predictor::runtimeAvailable()
{
    return runtimeKind() != RuntimeKind::None;
}

QString Trellis2Predictor::runtimeDescription()
{
    switch (runtimeKind()) {
    case RuntimeKind::TrellisCpp:
        return QStringLiteral("TRELLIS.2 runtime: trellis.cpp (%1, models %2)")
            .arg(trellisCliPath(), trellisCliModelsDir());
    case RuntimeKind::PythonSidecar:
        return QStringLiteral("TRELLIS.2 runtime: Python sidecar (%1)")
            .arg(runtimeDir());
    case RuntimeKind::None:
        break;
    }
    return QStringLiteral(
        "TRELLIS.2 runtime not installed. Either build trellis.cpp and point "
        "QTMESH_TRELLIS2_CLI / QSettings ai/trellis2Cli at trellis-cli (with "
        "its GGUF models next to it), or run `python3 ai/trellis2/install.py` "
        "(Linux + NVIDIA GPU) for the Python sidecar. See docs/TRELLIS2.md.");
}

MeshGenPredictor::Result Trellis2Predictor::predict(
    const QImage& image,
    const Options& opts,
    const MeshGenPredictor::ProgressFn& progress)
{
    using Stage = MeshGenPredictor::Stage;
    if (image.isNull())
        return failResult(QStringLiteral("trellis2: input image is null."));
    RuntimeKind kind = runtimeKind();
    // --mock is a Python-sidecar feature (synthetic generation for GPU-less
    // plumbing tests) — route mock runs there even when trellis.cpp is found,
    // and NEVER silently fall through to a real multi-minute generation when
    // the sidecar isn't there to serve the mock.
    const bool mockRun =
        opts.mock || qEnvironmentVariableIsSet("QTMESH_TRELLIS2_MOCK");
    if (mockRun && kind != RuntimeKind::None) {
        if (!pythonPath().isEmpty() && !generateScriptPath().isEmpty())
            kind = RuntimeKind::PythonSidecar;
        else if (kind == RuntimeKind::TrellisCpp)
            return failResult(QStringLiteral(
                "trellis2: mock generation needs the Python sidecar "
                "(ai/trellis2/generate.py + a python with numpy/Pillow) — "
                "refusing to run a real trellis.cpp generation for a mock "
                "request."));
    }
    // The QTMESH_TRELLIS2_IMPORT re-bake seam (Phase 9) skips inference
    // entirely — it must work with NO runtime installed.
    const QString importPath = qEnvironmentVariable("QTMESH_TRELLIS2_IMPORT");
    if (kind == RuntimeKind::None && importPath.isEmpty())
        return failResult(runtimeDescription());

    auto report = [&progress](Stage s, int done, int total) -> bool {
        return !progress || progress(s, done, total);
    };

    // ---- 1. alpha matte (QtMeshEditor-side background removal) --------------
    // TRELLIS.2's preprocess uses a supplied alpha channel directly and never
    // touches its rembg model on such input — which is exactly how the
    // non-commercial briaai/RMBG-2.0 stays unused (docs/trellis2-dependencies.md).
    QImage subject = image;
    const bool inputHasMatte = [&image]() {
        if (!image.hasAlphaChannel())
            return false;
        const QImage a = image.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < a.height(); ++y) {
            const uchar* line = a.constScanLine(y);
            for (int x = 0; x < a.width(); ++x)
                if (line[x * 4 + 3] != 255)
                    return true;
        }
        return false;
    }();
    QString warning;
    bool matteReady = inputHasMatte;
    if (!inputHasMatte && opts.removeBackground && !mockRun) {
        if (!report(Stage::Encode, 0, 1))
            return failResult(QStringLiteral("cancelled"));
        if (BackgroundRemover::isAvailable()) {
            // ensureModelBlocking spins a nested QEventLoop — only safe on the
            // main thread. On a worker (the GUI controller's std::thread) the
            // model must have been ensured up front; here we just read it.
            const bool onMainThread =
                QCoreApplication::instance()
                && QThread::currentThread()
                       == QCoreApplication::instance()->thread();
            const QString model = onMainThread
                ? BackgroundRemover::ensureModelBlocking()
                : (BackgroundRemover::modelPresent()
                       ? BackgroundRemover::modelPath() : QString());
            BackgroundRemover::Options bg;
            bg.keepAlpha = true;
            const BackgroundRemover::Result cut =
                BackgroundRemover::removeBackground(subject, model, bg);
            if (cut.ok) {
                subject = cut.image;
                matteReady = true;
            } else
                warning = QStringLiteral(
                    "background removal unavailable (%1); the whole frame is "
                    "treated as foreground.").arg(cut.error);
        } else {
            warning = QStringLiteral(
                "built without ENABLE_ONNX — no U²-Net background removal; "
                "the whole frame is treated as foreground.");
        }
    }

    // ---- 2. sidecar process --------------------------------------------------
    QTemporaryDir tmp;
    if (!tmp.isValid())
        return failResult(QStringLiteral("trellis2: cannot create temp dir."));
    const QString inputPng = QDir(tmp.path()).filePath(QStringLiteral("input.png"));
    const QString outQtm3d = QDir(tmp.path()).filePath(QStringLiteral("out.qtm3d"));
    if (!subject.save(inputPng, "PNG"))
        return failResult(QStringLiteral("trellis2: cannot write temp input image."));

    Trellis2Interchange::Data srcData;
    QString pythonQtm3dPath;   // set by the Python flavor (for the source copy)

    // Phase 9 hook + long-run test seam: QTMESH_TRELLIS2_IMPORT=<file.qtm3d>
    // skips inference entirely and re-runs the native pipeline (game-ready +
    // bake) on a previously preserved generation — re-bake textures /
    // re-target LODs without paying the model again.
    if (!importPath.isEmpty()) {
        Trellis2Interchange::ReadResult rr = Trellis2Interchange::read(importPath);
        if (!rr.ok)
            return failResult(QStringLiteral(
                "trellis2: cannot import preserved source %1: %2")
                                  .arg(importPath, rr.error));
        srcData = std::move(rr.data);
        pythonQtm3dPath = importPath;   // re-preserve by copying, if asked
    } else if (kind == RuntimeKind::TrellisCpp) {
    // trellis.cpp flavor (#966): run trellis-cli with --dump-post so it emits
    // the RAW decoded mesh + sparse PBR volume and exits before its own
    // remesh/UV/bake — QtMeshEditor keeps the whole asset pipeline (game-ready
    // simplify + native texture bake), exactly like the Python flavor.
    const QString cli = trellisCliPath();
    const QString models = trellisCliModelsDir();
    const QString outDump =
        QDir(tmp.path()).filePath(QStringLiteral("out.trellisraw"));
    int res = 1024;
    const QString presetName = opts.preset.toLower();
    if (presetName == QLatin1String("fast"))
        res = 512;
    else if (presetName == QLatin1String("high"))
        res = 1536;
    QString models2 = models;
    if (res > 512
        && !QDir(models2).exists(QStringLiteral("shape_flow_1024.gguf"))) {
        // The resolved dir (often the trellis-cli sibling with the 512 set)
        // has no cascade weights — but the AI Model Settings download
        // location may: prefer it when it holds the FULL set including the
        // cascade, so downloading "TRELLIS.2 cascade" in Settings lights up
        // the Balanced/High presets without touching the cli install.
        const QString catalogDir =
            QDir(QStandardPaths::writableLocation(
                     QStandardPaths::AppDataLocation))
                .filePath(QStringLiteral("ai_models/trellis2"));
        const QDir cd(catalogDir);
        const bool catalogHasAll =
            cd.exists(QStringLiteral("shape_flow_1024.gguf"))
            && cd.exists(QStringLiteral("tex_flow_1024.gguf"))
            && cd.exists(QStringLiteral("ss_flow.gguf"))
            && cd.exists(QStringLiteral("shape_flow_512.gguf"))
            && cd.exists(QStringLiteral("tex_flow_512.gguf"))
            && cd.exists(QStringLiteral("shape_dec.gguf"))
            && cd.exists(QStringLiteral("tex_dec.gguf"))
            && cd.exists(QStringLiteral("ss_dec.gguf"))
            && cd.exists(QStringLiteral("dinov3.gguf"));
        if (catalogHasAll) {
            models2 = catalogDir;
        } else {
            if (!warning.isEmpty())
                warning += QStringLiteral(" ");
            warning += QStringLiteral(
                "the '%1' preset needs the 1024-cascade weights, which are "
                "not installed — using the 512 pipeline (thin structures may "
                "be lost). Download 'TRELLIS.2 cascade' in AI Model Settings "
                "to enable it.").arg(presetName);
            res = 512;
        }
    }
    QStringList args{QStringLiteral("--image"),     inputPng,
                     QStringLiteral("--dump-post"), outDump,
                     QStringLiteral("--models"),    models2,
                     QStringLiteral("--res"),       QString::number(res),
                     QStringLiteral("--seed"),      QString::number(opts.seed)};
    QProcess proc;
    proc.setProgram(cli);
    proc.setArguments(args);
    // stderr (backend banner, ggml logs) forwards to ours; stdout carries the
    // "[k/7]" stage lines we map onto the shared Stage enum.
    proc.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    proc.start();
    if (!proc.waitForStarted(15000))
        return failResult(QStringLiteral("trellis2: failed to start %1").arg(cli));
    Stage lastStage = Stage::Encode;
    int lastDone = 0, lastTotal = 2;
    bool cancelled = false;
    QByteArray pending;
    auto handleLine = [&](const QByteArray& line) {
        if (line.size() >= 5 && line[0] == '[' && line[2] == '/'
            && line[4] == ']' && line[1] >= '1' && line[1] <= '9') {
            const int k = line[1] - '0';
            if (k <= 2) { lastStage = Stage::Encode;  lastDone = k - 1; lastTotal = 2; }
            else if (k <= 6) { lastStage = Stage::Denoise; lastDone = k - 3; lastTotal = 4; }
            else { lastStage = Stage::Decode; lastDone = 0; lastTotal = 1; }
        }
    };
    while (proc.state() != QProcess::NotRunning) {
        proc.waitForReadyRead(300);
        pending += proc.readAllStandardOutput();
        int nl;
        while ((nl = pending.indexOf('\n')) >= 0) {
            handleLine(pending.left(nl));
            pending.remove(0, nl + 1);
        }
        if (!report(lastStage, lastDone, lastTotal)) {
            cancelled = true;
            proc.terminate();
            if (!proc.waitForFinished(5000))
                proc.kill();
            proc.waitForFinished(2000);
            break;
        }
    }
    if (cancelled)
        return failResult(QStringLiteral("cancelled"));
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return failResult(QStringLiteral(
            "trellis2: trellis-cli exited with code %1 (see stderr log).")
                              .arg(proc.exitCode()));
    Trellis2Interchange::ReadResult rr =
        Trellis2Interchange::readTrellisCppDump(outDump);
    if (!rr.ok)
        return failResult(QStringLiteral("trellis2: bad trellis.cpp dump: %1")
                              .arg(rr.error));
    rr.data.meta.insert(QStringLiteral("seed"),
                        static_cast<int>(opts.seed));
    rr.data.meta.insert(QStringLiteral("preset"), presetName);
    srcData = std::move(rr.data);
    } else {
    const QString python = pythonPath();
    const QString script = generateScriptPath();

    QStringList args{script,
                     QStringLiteral("--input"), inputPng,
                     QStringLiteral("--output"), outQtm3d,
                     QStringLiteral("--seed"), QString::number(opts.seed),
                     QStringLiteral("--preset"), opts.preset.toLower()};
    if (opts.steps > 0)
        args << QStringLiteral("--steps") << QString::number(opts.steps);
    // QTMESH_TRELLIS2_MOCK: environment escape hatch so the CLI/e2e harness can
    // exercise the full plumbing (sidecar → interchange → bake → export) on
    // machines without a CUDA GPU.
    if (opts.mock || qEnvironmentVariableIsSet("QTMESH_TRELLIS2_MOCK"))
        args << QStringLiteral("--mock");
    if (!matteReady)
        args << QStringLiteral("--allow-opaque");

    QProcess proc;
    proc.setProgram(python);
    proc.setArguments(args);
    proc.setWorkingDirectory(QFileInfo(script).absolutePath());
    // Child stderr (tqdm, HF download logs) goes straight to our stderr so the
    // pipe can never fill up and stall the sidecar; stdout carries the JSON
    // protocol only.
    proc.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    env.insert(QStringLiteral("QTMESH_TRELLIS2_ENV"), runtimeDir());
    proc.setProcessEnvironment(env);
    proc.start();
    if (!proc.waitForStarted(15000))
        return failResult(QStringLiteral("trellis2: failed to start %1").arg(python));

    // Sidecar stage → (Stage, done, total). The generation itself is one long
    // coarse step; the native bake below reports fine-grained progress.
    Stage lastStage = Stage::Encode;
    int lastDone = 0, lastTotal = 1;
    QString sidecarError;
    QJsonObject doneInfo;
    bool cancelled = false;

    auto handleLine = [&](const QByteArray& line) {
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject())
            return;
        const QJsonObject o = doc.object();
        const QString event = o.value(QStringLiteral("event")).toString();
        if (event == QLatin1String("stage")) {
            const QString s = o.value(QStringLiteral("stage")).toString();
            if (s == QLatin1String("load_model")) {
                lastStage = Stage::Encode; lastDone = 0; lastTotal = 2;
            } else if (s == QLatin1String("preprocess")) {
                lastStage = Stage::Encode; lastDone = 1; lastTotal = 2;
            } else if (s == QLatin1String("generate")) {
                lastStage = Stage::Denoise; lastDone = 0; lastTotal = 1;
            } else if (s == QLatin1String("extract")) {
                lastStage = Stage::Denoise; lastDone = 1; lastTotal = 1;
            } else if (s == QLatin1String("attributes")) {
                lastStage = Stage::Decode; lastDone = 0; lastTotal = 1;
            } else if (s == QLatin1String("write")) {
                lastStage = Stage::Decode; lastDone = 1; lastTotal = 1;
            }
        } else if (event == QLatin1String("progress")) {
            lastDone = o.value(QStringLiteral("done")).toInt(lastDone);
            lastTotal = o.value(QStringLiteral("total")).toInt(lastTotal);
        } else if (event == QLatin1String("error")) {
            sidecarError = o.value(QStringLiteral("message")).toString();
        } else if (event == QLatin1String("done")) {
            doneInfo = o;
        }
    };

    QByteArray pending;
    while (proc.state() != QProcess::NotRunning) {
        proc.waitForReadyRead(300);
        pending += proc.readAllStandardOutput();
        int nl;
        while ((nl = pending.indexOf('\n')) >= 0) {
            handleLine(pending.left(nl));
            pending.remove(0, nl + 1);
        }
        // Re-report the latest stage so cancellation stays responsive during
        // the long silent stretches of a generation.
        if (!report(lastStage, lastDone, lastTotal)) {
            cancelled = true;
            proc.terminate();
            if (!proc.waitForFinished(5000))
                proc.kill();
            proc.waitForFinished(2000);
            break;
        }
    }
    pending += proc.readAllStandardOutput();
    for (const QByteArray& line : pending.split('\n'))
        if (!line.trimmed().isEmpty())
            handleLine(line);

    if (cancelled)
        return failResult(QStringLiteral("cancelled"));
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        if (!sidecarError.isEmpty())
            return failResult(QStringLiteral("trellis2: %1").arg(sidecarError));
        return failResult(QStringLiteral(
            "trellis2: sidecar exited with code %1 (see stderr log).")
                              .arg(proc.exitCode()));
    }

    // ---- 3. read the interchange ---------------------------------------------
    Trellis2Interchange::ReadResult read =
        Trellis2Interchange::read(outQtm3d);
    if (!read.ok)
        return failResult(QStringLiteral("trellis2: bad interchange: %1")
                              .arg(read.error));
    srcData = std::move(read.data);
    pythonQtm3dPath = outQtm3d;
    }
    const Trellis2Interchange::Data& src = srcData;

    // Phase 9: preserve the full-resolution generation.
    QString keptSourcePath;
    if (!opts.sourceKeepDir.isEmpty()) {
        QDir().mkpath(opts.sourceKeepDir);
        const QString base = opts.sourceKeepBaseName.isEmpty()
            ? QStringLiteral("trellis2") : opts.sourceKeepBaseName;
        keptSourcePath = QDir(opts.sourceKeepDir)
                             .filePath(base + QStringLiteral("_source.qtm3d"));
        QFile::remove(keptSourcePath);
        const bool kept = !pythonQtm3dPath.isEmpty()
            ? QFile::copy(pythonQtm3dPath, keptSourcePath)
            : Trellis2Interchange::write(keptSourcePath, src);
        if (!kept)
            keptSourcePath.clear();
    }

    // ---- 4. native game-ready processing (Phase 8) ----------------------------
    if (!report(Stage::Decode, 0, 1))
        return failResult(QStringLiteral("cancelled"));
    Trellis2Bake::GameReadyOptions gr;
    gr.targetTriangles = opts.targetTriangles;
    // "Original" (0) + texture bake: cap the density anyway. xatlas cannot
    // realistically unwrap a raw multi-million-triangle dual-grid mesh (its
    // chart compute is superlinear — a 4.86M-tri source burned 19 CPU-hours
    // without finishing), and upstream trellis.cpp itself always decimates to
    // 150k (res 512) / 300k (cascade) before unwrapping. The uncapped raw
    // mesh is still preserved in the .qtm3d source for later re-baking.
    if (gr.targetTriangles <= 0 && opts.bakeTexture) {
        const int cap = src.resolution > 512 ? 300000 : 150000;
        if (src.triangleCount > cap)
            gr.targetTriangles = cap;
    }
    // Voxel-scale weld (the upstream reference uses 1/(res*8)): the raw
    // dual-grid mesh is non-manifold at sub-voxel scale and unsimplifiable
    // without it.
    if (src.voxelSize > 0.0f)
        gr.weldEpsilonAbsolute = src.voxelSize / 8.0f;
    // Pre-smooth: voxel decodes of fuzzy subjects (fur/hair) carry sub-voxel
    // micro-pits that render as dark pepper speckle and derail QEM on thin
    // double-walled features (ear lace). Volume-preserving, so real shape
    // survives; only sub-voxel noise flattens.
    gr.taubinIterations = 5;
    const Trellis2Bake::GameReadyResult processed =
        Trellis2Bake::makeGameReady(src.positions, src.indices, gr);
    if (!processed.ok)
        return failResult(QStringLiteral("trellis2: %1").arg(processed.error));
    fprintf(stderr,
            "[trellis2] game-ready: %d -> %d tris (welded %d, dropped %d comps, "
            "simplify err %.4f)\n",
            processed.inputTriangles, processed.outputTriangles,
            processed.weldedVertices, processed.removedComponents,
            processed.simplifyError);

    Trellis2Bake::SparseVolumeSampler volume;
    if (src.voxelCount > 0)
        volume.build(src.voxelCoords.data(), src.voxelAttrs.data(),
                     src.voxelCount, src.voxelSize, src.origin);

    MeshGenPredictor::Result r;
    r.warning = warning;
    r.sourceInterchangePath = keptSourcePath;
    r.usedModel = !mockRun;
    r.bakeTripoSROrientation = false;   // TRELLIS.2 is +Y-up like TripoSG

    // ---- 5. native multi-channel PBR bake (Phase 7) ----------------------------
    bool baked = false;
    if (opts.bakeTexture && volume.valid()) {
        Trellis2Bake::BakeOptions bo;
        bo.textureSize = opts.textureSize;
        bo.supersample = opts.supersample;
        bo.bakeNormalMap = opts.bakeNormalMap;
        if (progress) {
            bo.progress = [&progress](int done, int total) {
                return progress(Stage::Bake, done, total);
            };
        }
        Trellis2Bake::BakeResult bk = Trellis2Bake::bake(
            processed.positions, processed.indices,
            src.positions, src.indices, volume, bo);
        if (bk.cancelled)
            return failResult(QStringLiteral("cancelled"));
        if (bk.ok) {
            r.positions = std::move(bk.positions);
            r.indices = std::move(bk.indices);
            r.uvs = std::move(bk.uvs);
            r.normals = std::move(bk.normals);
            r.vertexCount = bk.vertexCount;
            r.triangleCount = bk.triangleCount;
            r.texture = std::move(bk.baseColor);
            r.roughnessMap = std::move(bk.roughness);
            r.metallicMap = std::move(bk.metallic);
            r.normalMap = std::move(bk.normalMap);
            baked = true;
        } else {
            if (!r.warning.isEmpty())
                r.warning += QStringLiteral(" ");
            r.warning += QStringLiteral(
                "texture bake failed (%1) — using per-vertex colours.")
                             .arg(bk.error);
        }
    }

    if (!baked) {
        r.positions = processed.positions;
        r.indices = processed.indices;
        r.vertexCount = static_cast<int>(r.positions.size() / 3);
        r.triangleCount = static_cast<int>(r.indices.size() / 3);
        if (volume.valid()) {
            r.colors.resize(static_cast<size_t>(r.vertexCount) * 3);
            for (int v = 0; v < r.vertexCount; ++v) {
                float attr[6];
                volume.sample(&r.positions[static_cast<size_t>(v) * 3], attr);
                r.colors[static_cast<size_t>(v) * 3 + 0] = attr[0];
                r.colors[static_cast<size_t>(v) * 3 + 1] = attr[1];
                r.colors[static_cast<size_t>(v) * 3 + 2] = attr[2];
            }
        }
    }

    if (r.vertexCount <= 0 || r.triangleCount <= 0)
        return failResult(QStringLiteral("trellis2: generation produced an empty mesh."));

    report(Stage::Decode, 1, 1);
    r.ok = true;
    return r;
}
