#include "ArkitTemplate.h"

#include "../ModelDownloader.h"
#include "../SentryReporter.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QtEndian>

#include <cmath>
#include <cstring>

namespace FaceRig {

namespace {
constexpr const char* kModelFile = "arkit_template.bin";
constexpr const char* kMagic = "QMFRT1\0\0";  // 8 bytes
constexpr int kMagicLen = 8;
constexpr int kNameLen = 32;
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/facerig/";
constexpr const char* kBaseUrlSettingsKey = "ai/facerigModelBaseUrl";

// little-endian readers over a byte cursor
int32_t rdI32(const char*& p)
{
    int32_t v;
    std::memcpy(&v, p, 4);
    p += 4;
    return qFromLittleEndian(v);
}
float rdF32(const char*& p)
{
    quint32 raw;
    std::memcpy(&raw, p, 4);
    p += 4;
    raw = qFromLittleEndian(raw);
    float f;
    std::memcpy(&f, &raw, 4);
    return f;
}
}  // namespace

QStringList ArkitTemplate::shapeNames() const
{
    QStringList out;
    for (const auto& s : m_shapes)
        out << s.name;
    return out;
}

bool ArkitTemplate::load(const QString& path, QString* error)
{
    auto fail = [&](const QString& msg) {
        if (error)
            *error = msg;
        return false;
    };

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open %1").arg(path));
    const QByteArray blob = f.readAll();
    f.close();

    // header: magic(8) + 3*int32
    if (blob.size() < kMagicLen + 12)
        return fail(QStringLiteral("template too small / truncated"));
    if (std::memcmp(blob.constData(), kMagic, kMagicLen) != 0)
        return fail(QStringLiteral("bad magic (not an arkit_template.bin)"));

    const char* p = blob.constData() + kMagicLen;
    const char* end = blob.constData() + blob.size();
    const int V = rdI32(p);
    const int F = rdI32(p);
    const int S = rdI32(p);
    if (V <= 0 || F <= 0 || S <= 0 || V > 5'000'000 || S > 128)
        return fail(QStringLiteral("implausible header (V=%1 F=%2 S=%3)")
                        .arg(V).arg(F).arg(S));

    // exact byte budget check up front so a corrupt file can't over-read
    const qint64 need = qint64(kMagicLen) + 12 + qint64(V) * 3 * 4
                        + qint64(F) * 3 * 4
                        + qint64(S) * (kNameLen + qint64(V) * 3 * 4);
    if (blob.size() < need)
        return fail(QStringLiteral("template truncated (need %1 bytes, have %2)")
                        .arg(need).arg(blob.size()));

    m_vertexCount = V;
    m_faceCount = F;
    m_neutral.resize(size_t(V) * 3);
    for (auto& v : m_neutral) {
        v = rdF32(p);
        if (!std::isfinite(v))
            return fail(QStringLiteral("non-finite neutral position"));
    }
    m_faces.resize(size_t(F) * 3);
    for (auto& i : m_faces) {
        i = rdI32(p);
        // out-of-range indices would be dereferenced by the fit / renders
        if (i < 0 || i >= V)
            return fail(QStringLiteral("face index %1 out of range (V=%2)")
                            .arg(i).arg(V));
    }

    m_shapes.clear();
    m_shapes.reserve(S);
    for (int s = 0; s < S; ++s) {
        if (p + kNameLen > end)
            return fail(QStringLiteral("shape %1 name overruns").arg(s));
        ArkitShape shape;
        shape.name = QString::fromLatin1(p, qstrnlen(p, kNameLen));
        p += kNameLen;
        shape.deltas.resize(size_t(V) * 3);
        for (auto& d : shape.deltas) {
            d = rdF32(p);
            if (!std::isfinite(d))
                return fail(QStringLiteral("non-finite delta in shape %1")
                                .arg(shape.name));
        }
        m_shapes.push_back(std::move(shape));
    }
    return true;
}

QString ArkitTemplate::modelPath()
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(QStringLiteral("ai_models/facerig/")
                                   + QString::fromLatin1(kModelFile));
}

bool ArkitTemplate::present() { return QFileInfo::exists(modelPath()); }

QString ArkitTemplate::ensureModelBlocking()
{
    const QString dest = modelPath();
    if (QFileInfo::exists(dest))
        return dest;
    if (!qEnvironmentVariableIsEmpty("QTMESH_FACERIG_NO_DOWNLOAD"))
        return {};

    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_FACERIG_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty())
        return {};
    if (!base.endsWith('/'))
        base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl)
        return {};

    QDir().mkpath(QFileInfo(dest).absolutePath());
    const QString url = base + QString::fromLatin1(kModelFile);
    const QString label = QStringLiteral("ARKit face template");

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.face_rig"),
        QStringLiteral("ARKit template download start"));

    QEventLoop loop;
    bool ok = false, timedOut = false, done = false;
    auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted, &loop,
        [&](const QString& name, const QString&) {
            if (name == label) { ok = true; done = true; loop.quit(); }
        });
    auto onErr = QObject::connect(dl, &ModelDownloader::downloadError, &loop,
        [&](const QString& name, const QString&) {
            if (name == label) { ok = false; done = true; loop.quit(); }
        });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop,
                     [&]() { timedOut = true; loop.quit(); });
    timeout.start(300000);  // 5 min — the template is ~17 MB

    dl->startDownload(url, dest, label);
    // `done` guards the synchronous-failure case: startDownload can emit
    // downloadError DURING the call (another download active, .part file
    // unopenable) — entering the loop then would block for the full timeout.
    if (!done)
        loop.exec();

    QObject::disconnect(onDone);
    QObject::disconnect(onErr);
    if (timedOut && dl)
        dl->cancelDownload();

    const bool success = ok && !timedOut && QFileInfo::exists(dest);
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.face_rig"),
        success ? QStringLiteral("ARKit template download ok")
                : QStringLiteral("ARKit template download failed%1")
                      .arg(timedOut ? QStringLiteral(" (timeout)") : QString()));
    return success ? dest : QString();
}

}  // namespace FaceRig
