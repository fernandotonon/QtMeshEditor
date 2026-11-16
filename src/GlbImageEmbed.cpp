#include "GlbImageEmbed.h"
#include "WebPImage.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>

#include <cstring>

namespace GlbImageEmbed {

namespace {

constexpr quint32 kMagic = 0x46546C67u;     // "glTF"
constexpr quint32 kChunkJson = 0x4E4F534Au; // "JSON"
constexpr quint32 kChunkBin = 0x004E4942u;  // "BIN\0"

quint32 readU32(const QByteArray& b, qsizetype off)
{
    quint32 v = 0;
    std::memcpy(&v, b.constData() + off, 4); // GLB is little-endian; so are our targets
    return v;
}

void appendU32(QByteArray& b, quint32 v)
{
    char c[4];
    std::memcpy(c, &v, 4);
    b.append(c, 4);
}

void padTo4(QByteArray& b, char fill)
{
    while (b.size() % 4 != 0)
        b.append(fill);
}

// Core glTF images are PNG or JPEG only. Identify those by their bytes, never
// by the file name. (WebP would also need EXT_texture_webp on the texture and
// in extensionsUsed/Required, which Assimp's exporter does not write, so it is
// re-encoded like any other format.)
QString mimeFor(const QByteArray& data)
{
    if (data.startsWith("\x89PNG"))
        return QStringLiteral("image/png");
    if (data.size() >= 2 && static_cast<uchar>(data[0]) == 0xFF
        && static_cast<uchar>(data[1]) == 0xD8)
        return QStringLiteral("image/jpeg");
    return {};
}

// Anything else (WebP, BMP, TGA, ...) is re-encoded as PNG so the embedded
// bytes match their mimeType. False when it cannot be read (e.g. HDR): the
// image then stays an external reference rather than being embedded
// mislabelled.
bool transcodeToPng(QByteArray& data, const QString& fileName)
{
    QImage img;
    if (WebPImage::looksLikeWebP(data.constData(), static_cast<size_t>(data.size())))
        img = WebPImage::decode(data.constData(), static_cast<size_t>(data.size()));
    else if (!img.loadFromData(data, QFileInfo(fileName).suffix().toLatin1().constData()))
        img.loadFromData(data);
    if (img.isNull())
        return false;
    QByteArray png;
    QBuffer buf(&png);
    if (!buf.open(QIODevice::WriteOnly) || !img.save(&buf, "PNG"))
        return false;
    data = png;
    return true;
}

} // namespace

Result embedExternalImages(const QString& glbPath)
{
    Result r;
    QFile in(glbPath);
    if (!in.open(QIODevice::ReadOnly)) {
        r.error = QStringLiteral("cannot open %1").arg(glbPath);
        return r;
    }
    const QByteArray whole = in.readAll();
    in.close();
    if (whole.size() < 12 || readU32(whole, 0) != kMagic) {
        r.error = QStringLiteral("not a binary glTF");
        return r;
    }

    QByteArray json;
    QByteArray bin;
    bool haveBin = false;
    for (qsizetype off = 12; off + 8 <= whole.size();) {
        const quint32 len = readU32(whole, off);
        const quint32 type = readU32(whole, off + 4);
        const qsizetype data = off + 8;
        if (data + static_cast<qsizetype>(len) > whole.size()) {
            r.error = QStringLiteral("truncated chunk");
            return r;
        }
        if (type == kChunkJson && json.isEmpty())
            json = whole.mid(data, len);
        else if (type == kChunkBin && !haveBin) {
            bin = whole.mid(data, len);
            haveBin = true;
        }
        off = data + static_cast<qsizetype>(len);
    }
    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        r.error = QStringLiteral("bad JSON chunk: %1").arg(perr.errorString());
        return r;
    }
    QJsonObject root = doc.object();
    QJsonArray images = root.value(QStringLiteral("images")).toArray();
    if (images.isEmpty()) {
        r.ok = true;
        return r;
    }

