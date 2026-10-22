#include "SdCliHelper.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace SdCliHelper {

QString defaultHelperPath()
{
    const QByteArray env = qgetenv("QTMESH_SDCLI_HELPER");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("qtmesh_sdcli.exe");
#else
    const QString exe = QStringLiteral("qtmesh_sdcli");
#endif
    QStringList dirs;
    if (QCoreApplication::instance())
        dirs << QCoreApplication::applicationDirPath();
#ifdef Q_OS_LINUX
    dirs << QStringLiteral("/usr/share/qtmesheditor");
#endif
    for (const QString& d : dirs) {
        const QString p = QDir(d).filePath(exe);
        if (QFileInfo(p).isExecutable())
            return p;
    }
    return QString();
}

bool helperAvailable()
{
    const QString p = defaultHelperPath();
    return !p.isEmpty() && QFileInfo(p).isExecutable();
}

QStringList buildArguments(const Request& req)
{
    QStringList a;
    a << QStringLiteral("--diffusion-model") << req.diffusion
      << QStringLiteral("--llm") << req.llm
      << QStringLiteral("--vae") << req.vae
      << QStringLiteral("-p") << req.prompt
      << QStringLiteral("--cfg-scale") << QString::number(static_cast<double>(req.cfgScale), 'f', 2)
      << QStringLiteral("--steps") << QString::number(req.steps)
      << QStringLiteral("--sampling-method") << QStringLiteral("euler")
      << QStringLiteral("-W") << QString::number(req.width)
      << QStringLiteral("-H") << QString::number(req.height)
      << QStringLiteral("-o") << req.outputPath;
    if (req.seed >= 0)
        a << QStringLiteral("-s") << QString::number(req.seed);
    if (req.threads > 0)
        a << QStringLiteral("-t") << QString::number(req.threads);
    if (!req.refImagePath.isEmpty())
        a << QStringLiteral("-r") << req.refImagePath;
    return a;
}

bool parseProgress(const QByteArray& fragment, int* step, int* total)
{
    // "...| 12/16 - 1.23s/it" — a "<n>/<m>" directly followed by " - ".
    static const QRegularExpression re(QStringLiteral(R"((\d+)/(\d+)\s+-\s)"));
    const QRegularExpressionMatch m = re.match(QString::fromLatin1(fragment));
    if (!m.hasMatch())
        return false;
    if (step)  *step  = m.captured(1).toInt();
    if (total) *total = m.captured(2).toInt();
    return true;
}

Result run(const Request& req,
           const QString& helperPath,
           const std::function<void(int, int)>& progress,
           const std::atomic<bool>& stop)
{
    Result res;
    if (helperPath.isEmpty() || !QFileInfo(helperPath).isExecutable()) {
        res.error = QStringLiteral("The FLUX.3 helper (qtmesh_sdcli) was not found next to "
                                   "the editor — reinstall, or set QTMESH_SDCLI_HELPER");
        return res;
    }
    QFile::remove(req.outputPath);   // never mistake a stale file for this run's output

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(helperPath, buildArguments(req));
    if (!proc.waitForStarted(10000)) {
        res.error = QStringLiteral("Could not start the FLUX.3 helper: %1").arg(proc.errorString());
        return res;
    }

    QByteArray pending;       // current (unterminated) fragment
    QByteArray tail;          // last output, for error reports
    auto consume = [&](const QByteArray& chunk) {
        for (char c : chunk) {
            if (c == '\r' || c == '\n') {
                if (!pending.isEmpty()) {
                    int s = 0, t = 0;
                    if (parseProgress(pending, &s, &t)) {
                        // A bar fragment, whether or not anyone listens: it is
                        // never part of the error text, and only the sampling
                        // bar (not the tensor-loading one) is forwarded.
                        if (progress && t == req.steps)
                            progress(s, t);
                    } else {
                        tail += pending + '\n';
                        if (tail.size() > 4000)
                            tail.remove(0, tail.size() - 4000);
                    }
                    pending.clear();
                }
            } else {
                pending.append(c);
            }
        }
    };

    while (proc.state() != QProcess::NotRunning) {
        if (stop.load()) {
            proc.kill();
            proc.waitForFinished(5000);
            res.cancelled = true;
            return res;
        }
        proc.waitForReadyRead(200);
        consume(proc.readAll());
    }
    consume(proc.readAll());
    consume("\n");

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        // Keep the last few non-progress lines — the helper's own words.
        QStringList lines;
        for (const QByteArray& l : tail.split('\n')) {
            const QString s = QString::fromUtf8(l).trimmed();
            if (!s.isEmpty())
                lines << s;
        }
        while (lines.size() > 4)
            lines.removeFirst();
        res.error = QStringLiteral("The FLUX.3 helper failed (exit %1)%2")
                        .arg(proc.exitCode())
                        .arg(lines.isEmpty() ? QString() : QStringLiteral(": ") + lines.join(QStringLiteral(" | ")));
        return res;
    }
    if (!QFileInfo::exists(req.outputPath)) {
        res.error = QStringLiteral("The FLUX.3 helper finished but wrote no image");
        return res;
    }
    res.ok = true;
    return res;
}

} // namespace SdCliHelper
