#include "BrushAssetLibrary.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QStandardPaths>

#include <algorithm>
#include <cstring>

namespace BrushAssetLibrary {
namespace {

std::string qstrToStd(const QString& s)
{
    return s.toUtf8().constData();
}

QString bundledRoot()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString direct = QDir(appDir).filePath(QStringLiteral("media/paint"));
    if (QDir(direct).exists())
        return direct;
    const QString nested = QDir(appDir).filePath(QStringLiteral("../media/paint"));
    if (QDir(nested).exists())
        return nested;
    return direct;
}

QString subdirFor(AssetKind kind)
{
    return kind == AssetKind::Stamp ? QStringLiteral("stamps") : QStringLiteral("tilings");
}

QString userRoot()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        return {};
    return QDir(base).filePath(QStringLiteral("paint"));
}

QString ensureUserSubdir(AssetKind kind)
{
    const QString root = userRoot();
    if (root.isEmpty())
        return {};
    const QString dir = QDir(root).filePath(subdirFor(kind));
    QDir().mkpath(dir);
    return dir;
}

QString bundledSubdir(AssetKind kind)
{
    return QDir(bundledRoot()).filePath(subdirFor(kind));
}

BrushFootprint::ImageRgba fromQImage(const QImage& img)
{
    BrushFootprint::ImageRgba out;
    if (img.isNull())
        return out;
    QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    out.width = rgba.width();
    out.height = rgba.height();
    out.pixels.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4u);
    for (int y = 0; y < out.height; ++y) {
        const auto* row = rgba.constScanLine(y);
        std::memcpy(out.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(out.width) * 4u,
                    row, static_cast<size_t>(out.width) * 4u);
    }
    return out;
}

} // namespace

std::vector<std::string> bundledStampNames()
{
    return {"Soft Circle", "Hard Circle", "Charcoal", "Spatter",
            "Scratch Lines", "Foliage Cluster"};
}

std::vector<std::string> bundledTilingNames()
{
    return {"Wood", "Brick", "Fabric", "Concrete"};
}

std::string stampsDirectory()
{
    const QString custom = ensureUserSubdir(AssetKind::Stamp);
    return custom.isEmpty() ? qstrToStd(bundledSubdir(AssetKind::Stamp)) : qstrToStd(custom);
}

std::string tilingsDirectory()
{
    const QString custom = ensureUserSubdir(AssetKind::Tiling);
    return custom.isEmpty() ? qstrToStd(bundledSubdir(AssetKind::Tiling)) : qstrToStd(custom);
}

std::string safeFileStem(const std::string& name)
{
    QString stem = QString::fromUtf8(name.c_str()).trimmed();
    stem.replace(QChar('/'), QChar('_'));
    stem.replace(QChar('\\'), QChar('_'));
    if (stem.isEmpty())
        stem = QStringLiteral("asset");
    return qstrToStd(stem);
}

std::string resolvePath(const std::string& name, AssetKind kind)
{
    const QString stem = QString::fromUtf8(safeFileStem(name).c_str());
    const QString fileName = stem + QStringLiteral(".png");

    const QString userDir = ensureUserSubdir(kind);
    if (!userDir.isEmpty()) {
        const QString userPath = QDir(userDir).filePath(fileName);
        if (QFile::exists(userPath))
            return qstrToStd(userPath);
    }

    const QString bundledPath = QDir(bundledSubdir(kind)).filePath(fileName);
    if (QFile::exists(bundledPath))
        return qstrToStd(bundledPath);

    return {};
}

BrushFootprint::ImageRgba loadImage(const std::string& path)
{
    if (path.empty())
        return {};
    QImage img(QString::fromUtf8(path.c_str()));
    return fromQImage(img);
}

std::vector<AssetInfo> listAssets(AssetKind kind)
{
    std::vector<AssetInfo> out;
    const auto addFromDir = [&](const QString& dir, bool bundled) {
        QDir d(dir);
        if (!d.exists())
            return;
        const QStringList files = d.entryList({QStringLiteral("*.png"), QStringLiteral("*.tga"),
                                               QStringLiteral("*.jpg"), QStringLiteral("*.jpeg")},
                                              QDir::Files, QDir::Name);
        for (const QString& f : files) {
            AssetInfo info;
            info.kind = kind;
            info.bundled = bundled;
            info.path = qstrToStd(d.filePath(f));
            info.name = qstrToStd(QFileInfo(f).completeBaseName());
            out.push_back(std::move(info));
        }
    };

    addFromDir(bundledSubdir(kind), true);
    addFromDir(ensureUserSubdir(kind), false);

    std::vector<AssetInfo> merged;
    for (const AssetInfo& item : out) {
        auto it = std::find_if(merged.begin(), merged.end(), [&](const AssetInfo& m) {
            return QString::compare(QString::fromUtf8(m.name.c_str()),
                                  QString::fromUtf8(item.name.c_str()),
                                  Qt::CaseInsensitive) == 0;
        });
        if (it == merged.end()) {
            merged.push_back(item);
        } else if (!item.bundled) {
            *it = item;
        }
    }
    return merged;
}

std::string importAsset(const std::string& sourcePath, AssetKind kind,
                        const std::string& desiredName)
{
    const QString src = QString::fromUtf8(sourcePath.c_str());
    if (!QFile::exists(src))
        return {};
    const QString dir = ensureUserSubdir(kind);
    if (dir.isEmpty())
        return {};

    QString name = desiredName.empty()
        ? QFileInfo(src).completeBaseName()
        : QString::fromUtf8(desiredName.c_str());
    name = QString::fromUtf8(safeFileStem(qstrToStd(name)).c_str());
    const QString dest = QDir(dir).filePath(name + QStringLiteral(".png"));
    if (QFile::exists(dest))
        return {};

    QImage img(src);
    if (img.isNull())
        return {};
    if (!img.save(dest, "PNG"))
        return {};
    return qstrToStd(dest);
}

bool deleteCustom(const std::string& name, AssetKind kind)
{
    const QString dir = ensureUserSubdir(kind);
    if (dir.isEmpty())
        return false;
    const QString path = QDir(dir).filePath(QString::fromUtf8(safeFileStem(name).c_str())
                                            + QStringLiteral(".png"));
    if (!QFile::exists(path))
        return false;
    return QFile::remove(path);
}

bool renameCustom(const std::string& oldName, const std::string& newName, AssetKind kind)
{
    const QString dir = ensureUserSubdir(kind);
    if (dir.isEmpty())
        return false;
    const QString oldPath = QDir(dir).filePath(QString::fromUtf8(safeFileStem(oldName).c_str())
                                               + QStringLiteral(".png"));
    if (!QFile::exists(oldPath))
        return false;
    const QString newPath = QDir(dir).filePath(QString::fromUtf8(safeFileStem(newName).c_str())
                                               + QStringLiteral(".png"));
    if (QFile::exists(newPath))
        return false;
    return QFile::rename(oldPath, newPath);
}

std::string thumbnailDataUri(const std::string& path, int maxSize)
{
    QImage img(QString::fromUtf8(path.c_str()));
    if (img.isNull())
        return {};
    img = img.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return std::string("data:image/png;base64,") + bytes.toBase64().constData();
}

} // namespace BrushAssetLibrary
