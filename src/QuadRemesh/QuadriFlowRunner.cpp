#include "QuadriFlowRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <cstdio>

namespace QuadriFlowRunner {

QString defaultHelperPath()
{
    const QByteArray env = qgetenv("QTMESH_QUADRIFLOW_HELPER");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("qtmesh_quadriflow.exe");
#else
    const QString exe = QStringLiteral("qtmesh_quadriflow");
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

bool writeObj(const QString& path, const std::vector<float>& positions,
              const std::vector<uint32_t>& indices)
{
    // QByteArray::number is locale-independent; printf("%g") is not — under a
    // decimal-comma locale (pt_BR, de_DE…) it writes "0,5" and the helper
    // rejects every vertex line.
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QByteArray buf;
    buf.reserve(int(positions.size() * 12 + indices.size() * 8));
    const size_t nv = positions.size() / 3;
    for (size_t i = 0; i < nv; ++i) {
        buf += "v ";
        buf += QByteArray::number(positions[3 * i], 'g', 9);
        buf += ' ';
        buf += QByteArray::number(positions[3 * i + 1], 'g', 9);
        buf += ' ';
        buf += QByteArray::number(positions[3 * i + 2], 'g', 9);
        buf += '\n';
    }
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        buf += "f ";
        buf += QByteArray::number(indices[t] + 1);
        buf += ' ';
        buf += QByteArray::number(indices[t + 1] + 1);
        buf += ' ';
        buf += QByteArray::number(indices[t + 2] + 1);
        buf += '\n';
    }
    return f.write(buf) == buf.size() && f.flush();
}

bool readQuadObj(const QString& path, std::vector<float>& positions,
                 std::vector<std::vector<uint32_t>>& faces, QString* error)
{
    positions.clear();
    faces.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("cannot read remesh output");
        return false;
    }
    std::vector<std::vector<long>> raw;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.startsWith("v ")) {
            const QList<QByteArray> t = line.split(' ');
            if (t.size() < 4) continue;
            positions.push_back(t[1].toFloat());
            positions.push_back(t[2].toFloat());
            positions.push_back(t[3].toFloat());
        } else if (line.startsWith("f ")) {
            const QList<QByteArray> t = line.split(' ');
            std::vector<long> f;
            for (int i = 1; i < t.size(); ++i)
                if (!t[i].isEmpty())
                    f.push_back(t[i].split('/').first().toLong());
            raw.push_back(std::move(f));
        }
    }
    const long nv = static_cast<long>(positions.size() / 3);
    for (const auto& f : raw) {
        std::vector<uint32_t> face;
        for (long idx : f) {
            if (idx < 1 || idx > nv) { face.clear(); break; }
            const uint32_t v = static_cast<uint32_t>(idx - 1);
            // QuadriFlow emits a collapsed corner as a repeated index; keep the
            // distinct corners in order (a quad with one repeat is a triangle).
            if (std::find(face.begin(), face.end(), v) == face.end())
                face.push_back(v);
        }
        if (face.size() >= 3)
            faces.push_back(std::move(face));
    }
    if (positions.empty() || faces.empty()) {
        if (error) *error = QStringLiteral("remesh output is empty");
        return false;
    }
    return true;
}

Outcome run(const Request& req)
{
    Outcome out;
    const QString helper = req.helperPath.isEmpty() ? defaultHelperPath() : req.helperPath;
    if (helper.isEmpty() || !QFileInfo(helper).isExecutable()) {
        out.error = QStringLiteral("QuadriFlow helper (qtmesh_quadriflow) not found next to the "
                                   "editor — reinstall, or set QTMESH_QUADRIFLOW_HELPER");
        out.helperMissing = true;
        return out;
    }
    if (req.positions.size() < 9 || req.indices.size() < 3) {
        out.error = QStringLiteral("nothing to remesh");
        return out;
    }

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        out.error = QStringLiteral("cannot create a temporary directory");
        return out;
    }
    const QString in = tmp.filePath(QStringLiteral("in.obj"));
    const QString outPath = tmp.filePath(QStringLiteral("out.obj"));
    if (!writeObj(in, req.positions, req.indices)) {
        out.error = QStringLiteral("cannot write remesh input");
        return out;
    }

    QStringList args{QStringLiteral("-i"), in, QStringLiteral("-o"), outPath,
                     QStringLiteral("-f"), QString::number(std::max(16, req.targetFaces)),
                     QStringLiteral("-seed"), QString::number(req.seed)};
    if (req.preserveSharp) args << QStringLiteral("-sharp");
    if (req.preserveBoundary) args << QStringLiteral("-boundary");
    if (req.adaptiveScale) args << QStringLiteral("-adaptive");

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(helper, args);
    if (!proc.waitForStarted(15000)) {
        out.error = QStringLiteral("cannot start QuadriFlow helper: %1").arg(proc.errorString());
        return out;
    }

    QElapsedTimer clock;
    clock.start();
    QByteArray pending;
    bool done = false;
    QString helperError;
    auto consume = [&]() {
        pending += proc.readAllStandardOutput();
        int nl;
        while ((nl = pending.indexOf('\n')) >= 0) {
            const QByteArray line = pending.left(nl).trimmed();
            pending.remove(0, nl + 1);
            if (line.startsWith("QF_STAGE ")) {
                const QList<QByteArray> t = line.split(' ');
                if (t.size() >= 4 && req.progress
                    && !req.progress(t[1].toInt(), QString::fromLatin1(t[3]))) {
                    out.cancelled = true;
                }
            } else if (line.startsWith("QF_DONE")) {
                done = true;
            } else if (line.startsWith("QF_ERROR ")) {
                helperError = QString::fromUtf8(line.mid(9));
            }
        }
    };
    while (proc.state() != QProcess::NotRunning) {
        proc.waitForReadyRead(100);
        consume();
        // Give the UI thread's cancel a chance even when no output arrives.
        if (!out.cancelled && req.progress && !req.progress(0, QString()))
            out.cancelled = true;
        if (out.cancelled || (req.timeoutMs > 0 && clock.elapsed() > req.timeoutMs)) {
            out.timedOut = !out.cancelled;
            proc.kill();
            proc.waitForFinished(5000);
            break;
        }
    }
    consume();

    if (out.cancelled) {
        out.error = QStringLiteral("cancelled");
        return out;
    }
    if (out.timedOut) {
        out.error = QStringLiteral("QuadriFlow timed out after %1 s").arg(req.timeoutMs / 1000);
        return out;
    }
    if (!done) {
        if (!helperError.isEmpty())
            out.error = QStringLiteral("QuadriFlow failed: %1").arg(helperError);
        else if (proc.exitStatus() == QProcess::CrashExit)
            out.error = QStringLiteral("QuadriFlow crashed on this mesh");
        else
            out.error = QStringLiteral("QuadriFlow could not remesh this mesh (exit %1)")
                            .arg(proc.exitCode());
        return out;
    }
    QString readErr;
    if (!readQuadObj(outPath, out.positions, out.faces, &readErr)) {
        out.error = readErr;
        return out;
    }
    out.ok = true;
    return out;
}

} // namespace QuadriFlowRunner
