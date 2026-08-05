#ifndef MCPSERVER_H
#define MCPSERVER_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QSocketNotifier>
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMap>
#include <QTimer>
#include <functional>
#include <memory>

#include <OgreException.h>

namespace Ogre { class SceneNode; class Entity; }

class MainWindow;

/**
 * @brief MCP (Model Context Protocol) Server for QtMeshEditor
 *
 * This server allows AI agents (Claude, Cursor, etc.) to interact with
 * QtMeshEditor programmatically via the MCP protocol.
 *
 * MCP uses JSON-RPC 2.0 over stdio for communication.
 */
class MCPServer : public QObject
{
    Q_OBJECT

public:
    explicit MCPServer(QObject *parent = nullptr);
    ~MCPServer();

    /**
     * @brief Start the MCP server in stdio mode
     * Reads from stdin, writes to stdout
     */
    void start();

    /**
     * @brief Stop the MCP server
     */
    void stop();

    /**
     * @brief Start the HTTP REST API server
     * @param port TCP port to listen on (default 8080)
     * @return true if the server started successfully
     */
    bool startHttp(int port = 8080);

    /**
     * @brief Call a tool by name with arguments (public API for HTTP and AI Chat)
     */
    QJsonObject callTool(const QString &name, const QJsonObject &args);

    /**
     * @brief Build the list of available tools (public API for AI Chat)
     */
    QJsonArray buildToolsList();

    /**
     * @brief Set the main window reference for accessing editor functionality
     */
    void setMainWindow(QObject *mainWindow);

    /**
     * @brief Set the file descriptor for MCP output (instead of stdout)
     * Used when stdout is redirected to stderr to avoid mixing with Ogre output
     */
    void setOutputFd(int fd);

    /**
     * @brief Stop the HTTP REST API server
     */
    void stopHttp();

    /**
     * @brief Check if the HTTP server is currently running
     */
    bool isHttpRunning() const;

    /**
     * @brief Get the configured HTTP port
     */
    int httpPort() const;

    /**
     * @brief Check if server is running
     */
    bool isRunning() const { return m_running; }

    /**
     * @brief Mark Ogre initialization as failed (for testing without GL context)
     */
    void setOgreInitFailed(bool failed) { m_ogreInitFailed = failed; }

signals:
    void messageReceived(const QJsonObject &message);
    void errorOccurred(const QString &error);

private slots:
    void onReadyRead();
    void onHttpConnection();

private:
    using ToolHandler = QJsonObject (MCPServer::*)(const QJsonObject &);

    // Message handling
    void processMessage(const QByteArray &data);
    void sendResponse(const QJsonObject &response);
    void sendError(const QJsonValue &id, int code, const QString &message);
    void sendNotification(const QString &method, const QJsonObject &params);

    // MCP Protocol handlers
    QJsonObject handleInitialize(const QJsonObject &params);
    QJsonObject handleToolsList();
    QJsonObject handleToolsCall(const QJsonObject &params);
    QJsonObject handleResourcesList();
    QJsonObject handleResourcesRead(const QJsonObject &params);

