#ifndef DNA_DNACLI_H
#define DNA_DNACLI_H

// `qtmesh dna info <file.dna> [--lod N] [--json] [--keep-units]` — epic #1095.
// Reads a MetaHuman DNA and reports what it carries (descriptor, joints,
// meshes, blend-shape channels, rig-logic inventory) without touching Ogre.
// Importing/converting goes through the normal `qtmesh convert head.dna -o x.glb`.

namespace Dna {
namespace DnaCLI {
/// argv[0] is the program, argv[1] is "dna". Returns a process exit code.
int run(int argc, char* argv[]);
}
}

#endif
