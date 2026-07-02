#pragma once

#include <QString>
#include <QStringList>

/// Bundled + optional user-downloaded HDRI catalog (Slice F, #472).
namespace HdrBundledLibrary {

struct Entry {
    QString fileName;       // e.g. studio_neutral.hdr
    QString polyhavenId;    // empty for synthetic flat_grey
    QString attribution;    // Poly Haven asset id or "synthetic"
};

QStringList catalogFileNames();
const Entry* findByFileName(const QString& fileName);
const Entry* findByBaseName(const QString& baseName);

/// Bundled `media/hdri/` roots plus optional `<AppData>/hdri/`.
QStringList hdriSearchRoots();

QString userHdriDirectory();

/// Resolve a bundled file name or absolute path to an on-disk HDR/EXR file.
QString resolveHdriPath(const QString& pathOrFileName);

/// Blocking download of one catalog entry into the user HDRI folder (CLI).
bool downloadHdri(const QString& nameOrFileName, QString* errorOut = nullptr);

/// Optional user-downloaded HDRI folder as an Ogre FileSystem location (after resources.cfg).
void registerUserHdriResourceLocation();

/// First launch: studio_neutral + ACES + 0 EV (once per install).
void applyFirstRunDefaultsIfNeeded();

} // namespace HdrBundledLibrary