    // Tool implementations
    QJsonObject toolCreateMaterial(const QJsonObject &args);
    QJsonObject toolModifyMaterial(const QJsonObject &args);
    QJsonObject toolGetMaterial(const QJsonObject &args);
    QJsonObject toolListMaterials(const QJsonObject &args);
    QJsonObject toolApplyMaterial(const QJsonObject &args);
    QJsonObject toolListMaterialPresets(const QJsonObject &args);
    QJsonObject toolApplyMaterialPreset(const QJsonObject &args);
    QJsonObject toolSetHdrEnvironment(const QJsonObject &args);
    QJsonObject toolGetHdrEnvironment(const QJsonObject &args);
    QJsonObject toolSetTonemap(const QJsonObject &args);
    QJsonObject toolSetEnvIntensity(const QJsonObject &args);
    QJsonObject toolSetEnvTint(const QJsonObject &args);
    /// #406: LLM-assisted material from a natural-language description. Drives
    /// the local LLM synchronously and binds the generated material to the
    /// target/selected mesh. Errors gracefully when no model is loaded.
    QJsonObject toolDescribeMaterial(const QJsonObject &args);
    QJsonObject toolLoadMesh(const QJsonObject &args);
    /// Select every user scene node and frame the active viewport camera on the
    /// aggregate bounds, so a headless take_screenshot after load_mesh actually
    /// shows the mesh (frameSelection() no-ops on an empty selection). No-op
    /// when no MainWindow/viewport (headless --mcp without GUI).
    void frameSceneInActiveViewport();
    QJsonObject toolGetMeshInfo(const QJsonObject &args);
    QJsonObject toolTransformMesh(const QJsonObject &args);
    QJsonObject toolTransformSubMesh(const QJsonObject &args);
    QJsonObject toolListTextures(const QJsonObject &args);
    QJsonObject toolSetTexture(const QJsonObject &args);
    QJsonObject toolExportMesh(const QJsonObject &args);
    /// xatlas-backed automatic UV unwrap on the currently selected
    /// entity (issue #400). Writes non-overlapping UVs into the
    /// chosen channel (default 0). Skin weights survive the seam
    /// splits via xref remap.
    QJsonObject toolAutoUvUnwrap(const QJsonObject &args);
    /// Issue #465: UV channel/island/overlap info for the selected entity.
    QJsonObject toolUvInfo(const QJsonObject &args);
    /// Issue #465: headless geometric UV projection (box/cylinder/sphere/reset).
    QJsonObject toolUvProject(const QJsonObject &args);
    /// Issue #465: mark seam edges on the selected mesh.
    QJsonObject toolUvSetSeams(const QJsonObject &args);
    /// Issue #465: xatlas re-unwrap of selected triangle indices on one submesh.
    QJsonObject toolUvUnwrapSelection(const QJsonObject &args);
    /// Issue #401: triangle-pairing quad retopology on the
    /// currently selected entity. Pairs adjacent triangles into
    /// convex quads when coplanarity + shape + aspect ratio gates
    /// pass. Output is committed via the n-gon binding.
    QJsonObject toolRetopologize(const QJsonObject &args);
    /// Issue #402 (+ #819): compute skin weights (geodesic-voxel
    /// default). Mesh must have a skeleton attached.
    QJsonObject toolComputeSkinWeights(const QJsonObject &args);
    /// #819 Slice D: toggle the selected entity's skinning display
    /// mode ("linear" | "dual-quaternion") via RTSS hardware
    /// skinning. Display only — exported weights are unchanged.
    QJsonObject toolSetSkinningDisplay(const QJsonObject &args);
    /// #407: native auto-rig of the selected static mesh (template embedding),
    /// optional skin chain + re-export.
    QJsonObject toolAutoRig(const QJsonObject &args);
    /// #889: fit the ARKit blendshape template onto the selected face mesh
    /// (NRICP + deformation transfer), attach the 52 ARKit morph targets, and
    /// optionally re-export.
    QJsonObject toolAddArkitBlendshapes(const QJsonObject &args);
    /// Issue #403: mesh-aware (depth-conditioned) texture
    /// generation. Renders the selected entity's depth map and
    /// conditions sd.cpp on it via a ControlNet depth model, then
    /// applies the result to the active material diffuse. Async —
    /// returns immediately; progress flows through SD signals.
    QJsonObject toolGenerateMeshTexture(const QJsonObject &args);
    QJsonObject toolGeneratePbrMaps(const QJsonObject &args);
    /// Issue #405: Real-ESRGAN 2x/4x texture super-resolution. Writes
    /// <stem>_upscaled.png next to the source; model downloaded on first use.
    QJsonObject toolUpscaleTexture(const QJsonObject &args);
    QJsonObject toolGetSceneInfo(const QJsonObject &args);
    QJsonObject toolTakeScreenshot(const QJsonObject &args);
    QJsonObject toolCreatePrimitive(const QJsonObject &args);
    QJsonObject toolAnimate(const QJsonObject &args);
    QJsonObject toolListSkeletalAnimations(const QJsonObject &args);
    QJsonObject toolGetAnimationInfo(const QJsonObject &args);
    QJsonObject toolSetAnimationLength(const QJsonObject &args);
    QJsonObject toolSetAnimationTime(const QJsonObject &args);
    QJsonObject toolAddKeyframe(const QJsonObject &args);
    QJsonObject toolRemoveKeyframe(const QJsonObject &args);
    QJsonObject toolPlayAnimation(const QJsonObject &args);
    QJsonObject toolToggleSkeletonDebug(const QJsonObject &args);
    QJsonObject toolToggleBoneWeights(const QJsonObject &args);
    QJsonObject toolToggleNormals(const QJsonObject &args);
    QJsonObject toolToggleMeshInfo(const QJsonObject &args);
    QJsonObject toolMergeAnimations(const QJsonObject &args);
    QJsonObject toolResampleAnimation(const QJsonObject &args);
    QJsonObject toolSimplifyAnimation(const QJsonObject &args);
    QJsonObject toolAnalyzeAnimation(const QJsonObject &args);
    QJsonObject toolBakeAnimationFps(const QJsonObject &args);
    QJsonObject toolMotionInBetween(const QJsonObject &args);
    QJsonObject toolGenerateMotion(const QJsonObject &args);    // #411 text-to-motion
    QJsonObject toolAdjustArmSpace(const QJsonObject &args);    // #854 arm-space
    QJsonObject toolPinFeet(const QJsonObject &args);           // #856 foot-contact pin
    QJsonObject toolSegmentMesh(const QJsonObject &args);
    QJsonObject toolSplitMeshBySegments(const QJsonObject &args);
    QJsonObject toolExplodeMeshParts(const QJsonObject &args);   // #862/#864
    QJsonObject toolJoinMeshParts(const QJsonObject &args);      // #862/#864
    QJsonObject toolGenerateMeshFromImage(const QJsonObject &args);   // #764 image-to-3D
    QJsonObject toolSaveScene(const QJsonObject &args);
    QJsonObject toolOpenScene(const QJsonObject &args);
    QJsonObject toolValidateMesh(const QJsonObject &args);
    QJsonObject toolGenerateLods(const QJsonObject &args);
    QJsonObject toolGenerateAutoLods(const QJsonObject &args);
    QJsonObject toolRemoveLods(const QJsonObject &args);
    QJsonObject toolGetLodInfo(const QJsonObject &args);
    QJsonObject toolGetMemoryUsage(const QJsonObject &args);
    QJsonObject toolAnalyzeDrawCalls(const QJsonObject &args);
    QJsonObject toolOptimizeVertexCache(const QJsonObject &args);
    QJsonObject toolDecimateMesh(const QJsonObject &args);
    QJsonObject toolListFiles(const QJsonObject &args);
    QJsonObject toolSearchFiles(const QJsonObject &args);
    QJsonObject toolReadFile(const QJsonObject &args);
    QJsonObject toolCameraControl(const QJsonObject &args);
    QJsonObject toolGetCameraInfo(const QJsonObject &args);
    QJsonObject toolDeleteEntity(const QJsonObject &args);
    QJsonObject toolCreateLight(const QJsonObject &args);
    QJsonObject toolDeleteLight(const QJsonObject &args);
    QJsonObject toolListLights(const QJsonObject &args);
    QJsonObject toolSetLightProperty(const QJsonObject &args);
    QJsonObject toolApplyLightRig(const QJsonObject &args);
    QJsonObject toolDuplicateEntity(const QJsonObject &args);
    QJsonObject toolSetSnapSettings(const QJsonObject &args);
    QJsonObject toolGetSnapSettings(const QJsonObject &args);
    QJsonObject toolExportPose(const QJsonObject &args);
    QJsonObject toolGroupNodes(const QJsonObject &args);
    QJsonObject toolUngroupNode(const QJsonObject &args);
    QJsonObject toolReparentNode(const QJsonObject &args);
    QJsonObject toolSetPivotMode(const QJsonObject &args);
    QJsonObject toolGetPivotMode(const QJsonObject &args);
    /// Slice G: pack 1-4 grayscale source images into a single RGBA
    /// output texture (e.g. ORM = AO+Roughness+Metallic).
    QJsonObject toolPackTextures(const QJsonObject &args);
    /// Slice H: generate a tangent-space normal map from a height/bump
    /// source via Sobel filter.
    QJsonObject toolGenerateNormalMap(const QJsonObject &args);
    /// Phase 6 slice E: pack N input textures into a single atlas image
    /// + JSON manifest of per-tile UV remaps. Shelf bin-pack, deterministic.
    QJsonObject toolPackAtlas(const QJsonObject &args);
    /// Phase 6 slice E2: consume a packed-atlas manifest — rewrite each
    /// submesh's UV0 into the matching tile's sub-rect and rebind the
    /// diffuse TUS to the atlas texture. Counterpart to pack_atlas.
    QJsonObject toolApplyAtlas(const QJsonObject &args);
    /// Phase 6 slice G: batch optimize pipeline. Runs the slice A–E
    /// optimizations end-to-end on a single asset and writes the result.
    /// Per-stage applied/summary report on success.
    QJsonObject toolOptimizeMesh(const QJsonObject &args);
    /// #724: 8-direction isometric animated sprite grid export (file-in / file-out).
    QJsonObject toolGenerateIsometricSprites(const QJsonObject &args);
    /// Phase VAT slice 4: bake a skeletal animation to a Vertex
    /// Animation Texture + JSON sidecar. Args mirror the
    /// `qtmesh vat` CLI subcommand: file, anim, fps, encoding,
    /// target, normals, output_dir, basename.
    QJsonObject toolBakeVat(const QJsonObject &args);

