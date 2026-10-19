#ifndef DNA_DNACLI_H
#define DNA_DNACLI_H

// `qtmesh dna info <file.dna> [--lod N] [--json] [--keep-units]` — epic #1095.
// Reads a MetaHuman DNA and reports what it carries (descriptor, joints,
// meshes, blend-shape channels, rig-logic inventory) without touching Ogre.
// Importing/converting goes through the normal `qtmesh convert head.dna -o x.glb`.

#include <QString>
#include <QVariantMap>

namespace Dna {
namespace DnaCLI {
/// argv[0] is the program, argv[1] is "dna". Returns a process exit code.
int run(int argc, char* argv[]);

/// The `qtmesh dna fit` core for in-process callers (MCP `fit_dna_body`): fits the body DNA at `dnaPath`
/// to `targetPath` (a same-topology edited mesh, another body DNA, or — with `mode` "joints" /
/// "surface" — a character with its own topology) and returns the report as JSON-ready fields. Nodes the
/// fit creates in the live scene are removed again. `mode`: "" / "topology" (default), "joints", "surface".
/// Returns false and fills `error` on failure.
struct FitOptions {
    QString mode;
    bool poses = true, lods = false, alignLimbs = true;
    QString outputDna, outputMesh;
};
bool fitReport(const QString& dnaPath, const QString& targetPath, const FitOptions& options,
               QVariantMap* report, QString* error);
}
}

#endif
