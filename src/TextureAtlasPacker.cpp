#include "TextureAtlasPacker.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>

#include <algorithm>

namespace TextureAtlasPacker {

namespace {

// Shelf bin-pack state. Each shelf is a row that starts at `y` and is
// `height` tall; new tiles in the shelf are placed left-to-right at
// the current `x` cursor. When a tile doesn't fit width-wise on the
// current shelf, we open a new shelf below.
struct Shelf {
    int y = 0;
    int x = 0;
    int height = 0;
};

// Input loaded from disk + the slot we'll write back into the result.
struct LoadedTile {
    int originalIndex = 0;     // position in the caller's sourcePaths list
    QString path;
    QImage image;
    int paddedWidth = 0;       // width + 2*padding
    int paddedHeight = 0;
};

} // namespace

AtlasResult pack(const AtlasSpec& spec)
{
    AtlasResult result;

    if (spec.sourcePaths.isEmpty()) {
        result.error = QStringLiteral("No source images supplied");
        return result;
    }
    if (spec.atlasWidth <= 0 || spec.atlasHeight <= 0) {
        result.error = QStringLiteral("Atlas dimensions must be positive");
        return result;
    }
    if (spec.padding < 0) {
        result.error = QStringLiteral("Padding must be non-negative");
        return result;
    }

    const int pad = spec.padding;

    // Load every input. Bail on the first failure so the user sees the
    // bad file rather than a "couldn't fit" symptom downstream.
    QList<LoadedTile> tiles;
    tiles.reserve(spec.sourcePaths.size());
    for (int i = 0; i < spec.sourcePaths.size(); ++i) {
        const QString& path = spec.sourcePaths.at(i);
        QImage img;
        if (!img.load(path)) {
            result.error = QStringLiteral("Failed to load input image: %1").arg(path);
            return result;
        }
        if (img.format() != QImage::Format_RGBA8888 && img.format() != QImage::Format_ARGB32) {
            img = img.convertToFormat(QImage::Format_ARGB32);
        }

        LoadedTile t;
        t.originalIndex = i;
        t.path = path;
        t.image = img;
        t.paddedWidth = img.width() + 2 * pad;
        t.paddedHeight = img.height() + 2 * pad;

        if (t.paddedWidth > spec.atlasWidth || t.paddedHeight > spec.atlasHeight) {
            result.error = QStringLiteral(
                "Input '%1' (%2x%3 + %4px padding) is larger than the atlas (%5x%6)")
                .arg(QFileInfo(path).fileName())
                .arg(img.width()).arg(img.height())
                .arg(pad)
                .arg(spec.atlasWidth).arg(spec.atlasHeight);
            return result;
        }
        tiles.append(std::move(t));
    }

    // Shelf bin-pack: order tiles by height descending so each shelf is
    // sized by its tallest occupant. Stable: ties by width descending,
    // then by original index so the same input list always packs the
    // same way regardless of filesystem enumeration order.
    QList<int> order;
    order.reserve(tiles.size());
    for (int i = 0; i < tiles.size(); ++i) order.append(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const LoadedTile& ta = tiles[a];
        const LoadedTile& tb = tiles[b];
        if (ta.paddedHeight != tb.paddedHeight) return ta.paddedHeight > tb.paddedHeight;
        if (ta.paddedWidth  != tb.paddedWidth)  return ta.paddedWidth  > tb.paddedWidth;
        return ta.originalIndex < tb.originalIndex;
    });

    // Place each tile on the first shelf where it fits; open a new shelf
    // if none accommodates it.
    QList<Shelf> shelves;
    shelves.append({0, 0, 0});   // first shelf starts at y=0

    QList<AtlasTile> placement;
    placement.resize(tiles.size());   // indexed by originalIndex
    int usedW = 0;
    int usedH = 0;

