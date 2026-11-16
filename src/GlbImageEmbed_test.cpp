#include <gtest/gtest.h>

#include "GlbImageEmbed.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstring>

namespace {

quint32 u32(const QByteArray& b, qsizetype off)
{
    quint32 v = 0;
    std::memcpy(&v, b.constData() + off, 4);
    return v;
}

void put32(QByteArray& b, quint32 v)
{
    char c[4];
    std::memcpy(c, &v, 4);
    b.append(c, 4);
}

// Minimal GLB: one 12-byte buffer (stand-in geometry) + the given images.
QByteArray makeGlb(const QJsonArray& images)
{
    QJsonObject root;
    root.insert("asset", QJsonObject{{"version", "2.0"}});
    root.insert("buffers", QJsonArray{QJsonObject{{"byteLength", 12}}});
    root.insert("bufferViews", QJsonArray{QJsonObject{{"buffer", 0}, {"byteLength", 12}}});
    root.insert("images", images);
    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (json.size() % 4) json.append(' ');
    QByteArray bin("0123456789AB");
    QByteArray out;
    put32(out, 0x46546C67u);
    put32(out, 2u);
    put32(out, static_cast<quint32>(12 + 8 + json.size() + 8 + bin.size()));
    put32(out, static_cast<quint32>(json.size()));
    put32(out, 0x4E4F534Au);
    out.append(json);
    put32(out, static_cast<quint32>(bin.size()));
    put32(out, 0x004E4942u);
    out.append(bin);
    return out;
}

struct Parsed {
    QJsonObject json;
    QByteArray bin;
    quint32 declaredLength = 0;
    qsizetype fileSize = 0;
};

Parsed parse(const QString& path)
{
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray b = f.readAll();
    Parsed p;
    p.fileSize = b.size();
    p.declaredLength = u32(b, 8);
    const quint32 jlen = u32(b, 12);
    p.json = QJsonDocument::fromJson(b.mid(20, jlen)).object();
    const qsizetype binHdr = 20 + jlen;
    const quint32 blen = u32(b, binHdr);
    EXPECT_EQ(u32(b, binHdr + 4), 0x004E4942u);
    p.bin = b.mid(binHdr + 8, blen);
    EXPECT_EQ(jlen % 4, 0u);
    EXPECT_EQ(blen % 4, 0u);
    return p;
}

QByteArray pngBytes(const QColor& c)
{
    QImage img(2, 2, QImage::Format_RGBA8888);
    img.fill(c);
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return png;
}

void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(data);
}

} // namespace

TEST(GlbImageEmbed, MovesReferencedImagesIntoTheBinChunk)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray texA = pngBytes(Qt::red);
    const QByteArray texB = pngBytes(Qt::green);
    writeFile(dir.filePath("albedo.png"), texA);
    writeFile(dir.filePath("my rough.png"), texB);
    const QString dataUri = "data:image/png;base64,AAAA";
    const QString glb = dir.filePath("asset.glb");
    writeFile(glb, makeGlb(QJsonArray{
        QJsonObject{{"uri", "albedo.png"}},
        QJsonObject{{"uri", dataUri}},
        QJsonObject{{"uri", "missing.png"}},
        QJsonObject{{"uri", "my%20rough.png"}},   // percent-encoded, as exporters write it
    }));

    const auto r = GlbImageEmbed::embedExternalImages(glb);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.embedded, 2);
    EXPECT_EQ(r.missing, QStringList{"missing.png"});

    const Parsed p = parse(glb);
    EXPECT_EQ(static_cast<qsizetype>(p.declaredLength), p.fileSize);
    const QJsonArray images = p.json.value("images").toArray();
    const QJsonArray views = p.json.value("bufferViews").toArray();
    ASSERT_EQ(images.size(), 4);

    auto checkEmbedded = [&](int imageIndex, const QByteArray& expected) {
        const QJsonObject img = images.at(imageIndex).toObject();
        EXPECT_FALSE(img.contains("uri"));
        EXPECT_EQ(img.value("mimeType").toString(), QString("image/png"));
        const QJsonObject v = views.at(img.value("bufferView").toInt()).toObject();
        EXPECT_EQ(v.value("buffer").toInt(), 0);
        const qsizetype off = static_cast<qsizetype>(v.value("byteOffset").toDouble());
        const qsizetype len = static_cast<qsizetype>(v.value("byteLength").toDouble());
        EXPECT_EQ(p.bin.mid(off, len), expected);
    };
    checkEmbedded(0, texA);
    checkEmbedded(3, texB);
    EXPECT_EQ(images.at(1).toObject().value("uri").toString(), dataUri);
    EXPECT_EQ(images.at(2).toObject().value("uri").toString(), QString("missing.png"));

    // The original geometry bytes are untouched, buffer 0 covers everything.
    EXPECT_EQ(p.bin.left(12), QByteArray("0123456789AB"));
    const qsizetype declaredBuffer = static_cast<qsizetype>(
        p.json.value("buffers").toArray().at(0).toObject().value("byteLength").toDouble());
    EXPECT_GE(p.bin.size(), declaredBuffer);
    EXPECT_GT(declaredBuffer, 12);
}

TEST(GlbImageEmbed, FileWithNothingToEmbedIsNotRewritten)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString glb = dir.filePath("asset.glb");
    const QByteArray original = makeGlb(QJsonArray{QJsonObject{{"uri", "missing.png"}}});
    writeFile(glb, original);
    const auto r = GlbImageEmbed::embedExternalImages(glb);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.embedded, 0);
    QFile f(glb);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), original);
}

