# Tests CMakeLists.txt for MaterialEditorQML Comprehensive Unit Tests

if(BUILD_TESTS)
    
    # Include MaterialEditorQML header and source directories
    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${BUILD_INCLUDE_DIR}
        ${BUILD_UIH_DIR}
    )
    
    # Get the source files from parent scope (similar to src/CMakeLists.txt)
    # We need to replicate the source file collection logic from src/CMakeLists.txt
    
    # Basic source files (excluding main.cpp for tests)
    set(TEST_SRC_FILES 
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/about.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AppConsoleLog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AppLaunchHandler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ConsoleLogSanitize.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AnimationBlender.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AnimationControlController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CurveEditModel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CurveResampler.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SkeletonEditor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshImporterExporter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshDracoEncoder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/EditorViewport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ViewportTitleBar.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/RotationGizmo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TranslationGizmo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ScaleGizmo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/BevelGizmo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TransformOperator.cpp

        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PrimitivesWidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PrimitiveObject.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ViewportGrid.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AnimationWidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SelectionSet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightRigLibrary.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightLinking.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightGroupLibrary.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightGroupController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ViewportLightSoloController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/IesProfile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AreaLight.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightVisualizer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightsController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightPropertiesController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SceneLightingController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ShadowController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SceneLightsIO.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SceneLightsCLI.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SelectionBoxObject.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ObjectItemModel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialComboDelegate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialHighlighter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/QMLMaterialHighlighter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LLMManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LLMWorker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LLMSettingsWidget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OnnxRuntimeSettings.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ModelDownloader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SDManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SDWorker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SentryReporter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/BoneWeightOverlay.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/NormalVisualizer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AnimationMerger.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FootContact.cpp
        # Face auto-rig (#889): MCPServer/CLIPipeline/mainwindow reference
        # these — without them the per-suite test executables fail to link.
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FaceRigController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FaceRig/ArkitTemplate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FaceRig/NonRigidICP.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FaceRig/SparseSolve.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FaceRig/DeformationTransfer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FaceRig/FaceRigger.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FaceRig/FaceRigAttach.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FaceRig/FaceLandmarkDetector.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FaceRig/FaceRigLandmarks.cpp
        # Performance capture (#869): MCPServer/CLIPipeline/mainwindow
        # reference these too. The .cpp files self-guard with #ifdef
        # ENABLE_MOCAP (empty TU when off), so listing them unconditionally is
        # safe and keeps the per-suite test executables linking either way.
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/VideoFrameSource.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/MocapCameraHints.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/OneEuroFilter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/FaceCapMapper.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/FaceCapPose.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/FaceCapGeom.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/FaceCapPredictor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/MocapRecorder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/PoseCapPredictor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/PoseIKSolver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/MocapCLI.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/MocapController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/MocapPoseDebugOverlay.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/MocapBodyDriveDebug.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Mocap/HandCapPredictor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/RecordMocapClipCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MCPServer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MCPSettingsDialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshInfoOverlay.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CLIPipeline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/QtMeshCloudClient.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CloudCredentialStore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CloudDeepLink.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CloudUploadPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CloudCLIPipeline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CloudProjectsController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CloudUploadDialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CloudUploadProgress.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/DependencyResolver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ProjectPackager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/QtMeshCloudSession.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/GamificationTypes.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/GamificationEventQueue.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/GamificationManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/GamificationToast.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CloudAccountMenuButton.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FeedbackDiagnostics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FeedbackDialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FeedbackReportHelper.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/RTShaderHelper.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/QMLMaterialHighlighter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ViewCube/ViewCubeController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UndoManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/TransformCommands.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/LightCommands.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/MoveKeyframeCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/BulkKeyframeCommands.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/SetKeyframeValueCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/ResampleCurveCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/CurveEditModelChangeCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/DecimateTrackCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/BoneTransformCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/AddKeyframeCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/ApplyMaterialCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/DeleteKeyframeCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/SkeletonResolver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/BoneDragRelease.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PropertiesPanelController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SceneTreeModel.cpp

        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ThemeManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/BatchExporter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialPresetLibrary.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PaintChannelPresets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SymmetryMirrorMap.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TextureChannelPacker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TextureAtlasPacker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PaintBufferImageProvider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PaintLayerBlend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PaintLayerStack.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PaintSelectionMask.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/GradientRamp.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ColorPaletteLibrary.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/BrushEngine.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/BrushFootprint.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/BrushAssetLibrary.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TexturePaintBuffer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TexturePaintController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ProjectionPainter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/DecalSession.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MultiViewTextureBaker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/VertexColorBaker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/VATBaker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/VATBakerController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/IsometricSpritesController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/VATShaderEmitter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MinimalEXRWriter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HDR/HDREnvironmentManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HDR/HdrEquirectLoader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HDR/HdrIblPrecompute.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HDR/HdrCache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HDR/HdrPrecomputeWorker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HDR/HdrIblRtss.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HDR/HdrTonemap.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HDR/HdrViewportPipeline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HDR/HdrViewportController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HDR/HdrMaterialScript.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HDR/HdrEnvironmentController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HDR/HdrBundledLibrary.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MorphAnimationManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/MorphCommands.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/NodeAnimationManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/NodeAnimCommands.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PoseLibrary.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/PoseLibraryCommands.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/VertexAnimationManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AlembicImporter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/ComputeSkinWeightsCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/AutoRigCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/SplitMeshCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/ExplodePartsCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/JoinPartsCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/SkeletonBoneCommands.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/UVEditCommand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/UvSeamCommands.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UvSeamData.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UvSeamOps.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ApplyAtlas.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/EmbeddedTextureCache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/NormalMapGenerator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ImageTo3D/MarchingCubes.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ImageTo3D/MeshRefine.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ImageTo3D/MeshGenBaker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ImageTo3D/MeshGenPredictor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ImageTo3D/TripoSGPredictor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ImageTo3D/MeshGenBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ImageTo3D/MeshGenController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ImageTo3D/BackgroundRemover.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ImageTo3D/ImageCaptioner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PbrMapSynth.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TextureUpscaler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AIAssistManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MemoryEstimator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/DrawCallAnalyzer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/VertexCacheOptimizer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshDecimator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshDecimatorController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshLodController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PartOpsController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UvUnwrapController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UVEditorController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UVTransform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/QuadRetopo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/QuadRetopoController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SkinWeights.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SkinWeightsPost.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/GeodesicVoxelBind.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SkinningDisplay.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SkinMetrics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SkinEvaluate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SkinTokensPredictor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SkinWeightsController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AutoRig.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AutoRigController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UniRigPredictor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MotionInbetween.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MotionLibrary.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MotionGenerator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshSegmenter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshDepthRenderer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshOptimizerLod.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ExportOptimizer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UvUnwrap.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UvProject.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UvPipeline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshValidator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AIChatManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AIModelCatalog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/WelcomeScreenController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/WelcomeDialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SubMeshTransform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SubEntityHighlight.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ScanConfig.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ScanEngine.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PlatformProfile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AssetBrowserController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AssetScanController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialPreviewRenderer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ModelTurntableRenderer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ModelIsometricRenderer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/EditableMesh.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/EditModeController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/EditorModeController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HalfEdgeMesh.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SubMeshOps.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PartOpsMesh.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PartOpsScene.cpp

        # PS1 importers are referenced by core code (MaterialEditorQML, MeshImporterExporter).
        # Include them so the MaterialEditorQML test executables link successfully.
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/PS1MAT.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/PS1PLY.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/PS1RSD.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/PS1TMD.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/PS1TIM.cpp
    )

    if(ENABLE_PS1_RIP)
        list(APPEND TEST_SRC_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/CaptureBuffer.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/CaptureSnapshot.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/EmuCoreLoader.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/GteInverse.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/EmuViewport.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/Gp0CaptureStats.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/Gp0HookDispatch.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxGp0ChainRootScanner.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxGp0ChainWalker.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxOrderingTableScanner.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/GpuCommandParser.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/GteCapture.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxGteEngine.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxGteInstructionCapture.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxMipsGteRunner.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/MeshReconstructionStats.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/MeshReconstructor.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/MeshReconstructorTexKeys.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/MeshTopologyHash.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/Ps1AnimationExtractor.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/Ps1CoordinateNormalizer.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PS1CapturedAssets.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PS1ExtractedAssetBrowser.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PS1GeometryInspectorPanel.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PS1RipMeshBuilder.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PS1RipGamepadBridge.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PS1RipInputSettingsDialog.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PS1RipLegalityDialog.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PS1RipManager.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PS1RipSessionWindow.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxJoypadBindings.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PS1RipWorker.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxBiosValidator.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxDiscResolver.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxGoldenCapture.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxJoypadState.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxCaptureFilters.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxGpuRamScanner.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxGteRamScanner.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxHmdRamScanner.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/PsxTmdRamScanner.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/RipperHooks.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/TextureDecoder.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/VramSnapshot.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/PS1/runtime/VramViewerWidget.cpp
        )
    endif()

    set(TEST_HEADER_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AnimationBlender.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AnimationControlController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/GlobalDefinitions.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Euler.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/about.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AppLaunchHandler.h
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
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshDracoEncoder.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/EditorViewport.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ViewportTitleBar.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/RotationGizmo.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TranslationGizmo.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ScaleGizmo.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/BevelGizmo.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/TransformOperator.h

        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PrimitivesWidget.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PrimitiveObject.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ViewportGrid.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AnimationWidget.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SelectionSet.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightManager.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightRigLibrary.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightVisualizer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightsController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LightPropertiesController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SceneLightingController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ShadowController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SceneLightsIO.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SelectionBoxObject.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ObjectItemModel.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialComboDelegate.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialHighlighter.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/QMLMaterialHighlighter.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LLMManager.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LLMWorker.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/LLMSettingsWidget.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ModelDownloader.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SDManager.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SDWorker.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SentryReporter.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/BoneWeightOverlay.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/NormalVisualizer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AnimationMerger.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MCPServer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MCPSettingsDialog.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshInfoOverlay.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CLIPipeline.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/CloudDeepLink.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/QtMeshCloudClient.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/GamificationTypes.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/GamificationEventQueue.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/GamificationManager.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/GamificationToast.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/RTShaderHelper.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ViewCube/ViewCubeController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UndoManager.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/TransformCommands.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/LightCommands.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PropertiesPanelController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SceneTreeModel.h

        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ThemeManager.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/BatchExporter.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialPresetLibrary.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PaintChannel.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/PaintChannelPresets.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshLodController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshOptimizerLod.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ExportOptimizer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UvUnwrap.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UvProject.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/UvPipeline.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MeshValidator.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AIChatManager.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AIModelCatalog.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/WelcomeScreenController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/WelcomeDialog.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SubMeshTransform.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/SubEntityHighlight.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ScanConfig.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ScanEngine.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AssetBrowserController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/AssetScanController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/MaterialPreviewRenderer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ModelTurntableRenderer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ModelIsometricRenderer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/IsometricSpritesController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/EditableMesh.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/EditModeController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/EditorModeController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/HalfEdgeMesh.h
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

    # Add FBX sources (matching src/FBX/CMakeLists.txt)
    set(TEST_SRC_FILES ${TEST_SRC_FILES}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FBX/FBXExporter.cpp
    )

    set(TEST_HEADER_FILES ${TEST_HEADER_FILES}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FBX/FBXExporter.h
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

    # Shared static lib: compile MaterialEditor test support sources once instead
    # of rebuilding them for every *_test executable (CI build-wrapper budget).
    set(TEST_SUPPORT_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${BUILD_INCLUDE_DIR}
        ${BUILD_UIH_DIR}
        ${OGRE_PROCEDURAL_LIB_DIR}include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/OgreXML
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Assimp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/FBX
    )

    set(TEST_SUPPORT_LIBRARIES
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
        Qt::QuickControls2
        meshoptimizer
        xatlas
        qtmesh_sodium
        qtmesh_updater
    )

    if(ENABLE_LOCAL_LLM)
        list(APPEND TEST_SUPPORT_LIBRARIES llama ggml)
        # ImageCaptioner.cpp (in the test-common lib) uses mtmd under
        # ENABLE_MTMD — link it where available (not Windows-MinGW).
        if(QTMESH_HAVE_MTMD)
            list(APPEND TEST_SUPPORT_LIBRARIES mtmd)
        endif()
    endif()

    if(ENABLE_SENTRY)
        list(APPEND TEST_SUPPORT_LIBRARIES sentry)
    endif()

    # #404: PbrMapSynth.cpp links ONNX Runtime when ENABLE_ONNX is set, so the
    # QML component test executables (which compile it via this common lib) need
    # the imported target on the link line too.
    if(ENABLE_ONNX)
        list(APPEND TEST_SUPPORT_LIBRARIES qtmesh_onnx)
    endif()
    if(ENABLE_ALEMBIC)
        list(APPEND TEST_SUPPORT_LIBRARIES qtmesh_alembic)
    endif()
    # #506: MeshDracoEncoder.cpp (compiled into this common lib, and called from
    # CLIPipeline.cpp) links Draco when ENABLE_DRACO is set — the QML component
    # test executables need it on their link line too.
    if(ENABLE_DRACO AND QTMESH_DRACO_FOUND)
        list(APPEND TEST_SUPPORT_LIBRARIES qtmesh_draco)
    endif()

    add_library(qtmesh_test_common STATIC ${TEST_SRC_FILES} ${TEST_HEADER_FILES})
    set_target_properties(qtmesh_test_common PROPERTIES
        AUTOMOC ON
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(qtmesh_test_common PRIVATE ${TEST_SUPPORT_INCLUDES})
    if(stb_SOURCE_DIR)
        target_include_directories(qtmesh_test_common PRIVATE ${stb_SOURCE_DIR})
    endif()
    if(ENABLE_OPENEXR AND tinyexr_SOURCE_DIR)
        target_include_directories(qtmesh_test_common PRIVATE
            ${tinyexr_SOURCE_DIR}
            ${tinyexr_SOURCE_DIR}/deps/miniz)
        if(TARGET tinyexr_miniz)
            target_link_libraries(qtmesh_test_common PUBLIC tinyexr_miniz)
        endif()
    endif()
    target_compile_definitions(qtmesh_test_common PRIVATE
        BATCH_EXPORTER_TEST_SEAM
        QTMESH_UNIT_TESTS
        "QTMESH_UT_SOURCE_ROOT=\"${CMAKE_SOURCE_DIR}\"")
    # Manager.cpp (compiled into this lib) picks plugins_d.cfg vs plugins.cfg at
    # compile time via #ifdef DEBUG. The scoped-DEBUG fix (src/CMakeLists.txt)
    # applied DEBUG to QtMeshEditor + UnitTests but NOT here, so the standalone
    # per-file test executables (MaterialEditorQML_test, …) looked for the
    # nonexistent plugins.cfg in a Debug build → Ogre init failed. Apply DEBUG
    # here too (PRIVATE — can't leak into FetchContent deps).
    if(QTMESH_DEFINE_DEBUG)
        target_compile_definitions(qtmesh_test_common PRIVATE DEBUG)
    endif()
    target_link_libraries(qtmesh_test_common PUBLIC ${TEST_SUPPORT_LIBRARIES})

    if(APPLE)
        target_link_libraries(qtmesh_test_common PUBLIC "-framework Security")
    elseif(WIN32)
        target_link_libraries(qtmesh_test_common PUBLIC advapi32)
    endif()

    # Performance capture (#869): the Mocap frame sources include Qt
    # Multimedia headers (QCamera/QMediaPlayer/QVideoSink). The main build
    # links Qt6::Multimedia into UnitTests under ENABLE_MOCAP; do the same for
    # the per-suite test library so MCPServer/mainwindow (which reference the
    # Mocap layer) both compile and link.
    if(ENABLE_MOCAP)
        target_link_libraries(qtmesh_test_common PUBLIC Qt6::Multimedia)
    endif()

    if(ENABLE_PS1_RIP)
        add_dependencies(qtmesh_test_common qtmesh_ps1core_stub)
        if(TARGET qtmesh_ps1core_libretro)
            add_dependencies(qtmesh_test_common qtmesh_ps1core_libretro)
        endif()
        if(TARGET Qt6::Gamepad)
            target_compile_definitions(qtmesh_test_common PRIVATE HAVE_QT_GAMEPAD)
            target_link_libraries(qtmesh_test_common PUBLIC Qt6::Gamepad)
        endif()
    endif()

    ADD_DEPENDENCIES(qtmesh_test_common ui)

    set(COMMON_TEST_LIBRARIES
        gtest
        gtest_main
        gmock
        gmock_main
        qtmesh_test_common
    )
    
    # Helper function to create test executables
    function(create_test_executable target_name test_source_file)
        add_executable(${target_name} 
            ${test_source_file}
            ${TEST_RESOURCE_SRCS}
            ${TEST_QML_RESOURCE_SRCS}
        )
        
        target_include_directories(${target_name} PRIVATE ${TEST_SUPPORT_INCLUDES})
        
        target_link_libraries(${target_name} ${COMMON_TEST_LIBRARIES})
        
        # Add dependency on UI generation
        ADD_DEPENDENCIES(${target_name} ui)
        
        # Add the test to CTest
        add_test(NAME ${target_name} COMMAND ${target_name})
    endfunction()
    
    # Helper function to create test executables without auto-adding to CTest
    function(create_test_executable_no_autotest target_name test_source_file)
        add_executable(${target_name} 
            ${test_source_file}
            ${TEST_RESOURCE_SRCS}
            ${TEST_QML_RESOURCE_SRCS}
        )
        
        target_include_directories(${target_name} PRIVATE ${TEST_SUPPORT_INCLUDES})
        
        target_link_libraries(${target_name} ${COMMON_TEST_LIBRARIES})
        
        # Add dependency on UI generation
        ADD_DEPENDENCIES(${target_name} ui)
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

    # Cloud account toolbar widget tests
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../src/CloudAccountMenuButton_test.cpp")
        create_test_executable(CloudAccountMenuButton_test
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/CloudAccountMenuButton_test.cpp"
        )
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../src/ProjectPackager_test.cpp")
        create_test_executable(ProjectPackager_test
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/ProjectPackager_test.cpp"
        )
    endif()
    
    # 4. QML Component Test Runner (existing) - special handling for CI
    create_test_executable_no_autotest(MaterialEditorQML_qml_test_runner
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
    # Skip auto-discovery for QML tests in CI environments to avoid Qt platform issues
    if(TARGET MaterialEditorQML_test)
        gtest_discover_tests(MaterialEditorQML_test DISCOVERY_MODE PRE_TEST)
    endif()
    
    if(TARGET MaterialEditorQML_qml_test)
        gtest_discover_tests(MaterialEditorQML_qml_test DISCOVERY_MODE PRE_TEST)
    endif()
    
    if(TARGET MaterialEditorQML_perf_test)
        gtest_discover_tests(MaterialEditorQML_perf_test DISCOVERY_MODE PRE_TEST)
    endif()

    if(TARGET CloudAccountMenuButton_test)
        gtest_discover_tests(CloudAccountMenuButton_test DISCOVERY_MODE PRE_TEST)
    endif()
    if(TARGET ProjectPackager_test)
        gtest_discover_tests(ProjectPackager_test DISCOVERY_MODE PRE_TEST)
    endif()
    
    if(TARGET MaterialEditorQML_qml_test_runner)
        # Don't use gtest_discover_tests for QML runner to avoid CI issues
        # Just add it as a simple test
        add_test(NAME MaterialEditorQML_qml_test_runner 
                COMMAND MaterialEditorQML_qml_test_runner)
        
        # Set environment variables for headless execution
        set_tests_properties(MaterialEditorQML_qml_test_runner PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false")
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
