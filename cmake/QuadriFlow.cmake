# QuadriFlow quad remesher (BSD-3) — built as the standalone helper
# executable `qtmesh_quadriflow`, never linked into the editor: upstream
# calls exit() from inside its solvers on failure, so the editor drives it
# through QProcess (crash isolation + cancel = kill). See
# src/dependencies/quadriflow/README.QtMeshEditor.md for the vendored
# revision and the local patches (Boost and minisat shell-outs removed).
#
# Dependencies: Eigen 3.4.0 (MPL2, header-only, fetched by SHA-256 — never
# configured, so none of its tests/targets enter the build) and LEMON 1.3.1
# (Boost Software License, vendored header-only).

include(FetchContent)

FetchContent_Declare(
    qtmesh_eigen
    URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
    URL_HASH SHA256=8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72
    # Header-only: point SOURCE_SUBDIR at a directory with no CMakeLists so
    # MakeAvailable downloads + verifies but never configures Eigen.
    SOURCE_SUBDIR _qtmesh_header_only
)
FetchContent_MakeAvailable(qtmesh_eigen)

set(_qf_root ${CMAKE_SOURCE_DIR}/src/dependencies/quadriflow)
set(_qf_src
    ${_qf_root}/src/adjacent-matrix.cpp
    ${_qf_root}/src/dedge.cpp
    ${_qf_root}/src/hierarchy.cpp
    ${_qf_root}/src/localsat.cpp
    ${_qf_root}/src/merge-vertex.cpp
    ${_qf_root}/src/optimizer.cpp
    ${_qf_root}/src/parametrizer.cpp
    ${_qf_root}/src/parametrizer-flip.cpp
    ${_qf_root}/src/parametrizer-int.cpp
    ${_qf_root}/src/parametrizer-mesh.cpp
    ${_qf_root}/src/parametrizer-scale.cpp
    ${_qf_root}/src/parametrizer-sing.cpp
    ${_qf_root}/src/subdivide.cpp
    ${_qf_root}/src/loader.cpp   # only for Parametrizer::Load; the driver reads OBJ itself
    ${CMAKE_SOURCE_DIR}/src/QuadriFlowHelper/main.cpp
)

add_executable(qtmesh_quadriflow ${_qf_src})
target_include_directories(qtmesh_quadriflow SYSTEM PRIVATE
    ${qtmesh_eigen_SOURCE_DIR}
    ${_qf_root}/3rd/lemon
    ${_qf_root}/3rd/pcg32
    ${_qf_root}/3rd/pss)
target_include_directories(qtmesh_quadriflow PRIVATE ${_qf_root}/src)
target_compile_definitions(qtmesh_quadriflow PRIVATE
    EIGEN_MPL2_ONLY
    LEMON_ONLY_TEMPLATES
    _USE_MATH_DEFINES)           # M_PI on MinGW
target_compile_features(qtmesh_quadriflow PRIVATE cxx_std_17)
if(NOT MSVC)
    # Third-party numerics: keep the build log readable, and always optimise —
    # unoptimised Eigen is ~50x slower, so a Debug remesh would look hung.
    target_compile_options(qtmesh_quadriflow PRIVATE -w -O2)
endif()
# Plain console tool: no macOS bundle, no Windows GUI subsystem.
set_target_properties(qtmesh_quadriflow PROPERTIES MACOSX_BUNDLE FALSE WIN32_EXECUTABLE FALSE
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

install(TARGETS qtmesh_quadriflow RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX})
# Licence notices travel with the binary in every package.
set(QTMESH_QUADRIFLOW_LICENSES
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/qtmesh_quadriflow.THIRD_PARTY_LICENSES.txt")
file(READ ${_qf_root}/LICENSE.txt _qf_lic)
file(READ ${_qf_root}/3rd/lemon/LICENSE _lemon_lic)
file(READ ${_qf_root}/3rd/pss/LICENSE _pss_lic)
file(READ ${_qf_root}/3rd/pcg32/LICENSE _pcg_lic)
file(READ ${qtmesh_eigen_SOURCE_DIR}/COPYING.MPL2 _eigen_lic)
file(WRITE ${QTMESH_QUADRIFLOW_LICENSES}
    "qtmesh_quadriflow bundles third-party code under the licences below.\n"
    "\n===== QuadriFlow (BSD-3-Clause, https://github.com/hjwdzh/QuadriFlow) =====\n${_qf_lic}"
    "\n===== LEMON 1.3.1 (Boost Software License 1.0) =====\n${_lemon_lic}"
    "\n===== Intel parallel stable sort (BSD-3-Clause) =====\n${_pss_lic}"
    "\n===== pcg32 (Apache-2.0) =====\n${_pcg_lic}"
    "\n===== Eigen 3.4.0 (MPL-2.0; source: https://gitlab.com/libeigen/eigen) =====\n${_eigen_lic}")
install(FILES ${QTMESH_QUADRIFLOW_LICENSES} DESTINATION ${CMAKE_INSTALL_PREFIX})

message(STATUS "QuadriFlow quad remesher enabled (qtmesh_quadriflow helper)")
