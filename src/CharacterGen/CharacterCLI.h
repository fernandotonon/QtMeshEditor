#ifndef CHARACTERGEN_CHARACTERCLI_H
#define CHARACTERGEN_CHARACTERCLI_H

#include <QString>

namespace CharacterGen {
namespace CharacterCLI {

/// `qtmesh character -o out.glb [--seed N] [--variety S] [--body k=v,..] [--face k=v,..] [--skin C] [--eye C] [--assets DIR] [--json]`
/// argv[0] is the program, argv[1] is "character". Returns a process exit code.
int run(int argc, char* argv[]);

/// Directory holding mhr_character.qmcb + ict_character.qmcb: QTMESH_CHARACTER_DIR, else <AppData>/ai_models/character.
QString assetsDir();

}  // namespace CharacterCLI
}  // namespace CharacterGen

#endif
