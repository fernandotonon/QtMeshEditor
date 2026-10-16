# QuadriFlow (vendored)

Field-aligned quad remeshing — Huang, Zhou, Niessner, Chen, Guibas, Fu,
*QuadriFlow: A Scalable and Robust Method for Quadrangulation*, SGP 2018.

| | |
|---|---|
| Upstream | https://github.com/hjwdzh/QuadriFlow |
| Revision | `810b7a0967c35b0dc85b4464e3835e26a756c967` (2019-12-07) |
| Licence  | BSD-3-Clause (`LICENSE.txt`) |
| Built as | the `qtmesh_quadriflow` helper executable (`cmake/QuadriFlow.cmake`), driven by `src/QuadRemesh/QuadriFlowRunner.cpp` |

QuadriFlow is **never linked into the editor**. Upstream calls `exit()` from
inside its solvers when they fail and can crash on unusual input, so it runs
out of process: a failure is an error message, and Cancel kills the process.
The driver (`src/QuadriFlowHelper/main.cpp`, our code) reports success with a
`QF_DONE` line — exit status 0 alone means nothing, upstream exits 0 on
several failures.

## What is vendored

* `src/` — upstream sources **except** `main.cpp` (replaced by our driver),
  `post-solver.{cpp,hpp}` (Boost + Ceres; unused upstream — its only call is
  commented out) and `Optimizer.cu` (CUDA).
* `3rd/pcg32/pcg32/pcg32.h` — Apache-2.0 (notice: `3rd/pcg32/LICENSE`).
* `3rd/pss/pss/*.h` — Intel parallel stable sort, BSD-3-Clause (notice: `3rd/pss/LICENSE`).
* `3rd/lemon/` — LEMON 1.3.1 headers, Boost Software License 1.0
  (`3rd/lemon/LICENSE`). Header-only use (`LEMON_ONLY_TEMPLATES`): the `.cc`
  sources, the LP/MIP solver wrappers (cbc/clp/cplex/glpk/soplex/lp*), the
  CMake files and `lemon.pc.in` were removed; `lemon/config.h` is hand-written
  (upstream generates it at configure time).
* Eigen 3.4.0 (MPL-2.0, `EIGEN_MPL2_ONLY`) is **not** vendored — CMake
  downloads the release tarball and checks its SHA-256.

## Local changes (all marked `QtMeshEditor:` in the source)

1. **Boost removed** (`src/flow.hpp`, `src/optimizer.cpp`). Upstream used
   `BoykovMaxFlowHelper` (Boost.Graph) when the flow supply is ≥ 20 and its
   own shortest-augmenting-path `ECMaxFlowHelper` otherwise. The Boykov class
   is gone and `ECMaxFlowHelper` is used for every supply.
   *Why not another max-flow library:* which maximum flow is found matters,
   not only its value — QuadriFlow applies each arc's flow as an integer
   offset to the position field. LEMON's push-relabel `Preflow` reached the
   same flow value but changed the offsets ~4.7× as much (L1 28496 vs 6020)
   and produced 542 quads for a 2000-quad target where the EC solver gives
   2043; cancelling its flow cycles did not help. A Dinic solver matched EC's
   output exactly but was slower (30k-face remesh: 39 s vs 31 s).
2. **No shell-outs.** `localsat.cpp::RunCNF` wrote a CNF to the working
   directory and ran an external `minisat` through `system()`;
   `Hierarchy::FixFlipSat` probed `which minisat` / `which timeout`. Both are
   only reachable with `flag_aggresive_sat`, which QtMeshEditor never sets;
   they now report "not available" — what upstream does when minisat is not
   installed.
3. **`post-solver.hpp` include removed** from `src/parametrizer.hpp` (the file
   is not vendored; nothing called into it).
4. **`std::unary_function` base removed** from `obj_vertexHash` in
   `src/loader.cpp` (removed from the standard library in C++17).

5. **`Parametrizer::FixValence` loops bounded** (`src/parametrizer-mesh.cpp`).
   On a non-manifold quad structure a half-edge fan walk can enter a cycle
   that never returns to its start edge, and upstream spun forever (a real
   rigged character, voxel-rebuilt, with `-sharp -adaptive`: the helper hit
   the editor's timeout in the `extract` stage). Every walk now stops on a
   revisit or after `4·|F|+8` steps, and the two fixed-point passes are
   capped at 64 iterations; stopping early only leaves a valence fix undone.

6. **MinGW build** (`src/dedge.cpp`): the `_InterlockedCompareExchange` branch
   is gated on `_MSC_VER` instead of `_WIN32` — it is an MSVC intrinsic, and
   MinGW (GCC) uses the `__sync` builtin like the other platforms.

Nothing else differs from upstream; `exit()` calls are left as they are.
