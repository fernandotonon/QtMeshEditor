#include "AppConsoleLog.h"
#include "ConsoleLogSanitize.h"
#include "mainwindow.h"

#include <QApplication>
#include <QList>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QByteArrayView>
#include <QThread>
#include <QtGlobal>

#include <atomic>
#include <cstdio>

#ifndef Q_OS_WIN
#include <unistd.h>
#else
#include <fcntl.h>
#include <io.h>
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
#endif

namespace {

QMutex s_mutex;
QList<QString> s_pending;
MainWindow* s_window = nullptr;
QtMessageHandler s_prevHandler = nullptr;
bool s_installed = false;

constexpr int kMaxPendingLines = 8000;

// --- Stdio capture (GUI only) ---------------------------------------------
std::atomic_bool s_stdioCaptureActive{false};
std::atomic_bool s_stdioFlushScheduled{false};

QMutex s_stdioChunkMutex;
QList<QByteArray> s_stdioChunks;

int s_savedStdout = -1;
int s_savedStderr = -1;
int s_pipeRead = -1;

class StdioReaderThread final : public QThread {
public:
    explicit StdioReaderThread(int readFd)
        : m_readFd(readFd) {}

protected:
    void run() override
    {
        char buf[4096];
        for (;;) {
#ifndef Q_OS_WIN
            const ssize_t n = ::read(m_readFd, buf, sizeof(buf));
#else
            const int n = ::_read(m_readFd, buf, static_cast<unsigned>(sizeof(buf)));
#endif
            if (n <= 0)
                break;

            {
                QMutexLocker lock(&s_stdioChunkMutex);
                s_stdioChunks.append(QByteArray(buf, int(n)));
            }
            if (!s_stdioFlushScheduled.exchange(true))
                QMetaObject::invokeMethod(qApp, []() { AppConsoleLog::flushStdioChunks(); }, Qt::QueuedConnection);
        }
    }

private:
    int m_readFd;
};

StdioReaderThread* s_reader = nullptr;

/// Bytes that are not yet a complete UTF-8 code point sequence (carried across read() calls)
static QByteArray s_utf8Carry;

void emitConsoleLine(const QString& line)
{
    MainWindow* target = nullptr;
    {
        QMutexLocker lock(&s_mutex);
        target = s_window;
        if (!target) {
            s_pending.append(line);
            while (s_pending.size() > kMaxPendingLines)
                s_pending.removeFirst();
            return;
        }
    }

    QMetaObject::invokeMethod(target, "appendConsoleLine", Qt::QueuedConnection,
                              Q_ARG(QString, line));
}

void consoleMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (s_prevHandler)
        s_prevHandler(type, ctx, msg);

    // When stdout/stderr are teed into the pipe, the chained handler already
    // wrote this message there — avoid duplicating it in the console widget.
    if (s_stdioCaptureActive.load())
        return;

    const char* tag = "I";
    switch (type) {
    case QtDebugMsg: tag = "D"; break;
    case QtInfoMsg: tag = "I"; break;
    case QtWarningMsg: tag = "W"; break;
    case QtCriticalMsg: tag = "C"; break;
    case QtFatalMsg: tag = "F"; break;
    }
    Q_UNUSED(ctx);
    emitConsoleLine(QStringLiteral("[%1] %2").arg(QString::fromLatin1(tag), msg));
}

/// Text after last newline (may span multiple read() calls)
static QString s_partialStdoutLine;

static void appendCompleteStdioLines(const QString& decodedChunk)
{
    if (decodedChunk.isEmpty())
        return;
    s_partialStdoutLine += decodedChunk;
    for (;;) {
        const int nl = s_partialStdoutLine.indexOf(QLatin1Char('\n'));
        if (nl < 0)
            break;
        QString line = s_partialStdoutLine.left(nl);
        s_partialStdoutLine.remove(0, nl + 1);
        if (!line.isEmpty() && line.back() == QLatin1Char('\r'))
            line.chop(1);
        const QString cleaned = ConsoleLogSanitize::sanitizeCapturedStdioLine(line);
        if (!cleaned.isEmpty())
            emitConsoleLine(cleaned);
    }
}

} // namespace