    /// Morph A6: list named morph targets / blend shapes on a mesh
    /// file. Args: `file` (path). Heavy — does a full mesh import.
    QJsonObject toolListMorphTargets(const QJsonObject &args);

    /// Morph A6: set a morph-target weight on the first selected
    /// entity in the editor. Args: `name`, `weight` (0..1).
    /// Light — pure state poke on a live entity.
    QJsonObject toolSetMorphWeight(const QJsonObject &args);

    /// Vertex-anim B3 (#519): import an Alembic (.abc) vertex cache into the
    /// live scene as a VAT_POSE-animated mesh. Args: `file` (path). Heavy —
    /// decodes the cache + builds an entity. Needs a build with ENABLE_ALEMBIC.
    QJsonObject toolImportAlembic(const QJsonObject &args);
    QJsonObject toolCaptureFaceFromVideo(const QJsonObject &args);
    QJsonObject toolCaptureBodyFromVideo(const QJsonObject &args);
    QJsonObject toolListCaptureDevices(const QJsonObject &args);
    QJsonObject toolStartLiveCapture(const QJsonObject &args);
    QJsonObject toolStopLiveCapture(const QJsonObject &args);
    QJsonObject toolSetCaptureChannels(const QJsonObject &args);

    /// Vertex-anim B3 (#519): enable + play a vertex-animation clip on an
    /// entity. Args: `entity`, `animation`. Light — state poke on a live entity.
    QJsonObject toolPlayVertexAnimation(const QJsonObject &args);

