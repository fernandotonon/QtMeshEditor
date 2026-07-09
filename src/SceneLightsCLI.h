#pragma once

/// Headless `qtmesh light` subcommand (issue #490, Slice H).
namespace SceneLightsCLI
{
/// Entry point for `qtmesh light …`. Returns process exit code.
int run(int argc, char* argv[]);
} // namespace SceneLightsCLI