    for (int idx : order) {
        const LoadedTile& t = tiles[idx];
        int chosen = -1;
        for (int s = 0; s < shelves.size(); ++s) {
            // Existing shelf must have enough horizontal room AND its
            // height must already accommodate the tile (we don't grow
            // shelves after a tile lands on them — keeps the layout
            // predictable and the height-sort tractable).
            if (shelves[s].x + t.paddedWidth <= spec.atlasWidth
                && t.paddedHeight <= shelves[s].height) {
                chosen = s;
                break;
            }
        }
        if (chosen < 0) {
            // Open a new shelf below the last one. Use the freshly-placed
            // tile's padded height as the new shelf's height.
            const int prevTop = shelves.last().y + shelves.last().height;
            if (prevTop + t.paddedHeight > spec.atlasHeight) {
                AtlasResult r;
                r.error = QStringLiteral(
                    "Atlas too small: %1 inputs do not fit in %2x%3 with padding %4")
                    .arg(spec.sourcePaths.size())
                    .arg(spec.atlasWidth).arg(spec.atlasHeight).arg(pad);
                return r;
            }
            shelves.append({prevTop, 0, t.paddedHeight});
            chosen = shelves.size() - 1;
        }

        AtlasTile out;
        out.sourcePath = t.path;
        out.x = shelves[chosen].x + pad;
        out.y = shelves[chosen].y + pad;
        out.width = t.image.width();
        out.height = t.image.height();
        out.u0 = static_cast<float>(out.x) / static_cast<float>(spec.atlasWidth);
        out.v0 = static_cast<float>(out.y) / static_cast<float>(spec.atlasHeight);
        out.u1 = static_cast<float>(out.x + out.width)  / static_cast<float>(spec.atlasWidth);
        out.v1 = static_cast<float>(out.y + out.height) / static_cast<float>(spec.atlasHeight);
        placement[t.originalIndex] = out;

        // Advance shelf cursor and track used extents.
        shelves[chosen].x += t.paddedWidth;
        usedW = std::max(usedW, shelves[chosen].x);
        usedH = std::max(usedH, shelves[chosen].y + shelves[chosen].height);
    }

    // Composite step: blit each input image into the atlas at the
    // placement (x, y). QImage::Format_ARGB32_Premultiplied gives QPainter
    // a fast blit path. We keep alpha so masked textures atlas correctly.
    QImage atlas(spec.atlasWidth, spec.atlasHeight, QImage::Format_ARGB32_Premultiplied);
    atlas.fill(Qt::transparent);
    {
        QPainter p(&atlas);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        for (const LoadedTile& t : tiles) {
            const AtlasTile& slot = placement[t.originalIndex];
            p.drawImage(slot.x, slot.y, t.image);
        }
    }

    result.ok = true;
    result.image = atlas.convertToFormat(QImage::Format_RGBA8888);
    result.tiles = placement;
    result.usedWidth = usedW;
    result.usedHeight = usedH;
    return result;
}

AtlasResult packToFile(const AtlasSpec& spec, const QString& outPath)
{
    AtlasResult r = pack(spec);
    if (!r.ok) return r;

    const QString ext = QFileInfo(outPath).suffix().toUpper();
    const QByteArray fmt = ext.isEmpty() ? QByteArray("PNG") : ext.toUtf8();
    if (!r.image.save(outPath, fmt.constData())) {
        r.ok = false;
        r.image = QImage();
        r.error = QStringLiteral("Failed to save atlas to %1").arg(outPath);
    }
    return r;
}

QString manifestToJson(const AtlasResult& result, int padding)
{
    QJsonObject root;
    root["width"]   = result.image.width();
    root["height"]  = result.image.height();
    root["padding"] = padding;

    QJsonArray tiles;
    for (const AtlasTile& t : result.tiles) {
        QJsonObject o;
        o["source"] = t.sourcePath;
        o["x"]      = t.x;
        o["y"]      = t.y;
        o["w"]      = t.width;
        o["h"]      = t.height;
        o["u0"]     = static_cast<double>(t.u0);
        o["v0"]     = static_cast<double>(t.v0);
        o["u1"]     = static_cast<double>(t.u1);
        o["v1"]     = static_cast<double>(t.v1);
        tiles.append(o);
    }
    root["tiles"] = tiles;

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

} // namespace TextureAtlasPacker
