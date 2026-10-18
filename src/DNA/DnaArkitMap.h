#ifndef DNA_DNAARKITMAP_H
#define DNA_DNAARKITMAP_H

// ARKit-52 -> MetaHuman face-rig control mapping (epic #1095, Phase 3).
//
// A MetaHuman head exposes ~250 expression-level raw controls named
// "CTRL_expressions.<name>" (browDownL, eyeBlinkL, jawOpen, mouthCornerPullL …).
// ARKit's 52 blendshapes are coarser and differently split, so the link is an
// EXPLICIT table, never an assumption of a one-to-one name match: an ARKit
// channel drives one or more MetaHuman controls, each with a weight. Channels
// whose MetaHuman equivalent is only a close composite are flagged
// `approximate`, so the supported subset is documented and queryable.
//
// Pure data (QtCore only); the default table is compiled in so it cannot go
// missing from an install, and a JSON file can override it per project.
//
// Override file (qtmesh-dna-arkit-map-v1):
//   { "schema": "qtmesh-dna-arkit-map-v1",
//     "map": { "jawOpen": { "jawOpen": 1.0, "jawOpenExtreme": 0.2 }, ... } }
// An ARKit channel present in "map" REPLACES its default entry; one set to {}
// is dropped. Control names omit the "CTRL_expressions." prefix.

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace Dna {
namespace ArkitMap {

struct Entry {
    QString arkit;                              ///< canonical ARKit name, e.g. "jawOpen"
    QList<QPair<QString, double>> controls;     ///< MetaHuman raw control (no prefix) + weight
    bool approximate = false;                   ///< a close composite, not an exact equivalent
    QString note;
};

/// The prefix every expression raw control carries in the DNA.
QString controlPrefix();

/// The built-in mapping for all 51 ARKit channels (every canonical name except
/// the "_neutral" rest channel).
QList<Entry> defaultMap();

struct LoadResult {
    bool ok = false;
    QString error;
    QList<Entry> entries;       ///< defaults with the file's overrides applied
    QStringList overridden;     ///< ARKit names the file replaced
    QStringList dropped;        ///< ARKit names the file emptied
};

/// Apply an override file to the defaults. Unknown ARKit names are an error
/// (a typo would otherwise silently do nothing).
LoadResult loadOverride(const QString& jsonPath);

/// Exact subset: entries not marked approximate.
QStringList exactChannels(const QList<Entry>& map);
QStringList approximateChannels(const QList<Entry>& map);

}  // namespace ArkitMap
}  // namespace Dna

#endif  // DNA_DNAARKITMAP_H
