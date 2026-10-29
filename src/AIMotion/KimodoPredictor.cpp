#include "KimodoPredictor.h"
#include "KimodoNative.h"

#include "../AppStorage.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>

#include <cstdio>

namespace AIMotion {

namespace {

constexpr const char* kEnvDirVar = "QTMESH_KIMODO_ENV";
constexpr const char* kEnvPythonVar = "QTMESH_KIMODO_PYTHON";
constexpr const char* kEnvGenerateVar = "QTMESH_KIMODO_GENERATE";
constexpr const char* kDirSettingsKey = "ai/kimodoEnv";
constexpr const char* kPythonSettingsKey = "ai/kimodoPython";

QJsonObject runtimeManifest(const QString& dir)
{
    QFile f(QDir(dir).filePath(QStringLiteral("runtime.json")));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

KimodoPredictor::Result failed(const QString& message)
{
    KimodoPredictor::Result r;
    r.error = message;
    return r;
}

// The sidecar's last {"event":"error"} message, else the front of stderr.
QString failureText(const QString& lastError, const QByteArray& errBuf, int exitCode)
{
    if (!lastError.isEmpty()) return lastError;
    QString detail = QString::fromLocal8Bit(errBuf).trimmed();
    const int tb = detail.lastIndexOf(QLatin1String("\n"));
    if (tb > 0) detail = detail.mid(tb + 1);   // a Python traceback ends with the exception line
    if (detail.size() > 400) detail = detail.left(399) + QStringLiteral("…");
    return QStringLiteral("Kimodo sidecar exited with code %1%2").arg(exitCode).arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
}

} // namespace

QString KimodoPredictor::runtimeDir()
{
    QString dir = qEnvironmentVariable(kEnvDirVar);
    if (dir.isEmpty()) dir = QSettings().value(QLatin1String(kDirSettingsKey)).toString();
    if (dir.isEmpty()) dir = QDir(AppStorage::persistentRoot()).filePath(QStringLiteral("kimodo"));
    return QDir(dir).exists() ? dir : QString();
}

QString KimodoPredictor::pythonPath()
{
    QString py = qEnvironmentVariable(kEnvPythonVar);
    if (py.isEmpty()) py = QSettings().value(QLatin1String(kPythonSettingsKey)).toString();
    if (!py.isEmpty()) return QFileInfo::exists(py) ? py : QString();
    const QString dir = runtimeDir();
    if (dir.isEmpty()) return {};
    py = runtimeManifest(dir).value(QStringLiteral("python")).toString();
    if (!py.isEmpty() && QFileInfo::exists(py)) return py;
#ifdef Q_OS_WIN
    py = QDir(dir).filePath(QStringLiteral("env/Scripts/python.exe"));
#else
    py = QDir(dir).filePath(QStringLiteral("env/bin/python"));
#endif
    return QFileInfo::exists(py) ? py : QString();
}

QString KimodoPredictor::generateScriptPath()
{
    QString gen = qEnvironmentVariable(kEnvGenerateVar);
    if (!gen.isEmpty()) return QFileInfo::exists(gen) ? gen : QString();
    const QString dir = runtimeDir();
    if (dir.isEmpty()) return {};
    gen = runtimeManifest(dir).value(QStringLiteral("generate")).toString();
    if (!gen.isEmpty() && QFileInfo::exists(gen)) return gen;
    gen = QDir(dir).filePath(QStringLiteral("generate.py"));
    return QFileInfo::exists(gen) ? gen : QString();
}

bool KimodoPredictor::mockRequested(const Options& o)
{
    return o.mock || qEnvironmentVariableIsSet("QTMESH_KIMODO_MOCK");
}

bool KimodoPredictor::pythonRuntimeAvailable()
{
    return !pythonPath().isEmpty() && !generateScriptPath().isEmpty();
}

QString KimodoPredictor::pythonRuntimeDescription()
{
    const QString py = pythonPath();
    const QString gen = generateScriptPath();
    if (!py.isEmpty() && !gen.isEmpty())
        return QStringLiteral("Kimodo runtime: %1 (%2)").arg(QDir::toNativeSeparators(gen), QDir::toNativeSeparators(py));
    return QStringLiteral("Kimodo runtime not installed. Run `python3 ai/kimodo/install.py`, or point %1 / QSettings %2 at a "
                          "Python environment with Kimodo and %3 at ai/kimodo/generate.py. Kimodo's text encoder is Meta "
                          "Llama 3 (gated): accept its licence on Hugging Face and log in with `hf auth login`.")
        .arg(QLatin1String(kEnvPythonVar), QLatin1String(kPythonSettingsKey), QLatin1String(kEnvGenerateVar));
}

bool KimodoPredictor::runtimeAvailable()
{
    return (KimodoNative::buildSupported() && KimodoNative::modelsInstalled()) || pythonRuntimeAvailable();
}

QString KimodoPredictor::runtimeDescription()
{
    if (KimodoNative::buildSupported() && KimodoNative::modelsInstalled()) return KimodoNative::description();
    if (pythonRuntimeAvailable()) return pythonRuntimeDescription();
    if (KimodoNative::buildSupported())
        return KimodoNative::description() + QStringLiteral(": download them to generate without Python");
    return pythonRuntimeDescription();
}

QString KimodoPredictor::resolveRuntime(const Options& o, QString* why)
{
    const QString mode = o.runtime.isEmpty() ? QStringLiteral("auto") : o.runtime;
    const bool nativeReady = KimodoNative::buildSupported() && KimodoNative::modelsInstalled();
    // The mock and constraints files live in the Python sidecar only.
    const bool needsPython = mockRequested(o) || !o.constraintsPath.isEmpty();
    if (mode == QLatin1String("python") || needsPython) {
        if (mode == QLatin1String("native")) {
            if (why) *why = QStringLiteral("constraints files and the mock need the Python runtime");
            return {};
        }
        if (pythonRuntimeAvailable() || mockRequested(o)) return QStringLiteral("python");
        if (why) *why = pythonRuntimeDescription();
        return {};
    }
    if (mode == QLatin1String("native")) {
        if (nativeReady) return QStringLiteral("native");
        if (why) *why = KimodoNative::description();
        return {};
    }
    if (nativeReady) return QStringLiteral("native");
    if (pythonRuntimeAvailable()) return QStringLiteral("python");
    if (why) *why = runtimeDescription();
    return {};
}

QStringList KimodoPredictor::buildArguments(const Options& o, const QString& outputDir)
{
    QJsonArray segs;
    for (const MotionSegment& s : o.segments)
        segs.append(QJsonObject{{QStringLiteral("text"), s.text}, {QStringLiteral("duration"), s.duration}});
    QStringList args;
    args << QStringLiteral("--segments") << QString::fromUtf8(QJsonDocument(segs).toJson(QJsonDocument::Compact))
         << QStringLiteral("--output-dir") << outputDir
         << QStringLiteral("--num-samples") << QString::number(o.numSamples)
         << QStringLiteral("--model") << o.model;
    if (o.hasSeed) args << QStringLiteral("--seed") << QString::number(o.seed);
    if (o.diffusionSteps > 0) args << QStringLiteral("--diffusion-steps") << QString::number(o.diffusionSteps);
    if (!o.rootMotion) args << QStringLiteral("--no-root-motion");
    if (!o.postprocess) args << QStringLiteral("--no-postprocess");
    if (!o.constraintsPath.isEmpty()) args << QStringLiteral("--constraints") << o.constraintsPath;
    if (!o.textEncoderUrl.isEmpty()) args << QStringLiteral("--text-encoder-url") << o.textEncoderUrl;
    if (!o.textEncoderQuant.isEmpty()) args << QStringLiteral("--text-encoder-quant") << o.textEncoderQuant;
    if (mockRequested(o)) args << QStringLiteral("--mock");
    return args;
}

bool KimodoPredictor::validateOptions(const Options& o, QString* error)
{
    const auto bad = [error](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    if (o.segments.empty()) return bad(QStringLiteral("no prompt"));
    for (const MotionSegment& s : o.segments)
        if (s.text.trimmed().isEmpty() || !(s.duration > 0.0) || s.duration > 600.0)
            return bad(QStringLiteral("every segment needs text and a duration in (0, 600] seconds"));
    if (o.numSamples < 1 || o.numSamples > 16) return bad(QStringLiteral("variations must be 1..16"));
    // The DDIM schedule subsamples Kimodo's 1000 training steps: more denoising steps than that do not exist.
    if (o.diffusionSteps < 0 || o.diffusionSteps > kMaxDiffusionSteps)
        return bad(QStringLiteral("diffusion steps must be 1..%1 (0 = the model's default)").arg(kMaxDiffusionSteps));
    if (!o.textEncoderQuant.isEmpty() && !textEncoderQuantModes().contains(o.textEncoderQuant))
        return bad(QStringLiteral("text encoder quantization must be one of: %1").arg(textEncoderQuantModes().join(QStringLiteral(", "))));
    if (!o.runtime.isEmpty() && !runtimeModes().contains(o.runtime))
        return bad(QStringLiteral("runtime must be one of: %1").arg(runtimeModes().join(QStringLiteral(", "))));
    return true;
}

namespace {

// One run of the Python sidecar: JSON-line events on stdout, stderr passed through.
struct SidecarRun {
    QString stage = QStringLiteral("import");
    int done = 0;
    int total = 0;
    QString lastError;
    QJsonObject doneEvent;
    QByteArray pending;
    QByteArray errBuf;

    void handleLine(const QByteArray& line)
    {
        const QJsonObject ev = QJsonDocument::fromJson(line).object();
        const QString kind = ev.value(QStringLiteral("event")).toString();
        if (kind == QLatin1String("stage")) {
            stage = ev.value(QStringLiteral("stage")).toString();
            done = 0;
            total = 0;
        } else if (kind == QLatin1String("progress")) {
            done = ev.value(QStringLiteral("done")).toInt();
            total = ev.value(QStringLiteral("total")).toInt();
        } else if (kind == QLatin1String("error")) {
            lastError = ev.value(QStringLiteral("message")).toString();
        } else if (kind == QLatin1String("done")) {
            doneEvent = ev;
        }
    }

    void drain(QProcess& proc)
    {
        pending += proc.readAllStandardOutput();
        if (const QByteArray chunk = proc.readAllStandardError(); !chunk.isEmpty()) {
            errBuf = (errBuf + chunk).right(64 * 1024);
            fwrite(chunk.constData(), 1, size_t(chunk.size()), stderr);
            fflush(stderr);
        }
        for (int nl = pending.indexOf('\n'); nl >= 0; nl = pending.indexOf('\n')) {
            handleLine(pending.left(nl));
            pending.remove(0, nl + 1);
        }
    }
};

void stopProcess(QProcess& proc)
{
    proc.terminate();
    if (!proc.waitForFinished(5000)) proc.kill();
    proc.waitForFinished(2000);
}

KimodoPredictor::Result runSidecar(const KimodoPredictor::Options& o, const QString& outDir,
                                   const KimodoPredictor::ProgressFn& progress)
{
    const QString py = KimodoPredictor::pythonPath();
    const QString gen = KimodoPredictor::generateScriptPath();
    if (py.isEmpty() || gen.isEmpty()) return failed(KimodoPredictor::pythonRuntimeDescription());

    QProcess proc;
    proc.setProgram(py);
    proc.setArguments(QStringList{gen} + KimodoPredictor::buildArguments(o, outDir));
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    proc.setProcessEnvironment(env);
    proc.start();
    if (!proc.waitForStarted(15000)) return failed(QStringLiteral("could not start %1").arg(py));

    SidecarRun run;
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + qint64(o.timeoutSec) * 1000;
    bool timedOut = false;
    bool cancelled = false;
    while (proc.state() != QProcess::NotRunning) {
        proc.waitForReadyRead(300);
        run.drain(proc);
        timedOut = QDateTime::currentMSecsSinceEpoch() > deadline;
        cancelled = timedOut || (progress && !progress(run.stage, run.done, run.total));
        if (cancelled) {
            stopProcess(proc);
            break;
        }
    }
    run.drain(proc);
    if (cancelled) return failed(timedOut ? QStringLiteral("Kimodo timed out after %1 s").arg(o.timeoutSec) : QStringLiteral("cancelled"));
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0 || run.doneEvent.isEmpty())
        return failed(failureText(run.lastError, run.errBuf, proc.exitCode()));

    KimodoPredictor::Result r;
    r.outputDir = outDir;
    r.manifest = run.doneEvent;
    for (const QJsonValue& f : run.doneEvent.value(QStringLiteral("files")).toArray())
        if (QFileInfo::exists(f.toString())) r.files << f.toString();
    if (r.files.isEmpty()) return failed(QStringLiteral("Kimodo reported success but wrote no motion file"));
    r.ok = true;
    return r;
}

} // namespace

KimodoPredictor::Result KimodoPredictor::predict(const Options& o, const ProgressFn& progress)
{
    if (QString err; !validateOptions(o, &err)) return failed(err);
    QString why;
    const QString runtime = resolveRuntime(o, &why);
    if (runtime.isEmpty()) return failed(why);

    QString outDir = o.outputDir;
    if (outDir.isEmpty())
        outDir = QDir(AppStorage::persistentRoot())
                     .filePath(QStringLiteral("generated_motion/kimodo_%1").arg(QDateTime::currentMSecsSinceEpoch()));
    if (!QDir().mkpath(outDir)) return failed(QStringLiteral("cannot create %1").arg(outDir));
    if (runtime == QLatin1String("native")) {
        Options native = o;
        native.outputDir = outDir;
        return KimodoNative::predict(native, progress);
    }
    return runSidecar(o, outDir, progress);
}

} // namespace AIMotion
