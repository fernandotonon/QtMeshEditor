/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef VATSHADEREMITTER_H
#define VATSHADEREMITTER_H

#include <QString>
#include <QStringList>

/**
 * @brief Writes drop-in OpenVAT shader templates next to a bake.
 *
 * The actual `.gdshader` (Godot), `.shader` (Unity), and `.usf` (Unreal)
 * files plus the bundled README live at `tools/vat-shaders/` in source
 * and are baked into the binary as Qt resources under `:/vat-shaders/`.
 *
 * `writeShaders` copies the requested engine templates into `outputDir`
 * so a downstream consumer can grab everything in one place: the bake
 * (PNG + sidecar + glTF + bind file) AND the shader code that drives it.
 *
 * Pure-data API — no Ogre, no Qt object hierarchy; trivially unit-
 * testable by writing into a `QTemporaryDir`.
 */
class VATShaderEmitter
{
public:
    /// Engine identifiers accepted by `writeShaders`. Case-insensitive
    /// on the input side; `parseEngineList` lowercases and dedupes.
    static constexpr const char* kGodot  = "godot";
    static constexpr const char* kUnity  = "unity";
    static constexpr const char* kUnreal = "unreal";

    /// Parse a CLI-style comma-separated engine list (e.g.
    /// "godot,unity" or "all") into a deduplicated, lowercased,
    /// validated subset of {godot, unity, unreal}.
    ///
    /// "all" expands to all three. Unknown tokens are dropped (the
    /// caller may decide whether to warn). Order in the output is
    /// stable: godot, unity, unreal.
    static QStringList parseEngineList(const QString& csv);

    /// Write the engine templates for every entry in `engines` into
    /// `outputDir`. Returns the list of absolute paths actually written
    /// (may be shorter than `engines` if a target file failed to write
    /// — in that case `out` carries the partial success and the caller
    /// can surface a warning per missing file).
    ///
    /// Also writes a small `OpenVAT_README.md` next to the engine
    /// files when any engine was requested, so a consumer who pulls
    /// the bake folder into their project has the integration notes
    /// inline without chasing the GitHub repo.
    ///
    /// Idempotent — overwrites existing files at the destination
    /// without prompting. Engine-name strings are matched case-
    /// insensitively against {godot, unity, unreal}; anything else
    /// is silently skipped (use `parseEngineList` to surface that).
    static QStringList writeShaders(const QString& outputDir,
                                    const QStringList& engines);
};

#endif // VATSHADEREMITTER_H
