cmake_minimum_required(VERSION 2.6)

set(library_name OgreProcedural)
#project(OgreProceduralMain)

#include(PrecompiledHeader)
#include(ProceduralConfigTargets)

set( HDRS
	include/ProceduralPlatform.h
	include/Procedural.h
	include/ProceduralMeshGenerator.h
	include/ProceduralBoxGenerator.h
	include/ProceduralCapsuleGenerator.h
	include/ProceduralConeGenerator.h
	include/ProceduralCylinderGenerator.h
	include/ProceduralIcoSphereGenerator.h
	include/ProceduralPlaneGenerator.h
	include/ProceduralSphereGenerator.h
	include/ProceduralRoot.h
	include/ProceduralRoundedBoxGenerator.h
	include/ProceduralTorusGenerator.h
	include/ProceduralTorusKnotGenerator.h
	include/ProceduralTubeGenerator.h
	include/ProceduralUtils.h
	include/ProceduralExtruder.h
	include/ProceduralLathe.h
	include/ProceduralShape.h	
	include/ProceduralShapeGenerators.h
	include/ProceduralPath.h
	include/ProceduralPathGenerators.h
	include/ProceduralTrack.h
	include/ProceduralSplines.h
	include/ProceduralTriangulator.h
	include/ProceduralTriangleBuffer.h
	include/ProceduralStableHeaders.h
	include/ProceduralMultiShape.h	
	include/ProceduralGeometryHelpers.h
	)

set( SRCS
		src/ProceduralBoxGenerator.cpp
		src/ProceduralCapsuleGenerator.cpp
		src/ProceduralConeGenerator.cpp
		src/ProceduralCylinderGenerator.cpp
		src/ProceduralIcoSphereGenerator.cpp
		src/ProceduralPlaneGenerator.cpp
		src/ProceduralRoot.cpp
		src/ProceduralRoundedBoxGenerator.cpp
		src/ProceduralSphereGenerator.cpp
		src/ProceduralTorusGenerator.cpp
		src/ProceduralTorusKnotGenerator.cpp
		src/ProceduralTubeGenerator.cpp		
		src/ProceduralUtils.cpp
		src/ProceduralShape.cpp		
		src/ProceduralShapeGenerators.cpp
		src/ProceduralPath.cpp
		src/ProceduralPathGenerators.cpp
		src/ProceduralTrack.cpp
		src/ProceduralExtruder.cpp
		src/ProceduralLathe.cpp
		src/ProceduralTriangulator.cpp
		src/ProceduralPrecompiledHeaders.cpp
		src/ProceduralMultiShape.cpp
		src/ProceduralGeometryHelpers.cpp
	)

include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/include ${OIS_INCLUDE_DIRS}
	${OGRE_INCLUDE_DIRS}
)

# should we build static libs?
if(OgreProcedural_STATIC)
  set(OgreProcedural_LIB_TYPE STATIC)
else()
  set(OgreProcedural_LIB_TYPE SHARED)
  add_definitions(-DOgreProcedural_SHARED)
endif()

option(OgreProcedural_HARDENEDBUILDFLAGS "Hardened build flags" FALSE)

if(OgreProcedural_HARDENEDBUILDFLAGS)
    if(MSVC) # Sort the odd stuff out
        add_definitions("/WX /MP /EHsc")
    else() # Clang/GCC/Mingw
        add_definitions("-Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-but-set-parameter -pedantic -std=c++0x")
    endif()
endif()

add_library(${library_name} ${OgreProcedural_LIB_TYPE} ${HDRS} ${SRCS})

set_target_properties(${library_name} PROPERTIES DEBUG_POSTFIX _d)

if(NOT OgreProcedural_STATIC)
        if(MINGW)
                set_target_properties(${library_name} PROPERTIES PREFIX "")
	endif()
endif()

target_link_libraries(${library_name} ${OGRE_LIBRARIES} ${OIS_LIBRARIES} ${BOOST_LIB})

# definitions for using lib
set(OGRE_PROCEDURAL_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include PARENT_SCOPE)
set(OgreProcedural_LIBRARY ${library_name} PARENT_SCOPE)

#procedural_install_target(OgreProcedural)
		
install(FILES ${HDRS} DESTINATION ${BUILD_INCLUDE_DIR}/${library_name})

# specify a precompiled header to use
#use_precompiled_header(OgreProcedural 
#  "${CMAKE_CURRENT_SOURCE_DIR}/include/ProceduralStableHeaders.h"
#  "${CMAKE_CURRENT_SOURCE_DIR}/src/ProceduralPrecompiledHeaders.cpp"
#)
