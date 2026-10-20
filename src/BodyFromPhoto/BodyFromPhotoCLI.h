#pragma once

namespace SamBody {
namespace BodyCLI {

/// `qtmesh body <photo> -o body.obj [--bbox x1,y1,x2,y2] [--pose neutral|posed] [--facing-z] [--json]`
/// Photo -> MHR body mesh (SAM 3D Body). Returns the process exit code.
int run(int argc, char* argv[]);

} // namespace BodyCLI
} // namespace SamBody