TEST(GlbImageEmbed, RejectsNonGlbInput)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("asset.gltf");
    writeFile(path, QByteArray("{\"asset\":{\"version\":\"2.0\"}}"));
    const auto r = GlbImageEmbed::embedExternalImages(path);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(GlbImageEmbed, OtherFormatsAreReEncodedAsPngOrLeftExternal)
{
    // glTF readers decode PNG/JPEG/WebP only: a BMP must not be embedded with
    // its own bytes under an image/png label, and an unreadable format must
    // stay an external reference.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage img(3, 2, QImage::Format_RGB32);
    img.fill(QColor(10, 200, 30));
    ASSERT_TRUE(img.save(dir.filePath("albedo.bmp"), "BMP"));
    writeFile(dir.filePath("sky.hdr"), QByteArray("#?RADIANCE\nnot really\n"));
    const QString glb = dir.filePath("asset.glb");
    writeFile(glb, makeGlb(QJsonArray{QJsonObject{{"uri", "albedo.bmp"}},
                                      QJsonObject{{"uri", "sky.hdr"}}}));

    const auto r = GlbImageEmbed::embedExternalImages(glb);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.embedded, 1);
    EXPECT_EQ(r.unsupported, QStringList{"sky.hdr"});

    const Parsed p = parse(glb);
    const QJsonArray images = p.json.value("images").toArray();
    const QJsonObject bmp = images.at(0).toObject();
    EXPECT_EQ(bmp.value("mimeType").toString(), QString("image/png"));
    const QJsonObject v = p.json.value("bufferViews").toArray()
                              .at(bmp.value("bufferView").toInt()).toObject();
    const QByteArray bytes = p.bin.mid(static_cast<qsizetype>(v.value("byteOffset").toDouble()),
                                       static_cast<qsizetype>(v.value("byteLength").toDouble()));
    EXPECT_TRUE(bytes.startsWith("\x89PNG"));
    QImage back;
    ASSERT_TRUE(back.loadFromData(bytes, "PNG"));
    EXPECT_EQ(back.size(), QSize(3, 2));
    EXPECT_EQ(QColor(back.pixel(1, 1)), QColor(10, 200, 30));
    EXPECT_EQ(images.at(1).toObject().value("uri").toString(), QString("sky.hdr"));
}

TEST(GlbImageEmbed, LiteralFileNameWinsOverItsPercentDecodedTwin)
{
    // Assimp writes names unencoded: `rough%20map.png` names that very file,
    // even when a `rough map.png` sits next to it.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray literal = pngBytes(Qt::blue);
    const QByteArray decoded = pngBytes(Qt::yellow);
    writeFile(dir.filePath("rough%20map.png"), literal);
    writeFile(dir.filePath("rough map.png"), decoded);
    const QString glb = dir.filePath("asset.glb");
    writeFile(glb, makeGlb(QJsonArray{QJsonObject{{"uri", "rough%20map.png"}}}));

    const auto r = GlbImageEmbed::embedExternalImages(glb);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.embedded, 1);
    const Parsed p = parse(glb);
    const QJsonObject img = p.json.value("images").toArray().at(0).toObject();
    const QJsonObject v = p.json.value("bufferViews").toArray()
                              .at(img.value("bufferView").toInt()).toObject();
    EXPECT_EQ(p.bin.mid(static_cast<qsizetype>(v.value("byteOffset").toDouble()),
                        static_cast<qsizetype>(v.value("byteLength").toDouble())),
              literal);
}

TEST(GlbImageEmbed, WebPIsReEncodedAsPng)
{
    // Core glTF has no image/webp; embedding it as-is would need
    // EXT_texture_webp, which the exporter does not declare.
    // 4x2 lossless WebP (the fixture of WebPImage_test.cpp).
    static const unsigned char kWebP[] = {
        0x52, 0x49, 0x46, 0x46, 0x4c, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4c,
        0x40, 0x00, 0x00, 0x00, 0x2f, 0x03, 0x40, 0x00, 0x10, 0x3f, 0x40, 0x16, 0x60, 0xb2, 0x20, 0xfa,
        0x82, 0xca, 0x5f, 0x66, 0x1a, 0x64, 0xc2, 0x10, 0xfb, 0xf5, 0xef, 0xe2, 0x52, 0x0d, 0x99, 0x80,
        0xc5, 0x74, 0xf0, 0x6b, 0xa4, 0x7f, 0x9b, 0x11, 0x48, 0xda, 0xd8, 0x7c, 0x3c, 0xf9, 0x03, 0x80,
        0x57, 0xe4, 0x90, 0x6b, 0xd6, 0x57, 0xeb, 0x41, 0x28, 0x80, 0xc6, 0x60, 0x22, 0x09, 0xa6, 0x5f,
        0x44, 0xff, 0x03, 0x1b};
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeFile(dir.filePath("albedo.webp"),
              QByteArray(reinterpret_cast<const char*>(kWebP), sizeof(kWebP)));
    const QString glb = dir.filePath("asset.glb");
    writeFile(glb, makeGlb(QJsonArray{QJsonObject{{"uri", "albedo.webp"}}}));

    const auto r = GlbImageEmbed::embedExternalImages(glb);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.embedded, 1);
    const Parsed p = parse(glb);
    const QJsonObject img = p.json.value("images").toArray().at(0).toObject();
    EXPECT_EQ(img.value("mimeType").toString(), QString("image/png"));
    const QJsonObject v = p.json.value("bufferViews").toArray()
                              .at(img.value("bufferView").toInt()).toObject();
    QImage back;
    ASSERT_TRUE(back.loadFromData(
        p.bin.mid(static_cast<qsizetype>(v.value("byteOffset").toDouble()),
                  static_cast<qsizetype>(v.value("byteLength").toDouble())), "PNG"));
    EXPECT_EQ(back.size(), QSize(4, 2));
}
