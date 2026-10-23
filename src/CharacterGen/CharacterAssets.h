#ifndef CHARACTERGEN_CHARACTERASSETS_H
#define CHARACTERGEN_CHARACTERASSETS_H

// Character generator: where the two asset bundles live and how they are fetched.
//
//   mhr_character.qmcb   the MHR body (Apache-2.0 data)        ~11 MB
//   ict_character.qmcb   the ICT-FaceKit head (MIT)             ~25 MB
//   LICENSE-MHR.txt, LICENSE-ICT-FaceKit.txt                    the licence texts travel with the data
//
// Built offline by scripts/build-character-assets.py and hosted next to the other models. Directory: QTMESH_CHARACTER_DIR,
// else <AppData>/ai_models/character. Downloads go through ModelFetch with the SHA-256 of each file pinned (default hosting
// only: a mirror may serve a different export); QTMESH_CHARACTER_MODEL_BASE_URL / QSettings ai/characterModelBaseUrl override
// the base URL, QTMESH_CHARACTER_NO_DOWNLOAD verifies what is on disk without fetching.

#include <QString>
#include <QStringList>

namespace CharacterGen {
namespace CharacterAssets {

QString dir();
/// Both bundles exist (not verified: see ensureBlocking).
bool present();

/// Names of the hairstyles in the optional hair bundle of dir() (empty when there is none). Cached per file modification time.
QStringList hairStyleNames();
/// Make sure every file is on disk and matches its pinned digest, fetching what is missing. Blocks (nested event loop) — call it
/// from the main thread. Empty `error` on success.
bool ensureBlocking(QString* error = nullptr);

}  // namespace CharacterAssets
}  // namespace CharacterGen

#endif
