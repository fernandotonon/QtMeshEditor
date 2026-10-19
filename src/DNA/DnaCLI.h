#ifndef DNA_DNACLI_H
#define DNA_DNACLI_H

// `qtmesh dna info <file.dna> [--lod N] [--json] [--keep-units]` — epic #1095.
// Reads a MetaHuman DNA and reports what it carries (descriptor, joints,
// meshes, blend-shape channels, rig-logic inventory) without touching Ogre.
// Importing/converting goes through the normal `qtmesh convert head.dna -o x.glb`.

#include "../PoseSpaceRbf.h"
#include "DnaReader.h"

#include <QString>
#include <memory>
#include <vector>
#include <QVariantMap>

namespace Ogre { class Entity; }

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
    bool poseSpace = false;   ///< (live-entity fit) also bake + transfer generic pose-space correctives for the fitted body
    QString outputDna, outputMesh;
    QString outputCorrectives;   ///< write the fitted body's pose-space correctives (.qpsd binary, otherwise JSON)
};
bool fitReport(const QString& dnaPath, const QString& targetPath, const FitOptions& options,
               QVariantMap* report, QString* error);

/// The same fit against a LIVE entity (the editor's "Fit DNA body" action). `fittedRig` receives the
/// fitted rig (when the fit re-posed the limbs it is the re-posed one). Nothing is added to the scene.
bool fitEntityReport(const QString& dnaPath, Ogre::Entity* target, const FitOptions& options, Rig* fittedRig,
                     QVariantMap* report, QString* error, std::shared_ptr<std::vector<PoseSpace::Set>>* poseSpace = nullptr);
}
}

#endif
