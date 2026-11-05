#ifndef CHARACTERGEN_HAIRLIBRARY_H
#define CHARACTERGEN_HAIRLIBRARY_H

// Hair Studio, library half: a persistent, de-duplicated collection of imported hairstyles (with author / source / licence /
// attribution) and of the user's saved procedural presets.
//
//   <dir>/imported/<id>/{meta.json, hair.qmh, texture.png}   one folder per hairstyle (the geometry is stored as extracted from the
//                                                            source file; the original source path is kept in the metadata)
//   <dir>/presets/<name>.json                                 saved procedural parameter sets
//
// <dir> = QTMESH_HAIR_LIBRARY_DIR, else <AppData>/hair_library. Nothing is downloaded or bundled: assets arrive only when the user
// imports a file or a folder, and the licence field travels with every entry. The importer (HairImport.cpp) uses Assimp.

#include "HairFitting.h"

#include <QList>

namespace CharacterGen {
namespace Hair {
namespace Library {

struct Meta {
    QString id, name;
    QString kind = QStringLiteral("imported");        ///< imported | procedural | builtin
    QString author, source, license, attribution, collection;
    QString length;                                   ///< short | medium | long
    QString texture;                                  ///< straight | wavy | curly | coily
    QString gender;                                   ///< feminine | masculine | unisex
    QString style;                                    ///< free text: bob, ponytail, bun, afro, fade, pixie ...
    QString geometryHash;
    int triangles = 0;
    QJsonObject fit;                                  ///< default FitAdjust for this style
    QJsonObject toJson() const;
    static Meta fromJson(const QJsonObject& o);
};

/// Everything the Hair Studio needs to rebuild the character's hair: saved beside a generated character (<file>.hair.json) and in the
/// editor's settings, so the selection, the procedural source parameters and the fit survive closing and reopening.
struct StudioConfig {
    QString style;                  ///< "", "proc:<id>", "lib:<id>" or a built-in bundle style name
    Params procedural;
    FitAdjust fit;
    QString colour;                 ///< "#rrggbb" ("" = keep the default)
};
QJsonObject toJson(const StudioConfig& c);
bool fromJson(const QJsonObject& o, StudioConfig* out);
bool saveConfigFile(const QString& path, const StudioConfig& c);
bool loadConfigFile(const QString& path, StudioConfig* out);

QString dir();
QList<Meta> list();                                                   ///< imported entries, sorted by name
/// Load an entry. False when it is missing or its files are damaged.
bool load(const QString& id, ImportedHair* hair, Meta* meta = nullptr);
/// Store an entry. A hairstyle whose geometry is already registered is not added again: `*duplicateOf` gets the existing id and the
/// call returns true with an empty `*newId`.
bool save(const ImportedHair& hair, Meta meta, QString* newId, QString* duplicateOf = nullptr, QString* error = nullptr);
bool remove(const QString& id);
bool updateMeta(const Meta& meta);

/// Order-independent hash of the geometry (quantised to 0.1 mm of the style's own units): the duplicate test.
QString geometryHash(const ImportedHair& hair);
/// Guess length / curliness / gender / style tags from the geometry and the name (the user can edit them).
void inferTags(const ImportedHair& hair, Meta* meta);
/// A stable filesystem-safe id from a display name (+ hash prefix when needed).
QString makeId(const QString& name, const QString& hash);

// User presets of the procedural generator -------------------------------------------------------------------------------
QStringList presetNames();
bool savePreset(const QString& name, const Params& p, QString* error = nullptr);
bool loadPreset(const QString& name, Params* p, QString* error = nullptr);
bool removePreset(const QString& name);

// Import ------------------------------------------------------------------------------------------------------------------
struct ImportOptions {
    QString author, license, attribution, source, collection;
    bool recursive = true;
};
struct ImportResult {
    int found = 0, added = 0, duplicates = 0, failed = 0;
    QStringList ids, errors;
};
/// Split a model file (FBX, OBJ, GLB, DAE...) into hairstyles — one per child node of the scene root with geometry (or one per mesh
/// when the file has a single group) — and register them. Defined in HairImport.cpp.
ImportResult importFile(const QString& path, const ImportOptions& opt);
/// Import every supported model file below a folder.
ImportResult importFolder(const QString& folder, const ImportOptions& opt);
/// Same as importFile but returns the extracted styles without storing them (tests, preview).
QList<ImportedHair> extractFromFile(const QString& path, QStringList* errors = nullptr);

}  // namespace Library
}  // namespace Hair
}  // namespace CharacterGen

#endif
