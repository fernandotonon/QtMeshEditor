# Tests CMakeLists.txt for MaterialEditorQML Comprehensive Unit Tests

if(BUILD_TESTS)
    
    # Include MaterialEditorQML header and source directories
    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../ui_files
        ${BUILD_INCLUDE_DIR}
        ${BUILD_UIH_DIR}
    )
    
    # Get the source files from parent scope (similar to src/CMakeLists.txt)
    # We need to replicate the source file collection logic from src/CMakeLists.txt
    
    # Basic source files (excluding main.cpp for tests)
    set(TEST_SRC_FILES 
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/about.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animationcontrolwidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animationcontrolslider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Manager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/material.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialEditorQML.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/mainwindow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshTransform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreWidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/QtInputManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SpaceCamera.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SkeletonDebug.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SkeletonTransform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshImporterExporter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/EditorViewport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/RotationGizmo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TranslationGizmo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TransformOperator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TransformWidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PrimitivesWidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PrimitiveObject.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ViewportGrid.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AnimationWidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SelectionSet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SelectionBoxObject.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ObjectItemModel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialWidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialComboDelegate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialHighlighter.cpp
    )
    
    set(TEST_HEADER_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animationcontrolwidget.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/animationcontrolslider.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/GlobalDefinitions.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Euler.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/about.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/mainwindow.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Manager.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/material.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialEditorQML.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshTransform.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreWidget.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/QtInputManager.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/QtKeyListener.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/QtMouseListener.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SpaceCamera.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SkeletonDebug.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SkeletonTransform.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshImporterExporter.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/EditorViewport.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/RotationGizmo.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TranslationGizmo.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TransformOperator.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TransformWidget.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PrimitivesWidget.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PrimitiveObject.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ViewportGrid.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AnimationWidget.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SelectionSet.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SelectionBoxObject.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ObjectItemModel.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialWidget.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialComboDelegate.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialHighlighter.h
    )

    # Add Ogre-Procedural sources (matching src/CMakeLists.txt)
    set(OGRE_PROCEDURAL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/dependencies/ogre-procedural/library/")
    include_directories(${OGRE_PROCEDURAL_LIB_DIR}include)
    
    set(TEST_SRC_FILES ${TEST_SRC_FILES}
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralBoxGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralCapsuleGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralConeGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralCylinderGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralIcoSphereGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralPlaneGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralRoundedBoxGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralSphereGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTorusGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTorusKnotGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTubeGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralUtils.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralShape.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralShapeGenerators.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralPath.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralPathGenerators.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTrack.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralExtruder.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralLathe.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTriangulator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralPrecompiledHeaders.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralMultiShape.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralMultiShapeGenerators.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralGeometryHelpers.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralBoolean.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralSpringGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralSVG.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralDebugRendering.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTextureGenerator.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTextureModifiers.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralNoise.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralMeshModifiers.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTextureBuffer.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralTriangleBuffer.cpp
        ${OGRE_PROCEDURAL_LIB_DIR}src/ProceduralPrismGenerator.cpp
    )

    # Add OgreXML sources (matching src/OgreXML/CMakeLists.txt)
    set(TEST_SRC_FILES ${TEST_SRC_FILES}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/pugixml.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/tinystr.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/tinyxml.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/tinyxmlerror.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/tinyxmlparser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/OgreXMLMeshSerializer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/OgreXMLSkeletonSerializer.cpp
    )

    set(TEST_HEADER_FILES ${TEST_HEADER_FILES}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/pugiconfig.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/pugixml.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/tinystr.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/tinyxml.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/OgreXMLMeshSerializer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/OgreXMLPrerequisites.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML/OgreXMLSkeletonSerializer.h
    )

    # Add Assimp sources (matching src/Assimp/CMakeLists.txt)
    set(TEST_SRC_FILES ${TEST_SRC_FILES}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Assimp/Importer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Assimp/MaterialProcessor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Assimp/AnimationProcessor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Assimp/BoneProcessor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Assimp/MeshProcessor.cpp
    )

    set(TEST_HEADER_FILES ${TEST_HEADER_FILES}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Assimp/Importer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Assimp/MaterialProcessor.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Assimp/AnimationProcessor.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Assimp/BoneProcessor.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Assimp/MeshProcessor.h
    )

    # Add Qt resources (matching src/CMakeLists.txt)
    qt_add_resources(TEST_RESOURCE_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../resources/resource.qrc")
    qt_add_resources(TEST_QML_RESOURCE_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../src/qml_resources.qrc")

    # Setup Ogre library paths
    if(APPLE)
        SET(OGRE_Codec_Assimp_LIBRARY_REL ${OGRE_PLUGIN_DIR}/Codec_Assimp.dylib)
    elseif(UNIX)
        SET(OGRE_Codec_Assimp_LIBRARY_REL ${OGRE_PLUGIN_DIR}/Codec_Assimp.so)
    elseif(WIN32)
        SET(OGRE_Codec_Assimp_LIBRARY_REL ${OGRE_PLUGIN_DIR}/libCodec_Assimp.dll.a)
    endif()

    # Common libraries for all test executables
    set(COMMON_TEST_LIBRARIES
        gtest
        gtest_main
        gmock
        gmock_main
        ${OGRE_Codec_Assimp_LIBRARY_REL}
        ${OGRE_LIBRARIES}
        ${ASSIMP_LIBRARIES}
        Qt::Test
        Qt::Qml
        Qt::Quick
        Qt::Gui
        Qt::Core
        Qt::Widgets
        Qt::Network
        Qt::QuickWidgets
    )
    
    # Helper function to create test executables
    function(create_test_executable target_name test_source_file)
        add_executable(${target_name} 
            ${test_source_file}
            ${TEST_HEADER_FILES}
            ${TEST_SRC_FILES}
            ${TEST_RESOURCE_SRCS}
            ${TEST_QML_RESOURCE_SRCS}
        )
        
        target_include_directories(${target_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_CURRENT_SOURCE_DIR}/../ui_files
            ${BUILD_INCLUDE_DIR}
            ${BUILD_UIH_DIR}
            ${OGRE_PROCEDURAL_LIB_DIR}include
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/Assimp
        )
        
        target_link_libraries(${target_name} ${COMMON_TEST_LIBRARIES})
        
        # Add dependency on UI generation
        ADD_DEPENDENCIES(${target_name} ui)
        
        # Add the test to CTest
        add_test(NAME ${target_name} COMMAND ${target_name})
    endfunction()
    
    # 1. MaterialEditorQML C++ Unit Tests (comprehensive suite)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialEditorQML_test.cpp")
        create_test_executable(MaterialEditorQML_test 
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialEditorQML_test.cpp"
        )
    endif()
    
    # 2. MaterialEditorQML QML Integration Tests 
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialEditorQML_qml_test.cpp")
        create_test_executable(MaterialEditorQML_qml_test 
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialEditorQML_qml_test.cpp"
        )
    endif()
    
    # 3. MaterialEditorQML Performance Tests
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialEditorQML_perf_test.cpp")
        create_test_executable(MaterialEditorQML_perf_test 
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialEditorQML_perf_test.cpp"
        )
    endif()
    
    # 4. QML Component Test Runner (existing)
    create_test_executable(MaterialEditorQML_qml_test_runner
        MaterialEditorQML_qml_test_runner.cpp
    )
    
    # Copy QML test files to build directory for runtime access
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/MaterialEditorQML_component_test.qml
        ${CMAKE_CURRENT_BINARY_DIR}/MaterialEditorQML_component_test.qml
        COPYONLY
    )
    
    # Copy QML test files to runtime directory for CI execution
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/MaterialEditorQML_component_test.qml
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/MaterialEditorQML_component_test.qml
        COPYONLY
    )
    
    # Enable CTest integration for all MaterialEditorQML tests
    include(GoogleTest)
    
    # Discover individual Google Test cases for better reporting
    if(TARGET MaterialEditorQML_test)
        gtest_discover_tests(MaterialEditorQML_test)
    endif()
    
    if(TARGET MaterialEditorQML_qml_test)
        gtest_discover_tests(MaterialEditorQML_qml_test)
    endif()
    
    if(TARGET MaterialEditorQML_perf_test)
        gtest_discover_tests(MaterialEditorQML_perf_test)
    endif()
    
    if(TARGET MaterialEditorQML_qml_test_runner)
        gtest_discover_tests(MaterialEditorQML_qml_test_runner)
    endif()
    
    # Create a comprehensive test target that runs all MaterialEditorQML tests
    add_custom_target(run_all_materialeditorthml_tests
        COMMAND ctest --verbose
        DEPENDS MaterialEditorQML_qml_test_runner
        $<$<TARGET_EXISTS:MaterialEditorQML_test>:MaterialEditorQML_test>
        $<$<TARGET_EXISTS:MaterialEditorQML_qml_test>:MaterialEditorQML_qml_test>
        $<$<TARGET_EXISTS:MaterialEditorQML_perf_test>:MaterialEditorQML_perf_test>
        COMMENT "Running all MaterialEditorQML comprehensive tests"
    )
    
endif() 