    /// Node-anim C6: list node-animation clips on the live scene.
    /// No args. Light — pure read.
    QJsonObject toolListNodeAnimations(const QJsonObject &args);

    /// Node-anim C6: create a new named clip on the live scene.
    /// Args: `name`, `length` (seconds). Light.
    QJsonObject toolAddNodeAnimationClip(const QJsonObject &args);

    /// Node-anim C6: write a transform keyframe on a clip+node.
    /// Args: `clip`, `node`, `time` (seconds), `translate` ([x,y,z]),
    /// `rotation` ([w,x,y,z] quaternion), `scale` ([x,y,z]). Light.
    QJsonObject toolSetNodeKeyframe(const QJsonObject &args);

    /// Pose-lib D-MCP: list saved pose names on the first selected
    /// entity. Light read; returns `{ count, poses: [name…] }`.
    QJsonObject toolListPoses(const QJsonObject &args);

    /// Pose-lib D-MCP: capture current bone-TRS on the first
    /// selected entity under `name`. Overwrites in place if the
    /// name already exists. Args: `name`.
    QJsonObject toolSavePose(const QJsonObject &args);

    /// Pose-lib D-MCP: snap the first selected entity to a saved
    /// pose. Args: `name`. Returns error when no such pose.
    QJsonObject toolApplyPose(const QJsonObject &args);

    /// Pose-lib D-MCP: drop a saved pose by name. Args: `name`.
    QJsonObject toolDeletePose(const QJsonObject &args);

    /// Pose-lib D-MCP: mirror a saved pose across the YZ plane
    /// using the _l/_r/.L/.R/Left/Right bone-name heuristic.
    /// Args: `src` (existing pose), `dst` (output pose name).
    QJsonObject toolMirrorPose(const QJsonObject &args);

    /// Pose-lib D-Project: write the first-selected entity's pose
    /// library to a `.poselib` sidecar JSON file. Args: `path`.
    QJsonObject toolSavePoseLibrary(const QJsonObject &args);

    /// Pose-lib D-Project: load a `.poselib` sidecar into the
    /// first-selected entity's library, replacing whatever was
    /// in memory. Args: `path`.
    QJsonObject toolLoadPoseLibrary(const QJsonObject &args);

    /// Pose-lib D5: apply a saved pose only to a subset of bones.
    /// Args: `name` (pose), `bones` (JSON array of bone-name
    /// strings). Use case: facial expression without disturbing
    /// the body pose.
    QJsonObject toolApplyPoseMasked(const QJsonObject &args);

