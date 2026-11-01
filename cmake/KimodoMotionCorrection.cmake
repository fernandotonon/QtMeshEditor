# NVIDIA Kimodo MotionCorrection (Apache-2.0), vendored in src/dependencies/kimodo_motion_correction: the
# foot-skate / constraint post-processing of the native Kimodo runtime (src/AIMotion/KimodoNative.cpp).
# Built as one static library together with its QtMeshEditor entry point (KimodoCorrection.cpp), because the
# entry point includes the same SIMD math headers and must be compiled with the same flags.
#
# Dependencies: Eigen 3.4.0 (MPL-2.0, header-only; SparseLU only, so EIGEN_MPL2_ONLY holds), fetched by
# cmake/QuadriFlow.cmake. sse2neon v1.8.0 (MIT) maps the SSE intrinsics to NEON on ARM.

if(NOT DEFINED qtmesh_eigen_SOURCE_DIR)
    include(FetchContent)
    FetchContent_Declare(
        qtmesh_eigen
        URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
        URL_HASH SHA256=8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72
        SOURCE_SUBDIR _qtmesh_header_only
    )
    FetchContent_MakeAvailable(qtmesh_eigen)
endif()

set(_kmc_root ${CMAKE_SOURCE_DIR}/src/dependencies/kimodo_motion_correction)
add_library(qtmesh_kimodo_mc STATIC
    ${_kmc_root}/AnimProcessing/InverseKinematics.cpp
    ${_kmc_root}/AnimProcessing/TrajectoryCorrector.cpp
    ${_kmc_root}/AnimProcessing/Utility.cpp
    ${_kmc_root}/Math/Matrix.cpp
    ${_kmc_root}/Math/Quaternion.cpp
    ${_kmc_root}/Math/Transform.cpp
    ${_kmc_root}/Math/Types.cpp
    ${_kmc_root}/Math/Vector.cpp
    ${CMAKE_SOURCE_DIR}/src/AIMotion/KimodoCorrection.cpp)
target_include_directories(qtmesh_kimodo_mc PUBLIC ${_kmc_root})
target_include_directories(qtmesh_kimodo_mc SYSTEM PRIVATE ${qtmesh_eigen_SOURCE_DIR})
target_compile_definitions(qtmesh_kimodo_mc PRIVATE EIGEN_MPL2_ONLY _USE_MATH_DEFINES)
target_compile_features(qtmesh_kimodo_mc PRIVATE cxx_std_17)
set_target_properties(qtmesh_kimodo_mc PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    # Third-party numerics: quiet, and always optimised (Eigen is ~50x slower unoptimised).
    target_compile_options(qtmesh_kimodo_mc PRIVATE -w -O2)
    # SSE4.1 on x86 (no AVX: the one AVX intrinsic falls back to a portable gather). ARM uses sse2neon.
    if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
        # Universal builds compile every architecture in one invocation: SSE4.1 for the x86_64 slice only.
        target_compile_options(qtmesh_kimodo_mc PRIVATE -Xarch_x86_64 -msse4.1)
    elseif(NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
        target_compile_options(qtmesh_kimodo_mc PRIVATE -msse4.1)
    endif()
endif()

# Licence notices travel with the binary in every package (the library is linked into the editor).
set(QTMESH_KIMODO_MC_LICENSES
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/kimodo_motion_correction.THIRD_PARTY_LICENSES.txt")
file(READ ${_kmc_root}/LICENSE-Apache-2.0.txt _kmc_lic)
file(READ ${qtmesh_eigen_SOURCE_DIR}/COPYING.MPL2 _kmc_eigen_lic)
# sse2neon's MIT notice is the header's first comment block: everything up to the first "*/".
file(READ ${_kmc_root}/Math/sse2neon.h _kmc_sse2neon_head LIMIT 4096)
string(FIND "${_kmc_sse2neon_head}" "/*" _kmc_lic_begin)
string(FIND "${_kmc_sse2neon_head}" "*/" _kmc_lic_end)
math(EXPR _kmc_lic_len "${_kmc_lic_end} + 2 - ${_kmc_lic_begin}")
string(SUBSTRING "${_kmc_sse2neon_head}" ${_kmc_lic_begin} ${_kmc_lic_len} _kmc_sse2neon_lic)
file(WRITE ${QTMESH_KIMODO_MC_LICENSES}
    "QtMeshEditor links NVIDIA Kimodo's MotionCorrection (native Kimodo post-processing) under the licences below.\n"
    "\n===== NVIDIA Kimodo MotionCorrection (Apache-2.0; https://github.com/nv-tlabs/kimodo) =====\n${_kmc_lic}"
    "\n===== sse2neon v1.8.0 (MIT; https://github.com/DLTcollab/sse2neon) =====\n${_kmc_sse2neon_lic}\n"
    "\n===== Eigen 3.4.0 (MPL-2.0; source: https://gitlab.com/libeigen/eigen) =====\n${_kmc_eigen_lic}")
install(FILES ${QTMESH_KIMODO_MC_LICENSES} DESTINATION ${CMAKE_INSTALL_PREFIX})

message(STATUS "Kimodo MotionCorrection enabled (native Kimodo post-processing)")