namespace AppConsoleLog {

void install()
{
    if (s_installed)
        return;
    s_installed = true;
    s_prevHandler = qInstallMessageHandler(consoleMessageHandler);
}

bool installStdioCapture()
{
#ifndef Q_OS_WIN
    std::fflush(stdout);
    std::fflush(stderr);

    int p[2] = {-1, -1};
    if (::pipe(p) != 0)
        return false;

    s_savedStdout = ::dup(STDOUT_FILENO);
    s_savedStderr = ::dup(STDERR_FILENO);
    if (s_savedStdout < 0 || s_savedStderr < 0) {
        ::close(p[0]);
        ::close(p[1]);
        s_savedStdout = s_savedStderr = -1;
        return false;
    }

    if (::dup2(p[1], STDOUT_FILENO) < 0 || ::dup2(p[1], STDERR_FILENO) < 0) {
        ::close(p[0]);
        ::close(p[1]);
        ::dup2(s_savedStdout, STDOUT_FILENO);
        ::dup2(s_savedStderr, STDERR_FILENO);
        ::close(s_savedStdout);
        ::close(s_savedStderr);
        s_savedStdout = s_savedStderr = -1;
        return false;
    }
    ::close(p[1]);
    s_pipeRead = p[0];
#else
    std::fflush(stdout);
    std::fflush(stderr);

    int p[2] = {-1, -1};
    if (::_pipe(p, 65536, _O_BINARY) != 0)
        return false;

    s_savedStdout = ::_dup(STDOUT_FILENO);
    s_savedStderr = ::_dup(STDERR_FILENO);
    if (s_savedStdout < 0 || s_savedStderr < 0) {
        ::_close(p[0]);
        ::_close(p[1]);
        s_savedStdout = s_savedStderr = -1;
        return false;
    }

    if (::_dup2(p[1], STDOUT_FILENO) < 0 || ::_dup2(p[1], STDERR_FILENO) < 0) {
        ::_close(p[0]);
        ::_close(p[1]);
        ::_dup2(s_savedStdout, STDOUT_FILENO);
        ::_dup2(s_savedStderr, STDERR_FILENO);
        ::_close(s_savedStdout);
        ::_close(s_savedStderr);
        s_savedStdout = s_savedStderr = -1;
        return false;
    }
    ::_close(p[1]);
    s_pipeRead = p[0];
#endif

    s_utf8Carry.clear();
    s_partialStdoutLine.clear();
    s_stdioCaptureActive.store(true);

    s_reader = new StdioReaderThread(s_pipeRead);
    s_reader->start();

    return true;
}

void flushStdioChunks()
{
    s_stdioFlushScheduled.store(false);

    QList<QByteArray> chunks;
    {
        QMutexLocker lock(&s_stdioChunkMutex);
        chunks = std::move(s_stdioChunks);
    }

    QByteArray merged = s_utf8Carry;
    s_utf8Carry.clear();
    for (const QByteArray& c : chunks)
        merged.append(c);

    const int pre = ConsoleLogSanitize::utf8CompletePrefixLength(merged);
    const QString decodedChunk = QString::fromUtf8(QByteArrayView(merged.constData(), pre));
    s_utf8Carry = merged.mid(pre);

    appendCompleteStdioLines(decodedChunk);
}

void shutdown()
{
    s_stdioCaptureActive.store(false);

    if (s_reader) {
        std::fflush(stdout);
        std::fflush(stderr);

#ifndef Q_OS_WIN
        if (s_savedStdout >= 0)
            ::dup2(s_savedStdout, STDOUT_FILENO);
        if (s_savedStderr >= 0)
            ::dup2(s_savedStderr, STDERR_FILENO);
        if (s_savedStdout >= 0) {
            ::close(s_savedStdout);
            s_savedStdout = -1;
        }
        if (s_savedStderr >= 0) {
            ::close(s_savedStderr);
            s_savedStderr = -1;
        }
        if (s_pipeRead >= 0) {
            ::close(s_pipeRead);
            s_pipeRead = -1;
        }
#else
        if (s_savedStdout >= 0)
            ::_dup2(s_savedStdout, STDOUT_FILENO);
        if (s_savedStderr >= 0)
            ::_dup2(s_savedStderr, STDERR_FILENO);
        if (s_savedStdout >= 0) {
            ::_close(s_savedStdout);
            s_savedStdout = -1;
        }
        if (s_savedStderr >= 0) {
            ::_close(s_savedStderr);
            s_savedStderr = -1;
        }
        if (s_pipeRead >= 0) {
            ::_close(s_pipeRead);
            s_pipeRead = -1;
        }
#endif

        s_reader->wait();
        delete s_reader;
        s_reader = nullptr;
    }

    flushStdioChunks();

    if (!s_utf8Carry.isEmpty()) {
        appendCompleteStdioLines(QString::fromUtf8(s_utf8Carry));
        s_utf8Carry.clear();
    }
    if (!s_partialStdoutLine.isEmpty()) {
        const QString cleaned = ConsoleLogSanitize::sanitizeCapturedStdioLine(s_partialStdoutLine);
        if (!cleaned.isEmpty())
            emitConsoleLine(cleaned);
        s_partialStdoutLine.clear();
    }

    {
        QMutexLocker lock(&s_stdioChunkMutex);
        s_stdioChunks.clear();
    }
}

void attachMainWindow(MainWindow* window)
{
    if (!window)
        return;

    install();

    QList<QString> backlog;
    {
        QMutexLocker lock(&s_mutex);
        s_window = window;
        backlog = s_pending;
        s_pending.clear();
    }

    for (const QString& line : backlog) {
        QMetaObject::invokeMethod(window, "appendConsoleLine", Qt::QueuedConnection,
                                  Q_ARG(QString, line));
    }
}

void detachMainWindow(MainWindow* window)
{
    QMutexLocker lock(&s_mutex);
    if (s_window == window)
        s_window = nullptr;
}

} // namespace AppConsoleLog