    /// Epic #684 slice H: QtMesh Cloud account + upload tools.
    QJsonObject toolCloudStatus(const QJsonObject &args);
    QJsonObject toolCloudLimits(const QJsonObject &args);
    QJsonObject toolCloudLogin(const QJsonObject &args);
    QJsonObject toolCloudLogout(const QJsonObject &args);
    QJsonObject toolCloudListProjects(const QJsonObject &args);
    QJsonObject toolCloudDeleteProject(const QJsonObject &args);
    QJsonObject toolCloudUpload(const QJsonObject &args);

    // PS1 runtime ripper (#412) — headless drive-and-verify surface. Guarded
    // by ENABLE_PS1_RIP; each returns a structured error when the build lacks
    // the feature. All run on the MCP main thread (QSocketNotifier) and block
    // on a local QEventLoop while the worker boots/captures.
    QJsonObject toolPs1RipStart(const QJsonObject &args);
    QJsonObject toolPs1RipStop(const QJsonObject &args);
    QJsonObject toolPs1RipStatus(const QJsonObject &args);
    QJsonObject toolPs1RipRunFrames(const QJsonObject &args);
    QJsonObject toolPs1RipCapture(const QJsonObject &args);
    QJsonObject toolPs1RipStats(const QJsonObject &args);
    QJsonObject toolPs1RipClear(const QJsonObject &args);

    // Animation
    struct NodeAnimation {
        Ogre::SceneNode* node;
        float yawSpeed;   // degrees per second
        float pitchSpeed;
        float rollSpeed;
    };
    QMap<QString, NodeAnimation> m_animations;
    QTimer *m_animationTimer = nullptr;
    void onAnimationTick();

    // Skeleton entity resolution helper (shared by toggle_skeleton_debug / toggle_bone_weights)
    struct SkeletonEntityResult {
        Ogre::Entity* entity = nullptr;
        class AnimationWidget* animWidget = nullptr;
        QJsonObject error; // populated if validation failed
    };
    SkeletonEntityResult resolveSkeletonEntity(const QString &entityName);

    // Helper methods
    static QJsonObject makeErrorResult(const QString &message);
    static QJsonObject makeSuccessResult(const QString &message);

    /**
     * @brief Run an Ogre call and translate exceptions into MCP errors.
     *
     * 33+ tool handlers wrap their work in the same try/catch shape:
     *
     *     try { ... }
     *     catch (Ogre::Exception& e) {
     *         return makeErrorResult(QString("Ogre error: %1")...);
     *     }
     *
     * Sonar flagged the bulk of those repeats as duplication. The
     * helper keeps the same surface (return a QJsonObject) so call
     * sites stay one-line conversions: `return runOgreOp([&]{ ... });`
     */
    template <typename Body>
    static QJsonObject runOgreOp(Body&& body)
    {
        try {
            return body();
        } catch (const Ogre::Exception& e) {
            return makeErrorResult(QStringLiteral("Ogre error: %1")
                .arg(QString::fromStdString(e.getFullDescription())));
        } catch (const std::runtime_error& e) {
            return makeErrorResult(QStringLiteral("Error: %1").arg(e.what()));
        }
    }
    static const QMap<QString, ToolHandler>& toolHandlers();
    static bool isHeavyTool(const QString &name);
    bool ensureOgreInitialized();
    QJsonObject buildToolDefinition(const QString &name, const QString &description,
                                     const QJsonObject &inputSchema);

    bool m_ogreInitialized = false;
    bool m_ogreInitFailed = false;

    // HTTP server
    void handleHttpRequest(QTcpSocket *socket);
    QTcpServer *m_httpServer = nullptr;
    int m_httpPort = 8080;
    QMap<QTcpSocket*, QByteArray> m_httpBuffers;

    // Member variables
    QObject *m_mainWindow = nullptr;
    QSocketNotifier *m_stdinNotifier = nullptr;
    int m_stdinFd = -1;
    int m_stdoutFd = -1;
    bool m_running = false;
    bool m_initialized = false;
    bool m_httpBusy = false;  // Guard against re-entrant tool execution
    QByteArray m_buffer;

    // MCP Protocol version
    static constexpr const char* MCP_VERSION = "2024-11-05";
    static constexpr const char* SERVER_NAME = "QtMeshEditor";
    // 1.4.0 — added simplify_animation / analyze_animation tools
    // 1.5.0 — added bake_animation_fps tool
    // 1.6.0 — added list_material_presets / apply_material_preset (incl. PBR templates)
    static constexpr const char* SERVER_VERSION = "1.9.0";
};

#endif // MCPSERVER_H