    QJsonArray buffers = root.value(QStringLiteral("buffers")).toArray();
    // The GLB BIN chunk is buffer 0, which carries no uri. Create it if the
    // file had none (an export with no geometry data would be unusual, but a
    // buffer 0 WITH a uri cannot back the BIN chunk, so refuse that case).
    if (buffers.isEmpty()) {
        buffers.append(QJsonObject{{QStringLiteral("byteLength"), 0}});
    } else if (buffers.at(0).toObject().contains(QStringLiteral("uri"))) {
        r.error = QStringLiteral("buffer 0 is external; cannot append to the BIN chunk");
        return r;
    }
    // Buffer 0's own byteLength may be shorter than the padded chunk.
    const qsizetype declared = static_cast<qsizetype>(
        buffers.at(0).toObject().value(QStringLiteral("byteLength")).toDouble());
    if (declared > bin.size()) {
        r.error = QStringLiteral("buffer 0 is longer than the BIN chunk");
        return r;
    }
    bin.truncate(declared);

    QJsonArray views = root.value(QStringLiteral("bufferViews")).toArray();
    const QDir dir = QFileInfo(glbPath).absoluteDir();
    for (qsizetype i = 0; i < images.size(); ++i) {
        QJsonObject img = images.at(i).toObject();
        const QString uri = img.value(QStringLiteral("uri")).toString();
        if (uri.isEmpty() || uri.startsWith(QLatin1String("data:"))
            || img.contains(QStringLiteral("bufferView")))
            continue;
        // Assimp writes the file name as-is (no percent-encoding), while
        // conforming writers percent-encode it: try the literal name first so
        // a file really named `a%20b.png` is not swapped for `a b.png`.
        auto resolve = [&dir](const QString& name) {
            return QFileInfo(name).isAbsolute() ? name : dir.filePath(name);
        };
        QString path = resolve(uri);
        if (!QFileInfo::exists(path))
            path = resolve(QUrl::fromPercentEncoding(uri.toUtf8()));
        QFile tex(path);
        if (!tex.open(QIODevice::ReadOnly)) {
            r.missing.append(uri);
            continue;
        }
        QByteArray data = tex.readAll();
        if (data.isEmpty()) {
            r.missing.append(uri);
            continue;
        }
        QString mime = mimeFor(data);
        if (mime.isEmpty()) {
            if (!transcodeToPng(data, path)) {
                r.unsupported.append(uri);
                continue;
            }
            mime = QStringLiteral("image/png");
        }
        padTo4(bin, '\0');
        QJsonObject view;
        view.insert(QStringLiteral("buffer"), 0);
        view.insert(QStringLiteral("byteOffset"), static_cast<double>(bin.size()));
        view.insert(QStringLiteral("byteLength"), static_cast<double>(data.size()));
        bin.append(data);
        img.remove(QStringLiteral("uri"));
        img.insert(QStringLiteral("bufferView"), static_cast<int>(views.size()));
        img.insert(QStringLiteral("mimeType"), mime);
        views.append(view);
        images[i] = img;
        ++r.embedded;
    }
    if (r.embedded == 0) {
        r.ok = true;
        return r;
    }

    QJsonObject buf0 = buffers.at(0).toObject();
    buf0.insert(QStringLiteral("byteLength"), static_cast<double>(bin.size()));
    buffers[0] = buf0;
    root.insert(QStringLiteral("buffers"), buffers);
    root.insert(QStringLiteral("bufferViews"), views);
    root.insert(QStringLiteral("images"), images);

    QByteArray outJson = QJsonDocument(root).toJson(QJsonDocument::Compact);
    padTo4(outJson, ' ');
    padTo4(bin, '\0');
    QByteArray out;
    appendU32(out, kMagic);
    appendU32(out, 2u);
    appendU32(out, static_cast<quint32>(12 + 8 + outJson.size() + 8 + bin.size()));
    appendU32(out, static_cast<quint32>(outJson.size()));
    appendU32(out, kChunkJson);
    out.append(outJson);
    appendU32(out, static_cast<quint32>(bin.size()));
    appendU32(out, kChunkBin);
    out.append(bin);

    QSaveFile save(glbPath);
    if (!save.open(QIODevice::WriteOnly) || save.write(out) != out.size() || !save.commit()) {
        r.error = QStringLiteral("cannot write %1").arg(glbPath);
        r.embedded = 0;
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace GlbImageEmbed
