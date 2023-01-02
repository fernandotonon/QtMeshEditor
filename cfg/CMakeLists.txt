#Copy required file in the exe directory
set(OGRE_PLUGIN_DIR ${OGRE_PLUGIN_DIR_REL})

IF(WIN32)
    set(PLUGIN_DEBUG_POSTFIX "_d")
ELSE()
    set(PLUGIN_DEBUG_POSTFIX "")

ENDIF(WIN32)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        IF(WIN32)
            set(OGRE_PLUGIN_DIR ${OGRE_PLUGIN_DIR_DBG})
        ENDIF(WIN32)

	configure_file (
  	"${CMAKE_CURRENT_SOURCE_DIR}/plugins_d.cfg.in"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/cfg/plugins_d.cfg"
        @ONLY)

	configure_file (
  	"${CMAKE_CURRENT_SOURCE_DIR}/resources_d.cfg.in"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/cfg/resources_d.cfg"
        @ONLY)
else()
	configure_file (
  	"${CMAKE_CURRENT_SOURCE_DIR}/plugins.cfg.in"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/cfg/plugins.cfg"
        @ONLY)

	configure_file (
  	"${CMAKE_CURRENT_SOURCE_DIR}/resources.cfg.in"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/cfg/resources.cfg"
        @ONLY)


endif()
        configure_file (
        "${CMAKE_CURRENT_SOURCE_DIR}/version.txt.in"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/cfg/version.txt"
        @ONLY)

FILE(GLOB CFG_FILES ${CMAKE_CURRENT_SOURCE_DIR}/*)

#Copy all file/tree in the exe directory
#file(COPY ${CMAKE_CURRENT_SOURCE_DIR}
#    DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

add_custom_target(_cfg SOURCES ${CFG_FILES})


