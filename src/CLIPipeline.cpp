#include "CLIPipeline.h"
#include "CloudCLIPipeline.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "AnimationMerger.h"
#include "MotionInbetween.h"
#include "MotionLibrary.h"
#include "MotionGenerator.h"
#include "MeshValidator.h"
#include "MeshLodController.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "ScanConfig.h"
#include "ScanEngine.h"
#include "PlatformProfile.h"
#include "FBX/FBXExporter.h"
#include "MaterialPresetLibrary.h"
#include "TextureChannelPacker.h"
#include "TextureAtlasPacker.h"
#include "ApplyAtlas.h"
#include "NormalMapGenerator.h"
#include "MemoryEstimator.h"
#include "DrawCallAnalyzer.h"
#include "VertexCacheOptimizer.h"
#include "ExportOptimizer.h"
#include "UvUnwrap.h"
#include "UvPipeline.h"
#include "UvProject.h"
#include "QuadRetopo.h"
#include "SkinWeights.h"
#include "AutoRig.h"
#include "ImageTo3D/MeshGenPredictor.h"
#include "ImageTo3D/MeshGenBuilder.h"
#include "MeshSegmenter.h"
#include "MeshDecimator.h"
#include "EditableMesh.h"
#include "TexturePaintBuffer.h"
#include "VertexColorBaker.h"
#include "VATBaker.h"
#include "VATShaderEmitter.h"
#include "MorphAnimationManager.h"
#include "NodeAnimationManager.h"
#include "PoseLibrary.h"
#include "ModelIsometricRenderer.h"
#include "ModelTurntableRenderer.h"
#include "QtMeshCloudClient.h"
#ifdef ENABLE_STABLE_DIFFUSION
#include "SDManager.h"
#include "MeshDepthRenderer.h"
#endif
// #406: the LLM "describe material" path is always compiled (LLMManager itself
// is always built; only the llama.cpp linking is gated by ENABLE_LOCAL_LLM), so
// these must live OUTSIDE the ENABLE_STABLE_DIFFUSION guard above. QImage is
// also used by always-on subcommands (turntable/isometric/upscale).
#include "LLMManager.h"
#include <QEventLoop>
#include <QImage>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#ifdef ENABLE_ONNX
#include "AIAssistManager.h"
#include "PbrMapSynth.h"
#include "TextureUpscaler.h"
#endif
// RTShaderHelper is a core RTSS helper (no ONNX/SD/LLM dependency) used
// unconditionally by the #406 describe-material path, so it must NOT sit inside
// the ENABLE_ONNX guard above — Windows MinGW builds ONNX off and would
// otherwise fail with "'RTShaderHelper' has not been declared".
#include "RTShaderHelper.h"
#include <OgreMaterialSerializer.h>
#include <QApplication>
#include <QWidget>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QDebug>
#include <QTextStream>
#include <QLocale>
#include <QSysInfo>

#include <assimp/Importer.hpp>
#include <assimp/Exporter.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <OgreSubMesh.h>

#include <unordered_map>
#include <vector>
#include <memory>

#include <set>
#include <cstdio>

#ifndef Q_OS_WIN
#include <unistd.h>
#else
#include <io.h>
#endif

// Saved original stdout fd — Ogre's stdout gets redirected to stderr
// so that Ogre debug output doesn't contaminate CLI pipeline output.
static int s_savedStdoutFd = -1;

/// Write a string to the CLI output stream (original stdout before redirect).
static void cliWrite(const QString& text)
{
    QByteArray utf8 = text.toUtf8();
#ifndef Q_OS_WIN
    if (s_savedStdoutFd >= 0) {
        ::write(s_savedStdoutFd, utf8.constData(), utf8.size());
    } else
#endif
    {
        fwrite(utf8.constData(), 1, utf8.size(), stdout);
        fflush(stdout);
    }
}

// ---------------------------------------------------------------------------
// Animation-only export helpers (no mesh artifacts)
// ---------------------------------------------------------------------------

static aiMatrix4x4 ogreTransformToAi(const Ogre::Vector3& pos,
                                     const Ogre::Quaternion& rot,
                                     const Ogre::Vector3& scale)
{
    // Assimp uses row-major aiMatrix4x4; build from SRT.
    aiMatrix4x4 s;
    s.a1 = scale.x; s.b2 = scale.y; s.c3 = scale.z;
    s.d4 = 1.0f;

    Ogre::Matrix3 r3;
    rot.ToRotationMatrix(r3);
    aiMatrix4x4 r;
    r.a1 = r3[0][0]; r.a2 = r3[0][1]; r.a3 = r3[0][2];
    r.b1 = r3[1][0]; r.b2 = r3[1][1]; r.b3 = r3[1][2];
    r.c1 = r3[2][0]; r.c2 = r3[2][1]; r.c3 = r3[2][2];
    r.d4 = 1.0f;

    aiMatrix4x4 t;
    t.a4 = pos.x;
    t.b4 = pos.y;
    t.c4 = pos.z;
    t.d4 = 1.0f;

    return t * r * s;
}

static aiScene* buildAnimOnlyAiSceneFromSkeleton(const Ogre::Skeleton* skel)
{
    if (!skel)
        return nullptr;

    auto* scene = new aiScene();

    // Root node
    scene->mRootNode = new aiNode();
    scene->mRootNode->mName = aiString("RootNode");
    scene->mRootNode->mTransformation = aiMatrix4x4(); // identity

    // Build bone node tree.
    std::unordered_map<const Ogre::Bone*, aiNode*> boneToNode;
    boneToNode.reserve(skel->getNumBones());

    auto makeNodeForBone = [&](const Ogre::Bone* b) -> aiNode* {
        auto it = boneToNode.find(b);
        if (it != boneToNode.end())
            return it->second;

        auto* n = new aiNode();
        n->mName = aiString(b->getName().c_str());
        n->mTransformation = ogreTransformToAi(b->getPosition(), b->getOrientation(), b->getScale());
        boneToNode[b] = n;
        return n;
    };

    // Attach root bones under scene root; attach children under parents.
    std::vector<aiNode*> rootBones;
    rootBones.reserve(skel->getNumBones());

    for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
        const Ogre::Bone* b = skel->getBone(i);
        if (!b)
            continue;

        aiNode* node = makeNodeForBone(b);
        const Ogre::Node* parent = b->getParent();
        const Ogre::Bone* parentBone = dynamic_cast<const Ogre::Bone*>(parent);
        if (!parentBone) {
            rootBones.push_back(node);
            continue;
        }

        aiNode* parentNode = makeNodeForBone(parentBone);
        // Append as child
        aiNode** newChildren = new aiNode*[parentNode->mNumChildren + 1];
        for (unsigned int ci = 0; ci < parentNode->mNumChildren; ++ci)
            newChildren[ci] = parentNode->mChildren[ci];
        newChildren[parentNode->mNumChildren] = node;
        delete[] parentNode->mChildren;
        parentNode->mChildren = newChildren;
        parentNode->mNumChildren += 1;
        node->mParent = parentNode;
    }

    if (!rootBones.empty()) {
        scene->mRootNode->mChildren = new aiNode*[rootBones.size()];
        scene->mRootNode->mNumChildren = static_cast<unsigned int>(rootBones.size());
        for (unsigned int i = 0; i < scene->mRootNode->mNumChildren; ++i) {
            scene->mRootNode->mChildren[i] = rootBones[i];
            rootBones[i]->mParent = scene->mRootNode;
        }
    }

    // Animations
    scene->mNumAnimations = skel->getNumAnimations();
    scene->mAnimations = scene->mNumAnimations ? new aiAnimation*[scene->mNumAnimations] : nullptr;

    for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai) {
        const Ogre::Animation* anim = skel->getAnimation(ai);
        auto* a = new aiAnimation();
        a->mName = aiString(anim->getName().c_str());
        a->mDuration = anim->getLength();
        a->mTicksPerSecond = 1.0; // key times are in seconds

        // Collect bone tracks.
        std::vector<const Ogre::NodeAnimationTrack*> tracks;
        tracks.reserve(anim->getNumNodeTracks());
        for (unsigned short ti = 0; ti < anim->getNumNodeTracks(); ++ti) {
            const Ogre::NodeAnimationTrack* t = anim->getNodeTrack(ti);
            if (t)
                tracks.push_back(t);
        }

        a->mNumChannels = static_cast<unsigned int>(tracks.size());
        a->mChannels = a->mNumChannels ? new aiNodeAnim*[a->mNumChannels] : nullptr;

        for (unsigned int ci = 0; ci < a->mNumChannels; ++ci) {
            const Ogre::NodeAnimationTrack* t = tracks[ci];
            const Ogre::Node* target = t->getAssociatedNode();
            const Ogre::Bone* bone = dynamic_cast<const Ogre::Bone*>(target);

            auto* ch = new aiNodeAnim();
            ch->mNodeName = aiString(bone ? bone->getName().c_str() : "Unknown");

            const unsigned short kCount = t->getNumKeyFrames();
            ch->mNumPositionKeys = kCount;
            ch->mNumRotationKeys = kCount;
            ch->mNumScalingKeys = kCount;
            ch->mPositionKeys = kCount ? new aiVectorKey[kCount] : nullptr;
            ch->mRotationKeys = kCount ? new aiQuatKey[kCount] : nullptr;
            ch->mScalingKeys = kCount ? new aiVectorKey[kCount] : nullptr;

            for (unsigned short ki = 0; ki < kCount; ++ki) {
                const Ogre::TransformKeyFrame* kf = t->getNodeKeyFrame(ki);
                const double time = kf->getTime();

                const Ogre::Vector3 p = kf->getTranslate();
                const Ogre::Quaternion r = kf->getRotation();
                const Ogre::Vector3 s = kf->getScale();

                ch->mPositionKeys[ki].mTime = time;
                ch->mPositionKeys[ki].mValue = aiVector3D(p.x, p.y, p.z);

                ch->mRotationKeys[ki].mTime = time;
                ch->mRotationKeys[ki].mValue = aiQuaternion(r.w, r.x, r.y, r.z);

                ch->mScalingKeys[ki].mTime = time;
                ch->mScalingKeys[ki].mValue = aiVector3D(s.x, s.y, s.z);
            }

            a->mChannels[ci] = ch;
        }

        scene->mAnimations[ai] = a;
    }

    return scene;
}

static QString assimpExportFormatIdForAnimOnlyPath(const QString& outputPath)
{
    const QString fmt = CLIPipeline::formatForExtension(outputPath);
    static const QMap<QString, QString> kUiToAssimp = {
        {QStringLiteral("Collada (*.dae)"), QStringLiteral("collada")},
        {QStringLiteral("X (*.x)"), QStringLiteral("x")},
        {QStringLiteral("OBJ (*.obj)"), QStringLiteral("obj")},
        {QStringLiteral("STL (*.stl)"), QStringLiteral("stl")},
        {QStringLiteral("PLY (*.ply)"), QStringLiteral("ply")},
        {QStringLiteral("3DS (*.3ds)"), QStringLiteral("3ds")},
        {QStringLiteral("glTF 2.0 (*.gltf)"), QStringLiteral("gltf2")},
        {QStringLiteral("glTF 2.0 (*.gltf2)"), QStringLiteral("gltf2")},
        {QStringLiteral("glTF 2.0 Binary (*.glb)"), QStringLiteral("glb2")},
        {QStringLiteral("glTF 2.0 Binary (*.glb2)"), QStringLiteral("glb2")},
        {QStringLiteral("VRM / glTF 2.0 (*.vrm)"), QStringLiteral("gltf2")},
        {QStringLiteral("FBX Binary (*.fbx)"), QStringLiteral("fbx")},
        {QStringLiteral("Assimp Binary (*.assbin)"), QStringLiteral("assbin")},
    };
    if (auto it = kUiToAssimp.find(fmt); it != kUiToAssimp.end())
        return it.value();
    const QString suf = QFileInfo(outputPath).suffix().toLower();
    static const QMap<QString, QString> kExtToAssimp = {
        {QStringLiteral("fbx"), QStringLiteral("fbx")},
        {QStringLiteral("dae"), QStringLiteral("collada")},
        {QStringLiteral("obj"), QStringLiteral("obj")},
        {QStringLiteral("stl"), QStringLiteral("stl")},
        {QStringLiteral("ply"), QStringLiteral("ply")},
        {QStringLiteral("3ds"), QStringLiteral("3ds")},
        {QStringLiteral("gltf"), QStringLiteral("gltf2")},
        {QStringLiteral("glb"), QStringLiteral("glb2")},
        {QStringLiteral("vrm"), QStringLiteral("gltf2")},
        {QStringLiteral("assbin"), QStringLiteral("assbin")},
        {QStringLiteral("x"), QStringLiteral("x")},
    };
    return kExtToAssimp.value(suf, QStringLiteral("fbx"));
}

static bool exportAnimOnlyViaAssimp(const Ogre::SkeletonPtr& skel, const QString& outputPath, QString* outError = nullptr)
{
    if (!skel) {
        if (outError) *outError = QStringLiteral("No skeleton");
        return false;
    }

    std::unique_ptr<aiScene> scene(buildAnimOnlyAiSceneFromSkeleton(skel.get()));
    if (!scene) {
        if (outError) *outError = QStringLiteral("Failed to build animation-only scene");
        return false;
    }

    const QString formatId = assimpExportFormatIdForAnimOnlyPath(outputPath);
    const unsigned int exportFlags =
        (formatId == QLatin1String("x")) ? 0u : aiProcess_ConvertToLeftHanded;

    Assimp::Exporter exporter;
    const aiReturn r = exporter.Export(scene.get(), formatId.toStdString().c_str(),
                                       outputPath.toStdString().c_str(), exportFlags);
    if (r != AI_SUCCESS) {
        if (outError)
            *outError = QString::fromUtf8(exporter.GetErrorString());
        return false;
    }
    return true;
}

static bool exportAnimOnly(const Ogre::SkeletonPtr& skel, const QString& outputPath, QString* outError = nullptr)
{
    const QString suf = QFileInfo(outputPath).suffix().toLower();
    if (suf == QStringLiteral("fbx") || suf == QStringLiteral("fbxa")) {
        if (!skel) {
            if (outError) *outError = QStringLiteral("No skeleton");
            return false;
        }
        if (!FBXExporter::exportSkeletonOnlyFBX(skel.get(), outputPath)) {
            if (outError) *outError = QStringLiteral("Custom FBX exporter failed");
            return false;
        }
        return true;
    }
    return exportAnimOnlyViaAssimp(skel, outputPath, outError);
}

static bool cliSupportsColor()
{
    if (qEnvironmentVariableIsSet("NO_COLOR"))
        return false;

    const QByteArray forceColor = qgetenv("CLICOLOR_FORCE");
    if (!forceColor.isEmpty() && forceColor != "0")
        return true;

#ifdef Q_OS_WIN
    const int fd = (s_savedStdoutFd >= 0) ? s_savedStdoutFd : _fileno(stdout);
    return fd >= 0 && _isatty(fd);
#else
    const int fd = (s_savedStdoutFd >= 0) ? s_savedStdoutFd : fileno(stdout);
    return fd >= 0 && ::isatty(fd);
#endif
}

static QString colorizeWord(const QString& text, const char* ansiColor, bool enabled)
{
    if (!enabled)
        return text;
    return QStringLiteral("\x1b[%1m%2\x1b[0m").arg(QString::fromLatin1(ansiColor), text);
}

static QString colorizeIconWhenPositive(const QString& icon, int value, const char* ansiColor, bool enabled)
{
    if (value <= 0)
        return icon;
    return colorizeWord(icon, ansiColor, enabled);
}
static QString scanStatusLabel(bool hasError, bool hasWarning, bool colorize)
{
    if (hasError)
        return colorizeWord("ERROR", "31", colorize);
    if (hasWarning)
        return colorizeWord("WARN", "33", colorize);
    return colorizeWord("OK", "32", colorize);
}

static QString findingSeverityTag(const Finding& f)
{
    if (f.fixed)
        return "fixed";
    if (f.skipped)
        return "skipped";
    switch (f.severity) {
    case Severity::Error:   return "error";
    case Severity::Warning: return "warn";
    case Severity::Info:    return "info";
    }
    return "info";
}

static QString formatScanAssetLine(const AssetInfo& asset, const QList<Finding>& findings, bool colorize)
{
    bool hasError = false;
    bool hasWarning = false;
    for (const auto& f : findings) {
        if (f.fixed)
            continue;
        if (f.skipped) {
            continue;
        }
        if (f.severity == Severity::Error)
            hasError = true;
        else if (f.severity == Severity::Warning)
            hasWarning = true;
    }

    QString out;
    QTextStream s(&out);
    const QString status = scanStatusLabel(hasError, hasWarning, colorize);
    if (!hasError && !hasWarning)
        s << "  " << status << "    " << asset.relativePath << "\n";
    else if (hasWarning)
        s << status << "    " << asset.relativePath << "\n";
    else
        s << status << "   " << asset.relativePath << "\n";

    for (const auto& f : findings) {
        s << "         [" << findingSeverityTag(f) << "] "
          << f.rule << ": " << f.message << "\n";
    }
    return out;
}

static QString formatScanSummary(const ScanResult& result, bool colorize)
{
    QString out;
    QTextStream s(&out);

    const QString passIcon = colorizeIconWhenPositive(QStringLiteral("✓"), result.passed, "32", colorize);
    const QString warnIcon = colorizeIconWhenPositive(QStringLiteral("▲"), result.warnings, "33", colorize);
    const QString errorIcon = colorizeIconWhenPositive(QStringLiteral("✗"), result.errors, "31", colorize);
    const QString infoIcon = colorizeIconWhenPositive(QStringLiteral("ℹ"), result.infos, "36", colorize);
    const QString fixedIcon = colorizeIconWhenPositive(QStringLiteral("🔧"), result.fixed, "32", colorize);
    const QString savedIcon = colorizeIconWhenPositive(QStringLiteral("📉"), result.bytesSaved > 0 ? 1 : 0, "32", colorize);
    const QString keysIcon = colorizeIconWhenPositive(QStringLiteral("🧹"), result.keysRemoved > 0 ? 1 : 0, "32", colorize);
    const QString skippedIcon = colorizeWord(QStringLiteral("⏭"), "90", colorize);
    const QString timeIcon = colorizeWord(QStringLiteral("⏱"), "34", colorize);

    s << "\n";
    s << "Summary:\n";
    s << "  • Scanned:  " << result.scanned  << "\n";
    s << "  " << passIcon << " Passed:   " << result.passed   << "\n";
    s << "  " << warnIcon << " Warnings: " << result.warnings << "\n";
    s << "  " << errorIcon << " Errors:   " << result.errors   << "\n";
    if (result.infos > 0)
        s << "  " << infoIcon << " Info:     " << result.infos << "\n";
    if (result.fixed > 0)
        s << "  " << fixedIcon << " Fixed:    " << result.fixed << "\n";
    if (result.bytesSaved > 0)
        s << "  " << savedIcon << " Saved:    " << QString::number(result.bytesSaved / (1024.0 * 1024.0), 'f', 2) << " MB\n";
    if (result.keysRemoved > 0) {
        const QString n = QLocale::system().toString(result.keysRemoved);
        s << "  " << keysIcon << " Keys removed: " << n << "\n";
    }
    if (result.skipped > 0)
        s << "  " << skippedIcon << " Skipped:  " << result.skipped << "\n";
    s << "  " << timeIcon << " Time:     " << QString::number(result.elapsedMs / 1000.0, 'f', 1) << "s\n";
    QString utcStart, utcEnd;
    ScanEngine::scanReportUtcTimes(result, &utcStart, &utcEnd);
    s << "  UTC start:  " << utcStart << "\n";
    s << "  UTC end:    " << utcEnd << "\n";
    return out;
}

static QTextStream& err()
{
    static QTextStream s(stderr);
    return s;
}

/// Ingest token: `--token` overrides `QTMESH_TOKEN`, then `QTMESH_CLOUD_TOKEN`.
static QString resolveIngestToken(const QString& flagToken)
{
    const QString trimmed = flagToken.trimmed();
    if (!trimmed.isEmpty())
        return trimmed;
    const QByteArray a = qgetenv("QTMESH_TOKEN");
    if (!a.isEmpty())
        return QString::fromUtf8(a);
    const QByteArray b = qgetenv("QTMESH_CLOUD_TOKEN");
    if (!b.isEmpty())
        return QString::fromUtf8(b);
    return {};
}

#ifndef QTMESH_CLOUD_WEB_URL
#define QTMESH_CLOUD_WEB_URL "https://qtmesh.dev"
#endif

/// One-line nudge pointing users at QtMesh Cloud so they can track
/// historical scan results and mesh info. Skipped when a token is already
/// configured (the user has signed up — passes flagToken so the per-command
/// `--token` value short-circuits the same as env vars), or when the caller
/// is emitting machine-readable JSON we mustn't pollute.
static void maybePrintCloudPromo(bool jsonOutput, const QString& flagToken = {})
{
    if (jsonOutput) return;
    if (!resolveIngestToken(flagToken).isEmpty()) return;
    err() << "Tip: sign up at " << QTMESH_CLOUD_WEB_URL
          << " to track your scans and view results in the dashboard."
          << Qt::endl;
}

static bool s_verbose = false;
static bool s_noTelemetry = false;

/// Suppress qDebug/qInfo/qWarning in non-verbose mode.
/// qCritical and qFatal always pass through.
static void cliMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    Q_UNUSED(ctx);
    if (s_verbose || type == QtCriticalMsg || type == QtFatalMsg) {
        fprintf(stderr, "%s\n", qPrintable(msg));
    }
}

/// Redirect stdout to stderr so Ogre/Qt debug output doesn't pollute
/// CLI output. Actual CLI output uses the saved original stdout fd.
static void redirectStdout()
{
#ifndef Q_OS_WIN
    s_savedStdoutFd = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
#endif
    qInstallMessageHandler(cliMessageHandler);
}

void CLIPipeline::printVersion()
{
    cliWrite(QString("qtmesh %1\n").arg(QTMESHEDITOR_VERSION));
}

void CLIPipeline::printUsage()
{
    cliWrite(
        "Usage: qtmesh <command> [options]\n"
        "\n"
        "Commands:\n"
        "  info <file> [--json]              Show mesh information\n"
        "  fix <file> [-o <output>] [flags]   Fix/optimize a mesh (overwrites input if no -o)\n"
        "  convert <file> -o <output>        Convert between 3D formats\n"
        "  anim <file> --list [--json]       List animations\n"
        "  anim <file> --rename <old> <new> [-o <output>]\n"
        "                                    Rename an animation (overwrites input if no -o)\n"
        "  anim <file> --merge <f1> [f2...] [-o <output>]\n"
        "                                    Merge animations from other files into base\n"
        "                                    (overwrites input if no -o)\n"
        "  anim <file> --resample N [-o <output>] [--animation <name>]\n"
        "                                    Resample to exactly N evenly-spaced keyframes\n"
        "  anim <file> --decimate-step S [-o <output>] [--animation <name>]\n"
        "                                    Keep every Sth keyframe (plus first and last)\n"
        "  anim <file> --simplify [--preset {conservative|balanced|aggressive}] [--tolerance T] [--rotation-tolerance-deg D] [-o <output>] [--animation <name>]\n"
        "                                    Remove redundant keyframes (tolerance-based, preserves sharp keys)\n"
        "                                    Default preset: conservative (~0.1mm / 0.05°) — destructive, so the safe default.\n"
        "                                    Use --preset balanced / aggressive for more aggressive reduction.\n"
        "                                    --tolerance T sets translation+scale tolerance (world units)\n"
        "  anim <file> --bake-fps N [-o <output>] [--animation <name>]\n"
        "                                    Re-grid to exactly N keyframes per second (uniform)\n"
        "                                    Common values: 10, 15, 30, 60. Mixamo / pipeline export.\n"
        "  anim <file> --analyze [--json] [--preset ...] [--tolerance T] [--rotation-tolerance-deg D]\n"
        "                                    Report % redundant keyframes and projected file-size savings\n"
        "  validate <file> [--json]          Validate mesh geometry (exit 1 if errors found)\n"
        "  lod <file> --count N [--reductions r,...] [--algo ogre|meshopt] [-o output]\n"
        "                                    Generate N LOD levels; exports <base>_lod1.<ext> etc.\n"
        "                                    --algo: ogre (default) | meshopt (preserves UV seams + skin weights)\n"
        "  lod <file> --auto [-o output]     Auto-generate LOD levels (Ogre backend)\n"
        "  lod <file> --remove [-o output]   Remove LOD levels (overwrites input if no -o)\n"
        "  lod <file> --info [--json]        Show LOD level info\n"
        "  pose <file> --animation <name> --time <t> -o <output>\n"
        "                                    Export a single posed frame as static mesh\n"
        "  pose <file> --animation <name> --count N -o <pattern>\n"
        "                                    Export N evenly spaced frames (use %02d in pattern)\n"
        "  pose <library.poselib> --library list [--json]\n"
        "                                    List pose names in a `.poselib` sidecar JSON file.\n"
        "                                    No mesh load needed; reads the file directly.\n"
        "  pose <mesh> --library apply --lib <lib.poselib> --apply <name> -o <out>\n"
        "                                    Load mesh, apply named pose from sidecar to the\n"
        "                                    skeleton, export the posed mesh. Requires a skinned mesh.\n"
        "  turntable <file> -o <output> [--frames N] [--size WxH] [--columns C]\n"
        "                                    Render a PNG turntable (default: horizontal sprite sheet)\n"
        "  turntable <file> -o <pattern>     Use %02d in -o to write separate frame PNGs\n"
        "                                    Options: --axis y|x|z, --elevation/--camera-height <deg>,\n"
        "                                    --width/--height, --json\n"
        "  isometric <file> -o <output> [--directions N] [--frames N] [--animation NAME]\n"
        "                                    8-direction isometric sprite grid (rows=directions,\n"
        "                                    cols=animation frames). Static mesh when no animation.\n"
        "                                    Options: --elevation/--camera-height <deg>, --size WxH,\n"
        "                                    --resolution N, --width/--height, --start-azimuth <deg>,\n"
        "                                    --camera-distance N, --padding F, --json\n"
        "  scan [path] [options]           Scan directory for 3D asset issues (default path: .)\n"
        "  material <file> --preset <name> [-o <output>]\n"
        "                                  Apply a built-in material preset to every sub-entity\n"
        "                                  (Plastic/Metal/Wood/Glass/Unlit/Wireframe + PBR templates:\n"
        "                                  Metallic-Roughness, Specular-Glossiness, Unlit PBR)\n"
        "  material --list-presets         List the built-in preset names\n"
        "  material <file> --generate-texture \"<prompt>\" [--model <name>]\n"
        "                  [--controlnet <path>] [--controlnet-strength <0..1>]\n"
        "                  [--width N] [--height N] [-o <output>]\n"
        "                                  AI mesh-aware (depth-conditioned) texture\n"
        "                                  generation; binds result as diffuse and\n"
        "                                  re-exports (needs an SD build + base model;\n"
        "                                  run 'uv --unwrap' first if the mesh lacks UVs)\n"
        "  material --texture <low.png> --upscale {2|4} [-o <high.png>]\n"
        "                                  AI super-resolution (Real-ESRGAN); 2x/4x\n"
        "                                  (needs an ONNX build + first-run model download)\n"
        "  material --texture <albedo.png> --generate-pbr [<mesh>] [-o <output>]\n"
        "                  [--tile-size N] [--no-normal] [--no-roughness] [--no-height]\n"
        "                                  AI PBR map synthesis (normal/roughness/height)\n"
        "                                  from a diffuse texture; writes maps next to it,\n"
        "                                  and if <mesh> given binds them + re-exports\n"
        "                                  (needs an ONNX build + first-run model download)\n"
        "\n"
        "Scan options:\n"
        "  --target <id>             Alias for --profile (CI-friendly). Built-in: ps1, n64, nds, dreamcast,\n"
        "                            modern-console, switch-like, steamdeck, mobile-low, webgl, vr, example-minimal\n"
        "  --profile <id>            Built-in platform profile (e.g. mobile-low) or path to .json\n"
        "  --list-profiles           List built-in platform profile ids and exit\n"
        "                            Profiles validate glTF/FBX source budgets — not engine/cooked export.\n"
        "  --config <file>           Config file (default: qtmesh.yml, qtmesh.json)\n"
        "  --json                    Output as JSON\n"
        "  --report <file>           Write JSON report to file\n"
        "  --sarif <file>            Write SARIF report to file\n"
        "  --fix                     Enable auto-fixes\n"
        "  --dry-run                 Show what fixes would be applied\n"
        "  --include <patterns>      File patterns, comma-separated (e.g. *.fbx,*.glb)\n"
        "  --exclude <patterns>      Exclude patterns, comma-separated\n"
        "  --allowed-formats <list>  Allowed formats CSV (e.g. fbx,glb,obj)\n"
        "  --forbidden-extensions <list> Forbidden formats CSV\n"
        "  --max-file-size-mb <n>    Override max_file_size_mb (0 = no limit)\n"
        "  --min-file-size-mb <n>    Override min_file_size_mb (0 = no limit)\n"
        "  --max-meshes <n>          Override max_mesh_count (0 = no limit)\n"
        "  --min-meshes <n>          Override min_mesh_count (0 = no limit)\n"
        "  --max-materials <n>       Override max_material_count (0 = no limit)\n"
        "  --min-materials <n>       Override min_material_count (0 = no limit)\n"
        "  --max-vertices <n>        Override max_vertex_count (0 = no limit)\n"
        "  --min-vertices <n>        Override min_vertex_count (0 = no limit)\n"
        "  --max-triangles <n>       Override max_triangle_count (0 = no limit)\n"
        "  --max-triangles-per-mesh <n>  Override max_triangles_per_mesh (0 = no limit)\n"
        "  --max-bones <n>           Override max_bones (0 = no limit)\n"
        "  --max-submeshes <n>       Override max_submesh_count (0 = no limit)\n"
        "  --max-draw-calls <n>      Override max_draw_calls (0 = no limit)\n"
        "  --max-acmr <n>            Override max_acmr (0 = no limit, e.g. 1.5)\n"
        "  --require-skeleton / --no-require-skeleton\n"
        "                            Override require_skeleton\n"
        "  --require-animations / --no-require-animations\n"
        "                            Override require_animations\n"
        "  --allow-embedded-textures / --disallow-embedded-textures\n"
        "                            Override allow_embedded_textures\n"
        "  --require-textures-exist / --no-require-textures-exist\n"
        "                            Override require_textures_exist\n"
        "  --allow-missing-materials / --disallow-missing-materials\n"
        "                            Override allow_missing_materials\n"
        "  --file-name-case <name>   snake_case, kebab-case, camelCase, PascalCase, lowercase\n"
        "  --max-anim-keyframes <n>  Override max_anim_keyframes (0 = no limit)\n"
        "  --min-anim-keyframes <n>  Override min_anim_keyframes (0 = no limit)\n"
        "  --max-anim-duration <n>   Override max_anim_duration seconds (0 = no limit)\n"
        "  --min-anim-duration <n>   Override min_anim_duration seconds (0 = no limit)\n"
        "  --require-animation-names <list> Required animation names/patterns CSV\n"
        "  --require-bone-names <list> Required bone names/patterns CSV\n"
        "\n"
        "  Quality / budget rules (config or --target profile — set in qtmesh.yml):\n"
        "    max_triangle_count: <n>           File-level triangle budget\n"
        "    max_triangles_per_mesh: <n>       Per-mesh triangle ceiling\n"
        "    max_bones: <n>                    Skeleton bone budget\n"
        "    max_submesh_count: <n>            Submesh / material-split ceiling\n"
        "    max_draw_calls: <n>               Estimated draw-call ceiling\n"
        "    max_texture_resolution: <px>      Largest texture edge ceiling (e.g. 2048)\n"
        "    max_texture_dimension: <px>       Alias for max_texture_resolution\n"
        "    texture_not_power_of_two: true     Warn on non-POT textures (needs inspect_textures)\n"
        "    allowed_texture_formats: [png,jpg]  Texture extension allow-list\n"
        "    disallowed_texture_formats: [tga]   Blocklisted texture extensions\n"
        "    require_uv_channels: <n>          Min UV sets per submesh (1=any, 2=lightmap)\n"
        "    detect_zero_weight_bones: true    Flag Mixamo-style unused bones (info)\n"
        "    detect_overlapping_uvs_pct: <n>   Warn at >= n% overlapping UV0 AABBs\n"
        "    detect_non_manifold_edges_pct: <n>  Warn at >= n% non-manifold edges\n"
        "    redundant_keyframes_pct: <n>      Warn at >= n% redundant anim keys (fixable)\n"
        "\n"
        "  --fail-on <level>         Exit 1 threshold: info, warning, error, never\n"
        "  --token <token>           Ingest token (overrides QTMESH_TOKEN / QTMESH_CLOUD_TOKEN)\n"
        "  --no-upload               Skip POSTing scan JSON to QtMesh Cloud when a token is set\n"
        "  --strict-upload           Exit 1 if cloud upload fails (default: warn only)\n"
        "\n"
        "  Cloud rules: if no --config and QTMESH_TOKEN or --token is set, remote rules are\n"
        "  fetched first (local qtmesh.yml is ignored). If the API fails, built-in defaults apply.\n"
        "  Without a token, qtmesh.yml|yaml|json in the cwd is used if present.\n"
        "  Config precedence: defaults → platform profile → project config → CLI flags.\n"
        "  Set profile in qtmesh.yml with `profile: <id>` or pass --profile (CLI wins).\n"
        "  --config always wins over cloud rules. Scan JSON uploads when a token is set (unless --no-upload).\n"
        "  Override API base with QTMESH_API_BASE.\n"
        "\n"
        "Fix flags:\n"
        "  --remove-degenerates  Remove degenerate triangles\n"
        "  --merge-materials     Remove redundant materials\n"
        "  --all                 Apply all extra fixes\n"
        "  (no flags)            Standard import/export (joins vertices, smooths normals, optimizes)\n"
        "\n"
        "  memory <file> [--json] [--budget <size>] [--token <t>] [--no-cloud]\n"
        "                                    Report per-mesh GPU bytes and per-texture VRAM bytes\n"
        "                                    --budget accepts e.g. 50MB, 1GB; exit 1 if exceeded\n"
        "                                    If --budget omitted and a token is set, the project's\n"
        "                                    memory_budget_mb is fetched from QtMesh Cloud rules.\n"
        "                                    --no-cloud opts out.\n"
        "  analyze <file> [--json]           Analyze draw calls: per-material grouping plus\n"
        "                                    merge suggestions for entities sharing a material.\n"
        "  vertex-cache <file> [-o <output>] [--json]\n"
        "                                    Reorder index buffers via Forsyth's algorithm; reports\n"
        "                                    before/after ACMR. Without -o, only analyzes (read-only).\n"
        "  decimate <file> -o <output> (--reduction <r> | --target-tris N | --target-verts N) [--algo ogre|meshopt] [--json]\n"
        "                                    Single-pass mesh decimation. Choose one target:\n"
        "                                    --reduction 0.5 (drop half the triangles),\n"
        "                                    --target-tris 5000, or --target-verts 2500.\n"
        "  atlas --inputs <csv> -o <atlas.png> [--size N] [--width N --height N] [--padding N]\n"
        "                                    [--manifest <atlas.json>]\n"
        "                                    Pack N textures into a single atlas + JSON manifest of\n"
        "                                    per-tile UV remaps. Shelf bin-pack; deterministic. Useful\n"
        "                                    for consolidating per-prop textures into one binding to\n"
        "                                    reduce GPU draw-call count.\n"
        "  atlas-apply <file> -o <output> --manifest <atlas.json> --atlas <atlas.png>\n"
        "                                    [--match {basename|fullpath}] [--no-clamp]\n"
        "                                    [--keep-extras] [--json]\n"
        "                                    Apply a previously-packed atlas to a mesh: rewrite UV0\n"
        "                                    into each tile's sub-rect and rebind the diffuse TUS to the\n"
        "                                    atlas texture. By default normal/AO/emissive TUSes are\n"
        "                                    stripped from affected materials because they sample UV0,\n"
        "                                    which is now diffuse-atlas-relative — pass --keep-extras\n"
        "                                    when you have pre-atlased auxiliary maps to match.\n"
        "                                    Counterpart to `atlas`.\n"
        "  optimize <file> -o <output> [flags] [--json]\n"
        "                                    Batch-optimize a single asset. Defaults to\n"
        "                                    --vertex-cache --simplify-anim when no flags are given.\n"
        "                                    Add --reduction <r> / --target-tris N / --target-verts N\n"
        "                                    to also decimate. --all enables vertex-cache + simplify-anim\n"
        "                                    together (decimation still requires an explicit target).\n"
        "                                    Anim simplify tolerances (Conservative preset by default —\n"
        "                                    simplify is destructive, so the safe choice. Use\n"
        "                                    larger values for Balanced (1e-3 / 0.5° / 1e-3) or\n"
        "                                    Aggressive (1e-2 / 1° / 1e-2) reduction):\n"
        "                                      --simplify-translation-tol T   default 0.0001 (world units, ~0.1mm)\n"
        "                                      --simplify-rotation-deg-tol D  default 0.05   (degrees)\n"
        "                                      --simplify-scale-tol S         default 0.0001\n"
        "                                      --simplify-preset P            shorthand for the three tolerances;\n"
        "                                                                     P = conservative | balanced | aggressive\n"
        "  bake-vertex-colors <file> -o <out.png> [--resolution N] [--dilation N] [--json]\n"
        "                                    Bake vertex colors to a UV-space PNG. Walks every UV-mapped\n"
        "                                    triangle, rasterizes barycentric-interpolated vertex colors,\n"
        "                                    then dilates outward by N pixels to mask seam bleed at MIP time.\n"
        "                                    Default resolution=1024, dilation=4. Output PNG is RGBA.\n"
        "  vat <file> --anim <name> [--fps N] [-o <dir>] [--include-shaders {godot,unity,unreal,all}] [--emit-uv2 [N]] [--bake-precision {16,32}] [--json]\n"
        "                                    Bake a skeletal animation into a Vertex Animation Texture\n"
        "                                    in OpenVAT (sharpen3d/openvat) format: a single 16-bit RGB\n"
        "                                    PNG (height = 2 × frames; top half positions, bottom half\n"
        "                                    normals) plus `<basename>-remap_info.json` with the\n"
        "                                    canonical `os-remap` sidecar shape. Off-the-shelf openvat\n"
        "                                    reference shaders for Godot / Unity / Unreal / Blender\n"
        "                                    consume the output unmodified. Drop-in shader templates\n"
        "                                    for Godot/Unity/Unreal live at `tools/vat-shaders/`.\n"
        "  uv <file> --unwrap [--resolution N] [--padding P] [--channel C] [--no-backup] -o <out>\n"
        "                                    Auto UV-unwrap via xatlas (used by Blender / Godot). Writes\n"
        "                                    non-overlapping UVs into the chosen channel (default 0) and\n"
        "                                    exports. The original UVs on the target channel are kept on\n"
        "                                    UV{C+1} unless --no-backup is set. Skin weights survive the\n"
        "                                    seam-split remap.\n"
        "  uv <file> --info [--json]         Report current UV channels + UV0 bounding-box coverage per\n"
        "                                    submesh without mutating the mesh.\n"
        "  retopo <file> [--target-faces N] [--max-angle DEG] [--shape-tol DEG] [--max-aspect R] -o <out> [--json]\n"
        "                                    Quad-dominant retopology via triangle pairing. Pairs adjacent\n"
        "                                    triangles into convex quads where coplanarity + shape + aspect-ratio\n"
        "                                    gates pass. Writes quads via the n-gon binding so the FBX / glTF\n"
        "                                    exporter round-trips them. No new vertices are introduced — UVs\n"
        "                                    and skin weights survive unchanged.\n"
        "  skin <file> [--max-influences N] [--falloff F] [--max-distance D] [--skip-unweighted] [--merge] -o <out> [--json]\n"
        "                                    Compute skin weights via inverse-distance heuristic (closest-point-on-\n"
        "                                    bone smooth bind). Mesh must have a skeleton attached. Bones with no\n"
        "                                    existing weights can be filtered with --skip-unweighted. --merge\n"
        "                                    keeps existing weights instead of replacing them.\n"
        "  morph <file> --list [--json]      List morph targets / blend shapes on a mesh. (Set/add/delete\n"
        "                                    land in follow-up slices once authoring is in place.)\n"
        "  nodeanim <file> --list [--json]   List node-animation clips on a scene (props, doors, machinery,\n"
        "                                    animated lights — anything non-skeletal). Authoring on the CLI\n"
        "                                    side needs the C5 glTF/FBX exporter round-trip first.\n"
        "\n"
        "  cloud login [--api-key <token>]   Sign in via device flow (prints URL + code) or store an API key.\n"
        "  cloud logout                      Sign out and clear the saved session.\n"
        "  cloud status [--json]             Show whether a QtMesh Cloud session is stored locally.\n"
        "  cloud limits [--json]             Print server-reported upload size limits.\n"
        "  cloud list [--json]               List cloud projects for the signed-in account.\n"
        "  cloud upload <file> [--name <n>] [--include \"<glob>,...\"] [--exclude \"<glob>,...\"]\n"
        "                                    [--no-scan] [--no-confirm] [--json]\n"
        "                                    Package the asset + dependencies and upload to QtMesh Cloud.\n"
        "                                    After files/complete, uploads the local scan report to the main file.\n"
        "  cloud delete <project-id>         Delete a cloud project by id.\n"
        "\n"
        "Global options:\n"
        "  --help, -h            Show this help\n"
        "  --version, -v         Show version\n"
        "  --verbose             Show Ogre/engine debug output\n"
        "  --no-telemetry        Permanently disable anonymous usage data\n"
    );
}

QString CLIPipeline::formatForExtension(const QString& path)
{
    struct ExtensionFormat {
        const char* extension;
        const char* format;
    };
    static const ExtensionFormat extensionFormats[] = {
        {".fbx", "FBX Binary (*.fbx)"},
        {".glb", "glTF 2.0 Binary (*.glb)"},
        {".glb2", "glTF 2.0 Binary (*.glb2)"},
        {".gltf", "glTF 2.0 (*.gltf)"},
        {".gltf2", "glTF 2.0 (*.gltf2)"},
        {".vrm", "VRM / glTF 2.0 (*.vrm)"},
        {".dae", "Collada (*.dae)"},
        {".obj", "OBJ (*.obj)"},
        {".stl", "STL (*.stl)"},
        {".ply", "PLY (*.ply)"},
        {".3ds", "3DS (*.3ds)"},
        {".x", "X (*.x)"},
        {".mesh.xml", "Ogre XML (*.mesh.xml)"},
        {".mesh", "Ogre Mesh (*.mesh)"},
        {".assbin", "Assimp Binary (*.assbin)"},
        {".tmd", "PlayStation TMD (*.tmd)"},
        {".rsd", "PlayStation RSD (*.rsd)"}
    };

    for (const ExtensionFormat& entry : extensionFormats) {
        if (path.endsWith(QString::fromLatin1(entry.extension), Qt::CaseInsensitive)) {
            return QString::fromLatin1(entry.format);
        }
    }

    return "Ogre Mesh (*.mesh)";
}

namespace {

bool parseCliInt(const QString& text, int* out)
{
    bool ok = false;
    const int v = text.toInt(&ok);
    if (ok && out)
        *out = v;
    return ok;
}

bool parseCliFloat(const QString& text, float* out)
{
    bool ok = false;
    const float v = text.toFloat(&ok);
    if (ok && out)
        *out = v;
    return ok;
}

bool turntableUsesFrameSequencePattern(const QString& path)
{
    for (int i = 0; i < path.size(); ++i) {
        if (path[i] != QLatin1Char('%'))
            continue;
        if (i + 1 < path.size() && path[i + 1] == QLatin1Char('%')) {
            ++i;
            continue;
        }
        return true;
    }
    return false;
}

bool validateTurntableSequencePattern(const QString& pattern, QString* errorOut)
{
    int conversions = 0;
    for (int i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != QLatin1Char('%'))
            continue;
        if (i + 1 < pattern.size() && pattern[i + 1] == QLatin1Char('%')) {
            ++i;
            continue;
        }
        ++conversions;
        if (conversions > 1) {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "Output pattern must contain exactly one frame index (use %% for a literal %)");
            return false;
        }
        ++i;
        while (i < pattern.size()
               && (pattern[i].isDigit() || pattern[i] == QLatin1Char('.') || pattern[i] == QLatin1Char('*')
                   || pattern[i] == QLatin1Char('-')))
            ++i;
        if (i >= pattern.size()) {
            if (errorOut)
                *errorOut = QStringLiteral("Output pattern ends with an incomplete conversion");
            return false;
        }
        const QChar spec = pattern[i];
        if (spec != QLatin1Char('d') && spec != QLatin1Char('i') && spec != QLatin1Char('u')) {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "Output pattern frame index must use %%d (for example frame_%%02d.png)");
            return false;
        }
    }
    if (conversions != 1) {
        if (errorOut)
            *errorOut = QStringLiteral(
                "Sequence output requires exactly one frame index (for example frame_%%02d.png)");
        return false;
    }
    return true;
}

QString formatTurntableFramePath(const QString& pattern, int frameIndex)
{
    char buf[2048];
    snprintf(buf, sizeof(buf), pattern.toUtf8().constData(), frameIndex);
    return QString::fromUtf8(buf);
}

struct IsometricCliParams {
    QString inputPath;
    QString outputPath;
    QString animationName;
    int frameCount = 1;
    bool frameCountExplicit = false;
    int directionCount = 8;
    int width = 512;
    int height = 512;
    float elevation = 30.0f;
    float startAzimuth = 0.0f;
    float cameraDistance = 0.0f;
    float cameraPadding = 1.25f;
    bool jsonOutput = false;
};

/// @return 0 on success, 2 on usage error.
int parseIsometricCliArgs(int argc, char *argv[], IsometricCliParams *out)
{
    if (!out)
        return 2;

    for (int i = 1; i < argc; ++i) {
        const QString arg(argv[i]);
        if (arg == "isometric" || arg == "--cli")
            continue;
        if (arg == "--json") {
            out->jsonOutput = true;
            continue;
        }
        if (arg == "-o" && i + 1 < argc) {
            out->outputPath = QString(argv[++i]);
            continue;
        }
        if (arg == "--animation" && i + 1 < argc) {
            out->animationName = QString(argv[++i]);
            continue;
        }
        if (arg == "--frames" && i + 1 < argc) {
            if (!parseCliInt(QString(argv[++i]), &out->frameCount) || out->frameCount <= 0) {
                err() << "Error: --frames must be a positive integer." << Qt::endl;
                return 2;
            }
            out->frameCountExplicit = true;
            continue;
        }
        if (arg == "--directions" && i + 1 < argc) {
            if (!parseCliInt(QString(argv[++i]), &out->directionCount) || out->directionCount <= 0) {
                err() << "Error: --directions must be a positive integer." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--resolution" && i + 1 < argc) {
            int res = 0;
            if (!parseCliInt(QString(argv[++i]), &res) || res < 16 || res > 8192) {
                err() << "Error: --resolution must be an integer in [16..8192]." << Qt::endl;
                return 2;
            }
            out->width = res;
            out->height = res;
            continue;
        }
        if (arg == "--width" && i + 1 < argc) {
            if (!parseCliInt(QString(argv[++i]), &out->width)) {
                err() << "Error: Invalid value for --width." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--height" && i + 1 < argc) {
            if (!parseCliInt(QString(argv[++i]), &out->height)) {
                err() << "Error: Invalid value for --height." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--size" && i + 1 < argc) {
            const auto sizeArg = QString(argv[++i]);
            if (const int xPos = static_cast<int>(sizeArg.indexOf(QLatin1Char('x'))); xPos > 0) {
                if (!parseCliInt(sizeArg.left(xPos), &out->width)
                    || !parseCliInt(sizeArg.mid(xPos + 1), &out->height)) {
                    err() << "Error: Invalid value for --size (expected WxH)." << Qt::endl;
                    return 2;
                }
            } else if (!parseCliInt(sizeArg, &out->width)) {
                err() << "Error: Invalid value for --size." << Qt::endl;
                return 2;
            } else {
                out->height = out->width;
            }
            continue;
        }
        if (arg == "--elevation" && i + 1 < argc) {
            if (!parseCliFloat(QString(argv[++i]), &out->elevation)) {
                err() << "Error: Invalid value for --elevation." << Qt::endl;
                return 2;
            }
            continue;
        }
        if ((arg == "--camera-height" || arg == "--camera_height") && i + 1 < argc) {
            if (!parseCliFloat(QString(argv[++i]), &out->elevation)) {
                err() << "Error: Invalid value for --camera-height." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--start-azimuth" && i + 1 < argc) {
            if (!parseCliFloat(QString(argv[++i]), &out->startAzimuth)) {
                err() << "Error: Invalid value for --start-azimuth." << Qt::endl;
                return 2;
            }
            continue;
        }
        if ((arg == "--camera-distance" || arg == "--camera_distance") && i + 1 < argc) {
            if (!parseCliFloat(QString(argv[++i]), &out->cameraDistance) || out->cameraDistance <= 0.0f) {
                err() << "Error: --camera-distance must be a positive number." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--padding" && i + 1 < argc) {
            if (!parseCliFloat(QString(argv[++i]), &out->cameraPadding) || out->cameraPadding <= 0.0f) {
                err() << "Error: --padding must be a positive number." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (!arg.startsWith(QLatin1Char('-')) && out->inputPath.isEmpty()) {
            out->inputPath = arg;
            continue;
        }
    }

    if (out->inputPath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh isometric <file> -o <output> [--directions N] [--frames N]" << Qt::endl;
        return 2;
    }
    if (out->outputPath.isEmpty()) {
        err() << "Error: Output path required (-o)." << Qt::endl;
        err() << "Usage: qtmesh isometric <file> -o <output.png> [--directions 8]" << Qt::endl;
        return 2;
    }

    if (!out->animationName.isEmpty() && !out->frameCountExplicit)
        out->frameCount = 8;

    return 0;
}

} // namespace

bool CLIPipeline::initOgreHeadless()
{
    // Suppress Ogre log output unless --verbose was given.
    // Creating our own LogManager before Root prevents Root from
    // creating a default one that writes to ogre.log and stdout.
    if (!s_verbose) {
        if (!Ogre::LogManager::getSingletonPtr()) {
            auto* logMgr = new Ogre::LogManager();
            logMgr->createLog("ogre.log", true, false, true); // default, debugOut=false, suppressFile=true
        } else {
            Ogre::LogManager::getSingleton().getDefaultLog()->setDebugOutputEnabled(false);
        }
    }

    try {
        Manager::getSingleton();
    } catch (...) {
        err() << "Error: Failed to initialize Ogre." << Qt::endl;
        return false;
    }

    auto* root = Manager::getSingleton()->getRoot();
    bool hasRenderWindow = false;
    if (root) {
        try {
            if (root->getRenderTarget("TestHidden") || root->getRenderTarget("CLIHidden"))
                hasRenderWindow = true;
        } catch (...) {
            // getRenderTarget may throw if not found in some Ogre versions
        }
    }

    if (!hasRenderWindow) {
        static QWidget* hiddenWidget = nullptr;
        if (!hiddenWidget) {
            hiddenWidget = new QWidget();
            hiddenWidget->setAttribute(Qt::WA_DontShowOnScreen);
            hiddenWidget->resize(1, 1);
            hiddenWidget->show();
        }

        try {
            Ogre::NameValuePairList params;
            params["externalWindowHandle"] = Ogre::StringConverter::toString(
                static_cast<unsigned long>(hiddenWidget->winId()));
#ifdef Q_OS_MACOS
            params["macAPI"] = "cocoa";
            params["macAPICocoaUseNSView"] = "true";
#endif
            Manager::getSingleton()->getRoot()->createRenderWindow(
                "CLIHidden", 1, 1, false, &params);
        } catch (...) {
            err() << "Error: Failed to create render window." << Qt::endl;
            return false;
        }
    }

    // Match GUI startup: RTSS and media need a GL context (MainWindow calls
    // loadResources() after createRenderWindow). Without this, CLI turntable
    // renders MSN_SHADERGEN with normal maps still in the FFP multitexture chain.
    static bool cliResourcesLoaded = false;
    if (!cliResourcesLoaded) {
        Manager::getSingleton()->loadResources();
        cliResourcesLoaded = true;
    }
    return true;
}

MeshInfo CLIPipeline::extractMeshInfo(const Ogre::Entity* entity, const QString& fileName)
{
    MeshInfo info;
    info.file = fileName;

    if (!entity) return info;

    const Ogre::MeshPtr& mesh = entity->getMesh();
    if (!mesh) return info;

    info.submeshes = mesh->getNumSubMeshes();

    // Count vertices + per-submesh ACMR (issue #399). ACMR uses
    // ExportOptimizer::computeAcmr which routes through
    // meshopt_analyzeVertexCache with the 32-entry cache size that
    // matches `qtmesh vertex-cache` output.
    if (mesh->sharedVertexData)
        info.vertices += mesh->sharedVertexData->vertexCount;
    for (unsigned int i = 0; i < info.submeshes; ++i) {
        Ogre::SubMesh* sub = mesh->getSubMesh(i);
        if (sub->vertexData)
            info.vertices += sub->vertexData->vertexCount;
        if (sub->indexData)
            info.triangles += sub->indexData->indexCount / 3;

        // Compute ACMR for this submesh. Skipped on zero-index buffers.
        if (sub->indexData && sub->indexData->indexCount > 0) {
            const auto* vdata = sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
            if (vdata) {
                auto indices = [&]() {
                    std::vector<uint32_t> out(sub->indexData->indexCount);
                    auto ibuf = sub->indexData->indexBuffer;
                    auto* src = static_cast<unsigned char*>(
                        ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                    src += sub->indexData->indexStart * ibuf->getIndexSize();
                    if (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_16BIT) {
                        const auto* p = reinterpret_cast<const uint16_t*>(src);
                        for (size_t k = 0; k < out.size(); ++k) out[k] = p[k];
                    } else {
                        std::memcpy(out.data(), src, out.size() * sizeof(uint32_t));
                    }
                    ibuf->unlock();
                    return out;
                }();
                MeshInfo::SubmeshAcmr sa;
                sa.submeshIndex  = static_cast<int>(i);
                sa.triangleCount = static_cast<int>(sub->indexData->indexCount / 3);
                sa.acmr          = ExportOptimizer::computeAcmr(indices, vdata->vertexCount);
                info.submeshAcmr.append(sa);
            }
        }
    }

    // Materials
    std::set<std::string, std::less<>> seenMats;
    for (unsigned int i = 0; i < entity->getNumSubEntities(); ++i) {
        Ogre::SubEntity* subEnt = entity->getSubEntity(i);
        if (subEnt && subEnt->getMaterial()) {
            auto name = subEnt->getMaterial()->getName();
            if (seenMats.insert(name).second)
                info.materials << QString::fromStdString(name);
        }
    }

    // Textures. The straightforward pass walks every CONTENT_NAMED
    // TextureUnitState. That covers diffuse/albedo/metallic/roughness/ao/
    // emissive — every PBR slot MaterialProcessor binds as a plain TUS.
    // It does NOT cover the normal map: MaterialProcessor wires that one
    // through RTSS's render-state side channel (see RTShaderHelper::
    // applyNormalMap), which creates a transient TUS during shader-state
    // resolution that's not visible on the base pass. To recover that
    // texture name we read the qtme.normal_map UOB hint the importer
    // leaves on the pass (the same hint slice #507 added so FBX export
    // could round-trip the normal map). Issue #510.
    std::set<std::string, std::less<>> seenTex;
    const auto collectFromPass = [&](const Ogre::Pass* pass) {
        if (!pass) return;
        for (auto* tus : pass->getTextureUnitStates()) {
            if (tus->getContentType() != Ogre::TextureUnitState::CONTENT_NAMED)
                continue;
            const auto& name = tus->getTextureName();
            if (!name.empty() && seenTex.insert(name).second)
                info.textures << QString::fromStdString(name);
        }
        // Recover RTSS-wired normal map from the UOB hint. Use the
        // pointer variant of any_cast (returns nullptr on type mismatch
        // — older assets / payload-type drift / a future writer with a
        // different shape) so we don't have to catch std::bad_cast just
        // to swallow it.
        const Ogre::Any& nh =
            pass->getUserObjectBindings().getUserAny("qtme.normal_map");
        if (!nh.has_value()) return;
        if (const Ogre::String* n = Ogre::any_cast<Ogre::String>(&nh)) {
            if (!n->empty() && seenTex.insert(*n).second)
                info.textures << QString::fromStdString(*n);
        }
    };
    for (unsigned int i = 0; i < entity->getNumSubEntities(); ++i) {
        const auto mat = entity->getSubEntity(i)->getMaterial();
        if (!mat) continue;
        for (const Ogre::Technique* tech : mat->getTechniques())
            for (const Ogre::Pass* pass : tech->getPasses())
                collectFromPass(pass);
    }

    // Skeleton & animations
    if (entity->hasSkeleton()) {
        Ogre::SkeletonPtr skel = mesh->getSkeleton();
        if (skel) {
            info.skeletonName = QString::fromStdString(skel->getName());
            info.boneCount = skel->getNumBones();
            for (unsigned short b = 0; b < skel->getNumBones(); ++b)
                info.bones << QString::fromStdString(skel->getBone(b)->getName());
            for (unsigned short a = 0; a < skel->getNumAnimations(); ++a) {
                auto* anim = skel->getAnimation(a);
                info.animations.append({
                    QString::fromStdString(anim->getName()),
                    anim->getLength()
                });
            }
        }
    }

    // Bounding box
    auto bb = mesh->getBounds();
    info.bbMin = bb.getMinimum();
    info.bbMax = bb.getMaximum();

    return info;
}

QString CLIPipeline::formatMeshInfoText(const MeshInfo& info)
{
    QString result;
    QTextStream s(&result);

    s << "File: " << info.file << "\n";
    s << "Coordinate system: "
      << (info.upAxis == 1 ? "Y-up (Mixamo/default)"
                           : info.upAxis == 2 ? "Z-up (Unreal Engine)"
                                              : "unknown")
      << "\n";
    s << "Vertices: " << info.vertices << "\n";
    s << "Triangles: " << info.triangles << "\n";
    s << "Submeshes: " << info.submeshes << "\n";
    s << "Materials: " << (info.materials.isEmpty() ? "(none)" : info.materials.join(", ")) << "\n";

    if (!info.textures.isEmpty())
        s << "Textures: " << info.textures.join(", ") << "\n";

    if (!info.skeletonName.isEmpty()) {
        s << "Skeleton: " << info.skeletonName
          << " (" << info.boneCount << " bones)\n";

        if (!info.bones.isEmpty()) {
            s << "Bones:\n";
            for (const auto& bone : info.bones)
                s << "  " << bone << "\n";
        }

        if (!info.animations.isEmpty()) {
            s << "Animations:\n";
            for (const auto& anim : info.animations)
                s << "  " << anim.name
                  << QString("  %1s").arg(anim.duration, 0, 'f', 3) << "\n";
        }
    }

    s << "Bounding Box: ("
      << QString::number(info.bbMin.x, 'f', 2) << ", "
      << QString::number(info.bbMin.y, 'f', 2) << ", "
      << QString::number(info.bbMin.z, 'f', 2) << ") to ("
      << QString::number(info.bbMax.x, 'f', 2) << ", "
      << QString::number(info.bbMax.y, 'f', 2) << ", "
      << QString::number(info.bbMax.z, 'f', 2) << ")\n";

    return result;
}

QString CLIPipeline::formatMeshInfoJson(const MeshInfo& info)
{
    QJsonObject obj;
    obj["file"] = info.file;
    obj["upAxis"] = info.upAxis == 1 ? "Y-up" : (info.upAxis == 2 ? "Z-up" : "unknown");
    obj["vertices"] = static_cast<int>(info.vertices);
    obj["triangles"] = static_cast<int>(info.triangles);
    obj["submeshes"] = static_cast<int>(info.submeshes);

    // Per-submesh ACMR — empty submeshes are omitted. Issue #399.
    if (!info.submeshAcmr.isEmpty()) {
        QJsonArray acmrArr;
        for (const auto& sa : info.submeshAcmr) {
            QJsonObject so;
            so["submeshIndex"]   = sa.submeshIndex;
            so["triangleCount"]  = sa.triangleCount;
            so["acmr"]           = sa.acmr;
            acmrArr.append(so);
        }
        obj["submeshAcmr"] = acmrArr;
    }

    QJsonArray mats;
    for (const auto& m : info.materials) mats.append(m);
    obj["materials"] = mats;

    if (!info.textures.isEmpty()) {
        QJsonArray texs;
        for (const auto& t : info.textures) texs.append(t);
        obj["textures"] = texs;
    }

    if (!info.skeletonName.isEmpty()) {
        QJsonObject skel;
        skel["name"] = info.skeletonName;
        skel["boneCount"] = info.boneCount;
        QJsonArray boneArr;
        for (const auto& b : info.bones) boneArr.append(b);
        skel["bones"] = boneArr;
        obj["skeleton"] = skel;

        QJsonArray anims;
        for (const auto& a : info.animations) {
            QJsonObject ao;
            ao["name"] = a.name;
            ao["duration"] = static_cast<double>(a.duration);
            anims.append(ao);
        }
        obj["animations"] = anims;
    }

    QJsonObject bb;
    bb["min"] = QJsonArray{info.bbMin.x, info.bbMin.y, info.bbMin.z};
    bb["max"] = QJsonArray{info.bbMax.x, info.bbMax.y, info.bbMax.z};
    obj["boundingBox"] = bb;

    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented));
}

int CLIPipeline::run(int argc, char* argv[])
{
    // Pre-scan for --verbose and --no-telemetry before anything else
    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "--verbose") s_verbose = true;
        if (arg == "--no-telemetry") s_noTelemetry = true;
    }

    // Find the subcommand (skip executable name and --cli flag)
    int cmdIndex = 1;
    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "--cli" || arg == "--verbose" || arg == "--no-telemetry") {
            cmdIndex = i + 1;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            printUsage();
            _exit(0);
        }
        if (arg == "--version" || arg == "-v") {
            printVersion();
            _exit(0);
        }
        // First non-flag argument is the subcommand
        if (!arg.startsWith("-")) {
            cmdIndex = i;
            break;
        }
    }

    if (cmdIndex >= argc) {
        printUsage();
        return 2;
    }

    QString cmd(argv[cmdIndex]);

    // Create QApplication once here so subcommands don't need to.
    // This also lets us call _exit() after the subcommand returns,
    // skipping QApplication/Ogre static destructor teardown that
    // causes SIGSEGV on macOS (GL context cleanup race).
    QApplication a(argc, argv);
    QCoreApplication::setOrganizationName("QtMeshEditor");
    QCoreApplication::setApplicationName("QtMeshEditor");
    QCoreApplication::setApplicationVersion(QTMESHEDITOR_VERSION);

    // Redirect stdout to stderr so Ogre/Qt debug output doesn't
    // pollute the CLI pipeline output (JSON, info text, etc.)
    redirectStdout();

    // Telemetry: --no-telemetry permanently opts out.
    // On first run (no stored preference), show a one-time notice and enable.
    // In ephemeral environments (Docker), QTMESH_NO_TELEMETRY_NOTICE=1
    // suppresses the notice to avoid printing it on every container run.
    if (s_noTelemetry) {
        SentryReporter::setEnabled(false);
        err() << "Telemetry disabled. This preference is stored permanently." << Qt::endl;
    } else if (SentryReporter::isFirstLaunch()) {
        SentryReporter::setEnabled(true);
        if (!qEnvironmentVariableIsSet("QTMESH_NO_TELEMETRY_NOTICE"))
            err() << "Note: Anonymous usage data is collected to improve qtmesh. "
                     "Use --no-telemetry to disable." << Qt::endl;
    }

    if (SentryReporter::isEnabled()) {
        SentryReporter::initialize();
        SentryReporter::setTag("os", QSysInfo::prettyProductName());
        SentryReporter::setTag("arch", QSysInfo::currentCpuArchitecture());
        SentryReporter::setTag("qt_version", qVersion());
        SentryReporter::setTag("launch_mode", "cli");
    }

    auto cliTxn = SentryReporter::startTransaction("cli." + cmd, "cli.command");
    SentryReporter::addBreadcrumb("cli", QString("CLI command: %1").arg(cmd));

    int rc = -1;
    if (cmd == "info") rc = cmdInfo(argc, argv);
    else if (cmd == "fix") rc = cmdFix(argc, argv);
    else if (cmd == "convert") rc = cmdConvert(argc, argv);
    else if (cmd == "anim") rc = cmdAnim(argc, argv);
    else if (cmd == "validate") rc = cmdValidate(argc, argv);
    else if (cmd == "lod") rc = cmdLod(argc, argv);
    else if (cmd == "pose") rc = cmdPose(argc, argv);
    else if (cmd == "turntable") rc = cmdTurntable(argc, argv);
    else if (cmd == "isometric") rc = cmdIsometric(argc, argv);
    else if (cmd == "scan") rc = cmdScan(argc, argv);
    else if (cmd == "material") rc = cmdMaterial(argc, argv);
    else if (cmd == "pack-textures") rc = cmdPackTextures(argc, argv);
    else if (cmd == "normal-from-height") rc = cmdNormalFromHeight(argc, argv);
    else if (cmd == "atlas") rc = cmdAtlas(argc, argv);
    else if (cmd == "atlas-apply") rc = cmdAtlasApply(argc, argv);
    else if (cmd == "memory") rc = cmdMemory(argc, argv);
    else if (cmd == "analyze") rc = cmdAnalyze(argc, argv);
    else if (cmd == "vertex-cache") rc = cmdVertexCache(argc, argv);
    else if (cmd == "decimate") rc = cmdDecimate(argc, argv);
    else if (cmd == "optimize") rc = cmdOptimize(argc, argv);
    else if (cmd == "bake-vertex-colors") rc = cmdBakeVertexColors(argc, argv);
    else if (cmd == "vat") rc = cmdVat(argc, argv);
    else if (cmd == "uv") rc = cmdUv(argc, argv);
    else if (cmd == "retopo") rc = cmdRetopo(argc, argv);
    else if (cmd == "skin") rc = cmdSkin(argc, argv);
    else if (cmd == "rig") rc = cmdRig(argc, argv);
    else if (cmd == "segment") rc = cmdSegment(argc, argv);
    else if (cmd == "generate3d") rc = cmdGenerate3d(argc, argv);
    else if (cmd == "morph") rc = cmdMorph(argc, argv);
    else if (cmd == "nodeanim") rc = cmdNodeAnim(argc, argv);
    else if (cmd == "cloud") rc = CloudCLIPipeline::run(argc, argv);

    if (rc < 0) {
        err() << "Error: Unknown command '" << cmd << "'" << Qt::endl;
        printUsage();
        rc = 2;
    }

    SentryReporter::finishTransaction(cliTxn);
    SentryReporter::shutdown();
    _exit(rc);
}

int CLIPipeline::cmdInfo(int argc, char* argv[])
{
    // Parse: info <file> [--json]
    QString filePath;
    bool jsonOutput = false;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "info" || arg == "--cli") continue;
        if (arg == "--json") { jsonOutput = true; continue; }
        if (!arg.startsWith("-") && filePath.isEmpty()) { filePath = arg; continue; }
    }

    if (filePath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh info <file> [--json]" << Qt::endl;
        return 2;
    }

    QFileInfo fi(filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << filePath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.info", QString("Inspect .%1%2").arg(fi.suffix(), jsonOutput ? " json=true" : ""));

    // Load the file; animation-only files produce no entity but populate animOnlySkeletons.
    QList<Ogre::SkeletonPtr> animOnlySkeletons;
    int upAxis = 1;
    MeshImporterExporter::importer({fi.absoluteFilePath()}, 0, &animOnlySkeletons, &upAxis);

    auto& entities = Manager::getSingleton()->getEntities();

    // Animation-only file: no entities, but skeleton was loaded.
    if (entities.isEmpty() && !animOnlySkeletons.isEmpty()) {
        QList<MeshInfo> infos;
        for (const Ogre::SkeletonPtr& skel : animOnlySkeletons) {
            if (!skel) continue;
            MeshInfo info;
            info.file = fi.fileName();
            info.upAxis = upAxis;
            info.skeletonName = QString::fromStdString(skel->getName());
            info.boneCount = skel->getNumBones();
            for (unsigned short b = 0; b < skel->getNumBones(); ++b)
                info.bones << QString::fromStdString(skel->getBone(b)->getName());
            for (unsigned short i = 0; i < skel->getNumAnimations(); ++i) {
                auto* anim = skel->getAnimation(i);
                info.animations.append({QString::fromStdString(anim->getName()), anim->getLength()});
            }
            infos.append(info);
        }
        if (jsonOutput) {
            QJsonArray arr;
            for (const auto& info : infos) {
                QJsonDocument doc = QJsonDocument::fromJson(formatMeshInfoJson(info).toUtf8());
                arr.append(doc.object());
            }
            cliWrite(QString::fromUtf8((arr.size() == 1
                ? QJsonDocument(arr[0].toObject())
                : QJsonDocument(arr)).toJson(QJsonDocument::Indented)));
        } else {
            for (const auto& info : infos)
                cliWrite(formatMeshInfoText(info));
        }
        maybePrintCloudPromo(jsonOutput);
        return 0;
    }

    if (entities.isEmpty()) {
        SentryReporter::captureMessage(QString("CLI info: import failed (.%1)").arg(fi.suffix()), "error");
        err() << "Error: Failed to load file: " << filePath << Qt::endl;
        return 1;
    }

    // If multiple entities loaded, show info for all
    if (jsonOutput) {
        QJsonArray arr;
        for (Ogre::Entity* entity : entities) {
            MeshInfo info = extractMeshInfo(entity, fi.fileName());
            info.upAxis = upAxis;
            QJsonDocument doc = QJsonDocument::fromJson(formatMeshInfoJson(info).toUtf8());
            arr.append(doc.object());
        }
        // Single entity: emit object directly; multiple: emit array
        if (arr.size() == 1)
            cliWrite(QString::fromUtf8(QJsonDocument(arr[0].toObject()).toJson(QJsonDocument::Indented)));
        else
            cliWrite(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Indented)));
    } else {
        for (Ogre::Entity* entity : entities) {
            MeshInfo info = extractMeshInfo(entity, fi.fileName());
            info.upAxis = upAxis;
            cliWrite(formatMeshInfoText(info));
        }
    }

    maybePrintCloudPromo(jsonOutput);
    return 0;
}

int CLIPipeline::cmdConvert(int argc, char* argv[])
{
    // Parse: convert <file> -o <output> [--format fmt]
    QString inputPath, outputPath, format;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "convert" || arg == "--cli") continue;
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString(argv[++i]);
            continue;
        }
        if (arg == "--format" && i + 1 < argc) {
            format = QString(argv[++i]);
            continue;
        }
        if (!arg.startsWith("-") && inputPath.isEmpty()) {
            inputPath = arg;
            continue;
        }
    }

    if (inputPath.isEmpty() || outputPath.isEmpty()) {
        err() << "Error: Missing required arguments." << Qt::endl;
        err() << "Usage: qtmesh convert <file> -o <output> [--format fmt]" << Qt::endl;
        return 2;
    }

    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << inputPath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.convert", QString("Convert .%1 -> .%2").arg(fi.suffix(), QFileInfo(outputPath).suffix()));

    MeshImporterExporter::importer({fi.absoluteFilePath()});

    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        SentryReporter::captureMessage(QString("CLI convert: import failed (.%1)").arg(fi.suffix()), "error");
        err() << "Error: Failed to load file: " << inputPath << Qt::endl;
        return 1;
    }

    Ogre::Entity* entity = entities.first();
    auto* node = entity->getParentSceneNode();

    QString fmt = format.isEmpty() ? formatForExtension(outputPath) : format;

    QFileInfo outFi(outputPath);
    QString absOutput = outFi.absoluteFilePath();

    int result = MeshImporterExporter::exporter(node, absOutput, fmt);
    if (result != 0) {
        SentryReporter::captureMessage(QString("CLI convert: export failed (.%1 -> .%2)").arg(fi.suffix(), QFileInfo(outputPath).suffix()), "error");
        err() << "Error: Export failed." << Qt::endl;
        return 1;
    }

    cliWrite(QString("Converted: %1 -> %2\n").arg(fi.fileName(), outFi.fileName()));

    return 0;
}

int CLIPipeline::cmdFix(int argc, char* argv[])
{
    // Parse: fix <file> -o <output> [flags]
    QString inputPath, outputPath;
    FixOptions opts;
    bool allFlag = false;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "fix" || arg == "--cli") continue;
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString(argv[++i]);
            continue;
        }
        if (arg == "--remove-degenerates") { opts.removeDegenerates = true; continue; }
        if (arg == "--merge-materials") { opts.mergeMaterials = true; continue; }
        if (arg == "--all") { allFlag = true; continue; }
        if (!arg.startsWith("-") && inputPath.isEmpty()) {
            inputPath = arg;
            continue;
        }
    }

    if (inputPath.isEmpty()) {
        err() << "Error: Missing required arguments." << Qt::endl;
        err() << "Usage: qtmesh fix <file> [-o <output>] [--remove-degenerates] [--merge-materials] [--all]" << Qt::endl;
        return 2;
    }

    if (outputPath.isEmpty()) {
        outputPath = inputPath;  // overwrite in place
    }

    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << inputPath << Qt::endl;
        return 1;
    }

    if (allFlag) {
        opts.removeDegenerates = true;
        opts.mergeMaterials = true;
    }

    QFileInfo outFi(outputPath);

    // Get "before" counts using the same Assimp flags as the standard import
    // pipeline, so numbers match what `info` reports for the file on disk.
    unsigned int vertsBefore = 0, trisBefore = 0;
    {
        Assimp::Importer rawImporter;
        rawImporter.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        unsigned int stdFlags = aiProcess_CalcTangentSpace |
                                aiProcess_JoinIdenticalVertices |
                                aiProcess_Triangulate |
                                aiProcess_RemoveComponent |
                                aiProcess_GenSmoothNormals |
                                aiProcess_ValidateDataStructure |
                                aiProcess_OptimizeGraph |
                                aiProcess_LimitBoneWeights |
                                aiProcess_SortByPType |
                                aiProcess_ImproveCacheLocality |
                                aiProcess_FixInfacingNormals |
                                aiProcess_PopulateArmatureData |
                                aiProcess_OptimizeMeshes |
                                aiProcess_GlobalScale;
        const aiScene* rawScene = rawImporter.ReadFile(
            fi.absoluteFilePath().toStdString(), stdFlags);
        if (rawScene) {
            for (unsigned int m = 0; m < rawScene->mNumMeshes; ++m) {
                vertsBefore += rawScene->mMeshes[m]->mNumVertices;
                trisBefore += rawScene->mMeshes[m]->mNumFaces;
            }
        }
    }

    // Route through the Ogre pipeline (MeshImporterExporter).
    // The standard Assimp import already applies key fixes:
    //   JoinIdenticalVertices, GenSmoothNormals, ValidateDataStructure,
    //   OptimizeMeshes, OptimizeGraph, ImproveCacheLocality, CalcTangentSpace.
    // MeshImporterExporter::exporter uses the custom FBXExporter for FBX
    // (Assimp's FBX exporter is broken and produces files that freeze viewers).
    if (!initOgreHeadless()) return 1;

    QStringList fixFlags;
    if (opts.removeDegenerates) fixFlags << "remove-degenerates";
    if (opts.mergeMaterials) fixFlags << "merge-materials";
    SentryReporter::addBreadcrumb("cli.fix", QString("Fix .%1 -> .%2%3")
        .arg(fi.suffix(), outFi.suffix(),
             fixFlags.isEmpty() ? "" : " [" + fixFlags.join(", ") + "]"));

    MeshImporterExporter::importer({fi.absoluteFilePath()}, opts.toAssimpFlags());
    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        SentryReporter::captureMessage(QString("CLI fix: import failed (.%1)").arg(fi.suffix()), "error");
        err() << "Error: Failed to load file: " << inputPath << Qt::endl;
        return 1;
    }

    // Gather "after" counts from Ogre entities
    unsigned int vertsAfter = 0, trisAfter = 0;
    for (Ogre::Entity* entity : entities) {
        MeshInfo info = extractMeshInfo(entity, fi.fileName());
        vertsAfter += info.vertices;
        trisAfter += info.triangles;
    }

    auto* node = entities.first()->getParentSceneNode();
    QString fmt = formatForExtension(outputPath);

    int result = MeshImporterExporter::exporter(node, outFi.absoluteFilePath(), fmt);
    if (result != 0) {
        SentryReporter::captureMessage(QString("CLI fix: export failed (.%1 -> .%2)").arg(fi.suffix(), outFi.suffix()), "error");
        err() << "Error: Export failed." << Qt::endl;
        return 1;
    }

    // Report
    QString report;
    report += QString("Fixed: %1 -> %2\n").arg(fi.fileName(), outFi.fileName());
    report += "  Standard: join-vertices, recalc-normals, optimize, validate\n";
    if (opts.anySet()) {
        QStringList extras;
        if (opts.removeDegenerates) extras << "remove-degenerates";
        if (opts.mergeMaterials) extras << "merge-materials";
        report += QString("  Extra: %1\n").arg(extras.join(", "));
    }

    if (vertsBefore > 0) {
        double vertChange = ((double)vertsAfter - (double)vertsBefore) / (double)vertsBefore * 100.0;
        report += QString("  Vertices: %1 -> %2").arg(vertsBefore).arg(vertsAfter);
        if (vertsBefore == vertsAfter)
            report += " (unchanged)";
        else
            report += QString(" (%1%2%)").arg(vertChange >= 0 ? "+" : "").arg(vertChange, 0, 'f', 1);
        report += "\n";

        double triChange = trisBefore > 0
            ? ((double)trisAfter - (double)trisBefore) / (double)trisBefore * 100.0 : 0.0;
        report += QString("  Triangles: %1 -> %2").arg(trisBefore).arg(trisAfter);
        if (trisBefore == trisAfter)
            report += " (unchanged)";
        else
            report += QString(" (%1%2%)").arg(triChange >= 0 ? "+" : "").arg(triChange, 0, 'f', 1);
        report += "\n";
    }

    cliWrite(report);
    return 0;
}

int CLIPipeline::cmdAnimGenerate(const QString& filePath, const QString& prompt,
                                 float duration, const QString& outputPath,
                                 bool jsonOutput, bool useModel)
{
    // #411 text-to-motion (template-clip MVP): match the prompt to a permissive
    // CMU motion clip from the downloadable library, retarget it onto the mesh's
    // skeleton via the #409 canonical mapping, and re-export.
    QFileInfo fi(filePath);
    if (!fi.exists()) { err() << "Error: File not found: " << filePath << Qt::endl; return 1; }
    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.text_to_motion"),
                                  QStringLiteral("CLI anim --generate"));

    // Acquire the canonical clip from one of two sources:
    //   (A) the EXPERIMENTAL trained model (--model), tried first when requested;
    //   (B) the TEMPLATE-clip library (default + automatic fallback).
    // Both yield LOCAL-frame quats consumed identically by applyMotionClip.
    QString action;
    std::vector<std::vector<std::array<float, 4>>> quats;
    int fps = 30;
    bool worldFrame = false;
    std::vector<std::array<float, 4>> cmuRest;   // template-only (model has none)
    QString clipSource;

    bool gotClip = false;
    if (useModel) {
        const QString mp = MotionGenerator::ensureModelBlocking();
        if (mp.isEmpty()) {
            err() << "Note: text-to-motion model unavailable "
                     "(needs ONNX build + first-use download); using template library."
                  << Qt::endl;
        } else {
            const auto mr = MotionGenerator::generate(prompt, mp,
                                                      MotionGenerator::vocabPath(),
                                                      duration);
            if (mr.ok) {
                action = mr.matchedAction;
                quats = mr.clip.quats;
                fps = mr.clip.fps;
                worldFrame = false;          // model emits LOCAL-frame quats
                clipSource = QStringLiteral("model");
                gotClip = true;
            } else {
                err() << "Note: model generation failed (" << mr.error
                      << "); using template library." << Qt::endl;
            }
        }
    }

    if (!gotClip) {
        const QString libPath = MotionLibrary::ensureLibraryBlocking();
        if (libPath.isEmpty()) {
            err() << "Error: motion library unavailable (offline, or set "
                     "QTMESH_MOTION_LIBRARY_BASE_URL)." << Qt::endl;
            return 1;
        }
        MotionLibrary lib;
        if (!lib.loadFromFile(libPath)) {
            err() << "Error: " << lib.error() << Qt::endl; return 1;
        }
        const int idx = lib.matchPrompt(prompt, &action);
        if (idx < 0) {
            err() << "Error: no motion matched \"" << prompt << "\". Known actions:";
            for (const QString& a : lib.actions()) err() << " " << a;
            err() << Qt::endl;
            return 1;
        }
        const MotionLibrary::Clip& clip = lib.clip(idx);
        quats = clip.quats;
        fps = clip.fps;
        worldFrame = lib.isWorldFrame();
        cmuRest = lib.cmuRestWorld();
        clipSource = QStringLiteral("template");
        // Optionally retime the clip to a requested duration by frame stride/pad.
        if (duration > 0.05f) {
            const int want = std::max(2, int(duration * clip.fps));
            std::vector<std::vector<std::array<float, 4>>> retimed(want);
            for (int f = 0; f < want; ++f) {
                const float src = (clip.frames - 1) * (float(f) / float(want - 1));
                retimed[f] = quats[std::min(clip.frames - 1, int(src + 0.5f))];
            }
            quats.swap(retimed);
        }
    }

    MeshImporterExporter::importer({fi.absoluteFilePath()});
    auto& entities = Manager::getSingleton()->getEntities();
    Ogre::Entity* entity = nullptr;
    for (auto* e : entities)
        if (e && e->getMovableType() == "Entity" && e->hasSkeleton()) { entity = e; break; }
    if (!entity) {
        err() << "Error: " << filePath << " has no skinned mesh — text-to-motion "
                 "needs a rigged humanoid skeleton to retarget onto." << Qt::endl;
        return 1;
    }
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();

    const std::string animName = ("generated_" + action).toStdString();
    auto res = AnimationMerger::applyMotionClip(skel.get(), animName, quats, fps,
                                                worldFrame, cmuRest);
    if (!res.ok) {
        err() << "Error: " << res.error << Qt::endl; return 1;
    }
    err() << "(source: " << clipSource << ")" << Qt::endl;

    auto* node = entity->getParentSceneNode();
    const QString fmt = formatForExtension(outputPath);
    if (MeshImporterExporter::exporter(node, QFileInfo(outputPath).absoluteFilePath(), fmt) != 0) {
        err() << "Error: export failed." << Qt::endl; return 1;
    }

    if (jsonOutput) {
        QJsonObject root;
        root["prompt"] = prompt; root["action"] = action; root["source"] = clipSource;
        root["animation"] = QString::fromStdString(animName);
        root["frames"] = res.frames; root["length"] = res.length;
        root["tracksWritten"] = res.tracksWritten; root["canonicalJoints"] = res.canonicalJoints;
        root["output"] = QFileInfo(outputPath).fileName();
        cliWrite(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)) + "\n");
    } else {
        cliWrite(QString("Generated motion '%1' (%2, %3) → %4 bones over %5 joints, "
                         "%6 frames (%7s) → %8\n")
                     .arg(action, clipSource, QString::fromStdString(animName))
                     .arg(res.tracksWritten).arg(res.canonicalJoints)
                     .arg(res.frames).arg(res.length, 0, 'f', 1)
                     .arg(QFileInfo(outputPath).fileName()));
    }
    return 0;
}

int CLIPipeline::cmdAnim(int argc, char* argv[])
{
    // Parse: anim <file> --list [--json]
    //    or: anim <file> --analyze [--json]
    //    or: anim <file> --rename <old> <new> [-o <output>]
    //    or: anim <file> --merge <f1> [f2...] [-o <output>]
    //    or: anim <file> --resample N [-o <output>] [--animation <name>]
    //    or: anim <file> --decimate-step S [-o <output>] [--animation <name>]
    QString filePath, oldName, newName, outputPath, animationFilter;
    bool listMode = false;
    bool analyzeMode = false;
    bool renameMode = false;
    bool mergeMode = false;
    bool resampleMode = false;
    bool decimateMode = false;
    bool simplifyMode = false;
    bool bakeFpsMode  = false;
    bool inbetweenMode = false;       // #409: AI in-betweening
    bool inbetweenNoModel = false;    // --no-model → force spline fallback
    bool generateMode = false;        // #411: text-to-motion (template-clip MVP)
    QString generatePrompt;           // --generate "<prompt>"
    float generateDuration = 0.0f;    // --duration N (seconds; 0 = clip's native length)
    bool generateUseModel = false;    // --model → experimental trained t2m model (template fallback)
    bool jsonOutput = false;
    int resampleCount = 0;
    int decimateStep = 0;
    int bakeFps      = 0;
    int inbetweenGapFrames = 0;       // --gap-frames N
    float inbetweenStart = -1.0f;     // --start-time S (default: clip start)
    float inbetweenEnd   = -1.0f;     // --end-time S   (default: clip end)
    // Default tolerances mirror the AnimationMerger "Balanced" preset
    // (1mm translation, 0.5° rotation) — visually indistinguishable on
    // meter-scale character clips. Override via --tolerance / --rotation-tolerance-deg
    // or --preset {conservative|balanced|aggressive}.
    AnimationMerger::SimplifyTolerances simplifyDefaults; // Balanced
    float simplifyTranslationTol = simplifyDefaults.translation;
    float simplifyRotationDegTol = simplifyDefaults.rotationDeg;
    float simplifyScaleTol       = simplifyDefaults.scale;
    QStringList mergeFiles;

    // Collect positional args (excluding flags)
    QStringList positional;
    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "anim" || arg == "--cli") continue;
        if (arg == "--list") { listMode = true; continue; }
        if (arg == "--analyze") { analyzeMode = true; continue; }
        if (arg == "--json") { jsonOutput = true; continue; }
        if (arg == "--rename" && i + 2 < argc) {
            renameMode = true;
            oldName = QString(argv[++i]);
            newName = QString(argv[++i]);
            continue;
        }
        if (arg == "--merge") {
            mergeMode = true;
            // Collect files until next --flag or end
            while (i + 1 < argc && QString(argv[i + 1]).left(2) != "--"
                   && QString(argv[i + 1]) != "-o") {
                mergeFiles.append(QString(argv[++i]));
            }
            continue;
        }
        if (arg == "--resample" && i + 1 < argc) {
            resampleMode = true;
            resampleCount = QString(argv[++i]).toInt();
            continue;
        }
        if (arg == "--decimate-step" && i + 1 < argc) {
            decimateMode = true;
            decimateStep = QString(argv[++i]).toInt();
            continue;
        }
        if (arg == "--bake-fps" && i + 1 < argc) {
            bakeFpsMode = true;
            bakeFps = QString(argv[++i]).toInt();
            continue;
        }
        if (arg == "--in-between") { inbetweenMode = true; continue; }
        if (arg == "--gap-frames" && i + 1 < argc) {
            inbetweenGapFrames = QString(argv[++i]).toInt();
            continue;
        }
        if (arg == "--start-time" && i + 1 < argc) {
            inbetweenStart = QString(argv[++i]).toFloat();
            continue;
        }
        if (arg == "--end-time" && i + 1 < argc) {
            inbetweenEnd = QString(argv[++i]).toFloat();
            continue;
        }
        if (arg == "--no-model") { inbetweenNoModel = true; continue; }
        if (arg == "--generate" && i + 1 < argc) {
            generateMode = true;
            generatePrompt = QString::fromLocal8Bit(argv[++i]);
            continue;
        }
        // --model (no arg, anim subcommand): opt into the EXPERIMENTAL trained
        // text-to-motion model for --generate; falls back to the template library
        // automatically if the model is unavailable.
        if (arg == "--model" && generateMode) { generateUseModel = true; continue; }
        if (arg == "--duration" && i + 1 < argc) {
            generateDuration = QString(argv[++i]).toFloat();
            continue;
        }
        if (arg == "--simplify") { simplifyMode = true; continue; }
        if (arg == "--analyze")  { analyzeMode  = true; continue; }
        if (arg == "--preset" && i + 1 < argc) {
            QString preset = QString(argv[++i]).toLower();
            bool presetOk = true;
            const auto presetTol = AnimationMerger::tolerancesForPreset(
                preset.toStdString(), &presetOk);
            if (!presetOk) {
                err() << "Error: Unknown preset '" << preset
                      << "'. Use conservative, balanced, or aggressive." << Qt::endl;
                return 2;
            }
            simplifyTranslationTol = presetTol.translation;
            simplifyRotationDegTol = presetTol.rotationDeg;
            simplifyScaleTol       = presetTol.scale;
            continue;
        }
        if (arg == "--tolerance" && i + 1 < argc) {
            simplifyTranslationTol = QString(argv[++i]).toFloat();
            simplifyScaleTol = simplifyTranslationTol;
            continue;
        }
        if (arg == "--rotation-tolerance-deg" && i + 1 < argc) {
            simplifyRotationDegTol = QString(argv[++i]).toFloat();
            continue;
        }
        if (arg == "--animation" && i + 1 < argc) {
            animationFilter = QString(argv[++i]);
            continue;
        }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString(argv[++i]);
            continue;
        }
        if (!arg.startsWith("-"))
            positional << arg;
    }

    if (positional.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        return 2;
    }

    filePath = positional[0];

    // #411: text-to-motion (template-clip MVP). Self-contained — load → match a
    // motion-library clip → retarget → export. Handled before the other modes.
    if (generateMode) {
        if (generatePrompt.trimmed().isEmpty()) {
            err() << "Error: --generate requires a prompt, e.g. --generate \"walking\"." << Qt::endl;
            return 2;
        }
        return cmdAnimGenerate(filePath, generatePrompt, generateDuration,
                               outputPath.isEmpty() ? filePath : outputPath, jsonOutput,
                               generateUseModel);
    }

    if (!listMode && !renameMode && !mergeMode && !resampleMode && !decimateMode
        && !simplifyMode && !analyzeMode && !bakeFpsMode && !inbetweenMode) {
        err() << "Error: Specify --list, --rename, --merge, --resample, --decimate-step, --simplify, --bake-fps, --in-between, --generate, or --analyze." << Qt::endl;
        err() << "Usage: qtmesh anim <file> --list [--json]" << Qt::endl;
        err() << "       qtmesh anim <file> --analyze [--json]" << Qt::endl;
        err() << "       qtmesh anim <file> --rename <old> <new> [-o <output>]" << Qt::endl;
        err() << "       qtmesh anim <file> --merge <f1> [f2...] [-o <output>]" << Qt::endl;
        err() << "       qtmesh anim <file> --resample N [-o <output>] [--animation <name>]" << Qt::endl;
        err() << "       qtmesh anim <file> --decimate-step S [-o <output>] [--animation <name>]" << Qt::endl;
        err() << "       qtmesh anim <file> --bake-fps N [-o <output>] [--animation <name>]" << Qt::endl;
        err() << "       qtmesh anim <file> --simplify [--preset {conservative|balanced|aggressive}] [--tolerance T] [--rotation-tolerance-deg D] [-o <output>] [--animation <name>]" << Qt::endl;
        err() << "                          (--tolerance T sets translation+scale tolerance in world units)" << Qt::endl;
        err() << "       qtmesh anim <file> --analyze [--json] [--preset ...] [--tolerance T] [--rotation-tolerance-deg D]" << Qt::endl;
        err() << "       qtmesh anim <file> --in-between --gap-frames N [--start-time S] [--end-time S] [--no-model] [-o <output>] [--animation <name>]" << Qt::endl;
        return 2;
    }

    if (inbetweenMode && inbetweenGapFrames < 1) {
        err() << "Error: --in-between requires --gap-frames N (N >= 1)." << Qt::endl;
        return 2;
    }

    if ((renameMode || mergeMode || resampleMode || decimateMode || simplifyMode
         || bakeFpsMode || inbetweenMode) && outputPath.isEmpty()) {
        outputPath = filePath;  // overwrite in place
    }

    QFileInfo fi(filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << filePath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    QString animOp = listMode      ? "list"
                   : renameMode    ? "rename"
                   : resampleMode  ? "resample"
                   : decimateMode  ? "decimate"
                   : simplifyMode  ? "simplify"
                   : bakeFpsMode   ? "bake-fps"
                   : analyzeMode   ? "analyze"
                                   : "merge";
    SentryReporter::addBreadcrumb("cli.anim", QString("Anim %1 .%2%3")
        .arg(animOp, fi.suffix(), mergeMode ? QString(" files=%1").arg(mergeFiles.size()) : ""));

    // Support animation-only files: importer may populate animOnlySkeletons without creating entities.
    QList<Ogre::SkeletonPtr> animOnlySkeletons;
    MeshImporterExporter::importer({fi.absoluteFilePath()}, 0, &animOnlySkeletons);

    auto& entities = Manager::getSingleton()->getEntities();
    Ogre::Entity* entity = nullptr;
    Ogre::SkeletonPtr skel;

    if (!entities.isEmpty()) {
        entity = entities.first();
        if (entity->hasSkeleton())
            skel = entity->getMesh()->getSkeleton();
    }

    if (!skel && !animOnlySkeletons.isEmpty())
        skel = animOnlySkeletons.first();

    if (!skel) {
        // Distinguish "import failed" from "loaded but non-rigged" so
        // CLI users get an actionable message. The first branch fires
        // when no entity was created AND no anim-only skeleton was
        // returned by the importer.
        const bool importFailed = entities.isEmpty() && animOnlySkeletons.isEmpty();
        SentryReporter::captureMessage(QString("CLI anim: %1 (.%2)")
            .arg(importFailed ? "import failed" : "no skeleton")
            .arg(fi.suffix()), "error");
        if (importFailed) {
            err() << "Error: Failed to load file: " << filePath << Qt::endl;
        } else {
            err() << "Error: No skeleton found." << Qt::endl;
        }
        return 1;
    }

    const bool isAnimOnlyInput = (entity == nullptr);

    if (listMode) {
        if (skel->getNumAnimations() == 0) {
            cliWrite(jsonOutput ? "[]\n" : "No animations found.\n");
            return 0;
        }

        if (jsonOutput) {
            QJsonArray arr;
            for (unsigned short i = 0; i < skel->getNumAnimations(); ++i) {
                auto* anim = skel->getAnimation(i);
                QJsonObject obj;
                obj["name"] = QString::fromStdString(anim->getName());
                obj["duration"] = static_cast<double>(anim->getLength());
                arr.append(obj);
            }
            cliWrite(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Indented)) + "\n");
        } else {
            QString listing = "Animations:\n";
            for (unsigned short i = 0; i < skel->getNumAnimations(); ++i) {
                auto* anim = skel->getAnimation(i);
                listing += QString("  %1  %2s\n")
                    .arg(QString::fromStdString(anim->getName()))
                    .arg(anim->getLength(), 0, 'f', 3);
            }
            cliWrite(listing);
        }

        return 0;
    }

    if (analyzeMode) {
        if (jsonOutput) {
            QJsonObject root;
            root["skeletonName"] = QString::fromStdString(skel->getName());
            root["boneCount"] = static_cast<int>(skel->getNumBones());
            QJsonArray animArr;
            for (unsigned short i = 0; i < skel->getNumAnimations(); ++i) {
                auto* anim = skel->getAnimation(i);
                QJsonObject a;
                a["name"] = QString::fromStdString(anim->getName());
                a["duration"] = static_cast<double>(anim->getLength());
                animArr.append(a);
            }
            root["animations"] = animArr;
            cliWrite(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)) + "\n");
        } else {
            QString out;
            out += QString("Skeleton: %1 (%2 bones)\n")
                       .arg(QString::fromStdString(skel->getName()))
                       .arg(skel->getNumBones());
            out += QString("Animations: %1\n").arg(skel->getNumAnimations());
            for (unsigned short i = 0; i < skel->getNumAnimations(); ++i) {
                auto* anim = skel->getAnimation(i);
                out += QString("  %1  %2s\n")
                           .arg(QString::fromStdString(anim->getName()))
                           .arg(anim->getLength(), 0, 'f', 3);
            }
            cliWrite(out);
        }
        return 0;
    }

    // Merge mode
    if (mergeMode) {
        if (!entity) {
            err() << "Error: --merge requires a base file with mesh geometry. "
                     "An animation-only file cannot be used as the merge base." << Qt::endl;
            return 1;
        }
        // Load animation files; animation-only files (no mesh) produce a skeleton instead of entity.
        QList<Ogre::SkeletonPtr> mergeAnimOnlySkeletons;
        for (const auto& f : mergeFiles) {
            int entityCountBefore = Manager::getSingleton()->getEntities().size();
            int skelCountBefore = mergeAnimOnlySkeletons.size();
            MeshImporterExporter::importer({f}, 0, &mergeAnimOnlySkeletons);
            bool gotEntity = Manager::getSingleton()->getEntities().size() > entityCountBefore;
            bool gotSkeleton = mergeAnimOnlySkeletons.size() > skelCountBefore;
            if (!gotEntity && !gotSkeleton) {
                SentryReporter::captureMessage(QString("CLI anim: merge input import failed (.%1)").arg(QFileInfo(f).suffix()), "error");
                err() << "Error: Failed to load animation file: " << f << Qt::endl;
                return 1;
            }
        }

        auto& allEntities = Manager::getSingleton()->getEntities();
        // allEntities includes the base entity (already validated above) plus any
        // mesh entities from merge files. If no additional mesh entities AND no
        // animation-only skeletons were collected, there is nothing to merge.
        if (allEntities.size() < 2 && mergeAnimOnlySkeletons.isEmpty()) {
            err() << "Error: Need at least one source file to merge (got none)." << Qt::endl;
            return 1;
        }

        QString mergeErr;
        Ogre::Entity* merged = AnimationMerger::mergeAnimations(allEntities.first(), allEntities, mergeAnimOnlySkeletons, mergeErr);
        if (!merged) {
            SentryReporter::captureMessage("CLI anim: merge failed", "error");
            err() << "Error: Merge failed: " << mergeErr << Qt::endl;
            return 1;
        }

        auto* mergeNode = merged->getParentSceneNode();
        QFileInfo outFi(outputPath);
        int result = MeshImporterExporter::exporter(mergeNode, outFi.absoluteFilePath(), formatForExtension(outputPath));
        if (result != 0) {
            SentryReporter::captureMessage(QString("CLI anim: merge export failed (.%1)").arg(outFi.suffix()), "error");
            err() << "Error: Export failed." << Qt::endl;
            return 1;
        }

        cliWrite(QString("Merged %1 files -> %2\n").arg(allEntities.size()).arg(outFi.fileName()));
        return 0;
    }

    // Resample mode
    if (resampleMode) {
        if (resampleCount < 2) {
            err() << "Error: --resample requires N >= 2." << Qt::endl;
            return 2;
        }

        SentryReporter::addBreadcrumb("cli.anim", QString("Resample N=%1 anim=%2")
            .arg(resampleCount).arg(animationFilter.isEmpty() ? "(all)" : animationFilter));

        int totalRemoved = 0;
        int animsProcessed = 0;
        unsigned short numAnims = skel->getNumAnimations();

        // Collect animation names first (modifying skeleton invalidates iteration)
        std::vector<std::string> animNames;
        for (unsigned short i = 0; i < numAnims; ++i)
            animNames.push_back(skel->getAnimation(i)->getName());

        for (const auto& name : animNames) {
            if (!animationFilter.isEmpty() && animationFilter.toStdString() != name)
                continue;
            int removed = AnimationMerger::resampleAnimation(skel.get(), name, resampleCount);
            totalRemoved += removed;
            ++animsProcessed;
        }

        if (animsProcessed == 0) {
            err() << "Error: No matching animation found." << Qt::endl;
            if (!animationFilter.isEmpty()) {
                err() << "Available animations:" << Qt::endl;
                for (const auto& name : animNames)
                    err() << "  " << QString::fromStdString(name) << Qt::endl;
            }
            return 1;
        }

        QFileInfo outFi(outputPath);
        if (isAnimOnlyInput) {
            QString exportErr;
            if (!exportAnimOnly(skel, outFi.absoluteFilePath(), &exportErr)) {
                SentryReporter::captureMessage(QString("CLI anim: resample export failed (anim-only)"), "error");
                err() << "Error: Export failed: " << exportErr << Qt::endl;
                return 1;
            }
        } else {
            entity->refreshAvailableAnimationState();
            auto* node = entity->getParentSceneNode();
            int result = MeshImporterExporter::exporter(node, outFi.absoluteFilePath(), formatForExtension(outputPath));
            if (result != 0) {
                SentryReporter::captureMessage(QString("CLI anim: resample export failed (.%1)").arg(outFi.suffix()), "error");
                err() << "Error: Export failed." << Qt::endl;
                return 1;
            }
        }

        cliWrite(QString("Resampled %1 animation(s) to %2 keyframes (removed %3 keyframes)\nOutput: %4\n")
            .arg(animsProcessed).arg(resampleCount).arg(totalRemoved).arg(outFi.fileName()));
        return 0;
    }

    // Decimate mode
    if (decimateMode) {
        if (decimateStep < 2) {
            err() << "Error: --decimate-step requires S >= 2." << Qt::endl;
            return 2;
        }

        SentryReporter::addBreadcrumb("cli.anim", QString("Decimate step=%1 anim=%2")
            .arg(decimateStep).arg(animationFilter.isEmpty() ? "(all)" : animationFilter));

        int totalRemoved = 0;
        int animsProcessed = 0;
        unsigned short numAnims = skel->getNumAnimations();

        // Collect animation names first (modifying skeleton invalidates iteration)
        std::vector<std::string> animNames;
        for (unsigned short i = 0; i < numAnims; ++i)
            animNames.push_back(skel->getAnimation(i)->getName());

        for (const auto& name : animNames) {
            if (!animationFilter.isEmpty() && animationFilter.toStdString() != name)
                continue;
            int removed = AnimationMerger::decimateAnimation(skel.get(), name, decimateStep);
            totalRemoved += removed;
            ++animsProcessed;
        }

        if (animsProcessed == 0) {
            err() << "Error: No matching animation found." << Qt::endl;
            if (!animationFilter.isEmpty()) {
                err() << "Available animations:" << Qt::endl;
                for (const auto& name : animNames)
                    err() << "  " << QString::fromStdString(name) << Qt::endl;
            }
            return 1;
        }

        QFileInfo outFi(outputPath);
        if (isAnimOnlyInput) {
            QString exportErr;
            if (!exportAnimOnly(skel, outFi.absoluteFilePath(), &exportErr)) {
                SentryReporter::captureMessage(QString("CLI anim: decimate export failed (anim-only)"), "error");
                err() << "Error: Export failed: " << exportErr << Qt::endl;
                return 1;
            }
        } else {
            entity->refreshAvailableAnimationState();
            auto* node = entity->getParentSceneNode();
            int result = MeshImporterExporter::exporter(node, outFi.absoluteFilePath(), formatForExtension(outputPath));
            if (result != 0) {
                SentryReporter::captureMessage(QString("CLI anim: decimate export failed (.%1)").arg(outFi.suffix()), "error");
                err() << "Error: Export failed." << Qt::endl;
                return 1;
            }
        }

        cliWrite(QString("Decimated %1 animation(s) with step %2 (removed %3 keyframes)\nOutput: %4\n")
            .arg(animsProcessed).arg(decimateStep).arg(totalRemoved).arg(outFi.fileName()));
        return 0;
    }

    // Bake-FPS mode: re-grid every animation to a uniform N FPS layout.
    if (bakeFpsMode) {
        if (bakeFps < 1) {
            err() << "Error: --bake-fps requires N >= 1." << Qt::endl;
            return 2;
        }

        SentryReporter::addBreadcrumb("cli.anim", QString("Bake fps=%1 anim=%2")
            .arg(bakeFps).arg(animationFilter.isEmpty() ? "(all)" : animationFilter));

        std::vector<std::string> animNames;
        unsigned short numAnims = skel->getNumAnimations();
        for (unsigned short i = 0; i < numAnims; ++i)
            animNames.push_back(skel->getAnimation(i)->getName());

        int totalKeys = 0;
        int animsProcessed = 0;
        for (const auto& name : animNames) {
            if (!animationFilter.isEmpty() && animationFilter.toStdString() != name)
                continue;
            const int kept = AnimationMerger::bakeAnimationAtFps(skel.get(), name, bakeFps);
            totalKeys += kept;
            ++animsProcessed;
        }

        if (animsProcessed == 0) {
            err() << "Error: No matching animation found." << Qt::endl;
            if (!animationFilter.isEmpty()) {
                err() << "Available animations:" << Qt::endl;
                for (const auto& name : animNames)
                    err() << "  " << QString::fromStdString(name) << Qt::endl;
            }
            return 1;
        }

        QFileInfo outFi(outputPath);
        if (isAnimOnlyInput) {
            QString exportErr;
            if (!exportAnimOnly(skel, outFi.absoluteFilePath(), &exportErr)) {
                SentryReporter::captureMessage(QString("CLI anim: bake-fps export failed (anim-only)"), "error");
                err() << "Error: Export failed: " << exportErr << Qt::endl;
                return 1;
            }
        } else {
            entity->refreshAvailableAnimationState();
            auto* node = entity->getParentSceneNode();
            int result = MeshImporterExporter::exporter(node, outFi.absoluteFilePath(), formatForExtension(outputPath));
            if (result != 0) {
                SentryReporter::captureMessage(QString("CLI anim: bake-fps export failed (.%1)").arg(outFi.suffix()), "error");
                err() << "Error: Export failed." << Qt::endl;
                return 1;
            }
        }

        cliWrite(QString("Baked %1 animation(s) at %2 FPS (%3 total keyframes)\nOutput: %4\n")
            .arg(animsProcessed).arg(bakeFps).arg(totalKeys).arg(outFi.fileName()));
        return 0;
    }

    // In-between mode (#409): fill the gap between two keyframes with predicted
    // intermediate poses (RMIB ONNX model when available, spline fallback else).
    if (inbetweenMode) {
        SentryReporter::addBreadcrumb("ai.assist.in_between",
            QString("gap=%1 anim=%2 noModel=%3")
                .arg(inbetweenGapFrames)
                .arg(animationFilter.isEmpty() ? "(all)" : animationFilter)
                .arg(inbetweenNoModel ? "yes" : "no"));

        // Resolve the model once (download on first use unless --no-model / no
        // ONNX build). Empty path → predict() uses the spline fallback.
        QString modelPath;
        if (!inbetweenNoModel)
            modelPath = MotionInbetween::ensureModelBlocking();

        std::vector<std::string> animNames;
        unsigned short numAnims = skel->getNumAnimations();
        for (unsigned short i = 0; i < numAnims; ++i)
            animNames.push_back(skel->getAnimation(i)->getName());

        int totalInserted = 0, animsProcessed = 0;
        bool anyUsedModel = false, anyUsedFallback = false;
        QString lastFallbackReason;
        for (const auto& name : animNames) {
            if (!animationFilter.isEmpty() && animationFilter.toStdString() != name)
                continue;
            Ogre::Animation* anim = skel->getAnimation(name);
            if (!anim) continue;
            // Default window = the whole clip [0, length].
            const float clipLen = anim->getLength();
            const float t0 = (inbetweenStart >= 0.0f) ? inbetweenStart : 0.0f;
            const float t1 = (inbetweenEnd   >= 0.0f) ? inbetweenEnd   : clipLen;

            const auto r = AnimationMerger::inbetweenAnimation(
                skel.get(), name, t0, t1, inbetweenGapFrames, modelPath,
                inbetweenNoModel);
            if (!r.ok) {
                err() << "Warning: in-between skipped for '"
                      << QString::fromStdString(name) << "': " << r.error << Qt::endl;
                continue;
            }
            totalInserted += r.keyframesInserted;
            ++animsProcessed;
            anyUsedModel = anyUsedModel || r.usedModel;
            anyUsedFallback = anyUsedFallback || !r.usedModel;
            if (!r.fallbackReason.isEmpty()) lastFallbackReason = r.fallbackReason;
        }

        if (animsProcessed == 0) {
            err() << "Error: No animation could be in-betweened." << Qt::endl;
            if (!animationFilter.isEmpty()) {
                err() << "Available animations:" << Qt::endl;
                for (const auto& name : animNames)
                    err() << "  " << QString::fromStdString(name) << Qt::endl;
            }
            return 1;
        }

        QFileInfo outFi(outputPath);
        if (isAnimOnlyInput) {
            QString exportErr;
            if (!exportAnimOnly(skel, outFi.absoluteFilePath(), &exportErr)) {
                err() << "Error: Export failed: " << exportErr << Qt::endl;
                return 1;
            }
        } else {
            entity->refreshAvailableAnimationState();
            auto* node = entity->getParentSceneNode();
            int result = MeshImporterExporter::exporter(node, outFi.absoluteFilePath(),
                                                        formatForExtension(outputPath));
            if (result != 0) {
                err() << "Error: Export failed." << Qt::endl;
                return 1;
            }
        }

        // Report the path honestly across all processed clips: model-only,
        // spline-only, or a mix (some clips used the model, others fell back).
        const QString via = (anyUsedModel && anyUsedFallback)
            ? QStringLiteral("RMIB model + spline fallback (mixed)")
            : anyUsedModel ? QStringLiteral("RMIB model")
                           : QStringLiteral("spline fallback");
        cliWrite(QString("In-betweened %1 animation(s): inserted %2 keyframes via %3\nOutput: %4\n")
            .arg(animsProcessed).arg(totalInserted).arg(via).arg(outFi.fileName()));
        if (anyUsedFallback && !lastFallbackReason.isEmpty())
            cliWrite(QString("Note: %1\n").arg(lastFallbackReason));
        return 0;
    }

    // Analyze / Simplify share keyframe redundancy detection.
    if (analyzeMode || simplifyMode) {
        AnimationMerger::SimplifyTolerances tol;
        tol.translation = simplifyTranslationTol;
        tol.rotationDeg = simplifyRotationDegTol;
        tol.scale       = simplifyScaleTol;

        SentryReporter::addBreadcrumb("cli.anim", QString("%1 anim=%2 tol_t=%3 tol_r=%4")
            .arg(simplifyMode ? "Simplify" : "Analyze")
            .arg(animationFilter.isEmpty() ? "(all)" : animationFilter)
            .arg(tol.translation).arg(tol.rotationDeg));

        std::vector<std::string> animNames;
        for (unsigned short i = 0; i < skel->getNumAnimations(); ++i)
            animNames.push_back(skel->getAnimation(i)->getName());

        // Pass 1: analyze every animation (both modes need totals for the report).
        struct AnimReport {
            QString name;
            int original = 0;
            int redundant = 0;
        };
        QList<AnimReport> reports;
        int totalOriginal = 0;
        int totalRedundant = 0;
        int matched = 0;

        for (const auto& name : animNames) {
            if (!animationFilter.isEmpty() && animationFilter.toStdString() != name)
                continue;
            ++matched;
            AnimReport rpt;
            rpt.name = QString::fromStdString(name);
            AnimationMerger::analyzeRedundantKeyframes(skel->getAnimation(name), tol,
                                                      &rpt.original, &rpt.redundant);
            totalOriginal += rpt.original;
            totalRedundant += rpt.redundant;
            reports.append(rpt);
        }

        if (matched == 0) {
            err() << "Error: No matching animation found." << Qt::endl;
            if (!animationFilter.isEmpty()) {
                err() << "Available animations:" << Qt::endl;
                for (const auto& name : animNames)
                    err() << "  " << QString::fromStdString(name) << Qt::endl;
            }
            return 1;
        }

        const qint64 originalSize = QFileInfo(filePath).size();
        const double pctTotal = totalOriginal > 0
            ? (100.0 * totalRedundant / totalOriginal) : 0.0;
        // The whole-file projection only makes sense when we analyzed the
        // whole file. With --animation NAME we only see one clip, so scaling
        // originalSize by its redundancy % would overstate savings. Skip the
        // projection when filtering and let the per-anim percent stand alone.
        const bool wholeFile = animationFilter.isEmpty();
        // Keyframe data dominates animation-only files; project the saved bytes
        // proportionally to the % of redundant keys. This is an estimate — actual
        // savings depend on the exporter and how much non-keyframe data the file
        // contains (mesh, materials, embedded textures).
        const qint64 projectedSize = wholeFile
            ? static_cast<qint64>(originalSize * (1.0 - (pctTotal / 100.0)))
            : originalSize;
        const qint64 savedBytes = wholeFile ? (originalSize - projectedSize) : 0;

        auto formatBytes = [](qint64 bytes) -> QString {
            if (bytes >= 1024 * 1024)
                return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
            if (bytes >= 1024)
                return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
            return QString("%1 B").arg(bytes);
        };

        if (analyzeMode) {
            if (jsonOutput) {
                QJsonObject root;
                root["file"] = QFileInfo(filePath).fileName();
                root["originalSize"] = originalSize;
                if (wholeFile) {
                    // Only include the projection when we analyzed the whole
                    // file — otherwise consumers might scale these numbers
                    // and double-count savings.
                    root["projectedSize"] = projectedSize;
                    root["savedBytes"] = savedBytes;
                }
                root["totalKeyframes"] = totalOriginal;
                root["redundantKeyframes"] = totalRedundant;
                root["redundantPercent"] = pctTotal;
                QJsonObject tolObj;
                tolObj["translation"] = tol.translation;
                tolObj["rotationDeg"] = tol.rotationDeg;
                tolObj["scale"] = tol.scale;
                root["tolerances"] = tolObj;
                QJsonArray arr;
                for (const auto& r : reports) {
                    QJsonObject obj;
                    obj["name"] = r.name;
                    obj["totalKeyframes"] = r.original;
                    obj["redundantKeyframes"] = r.redundant;
                    obj["redundantPercent"] = r.original > 0
                        ? (100.0 * r.redundant / r.original) : 0.0;
                    arr.append(obj);
                }
                root["animations"] = arr;
                cliWrite(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)) + "\n");
            } else {
                QString report;
                report += QString("Analysis: %1\n").arg(QFileInfo(filePath).fileName());
                report += QString("  Tolerances: translation=%1  rotation=%2°  scale=%3\n")
                    .arg(tol.translation).arg(tol.rotationDeg).arg(tol.scale);
                for (const auto& r : reports) {
                    double pct = r.original > 0 ? (100.0 * r.redundant / r.original) : 0.0;
                    report += QString("  %1: %2/%3 keyframes redundant (%4%)\n")
                        .arg(r.name).arg(r.redundant).arg(r.original).arg(pct, 0, 'f', 1);
                }
                report += QString("  Total: %1/%2 keyframes redundant (%3%)\n")
                    .arg(totalRedundant).arg(totalOriginal).arg(pctTotal, 0, 'f', 1);
                if (wholeFile && totalRedundant > 0 && originalSize > 0) {
                    report += QString("  Simplify to save ~%1. Original size: %2, projected size: %3\n")
                        .arg(formatBytes(savedBytes))
                        .arg(formatBytes(originalSize))
                        .arg(formatBytes(projectedSize));
                } else if (!wholeFile) {
                    report += QString("  (Whole-file size projection skipped because --animation filtered to one clip)\n");
                }
                cliWrite(report);
            }
            return 0;
        }

        // simplifyMode
        int totalRemoved = 0;
        int animsProcessed = 0;
        for (const auto& name : animNames) {
            if (!animationFilter.isEmpty() && animationFilter.toStdString() != name)
                continue;
            int removed = AnimationMerger::simplifyAnimation(skel.get(), name, tol);
            totalRemoved += removed;
            ++animsProcessed;
        }

        entity->refreshAvailableAnimationState();

        QFileInfo outFi(outputPath);
        auto* node = entity->getParentSceneNode();
        int result = MeshImporterExporter::exporter(node, outFi.absoluteFilePath(), formatForExtension(outputPath));
        if (result != 0) {
            SentryReporter::captureMessage(QString("CLI anim: simplify export failed (.%1)").arg(outFi.suffix()), "error");
            err() << "Error: Export failed." << Qt::endl;
            return 1;
        }

        // Compute the percentage from the actual removed count rather than the
        // pre-simplify analysis number — keeps the line internally consistent
        // even if simplifyAnimation and analyzeRedundantKeyframes ever drift.
        const double removedPct = totalOriginal > 0
            ? (100.0 * totalRemoved / totalOriginal) : 0.0;
        cliWrite(QString("Simplified %1 animation(s): removed %2/%3 keyframes (%4%)\nOutput: %5\n")
            .arg(animsProcessed).arg(totalRemoved).arg(totalOriginal)
            .arg(removedPct, 0, 'f', 1).arg(outFi.fileName()));
        return 0;
    }

    // Rename mode
    if (!skel->hasAnimation(oldName.toStdString())) {
        err() << "Error: Animation '" << oldName << "' not found." << Qt::endl;
        err() << "Available animations:" << Qt::endl;
        for (unsigned short i = 0; i < skel->getNumAnimations(); ++i)
            err() << "  " << QString::fromStdString(skel->getAnimation(i)->getName()) << Qt::endl;
        return 1;
    }

    if (oldName != newName && skel->hasAnimation(newName.toStdString())) {
        err() << "Error: Animation '" << newName << "' already exists." << Qt::endl;
        return 1;
    }

    AnimationMerger::renameAnimation(skel.get(), oldName.toStdString(), newName.toStdString());

    QFileInfo outFi(outputPath);
    if (isAnimOnlyInput) {
        QString exportErr;
        if (!exportAnimOnly(skel, outFi.absoluteFilePath(), &exportErr)) {
            SentryReporter::captureMessage(QString("CLI anim: rename export failed (anim-only)"), "error");
            err() << "Error: Export failed: " << exportErr << Qt::endl;
            return 1;
        }
    } else {
        entity->refreshAvailableAnimationState();
        auto* node = entity->getParentSceneNode();
        QString fmt = formatForExtension(outputPath);
        int result = MeshImporterExporter::exporter(node, outFi.absoluteFilePath(), fmt);
        if (result != 0) {
            SentryReporter::captureMessage(QString("CLI anim: rename export failed (.%1)").arg(outFi.suffix()), "error");
            err() << "Error: Export failed." << Qt::endl;
            return 1;
        }
    }

    cliWrite(QString("Renamed animation '%1' -> '%2'\nOutput: %3\n").arg(oldName, newName, outFi.fileName()));

    return 0;
}

int CLIPipeline::cmdValidate(int argc, char* argv[])
{
    // Parse: validate <file> [--json]
    QString filePath;
    bool jsonOutput = false;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "validate" || arg == "--cli") continue;
        if (arg == "--json") { jsonOutput = true; continue; }
        if (!arg.startsWith("-") && filePath.isEmpty()) { filePath = arg; continue; }
    }

    if (filePath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh validate <file> [--json]" << Qt::endl;
        return 2;
    }

    QFileInfo fi(filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << filePath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.validate",
        QString("Validate .%1%2").arg(fi.suffix(), jsonOutput ? " json=true" : ""));

    MeshImporterExporter::importer({fi.absoluteFilePath()});

    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        SentryReporter::captureMessage(
            QString("CLI validate: import failed (.%1)").arg(fi.suffix()), "error");
        err() << "Error: Failed to load file: " << filePath << Qt::endl;
        return 1;
    }

    // Select all loaded entities so MeshValidator can iterate them.
    auto* sel = SelectionSet::getSingleton();
    for (Ogre::Entity* entity : entities)
        sel->append(entity);

    MeshValidator::instance()->doValidate();
    QVariantList issues = MeshValidator::instance()->issues();

    bool hasErrors = false;
    for (const QVariant& v : issues) {
        if (v.toMap().value("type").toString() == "error")
            hasErrors = true;
    }

    if (jsonOutput) {
        QJsonArray arr;
        for (const QVariant& v : issues) {
            QVariantMap map = v.toMap();
            QJsonObject obj;
            obj["type"]        = map.value("type").toString();
            obj["description"] = map.value("description").toString();
            obj["count"]       = map.value("count").toInt();
            obj["fixable"]     = map.value("fixable").toBool();
            arr.append(obj);
        }
        cliWrite(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Indented)) + "\n");
    } else {
        QString output;
        for (const QVariant& v : issues) {
            QVariantMap map = v.toMap();
            QString type = map.value("type").toString();
            QString desc = map.value("description").toString();
            if (type == "ok")
                output += QString("OK: %1\n").arg(desc);
            else
                output += QString("[%1] %2\n").arg(type.toUpper(), desc);
        }
        cliWrite(output);
    }

    return hasErrors ? 1 : 0;
}

int CLIPipeline::cmdLod(int argc, char* argv[])
{
    // Parse: lod <file> --count N [--reductions r,...] [--algo meshopt|ogre] [-o output]
    //    or: lod <file> --auto [-o output]
    //    or: lod <file> --remove [-o output]
    //    or: lod <file> --info [--json]
    QString inputPath, outputPath;
    int lodCount = 0;
    bool autoMode   = false;
    bool removeMode = false;
    bool infoMode   = false;
    bool jsonOutput = false;
    QString algo = "ogre";      // default backend. Meshopt (#398) is opt-in via --algo meshopt.
    bool algoSpecified = false; // whether the caller passed --algo (vs default)
    QVariantList reductions;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "lod" || arg == "--cli") continue;
        if (arg == "--auto")   { autoMode   = true; continue; }
        if (arg == "--remove") { removeMode = true; continue; }
        if (arg == "--info")   { infoMode   = true; continue; }
        if (arg == "--json")   { jsonOutput = true; continue; }
        if (arg == "--count" && i + 1 < argc) {
            lodCount = QString(argv[++i]).toInt();
            continue;
        }
        if (arg == "--reductions" && i + 1 < argc) {
            for (const QString& p : QString(argv[++i]).split(','))
                reductions.append(p.trimmed().toFloat());
            continue;
        }
        if (arg == "--algo" && i + 1 < argc) {
            const QString val = QString(argv[++i]).toLower();
            if (val != "meshopt" && val != "ogre") {
                err() << "Error: --algo must be 'meshopt' or 'ogre' (got '" << val << "')." << Qt::endl;
                return 2;
            }
            algo = val;
            algoSpecified = true;
            continue;
        }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString(argv[++i]);
            continue;
        }
        if (!arg.startsWith("-") && inputPath.isEmpty()) {
            inputPath = arg;
            continue;
        }
    }

    if (inputPath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh lod <file> --count N [--reductions r,...] [--algo meshopt|ogre] [-o output]" << Qt::endl;
        err() << "       qtmesh lod <file> --auto [-o output]" << Qt::endl;
        err() << "       qtmesh lod <file> --remove [-o output]" << Qt::endl;
        err() << "       qtmesh lod <file> --info [--json]" << Qt::endl;
        return 2;
    }

    if (!autoMode && !removeMode && !infoMode && lodCount <= 0) {
        err() << "Error: Specify --count N, --auto, --remove, or --info." << Qt::endl;
        return 2;
    }

    // --algo is only meaningful for the explicit --count path. The
    // --auto / --remove / --info modes either drive Ogre's
    // auto-config heuristic or don't generate geometry at all, so
    // silently ignoring the flag would let `qtmesh lod model.fbx
    // --auto --algo meshopt` run a different backend than the
    // caller asked for. Fail fast instead.
    if (algoSpecified && (autoMode || removeMode || infoMode)) {
        err() << "Error: --algo is only supported with --count N "
                 "(--auto/--remove/--info ignore it)." << Qt::endl;
        return 2;
    }

    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << inputPath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    QString lodOp = infoMode   ? "info"
                  : autoMode   ? "auto"
                  : removeMode ? "remove"
                               : QString("count=%1").arg(lodCount);
    SentryReporter::addBreadcrumb("cli.lod",
        QString("LOD %1 .%2").arg(lodOp, fi.suffix()));

    MeshImporterExporter::importer({fi.absoluteFilePath()});

    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        SentryReporter::captureMessage(
            QString("CLI lod: import failed (.%1)").arg(fi.suffix()), "error");
        err() << "Error: Failed to load file: " << inputPath << Qt::endl;
        return 1;
    }

    // Select all loaded entities so MeshLodController can find them.
    auto* sel = SelectionSet::getSingleton();
    for (Ogre::Entity* entity : entities)
        sel->append(entity);

    Ogre::Entity* entity = entities.first();
    Ogre::MeshPtr mesh   = entity->getMesh();

    // ---- info ----
    if (infoMode) {
        QVariantList lodInfo = MeshLodController::instance()->lodLevelInfo();

        if (jsonOutput) {
            QJsonArray arr;
            for (const QVariant& v : lodInfo) {
                QVariantMap map = v.toMap();
                QJsonObject obj;
                obj["level"]     = map.value("level").toInt();
                obj["label"]     = map.value("label").toString();
                obj["triangles"] = map.value("triangles").toInt();
                arr.append(obj);
            }
            cliWrite(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Indented)) + "\n");
        } else {
            QString output = "LOD levels:\n";
            for (const QVariant& v : lodInfo) {
                QVariantMap map = v.toMap();
                output += QString("  LOD %1 (%2): %3 triangles\n")
                    .arg(map.value("level").toInt())
                    .arg(map.value("label").toString())
                    .arg(map.value("triangles").toInt());
            }
            cliWrite(output);
        }
        return 0;
    }

    // ---- remove ----
    if (removeMode) {
        MeshLodController::instance()->removeLods();

        QString outPath = outputPath.isEmpty() ? inputPath : outputPath;
        QFileInfo outFi(outPath);
        auto* node = entity->getParentSceneNode();

        if (MeshImporterExporter::exporter(node, outFi.absoluteFilePath(),
                                           formatForExtension(outPath)) != 0) {
            SentryReporter::captureMessage(
                QString("CLI lod: remove export failed (.%1)").arg(outFi.suffix()), "error");
            err() << "Error: Export failed." << Qt::endl;
            return 1;
        }

        cliWrite(QString("LOD levels removed. Saved: %1\n").arg(outFi.fileName()));
        return 0;
    }

    // ---- generate (--count N or --auto) ----
    // `--auto` always uses Ogre's auto-config path (count + distances
    // chosen by Ogre's heuristic) — meshoptimizer is invoked only when
    // the caller asked for explicit counts/reductions.
    const auto algoEnum = (algo == "meshopt")
        ? MeshLodController::Algorithm::Meshopt
        : MeshLodController::Algorithm::Ogre;
    if (autoMode) {
        MeshLodController::instance()->generateAutoLods();
    } else {
        lodCount = std::max(1, std::min(lodCount, 4));
        MeshLodController::instance()->generateLods(lodCount, reductions, algoEnum);
    }

    const unsigned int totalLods = mesh->getNumLodLevels();
    if (totalLods <= 1) {
        SentryReporter::captureMessage(
            QString("CLI lod: generation produced no levels (.%1)").arg(fi.suffix()), "error");
        err() << "Error: LOD generation produced no levels." << Qt::endl;
        return 1;
    }

    // Determine output naming
    QString outBaseName, outSuffix, outDir;
    if (outputPath.isEmpty()) {
        outDir      = fi.absolutePath();
        outBaseName = fi.completeBaseName();
        outSuffix   = fi.suffix();
    } else {
        QFileInfo outFi(outputPath);
        outDir      = outFi.absolutePath();
        outBaseName = outFi.completeBaseName();
        outSuffix   = outFi.suffix().isEmpty() ? fi.suffix() : outFi.suffix();
    }

    // Export each reduced LOD level as a separate file.
    // Temporarily swap each submesh's indexData with its LOD face list,
    // export, then restore — identical to MeshLodController::doExportLods.
    //
    // Also temporarily clear the `qtme.faces.<i>` n-gon bindings on the
    // mesh: when an FBX carries a cached EditableMesh ngon layer (set
    // up by quad-migration #326), FBXExporter prefers it over
    // SubMesh::indexData, which would silently emit the base mesh on
    // every LOD level. Save the bindings, erase them, export, restore.
    Ogre::SceneNode* sn = entity->getParentSceneNode();
    const unsigned int numSubs = mesh->getNumSubMeshes();
    int exported = 0;

    auto& userBindings = mesh->getUserObjectBindings();
    std::vector<Ogre::Any> savedNgon(numSubs);
    for (unsigned int s = 0; s < numSubs; ++s) {
        const std::string key = std::string("qtme.faces.") + std::to_string(s);
        savedNgon[s] = userBindings.getUserAny(key);
        userBindings.eraseUserAny(key);
    }

    for (unsigned int lod = 1; lod < totalLods; ++lod) {
        std::vector<Ogre::IndexData*> savedIndex(numSubs, nullptr);

        for (unsigned int s = 0; s < numSubs; ++s) {
            Ogre::SubMesh* sub = mesh->getSubMesh(s);
            savedIndex[s] = sub->indexData;
            if ((lod - 1) < sub->mLodFaceList.size())
                sub->indexData = sub->mLodFaceList[lod - 1];
        }

        const QString outPath = QDir(outDir).filePath(
            QString("%1_lod%2.%3").arg(outBaseName).arg(lod).arg(outSuffix));
        if (MeshImporterExporter::exporter(sn, outPath, formatForExtension(outPath),
                                           /*stripAnimations=*/true) == 0)
            ++exported;

        for (unsigned int s = 0; s < numSubs; ++s)
            mesh->getSubMesh(s)->indexData = savedIndex[s];
    }

    // Restore the cached n-gon bindings now that all LOD exports are
    // done — keeps the live in-memory mesh consistent with how it was
    // loaded (matters for tests that re-use the mesh in-process).
    for (unsigned int s = 0; s < numSubs; ++s) {
        if (!savedNgon[s].has_value()) continue;
        userBindings.setUserAny(
            std::string("qtme.faces.") + std::to_string(s), savedNgon[s]);
    }

    if (exported == 0) {
        SentryReporter::captureMessage(
            QString("CLI lod: all exports failed (.%1)").arg(fi.suffix()), "error");
        err() << "Error: Failed to export LOD files." << Qt::endl;
        return 1;
    }

    QString report = QString("Generated %1 LOD level(s) from %2:\n")
        .arg(totalLods - 1).arg(fi.fileName());
    for (unsigned int lod = 1; lod < totalLods; ++lod)
        report += QString("  LOD %1: %2_lod%3.%4\n").arg(lod).arg(outBaseName).arg(lod).arg(outSuffix);
    cliWrite(report);

    return 0;
}

int CLIPipeline::cmdPose(int argc, char* argv[])
{
    // Two modes:
    //   pose <file> --animation <name> --time <t> -o <output>
    //   pose <file> --animation <name> --count N -o <pattern>
    //     (skeleton-anim frame export — pre-existing path)
    //
    //   pose <library.poselib> --library list [--json]
    //     (list pose names in a `.poselib` sidecar JSON, no mesh
    //     load needed — added with D-Project. Future modes will
    //     extend this with --library apply <name> -o out.fbx
    //     once the round-trip exporter lands.)
    QString filePath, outputPath, animName;
    QString libraryOp;          // "list" or "apply"
    QString libraryApplyName;   // pose name (apply only)
    QString libraryPath;        // sidecar path (apply only; in list mode the positional is the lib)
    bool libraryMode = false;
    bool jsonOutput = false;
    float time = -1.0f;
    int count = 0;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "pose" || arg == "--cli") continue;
        if (arg == "--animation" && i + 1 < argc) {
            animName = QString(argv[++i]);
            continue;
        }
        if (arg == "--time" && i + 1 < argc) {
            time = QString(argv[++i]).toFloat();
            continue;
        }
        if (arg == "--count" && i + 1 < argc) {
            count = QString(argv[++i]).toInt();
            continue;
        }
        if (arg == "--library" && i + 1 < argc) {
            libraryMode = true;
            libraryOp = QString(argv[++i]);
            continue;
        }
        if (arg == "--apply" && i + 1 < argc) {
            libraryApplyName = QString(argv[++i]);
            continue;
        }
        if (arg == "--lib" && i + 1 < argc) {
            libraryPath = QString(argv[++i]);
            continue;
        }
        if (arg == "--json") { jsonOutput = true; continue; }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString(argv[++i]);
            continue;
        }
        if (!arg.startsWith("-") && filePath.isEmpty()) {
            filePath = arg;
            continue;
        }
    }

    if (libraryMode && libraryOp == QStringLiteral("apply")) {
        // Apply mode: positional is the MESH file, --lib is the
        // sidecar, --apply names the pose, -o is the output mesh.
        //   qtmesh pose <mesh> --library apply --lib <lib.poselib>
        //                      --apply <name> -o <out>
        if (filePath.isEmpty()) {
            err() << "Error: No input mesh specified." << Qt::endl;
            err() << "Usage: qtmesh pose <mesh> --library apply --lib <lib.poselib> --apply <name> -o <out>" << Qt::endl;
            return 2;
        }
        if (libraryPath.isEmpty()) {
            err() << "Error: --lib <library.poselib> is required for --library apply." << Qt::endl;
            return 2;
        }
        if (libraryApplyName.isEmpty()) {
            err() << "Error: --apply <pose-name> is required for --library apply." << Qt::endl;
            return 2;
        }
        if (outputPath.isEmpty()) {
            err() << "Error: -o <output> is required for --library apply." << Qt::endl;
            return 2;
        }
        QFileInfo meshFi(filePath);
        QFileInfo libFi(libraryPath);
        if (!meshFi.exists()) {
            err() << "Error: Mesh file not found: " << filePath << Qt::endl;
            return 1;
        }
        if (!libFi.exists()) {
            err() << "Error: Library file not found: " << libraryPath << Qt::endl;
            return 1;
        }

        if (!initOgreHeadless()) return 1;

        SentryReporter::addBreadcrumb("cli.pose",
            QString("Library apply '%1' on .%2").arg(libraryApplyName).arg(meshFi.suffix()));
        SentryReporter::addBreadcrumb("file.import",
            QString("Importing %1 for pose apply").arg(meshFi.absoluteFilePath()));

        MeshImporterExporter::importer({meshFi.absoluteFilePath()});
        auto& movables = Manager::getSingleton()->getEntities();
        Ogre::Entity* entity = nullptr;
        Ogre::Entity* firstAnyEntity = nullptr;
        // CodeRabbit Major on PR #606: prefer the first SKINNED
        // entity. Multi-entity imports often include an unskinned
        // helper (collision proxy, prop) first; if we picked it,
        // apply would fail with "no skeleton" even though a valid
        // rigged mesh loaded later in the scene. Track both so we
        // can give a precise error: "no entity at all" vs "found
        // entities but none skinned".
        for (auto* obj : movables) {
            if (!obj || obj->getMovableType() != "Entity") continue;
            auto* e = static_cast<Ogre::Entity*>(obj);
            if (!firstAnyEntity) firstAnyEntity = e;
            if (e->hasSkeleton()) {
                entity = e;
                break;
            }
        }
        if (!firstAnyEntity) {
            err() << "Error: Failed to load mesh: " << filePath << Qt::endl;
            return 1;
        }
        if (!entity) {
            err() << "Error: Loaded mesh has no skinned entity — cannot apply a pose." << Qt::endl;
            return 1;
        }

        auto* lib = PoseLibrary::instance();
        if (!lib->loadPoseLibrary(entity, libFi.absoluteFilePath())) {
            err() << "Error: Failed to load pose library: " << libraryPath << Qt::endl;
            return 1;
        }
        if (!lib->hasPose(entity, libraryApplyName)) {
            err() << "Error: Pose '" << libraryApplyName
                  << "' not found in library." << Qt::endl;
            err() << "Available poses:" << Qt::endl;
            for (const QString& n : lib->listPoses(entity))
                err() << "  " << n << Qt::endl;
            return 1;
        }
        if (!lib->applyPose(entity, libraryApplyName)) {
            err() << "Error: Failed to apply pose '" << libraryApplyName << "'." << Qt::endl;
            return 1;
        }

        // Export with skeleton in posed state. exportCurrentPose
        // is the same path the --animation/--time mode uses; for
        // FBX/glTF it preserves the skeleton, for STL/OBJ it
        // bakes the posed mesh down to triangles.
        QFileInfo outFi(outputPath);
        SentryReporter::addBreadcrumb("file.export",
            QString("Exporting posed mesh to %1").arg(outFi.absoluteFilePath()));
        const int result = MeshImporterExporter::exportCurrentPose(entity, outFi.absoluteFilePath());
        if (result != 0) {
            err() << "Error: Export failed: " << outputPath << Qt::endl;
            return 1;
        }
        cliWrite(QStringLiteral("Wrote %1\n").arg(outFi.absoluteFilePath()));
        return 0;
    }

    if (libraryMode) {
        // Library mode: filePath is the .poselib sidecar, not a
        // mesh. The PoseLibrary singleton needs an entity to key
        // poses against; we use a temporary "CLI pose-list anchor"
        // entity for this — no skeleton, just an anchor.
        if (libraryOp != QStringLiteral("list")) {
            err() << "Error: --library supports 'list' or 'apply'." << Qt::endl;
            return 2;
        }
        if (filePath.isEmpty()) {
            err() << "Error: No input file specified." << Qt::endl;
            err() << "Usage: qtmesh pose <library.poselib> --library list [--json]" << Qt::endl;
            return 2;
        }
        QFileInfo libFi(filePath);
        if (!libFi.exists()) {
            err() << "Error: File not found: " << filePath << Qt::endl;
            return 1;
        }
        // Read JSON directly — we don't need an Ogre scene at all.
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly)) {
            err() << "Error: Cannot open " << filePath << Qt::endl;
            return 1;
        }
        const QByteArray bytes = f.readAll();
        f.close();
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
            err() << "Error: malformed JSON in " << filePath << Qt::endl;
            return 1;
        }
        const QJsonObject root = doc.object();
        if (root.value("schema").toString() != QStringLiteral("qtmesheditor.poselib.v1")) {
            err() << "Error: schema mismatch in " << filePath << Qt::endl;
            return 1;
        }
        // Validate `poses` is actually an array — a schema-matching
        // file with `poses` missing or non-array would otherwise
        // silently report "No poses in library" with exit 0,
        // masking corruption (Codex P2 on PR #604, same bug class
        // as the loadPoseLibrary fix in #603).
        const QJsonValue posesV = root.value("poses");
        if (!posesV.isArray()) {
            err() << "Error: malformed 'poses' field in " << filePath << Qt::endl;
            return 1;
        }
        const QJsonArray poses = posesV.toArray();
        QStringList names;
        for (const QJsonValue& v : poses) {
            const QString n = v.toObject().value("name").toString();
            if (!n.isEmpty()) names << n;
        }

        SentryReporter::addBreadcrumb("cli.pose",
            QString("Library list .%1 (%2 poses)").arg(libFi.suffix()).arg(names.size()));

        if (jsonOutput) {
            QJsonArray arr;
            for (const QString& n : names) arr.append(n);
            QJsonObject out;
            out["file"]  = filePath;
            out["count"] = static_cast<int>(names.size());
            out["poses"] = arr;
            cliWrite(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Indented)));
        } else {
            if (names.isEmpty()) {
                cliWrite(QStringLiteral("No poses in library.\n"));
            } else {
                cliWrite(QStringLiteral("Poses (%1):\n").arg(names.size()));
                for (const QString& n : names)
                    cliWrite(QStringLiteral("  %1\n").arg(n));
            }
        }
        return 0;
    }

    if (filePath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh pose <file> --animation <name> --time <t> -o <output>" << Qt::endl;
        err() << "       qtmesh pose <file> --animation <name> --count N -o <pattern>" << Qt::endl;
        return 2;
    }

    if (animName.isEmpty()) {
        err() << "Error: --animation <name> is required." << Qt::endl;
        return 2;
    }

    if (outputPath.isEmpty()) {
        err() << "Error: -o <output> is required." << Qt::endl;
        return 2;
    }

    if (time < 0.0f && count <= 0) {
        err() << "Error: Specify --time <t> or --count <N>." << Qt::endl;
        return 2;
    }

    QFileInfo fi(filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << filePath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.pose", QString("Pose export .%1 anim=%2")
        .arg(fi.suffix(), animName));

    MeshImporterExporter::importer({fi.absoluteFilePath()});

    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        SentryReporter::captureMessage(QString("CLI pose: import failed (.%1)").arg(fi.suffix()), "error");
        err() << "Error: Failed to load file: " << filePath << Qt::endl;
        return 1;
    }

    Ogre::Entity* entity = entities.first();
    if (!entity->hasSkeleton()) {
        err() << "Error: File has no skeleton — cannot export pose." << Qt::endl;
        return 1;
    }

    // Find the animation
    auto* animStates = entity->getAllAnimationStates();
    if (!animStates || !animStates->hasAnimationState(animName.toStdString())) {
        err() << "Error: Animation '" << animName << "' not found." << Qt::endl;
        err() << "Available animations:" << Qt::endl;
        Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
        if (skel) {
            for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai)
                err() << "  " << QString::fromStdString(skel->getAnimation(ai)->getName()) << Qt::endl;
        }
        return 1;
    }

    auto* animState = animStates->getAnimationState(animName.toStdString());
    float animLength = animState->getLength();

    if (count > 0) {
        // Export multiple evenly-spaced frames
        int exported = 0;
        for (int f = 0; f < count; ++f) {
            float t = (count == 1) ? 0.0f : (animLength * f / (count - 1));

            // Enable animation at the desired time
            animState->setEnabled(true);
            animState->setTimePosition(t);

            // Build output path with frame number substitution
            // Support printf-style patterns like pose_%02d.stl
            QString framePath;
            if (outputPath.contains('%')) {
                // Use sprintf-style formatting
                char buf[1024];
                snprintf(buf, sizeof(buf), outputPath.toUtf8().constData(), f);
                framePath = QString::fromUtf8(buf);
            } else {
                // Insert frame number before extension
                QFileInfo outFi(outputPath);
                framePath = QString("%1/%2_%3.%4")
                    .arg(outFi.path(), outFi.completeBaseName())
                    .arg(f, 2, 10, QChar('0'))
                    .arg(outFi.suffix());
            }

            int result = MeshImporterExporter::exportCurrentPose(entity, framePath);
            if (result == 0) {
                ++exported;
                cliWrite(QString("Exported frame %1/%2 (t=%3s): %4\n")
                    .arg(f + 1).arg(count)
                    .arg(t, 0, 'f', 3)
                    .arg(QFileInfo(framePath).fileName()));
            } else {
                err() << "Error: Failed to export frame " << (f + 1) << Qt::endl;
            }

            animState->setEnabled(false);
        }

        if (exported == 0) {
            err() << "Error: All frame exports failed." << Qt::endl;
            return 1;
        }

        return 0;

    } else {
        // Export single frame at specified time
        animState->setEnabled(true);
        animState->setTimePosition(time);

        int result = MeshImporterExporter::exportCurrentPose(entity, outputPath);
        animState->setEnabled(false);

        if (result != 0) {
            SentryReporter::captureMessage(
                QString("CLI pose: export failed (.%1)").arg(fi.suffix()), "error");
            err() << "Error: Export failed." << Qt::endl;
            return 1;
        }

        cliWrite(QString("Exported pose (t=%1s): %2\n")
            .arg(time, 0, 'f', 3)
            .arg(QFileInfo(outputPath).fileName()));
        return 0;
    }
}

int CLIPipeline::cmdTurntable(int argc, char* argv[])
{
    // turntable <file> -o <output> [--frames N] [--size WxH] [--width W] [--height H]
    //                     [--columns C] [--axis y|x|z] [--elevation deg] [--camera-height deg] [--json]
    QString inputPath, outputPath;
    int frameCount = 12;
    int width = 512;
    int height = 512;
    int columns = 0;
    float elevation = 20.0f;
    bool jsonOutput = false;
    TurntableAxis axis = TurntableAxis::Y;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "turntable" || arg == "--cli")
            continue;
        if (arg == "--json") {
            jsonOutput = true;
            continue;
        }
        if (arg == "-o" && i + 1 < argc) {
            outputPath = QString(argv[++i]);
            continue;
        }
        if (arg == "--frames" && i + 1 < argc) {
            if (!parseCliInt(QString(argv[++i]), &frameCount)) {
                err() << "Error: Invalid value for --frames." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--columns" && i + 1 < argc) {
            if (!parseCliInt(QString(argv[++i]), &columns)) {
                err() << "Error: Invalid value for --columns." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--width" && i + 1 < argc) {
            if (!parseCliInt(QString(argv[++i]), &width)) {
                err() << "Error: Invalid value for --width." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--height" && i + 1 < argc) {
            if (!parseCliInt(QString(argv[++i]), &height)) {
                err() << "Error: Invalid value for --height." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--size" && i + 1 < argc) {
            const QString sizeArg = QString(argv[++i]);
            const int xPos = sizeArg.indexOf(QLatin1Char('x'));
            if (xPos > 0) {
                if (!parseCliInt(sizeArg.left(xPos), &width) || !parseCliInt(sizeArg.mid(xPos + 1), &height)) {
                    err() << "Error: Invalid value for --size (expected WxH)." << Qt::endl;
                    return 2;
                }
            } else if (!parseCliInt(sizeArg, &width)) {
                err() << "Error: Invalid value for --size." << Qt::endl;
                return 2;
            } else {
                height = width;
            }
            continue;
        }
        if (arg == "--elevation" && i + 1 < argc) {
            if (!parseCliFloat(QString(argv[++i]), &elevation)) {
                err() << "Error: Invalid value for --elevation." << Qt::endl;
                return 2;
            }
            continue;
        }
        if ((arg == "--camera-height" || arg == "--camera_height") && i + 1 < argc) {
            if (!parseCliFloat(QString(argv[++i]), &elevation)) {
                err() << "Error: Invalid value for --camera-height." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--axis" && i + 1 < argc) {
            TurntableAxis parsed = TurntableAxis::Y;
            if (!ModelTurntableRenderer::parseAxis(QString(argv[++i]), &parsed)) {
                err() << "Error: --axis must be y, x, or z." << Qt::endl;
                return 2;
            }
            axis = parsed;
            continue;
        }
        if (!arg.startsWith(QLatin1Char('-')) && inputPath.isEmpty()) {
            inputPath = arg;
            continue;
        }
    }

    if (inputPath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh turntable <file> -o <output> [--frames N] [--size WxH]" << Qt::endl;
        return 2;
    }
    if (outputPath.isEmpty()) {
        err() << "Error: Output path required (-o)." << Qt::endl;
        err() << "Usage: qtmesh turntable <file> -o <output.png> [--frames N]" << Qt::endl;
        return 2;
    }

    const bool sequenceOutput = turntableUsesFrameSequencePattern(outputPath);
    if (sequenceOutput) {
        QString patternError;
        if (!validateTurntableSequencePattern(outputPath, &patternError)) {
            err() << "Error: " << patternError << Qt::endl;
            return 2;
        }
    }

    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << inputPath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless())
        return 1;

    SentryReporter::addBreadcrumb("cli.turntable",
                                  QString("Turntable .%1 frames=%2").arg(fi.suffix()).arg(frameCount));
    SentryReporter::addBreadcrumb("file.import", fi.absoluteFilePath());

    MeshImporterExporter::importer({fi.absoluteFilePath()});

    QList<Ogre::Entity *> entityList;
    for (auto *obj : Manager::getSingleton()->getEntities()) {
        if (obj && obj->getMovableType() == "Entity")
            entityList.append(static_cast<Ogre::Entity *>(obj));
    }
    if (entityList.isEmpty()) {
        SentryReporter::captureMessage(QString("CLI turntable: import failed (.%1)").arg(fi.suffix()),
                                       "error");
        err() << "Error: Failed to load file: " << inputPath << Qt::endl;
        return 1;
    }

    TurntableOptions options;
    options.width = width;
    options.height = height;
    options.frameCount = qBound(1, frameCount, 360);
    options.axis = axis;
    options.elevationDegrees = elevation;

    QList<QImage> frames;
    QString renderError;
    if (!ModelTurntableRenderer::renderToImages(entityList, options, &frames, &renderError)) {
        ModelTurntableRenderer::shutdown();
        err() << "Error: " << renderError << Qt::endl;
        return 1;
    }

    QStringList writtenPaths;

    if (sequenceOutput) {
        for (int f = 0; f < frames.size(); ++f) {
            const QString framePath = formatTurntableFramePath(outputPath, f);
            if (!frames.at(f).save(framePath)) {
                ModelTurntableRenderer::shutdown();
                err() << "Error: Failed to write " << framePath << Qt::endl;
                return 1;
            }
            writtenPaths << framePath;
        }
    } else if (frames.size() == 1) {
        if (!frames.first().save(outputPath)) {
            ModelTurntableRenderer::shutdown();
            err() << "Error: Failed to write " << outputPath << Qt::endl;
            return 1;
        }
        writtenPaths << outputPath;
    } else {
        const QImage sheet = ModelTurntableRenderer::composeSpriteSheet(frames, columns);
        if (sheet.isNull() || !sheet.save(outputPath)) {
            ModelTurntableRenderer::shutdown();
            err() << "Error: Failed to write sprite sheet " << outputPath << Qt::endl;
            return 1;
        }
        writtenPaths << outputPath;
    }

    ModelTurntableRenderer::shutdown();

    for (const QString& written : writtenPaths)
        SentryReporter::addBreadcrumb("file.export", written);

    if (jsonOutput) {
        QJsonObject root;
        root["input"] = fi.absoluteFilePath();
        root["frames"] = frames.size();
        root["width"] = width;
        root["height"] = height;
        root["elevation"] = elevation;
        root["axis"] = axis == TurntableAxis::X ? "x" : axis == TurntableAxis::Z ? "z" : "y";
        root["sequence"] = sequenceOutput;
        QJsonArray paths;
        for (const QString &p : writtenPaths)
            paths.append(p);
        root["outputs"] = paths;
        cliWrite(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)) + "\n");
    } else {
        if (sequenceOutput) {
            cliWrite(QString("Wrote %1 turntable frame(s) to %2\n")
                         .arg(writtenPaths.size())
                         .arg(QFileInfo(outputPath).absolutePath()));
        } else if (frames.size() == 1) {
            cliWrite(QString("Wrote turntable PNG: %1\n").arg(QFileInfo(outputPath).fileName()));
        } else {
            cliWrite(QString("Wrote turntable sprite sheet (%1 frames): %2\n")
                         .arg(frames.size())
                         .arg(QFileInfo(outputPath).fileName()));
        }
    }

    return 0;
}

int CLIPipeline::cmdIsometric(int argc, char* argv[])
{
    IsometricCliParams params;
    if (const int parseRc = parseIsometricCliArgs(argc, argv, &params); parseRc != 0)
        return parseRc;

    const QFileInfo fi(params.inputPath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << params.inputPath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless())
        return 1;

    SentryReporter::addBreadcrumb("ui.action",
                                  QString("Isometric .%1 dirs=%2 frames=%3 anim=%4")
                                      .arg(fi.suffix())
                                      .arg(params.directionCount)
                                      .arg(params.frameCount)
                                      .arg(params.animationName.isEmpty() ? QStringLiteral("static")
                                                                            : params.animationName));
    SentryReporter::addBreadcrumb("file.import", fi.absoluteFilePath());

    MeshImporterExporter::importer({fi.absoluteFilePath()});

    QList<Ogre::Entity *> entityList;
    for (auto *obj : Manager::getSingleton()->getEntities()) {
        if (obj && obj->getMovableType() == "Entity")
            entityList.append(static_cast<Ogre::Entity *>(obj));
    }
    if (entityList.isEmpty()) {
        SentryReporter::captureMessage(QString("CLI isometric: import failed (.%1)").arg(fi.suffix()),
                                       "error");
        err() << "Error: Failed to load file: " << params.inputPath << Qt::endl;
        return 1;
    }

    Ogre::Entity *animatedEntity = nullptr;
    if (!params.animationName.isEmpty()) {
        animatedEntity =
            ModelIsometricRenderer::findEntityWithAnimation(entityList, params.animationName);
        if (!animatedEntity) {
            err() << "Error: --animation requires a skinned mesh with clip '" << params.animationName
                  << "'." << Qt::endl;
            err() << "Available animations:" << Qt::endl;
            err() << ModelIsometricRenderer::formatAvailableAnimations(entityList);
            return 1;
        }
    }

    IsometricOptions options;
    options.width = params.width;
    options.height = params.height;
    options.elevationDegrees = params.elevation;
    options.directionCount = qBound(1, params.directionCount, 64);
    options.startAzimuthDegrees = params.startAzimuth;
    options.cameraDistance = params.cameraDistance;
    options.cameraPadding = params.cameraPadding;

    QList<QList<QImage>> grid;
    if (QString renderError;
        !ModelIsometricRenderer::renderToGrid(entityList, animatedEntity, params.animationName,
                                              params.frameCount, options, &grid, &renderError)) {
        ModelIsometricRenderer::shutdown();
        err() << "Error: " << renderError << Qt::endl;
        if (renderError.contains(QStringLiteral("not found"))) {
            err() << "Available animations:" << Qt::endl;
            err() << ModelIsometricRenderer::formatAvailableAnimations(entityList);
        }
        return 1;
    }

    const QImage sheet = ModelIsometricRenderer::composeDirectionGrid(grid);
    if (sheet.isNull() || !sheet.save(params.outputPath)) {
        ModelIsometricRenderer::shutdown();
        err() << "Error: Failed to write isometric sprite sheet " << params.outputPath << Qt::endl;
        return 1;
    }

    ModelIsometricRenderer::shutdown();
    SentryReporter::addBreadcrumb("file.export", QFileInfo(params.outputPath).absoluteFilePath());

    const int dirs = static_cast<int>(grid.size());
    const int frames = dirs > 0 ? static_cast<int>(grid.first().size()) : 0;

    if (params.jsonOutput) {
        QJsonObject root;
        root["input"] = fi.absoluteFilePath();
        root["output"] = QFileInfo(params.outputPath).absoluteFilePath();
        root["directions"] = dirs;
        root["frames"] = frames;
        root["cellWidth"] = params.width;
        root["cellHeight"] = params.height;
        if (params.width == params.height)
            root["resolution"] = params.width;
        root["sheetWidth"] = sheet.width();
        root["sheetHeight"] = sheet.height();
        root["elevation"] = params.elevation;
        root["startAzimuth"] = params.startAzimuth;
        if (params.cameraDistance > 0.0f)
            root["cameraDistance"] = params.cameraDistance;
        else
            root["cameraPadding"] = params.cameraPadding;
        root["directionOrder"] = ModelIsometricRenderer::directionOrderConvention();
        if (!params.animationName.isEmpty())
            root["animation"] = params.animationName;
        cliWrite(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)) + "\n");
    } else {
        cliWrite(QString("Wrote isometric sprite sheet (%1 directions × %2 frames): %3\n")
                     .arg(dirs)
                     .arg(frames)
                     .arg(QFileInfo(params.outputPath).fileName()));
    }

    return 0;
}

int CLIPipeline::cmdMaterial(int argc, char* argv[])
{
    // Parse:
    //   material <file> --preset <name> [-o <output>]
    //   material <file> --generate-texture <prompt> [--model <name>]
    //                   [--controlnet <path>] [--controlnet-strength <0..1>]
    //                   [--width N] [--height N] [-o <output>]
    //   material <file> --list-presets
    //   material --list-presets
    QString inputPath, outputPath, presetName;
    QString genPrompt, sdModel, controlNetPath;
    double controlStrength = 0.9;
    int genWidth = 512, genHeight = 512;
    bool listPresets = false;
    // #406: LLM-assisted material authoring from a natural-language prompt.
    QString describePrompt, llmModel;
    // #404 PBR map synthesis from a diffuse texture.
    QString pbrAlbedo;
    bool generatePbr = false;
    int pbrTileSize = 256;
    bool pbrNoNormal = false, pbrNoRoughness = false, pbrNoHeight = false;
    // #405 Real-ESRGAN upscaling (shares --texture as the input).
    int upscaleFactor = 0;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "material" || arg == "--cli") continue;
        if (arg == "--list-presets") { listPresets = true; continue; }
        if (arg == "--preset" && i + 1 < argc) {
            presetName = QString(argv[++i]);
            continue;
        }
        if (arg == "--generate-texture" && i + 1 < argc) {
            genPrompt = QString(argv[++i]);
            continue;
        }
        if (arg == "--describe" && i + 1 < argc) {
            describePrompt = QString(argv[++i]);
            continue;
        }
        if (arg == "--generate-pbr") { generatePbr = true; continue; }
        if (arg == "--upscale" && i + 1 < argc) {
            bool usOk = false;
            upscaleFactor = QString(argv[++i]).toInt(&usOk);
            if (!usOk) {
                err() << "Error: --upscale must be an integer (2 or 4)." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--texture" && i + 1 < argc) {
            pbrAlbedo = QString(argv[++i]);
            continue;
        }
        if (arg == "--tile-size" && i + 1 < argc) {
            bool tsOk = false;
            pbrTileSize = QString(argv[++i]).toInt(&tsOk);
            if (!tsOk) {
                err() << "Error: --tile-size must be an integer." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--no-normal")    { pbrNoNormal = true; continue; }
        if (arg == "--no-roughness") { pbrNoRoughness = true; continue; }
        if (arg == "--no-height")    { pbrNoHeight = true; continue; }
        if (arg == "--model" && i + 1 < argc) {
            sdModel = QString(argv[++i]);
            continue;
        }
        if (arg == "--controlnet" && i + 1 < argc) {
            controlNetPath = QString(argv[++i]);
            continue;
        }
        if (arg == "--controlnet-strength" && i + 1 < argc) {
            controlStrength = QString(argv[++i]).toDouble();
            continue;
        }
        if (arg == "--width" && i + 1 < argc) {
            genWidth = QString(argv[++i]).toInt();
            continue;
        }
        if (arg == "--height" && i + 1 < argc) {
            genHeight = QString(argv[++i]).toInt();
            continue;
        }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString(argv[++i]);
            continue;
        }
        if (!arg.startsWith("-") && inputPath.isEmpty()) {
            inputPath = arg;
            continue;
        }
    }

    // Depth-conditioned mesh-aware texture generation (issue #403). Distinct
    // enough (async SD worker, model loading, depth RTT) to live in its own
    // helper; presets continue below.
    // #405: Real-ESRGAN texture upscaling (--texture in, -o out).
    if (upscaleFactor != 0) {
        return cmdMaterialUpscale(pbrAlbedo, outputPath, upscaleFactor);
    }

    // #404: PBR map synthesis (normal/roughness/height) from a diffuse via ONNX.
    if (generatePbr) {
        return cmdMaterialGeneratePbr(pbrAlbedo, inputPath, outputPath,
                                      pbrTileSize, !pbrNoNormal,
                                      !pbrNoRoughness, !pbrNoHeight);
    }

    if (!genPrompt.isEmpty()) {
        return cmdMaterialGenerateTexture(inputPath, outputPath, genPrompt,
                                          sdModel, controlNetPath,
                                          controlStrength, genWidth, genHeight);
    }

    // #406: LLM-assisted material from a natural-language description. --model
    // selects the GGUF model (shared flag with the SD path; harmless overlap
    // since the two are never used together in one invocation).
    if (!describePrompt.isEmpty()) {
        llmModel = sdModel;
        return cmdMaterialDescribe(inputPath, outputPath, describePrompt, llmModel);
    }

    auto* lib = MaterialPresetLibrary::instance();
    const QStringList names = lib->presetNames();

    // --list-presets is a standalone op: dump names and exit.
    if (listPresets) {
        QString out;
        for (const QString& n : names) out += n + "\n";
        cliWrite(out);
        return 0;
    }

    if (inputPath.isEmpty() || presetName.isEmpty()) {
        err() << "Error: Missing required arguments." << Qt::endl;
        err() << "Usage: qtmesh material <file> --preset <name> [-o <output>]" << Qt::endl;
        err() << "       qtmesh material <file> --describe \"<description>\" [--model <name>] [-o <output>]" << Qt::endl;
        err() << "       qtmesh material --list-presets" << Qt::endl;
        return 2;
    }

    if (!names.contains(presetName)) {
        err() << "Error: Unknown preset '" << presetName << "'." << Qt::endl;
        err() << "Available presets:" << Qt::endl;
        for (const QString& n : names)
            err() << "  " << n << Qt::endl;
        return 2;
    }

    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << inputPath << Qt::endl;
        return 1;
    }

    if (outputPath.isEmpty()) outputPath = inputPath;
    QFileInfo outFi(outputPath);

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.material",
        QString("Apply preset '%1' to .%2 -> .%3")
            .arg(presetName, fi.suffix(), outFi.suffix()));
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing file %1").arg(fi.absoluteFilePath()));

    MeshImporterExporter::importer({fi.absoluteFilePath()});

    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        SentryReporter::captureMessage(
            QString("CLI material: import failed (.%1)").arg(fi.suffix()), "error");
        err() << "Error: Failed to load file: " << inputPath << Qt::endl;
        return 1;
    }

    // Select all loaded entities so applyPreset finds them.
    // Manager::getEntities() returns all attached objects including
    // ManualObjects; check the movable type before casting to avoid
    // a crash on non-Entity items.
    auto* sel = SelectionSet::getSingleton();
    int selectedEntityCount = 0;
    for (auto* obj : entities) {
        if (!obj || obj->getMovableType() != "Entity")
            continue;
        sel->append(obj);
        ++selectedEntityCount;
    }
    if (selectedEntityCount == 0) {
        err() << "Error: No Ogre Entity objects found in imported scene." << Qt::endl;
        return 1;
    }

    lib->applyPreset(presetName);

    Ogre::Entity* entity = entities.first();
    Ogre::SceneNode* node = entity->getParentSceneNode();

    SentryReporter::addBreadcrumb("file.export",
        QString("Exporting file %1").arg(outFi.absoluteFilePath()));

    int result = MeshImporterExporter::exporter(node, outFi.absoluteFilePath(),
                                                formatForExtension(outputPath));
    if (result != 0) {
        SentryReporter::captureMessage(
            QString("CLI material: export failed (.%1)").arg(outFi.suffix()), "error");
        err() << "Error: Export failed." << Qt::endl;
        return 1;
    }

    // Write a sidecar .material file alongside the output mesh so that
    // engines/tools that expect Ogre material scripts can pick up the preset.
    // Sidecar generation is part of the core feature contract — failures
    // here propagate as a non-zero exit code, not silent success.
    const std::string matName = ("Preset/" + presetName).toStdString();
    auto matMgr = Ogre::MaterialManager::getSingletonPtr();
    if (!matMgr || !matMgr->resourceExists(matName)) {
        err() << "Error: Material resource not found for sidecar export: "
              << QString::fromStdString(matName) << Qt::endl;
        return 1;
    }
    Ogre::MaterialPtr mat = matMgr->getByName(matName);
    if (!mat) {
        err() << "Error: Failed to resolve material for sidecar export: "
              << QString::fromStdString(matName) << Qt::endl;
        return 1;
    }
    Ogre::MaterialSerializer ms;
    ms.queueForExport(mat, false, false, matName);
    const QString matText = QString::fromStdString(ms.getQueuedAsString());

    const QString sidecarPath = QDir(outFi.absolutePath())
        .filePath(outFi.completeBaseName() + ".material");
    QFile matFile(sidecarPath);
    if (!matFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        err() << "Error: Failed to write material sidecar: " << sidecarPath << Qt::endl;
        return 1;
    }
    matFile.write(matText.toUtf8());
    matFile.close();
    SentryReporter::addBreadcrumb("file.export",
        QString("Wrote material sidecar %1").arg(sidecarPath));

    cliWrite(QString("Applied preset '%1' to %2 (%3 entit%4). Saved: %5\n")
        .arg(presetName)
        .arg(fi.fileName())
        .arg(selectedEntityCount)
        .arg(selectedEntityCount == 1 ? "y" : "ies")
        .arg(outFi.fileName()));

    return 0;
}

int CLIPipeline::cmdMaterialGenerateTexture(const QString& inputPath,
                                            QString outputPath,
                                            const QString& prompt,
                                            const QString& modelName,
                                            QString controlNetPath,
                                            double controlStrength,
                                            int width, int height)
{
#ifndef ENABLE_STABLE_DIFFUSION
    Q_UNUSED(inputPath); Q_UNUSED(outputPath); Q_UNUSED(prompt);
    Q_UNUSED(modelName); Q_UNUSED(controlNetPath); Q_UNUSED(controlStrength);
    Q_UNUSED(width); Q_UNUSED(height);
    err() << "Error: this build was compiled without AI texture generation "
             "(rebuild with -DENABLE_STABLE_DIFFUSION=ON)." << Qt::endl;
    return 1;
#else
    if (inputPath.isEmpty()) {
        err() << "Error: missing <file> for --generate-texture." << Qt::endl;
        return 2;
    }
    if (width < 64 || width > 2048 || height < 64 || height > 2048) {
        err() << "Error: --width/--height must be between 64 and 2048." << Qt::endl;
        return 2;
    }
    controlStrength = std::clamp(controlStrength, 0.0, 1.0);

    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << inputPath << Qt::endl;
        return 1;
    }
    if (outputPath.isEmpty()) outputPath = inputPath;
    const QFileInfo outFi(outputPath);

    if (!controlNetPath.isEmpty() && !QFileInfo(controlNetPath).isFile()) {
        err() << "Error: ControlNet model not found: " << controlNetPath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.mesh_texture"),
        QStringLiteral("CLI generate-texture .%1 strength=%2 size=%3x%4")
            .arg(fi.suffix()).arg(controlStrength).arg(width).arg(height));

    MeshImporterExporter::importer({fi.absoluteFilePath()});
    auto& entities = Manager::getSingleton()->getEntities();
    Ogre::Entity* entity = nullptr;
    for (auto* obj : entities) {
        if (obj && obj->getMovableType() == "Entity") {
            entity = static_cast<Ogre::Entity*>(obj);
            break;
        }
    }
    if (!entity || !entity->getMesh()) {
        err() << "Error: Failed to load a mesh from: " << inputPath << Qt::endl;
        return 1;
    }

    SDManager* sd = SDManager::instance();
    if (!sd) {
        err() << "Error: AI texture generation unavailable." << Qt::endl;
        return 1;
    }

    // Resolve and load a base SD model synchronously. --model overrides;
    // otherwise fall back to the last-used / first available model.
    sd->scanForModels();
    QString chosenModel = modelName;
    if (chosenModel.isEmpty()) chosenModel = sd->lastModelName();
    if (chosenModel.isEmpty()) {
        const QStringList avail = sd->availableModels();
        if (!avail.isEmpty()) chosenModel = avail.first();
    }
    if (chosenModel.isEmpty()) {
        err() << "Error: no SD model found in the models directory ("
              << sd->modelsDirectory() << "). Place a .safetensors/.ckpt/.gguf "
                 "base model there or pass --model." << Qt::endl;
        return 1;
    }
    if (!sd->isModelLoaded() || sd->currentModelName() != chosenModel) {
        QEventLoop loadLoop;
        bool loadOk = false;
        QString loadErr;
        QObject::connect(sd, &SDManager::modelLoadCompleted, &loadLoop,
            [&](const QString&) { loadOk = true; loadLoop.quit(); });
        QObject::connect(sd, &SDManager::modelLoadError, &loadLoop,
            [&](const QString& e) { loadErr = e; loadLoop.quit(); });
        sd->loadModel(chosenModel);
        loadLoop.exec();
        if (!loadOk) {
            err() << "Error: failed to load SD model '" << chosenModel << "': "
                  << loadErr << Qt::endl;
            return 1;
        }
    }

    // Render the depth map from the front view (same path GUI/MCP use).
    QString depthErr;
    const int depthSize = std::max(width, height);
    const QImage depth = MeshDepthRenderer::renderDepthMap(entity, depthSize, &depthErr);
    if (depth.isNull()) {
        err() << "Error: depth render failed: " << depthErr << Qt::endl;
        return 1;
    }

    // Auto-discover an SD1.5 depth ControlNet if none was given (same heuristic
    // as the MCP tool); missing one degrades to plain txt2img.
    if (controlNetPath.isEmpty()) {
        QDir d(sd->modelsDirectory());
        const QStringList files = d.entryList(
            QStringList() << "*.safetensors" << "*.ckpt", QDir::Files);
        auto isSdxl = [](const QString& l) {
            return l.contains("sdxl") || l.contains("xl_")
                || l.contains("-xl") || l.contains("_xl"); };
        QString fallback;
        for (const QString& f : files) {
            const QString l = f.toLower();
            if (!(l.contains("control") && l.contains("depth")) || isSdxl(l))
                continue;
            if (l.contains("sd15") || l.contains("sd_15") || l.contains("v11")) {
                controlNetPath = d.filePath(f); break;
            }
            if (fallback.isEmpty()) fallback = d.filePath(f);
        }
        if (controlNetPath.isEmpty()) controlNetPath = fallback;
    }

    // Drive generation synchronously: write to a deterministic file next to the
    // output mesh so the result is easy to find and re-bind.
    const QString texName = outFi.completeBaseName() + "_ai.png";
    QEventLoop genLoop;
    QString genPath, genErr;
    QObject::connect(sd, &SDManager::generationCompleted, &genLoop,
        [&](const QString& p) { genPath = p; genLoop.quit(); });
    QObject::connect(sd, &SDManager::generationError, &genLoop,
        [&](const QString& e) { genErr = e; genLoop.quit(); });
    QObject::connect(sd, &SDManager::generationStopped, &genLoop,
        [&]() { genErr = QStringLiteral("generation stopped"); genLoop.quit(); });

    sd->generateMeshTexture(prompt, depth, controlNetPath,
                            static_cast<float>(controlStrength), texName,
                            width, height);
    genLoop.exec();

    if (genPath.isEmpty() || !QFileInfo::exists(genPath)) {
        err() << "Error: texture generation failed: "
              << (genErr.isEmpty() ? QStringLiteral("no output produced") : genErr)
              << Qt::endl;
        return 1;
    }

    // Copy the generated PNG next to the output mesh and bind it as the diffuse
    // texture on every submesh, so the exported asset references a local file.
    const QString localTex = QDir(outFi.absolutePath()).filePath(texName);
    if (QFileInfo(genPath).absoluteFilePath() != QFileInfo(localTex).absoluteFilePath()) {
        QFile::remove(localTex);
        QFile::copy(genPath, localTex);
    }
    Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
        outFi.absolutePath().toStdString(), "FileSystem",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    int boundCount = 0;
    for (unsigned int s = 0; s < entity->getNumSubEntities(); ++s) {
        Ogre::SubEntity* sub = entity->getSubEntity(s);
        const auto mat = sub ? sub->getMaterial() : Ogre::MaterialPtr();
        if (!mat || mat->getNumTechniques() == 0) continue;
        auto* pass = mat->getTechnique(0)->getNumPasses() > 0
            ? mat->getTechnique(0)->getPass(0) : nullptr;
        if (!pass) continue;
        Ogre::TextureUnitState* target = nullptr;
        for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
            auto* tus = pass->getTextureUnitState(i);
            if (tus->getName() == "diffuse_map" || tus->getName() == "albedo") {
                target = tus; break;
            }
        }
        if (!target && pass->getNumTextureUnitStates() > 0)
            target = pass->getTextureUnitState(0);
        if (!target)
            target = pass->createTextureUnitState();
        target->setTextureName(texName.toStdString());
        ++boundCount;
    }

    Ogre::SceneNode* node = entity->getParentSceneNode();
    const int result = MeshImporterExporter::exporter(
        node, outFi.absoluteFilePath(), formatForExtension(outputPath));
    if (result != 0) {
        err() << "Error: Export failed." << Qt::endl;
        return 1;
    }

    cliWrite(QString("Generated mesh-aware texture for %1%2.\n"
                     "  texture: %3\n  bound to %4 submesh(es)\n  saved: %5\n")
        .arg(fi.fileName())
        .arg(controlNetPath.isEmpty()
                 ? QStringLiteral(" (no ControlNet — plain txt2img)")
                 : QStringLiteral(" (depth-conditioned via %1)")
                       .arg(QFileInfo(controlNetPath).fileName()))
        .arg(texName).arg(boundCount).arg(outFi.fileName()));
    return 0;
#endif
}

QString CLIPipeline::llmDescribeMaterialToEntity(Ogre::Entity* entity,
                                                 const QString& prompt,
                                                 const QString& modelName,
                                                 QString& error)
{
    error.clear();
    if (!entity || !entity->getMesh()) {
        error = QStringLiteral("no mesh to apply the material to");
        return {};
    }

    LLMManager* llm = LLMManager::instance();
    if (!llm) {
        error = QStringLiteral("local LLM unavailable in this build "
            "(rebuild with -DENABLE_LOCAL_LLM=ON).");
        return {};
    }

    // Resolve + load a GGUF model synchronously. modelName overrides; otherwise
    // fall back to last-used / first available (mirrors the SD texture path).
    llm->scanForModels();
    QString chosen = modelName;
    if (chosen.isEmpty()) chosen = llm->lastModelName();
    if (chosen.isEmpty()) {
        const QStringList avail = llm->availableModels();
        if (!avail.isEmpty()) chosen = avail.first();
    }
    if (chosen.isEmpty()) {
        error = QStringLiteral("no LLM model found in the models directory (%1). "
            "Download a GGUF model from AI Settings or pass --model.")
            .arg(llm->modelsDirectory());
        return {};
    }
    if (!llm->isModelLoaded() || llm->currentModelName() != chosen) {
        QEventLoop loadLoop;
        // `done` guards the case where loadModel() emits its result signal
        // SYNCHRONOUSLY (e.g. an explicit missing-model name fails validation
        // before returning) — quit() before exec() is a no-op, so we must skip
        // exec() entirely or the loop would block forever. A safety timeout
        // covers a worker that neither completes nor errors.
        bool done = false, loadOk = false;
        QString loadErr;
        QObject::connect(llm, &LLMManager::modelLoadCompleted, &loadLoop,
            [&](const QString&) { done = true; loadOk = true; loadLoop.quit(); });
        QObject::connect(llm, &LLMManager::modelLoadError, &loadLoop,
            [&](const QString& e) { done = true; loadErr = e; loadLoop.quit(); });
        llm->loadModel(chosen);
        if (!done) {
            QTimer::singleShot(180000, &loadLoop, [&]() {
                if (!done) { loadErr = QStringLiteral("timed out loading model"); loadLoop.quit(); }
            });
            loadLoop.exec();
        }
        if (!loadOk || !llm->isModelLoaded()) {
            error = QStringLiteral("failed to load LLM model '%1': %2")
                .arg(chosen, loadErr.isEmpty() ? QStringLiteral("unknown error") : loadErr);
            return {};
        }
    }

    // Drive material generation synchronously. generateMaterial guards against
    // an unloaded model itself and would emit generationError in that case —
    // possibly synchronously, so guard exec() with the same `done` flag.
    QEventLoop genLoop;
    bool genDone = false;
    QString script, genErr;
    QObject::connect(llm, &LLMManager::generationCompleted, &genLoop,
        [&](const QString& s) { genDone = true; script = s; genLoop.quit(); });
    QObject::connect(llm, &LLMManager::generationError, &genLoop,
        [&](const QString& e) { genDone = true; genErr = e; genLoop.quit(); });
    QObject::connect(llm, &LLMManager::generationStopped, &genLoop,
        [&]() { genDone = true; genErr = QStringLiteral("generation stopped"); genLoop.quit(); });

    llm->generateMaterial(prompt, QString(), QStringList());
    if (!genDone) {
        QTimer::singleShot(300000, &genLoop, [&]() {
            if (!genDone) { genErr = QStringLiteral("timed out generating material"); genLoop.quit(); }
        });
        genLoop.exec();
    }

    if (script.trimmed().isEmpty()) {
        error = genErr.isEmpty() ? QStringLiteral("LLM produced no material script")
                                 : genErr;
        return {};
    }

    // Strip markdown code fences the model may wrap the script in (same cleanup
    // the GUI's onLLMGenerationCompleted does before applying).
    QString cleaned = script.trimmed();
    if (cleaned.startsWith(QStringLiteral("```"))) {
        const int nl = cleaned.indexOf('\n');
        if (nl != -1) cleaned = cleaned.mid(nl + 1);
    }
    if (cleaned.endsWith(QStringLiteral("```")))
        cleaned = cleaned.left(cleaned.length() - 3);
    cleaned = cleaned.trimmed();

    // Extract the `material <name>` header, then REWRITE it to a unique name
    // before parsing. The LLM often emits a generic name ("material Material"),
    // which would otherwise collide with — and clobber — an unrelated material
    // already in the scene. Rewriting (rather than removing the existing
    // resource) keeps other scene materials intact.
    int nameStart = -1, nameLen = 0;
    QString origName;
    {
        const QRegularExpression re(QStringLiteral(R"(^\s*material\s+([^\s{]+))"),
                                    QRegularExpression::MultilineOption);
        const auto m = re.match(cleaned);
        if (m.hasMatch()) {
            origName  = m.captured(1).trimmed();
            nameStart = static_cast<int>(m.capturedStart(1));
            nameLen   = static_cast<int>(m.capturedLength(1));
        }
    }
    if (origName.isEmpty()) {
        error = QStringLiteral("generated script has no 'material <name>' header");
        return {};
    }
    const QString matName = QStringLiteral("%1_%2")
        .arg(origName, QUuid::createUuid().toString(QUuid::Id128));
    cleaned.replace(nameStart, nameLen, matName);

    // Parse the (now uniquely-named) script into Ogre's MaterialManager.
    try {
        auto& mm = Ogre::MaterialManager::getSingleton();
        const Ogre::String s = cleaned.toStdString();
        Ogre::DataStreamPtr ds(new Ogre::MemoryDataStream(
            (void*)s.c_str(), s.length() * sizeof(char)));
        mm.parseScript(ds, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        Ogre::MaterialPtr mat = mm.getByName(matName.toStdString());
        if (!mat) {
            error = QStringLiteral("failed to parse generated material '%1'")
                .arg(matName);
            return {};
        }
        mat->compile();
        // Honor a pbr_workflow tag if the model emitted one (upgrade to
        // Cook-Torrance), matching the Material Editor's apply path.
        RTShaderHelper::applyPbrIfTagged(mat);

        const std::string stdName = matName.toStdString();
        for (unsigned int s2 = 0; s2 < entity->getNumSubEntities(); ++s2)
            entity->getSubEntity(s2)->setMaterialName(stdName);
        entity->setMaterialName(stdName);
    } catch (const Ogre::Exception& e) {
        error = QStringLiteral("Ogre error applying generated material: %1")
            .arg(e.what());
        return {};
    }

    return matName;
}

int CLIPipeline::cmdMaterialDescribe(const QString& inputPath,
                                     QString outputPath,
                                     const QString& prompt,
                                     const QString& modelName)
{
    if (inputPath.isEmpty()) {
        err() << "Error: missing <file> for --describe." << Qt::endl;
        return 2;
    }
    if (prompt.trimmed().isEmpty()) {
        err() << "Error: --describe requires a non-empty description." << Qt::endl;
        return 2;
    }
    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << inputPath << Qt::endl;
        return 1;
    }
    if (outputPath.isEmpty()) outputPath = inputPath;
    const QFileInfo outFi(outputPath);

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.describe_material"),
        QStringLiteral("CLI describe-material .%1 -> .%2")
            .arg(fi.suffix(), outFi.suffix()));
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing file %1").arg(fi.absoluteFilePath()));

    MeshImporterExporter::importer({fi.absoluteFilePath()});
    auto& entities = Manager::getSingleton()->getEntities();
    Ogre::Entity* entity = nullptr;
    for (auto* obj : entities) {
        if (obj && obj->getMovableType() == "Entity") {
            entity = static_cast<Ogre::Entity*>(obj);
            break;
        }
    }
    if (!entity || !entity->getMesh()) {
        err() << "Error: Failed to load a mesh from: " << inputPath << Qt::endl;
        return 1;
    }

    QString error;
    const QString matName =
        llmDescribeMaterialToEntity(entity, prompt, modelName, error);
    if (matName.isEmpty()) {
        err() << "Error: " << error << Qt::endl;
        return 1;
    }

    Ogre::SceneNode* node = entity->getParentSceneNode();
    SentryReporter::addBreadcrumb("file.export",
        QString("Exporting file %1").arg(outFi.absoluteFilePath()));
    const int result = MeshImporterExporter::exporter(
        node, outFi.absoluteFilePath(), formatForExtension(outputPath));
    if (result != 0) {
        err() << "Error: Export failed." << Qt::endl;
        return 1;
    }

    // Persist the generated material next to the mesh (mirrors the preset
    // path). Without this an exported .mesh references `matName` with no
    // material definition on disk.
    QString sidecarPath;
    if (auto* matMgr = Ogre::MaterialManager::getSingletonPtr()) {
        const std::string matNameStd = matName.toStdString();
        Ogre::MaterialPtr mat = matMgr->resourceExists(matNameStd)
            ? matMgr->getByName(matNameStd) : Ogre::MaterialPtr();
        if (mat) {
            Ogre::MaterialSerializer ms;
            ms.queueForExport(mat, false, false, matNameStd);
            const QString matText = QString::fromStdString(ms.getQueuedAsString());
            sidecarPath = QDir(outFi.absolutePath())
                .filePath(outFi.completeBaseName() + ".material");
            QFile matFile(sidecarPath);
            if (matFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                matFile.write(matText.toUtf8());
                matFile.close();
                SentryReporter::addBreadcrumb("file.export",
                    QString("Wrote material sidecar %1").arg(sidecarPath));
            } else {
                err() << "Error: Failed to write material sidecar: "
                      << sidecarPath << Qt::endl;
                return 1;
            }
        }
    }

    cliWrite(QString("Generated material from description for %1.\n"
                     "  prompt: \"%2\"\n  material: %3\n  saved: %4%5\n")
        .arg(fi.fileName(), prompt, matName, outFi.fileName(),
             sidecarPath.isEmpty()
                 ? QString()
                 : QStringLiteral(" (+ %1)").arg(QFileInfo(sidecarPath).fileName())));
    return 0;
}

int CLIPipeline::cmdMaterialGeneratePbr(const QString& albedoPath,
                                        const QString& meshPath,
                                        QString outputPath,
                                        int tileSize, bool wantNormal,
                                        bool wantRoughness, bool wantHeight)
{
#ifndef ENABLE_ONNX
    Q_UNUSED(albedoPath); Q_UNUSED(meshPath); Q_UNUSED(outputPath);
    Q_UNUSED(tileSize); Q_UNUSED(wantNormal); Q_UNUSED(wantRoughness);
    Q_UNUSED(wantHeight);
    err() << "Error: this build was compiled without AI PBR map synthesis "
             "(rebuild with -DENABLE_ONNX=ON)." << Qt::endl;
    return 1;
#else
    if (albedoPath.isEmpty()) {
        err() << "Error: --generate-pbr requires --texture <albedo.png>." << Qt::endl;
        return 2;
    }
    if (!QFileInfo::exists(albedoPath)) {
        err() << "Error: albedo not found: " << albedoPath << Qt::endl;
        return 1;
    }
    if (tileSize != 0 && (tileSize < 32 || tileSize > 4096)) {
        err() << "Error: --tile-size must be 0 (whole image) or between 32 and 4096." << Qt::endl;
        return 2;
    }

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.pbr_synth"),
        QStringLiteral("CLI generate-pbr from %1 (n=%2 r=%3 h=%4)")
            .arg(QFileInfo(albedoPath).fileName())
            .arg(wantNormal).arg(wantRoughness).arg(wantHeight));

    PbrMapSynth::Options opts;
    opts.generateNormal = wantNormal;
    opts.generateRoughness = wantRoughness;
    opts.generateHeight = wantHeight;
    opts.tileSize = tileSize;

    PbrMapSynthResult res =  // non-const: copyBeside() may relocate map paths
        AIAssistManager::instance()->synthesizePbrMaps(albedoPath, opts);
    if (!res.ok) {
        err() << "Error: PBR synthesis failed: "
              << (res.error.isEmpty() ? QStringLiteral("unknown error") : res.error)
              << Qt::endl;
        return 1;
    }

    // If no mesh was given, the maps + an optional .material sidecar are the
    // whole deliverable. Write a minimal sidecar referencing the generated maps.
    if (meshPath.isEmpty()) {
        cliWrite(QString("Generated PBR maps from %1:\n%2%3%4")
            .arg(QFileInfo(albedoPath).fileName())
            .arg(res.normalPath.isEmpty() ? QString()
                 : QStringLiteral("  normal:    %1\n").arg(QFileInfo(res.normalPath).fileName()))
            .arg(res.roughnessPath.isEmpty() ? QString()
                 : QStringLiteral("  roughness: %1\n").arg(QFileInfo(res.roughnessPath).fileName()))
            .arg(res.heightPath.isEmpty() ? QString()
                 : QStringLiteral("  height:    %1\n").arg(QFileInfo(res.heightPath).fileName())));
        return 0;
    }

    // Mesh target: import, bind maps into the canonical slice-E slots, export.
    const QFileInfo meshFi(meshPath);
    if (!meshFi.exists()) {
        err() << "Error: mesh not found: " << meshPath << Qt::endl;
        return 1;
    }
    if (outputPath.isEmpty()) outputPath = meshPath;
    const QFileInfo outFi(outputPath);

    if (!initOgreHeadless()) return 1;
    MeshImporterExporter::importer({meshFi.absoluteFilePath()});
    auto& entities = Manager::getSingleton()->getEntities();
    Ogre::Entity* entity = nullptr;
    for (auto* obj : entities) {
        if (obj && obj->getMovableType() == "Entity") {
            entity = static_cast<Ogre::Entity*>(obj); break;
        }
    }
    if (!entity) {
        err() << "Error: no mesh entity loaded from: " << meshPath << Qt::endl;
        return 1;
    }

    // The maps were written next to the ALBEDO; the bind below references them
    // by basename. When -o writes the mesh to a different directory, copy the
    // maps beside the output mesh so the exported material's texture refs
    // resolve next to it. (No-op when albedo dir == output dir.)
    const QString outDir = outFi.absolutePath();
    auto copyBeside = [&](QString& path) {
        if (path.isEmpty()) return;
        const QString dst = QDir(outDir).filePath(QFileInfo(path).fileName());
        if (QFileInfo(path).absoluteFilePath() != QFileInfo(dst).absoluteFilePath()) {
            QFile::remove(dst);
            if (QFile::copy(path, dst))
                path = dst;
        }
    };
    copyBeside(res.normalPath);
    copyBeside(res.roughnessPath);
    copyBeside(res.heightPath);

    // Register the output directory so the (now co-located) PNGs resolve by name.
    Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
        outDir.toStdString(), "FileSystem",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto bindSlot = [&](Ogre::Pass* pass, const char* slot, const QString& path) {
        if (path.isEmpty()) return;
        const std::string tex = QFileInfo(path).fileName().toStdString();
        Ogre::TextureUnitState* tus = nullptr;
        for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i)
            if (pass->getTextureUnitState(i)->getName() == slot) { tus = pass->getTextureUnitState(i); break; }
        if (!tus) { tus = pass->createTextureUnitState(); tus->setName(slot); }
        tus->setTextureName(tex);
    };

    int bound = 0;
    for (unsigned int s = 0; s < entity->getNumSubEntities(); ++s) {
        const auto mat = entity->getSubEntity(s)->getMaterial();
        if (!mat || mat->getNumTechniques() == 0) continue;
        auto* tech = mat->getTechnique(0);
        if (tech->getNumPasses() == 0) continue;
        auto* pass = tech->getPass(0);
        bindSlot(pass, "normal_map", res.normalPath);
        bindSlot(pass, "roughness",  res.roughnessPath);
        RTShaderHelper::wirePbrSlotsForFFP(mat.get());
        mat->compile();
        ++bound;
    }

    Ogre::SceneNode* node = entity->getParentSceneNode();
    if (MeshImporterExporter::exporter(node, outFi.absoluteFilePath(),
                                       formatForExtension(outputPath)) != 0) {
        err() << "Error: Export failed." << Qt::endl;
        return 1;
    }

    cliWrite(QString("Generated PBR maps from %1 and bound to %2 submesh(es).\n"
                     "  saved: %3\n")
        .arg(QFileInfo(albedoPath).fileName()).arg(bound).arg(outFi.fileName()));
    return 0;
#endif
}

int CLIPipeline::cmdMaterialUpscale(const QString& srcPath, QString outputPath,
                                    int scale)
{
#ifndef ENABLE_ONNX
    Q_UNUSED(srcPath); Q_UNUSED(outputPath); Q_UNUSED(scale);
    err() << "Error: this build was compiled without AI texture upscaling "
             "(rebuild with -DENABLE_ONNX=ON)." << Qt::endl;
    return 1;
#else
    if (srcPath.isEmpty()) {
        err() << "Error: --upscale requires --texture <image>." << Qt::endl;
        return 2;
    }
    if (scale != 2 && scale != 4) {
        err() << "Error: --upscale must be 2 or 4." << Qt::endl;
        return 2;
    }
    if (!QFileInfo::exists(srcPath)) {
        err() << "Error: texture not found: " << srcPath << Qt::endl;
        return 1;
    }
    const QImage src(srcPath);
    if (src.isNull()) {
        err() << "Error: could not load image: " << srcPath << Qt::endl;
        return 1;
    }

    // The facade handles model download (first-run) + run + cache; it writes
    // <stem>_upscaled.png next to the source. (Breadcrumb emitted there.)
    const QString produced =
        AIAssistManager::instance()->upscaleTexture(srcPath, scale, /*overwrite=*/true);
    if (produced.isEmpty()) {
        err() << "Error: upscale failed (model unavailable or inference error)." << Qt::endl;
        return 1;
    }
    // Honour an explicit -o strictly: move the facade's output there, falling
    // back to copy+remove for cross-device moves. If the requested file can't
    // be produced, fail (exit 1) rather than silently leaving it at the
    // temp <stem>_upscaled_xN.png path — automation relies on -o.
    QString finalPath = produced;
    if (!outputPath.isEmpty()
        && QFileInfo(outputPath).absoluteFilePath() != QFileInfo(produced).absoluteFilePath()) {
        QFile::remove(outputPath);
        bool moved = QFile::rename(produced, outputPath);
        if (!moved && QFile::copy(produced, outputPath)) {
            QFile::remove(produced);
            moved = true;
        }
        if (!moved) {
            err() << "Error: could not write the requested output file: "
                  << outputPath << " (upscaled image is at "
                  << produced << ")" << Qt::endl;
            return 1;
        }
        finalPath = outputPath;
    }
    const QImage outImg(finalPath);
    cliWrite(QString("Upscaled %1 by %2x → %3 (%4×%5 → %6×%7)\n")
        .arg(QFileInfo(srcPath).fileName()).arg(scale)
        .arg(QFileInfo(finalPath).fileName())
        .arg(src.width()).arg(src.height())
        .arg(outImg.width()).arg(outImg.height()));
    return 0;
#endif
}

int CLIPipeline::cmdPackTextures(int argc, char* argv[])
{
    // Parse:
    //   pack-textures --r ao.png --g rough.png --b metal.png [--a path]
    //                 [--rc <0..1>] [--gc <0..1>] [--bc <0..1>] [--ac <0..1>]
    //                 [--invert-r] [--invert-g] [--invert-b] [--invert-a]
    //                 [--width N] [--height N] [--no-alpha] -o out.png
    TextureChannelPacker::PackingSpec spec;
    QString outputPath;

    auto setPath = [](TextureChannelPacker::ChannelSource& dst, const QString& v) {
        dst.path = v;
    };
    auto setConst = [](TextureChannelPacker::ChannelSource& dst, const QString& v) {
        dst.constantValue = v.toFloat();
    };

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "pack-textures" || arg == "--cli") continue;
        if ((arg == "--r" || arg == "--red")     && i + 1 < argc) { setPath(spec.red,   QString(argv[++i])); continue; }
        if ((arg == "--g" || arg == "--green")   && i + 1 < argc) { setPath(spec.green, QString(argv[++i])); continue; }
        if ((arg == "--b" || arg == "--blue")    && i + 1 < argc) { setPath(spec.blue,  QString(argv[++i])); continue; }
        if ((arg == "--a" || arg == "--alpha")   && i + 1 < argc) { setPath(spec.alpha, QString(argv[++i])); continue; }
        if (arg == "--rc" && i + 1 < argc) { setConst(spec.red,   QString(argv[++i])); continue; }
        if (arg == "--gc" && i + 1 < argc) { setConst(spec.green, QString(argv[++i])); continue; }
        if (arg == "--bc" && i + 1 < argc) { setConst(spec.blue,  QString(argv[++i])); continue; }
        if (arg == "--ac" && i + 1 < argc) { setConst(spec.alpha, QString(argv[++i])); continue; }
        if (arg == "--invert-r") { spec.red.invert   = true; continue; }
        if (arg == "--invert-g") { spec.green.invert = true; continue; }
        if (arg == "--invert-b") { spec.blue.invert  = true; continue; }
        if (arg == "--invert-a") { spec.alpha.invert = true; continue; }
        if (arg == "--width"  && i + 1 < argc) { spec.outputWidth  = QString(argv[++i]).toInt(); continue; }
        if (arg == "--height" && i + 1 < argc) { spec.outputHeight = QString(argv[++i]).toInt(); continue; }
        if (arg == "--no-alpha") { spec.includeAlpha = false; continue; }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString(argv[++i]);
            continue;
        }
    }

    if (outputPath.isEmpty()) {
        err() << "Error: missing -o/--output." << Qt::endl;
        err() << "Usage: qtmesh pack-textures --r <img> [--g <img> --b <img> --a <img>]" << Qt::endl;
        err() << "                            [--rc/--gc/--bc/--ac <0..1>]" << Qt::endl;
        err() << "                            [--invert-r/g/b/a]" << Qt::endl;
        err() << "                            [--width N --height N] [--no-alpha]" << Qt::endl;
        err() << "                            -o <output.png>" << Qt::endl;
        return 2;
    }

    SentryReporter::addBreadcrumb("cli.pack-textures",
        QString("Pack textures -> %1").arg(QFileInfo(outputPath).fileName()));

    auto r = TextureChannelPacker::packToFile(spec, outputPath);
    if (!r.ok) {
        err() << "Error: " << r.error << Qt::endl;
        return 1;
    }

    cliWrite(QString("Packed %1x%2 -> %3\n")
                 .arg(r.usedWidth)
                 .arg(r.usedHeight)
                 .arg(QFileInfo(outputPath).fileName()));
    return 0;
}

int CLIPipeline::cmdNormalFromHeight(int argc, char* argv[])
{
    // Parse:
    //   normal-from-height --src <bump.png> [--strength N]
    //                      [--invert-r] [--invert-g]
    //                      [--width N] [--height N]
    //                      -o <out.png>
    NormalMapGenerator::GenSpec spec;
    QString outputPath;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "normal-from-height" || arg == "--cli") continue;
        if (arg == "--src" && i + 1 < argc) { spec.sourcePath = QString(argv[++i]); continue; }
        if (arg == "--strength" && i + 1 < argc) { spec.strength = QString(argv[++i]).toFloat(); continue; }
        if (arg == "--width"  && i + 1 < argc) { spec.outputWidth  = QString(argv[++i]).toInt(); continue; }
        if (arg == "--height" && i + 1 < argc) { spec.outputHeight = QString(argv[++i]).toInt(); continue; }
        if (arg == "--invert-r") { spec.invertR = true; continue; }
        if (arg == "--invert-g" || arg == "--directx") { spec.invertG = true; continue; }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString(argv[++i]);
            continue;
        }
    }

    if (spec.sourcePath.isEmpty() || outputPath.isEmpty()) {
        err() << "Error: missing --src or -o." << Qt::endl;
        err() << "Usage: qtmesh normal-from-height --src <height.png>" << Qt::endl;
        err() << "                                 [--strength N] [--invert-r] [--invert-g]" << Qt::endl;
        err() << "                                 [--width N --height N]" << Qt::endl;
        err() << "                                 -o <normal.png>" << Qt::endl;
        return 2;
    }

    SentryReporter::addBreadcrumb("cli.normal-from-height",
        QString("Normal map from %1 -> %2")
            .arg(QFileInfo(spec.sourcePath).fileName(),
                 QFileInfo(outputPath).fileName()));

    auto r = NormalMapGenerator::generateToFile(spec, outputPath);
    if (!r.ok) {
        err() << "Error: " << r.error << Qt::endl;
        return 1;
    }
    cliWrite(QString("Normal map %1x%2 -> %3\n")
                 .arg(r.usedWidth)
                 .arg(r.usedHeight)
                 .arg(QFileInfo(outputPath).fileName()));
    return 0;
}

int CLIPipeline::cmdAtlas(int argc, char* argv[])
{
    // Parse:
    //   atlas --inputs a.png,b.png,c.png -o atlas.png
    //         [--size 2048] [--width N] [--height N] [--padding N]
    //         [--manifest atlas.json]
    TextureAtlasPacker::AtlasSpec spec;
    QString outputPath;
    QString manifestPath;
    QString inputsArg;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "atlas" || arg == "--cli") continue;
        if (arg == "--inputs" && i + 1 < argc) { inputsArg = QString(argv[++i]); continue; }
        if (arg == "--size" && i + 1 < argc) {
            const int n = QString(argv[++i]).toInt();
            spec.atlasWidth = n;
            spec.atlasHeight = n;
            continue;
        }
        if (arg == "--width"   && i + 1 < argc) { spec.atlasWidth   = QString(argv[++i]).toInt(); continue; }
        if (arg == "--height"  && i + 1 < argc) { spec.atlasHeight  = QString(argv[++i]).toInt(); continue; }
        if (arg == "--padding" && i + 1 < argc) { spec.padding      = QString(argv[++i]).toInt(); continue; }
        if (arg == "--manifest" && i + 1 < argc) { manifestPath = QString(argv[++i]); continue; }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString(argv[++i]);
            continue;
        }
        // Accept bare positional inputs too (a.png b.png c.png).
        if (!arg.startsWith("-")) {
            if (inputsArg.isEmpty()) inputsArg = arg;
            else inputsArg += "," + arg;
        }
    }

    if (inputsArg.isEmpty() || outputPath.isEmpty()) {
        err() << "Error: missing --inputs or -o." << Qt::endl;
        err() << "Usage: qtmesh atlas --inputs a.png,b.png,c.png -o atlas.png" << Qt::endl;
        err() << "                    [--size 2048] [--width N --height N]" << Qt::endl;
        err() << "                    [--padding 2] [--manifest atlas.json]" << Qt::endl;
        return 2;
    }

    // Split comma-separated inputs, trim whitespace, drop empties.
    const QStringList parts = inputsArg.split(',', Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        const QString trimmed = p.trimmed();
        if (!trimmed.isEmpty())
            spec.sourcePaths.append(trimmed);
    }
    if (spec.sourcePaths.isEmpty()) {
        err() << "Error: --inputs must list at least one image." << Qt::endl;
        return 2;
    }

    SentryReporter::addBreadcrumb("cli.atlas",
        QString("Atlas %1 inputs -> %2")
            .arg(spec.sourcePaths.size())
            .arg(QFileInfo(outputPath).fileName()));

    auto r = TextureAtlasPacker::packToFile(spec, outputPath);
    if (!r.ok) {
        err() << "Error: " << r.error << Qt::endl;
        return 1;
    }
    SentryReporter::addBreadcrumb("file.export",
        QString("Atlas %1 tiles -> %2").arg(r.tiles.size()).arg(QFileInfo(outputPath).fileName()));

    cliWrite(QString("Packed %1 tiles into %2x%3 atlas (used %4x%5) -> %6\n")
                 .arg(r.tiles.size())
                 .arg(spec.atlasWidth)
                 .arg(spec.atlasHeight)
                 .arg(r.usedWidth)
                 .arg(r.usedHeight)
                 .arg(QFileInfo(outputPath).fileName()));

    if (!manifestPath.isEmpty()) {
        const QString json = TextureAtlasPacker::manifestToJson(r, spec.padding);
        const QByteArray bytes = json.toUtf8();
        QFile mf(manifestPath);
        if (!mf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            err() << "Error: could not open manifest path: " << manifestPath << Qt::endl;
            return 1;
        }
        const qint64 written = mf.write(bytes);
        mf.close();
        if (written != bytes.size()) {
            err() << "Error: short write to manifest path: " << manifestPath
                  << " (" << written << "/" << bytes.size() << " bytes)" << Qt::endl;
            return 1;
        }
        SentryReporter::addBreadcrumb("file.export",
            QString("Atlas manifest -> %1").arg(QFileInfo(manifestPath).fileName()));
        cliWrite(QString("Manifest -> %1\n").arg(QFileInfo(manifestPath).fileName()));
    }

    return 0;
}

int CLIPipeline::cmdAtlasApply(int argc, char* argv[])
{
    // Parse:
    //   atlas-apply <file> -o <output> --manifest <atlas.json> --atlas <atlas.png>
    //               [--match {basename|fullpath}] [--no-clamp] [--json]
    QString inputPath, outputPath, manifestPath, atlasImagePath;
    QString matchMode = "basename";
    bool noClamp = false;
    bool keepExtras = false;
    bool jsonOutput = false;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "atlas-apply" || arg == "--cli") continue;
        if (arg == "--manifest" && i + 1 < argc) { manifestPath = QString(argv[++i]); continue; }
        if (arg == "--atlas"    && i + 1 < argc) { atlasImagePath = QString(argv[++i]); continue; }
        if (arg == "--match"    && i + 1 < argc) { matchMode = QString(argv[++i]).toLower(); continue; }
        if (arg == "--no-clamp") { noClamp = true; continue; }
        if (arg == "--keep-extras") { keepExtras = true; continue; }
        if (arg == "--json")     { jsonOutput = true; continue; }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString(argv[++i]); continue;
        }
        if (!arg.startsWith("-") && inputPath.isEmpty()) {
            inputPath = arg; continue;
        }
    }

    if (inputPath.isEmpty() || outputPath.isEmpty()
        || manifestPath.isEmpty() || atlasImagePath.isEmpty()) {
        err() << "Error: missing required arguments." << Qt::endl;
        err() << "Usage: qtmesh atlas-apply <file> -o <output>" << Qt::endl;
        err() << "         --manifest <atlas.json> --atlas <atlas.png>" << Qt::endl;
        err() << "         [--match {basename|fullpath}] [--no-clamp]" << Qt::endl;
        err() << "         [--keep-extras] [--json]" << Qt::endl;
        return 2;
    }
    if (matchMode != "basename" && matchMode != "fullpath") {
        err() << "Error: --match must be 'basename' or 'fullpath' (got '"
              << matchMode << "')" << Qt::endl;
        return 2;
    }

    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << inputPath << Qt::endl;
        return 1;
    }
    QFile manifestFile(manifestPath);
    if (!manifestFile.exists()) {
        err() << "Error: Manifest not found: " << manifestPath << Qt::endl;
        return 1;
    }
    if (!QFileInfo::exists(atlasImagePath)) {
        err() << "Error: Atlas image not found: " << atlasImagePath << Qt::endl;
        return 1;
    }
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        err() << "Error: Could not read manifest: " << manifestPath << Qt::endl;
        return 1;
    }
    const QByteArray manifestJson = manifestFile.readAll();
    manifestFile.close();

    auto parsed = ApplyAtlas::parseManifestJson(manifestJson);
    if (!parsed.ok) {
        err() << "Error: " << parsed.error << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.atlas-apply",
        QString("Apply atlas (%1 tiles) to .%2").arg(parsed.manifest.tiles.size())
            .arg(fi.suffix()));
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing %1").arg(fi.absoluteFilePath()));

    MeshImporterExporter::importer({fi.absoluteFilePath()});
    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        err() << "Error: Failed to load file: " << inputPath << Qt::endl;
        return 1;
    }

    // Register the atlas image directory as a resource location so the
    // material can resolve the texture by leaf name on re-export.
    const QFileInfo atlasFi(atlasImagePath);
    const QString atlasDir = atlasFi.absolutePath();
    const Ogre::String atlasTexName = atlasFi.fileName().toStdString();
    try {
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
            atlasDir.toStdString(), "FileSystem",
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, true);
    } catch (const Ogre::Exception&) {
        // Already registered — fine, the texture lookup below will still
        // resolve. We swallow the duplicate-location exception only.
    }

    ApplyAtlas::ApplyOptions opts;
    opts.matchMode = (matchMode == "fullpath")
        ? ApplyAtlas::MatchMode::FullPath
        : ApplyAtlas::MatchMode::Basename;
    opts.atlasTextureName = atlasFi.fileName();
    opts.clampOutOfRangeUVs = !noClamp;
    opts.stripNonDiffuseTextures = !keepExtras;

    // Apply to every loaded Entity in turn. Multi-entity scenes (e.g. a
    // scene-export glTF) get every applicable submesh rewritten in one go.
    int totalSubmeshes = 0;
    int totalRewritten = 0;
    int totalOutOfRange = 0;
    QJsonArray perEntity;
    for (auto* obj : entities) {
        if (!obj || obj->getMovableType() != "Entity") continue;
        auto* ent = static_cast<Ogre::Entity*>(obj);
        ApplyAtlas::ApplyReport r = ApplyAtlas::applyToEntity(ent, parsed.manifest, opts);
        if (!r.ok) {
            err() << "Error: " << r.error << Qt::endl;
            return 1;
        }
        totalSubmeshes  += r.submeshCount();
        totalRewritten  += r.rewrittenCount();
        for (const auto& s : r.submeshes) totalOutOfRange += s.outOfRangeUVs;
        QJsonObject e = r.toJson();
        e["entity"] = QString::fromStdString(ent->getName());
        perEntity.append(e);
    }

    // Export the mutated scene.
    Ogre::Entity* firstEntity = nullptr;
    for (auto* obj : entities) {
        if (obj && obj->getMovableType() == "Entity") {
            firstEntity = static_cast<Ogre::Entity*>(obj);
            break;
        }
    }
    if (!firstEntity) {
        err() << "Error: no Entity to export." << Qt::endl;
        return 1;
    }
    SentryReporter::addBreadcrumb("file.export",
        QString("Exporting %1").arg(QFileInfo(outputPath).absoluteFilePath()));
    if (MeshImporterExporter::exporter(firstEntity->getParentSceneNode(),
                                       outputPath,
                                       formatForExtension(outputPath)) != 0) {
        err() << "Error: Export failed." << Qt::endl;
        return 1;
    }

    if (jsonOutput) {
        QJsonObject root;
        root["file"]   = fi.fileName();
        root["output"] = QFileInfo(outputPath).fileName();
        root["atlas"]  = atlasFi.fileName();
        root["manifest"] = QFileInfo(manifestPath).fileName();
        root["submeshes"]      = totalSubmeshes;
        root["rewritten"]      = totalRewritten;
        root["outOfRangeUVs"]  = totalOutOfRange;
        root["entities"]       = perEntity;
        cliWrite(QString::fromUtf8(
            QJsonDocument(root).toJson(QJsonDocument::Indented)) + "\n");
    } else {
        cliWrite(QString("Applied atlas '%1' to %2: %3/%4 submeshes rewritten"
                         "%5 -> %6\n")
                     .arg(atlasFi.fileName())
                     .arg(fi.fileName())
                     .arg(totalRewritten).arg(totalSubmeshes)
                     .arg(totalOutOfRange > 0
                          ? QString(" (%1 UV(s) %2)").arg(totalOutOfRange)
                                .arg(noClamp ? "skipped" : "clamped")
                          : QString())
                     .arg(QFileInfo(outputPath).fileName()));
    }
    return 0;
}

int CLIPipeline::cmdScan(int argc, char* argv[])
{
    // Parse: scan [path] [options]
    QString scanRoot;
    QString configPath;
    QString profileIdArg;
    QString targetIdArg;
    bool listProfiles = false;
    QString tokenArg;
    bool strictUpload = false;
    bool noUpload = false;
    bool jsonOutput = false;
    QString reportPath;
    QString sarifPath;
    bool fix = false;
    bool dryRun = false;
    QString includeArg;
    QString excludeArg;
    QString failOn;
    QString allowedFormatsArg;
    QString forbiddenExtensionsArg;
    QString fileNameCaseOverride;
    QString requiredAnimationNamesArg;
    QString requiredBoneNamesArg;
    bool hasAllowedFormatsOverride = false;
    bool hasForbiddenExtensionsOverride = false;
    bool hasFileNameCaseOverride = false;
    bool hasRequiredAnimationNamesOverride = false;
    bool hasRequiredBoneNamesOverride = false;

    int maxVerticesOverride = -1;
    int minVerticesOverride = -1;
    int maxTrianglesOverride = -1;
    int maxTrianglesPerMeshOverride = -1;
    int maxBonesOverride = -1;
    int maxSubmeshesOverride = -1;
    int maxDrawCallsOverride = -1;
    double maxAcmrOverride = -1.0;
    int maxMeshesOverride = -1;
    int minMeshesOverride = -1;
    int maxMaterialsOverride = -1;
    int minMaterialsOverride = -1;
    int maxAnimKeyframesOverride = -1;
    int minAnimKeyframesOverride = -1;
    double maxFileSizeMbOverride = -1.0;
    double minFileSizeMbOverride = -1.0;
    double maxAnimDurationOverride = -1.0;
    double minAnimDurationOverride = -1.0;
    int requireSkeletonOverride = -1;        // -1 = unchanged
    int requireAnimationsOverride = -1;      // -1 = unchanged
    int allowEmbeddedTexturesOverride = -1;  // -1 = unchanged
    int requireTexturesExistOverride = -1;   // -1 = unchanged
    int allowMissingMaterialsOverride = -1;  // -1 = unchanged

    enum class ParseValueResult { NoMatch, Matched, Error };
    auto parseValueArg = [&](const QString& arg, const QString& option, int& argIndex, QString& outValue) -> ParseValueResult {
        if (arg == option) {
            if (argIndex + 1 >= argc) {
                err() << "Error: " << option << " requires a value" << Qt::endl;
                return ParseValueResult::Error;
            }
            outValue = QString::fromUtf8(argv[++argIndex]);
            return ParseValueResult::Matched;
        }
        const QString withEquals = option + "=";
        if (arg.startsWith(withEquals)) {
            outValue = arg.mid(withEquals.size());
            return ParseValueResult::Matched;
        }
        return ParseValueResult::NoMatch;
    };

    auto parseNonNegativeInt = [&](const QString& option, const QString& value, int& outValue) -> bool {
        bool ok = false;
        const int parsed = value.toInt(&ok);
        if (!ok || parsed < 0) {
            err() << "Error: " << option << " must be an integer >= 0" << Qt::endl;
            return false;
        }
        outValue = parsed;
        return true;
    };

    auto parseNonNegativeDouble = [&](const QString& option, const QString& value, double& outValue) -> bool {
        bool ok = false;
        const double parsed = value.toDouble(&ok);
        if (!ok || parsed < 0.0) {
            err() << "Error: " << option << " must be a number >= 0" << Qt::endl;
            return false;
        }
        outValue = parsed;
        return true;
    };

    auto splitCsv = [](const QString& csv) -> QStringList {
        QStringList out;
        for (const auto& part : csv.split(",", Qt::SkipEmptyParts)) {
            QString value = part.trimmed();
            if (!value.isEmpty())
                out.append(value);
        }
        return out;
    };

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "scan" || arg == "--cli" || arg == "--verbose" || arg == "--no-telemetry")
            continue;
        if (arg == "--json")    { jsonOutput = true; continue; }
        if (arg == "--fix")     { fix = true; continue; }
        if (arg == "--dry-run") { dryRun = true; continue; }
        if (arg == "--strict-upload") { strictUpload = true; continue; }
        if (arg == "--no-upload") { noUpload = true; continue; }
        if (arg == "--list-profiles") { listProfiles = true; continue; }
        QString value;
        ParseValueResult parseResult = parseValueArg(arg, "--target", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) { targetIdArg = value; continue; }
        parseResult = parseValueArg(arg, "--profile", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) { profileIdArg = value; continue; }
        parseResult = parseValueArg(arg, "--config", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) { configPath = value; continue; }
        parseResult = parseValueArg(arg, "--report", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) { reportPath = value; continue; }
        parseResult = parseValueArg(arg, "--sarif", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) { sarifPath = value; continue; }
        parseResult = parseValueArg(arg, "--include", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) { includeArg = value; continue; }
        parseResult = parseValueArg(arg, "--exclude", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) { excludeArg = value; continue; }
        parseResult = parseValueArg(arg, "--fail-on", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) { failOn = value; continue; }
        parseResult = parseValueArg(arg, "--token", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) { tokenArg = value; continue; }
        parseResult = parseValueArg(arg, "--allowed-formats", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            allowedFormatsArg = value;
            hasAllowedFormatsOverride = true;
            continue;
        }
        parseResult = parseValueArg(arg, "--forbidden-extensions", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            forbiddenExtensionsArg = value;
            hasForbiddenExtensionsOverride = true;
            continue;
        }
        parseResult = parseValueArg(arg, "--file-name-case", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            fileNameCaseOverride = value;
            hasFileNameCaseOverride = true;
            continue;
        }
        parseResult = parseValueArg(arg, "--require-animation-names", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            requiredAnimationNamesArg = value;
            hasRequiredAnimationNamesOverride = true;
            continue;
        }
        parseResult = parseValueArg(arg, "--require-bone-names", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            requiredBoneNamesArg = value;
            hasRequiredBoneNamesOverride = true;
            continue;
        }

        parseResult = parseValueArg(arg, "--max-vertices", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--max-vertices", value, maxVerticesOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--min-vertices", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--min-vertices", value, minVerticesOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--max-acmr", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeDouble("--max-acmr", value, maxAcmrOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--max-triangles", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--max-triangles", value, maxTrianglesOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--max-triangles-per-mesh", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--max-triangles-per-mesh", value, maxTrianglesPerMeshOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--max-bones", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--max-bones", value, maxBonesOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--max-submeshes", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--max-submeshes", value, maxSubmeshesOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--max-draw-calls", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--max-draw-calls", value, maxDrawCallsOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--max-meshes", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--max-meshes", value, maxMeshesOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--min-meshes", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--min-meshes", value, minMeshesOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--max-materials", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--max-materials", value, maxMaterialsOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--min-materials", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--min-materials", value, minMaterialsOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--max-anim-keyframes", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--max-anim-keyframes", value, maxAnimKeyframesOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--min-anim-keyframes", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeInt("--min-anim-keyframes", value, minAnimKeyframesOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--max-file-size-mb", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeDouble("--max-file-size-mb", value, maxFileSizeMbOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--min-file-size-mb", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeDouble("--min-file-size-mb", value, minFileSizeMbOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--max-anim-duration", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeDouble("--max-anim-duration", value, maxAnimDurationOverride)) return 2;
            continue;
        }
        parseResult = parseValueArg(arg, "--min-anim-duration", i, value);
        if (parseResult == ParseValueResult::Error) return 2;
        if (parseResult == ParseValueResult::Matched) {
            if (!parseNonNegativeDouble("--min-anim-duration", value, minAnimDurationOverride)) return 2;
            continue;
        }

        if (arg == "--require-skeleton") { requireSkeletonOverride = 1; continue; }
        if (arg == "--no-require-skeleton") { requireSkeletonOverride = 0; continue; }
        if (arg == "--require-animations") { requireAnimationsOverride = 1; continue; }
        if (arg == "--no-require-animations") { requireAnimationsOverride = 0; continue; }
        if (arg == "--allow-embedded-textures") { allowEmbeddedTexturesOverride = 1; continue; }
        if (arg == "--disallow-embedded-textures") { allowEmbeddedTexturesOverride = 0; continue; }
        if (arg == "--require-textures-exist") { requireTexturesExistOverride = 1; continue; }
        if (arg == "--no-require-textures-exist") { requireTexturesExistOverride = 0; continue; }
        if (arg == "--allow-missing-materials") { allowMissingMaterialsOverride = 1; continue; }
        if (arg == "--disallow-missing-materials") { allowMissingMaterialsOverride = 0; continue; }

        if (!arg.startsWith("-") && scanRoot.isEmpty()) { scanRoot = arg; continue; }
    }

    if (listProfiles) {
        const QStringList ids = PlatformProfileLoader::listBuiltinIds();
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                    QStringLiteral("scan --list-profiles count=%1 dir=%2")
                                        .arg(ids.size())
                                        .arg(PlatformProfileLoader::builtinProfilesDirectory()));
        if (ids.isEmpty()) {
            err() << "No built-in platform profiles found (searched "
                 << PlatformProfileLoader::builtinProfilesDirectory() << ")." << Qt::endl;
            return 2;
        }
        cliWrite(QStringLiteral("Built-in platform profiles:\n"));
        for (const QString& id : ids)
            cliWrite(QStringLiteral("  %1\n").arg(id));
        return 0;
    }

    // Load config (precedence):
    // 1) ScanConfig::defaults()
    // 2) Platform profile (--profile or profile: in project file)
    // 3) Project config (--config, cloud rules, or local qtmesh.yml|yaml|json)
    // 4) CLI flags (below)
    ScanConfig config = ScanConfig::defaults();
    QVariantMap projectRoot;
    QString activeProfileId;

    if (!configPath.isEmpty()) {
        if (!QFileInfo::exists(configPath)) {
            err() << "Error: Config file not found: " << configPath << Qt::endl;
            return 2;
        }
        projectRoot = ScanConfig::loadProjectMapFromFile(configPath);
        SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
            QStringLiteral("scan config=%1").arg(QFileInfo(configPath).absoluteFilePath()));
        if (!resolveIngestToken(tokenArg).isEmpty()) {
            err() << "Note: Using --config file; remote cloud rules were not fetched."
                 << " Scan JSON is still uploaded when an ingest token is set (unless --no-upload)."
                 << Qt::endl;
        }
        if (!projectRoot.isEmpty())
            err() << "Note: Using config file " << configPath << "." << Qt::endl;
    } else {
        const QString ingestForRules = resolveIngestToken(tokenArg);
        if (!ingestForRules.isEmpty()) {
            SentryReporter::addBreadcrumb(QStringLiteral("cli.scan"),
                QStringLiteral("QtMesh Cloud fetchRules: requested"));
            const auto rules = QtMeshCloudClient::fetchRules(ingestForRules);
            if (rules.ok) {
                projectRoot = rules.config.toVariantMap();
                err() << "Note: Using QtMesh Cloud rules (source: " << rules.source << ")." << Qt::endl;
                SentryReporter::addBreadcrumb(QStringLiteral("cli.scan"),
                    QStringLiteral("QtMesh Cloud fetchRules: ok source=%1").arg(rules.source));
            } else {
                err() << "Warning: Could not load QtMesh Cloud rules (" << rules.errorString
                     << "). Using built-in defaults." << Qt::endl;
                SentryReporter::addBreadcrumb(QStringLiteral("cli.scan"),
                    QStringLiteral("QtMesh Cloud fetchRules: failed %1").arg(rules.errorString),
                    QStringLiteral("warning"));
            }
        } else {
            QString localAutoPath;
            if (QFileInfo::exists(QStringLiteral("qtmesh.yml")))
                localAutoPath = QStringLiteral("qtmesh.yml");
            else if (QFileInfo::exists(QStringLiteral("qtmesh.yaml")))
                localAutoPath = QStringLiteral("qtmesh.yaml");
            else if (QFileInfo::exists(QStringLiteral("qtmesh.json")))
                localAutoPath = QStringLiteral("qtmesh.json");

            if (!localAutoPath.isEmpty()) {
                projectRoot = ScanConfig::loadProjectMapFromFile(localAutoPath);
                if (!projectRoot.isEmpty()) {
                    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                        QStringLiteral("scan config=%1").arg(QFileInfo(localAutoPath).absoluteFilePath()));
                    err() << "Note: Using local " << localAutoPath << "." << Qt::endl;
                }
            }
        }
    }

    QString profileId = profileIdArg.trimmed();
    const QString targetId = targetIdArg.trimmed();
    if (!targetId.isEmpty()) {
        if (!profileId.isEmpty() && profileId != targetId) {
            err() << "Error: --target and --profile both provided with different values ("
                  << targetId << " vs " << profileId << ")" << Qt::endl;
            return 2;
        }
        profileId = targetId;
    }
    if (profileId.isEmpty())
        profileId = projectRoot.value(QStringLiteral("profile")).toString().trimmed();

    if (!profileId.isEmpty()) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
            QStringLiteral("scan profile=%1").arg(profileId));
        const PlatformProfileScanSetup setup = buildScanConfigWithPlatformProfile(profileId);
        if (!setup.ok) {
            err() << "Error: " << setup.error << Qt::endl;
            return 2;
        }
        for (const QString& w : setup.warnings)
            err() << "Warning: " << w << Qt::endl;
        config = setup.config;
        activeProfileId = setup.profileId;
        err() << "Note: Using platform profile '" << setup.profileId << "'." << Qt::endl;
        SentryReporter::addBreadcrumb(QStringLiteral("cli.scan"),
            QStringLiteral("platform profile=%1").arg(setup.profileId));
    }

    if (!projectRoot.isEmpty())
        ScanConfig::applyProjectConfig(config, projectRoot);

    // CLI overrides
    if (fix)     config.fixEnabled = true;
    if (dryRun)  config.dryRun = true;

    if (hasAllowedFormatsOverride) {
        config.allowedFormats.clear();
        for (auto fmt : splitCsv(allowedFormatsArg)) {
            if (fmt.startsWith("."))
                fmt.remove(0, 1);
            config.allowedFormats.append(fmt.toLower());
        }
    }
    if (hasForbiddenExtensionsOverride) {
        config.forbiddenExtensions.clear();
        for (auto ext : splitCsv(forbiddenExtensionsArg)) {
            if (ext.startsWith("."))
                ext.remove(0, 1);
            config.forbiddenExtensions.append(ext.toLower());
        }
    }

    if (maxFileSizeMbOverride >= 0.0) config.maxFileSizeMb = maxFileSizeMbOverride;
    if (minFileSizeMbOverride >= 0.0) config.minFileSizeMb = minFileSizeMbOverride;
    if (maxMeshesOverride >= 0) config.maxMeshCount = maxMeshesOverride;
    if (minMeshesOverride >= 0) config.minMeshCount = minMeshesOverride;
    if (maxMaterialsOverride >= 0) config.maxMaterialCount = maxMaterialsOverride;
    if (minMaterialsOverride >= 0) config.minMaterialCount = minMaterialsOverride;
    if (maxVerticesOverride >= 0) config.maxVertexCount = maxVerticesOverride;
    if (minVerticesOverride >= 0) config.minVertexCount = minVerticesOverride;
    if (maxTrianglesOverride >= 0) config.maxTriangleCount = maxTrianglesOverride;
    if (maxTrianglesPerMeshOverride >= 0) config.maxTrianglesPerMesh = maxTrianglesPerMeshOverride;
    if (maxBonesOverride >= 0) config.maxBoneCount = maxBonesOverride;
    if (maxSubmeshesOverride >= 0) config.maxSubmeshCount = maxSubmeshesOverride;
    if (maxDrawCallsOverride >= 0) config.maxDrawCalls = maxDrawCallsOverride;
    if (maxAcmrOverride >= 0.0)   config.maxAcmr        = maxAcmrOverride;
    if (maxAnimKeyframesOverride >= 0) config.maxAnimKeyframes = maxAnimKeyframesOverride;
    if (minAnimKeyframesOverride >= 0) config.minAnimKeyframes = minAnimKeyframesOverride;
    if (maxAnimDurationOverride >= 0.0) config.maxAnimDuration = maxAnimDurationOverride;
    if (minAnimDurationOverride >= 0.0) config.minAnimDuration = minAnimDurationOverride;

    if (requireSkeletonOverride >= 0) config.requireSkeleton = (requireSkeletonOverride == 1);
    if (requireAnimationsOverride >= 0) config.requireAnimations = (requireAnimationsOverride == 1);
    if (allowEmbeddedTexturesOverride >= 0) config.allowEmbeddedTextures = (allowEmbeddedTexturesOverride == 1);
    if (requireTexturesExistOverride >= 0) config.requireTexturesExist = (requireTexturesExistOverride == 1);
    if (allowMissingMaterialsOverride >= 0) config.allowMissingMaterials = (allowMissingMaterialsOverride == 1);

    if (hasFileNameCaseOverride) {
        const QString c = fileNameCaseOverride.trimmed();
        if (!c.isEmpty() && c != "snake_case" && c != "kebab-case" && c != "camelCase" &&
            c != "PascalCase" && c != "lowercase") {
            err() << "Error: --file-name-case must be one of: snake_case, kebab-case, camelCase, PascalCase, lowercase" << Qt::endl;
            return 2;
        }
        config.fileNameCase = c;
    }

    if (hasRequiredAnimationNamesOverride)
        config.requireAnimationNames = splitCsv(requiredAnimationNamesArg);
    if (hasRequiredBoneNamesOverride)
        config.requireBoneNames = splitCsv(requiredBoneNamesArg);

    // Ensure CLI rule overrides have highest precedence even when scoped rules are present.
    // Append a catch-all scope last so withScopeOverrides() reapplies CLI values after file scopes.
    QVariantMap cliRuleOverrides;
    if (hasAllowedFormatsOverride)             cliRuleOverrides["allowed_formats"] = config.allowedFormats;
    if (hasForbiddenExtensionsOverride)        cliRuleOverrides["forbidden_extensions"] = config.forbiddenExtensions;
    if (maxFileSizeMbOverride >= 0.0)          cliRuleOverrides["max_file_size_mb"] = config.maxFileSizeMb;
    if (minFileSizeMbOverride >= 0.0)          cliRuleOverrides["min_file_size_mb"] = config.minFileSizeMb;
    if (maxMeshesOverride >= 0)                cliRuleOverrides["max_mesh_count"] = config.maxMeshCount;
    if (minMeshesOverride >= 0)                cliRuleOverrides["min_mesh_count"] = config.minMeshCount;
    if (maxMaterialsOverride >= 0)             cliRuleOverrides["max_material_count"] = config.maxMaterialCount;
    if (minMaterialsOverride >= 0)             cliRuleOverrides["min_material_count"] = config.minMaterialCount;
    if (maxVerticesOverride >= 0)              cliRuleOverrides["max_vertex_count"] = config.maxVertexCount;
    if (minVerticesOverride >= 0)              cliRuleOverrides["min_vertex_count"] = config.minVertexCount;
    if (maxAcmrOverride >= 0.0)                cliRuleOverrides["max_acmr"] = config.maxAcmr;
    if (maxTrianglesOverride >= 0)             cliRuleOverrides["max_triangle_count"] = config.maxTriangleCount;
    if (maxTrianglesPerMeshOverride >= 0)      cliRuleOverrides["max_triangles_per_mesh"] = config.maxTrianglesPerMesh;
    if (maxBonesOverride >= 0)                 cliRuleOverrides["max_bones"] = config.maxBoneCount;
    if (maxSubmeshesOverride >= 0)            cliRuleOverrides["max_submesh_count"] = config.maxSubmeshCount;
    if (maxDrawCallsOverride >= 0)             cliRuleOverrides["max_draw_calls"] = config.maxDrawCalls;
    if (maxAnimKeyframesOverride >= 0)         cliRuleOverrides["max_anim_keyframes"] = config.maxAnimKeyframes;
    if (minAnimKeyframesOverride >= 0)         cliRuleOverrides["min_anim_keyframes"] = config.minAnimKeyframes;
    if (maxAnimDurationOverride >= 0.0)        cliRuleOverrides["max_anim_duration"] = config.maxAnimDuration;
    if (minAnimDurationOverride >= 0.0)        cliRuleOverrides["min_anim_duration"] = config.minAnimDuration;
    if (requireSkeletonOverride >= 0)          cliRuleOverrides["require_skeleton"] = config.requireSkeleton;
    if (requireAnimationsOverride >= 0)        cliRuleOverrides["require_animations"] = config.requireAnimations;
    if (allowEmbeddedTexturesOverride >= 0)    cliRuleOverrides["allow_embedded_textures"] = config.allowEmbeddedTextures;
    if (requireTexturesExistOverride >= 0)     cliRuleOverrides["require_textures_exist"] = config.requireTexturesExist;
    if (allowMissingMaterialsOverride >= 0)    cliRuleOverrides["allow_missing_materials"] = config.allowMissingMaterials;
    if (hasFileNameCaseOverride)               cliRuleOverrides["file_name_case"] = config.fileNameCase;
    if (hasRequiredAnimationNamesOverride)     cliRuleOverrides["require_animation_names"] = config.requireAnimationNames;
    if (hasRequiredBoneNamesOverride)          cliRuleOverrides["require_bone_names"] = config.requireBoneNames;
    if (!cliRuleOverrides.isEmpty()) {
        ScanScope cliOverrideScope;
        cliOverrideScope.pathPattern = "**/*";
        cliOverrideScope.rules = cliRuleOverrides;
        config.scopes.append(cliOverrideScope);
    }

    if (!failOn.isEmpty()) {
        failOn = failOn.toLower();
        if (failOn != "info" && failOn != "warning" && failOn != "error" && failOn != "never") {
            err() << "Error: --fail-on must be one of: info, warning, error, never" << Qt::endl;
            return 2;
        }
        config.failOn = failOn;
    }

    auto stripOuterQuotesToken = [](QString s) {
        s = s.trimmed();
        if (s.size() >= 2) {
            const QChar a = s.front(), b = s.back();
            if ((a == '"' && b == '"') || (a == '\'' && b == '\''))
                s = s.mid(1, s.size() - 2).trimmed();
        }
        return s;
    };

    if (!includeArg.isEmpty()) {
        config.includePatterns.clear();
        for (const auto& p : includeArg.split(",")) {
            QString pattern = stripOuterQuotesToken(p);
            if (pattern.isEmpty())
                continue;
            // Normalize bare extension patterns: *.fbx → **/*.fbx
            if (!pattern.contains("/") && !pattern.startsWith("**/"))
                pattern = "**/" + pattern;
            config.includePatterns.append(pattern);
        }
    }
    if (!excludeArg.isEmpty()) {
        for (const auto& p : excludeArg.split(",")) {
            QString pattern = stripOuterQuotesToken(p);
            if (pattern.isEmpty())
                continue;
            if (!pattern.contains("/") && !pattern.startsWith("**/"))
                pattern = "**/" + pattern;
            config.excludePatterns.append(pattern);
        }
    }

    // Validate scan root
    if (!scanRoot.isEmpty() && !QFileInfo(scanRoot).isDir()) {
        err() << "Error: Not a directory: " << scanRoot << Qt::endl;
        return 2;
    }

    SentryReporter::addBreadcrumb("cli.scan",
        QString("Scan root=%1 json=%2 fix=%3")
            .arg(scanRoot.isEmpty() ? "(default)" : scanRoot)
            .arg(jsonOutput).arg(fix));

    const bool streamTextOutput = !jsonOutput;
    const bool colorizeTextOutput = streamTextOutput && cliSupportsColor();

    // Run the scan
    ScanResult result = ScanEngine::run(
        config, scanRoot,
        streamTextOutput
            ? ScanEngine::AssetProcessedCallback(
                  [colorizeTextOutput](const AssetInfo& asset, const QList<Finding>& findings) {
                      cliWrite(formatScanAssetLine(asset, findings, colorizeTextOutput));
                  })
            : ScanEngine::AssetProcessedCallback());

    if (jsonOutput) {
        for (const auto& f : result.findings) {
            if (f.rule == QLatin1String("load_error"))
                err() << "Load error (" << f.file << "): " << f.message << Qt::endl;
        }
    }

    QJsonObject reportJson = ScanEngine::scanReportToJsonObject(result);
    if (!activeProfileId.isEmpty())
        reportJson.insert(QStringLiteral("profile"), activeProfileId);

    // Output to terminal
    if (jsonOutput) {
        cliWrite(QString::fromUtf8(QJsonDocument(reportJson).toJson(QJsonDocument::Indented)) + "\n");
    } else {
        if (!activeProfileId.isEmpty())
            cliWrite(QStringLiteral("Profile: %1\n").arg(activeProfileId));
        cliWrite(formatScanSummary(result, colorizeTextOutput));
    }

    // Write report files
    if (!reportPath.isEmpty()) {
        QFile f(reportPath);
        QDir().mkpath(QFileInfo(reportPath).path());
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write(QJsonDocument(reportJson).toJson(QJsonDocument::Indented));
        else
            err() << "Warning: Could not write report to " << reportPath << Qt::endl;
    }

    if (!sarifPath.isEmpty()) {
        QFile f(sarifPath);
        QDir().mkpath(QFileInfo(sarifPath).path());
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write(ScanEngine::formatSarif(result, activeProfileId).toUtf8());
        else
            err() << "Warning: Could not write SARIF report to " << sarifPath << Qt::endl;
    }

    // Also write reports if configured in the config file
    if (reportPath.isEmpty() && !config.reportOutput.isEmpty()) {
        QFile f(config.reportOutput);
        QDir().mkpath(QFileInfo(config.reportOutput).path());
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            if (config.reportFormat == "text")
                f.write(ScanEngine::formatText(result, config, false, activeProfileId).toUtf8());
            else
                f.write(QJsonDocument(reportJson).toJson(QJsonDocument::Indented));
        }
    }
    if (sarifPath.isEmpty() && !config.sarifOutput.isEmpty()) {
        QFile f(config.sarifOutput);
        QDir().mkpath(QFileInfo(config.sarifOutput).path());
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write(ScanEngine::formatSarif(result, activeProfileId).toUtf8());
    }

    bool uploadOk = true;
    const QString ingestToken = resolveIngestToken(tokenArg);
    if (!ingestToken.isEmpty() && !noUpload) {
        SentryReporter::addBreadcrumb(QStringLiteral("cli.scan"),
            QStringLiteral("QtMesh Cloud uploadScan: posting"));
        const QJsonObject reportForUpload = ScanEngine::mergeGithubActionsMetaIntoReport(reportJson);
        const auto up = QtMeshCloudClient::uploadScanReport(ingestToken, reportForUpload);
        uploadOk = up.ok;
        if (up.ok) {
            SentryReporter::addBreadcrumb(QStringLiteral("cli.scan"),
                QStringLiteral("QtMesh Cloud uploadScan: ok HTTP %1").arg(up.httpStatus));
        } else {
            SentryReporter::addBreadcrumb(QStringLiteral("cli.scan"),
                QStringLiteral("QtMesh Cloud uploadScan: failed HTTP %1").arg(up.httpStatus),
                QStringLiteral("warning"));
            const QString prefix = strictUpload ? QStringLiteral("Error: ")
                                                : QStringLiteral("Warning: ");
            err() << prefix << "QtMesh Cloud scan upload failed (HTTP " << up.httpStatus << "): "
                 << up.errorString;
            if (!up.responseBodySnippet.isEmpty()
                && !up.errorString.contains(up.responseBodySnippet)) {
                err() << " — " << up.responseBodySnippet;
            }
            err() << Qt::endl;
        }
    }

    maybePrintCloudPromo(jsonOutput, tokenArg);

    // Exit code from fail_on threshold (scan/lint outcome)
    int scanExit = 0;
    if (config.failOn != "never") {
        if (config.failOn == "error" && result.errors > 0)
            scanExit = 1;
        else if (config.failOn == "warning" && (result.errors > 0 || result.warnings > 0))
            scanExit = 1;
        else if (config.failOn == "info"
                 && (result.errors > 0 || result.warnings > 0 || result.infos > 0))
            scanExit = 1;
    }
    if (strictUpload && !uploadOk)
        return 1;
    return scanExit;
}

namespace {

struct MemoryCmdArgs {
    QString filePath;
    QString tokenArg;
    bool jsonOutput = false;
    bool noCloud = false;
    quint64 budgetBytes = 0;
    bool budgetExplicit = false;
};

// Parse argv into MemoryCmdArgs. Returns:
//   0 = parsed ok, run command
//   1 = print usage + exit 2 (missing file / bad value)
//   2 = print "invalid budget" + exit 2
int parseMemoryArgs(int argc, char* argv[], MemoryCmdArgs& out)
{
    int i = 1;
    while (i < argc) {
        const QString arg(argv[i]);
        ++i;
        if (arg == "memory" || arg == "--cli") continue;
        if (arg == "--json")     { out.jsonOutput = true; continue; }
        if (arg == "--no-cloud") { out.noCloud    = true; continue; }
        if (arg == "--token" && i < argc) {
            out.tokenArg = QString::fromLocal8Bit(argv[i++]);
            continue;
        }
        if (arg == "--budget" && i < argc) {
            out.budgetBytes = MemoryEstimator::parseBudget(argv[i++]);
            if (out.budgetBytes == 0) return 2;
            out.budgetExplicit = true;
            continue;
        }
        if (!arg.startsWith("-") && out.filePath.isEmpty()) {
            out.filePath = arg;
        }
    }
    return out.filePath.isEmpty() ? 1 : 0;
}

// Apply QtMesh Cloud's rules.memory_budget_mb when no explicit --budget was
// given. Mutates budgetBytes and budgetSource; never fails the command.
void applyCloudBudget(const QString& tokenArg, quint64& budgetBytes, QString& budgetSource)
{
    const QString ingest = resolveIngestToken(tokenArg);
    if (ingest.isEmpty()) return;

    SentryReporter::addBreadcrumb(QStringLiteral("cli.memory"),
        QStringLiteral("QtMesh Cloud fetchRules: requested"));
    const auto rules = QtMeshCloudClient::fetchRules(ingest);
    if (!rules.ok) {
        err() << "Warning: Could not load QtMesh Cloud rules ("
              << rules.errorString << "). Continuing without remote budget." << Qt::endl;
        SentryReporter::addBreadcrumb(QStringLiteral("cli.memory"),
            QStringLiteral("QtMesh Cloud fetchRules: failed %1").arg(rules.errorString),
            QStringLiteral("warning"));
        return;
    }

    const QJsonObject rulesObj = rules.config.value("rules").toObject();
    const double mb = rulesObj.value("memory_budget_mb").toDouble(0.0);
    if (mb > 0.0) {
        budgetBytes = static_cast<quint64>(mb * 1024.0 * 1024.0);
        budgetSource = QStringLiteral("cloud:%1").arg(rules.source);
        err() << "Note: Using QtMesh Cloud memory_budget_mb="
              << mb << " (source: " << rules.source << ")." << Qt::endl;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("cli.memory"),
        QStringLiteral("QtMesh Cloud fetchRules: ok source=%1 budget_mb=%2")
            .arg(rules.source).arg(mb));
}

void emitMemoryReport(const SceneMemoryReport& report, const QFileInfo& fi,
                      const QString& budgetSource, bool jsonOutput)
{
    if (jsonOutput) {
        QJsonObject obj = MemoryEstimator::toJson(report);
        obj["file"] = fi.fileName();
        if (!budgetSource.isEmpty())
            obj["budgetSource"] = budgetSource;
        cliWrite(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented)));
    } else {
        cliWrite(MemoryEstimator::toText(report));
    }
}

} // namespace

int CLIPipeline::cmdMemory(int argc, char* argv[])
{
    // Parse: memory <file> [--json] [--budget <size>] [--token <t>] [--no-cloud]
    MemoryCmdArgs cmdArgs;
    const int parseRc = parseMemoryArgs(argc, argv, cmdArgs);
    if (parseRc == 2) {
        err() << "Error: Invalid --budget value. Use a positive size "
                 "(e.g. 50MB, 1GB) or omit --budget for unlimited." << Qt::endl;
        return 2;
    }
    if (parseRc == 1) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh memory <file> [--json] [--budget <size>] [--token <t>] [--no-cloud]"
              << Qt::endl;
        return 2;
    }

    const QFileInfo fi(cmdArgs.filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << cmdArgs.filePath << Qt::endl;
        return 1;
    }

    // No explicit --budget: try QtMesh Cloud's memory_budget_mb (token gated).
    QString budgetSource = cmdArgs.budgetExplicit ? QStringLiteral("cli") : QString();
    if (!cmdArgs.budgetExplicit && !cmdArgs.noCloud)
        applyCloudBudget(cmdArgs.tokenArg, cmdArgs.budgetBytes, budgetSource);

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.memory",
        QString("Memory .%1%2 source=%3").arg(
            fi.suffix(),
            cmdArgs.budgetBytes > 0 ? QString(" budget=%1B").arg(cmdArgs.budgetBytes) : QString(),
            budgetSource.isEmpty() ? QStringLiteral("none") : budgetSource));

    MeshImporterExporter::importer({fi.absoluteFilePath()}, 0);
    if (const auto& entities = Manager::getSingleton()->getEntities(); entities.isEmpty()) {
        err() << "Error: Failed to load file: " << cmdArgs.filePath << Qt::endl;
        return 1;
    }

    const SceneMemoryReport report = MemoryEstimator::estimateScene(cmdArgs.budgetBytes);
    emitMemoryReport(report, fi, budgetSource, cmdArgs.jsonOutput);
    return report.overBudget() ? 1 : 0;
}

int CLIPipeline::cmdAnalyze(int argc, char* argv[])
{
    // Parse: analyze <file> [--json]
    QString filePath;
    bool jsonOutput = false;

    for (int i = 1; i < argc; ++i) {
        const QString arg(argv[i]);
        if (arg == "analyze" || arg == "--cli") continue;
        if (arg == "--json") { jsonOutput = true; continue; }
        if (!arg.startsWith("-") && filePath.isEmpty()) filePath = arg;
    }

    if (filePath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh analyze <file> [--json]" << Qt::endl;
        return 2;
    }

    const QFileInfo fi(filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << filePath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.analyze",
        QString("Analyze .%1").arg(fi.suffix()));
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing %1").arg(fi.absoluteFilePath()));

    MeshImporterExporter::importer({fi.absoluteFilePath()}, 0);
    const auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        err() << "Error: Failed to load file: " << filePath << Qt::endl;
        return 1;
    }

    const DrawCallReport report = DrawCallAnalyzer::analyze(entities);

    if (jsonOutput) {
        QJsonObject obj = DrawCallAnalyzer::toJson(report);
        obj["file"] = fi.fileName();
        cliWrite(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented)));
    } else {
        cliWrite(QString("File: %1\n").arg(fi.fileName()));
        cliWrite(DrawCallAnalyzer::toText(report));
    }
    return 0;
}

namespace {

struct VertexCacheCmdArgs {
    QString filePath;
    QString outputPath;
    bool jsonOutput = false;
};

// Parse argv for cmdVertexCache. Returns true on success.
bool parseVertexCacheArgs(int argc, char* argv[], VertexCacheCmdArgs& out)
{
    int i = 1;
    while (i < argc) {
        const QString arg(argv[i]);
        ++i;
        if (arg == "vertex-cache" || arg == "--cli") continue;
        if (arg == "--json")              { out.jsonOutput = true; continue; }
        if (arg == "-o" && i < argc)      { out.outputPath = argv[i++]; continue; }
        if (!arg.startsWith("-") && out.filePath.isEmpty()) out.filePath = arg;
    }
    return !out.filePath.isEmpty();
}

// Export the (potentially mutated) scene rooted at `entities.first()` to
// `outputPath`. Returns 0 on success, 1 on export failure.
int exportRewrittenMesh(const QList<Ogre::Entity*>& entities,
                        const QFileInfo& srcFi, const QString& outputPath)
{
    const Ogre::Entity* entity = entities.first();
    const auto* node = entity->getParentSceneNode();
    const QFileInfo outFi(outputPath);
    const QString fmt = CLIPipeline::formatForExtension(outputPath);
    SentryReporter::addBreadcrumb("file.export",
        QString("Exporting %1").arg(outFi.absoluteFilePath()));
    if (MeshImporterExporter::exporter(node, outFi.absoluteFilePath(), fmt) != 0) {
        SentryReporter::captureMessage(
            QString("CLI vertex-cache: export failed (.%1 -> .%2)")
                .arg(srcFi.suffix(), outFi.suffix()), "error");
        err() << "Error: Export failed." << Qt::endl;
        return 1;
    }
    return 0;
}

void emitVertexCacheReport(const VertexCacheReport& report, const QFileInfo& fi,
                           const QString& outputPath, bool jsonOutput)
{
    const bool rewrite = !outputPath.isEmpty();
    if (jsonOutput) {
        QJsonObject obj = VertexCacheOptimizer::toJson(report);
        obj["file"] = fi.fileName();
        if (rewrite) obj["output"] = QFileInfo(outputPath).fileName();
        cliWrite(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented)));
    } else {
        cliWrite(QString("File: %1%2\n")
                     .arg(fi.fileName(),
                          rewrite ? QString(" -> %1").arg(QFileInfo(outputPath).fileName())
                                  : QString()));
        cliWrite(VertexCacheOptimizer::toText(report));
    }
}

} // namespace

int CLIPipeline::cmdVertexCache(int argc, char* argv[])
{
    VertexCacheCmdArgs cmdArgs;
    if (!parseVertexCacheArgs(argc, argv, cmdArgs)) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh vertex-cache <file> [-o <output>] [--json]" << Qt::endl;
        return 2;
    }

    const QFileInfo fi(cmdArgs.filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << cmdArgs.filePath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    const bool rewrite = !cmdArgs.outputPath.isEmpty();
    SentryReporter::addBreadcrumb("cli.vertex-cache",
        QString("Vertex-cache .%1%2").arg(fi.suffix(), rewrite ? " rewrite" : " analyze"));
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing %1").arg(fi.absoluteFilePath()));

    MeshImporterExporter::importer({fi.absoluteFilePath()}, 0);
    const auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        err() << "Error: Failed to load file: " << cmdArgs.filePath << Qt::endl;
        return 1;
    }

    VertexCacheReport aggregate;
    for (Ogre::Entity* entity : entities) {
        VertexCacheOptimizer::mergeReport(
            aggregate, VertexCacheOptimizer::analyzeEntity(entity, rewrite));
    }
    VertexCacheOptimizer::finalize(aggregate);

    if (rewrite) {
        if (const int rc = exportRewrittenMesh(entities, fi, cmdArgs.outputPath); rc != 0)
            return rc;
    }

    emitVertexCacheReport(aggregate, fi, cmdArgs.outputPath, cmdArgs.jsonOutput);
    return 0;
}

namespace {

struct DecimateCmdArgs {
    QString filePath;
    QString outputPath;
    bool jsonOutput = false;
    double reduction = -1.0;  // -1 = unset (only one of reduction / targetTris / targetVerts wins)
    int targetTris = -1;
    int targetVerts = -1;
    QString algo = "ogre";    // "ogre" (default) or "meshopt". Same options as `qtmesh lod --algo`.
};

// Parse a numeric --target-* flag strictly: any non-numeric input (e.g. a
// typo like `--target-tris foo`) is rejected with an error rather than
// silently falling through to 0 (which clampReduction would interpret as a
// 95% reduction — a destructive mismatch). Returns true on success.
bool parseStrictInt(const QString& flag, const QString& raw, int& out)
{
    bool ok = false;
    const int v = raw.toInt(&ok);
    if (!ok) {
        err() << "Error: " << flag << " expects an integer, got: " << raw << Qt::endl;
        return false;
    }
    out = v;
    return true;
}

bool parseStrictDouble(const QString& flag, const QString& raw, double& out)
{
    bool ok = false;
    const double v = raw.toDouble(&ok);
    if (!ok) {
        err() << "Error: " << flag << " expects a number, got: " << raw << Qt::endl;
        return false;
    }
    out = v;
    return true;
}

// Apply one argv[i] token to `out`. `i` is updated when a value-taking flag
// consumes its successor. Returns 1 on success, 0 on parse error.
int applyDecimateArg(const QString& arg, int argc, char* argv[], int& i,
                     DecimateCmdArgs& out)
{
    if (arg == "decimate" || arg == "--cli") return 1;
    if (arg == "--json")          { out.jsonOutput = true; return 1; }
    if (arg == "-o" && i < argc)  { out.outputPath = argv[i++]; return 1; }
    if (arg == "--reduction" && i < argc) {
        return parseStrictDouble("--reduction",
                                 QString::fromLocal8Bit(argv[i++]),
                                 out.reduction) ? 1 : 0;
    }
    if (arg == "--target-tris" && i < argc) {
        return parseStrictInt("--target-tris",
                              QString::fromLocal8Bit(argv[i++]),
                              out.targetTris) ? 1 : 0;
    }
    if (arg == "--target-verts" && i < argc) {
        return parseStrictInt("--target-verts",
                              QString::fromLocal8Bit(argv[i++]),
                              out.targetVerts) ? 1 : 0;
    }
    if (arg == "--algo" && i < argc) {
        const QString val = QString::fromLocal8Bit(argv[i++]).toLower();
        if (val != "ogre" && val != "meshopt") {
            err() << "Error: --algo must be 'ogre' or 'meshopt' (got '" << val << "')." << Qt::endl;
            return 0;
        }
        out.algo = val;
        return 1;
    }
    if (!arg.startsWith("-") && out.filePath.isEmpty()) out.filePath = arg;
    return 1;
}

// Returns 1 on success, 0 on usage error (callers should return 2).
int parseDecimateArgs(int argc, char* argv[], DecimateCmdArgs& out)
{
    int i = 1;
    while (i < argc) {
        const QString arg(argv[i]);
        ++i;
        if (applyDecimateArg(arg, argc, argv, i, out) == 0) return 0;
    }
    if (out.filePath.isEmpty()) return 0;

    // Enforce exactly one target mode — "at least one" lets the user pass
    // ambiguous combinations like --reduction 0.5 --target-tris 1000.
    if (const int modesProvided = (out.reduction >= 0.0 ? 1 : 0)
                                + (out.targetTris >= 0 ? 1 : 0)
                                + (out.targetVerts >= 0 ? 1 : 0);
        modesProvided > 1) {
        err() << "Error: pass exactly one of --reduction / --target-tris / --target-verts."
              << Qt::endl;
        return 0;
    }
    return 1;
}

double resolveReduction(const DecimateCmdArgs& args, int currentTris, int currentVerts)
{
    if (args.reduction >= 0.0)      return MeshDecimator::clampReduction(args.reduction);
    if (args.targetTris >= 0)       return MeshDecimator::reductionFromTargetTris(currentTris, args.targetTris);
    if (args.targetVerts >= 0)      return MeshDecimator::reductionFromTargetVerts(currentVerts, args.targetVerts);
    return 0.0;
}

void emitDecimationReport(const DecimationReport& report, const QFileInfo& fi,
                          const QString& outputPath, bool jsonOutput)
{
    if (jsonOutput) {
        QJsonObject obj = MeshDecimator::toJson(report);
        obj["file"] = fi.fileName();
        obj["output"] = QFileInfo(outputPath).fileName();
        cliWrite(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented)));
    } else {
        cliWrite(QString("File: %1 -> %2\n")
                     .arg(fi.fileName(), QFileInfo(outputPath).fileName()));
        cliWrite(MeshDecimator::toText(report));
    }
}

} // namespace

int CLIPipeline::cmdDecimate(int argc, char* argv[])
{
    DecimateCmdArgs cmdArgs;
    if (parseDecimateArgs(argc, argv, cmdArgs) == 0) {
        // parseDecimateArgs already printed a specific error to err() — only
        // emit the generic usage banner when no file was given at all.
        if (cmdArgs.filePath.isEmpty()) {
            err() << "Error: No input file specified." << Qt::endl;
            err() << "Usage: qtmesh decimate <file> -o <output> "
                     "(--reduction <r> | --target-tris N | --target-verts N) "
                     "[--algo ogre|meshopt] [--json]"
                  << Qt::endl;
        }
        return 2;
    }
    if (cmdArgs.outputPath.isEmpty()) {
        err() << "Error: --output (-o) is required for decimate (this is a destructive op; "
                 "we never overwrite the input)." << Qt::endl;
        return 2;
    }
    if (cmdArgs.reduction < 0 && cmdArgs.targetTris < 0 && cmdArgs.targetVerts < 0) {
        err() << "Error: provide one of --reduction <r>, --target-tris N, --target-verts N." << Qt::endl;
        return 2;
    }

    const QFileInfo fi(cmdArgs.filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << cmdArgs.filePath << Qt::endl;
        return 1;
    }

    // Refuse to overwrite the source asset. The earlier error promises we
    // never do this; enforce it by comparing canonical paths (handles
    // symlinks and relative paths). When the output file doesn't exist yet,
    // QFileInfo::canonicalFilePath() returns empty — fall back to absolute.
    const QFileInfo outFi(cmdArgs.outputPath);
    const QString inCanon = fi.canonicalFilePath().isEmpty()
                                ? fi.absoluteFilePath() : fi.canonicalFilePath();
    const QString outCanon = outFi.canonicalFilePath().isEmpty()
                                ? outFi.absoluteFilePath() : outFi.canonicalFilePath();
    if (inCanon == outCanon) {
        err() << "Error: -o points to the input file. Decimation is destructive; "
                 "choose a different output path." << Qt::endl;
        return 2;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.decimate",
        QString("Decimate .%1").arg(fi.suffix()));
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing %1").arg(fi.absoluteFilePath()));

    MeshImporterExporter::importer({fi.absoluteFilePath()}, 0);
    const auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        err() << "Error: Failed to load file: " << cmdArgs.filePath << Qt::endl;
        return 1;
    }
    // Multi-entity scenes: refuse rather than silently decimate the first
    // entity and exporting a partially-reduced scene. Whole-scene decimation
    // is a future-slice feature; today's contract is one entity, one output.
    if (entities.size() > 1) {
        err() << "Error: " << cmdArgs.filePath << " contains "
              << entities.size() << " mesh entities. qtmesh decimate currently "
              << "supports one entity per file — split the source or run a "
              << "follow-up scene-decimation slice when that lands." << Qt::endl;
        return 1;
    }

    Ogre::Entity* entity = entities.first();

    // Count current tris / verts to resolve target-based reductions.
    int currentTris = 0;
    int currentVerts = 0;
    MeshDecimator::countBaseline(entity, currentTris, currentVerts);

    const double reduction = resolveReduction(cmdArgs, currentTris, currentVerts);
    if (reduction <= 0.0) {
        err() << "Note: target equals or exceeds current count; nothing to do." << Qt::endl;
        // Still produce a no-op output file so callers can rely on the path existing.
    }

    const auto algoEnum = (cmdArgs.algo == "meshopt")
        ? MeshDecimator::Algorithm::Meshopt
        : MeshDecimator::Algorithm::Ogre;
    const DecimationReport report = MeshDecimator::decimateEntity(entity, reduction, algoEnum);
    if (!report.applied && reduction > 0.0) {
        err() << "Error: Decimation failed (" << cmdArgs.algo << "). The mesh may not be "
                 "suitable for in-place reduction (e.g. zero index data)." << Qt::endl;
        return 1;
    }

    const auto* node = entity->getParentSceneNode();
    // outFi already declared earlier (canonical-path guard); reuse it.
    const QString fmt = formatForExtension(cmdArgs.outputPath);
    SentryReporter::addBreadcrumb("file.export",
        QString("Exporting %1").arg(outFi.absoluteFilePath()));
    if (MeshImporterExporter::exporter(node, outFi.absoluteFilePath(), fmt) != 0) {
        SentryReporter::captureMessage(
            QString("CLI decimate: export failed (.%1 -> .%2)")
                .arg(fi.suffix(), outFi.suffix()), "error");
        err() << "Error: Export failed." << Qt::endl;
        return 1;
    }

    emitDecimationReport(report, fi, cmdArgs.outputPath, cmdArgs.jsonOutput);
    return 0;
}

// ─── Phase 6 slice G: qtmesh optimize ────────────────────────────────
//
// Sequences the slice C / C4 / D optimizations end-to-end on a single
// asset and writes the result to `-o <path>`. Reuses the same Ogre
// scene the rest of the CLI flows through, so each stage operates on
// the output of the previous one with no intermediate file I/O.

namespace {

struct OptimizeCmdArgs {
    QString filePath;
    QString outputPath;
    bool jsonOutput = false;
    bool vertexCache = false;       // explicit toggle (-= --vertex-cache)
    bool simplifyAnim = false;      // explicit toggle (-= --simplify-anim)
    bool decimateRequested = false; // any of --reduction/--target-tris/--target-verts
    double reduction = -1.0;
    int targetTris = -1;
    int targetVerts = -1;
    // Animation simplify tolerances (defaults match AnimationMerger Balanced)
    // Defaults match AnimationMerger::SimplifyTolerances{} — Conservative
    // since simplify is destructive. Override via --simplify-translation-tol /
    // --simplify-rotation-deg-tol / --simplify-scale-tol when you want
    // Balanced (1e-3 / 0.5° / 1e-3) or Aggressive (1e-2 / 1° / 1e-2)
    // reduction at the cost of perceptible drift. The shorthand
    // --simplify-preset {conservative|balanced|aggressive} sets all three
    // in one go; reject if combined with an explicit per-axis flag (#509).
    float animTranslationTol = 1e-4f;
    float animRotationDegTol = 0.05f;
    float animScaleTol       = 1e-4f;
    QString simplifyPreset;         // empty when not set; "" / "conservative" / "balanced" / "aggressive"
    bool sawExplicitTol = false;    // any --simplify-*-tol flag explicitly given
    bool explicitFlags = false;     // any optimization flag was supplied?
};

int applyOptimizeArg(const QString& arg, int argc, char* argv[], int& i,
                     OptimizeCmdArgs& out)
{
    if (arg == "optimize" || arg == "--cli") return 1;
    if (arg == "--json")            { out.jsonOutput = true; return 1; }
    if (arg == "-o" && i < argc)    { out.outputPath = argv[i++]; return 1; }
    if (arg == "--vertex-cache")    { out.vertexCache = true; out.explicitFlags = true; return 1; }
    if (arg == "--simplify-anim")   { out.simplifyAnim = true; out.explicitFlags = true; return 1; }
    if (arg == "--all") {
        out.vertexCache = true;
        out.simplifyAnim = true;
        out.explicitFlags = true;
        return 1;
    }
    if (arg == "--reduction" && i < argc) {
        out.decimateRequested = true; out.explicitFlags = true;
        if (!parseStrictDouble("--reduction",
                               QString::fromLocal8Bit(argv[i++]),
                               out.reduction)) return 0;
        if (out.reduction < 0.0) {
            err() << "Error: --reduction must be >= 0 (got " << out.reduction << ")" << Qt::endl;
            return 0;
        }
        return 1;
    }
    if (arg == "--target-tris" && i < argc) {
        out.decimateRequested = true; out.explicitFlags = true;
        if (!parseStrictInt("--target-tris",
                            QString::fromLocal8Bit(argv[i++]),
                            out.targetTris)) return 0;
        if (out.targetTris < 0) {
            err() << "Error: --target-tris must be >= 0 (got " << out.targetTris << ")" << Qt::endl;
            return 0;
        }
        return 1;
    }
    if (arg == "--target-verts" && i < argc) {
        out.decimateRequested = true; out.explicitFlags = true;
        if (!parseStrictInt("--target-verts",
                            QString::fromLocal8Bit(argv[i++]),
                            out.targetVerts)) return 0;
        if (out.targetVerts < 0) {
            err() << "Error: --target-verts must be >= 0 (got " << out.targetVerts << ")" << Qt::endl;
            return 0;
        }
        return 1;
    }
    if (arg == "--simplify-translation-tol" && i < argc) {
        double v = 0.0;
        if (!parseStrictDouble("--simplify-translation-tol",
                               QString::fromLocal8Bit(argv[i++]), v)) return 0;
        out.animTranslationTol = static_cast<float>(v);
        out.sawExplicitTol = true;
        return 1;
    }
    if (arg == "--simplify-rotation-deg-tol" && i < argc) {
        double v = 0.0;
        if (!parseStrictDouble("--simplify-rotation-deg-tol",
                               QString::fromLocal8Bit(argv[i++]), v)) return 0;
        out.animRotationDegTol = static_cast<float>(v);
        out.sawExplicitTol = true;
        return 1;
    }
    if (arg == "--simplify-scale-tol" && i < argc) {
        double v = 0.0;
        if (!parseStrictDouble("--simplify-scale-tol",
                               QString::fromLocal8Bit(argv[i++]), v)) return 0;
        out.animScaleTol = static_cast<float>(v);
        out.sawExplicitTol = true;
        return 1;
    }
    if (arg == "--simplify-preset" && i < argc) {
        out.simplifyPreset = QString::fromLocal8Bit(argv[i++]).toLower();
        return 1;
    }
    if (!arg.startsWith("-") && out.filePath.isEmpty()) out.filePath = arg;
    return 1;
}

int parseOptimizeArgs(int argc, char* argv[], OptimizeCmdArgs& out)
{
    int i = 1;
    while (i < argc) {
        const QString arg(argv[i]);
        ++i;
        if (applyOptimizeArg(arg, argc, argv, i, out) == 0) return 0;
    }
    if (out.filePath.isEmpty()) return 0;

    // Same exactly-one-target rule the decimate subcommand enforces.
    if (const int modes = (out.reduction >= 0.0 ? 1 : 0)
                        + (out.targetTris  >= 0 ? 1 : 0)
                        + (out.targetVerts >= 0 ? 1 : 0);
        modes > 1) {
        err() << "Error: pass at most one of --reduction / --target-tris / --target-verts."
              << Qt::endl;
        return 0;
    }

    // --simplify-preset: shorthand for the three --simplify-*-tol flags.
    // Resolve via AnimationMerger so CLI / Inspector / MCP / scan-engine
    // all consume the same preset table (#509). Reject combining with
    // explicit per-axis flags — letting both through silently picks one
    // over the other and surprises the caller.
    if (!out.simplifyPreset.isEmpty()) {
        if (out.sawExplicitTol) {
            err() << "Error: --simplify-preset cannot be combined with explicit "
                     "--simplify-translation-tol / --simplify-rotation-deg-tol / "
                     "--simplify-scale-tol. Use one or the other." << Qt::endl;
            return 0;
        }
        bool ok = false;
        const auto tol =
            AnimationMerger::tolerancesForPreset(out.simplifyPreset.toStdString(), &ok);
        if (!ok) {
            err() << "Error: --simplify-preset must be one of "
                     "{conservative, balanced, aggressive} (got '"
                  << out.simplifyPreset << "')." << Qt::endl;
            return 0;
        }
        out.animTranslationTol = tol.translation;
        out.animRotationDegTol = tol.rotationDeg;
        out.animScaleTol       = tol.scale;
    }

    // Default selection. The non-destructive optimizations
    // (vertex-cache + simplify-anim) run by default unless the user
    // explicitly disabled them by listing only --vertex-cache or only
    // --simplify-anim. Decimation always requires an explicit target.
    //
    // Heuristic: when at least one *non-decimate* flag was given, we
    // honor the user's exact selection. When only --reduction /
    // --target-* was given (with no --vertex-cache / --simplify-anim /
    // --all), enable the non-destructive defaults too — "decimate this
    // and clean it up" is what users mean.
    const bool anyNonDecimateFlag = out.vertexCache || out.simplifyAnim;
    if (!anyNonDecimateFlag) {
        out.vertexCache = true;
        out.simplifyAnim = true;
    }
    return 1;
}

struct StageReport {
    QString name;
    bool applied = false;
    QString summary;
    QJsonObject details;
};

void emitOptimizeReport(const QFileInfo& srcFi, const QString& outputPath,
                        qint64 srcBytes, qint64 outBytes,
                        const QList<StageReport>& stages, bool jsonOutput)
{
    if (jsonOutput) {
        QJsonObject root;
        root["file"] = srcFi.fileName();
        root["output"] = QFileInfo(outputPath).fileName();
        root["inputBytes"] = srcBytes;
        root["outputBytes"] = outBytes;
        const qint64 delta = srcBytes - outBytes;
        root["bytesDelta"] = delta;
        if (srcBytes > 0)
            root["bytesDeltaPct"] = 100.0 * static_cast<double>(delta) / static_cast<double>(srcBytes);
        QJsonArray stagesArr;
        for (const auto& s : stages) {
            QJsonObject o;
            o["name"] = s.name;
            o["applied"] = s.applied;
            if (!s.summary.isEmpty()) o["summary"] = s.summary;
            if (!s.details.isEmpty()) o["details"] = s.details;
            stagesArr.append(o);
        }
        root["stages"] = stagesArr;
        cliWrite(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)));
        return;
    }
    cliWrite(QString("File: %1 -> %2\n")
                 .arg(srcFi.fileName(), QFileInfo(outputPath).fileName()));
    cliWrite(QString("Mesh Optimization\n=================\n\n"));
    for (const auto& s : stages) {
        cliWrite(QString("  [%1] %2: %3\n")
                     .arg(s.applied ? QStringLiteral("OK") : QStringLiteral("--"))
                     .arg(s.name)
                     .arg(s.summary.isEmpty() ? QStringLiteral("skipped") : s.summary));
    }
    const qint64 delta = srcBytes - outBytes;
    const double pct = (srcBytes > 0)
        ? 100.0 * static_cast<double>(delta) / static_cast<double>(srcBytes)
        : 0.0;
    cliWrite(QString("\n  %1 KB -> %2 KB  (%3 KB %4, %5%)\n")
                 .arg(srcBytes / 1024)
                 .arg(outBytes / 1024)
                 .arg(qAbs(delta) / 1024)
                 .arg(delta >= 0 ? QStringLiteral("saved") : QStringLiteral("grew"))
                 .arg(QString::number(pct, 'f', 1)));
}

} // namespace

int CLIPipeline::cmdOptimize(int argc, char* argv[])
{
    OptimizeCmdArgs cmdArgs;
    if (parseOptimizeArgs(argc, argv, cmdArgs) == 0) {
        if (cmdArgs.filePath.isEmpty()) {
            err() << "Error: No input file specified." << Qt::endl;
            err() << "Usage: qtmesh optimize <file> -o <output> [flags] [--json]" << Qt::endl;
            err() << "  Flags: --vertex-cache  --simplify-anim  --all" << Qt::endl;
            err() << "         --reduction <r> | --target-tris N | --target-verts N" << Qt::endl;
            err() << "         --simplify-translation-tol T  --simplify-rotation-deg-tol D  --simplify-scale-tol S" << Qt::endl;
            err() << "         --simplify-preset {conservative|balanced|aggressive}" << Qt::endl;
        }
        return 2;
    }
    if (cmdArgs.outputPath.isEmpty()) {
        err() << "Error: --output (-o) is required for optimize." << Qt::endl;
        return 2;
    }
    const QFileInfo fi(cmdArgs.filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << cmdArgs.filePath << Qt::endl;
        return 1;
    }

    // Refuse to overwrite the source asset (same guard as decimate; the
    // optimize pipeline is at least as destructive when --reduction is set).
    const QFileInfo outFi(cmdArgs.outputPath);
    const QString inCanon = fi.canonicalFilePath().isEmpty()
                                ? fi.absoluteFilePath() : fi.canonicalFilePath();
    const QString outCanon = outFi.canonicalFilePath().isEmpty()
                                ? outFi.absoluteFilePath() : outFi.canonicalFilePath();
    if (inCanon == outCanon) {
        err() << "Error: -o points to the input file. optimize is potentially "
                 "destructive (decimation, anim simplify) — choose a different output path." << Qt::endl;
        return 2;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.optimize",
        QString("Optimize .%1 vertex_cache=%2 simplify_anim=%3 decimate=%4")
            .arg(fi.suffix())
            .arg(cmdArgs.vertexCache).arg(cmdArgs.simplifyAnim)
            .arg(cmdArgs.decimateRequested));
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing %1").arg(fi.absoluteFilePath()));

    QList<Ogre::SkeletonPtr> animOnlySkeletons;
    MeshImporterExporter::importer({fi.absoluteFilePath()}, 0, &animOnlySkeletons);
    const auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        err() << "Error: Failed to load file: " << cmdArgs.filePath << Qt::endl;
        return 1;
    }
    // Same one-entity contract as decimate — multi-entity scene support is
    // deferred to a future slice (issue tracker pickup).
    if (cmdArgs.decimateRequested && entities.size() > 1) {
        err() << "Error: " << cmdArgs.filePath << " contains "
              << entities.size() << " mesh entities. qtmesh optimize with "
              << "--reduction / --target-* currently supports one entity per file." << Qt::endl;
        return 1;
    }

    QList<StageReport> stages;

    // Stage 1: vertex-cache reorder (per submesh, every entity in the scene).
    if (cmdArgs.vertexCache) {
        StageReport s;
        s.name = "vertex-cache";
        VertexCacheReport aggregate;
        for (Ogre::Entity* entity : entities) {
            VertexCacheOptimizer::mergeReport(
                aggregate, VertexCacheOptimizer::analyzeEntity(entity, /*rewrite=*/true));
        }
        VertexCacheOptimizer::finalize(aggregate);
        s.applied = aggregate.totalReordered > 0 || aggregate.totalTriangles > 0;
        s.summary = QString("ACMR %1 -> %2 across %3 triangles, %4 submeshes rewritten")
                        .arg(QString::number(aggregate.weightedAcmrBefore, 'f', 3))
                        .arg(QString::number(aggregate.weightedAcmrAfter, 'f', 3))
                        .arg(aggregate.totalTriangles)
                        .arg(aggregate.totalReordered);
        s.details = VertexCacheOptimizer::toJson(aggregate);
        stages.append(s);
    }

    // Stage 2: decimation (single entity, slice D code path).
    if (cmdArgs.decimateRequested) {
        StageReport s;
        s.name = "decimate";
        Ogre::Entity* entity = entities.first();
        int currentTris = 0, currentVerts = 0;
        MeshDecimator::countBaseline(entity, currentTris, currentVerts);

        double reduction = 0.0;
        if (cmdArgs.reduction >= 0.0)
            reduction = MeshDecimator::clampReduction(cmdArgs.reduction);
        else if (cmdArgs.targetTris >= 0)
            reduction = MeshDecimator::reductionFromTargetTris(currentTris, cmdArgs.targetTris);
        else if (cmdArgs.targetVerts >= 0)
            reduction = MeshDecimator::reductionFromTargetVerts(currentVerts, cmdArgs.targetVerts);

        if (reduction <= 0.0) {
            s.applied = false;
            s.summary = "target equals or exceeds current count; nothing to do";
        } else {
            const DecimationReport report = MeshDecimator::decimateEntity(entity, reduction);
            s.applied = report.applied;
            s.summary = QString("%1% triangle reduction (%2 -> %3)")
                            .arg(QString::number(100.0 * report.effectiveReduction(), 'f', 1))
                            .arg(report.totalTrianglesBefore)
                            .arg(report.totalTrianglesAfter);
            s.details = MeshDecimator::toJson(report);
            // Mirror cmdDecimate: when a positive reduction was asked for
            // but didn't apply, that's a hard error — the asset isn't
            // suitable for in-place reduction. Don't silently emit a
            // success report.
            if (!report.applied) {
                stages.append(s);
                err() << "Error: Decimation failed (MeshLodGenerator). The mesh may not "
                         "be suitable for in-place reduction (e.g. zero index data)." << Qt::endl;
                emitOptimizeReport(fi, cmdArgs.outputPath, fi.size(), 0, stages, cmdArgs.jsonOutput);
                return 1;
            }
        }
        stages.append(s);
    }

    // Stage 3: animation simplify (same analyzer + same tolerances as
    // `qtmesh anim --simplify` and the Inspector "Simplify" button).
    // Walks *every* skeleton — multi-entity scenes (different mesh files
    // merged into one optimize call, or co-loaded animation-only skeletons)
    // had only the first one simplified before; now they all get the same
    // treatment.
    if (cmdArgs.simplifyAnim) {
        StageReport s;
        s.name = "simplify-anim";
        AnimationMerger::SimplifyTolerances tol;
        tol.translation = cmdArgs.animTranslationTol;
        tol.rotationDeg = cmdArgs.animRotationDegTol;
        tol.scale       = cmdArgs.animScaleTol;

        int totalRemoved = 0;
        long long totalKeysBefore = 0;
        QList<Ogre::SkeletonPtr> skels;
        std::set<std::string> seenSkelNames;
        for (Ogre::Entity* entity : entities) {
            if (!entity || !entity->hasSkeleton()) continue;
            Ogre::SkeletonPtr s2;
            if (auto* mesh = entity->getMesh().get())
                s2 = mesh->getSkeleton();
            if (s2 && seenSkelNames.insert(s2->getName()).second)
                skels.append(s2);
        }
        for (const auto& s2 : animOnlySkeletons) {
            if (s2 && seenSkelNames.insert(s2->getName()).second)
                skels.append(s2);
        }

        for (const Ogre::SkeletonPtr& skel : skels) {
            for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai) {
                const Ogre::Animation* a = skel->getAnimation(ai);
                if (!a) continue;
                for (const auto& [handle, track] : a->_getNodeTrackList()) {
                    Q_UNUSED(handle);
                    if (track) totalKeysBefore += track->getNumKeyFrames();
                }
            }
            // Snapshot names first — simplifyAnimation invalidates the
            // animation pointers when it rewrites tracks in place.
            std::vector<std::string> names;
            names.reserve(skel->getNumAnimations());
            for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai)
                names.push_back(skel->getAnimation(ai)->getName());
            for (const auto& n : names)
                totalRemoved += AnimationMerger::simplifyAnimation(skel.get(), n, tol);
        }

        s.applied = totalRemoved > 0;
        if (skels.isEmpty())
            s.summary = "no skeleton / animations to simplify";
        else if (totalRemoved <= 0)
            s.summary = "no additional simplification within configured tolerances";
        else
            s.summary = QString("removed %1 / %2 keyframes (%3%) across %4 skeleton(s)")
                            .arg(totalRemoved).arg(totalKeysBefore)
                            .arg(totalKeysBefore > 0
                                ? QString::number(100.0 * totalRemoved / static_cast<double>(totalKeysBefore), 'f', 1)
                                : QString("0.0"))
                            .arg(skels.size());
        QJsonObject d;
        d["removed"] = totalRemoved;
        d["totalKeyframesBefore"] = static_cast<qint64>(totalKeysBefore);
        d["skeletons"] = skels.size();
        s.details = d;
        stages.append(s);
    }

    // Export the (possibly mutated) scene rooted at the first entity. The
    // anim-simplify stage rewrites the skeleton's animation tracks in
    // place; refreshAvailableAnimationState() picks up the new tracks for
    // any per-entity AnimationState cache.
    Ogre::Entity* entity = entities.first();
    if (entity) entity->refreshAvailableAnimationState();
    const auto* node = entity->getParentSceneNode();
    const QString fmt = formatForExtension(cmdArgs.outputPath);
    SentryReporter::addBreadcrumb("file.export",
        QString("Exporting %1").arg(outFi.absoluteFilePath()));
    if (MeshImporterExporter::exporter(node, outFi.absoluteFilePath(), fmt) != 0) {
        err() << "Error: Export failed." << Qt::endl;
        return 1;
    }

    const qint64 srcBytes = fi.size();
    const qint64 outBytes = QFileInfo(cmdArgs.outputPath).size();
    emitOptimizeReport(fi, cmdArgs.outputPath, srcBytes, outBytes, stages, cmdArgs.jsonOutput);
    return 0;
}

int CLIPipeline::cmdBakeVertexColors(int argc, char* argv[])
{
    // Parse:
    //   bake-vertex-colors <file> -o <out.png>
    //                      [--resolution N] [--dilation N] [--json]
    QString inputPath, outputPath;
    int resolution = 1024;
    int dilation = 4;
    bool jsonOutput = false;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "bake-vertex-colors" || arg == "--cli") continue;
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString(argv[++i]); continue;
        }
        if (arg == "--resolution" && i + 1 < argc) {
            bool ok = false;
            const int v = QString(argv[++i]).toInt(&ok);
            if (!ok || v < 16 || v > 8192) {
                err() << "Error: --resolution must be an integer in [16..8192]" << Qt::endl;
                return 2;
            }
            resolution = v; continue;
        }
        if (arg == "--dilation" && i + 1 < argc) {
            bool ok = false;
            const int v = QString(argv[++i]).toInt(&ok);
            if (!ok || v < 0 || v > 64) {
                err() << "Error: --dilation must be an integer in [0..64]" << Qt::endl;
                return 2;
            }
            dilation = v; continue;
        }
        if (arg == "--json") { jsonOutput = true; continue; }
        if (!arg.startsWith("-") && inputPath.isEmpty()) {
            inputPath = arg; continue;
        }
    }

    if (inputPath.isEmpty() || outputPath.isEmpty()) {
        err() << "Error: missing required arguments." << Qt::endl;
        err() << "Usage: qtmesh bake-vertex-colors <file> -o <out.png>" << Qt::endl;
        err() << "         [--resolution N] [--dilation N] [--json]" << Qt::endl;
        return 2;
    }

    const QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << inputPath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.bake-vertex-colors",
        QString("Bake vertex colors → %1×%1 PNG (dilation=%2)").arg(resolution).arg(dilation));
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing %1").arg(fi.absoluteFilePath()));

    MeshImporterExporter::importer({fi.absoluteFilePath()}, 0);
    const auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        err() << "Error: Failed to load file: " << inputPath << Qt::endl;
        return 1;
    }

    // Bake the first entity found. Multi-entity scenes get a warning; the
    // caller can pre-split the scene if they need per-entity bakes.
    Ogre::Entity* entity = nullptr;
    int entityCount = 0;
    for (auto* obj : entities) {
        if (!obj || obj->getMovableType() != "Entity") continue;
        if (!entity) entity = static_cast<Ogre::Entity*>(obj);
        ++entityCount;
    }
    if (!entity) {
        err() << "Error: No mesh entity in: " << inputPath << Qt::endl;
        return 1;
    }
    if (entityCount > 1) {
        err() << "Note: " << entityCount
              << " entities in scene; baking the first only." << Qt::endl;
    }

    EditableMesh mesh;
    if (!mesh.loadFromEntity(entity)) {
        err() << "Error: Failed to decompose mesh for: " << inputPath << Qt::endl;
        return 1;
    }

    TexturePaintBuffer buffer;
    VertexColorBaker::Options opts;
    opts.resolution = resolution;
    opts.dilationPixels = dilation;
    const int painted = VertexColorBaker::bake(mesh, buffer, opts);

    if (!buffer.save(outputPath.toStdString())) {
        err() << "Error: Failed to write: " << outputPath << Qt::endl;
        return 1;
    }

    if (jsonOutput) {
        QJsonObject root;
        root["input"] = inputPath;
        root["output"] = outputPath;
        root["resolution"] = resolution;
        root["dilation"] = dilation;
        root["pixels_rasterized"] = painted;
        root["entity"] = QString::fromStdString(entity->getName());
        QJsonDocument doc(root);
        cliWrite(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)) + "\n");
    } else {
        cliWrite(QStringLiteral("Baked %1 pixels into %2×%2 texture: %3\n")
                     .arg(painted).arg(resolution).arg(outputPath));
        cliWrite(QStringLiteral("  dilation: %1 px\n").arg(dilation));
    }
    return 0;
}

namespace {

// Per-vertex signature used to disambiguate same-position vertices
// (UV seams, hard edges, weight-splits) when building the bake↔glTF
// permutation. Position alone is ambiguous on these splits — multiple
// distinct vertices share the same bind-pose position, and matching
// by position only would arbitrarily swap them between vertex slots
// and misroute the normals texture half.
struct BakeVertex {
    Ogre::Vector3 position;
    Ogre::Vector3 normal;
    float         uv0u;
    float         uv0v;
    bool          hasNormal;
    bool          hasUV;
};

// Read Ogre bind-pose vertex signatures from `entity` in vertex-buffer
// order, matching the order `VATBaker::collectPostSkinPositions` walks
// (submesh-index, skip-shared-after-first). Returned vector has length
// equal to the bake's `vertexCount`.
std::vector<BakeVertex> readOgreBindVertices(Ogre::Entity* entity)
{
    std::vector<BakeVertex> out;
    if (!entity) return out;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return out;

    bool sharedAppended = false;
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub) continue;
        if (sub->useSharedVertices && sharedAppended) continue;

        const Ogre::VertexData* vData = sub->useSharedVertices
            ? mesh->sharedVertexData
            : sub->vertexData;
        if (!vData) continue;

        const auto* posElem = vData->vertexDeclaration->findElementBySemantic(
            Ogre::VES_POSITION);
        if (!posElem) continue;

        const auto* normElem = vData->vertexDeclaration->findElementBySemantic(
            Ogre::VES_NORMAL);
        // UV0 specifically (semantic + index 0). Subsequent UV sets are
        // ignored — Assimp's glTF export writes UV0 to TEXCOORD_0 in order.
        const auto* uvElem = vData->vertexDeclaration->findElementBySemantic(
            Ogre::VES_TEXTURE_COORDINATES, 0);

        // Each VES_* may live in a different bound vertex buffer; lock all
        // sources that this submesh touches.
        auto lockSource = [&](unsigned short src) -> unsigned char* {
            auto vbuf = vData->vertexBufferBinding->getBuffer(src);
            if (!vbuf) return nullptr;
            return static_cast<unsigned char*>(
                vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        };
        auto unlockSource = [&](unsigned short src) {
            auto vbuf = vData->vertexBufferBinding->getBuffer(src);
            if (vbuf) vbuf->unlock();
        };

        unsigned char* posBytes  = lockSource(posElem->getSource());
        unsigned char* normBytes = normElem
            ? (normElem->getSource() == posElem->getSource()
                   ? posBytes : lockSource(normElem->getSource()))
            : nullptr;
        unsigned char* uvBytes   = uvElem
            ? (uvElem->getSource() == posElem->getSource()
                   ? posBytes
                   : (normElem && uvElem->getSource() == normElem->getSource()
                          ? normBytes : lockSource(uvElem->getSource())))
            : nullptr;

        const size_t posStride = vData->vertexBufferBinding->getBuffer(
            posElem->getSource())->getVertexSize();
        const size_t normStride = normElem
            ? vData->vertexBufferBinding->getBuffer(
                  normElem->getSource())->getVertexSize() : 0;
        const size_t uvStride = uvElem
            ? vData->vertexBufferBinding->getBuffer(
                  uvElem->getSource())->getVertexSize() : 0;

        for (size_t j = 0; j < vData->vertexCount; ++j) {
            BakeVertex bv{};
            Ogre::Real* pPos = nullptr;
            posElem->baseVertexPointerToElement(posBytes + j * posStride, &pPos);
            bv.position = {pPos[0], pPos[1], pPos[2]};
            if (normElem && normBytes) {
                Ogre::Real* pN = nullptr;
                normElem->baseVertexPointerToElement(normBytes + j * normStride, &pN);
                bv.normal = {pN[0], pN[1], pN[2]};
                bv.hasNormal = true;
            }
            if (uvElem && uvBytes) {
                Ogre::Real* pUV = nullptr;
                uvElem->baseVertexPointerToElement(uvBytes + j * uvStride, &pUV);
                bv.uv0u = pUV[0];
                bv.uv0v = pUV[1];
                bv.hasUV = true;
            }
            out.push_back(bv);
        }

        // Unlock in matching order — same buffer must only be unlocked once.
        std::set<unsigned short> unlocked;
        unlocked.insert(posElem->getSource());
        unlockSource(posElem->getSource());
        if (normElem && !unlocked.count(normElem->getSource())) {
            unlocked.insert(normElem->getSource());
            unlockSource(normElem->getSource());
        }
        if (uvElem && !unlocked.count(uvElem->getSource())) {
            unlockSource(uvElem->getSource());
        }
        if (sub->useSharedVertices) sharedAppended = true;
    }
    return out;
}

// Read positions + normals + UV0 per primitive from a glTF file. `out`
// is a flat list parallel to `readOgreBindVertices` — concatenated in
// primitive-index order. Returns true on success.
bool readGltfVertices(const QString& gltfPath,
                      std::vector<BakeVertex>& out)
{
    out.clear();
    QFile f(gltfPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray jsonBytes = f.readAll();
    f.close();
    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    QJsonObject root = doc.object();

    QJsonArray buffers = root.value(QStringLiteral("buffers")).toArray();
    QFileInfo gi(gltfPath);
    QVector<QByteArray> bufData(buffers.size());
    for (int i = 0; i < buffers.size(); ++i) {
        QJsonObject b = buffers.at(i).toObject();
        QString uri = b.value(QStringLiteral("uri")).toString();
        if (uri.isEmpty()) return false;
        QFile bf(gi.absoluteDir().filePath(uri));
        if (!bf.open(QIODevice::ReadOnly)) return false;
        bufData[i] = bf.readAll();
    }

    QJsonArray accessors    = root.value(QStringLiteral("accessors")).toArray();
    QJsonArray bufferViews  = root.value(QStringLiteral("bufferViews")).toArray();
    QJsonArray meshes       = root.value(QStringLiteral("meshes")).toArray();
    if (meshes.isEmpty()) return false;

    // Per-accessor reader returning N×3 (or N×2) floats. Returns false on
    // any decoding error so the caller can short-circuit alignment.
    auto readVec = [&](int accIdx, int components, std::vector<float>& dst) -> bool {
        if (accIdx < 0 || accIdx >= accessors.size()) return false;
        QJsonObject acc = accessors.at(accIdx).toObject();
        int bvIdx = acc.value(QStringLiteral("bufferView")).toInt(-1);
        if (bvIdx < 0 || bvIdx >= bufferViews.size()) return false;
        int count = acc.value(QStringLiteral("count")).toInt(0);
        int byteOffsetAcc = acc.value(QStringLiteral("byteOffset")).toInt(0);
        QJsonObject bv = bufferViews.at(bvIdx).toObject();
        int bufferIdx = bv.value(QStringLiteral("buffer")).toInt(-1);
        int byteOffsetBv = bv.value(QStringLiteral("byteOffset")).toInt(0);
        int byteStride = bv.value(QStringLiteral("byteStride")).toInt(components * 4);
        if (bufferIdx < 0 || bufferIdx >= bufData.size()) return false;
        const QByteArray& bd = bufData[bufferIdx];
        const int start = byteOffsetBv + byteOffsetAcc;
        if (start + count * byteStride > bd.size()) return false;
        const auto* base = reinterpret_cast<const unsigned char*>(bd.constData() + start);
        dst.reserve(dst.size() + static_cast<size_t>(count) * components);
        for (int i = 0; i < count; ++i) {
            const float* p = reinterpret_cast<const float*>(base + i * byteStride);
            for (int c = 0; c < components; ++c) dst.push_back(p[c]);
        }
        return true;
    };

    QJsonObject mesh0 = meshes.first().toObject();
    QJsonArray prims = mesh0.value(QStringLiteral("primitives")).toArray();
    for (const auto& pv : prims) {
        QJsonObject prim = pv.toObject();
        QJsonObject attrs = prim.value(QStringLiteral("attributes")).toObject();
        const int posIdx  = attrs.value(QStringLiteral("POSITION")).toInt(-1);
        const int normIdx = attrs.value(QStringLiteral("NORMAL")).toInt(-1);
        const int uvIdx   = attrs.value(QStringLiteral("TEXCOORD_0")).toInt(-1);

        std::vector<float> posBuf, normBuf, uvBuf;
        if (!readVec(posIdx, 3, posBuf)) return false;
        const size_t n = posBuf.size() / 3;
        const bool hasNormal = (normIdx >= 0) && readVec(normIdx, 3, normBuf);
        const bool hasUV     = (uvIdx   >= 0) && readVec(uvIdx,   2, uvBuf);
        // If a sub-attribute exists but failed to read (count mismatch
        // etc.), treat the primitive's signature as positions-only rather
        // than feed truncated arrays into the matcher.
        const bool useNormal = hasNormal && (normBuf.size() / 3 == n);
        const bool useUV     = hasUV     && (uvBuf.size()   / 2 == n);

        for (size_t i = 0; i < n; ++i) {
            BakeVertex bv{};
            bv.position = {posBuf[i*3 + 0], posBuf[i*3 + 1], posBuf[i*3 + 2]};
            if (useNormal) {
                bv.normal = {normBuf[i*3 + 0], normBuf[i*3 + 1], normBuf[i*3 + 2]};
                bv.hasNormal = true;
            }
            if (useUV) {
                bv.uv0u = uvBuf[i*2 + 0];
                // V convention is exporter-dependent — Assimp's gltf2
                // path has flipped vs not-flipped across engine builds
                // depending on which post-process flags it applied
                // (FlipUVs is bundled into ConvertToLeftHanded and can
                // be re-applied/cancelled inside the exporter). Rather
                // than guess, we keep V as-written here; the matcher
                // (buildVertexPermutation) tries both V conventions
                // and picks the one that resolves all vertices.
                bv.uv0v = uvBuf[i*2 + 1];
                bv.hasUV = true;
            }
            out.push_back(bv);
        }
    }
    return true;
}

// Build a permutation `perm` such that Ogre vertex index `i` should
// be written to texture column `perm[i]`. Matching is per-submesh
// (Assimp's `JoinIdenticalVertices` permutes within a primitive but
// never across primitives — submesh boundaries match glTF primitive
// boundaries 1:1).
//
// The match key is the full bind-pose vertex signature — position,
// normal, and UV0 — not position alone. UV seams, hard edges, and
// weight-splits all introduce multiple distinct vertices that share
// the same bind-pose position but differ in normal/UV; matching by
// position only would arbitrarily swap them between vertex slots and
// silently misroute the normals half of the bake.
//
// Returns empty vector if any Ogre vertex has no unique match in the
// glTF (zero matches, or two glTF candidates with identical
// signatures — both safer to fall back than guess).
// Forward declaration of the inner worker — `buildVertexPermutation`
// calls it twice with `flipV = false` and `flipV = true` so the
// matcher succeeds regardless of whether Assimp's gltf2 exporter
// preserved or flipped V on this engine version.
static std::vector<uint32_t> buildVertexPermutationImpl(
    const std::vector<BakeVertex>& ogre,
    const std::vector<BakeVertex>& gltf,
    const std::vector<size_t>& submeshStarts,
    bool flipGltfV);

std::vector<uint32_t> buildVertexPermutation(
    const std::vector<BakeVertex>& ogre,
    const std::vector<BakeVertex>& gltf,
    const std::vector<size_t>& submeshStarts)
{
    // Try V-as-written first (matches the explicit FlipUVs export
    // path); fall back to V-flipped if any vertex doesn't resolve.
    auto perm = buildVertexPermutationImpl(ogre, gltf, submeshStarts, false);
    if (!perm.empty()) return perm;
    return buildVertexPermutationImpl(ogre, gltf, submeshStarts, true);
}

static std::vector<uint32_t> buildVertexPermutationImpl(
    const std::vector<BakeVertex>& ogre,
    const std::vector<BakeVertex>& gltf,
    const std::vector<size_t>& submeshStarts,
    bool flipGltfV)
{
    std::vector<uint32_t> perm;
    if (ogre.size() != gltf.size() || ogre.empty()) return perm;
    perm.resize(ogre.size(), UINT32_MAX);

    // Two-tier quantization tolerance:
    //   positions: 1e-5 (≈ sub-millimeter on a 1-unit model — these
    //     round-trip exactly through float32 → glTF binary → float32)
    //   normals + UVs: 1e-3 (one part in a thousand — Assimp's glTF
    //     exporter re-normalises normals and may re-encode UVs through
    //     a separate code path, both of which introduce sub-1e-4 drift
    //     that a tighter quantizer would split into separate buckets)
    //
    // The looser normal/UV tolerance is still tight enough to keep
    // genuinely distinct vertices (UV seams, hard edges) in separate
    // buckets — typical seam splits change UVs by ≥1e-2 and normals by
    // ≥1e-2 unit length.
    auto qPos = [](float v) -> int64_t {
        return static_cast<int64_t>(std::lround(static_cast<double>(v) * 1e5));
    };
    auto qLoose = [](float v) -> int64_t {
        return static_cast<int64_t>(std::lround(static_cast<double>(v) * 1e3));
    };
    struct Key {
        int64_t px, py, pz;   // position
        int64_t nx, ny, nz;   // normal (zero if absent)
        int64_t u,  v;        // UV0 (zero if absent)
        int8_t  flags;        // bit 0 = hasNormal, bit 1 = hasUV
        bool operator==(const Key& o) const noexcept {
            return px==o.px && py==o.py && pz==o.pz
                && nx==o.nx && ny==o.ny && nz==o.nz
                && u==o.u   && v==o.v
                && flags==o.flags;
        }
    };
    auto mixHash = [](size_t& h, int64_t v) {
        h ^= std::hash<int64_t>{}(v) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            size_t h = std::hash<int64_t>{}(k.px);
            auto mix = [&](int64_t v) {
                h ^= std::hash<int64_t>{}(v) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            };
            mix(k.py); mix(k.pz);
            mix(k.nx); mix(k.ny); mix(k.nz);
            mix(k.u);  mix(k.v);
            mix(k.flags);
            return h;
        }
    };
    auto keyOf = [&](const BakeVertex& v) -> Key {
        Key k{};
        k.px = qPos(v.position.x); k.py = qPos(v.position.y); k.pz = qPos(v.position.z);
        if (v.hasNormal) {
            k.nx = qLoose(v.normal.x); k.ny = qLoose(v.normal.y); k.nz = qLoose(v.normal.z);
            k.flags |= 1;
        }
        if (v.hasUV) {
            k.u = qLoose(v.uv0u); k.v = qLoose(v.uv0v);
            k.flags |= 2;
        }
        return k;
    };
    (void)mixHash; // silence unused-lambda warning on builds without -Wunused-lambda-capture

    // Optional V flip applied only to glTF-side keys, so we can try
    // both V conventions without copying the full BakeVertex array.
    auto gltfKey = [&](const BakeVertex& v) -> Key {
        if (!flipGltfV) return keyOf(v);
        BakeVertex flipped = v;
        if (flipped.hasUV) flipped.uv0v = 1.0f - flipped.uv0v;
        return keyOf(flipped);
    };

    // Walk each submesh in lockstep.
    for (size_t si = 0; si + 1 < submeshStarts.size(); ++si) {
        const size_t a = submeshStarts[si];
        const size_t b = submeshStarts[si + 1];
        std::unordered_map<Key, std::vector<size_t>, KeyHash> bucket;
        for (size_t j = a; j < b; ++j)
            bucket[gltfKey(gltf[j])].push_back(j);

        for (size_t i = a; i < b; ++i) {
            const Key k = keyOf(ogre[i]);
            auto it = bucket.find(k);
            if (it == bucket.end() || it->second.empty()) {
                // No match. Fall back to identity packing — caller
                // suppresses the "mesh matches the bake" claim.
                perm.clear();
                return perm;
            }
            if (it->second.size() > 1) {
                // Two distinct glTF vertices have IDENTICAL position +
                // normal + UV0. That can happen on degenerate splits
                // (e.g. a hard edge with a duplicated UV island) where
                // the only differentiator is skinning weights or vertex
                // colour. Picking either side arbitrarily would misroute
                // normals on one of them and produce subtle shading
                // glitches — refuse instead of guessing.
                perm.clear();
                return perm;
            }
            perm[i] = static_cast<uint32_t>(it->second.front());
            it->second.clear();
        }
    }
    return perm;
}

// Rewrite `gltfPath` so each primitive carries a TEXCOORD_<channel>
// attribute whose value is `(column % texWidth, column / texWidth)`
// for the matching bake column. `permutation` is the same mapping
// `buildVertexPermutation` returns: `permutation[ogre_i] = gltf_j`
// (i.e. Ogre vertex `i` lives at glTF buffer index `j` after Assimp's
// JoinIdenticalVertices reorder). We invert it to `ogreIndexFor[j]`
// per primitive, then write that as the UV2 attribute.
//
// Why a post-pass JSON rewrite instead of seeding UV2 on the Ogre
// side before export: Assimp's gltf2 exporter ALWAYS runs
// JoinIdenticalVertices, which permutes the vertex buffer
// independently of any input UV channel — so the input UV2 would
// land at the wrong vertex indices in the output. Writing UV2 AFTER
// the export, using the permutation we built by reading the
// post-export positions back, sidesteps the issue.
//
// Returns true on success. On any failure the original glTF is left
// untouched so the user still has a valid (if UV2-less) mesh.
bool emitGltfUv2(const QString& gltfPath,
                 const std::vector<uint32_t>& permutation,
                 const std::vector<size_t>& submeshStarts,
                 int texWidth,
                 int channel,
                 QString& outError)
{
    outError.clear();
    if (channel < 0 || channel > 7) {
        outError = QStringLiteral("invalid UV channel %1").arg(channel);
        return false;
    }
    if (permutation.empty()) {
        outError = QStringLiteral("permutation is empty");
        return false;
    }
    if (texWidth <= 0) {
        outError = QStringLiteral("texWidth must be > 0");
        return false;
    }

    // Read + parse the glTF.
    QFile gf(gltfPath);
    if (!gf.open(QIODevice::ReadOnly)) {
        outError = QStringLiteral("cannot open glTF: %1").arg(gltfPath);
        return false;
    }
    const QByteArray gltfBytes = gf.readAll();
    gf.close();
    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(gltfBytes, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        outError = QStringLiteral("glTF JSON parse failed: %1")
            .arg(perr.errorString());
        return false;
    }
    QJsonObject root = doc.object();

    // Load the binary buffer that we'll append to.
    QJsonArray buffers = root.value(QStringLiteral("buffers")).toArray();
    if (buffers.isEmpty()) {
        outError = QStringLiteral("glTF has no buffers");
        return false;
    }
    QJsonObject buf0 = buffers.first().toObject();
    const QString binUri = buf0.value(QStringLiteral("uri")).toString();
    if (binUri.isEmpty()) {
        outError = QStringLiteral("buffer 0 has no URI (embedded base64 "
                                  "not supported by this rewrite)");
        return false;
    }
    QFileInfo gi(gltfPath);
    const QString binPath = gi.absoluteDir().filePath(binUri);
    QFile bf(binPath);
    if (!bf.open(QIODevice::ReadWrite)) {
        outError = QStringLiteral("cannot open .bin for append: %1").arg(binPath);
        return false;
    }
    // Append UV2 data at the end. 2× float32 per vertex.
    const qint64 origBinSize = bf.size();
    bf.seek(origBinSize);

    QJsonArray accessors   = root.value(QStringLiteral("accessors")).toArray();
    QJsonArray bufferViews = root.value(QStringLiteral("bufferViews")).toArray();
    QJsonArray meshes      = root.value(QStringLiteral("meshes")).toArray();
    if (meshes.isEmpty()) {
        outError = QStringLiteral("glTF has no meshes");
        bf.close();
        return false;
    }
    QJsonObject mesh0 = meshes.first().toObject();
    QJsonArray prims  = mesh0.value(QStringLiteral("primitives")).toArray();
    if (static_cast<size_t>(prims.size()) + 1 != submeshStarts.size()) {
        outError = QStringLiteral("primitive count (%1) doesn't match "
                                  "submesh starts (%2)")
            .arg(prims.size()).arg(submeshStarts.size() - 1);
        bf.close();
        return false;
    }

    // Invert the permutation: gltfIndexToOgre[j] = i s.t. perm[i] == j.
    // The perm is per-primitive (each primitive's vertices live in
    // submeshStarts[si]..submeshStarts[si+1]); we walk each range.
    const QString texCoordKey = QStringLiteral("TEXCOORD_%1").arg(channel);

    qint64 cursorBytes = origBinSize;

    for (int si = 0; si < prims.size(); ++si) {
        const size_t a = submeshStarts[si];
        const size_t b = submeshStarts[si + 1];
        const size_t count = b - a;

        // Build the UV2 payload for this primitive.
        QByteArray payload;
        payload.resize(static_cast<int>(count * 2 * sizeof(float)));
        auto* fout = reinterpret_cast<float*>(payload.data());
        // Min/max for the accessor (glTF spec requires them for POSITION
        // but they're optional for other accessors — we emit them anyway
        // so importers that validate ranges don't complain).
        float uMin = std::numeric_limits<float>::infinity();
        float uMax = -std::numeric_limits<float>::infinity();
        float vMin = std::numeric_limits<float>::infinity();
        float vMax = -std::numeric_limits<float>::infinity();
        for (size_t i = a; i < b; ++i) {
            // i = Ogre vertex index = bake column index (the bake walks
            // Ogre's vertex buffer in order, so column == Ogre index).
            // gltfIdx = glTF buffer position of the SAME vertex after
            // Assimp's JoinIdenticalVertices reorder.
            const uint32_t gltfIdx = permutation[i];
            const uint32_t ogreIdx = static_cast<uint32_t>(i);
            // The bake column we want this glTF vertex to read is the
            // ORIGINAL Ogre column `i`, not `gltfIdx`. Earlier code
            // mistakenly used `gltfIdx`, which on meshes with non-
            // identity Assimp permutations (e.g. Mixamo Hip Hop Dancing)
            // meant every vertex read animation data from a
            // *different* vertex's column — body parts visibly flew
            // apart on specific frames where the wrong-source motion
            // was large.
            const uint32_t col = ogreIdx % static_cast<uint32_t>(texWidth);
            const uint32_t row = ogreIdx / static_cast<uint32_t>(texWidth);
            // The destination is glTF index gltfIdx, which is offset
            // (gltfIdx - a) within this primitive.
            const size_t localDst = static_cast<size_t>(gltfIdx) - a;
            const float u = static_cast<float>(col);
            const float v = static_cast<float>(row);
            fout[localDst * 2 + 0] = u;
            fout[localDst * 2 + 1] = v;
            if (u < uMin) uMin = u;
            if (u > uMax) uMax = u;
            if (v < vMin) vMin = v;
            if (v > vMax) vMax = v;
        }
        // Append payload to the .bin and add a bufferView + accessor.
        // Check the write result: a short write (disk full, FS quota,
        // I/O error mid-stream) would otherwise leave the .bin
        // truncated while the JSON's bufferView still claimed full
        // length, producing a glTF that references data that was
        // never fully written. On failure, truncate back to the
        // original size so the file is left exactly as we found it.
        const qint64 written = bf.write(payload);
        if (written != static_cast<qint64>(payload.size())) {
            const QString errStr = bf.errorString();
            bf.resize(origBinSize);
            bf.close();
            outError = QStringLiteral(
                "short write appending UV2 payload to %1 "
                "(wrote %2 of %3 bytes): %4")
                .arg(binPath).arg(written).arg(payload.size()).arg(errStr);
            return false;
        }
        const qint64 bvOffset = cursorBytes;
        cursorBytes += payload.size();

        QJsonObject bv;
        bv["buffer"]     = 0;
        bv["byteOffset"] = static_cast<double>(bvOffset);
        bv["byteLength"] = static_cast<double>(payload.size());
        bv["byteStride"] = static_cast<double>(2 * sizeof(float));
        bv["target"]     = 34962; // ARRAY_BUFFER
        const int bvIndex = bufferViews.size();
        bufferViews.append(bv);

        QJsonObject acc;
        acc["bufferView"]    = bvIndex;
        acc["byteOffset"]    = 0;
        acc["componentType"] = 5126; // FLOAT
        acc["count"]         = static_cast<int>(count);
        acc["type"]          = QStringLiteral("VEC2");
        QJsonArray jMin {uMin, vMin};
        QJsonArray jMax {uMax, vMax};
        acc["min"]           = jMin;
        acc["max"]           = jMax;
        const int accIndex = accessors.size();
        accessors.append(acc);

        // Wire the accessor into the primitive's attributes.
        QJsonObject prim = prims.at(si).toObject();
        QJsonObject attrs = prim.value(QStringLiteral("attributes")).toObject();
        attrs[texCoordKey] = accIndex;
        prim["attributes"] = attrs;
        prims.replace(si, prim);
    }

    bf.close();

    // Update buffer 0's byteLength to reflect the appended payload.
    buf0["byteLength"] = static_cast<double>(cursorBytes);
    buffers.replace(0, buf0);

    root["buffers"]     = buffers;
    root["bufferViews"] = bufferViews;
    root["accessors"]   = accessors;

    mesh0["primitives"] = prims;
    meshes.replace(0, mesh0);
    root["meshes"]      = meshes;

    QJsonDocument out(root);
    QFile gw(gltfPath);
    if (!gw.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        outError = QStringLiteral("cannot rewrite glTF: %1").arg(gltfPath);
        return false;
    }
    gw.write(out.toJson(QJsonDocument::Indented));
    gw.close();
    return true;
}

} // namespace

int CLIPipeline::cmdVat(int argc, char* argv[])
{
    // Parse: vat <file> --anim <name> [--fps N] [-o <dir>] [--include-shaders {godot,unity,unreal,all}] [--emit-uv2 [N]] [--bake-precision {16,32}] [--json]
    //
    // Output is always OpenVAT (sharpen3d/openvat) — a single packed
    // 16-bit RGB PNG (`<basename>_pos.png`, height = 2*frames, top half
    // positions, bottom half normals) plus `<basename>-remap_info.json`
    // with the canonical `os-remap` schema. Consumed unmodified by the
    // openvat reference shaders for Godot / Unity / Unreal / Blender.
    QString filePath, animName, outDir;
    double fps = 30.0;
    bool jsonOutput = false;
    QString includeShadersArg;
    // --emit-uv2 <channel>: inject the per-vertex bake-column index
    // as TEXCOORD_<channel> into source.gltf. -1 (default) = off.
    // The bake itself doesn't care about UV2 — this exists so
    // engine consumers (Godot/Unity/Unreal) can skip the runtime
    // bind-sidecar matching and just read TEXCOORD_<channel> as
    // (column % tex_width, column / tex_width) directly.
    int emitUv2Channel = -1;
    // --bake-precision {16,32}: per-channel bit depth for the position
    // texture. Default 16 (PNG, ~0.03mm precision over 2m bounds).
    // 32 writes an EXR with raw float32 positions — sidesteps the
    // sub-mm quantization artifact that flickers Mixamo's coplanar
    // eye-sphere/head-plug geometry. ~2× larger file.
    int bakeBitDepth = 16;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "vat" || arg == "--cli") continue;
        if (arg == "--json") { jsonOutput = true; continue; }
        if ((arg == "--anim" || arg == "--animation") && i + 1 < argc) {
            animName = QString(argv[++i]); continue;
        }
        if (arg == "--fps" && i + 1 < argc) {
            bool ok = false;
            const double v = QString(argv[++i]).toDouble(&ok);
            if (!ok || v <= 0.0) {
                err() << "Error: --fps must be a positive number" << Qt::endl;
                return 2;
            }
            fps = v; continue;
        }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outDir = QString(argv[++i]); continue;
        }
        if (arg == "--bake-precision" && i + 1 < argc) {
            bool ok = false;
            const int v = QString(argv[++i]).toInt(&ok);
            if (!ok || (v != 16 && v != 32)) {
                err() << "Error: --bake-precision must be 16 or 32 "
                         "(got \"" << argv[i] << "\")." << Qt::endl;
                return 2;
            }
            bakeBitDepth = v;
            continue;
        }
        // --include-shaders <list>: comma-separated subset of
        // {godot, unity, unreal} (or "all") — copies the matching
        // drop-in shader templates into the bake's output directory
        // so the consumer has the bake AND the engine glue together.
        if (arg == "--include-shaders") {
            if (i + 1 >= argc) {
                err() << "Error: --include-shaders requires a value "
                         "(godot, unity, unreal, all — comma-separated)."
                      << Qt::endl;
                return 2;
            }
            includeShadersArg = QString(argv[++i]);
            continue;
        }
        if (arg == "--emit-uv2") {
            // Accept either `--emit-uv2` (defaults to channel 1, the
            // OpenVAT convention) or `--emit-uv2 <N>` for a specific
            // channel index. Channel 0 is normally the mesh's
            // diffuse UV so we reject it to avoid clobbering.
            int channel = 1;
            if (i + 1 < argc) {
                // Only consume the next token as a channel if it
                // *looks* like an integer (digits-only or +/- digits).
                // Otherwise it's likely the positional file argument
                // and we leave it alone — the user wanted the bare
                // form `--emit-uv2`.
                const QString peek = QString(argv[i + 1]);
                bool looksNumeric = !peek.isEmpty();
                int start = (peek[0] == '-' || peek[0] == '+') ? 1 : 0;
                if (start >= peek.size()) looksNumeric = false;
                for (int k = start; looksNumeric && k < peek.size(); ++k)
                    if (!peek[k].isDigit()) looksNumeric = false;
                if (looksNumeric) {
                    bool ok = false;
                    const int n = peek.toInt(&ok);
                    if (!ok) {
                        err() << "Error: --emit-uv2 value \"" << peek
                              << "\" is not an integer." << Qt::endl;
                        return 2;
                    }
                    // Consume the token first so we don't fall through
                    // to the positional-arg branch on error.
                    ++i;
                    if (n < 0 || n > 7) {
                        err() << "Error: --emit-uv2 channel " << n
                              << " is out of range (must be 1..7; "
                                 "channel 0 would overwrite the diffuse UV)."
                              << Qt::endl;
                        return 2;
                    }
                    channel = n;
                }
            }
            if (channel == 0) {
                err() << "Error: --emit-uv2 0 would overwrite the "
                         "diffuse UV; use channel 1 or higher."
                      << Qt::endl;
                return 2;
            }
            emitUv2Channel = channel;
            continue;
        }
        if (!arg.startsWith("-") && filePath.isEmpty()) {
            filePath = arg; continue;
        }
    }

    if (filePath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh vat <file> --anim <name> [--fps N] [-o <dir>] [--include-shaders {godot,unity,unreal,all}] [--emit-uv2 [N]] [--bake-precision {16,32}] [--json]" << Qt::endl;
        return 2;
    }
    if (animName.isEmpty()) {
        err() << "Error: --anim <name> is required." << Qt::endl;
        return 2;
    }
    if (outDir.isEmpty()) {
        QFileInfo fi(filePath);
        outDir = fi.absoluteDir().filePath(fi.completeBaseName() + "_vat");
    }

    QFileInfo fi(filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << filePath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.vat",
        QString("VAT bake .%1 anim=%2 fps=%3").arg(fi.suffix(), animName).arg(fps));
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing %1").arg(fi.absoluteFilePath()));

    MeshImporterExporter::importer({fi.absoluteFilePath()});

    auto& entities = Manager::getSingleton()->getEntities();
    Ogre::Entity* entity = nullptr;
    for (auto* obj : entities) {
        if (obj && obj->getMovableType() == "Entity") {
            entity = static_cast<Ogre::Entity*>(obj);
            break;
        }
    }
    if (!entity) {
        SentryReporter::captureMessage(
            QString("CLI vat: import failed (.%1)").arg(fi.suffix()), "error");
        err() << "Error: Failed to load file: " << filePath << Qt::endl;
        return 1;
    }
    if (!entity->hasSkeleton()) {
        err() << "Error: File has no skeleton — cannot bake VAT." << Qt::endl;
        return 1;
    }

    // Export the source mesh as glTF BEFORE running the bake. Both
    // the bake's vertex walk and the glTF exporter iterate Ogre
    // submeshes in submesh-index order, so writing them from the same
    // entity guarantees the bake's column index `i` corresponds to
    // glTF vertex `i`. Doing this AFTER the bake produced a malformed
    // glTF (the bake's software-skinning request leaves Ogre's
    // animation state in a half-state that the exporter mishandles —
    // we get a meshes dict instead of an array). Doing it BEFORE
    // sidesteps the issue entirely and gives consumers a mesh that
    // matches the bake bit-for-bit on vertex order.
    // VATBaker::bake creates `outDir` itself; we need it now too.
    QDir().mkpath(outDir);
    QString gltfPath = QFileInfo(QDir(outDir).filePath("source.gltf")).absoluteFilePath();
    // Use the display-name format string (matches `cmdConvert`).
    // Short forms like "gltf2" route to a different exporter path
    // that produces a malformed meshes-as-dict glTF.
    int exportResult = MeshImporterExporter::exporter(
        entity->getParentSceneNode(), gltfPath, formatForExtension(gltfPath));

    // Assimp's gltf2 exporter hardcodes `aiProcess_JoinIdenticalVertices`
    // (assimp/code/Common/Exporter.cpp), which permutes per-primitive
    // vertex order even when no duplicates actually get merged. Without
    // a remap the bake's column-index → vertex-index relationship is
    // broken and the consumer renders shattered triangles.
    //
    // Strategy: after the glTF is on disk, read Ogre's bind-pose
    // positions (the same data Assimp wrote, pre-permutation) and the
    // post-export glTF positions; build a `perm[ogre_i] = gltf_j`
    // mapping by quantized-position match per submesh; pass it to the
    // bake so each row's columns land in the glTF's vertex order.
    // Tracks whether source.gltf's vertex order is guaranteed to match the
    // bake's column order. Only set true after a successful permutation
    // match — if alignment fails for ANY reason (export failure, read-back
    // failure, count mismatch, ambiguous position match), the final report
    // must NOT advertise "vertex order matches the bake" because consumers
    // would render shattered triangles when they trust that label.
    bool sourceMeshMatchesBake = false;
    std::vector<uint32_t> vertexPerm;

    // Emit a per-vertex bind-pose sidecar so consumers can re-bind the
    // bake's column order to whatever vertex order their engine loads
    // the mesh in (Godot reorders on import via the resource pipeline;
    // Unity's import does the same; Unreal likewise). Layout: a flat
    // float32 array of `vertexCount * 3` values, in Ogre's vertex-
    // buffer walk order — i.e. matching the bake's column index 1:1.
    // Consumers iterate their imported mesh's vertices and match each
    // bind position back to a column index here. Tiny (5828 × 12 B ≈
    // 70 KB on the Rumba dancer), and worth its weight by being the
    // *only* path that survives every engine importer's reordering.
    // Sidecar layout (little-endian, packed):
    //   uint32  magic         = 0x42565442 ("BTVB" — Bake-To-Vertex Bind)
    //   uint32  version       = 1
    //   uint32  vertexCount
    //   uint32  flags         (bit 0 = positions, bit 1 = normals, bit 2 = uv)
    //   then per-vertex: 3 floats position, 3 floats normal (if flag set),
    //                    2 floats uv0 (if flag set)
    //
    // The consumer matches each loaded mesh vertex against this stream's
    // full signature (pos+normal+uv) to find its bake column index.
    // Position-only matching is ambiguous on UV seams / hard edges /
    // weight splits, which Mixamo characters carry by the hundreds —
    // 17% of verts on Rumba Dancing share a quantized bind position
    // with at least one other vert.
    auto writeOgreBindSidecar = [&](Ogre::Entity* e, const QString& path) {
        std::vector<BakeVertex> verts = readOgreBindVertices(e);
        if (verts.empty()) return false;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        const uint32_t magic   = 0x42565442;
        const uint32_t version = 1;
        const uint32_t count   = static_cast<uint32_t>(verts.size());
        uint32_t flags = 0x1;
        bool hasN = !verts.empty() && verts[0].hasNormal;
        bool hasU = !verts.empty() && verts[0].hasUV;
        if (hasN) flags |= 0x2;
        if (hasU) flags |= 0x4;
        f.write(reinterpret_cast<const char*>(&magic),   sizeof(magic));
        f.write(reinterpret_cast<const char*>(&version), sizeof(version));
        f.write(reinterpret_cast<const char*>(&count),   sizeof(count));
        f.write(reinterpret_cast<const char*>(&flags),   sizeof(flags));
        for (const auto& v : verts) {
            float xyz[3] = { v.position.x, v.position.y, v.position.z };
            f.write(reinterpret_cast<const char*>(xyz), sizeof(xyz));
            if (hasN) {
                float n[3] = { v.normal.x, v.normal.y, v.normal.z };
                f.write(reinterpret_cast<const char*>(n), sizeof(n));
            }
            if (hasU) {
                float uv[2] = { v.uv0u, v.uv0v };
                f.write(reinterpret_cast<const char*>(uv), sizeof(uv));
            }
        }
        f.close();
        return true;
    };
    const QString bindPath = QDir(outDir).filePath(animName + "_ogre_bind.bin");
    bool bindWritten = writeOgreBindSidecar(entity, bindPath);
    if (!bindWritten) {
        err() << "Warning: failed to write Ogre bind-pose sidecar to "
              << bindPath << " — consumers may not be able to align the "
                             "bake with their imported mesh." << Qt::endl;
    }
    // Kept in scope past the bake so the `--emit-uv2` post-pass can
    // inject TEXCOORD_<channel> into the exported glTF.
    std::vector<size_t> submeshStarts;
    if (exportResult == 0) {
        std::vector<BakeVertex> ogreVerts = readOgreBindVertices(entity);
        std::vector<BakeVertex> gltfVerts;
        if (!readGltfVertices(gltfPath, gltfVerts)) {
            err() << "Warning: failed to read back source.gltf for VAT "
                     "alignment — bake will use Ogre vertex-buffer order; "
                     "the emitted mesh is NOT marked as matching the bake."
                  << Qt::endl;
        } else if (ogreVerts.size() != gltfVerts.size()) {
            err() << "Warning: source.gltf vertex count (" << gltfVerts.size()
                  << ") differs from Ogre bake count (" << ogreVerts.size()
                  << ") — bake will use Ogre vertex-buffer order; the "
                     "emitted mesh is NOT marked as matching the bake."
                  << Qt::endl;
        } else {
            // Build per-submesh start offsets — both Ogre walk and glTF
            // primitive walk iterate submeshes in the same order, with
            // identical per-submesh counts (Assimp can permute within
            // a primitive but cannot move vertices across submeshes).
            submeshStarts.push_back(0);
            Ogre::MeshPtr mesh = entity->getMesh();
            bool sharedAppended = false;
            for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
                Ogre::SubMesh* sub = mesh->getSubMesh(si);
                if (!sub) continue;
                if (sub->useSharedVertices && sharedAppended) continue;
                const Ogre::VertexData* vData = sub->useSharedVertices
                    ? mesh->sharedVertexData
                    : sub->vertexData;
                if (!vData) continue;
                submeshStarts.push_back(submeshStarts.back() + vData->vertexCount);
                if (sub->useSharedVertices) sharedAppended = true;
            }
            vertexPerm = buildVertexPermutation(ogreVerts, gltfVerts, submeshStarts);
            if (vertexPerm.empty()) {
                err() << "Warning: failed to align bake columns with glTF "
                         "vertex order — the bake will use Ogre vertex-"
                         "buffer order, and the emitted mesh is NOT marked "
                         "as matching the bake." << Qt::endl;
                submeshStarts.clear();
            } else {
                sourceMeshMatchesBake = true;
            }
        }
    }

    VATBaker::Options opts;
    opts.animationName     = animName;
    opts.fps               = fps;
    opts.outputDir         = outDir;
    opts.basename          = animName;
    opts.bitDepth          = bakeBitDepth;
    // Keep a copy for the UV2 post-pass (the move below sinks the
    // original into VATBaker::Options).
    std::vector<uint32_t> vertexPermCopy = vertexPerm;
    opts.vertexPermutation = std::move(vertexPerm);

    SentryReporter::addBreadcrumb("file.export",
        QString("Writing OpenVAT bake to %1 (anim=%2)")
            .arg(QDir(outDir).absolutePath(), animName));
    VATBaker::BakeResult result = VATBaker::bake(entity, opts);
    if (!result.ok) {
        SentryReporter::captureMessage(
            QString("CLI vat: bake failed (%1)").arg(result.error), "error");
        err() << "Error: VAT bake failed: " << result.error << Qt::endl;
        return 1;
    }

    if (exportResult != 0) {
        // Non-fatal — a consumer can still drive the bake against a
        // separately-converted mesh if they accept the risk. We do
        // log a warning so users notice when it failed.
        SentryReporter::captureMessage(
            QStringLiteral("CLI vat: source.gltf export failed (rc=%1)").arg(exportResult),
            "warning");
        err() << "Warning: source.gltf export failed (rc=" << exportResult
              << "). Bake is still valid; you'll need to provide a "
                 "vertex-order-matching mesh separately." << Qt::endl;
        gltfPath.clear();  // Mark as missing so the post-run report skips it.
    }

    // --emit-uv2: inject the per-vertex column index as
    // TEXCOORD_<channel> into source.gltf so engine consumers can
    // drop the runtime bind-sidecar matcher. Only emitted when:
    //   1. The user asked for it (channel >= 0).
    //   2. The glTF export succeeded.
    //   3. The permutation step actually built a valid mapping —
    //      without it we can't tell which Ogre column each glTF
    //      vertex corresponds to.
    bool uv2Emitted = false;
    if (emitUv2Channel >= 0 && exportResult == 0
        && sourceMeshMatchesBake && !vertexPermCopy.empty()) {
        QString uv2Err;
        uv2Emitted = emitGltfUv2(gltfPath, vertexPermCopy, submeshStarts,
                                 result.vertexCount, emitUv2Channel,
                                 uv2Err);
        if (!uv2Emitted) {
            err() << "Warning: --emit-uv2 failed: " << uv2Err
                  << " — consumers will need the bind-sidecar matcher path."
                  << Qt::endl;
            SentryReporter::addBreadcrumb("file.export",
                QStringLiteral("VAT --emit-uv2 failed: %1").arg(uv2Err));
        } else {
            SentryReporter::addBreadcrumb("file.export",
                QStringLiteral("VAT TEXCOORD_%1 injected into %2")
                    .arg(emitUv2Channel).arg(gltfPath));
        }
    } else if (emitUv2Channel >= 0) {
        err() << "Warning: --emit-uv2 skipped — needs successful glTF "
                 "export AND alignment. Re-run without the warnings above."
              << Qt::endl;
    }

    // --include-shaders: drop the requested engine templates next to
    // the bake so a consumer has everything in one folder.
    QStringList shadersWritten;
    if (!includeShadersArg.isEmpty()) {
        QStringList rejectedTokens;
        const QStringList engines = VATShaderEmitter::parseEngineList(
            includeShadersArg, &rejectedTokens);
        // Surface invalid tokens even when SOME of the list was
        // recognised — otherwise `--include-shaders godot,blender`
        // silently drops "blender" and the user discovers the
        // missing template only during engine integration.
        if (!rejectedTokens.isEmpty()) {
            err() << "Warning: --include-shaders ignored unknown engine"
                  << (rejectedTokens.size() == 1 ? " " : "s ")
                  << "\"" << rejectedTokens.join("\", \"")
                  << "\" (accepted: godot, unity, unreal, all)." << Qt::endl;
            SentryReporter::addBreadcrumb("file.export",
                QStringLiteral("VAT shaders: rejected unknown engine(s): %1")
                    .arg(rejectedTokens.join(QStringLiteral(", "))));
        }
        if (engines.isEmpty()) {
            err() << "Warning: --include-shaders=\"" << includeShadersArg
                  << "\" did not match any known engine "
                     "(accepted: godot, unity, unreal, all)." << Qt::endl;
            SentryReporter::addBreadcrumb("file.export",
                QStringLiteral("VAT shaders: no valid engines parsed from '%1'")
                    .arg(includeShadersArg));
        } else {
            shadersWritten = VATShaderEmitter::writeShaders(outDir, engines);
            if (shadersWritten.isEmpty()) {
                err() << "Warning: --include-shaders requested "
                      << engines.join(",") << " but no files could be written."
                      << Qt::endl;
                SentryReporter::addBreadcrumb("file.export",
                    QStringLiteral("VAT shaders: write failed for engines: %1")
                        .arg(engines.join(QStringLiteral(", "))));
            } else {
                SentryReporter::addBreadcrumb("file.export",
                    QStringLiteral("VAT shaders written: %1")
                        .arg(shadersWritten.join(QStringLiteral(", "))));
            }
        }
    }

    if (jsonOutput) {
        QJsonObject obj;
        obj["ok"]          = true;
        obj["texture"]     = result.posTexPath;
        obj["sidecar"]     = result.jsonPath;
        if (sourceMeshMatchesBake)
            obj["sourceMesh"] = gltfPath;
        if (bindWritten)
            obj["bindSidecar"] = bindPath;
        if (!shadersWritten.isEmpty()) {
            QJsonArray sarr;
            for (const auto& p : shadersWritten) sarr.append(p);
            obj["shaders"] = sarr;
        }
        if (uv2Emitted)
            obj["uv2Channel"] = emitUv2Channel;
        obj["frameCount"]  = result.frameCount;
        obj["vertexCount"] = result.vertexCount;
        obj["animation"]   = animName;
        obj["fps"]         = fps;
        QJsonObject bounds;
        QJsonObject lo, hi;
        lo["x"] = result.minBound.x; lo["y"] = result.minBound.y; lo["z"] = result.minBound.z;
        hi["x"] = result.maxBound.x; hi["y"] = result.maxBound.y; hi["z"] = result.maxBound.z;
        bounds["min"] = lo; bounds["max"] = hi;
        obj["bounds"] = bounds;
        cliWrite(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented)));
    } else {
        cliWrite(QStringLiteral("Baked OpenVAT for '%1' (%2 frames × %3 vertices)\n")
                     .arg(animName).arg(result.frameCount).arg(result.vertexCount));
        cliWrite(QStringLiteral("  texture:  %1\n").arg(result.posTexPath));
        cliWrite(QStringLiteral("  sidecar:  %1\n").arg(result.jsonPath));
        if (bindWritten)
            cliWrite(QStringLiteral("  bind:     %1 (per-vertex bind-pose signature; "
                                    "consumers use this to align UV2 to the bake's "
                                    "column order regardless of importer reordering)\n")
                .arg(bindPath));
        if (sourceMeshMatchesBake)
            cliWrite(QStringLiteral("  mesh:     %1 (vertex order matches the bake)\n").arg(gltfPath));
        if (uv2Emitted)
            cliWrite(QStringLiteral("  uv2:      injected as TEXCOORD_%1 — consumers can "
                                    "drop the runtime bind-sidecar matcher and read "
                                    "(col, row) from the mesh's UV%1 directly\n")
                .arg(emitUv2Channel));
        cliWrite(QStringLiteral("  bounds:   min=(%1, %2, %3) max=(%4, %5, %6)\n")
                     .arg(result.minBound.x, 0, 'f', 3)
                     .arg(result.minBound.y, 0, 'f', 3)
                     .arg(result.minBound.z, 0, 'f', 3)
                     .arg(result.maxBound.x, 0, 'f', 3)
                     .arg(result.maxBound.y, 0, 'f', 3)
                     .arg(result.maxBound.z, 0, 'f', 3));
        if (!shadersWritten.isEmpty()) {
            for (const QString& path : shadersWritten)
                cliWrite(QStringLiteral("  shader:   %1\n").arg(path));
        } else {
            cliWrite(QStringLiteral(
                "  shaders:  drop-in Godot/Unity/Unreal templates at tools/vat-shaders/\n"
                "            (re-run with `--include-shaders all` to copy them next to the bake)\n"));
        }
    }
    return 0;
}

int CLIPipeline::cmdUv(int argc, char* argv[])
{
    // Parse:
    //   uv <file> --info [--json] [--channel N]
    //   uv <file> --unwrap [--resolution N] [--padding P] [--channel C] [--no-backup] -o out
    //   uv <file> --project box|cylinder|sphere|reset [--axis N] [--scale S] [--channel C] -o out [--json]
    //   uv <file> --set-seams "0:1-2,0:2-3" -o out
    QString inputPath, outputPath, projectMode, seamSpec;
    bool unwrap = false, infoMode = false, projectModeFlag = false, setSeams = false;
    bool jsonOutput = false;
    int resolution = 1024, padding = 4, channel = 0;
    int axis = 1;
    double scale = 1.0;
    bool preserveBackup = true;

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "uv" || arg == "--cli") continue;
        if (arg == "--unwrap") { unwrap = true; continue; }
        if (arg == "--info")   { infoMode = true; continue; }
        if (arg == "--project" && i + 1 < argc) {
            projectModeFlag = true;
            projectMode = QString::fromLocal8Bit(argv[++i]);
            continue;
        }
        if (arg == "--set-seams" && i + 1 < argc) {
            setSeams = true;
            seamSpec = QString::fromLocal8Bit(argv[++i]);
            continue;
        }
        if (arg == "--json")   { jsonOutput = true; continue; }
        if (arg == "--no-backup") { preserveBackup = false; continue; }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString::fromLocal8Bit(argv[++i]); continue;
        }
        if (arg == "--resolution" && i + 1 < argc) {
            resolution = QString::fromLocal8Bit(argv[++i]).toInt(); continue;
        }
        if (arg == "--padding" && i + 1 < argc) {
            padding = QString::fromLocal8Bit(argv[++i]).toInt(); continue;
        }
        if (arg == "--channel" && i + 1 < argc) {
            channel = QString::fromLocal8Bit(argv[++i]).toInt(); continue;
        }
        if (arg == "--axis" && i + 1 < argc) {
            axis = QString::fromLocal8Bit(argv[++i]).toInt(); continue;
        }
        if (arg == "--scale" && i + 1 < argc) {
            scale = QString::fromLocal8Bit(argv[++i]).toDouble(); continue;
        }
        if (!arg.startsWith("-") && inputPath.isEmpty()) {
            inputPath = arg; continue;
        }
    }

    const int modeCount = static_cast<int>(unwrap) + static_cast<int>(infoMode)
                          + static_cast<int>(projectModeFlag) + static_cast<int>(setSeams);

    if (inputPath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh uv <file> --info [--json]" << Qt::endl;
        err() << "       qtmesh uv <file> --unwrap [--resolution N] [--padding P] [--channel C] -o <out>" << Qt::endl;
        err() << "       qtmesh uv <file> --project box|cylinder|sphere|reset [--axis N] [--scale S] -o <out>" << Qt::endl;
        err() << "       qtmesh uv <file> --set-seams \"sub:v0-v1,...\" -o <out>" << Qt::endl;
        return 2;
    }
    if (modeCount == 0) {
        err() << "Error: specify --info, --unwrap, --project, or --set-seams." << Qt::endl;
        return 2;
    }
    if (modeCount > 1) {
        err() << "Error: choose one of --info, --unwrap, --project, or --set-seams." << Qt::endl;
        return 2;
    }
    if ((unwrap || projectModeFlag || setSeams) && outputPath.isEmpty()) {
        err() << "Error: this mode requires -o <output>." << Qt::endl;
        return 2;
    }

    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: file not found: " << inputPath << Qt::endl; return 1;
    }
    if (!initOgreHeadless()) return 1;

    const QString modeLabel = unwrap ? QStringLiteral("unwrap")
                                    : infoMode ? QStringLiteral("info")
                                               : projectModeFlag ? QStringLiteral("project")
                                                                 : QStringLiteral("set_seams");
    SentryReporter::addBreadcrumb(QStringLiteral("cli.uv"),
        QString("uv .%1 mode=%2").arg(fi.suffix(), modeLabel));

    MeshImporterExporter::importer({fi.absoluteFilePath()});
    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        err() << "Error: failed to load " << inputPath << Qt::endl; return 1;
    }
    Ogre::Entity* entity = entities.first();

    if (infoMode) {
        const auto info = UvPipeline::analyzeEntity(entity, channel);
        if (jsonOutput) {
            cliWrite(QString::fromUtf8(
                QJsonDocument(UvPipeline::infoToJson(fi.fileName(), info))
                    .toJson(QJsonDocument::Indented)) + "\n");
        } else {
            cliWrite(UvPipeline::infoToText(fi.fileName(), info));
        }
        return 0;
    }

    if (projectModeFlag) {
        bool ok = false;
        const UvProject::Mode mode = UvPipeline::parseProjectMode(projectMode, &ok);
        if (!ok) {
            err() << "Error: unknown projection mode '" << projectMode
                  << "'. Use box, cylinder, sphere, or reset." << Qt::endl;
            return 2;
        }

        UvProject::Options opts;
        opts.axis = axis;
        opts.boxScale = static_cast<float>(scale > 0.0 ? scale : 1.0);

        SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.project"),
            UvProject::modeToString(mode));

        const auto report = UvPipeline::projectEntity(entity, mode, channel, opts);
        if (!report.applied) {
            err() << "Error: UV projection failed — " << report.error << Qt::endl;
            return 1;
        }

        auto* node = entity->getParentSceneNode();
        const QString fmt = formatForExtension(outputPath);
        if (MeshImporterExporter::exporter(node, QFileInfo(outputPath).absoluteFilePath(), fmt) != 0) {
            err() << "Error: export failed." << Qt::endl;
            return 1;
        }

        if (jsonOutput) {
            QJsonObject obj;
            obj[QStringLiteral("applied")] = true;
            obj[QStringLiteral("mode")] = UvProject::modeToString(mode);
            obj[QStringLiteral("vertsChanged")] = report.vertsChanged;
            obj[QStringLiteral("output")] = outputPath;
            cliWrite(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented)) + "\n");
        } else {
            cliWrite(QStringLiteral("Projected %1 vertices (%2)\nWrote: %3\n")
                         .arg(report.vertsChanged)
                         .arg(UvProject::modeToString(mode))
                         .arg(QFileInfo(outputPath).fileName()));
        }
        return 0;
    }

    if (setSeams) {
        std::vector<UvPipeline::SeamEdge> edges;
        QString parseError;
        if (!UvPipeline::parseSeamEdgeList(seamSpec, edges, &parseError)) {
            err() << "Error: invalid --set-seams list — " << parseError << Qt::endl;
            return 2;
        }

        SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.seam"),
            QStringLiteral("set_seams count=%1").arg(edges.size()));

        QString seamError;
        if (!UvPipeline::setSeamsOnEntity(entity, edges, &seamError)) {
            err() << "Error: set-seams failed — " << seamError << Qt::endl;
            return 1;
        }

        auto* node = entity->getParentSceneNode();
        const QString fmt = formatForExtension(outputPath);
        if (MeshImporterExporter::exporter(node, QFileInfo(outputPath).absoluteFilePath(), fmt) != 0) {
            err() << "Error: export failed." << Qt::endl;
            return 1;
        }

        if (jsonOutput) {
            QJsonObject obj;
            obj[QStringLiteral("applied")] = true;
            obj[QStringLiteral("seamCount")] = static_cast<int>(edges.size());
            obj[QStringLiteral("output")] = outputPath;
            cliWrite(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented)) + "\n");
        } else {
            cliWrite(QStringLiteral("Set %1 seam edges\nWrote: %2\n")
                         .arg(edges.size())
                         .arg(QFileInfo(outputPath).fileName()));
        }
        return 0;
    }

    // --unwrap path
    UvUnwrapOptions opts;
    opts.resolution = std::max(64, resolution);
    opts.padding    = std::max(0, padding);
    opts.channel    = std::max(0, channel);
    opts.preserveOriginalAsBackup = preserveBackup;

    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.unwrap"),
        QString("Unwrap %1 (res=%2 pad=%3 ch=%4)")
            .arg(fi.fileName()).arg(opts.resolution).arg(opts.padding).arg(opts.channel));

    const auto report = UvPipeline::unwrapEntity(entity, opts);
    if (!report.applied) {
        err() << "Error: UV unwrap failed — " << report.error << Qt::endl;
        return 1;
    }

    auto* node = entity->getParentSceneNode();
    const QString fmt = formatForExtension(outputPath);
    if (MeshImporterExporter::exporter(node, QFileInfo(outputPath).absoluteFilePath(), fmt) != 0) {
        err() << "Error: export failed." << Qt::endl;
        return 1;
    }

    if (jsonOutput) {
        cliWrite(QString::fromUtf8(
            QJsonDocument(UvUnwrap::reportToJson(report)).toJson(QJsonDocument::Indented)) + "\n");
    } else {
        cliWrite(UvUnwrap::reportToText(report)
                 + QString("Wrote: %1\n").arg(QFileInfo(outputPath).fileName()));
    }
    return 0;
}

int CLIPipeline::cmdRetopo(int argc, char* argv[])
{
    // Parse: retopo <file> [--target-faces N] [--max-angle DEG]
    //        [--shape-tol DEG] [--max-aspect R] -o <out> [--json]
    QString inputPath, outputPath;
    bool jsonOutput = false;
    int targetFaces = -1;
    double maxAngleDeg = 25.0;
    double shapeToleranceDeg = 65.0;
    double maxAspectRatio = 6.0;

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "retopo" || arg == "--cli") continue;
        if (arg == "--json") { jsonOutput = true; continue; }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString::fromLocal8Bit(argv[++i]); continue;
        }
        if (arg == "--target-faces" && i + 1 < argc) {
            bool ok = false;
            const int v = QString::fromLocal8Bit(argv[++i]).toInt(&ok);
            if (!ok || v <= 0) {
                err() << "Error: --target-faces must be a positive integer." << Qt::endl;
                return 2;
            }
            targetFaces = v; continue;
        }
        if (arg == "--max-angle" && i + 1 < argc) {
            bool ok = false;
            const double v = QString::fromLocal8Bit(argv[++i]).toDouble(&ok);
            if (!ok || v < 0.0 || v > 180.0) {
                err() << "Error: --max-angle must be a number in [0, 180]." << Qt::endl;
                return 2;
            }
            maxAngleDeg = v; continue;
        }
        if (arg == "--shape-tol" && i + 1 < argc) {
            bool ok = false;
            const double v = QString::fromLocal8Bit(argv[++i]).toDouble(&ok);
            if (!ok || v < 0.0 || v > 90.0) {
                err() << "Error: --shape-tol must be a number in [0, 90]." << Qt::endl;
                return 2;
            }
            shapeToleranceDeg = v; continue;
        }
        if (arg == "--max-aspect" && i + 1 < argc) {
            bool ok = false;
            const double v = QString::fromLocal8Bit(argv[++i]).toDouble(&ok);
            if (!ok || v < 1.0) {
                err() << "Error: --max-aspect must be a number >= 1." << Qt::endl;
                return 2;
            }
            maxAspectRatio = v; continue;
        }
        if (!arg.startsWith("-") && inputPath.isEmpty()) {
            inputPath = arg; continue;
        }
    }

    if (inputPath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh retopo <file> [--target-faces N] "
                 "[--max-angle DEG] [--shape-tol DEG] [--max-aspect R] -o <out> [--json]"
              << Qt::endl;
        return 2;
    }
    if (outputPath.isEmpty()) {
        err() << "Error: -o <output> required." << Qt::endl;
        return 2;
    }

    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: file not found: " << inputPath << Qt::endl; return 1;
    }
    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.retopo"),
        QString("retopo .%1 target=%2 maxAngle=%3")
            .arg(fi.suffix()).arg(targetFaces).arg(maxAngleDeg));

    MeshImporterExporter::importer({fi.absoluteFilePath()});
    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        err() << "Error: failed to load " << inputPath << Qt::endl; return 1;
    }
    if (entities.size() > 1) {
        err() << "Error: " << inputPath
              << " contains multiple mesh entities. `qtmesh retopo` "
                 "currently supports one entity per file."
              << Qt::endl;
        return 1;
    }
    Ogre::Entity* entity = entities.first();

    QuadRetopoOptions opts;
    opts.targetFaces        = targetFaces;
    opts.maxAngleDeg        = maxAngleDeg;
    opts.shapeToleranceDeg  = shapeToleranceDeg;
    opts.maxAspectRatio     = maxAspectRatio;

    const auto report = QuadRetopo::retopologize(entity, opts);
    if (!report.applied) {
        err() << "Error: retopology failed — " << report.error << Qt::endl;
        return 1;
    }

    auto* node = entity->getParentSceneNode();
    const QString fmt = formatForExtension(outputPath);
    if (MeshImporterExporter::exporter(node, QFileInfo(outputPath).absoluteFilePath(), fmt) != 0) {
        err() << "Error: export failed." << Qt::endl;
        return 1;
    }

    if (jsonOutput) {
        cliWrite(QString::fromUtf8(
            QJsonDocument(QuadRetopo::reportToJson(report)).toJson(QJsonDocument::Indented)) + "\n");
    } else {
        cliWrite(QuadRetopo::reportToText(report)
                 + QString("Wrote: %1\n").arg(QFileInfo(outputPath).fileName()));
    }
    return 0;
}

int CLIPipeline::cmdSkin(int argc, char* argv[])
{
    // Parse: skin <file> [--max-influences N] [--falloff F]
    //        [--max-distance D] [--skip-unweighted] [--merge] -o <out> [--json]
    QString inputPath, outputPath;
    bool jsonOutput = false;
    int  maxInfluences = 4;
    double falloff = 4.0;
    double maxDistance = 0.5;
    bool skipUnweighted = false;
    bool replaceExisting = true;

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "skin" || arg == "--cli") continue;
        if (arg == "--json") { jsonOutput = true; continue; }
        if (arg == "--skip-unweighted") { skipUnweighted = true; continue; }
        if (arg == "--merge") { replaceExisting = false; continue; }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString::fromLocal8Bit(argv[++i]); continue;
        }
        if (arg == "--max-influences" && i + 1 < argc) {
            bool ok = false;
            const int v = QString::fromLocal8Bit(argv[++i]).toInt(&ok);
            if (!ok || v < 1 || v > 8) {
                err() << "Error: --max-influences must be in [1, 8]." << Qt::endl;
                return 2;
            }
            maxInfluences = v; continue;
        }
        if (arg == "--falloff" && i + 1 < argc) {
            bool ok = false;
            const double v = QString::fromLocal8Bit(argv[++i]).toDouble(&ok);
            if (!ok || v < 0.5 || v > 16.0) {
                err() << "Error: --falloff must be in [0.5, 16]." << Qt::endl;
                return 2;
            }
            falloff = v; continue;
        }
        if (arg == "--max-distance" && i + 1 < argc) {
            bool ok = false;
            const double v = QString::fromLocal8Bit(argv[++i]).toDouble(&ok);
            if (!ok || v < 0.0 || v > 10.0) {
                err() << "Error: --max-distance must be in [0, 10]." << Qt::endl;
                return 2;
            }
            maxDistance = v; continue;
        }
        if (!arg.startsWith("-") && inputPath.isEmpty()) {
            inputPath = arg; continue;
        }
    }

    if (inputPath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh skin <file> [--max-influences N] [--falloff F] "
                 "[--max-distance D] [--skip-unweighted] [--merge] -o <out> [--json]"
              << Qt::endl;
        return 2;
    }
    if (outputPath.isEmpty()) {
        err() << "Error: -o <output> required." << Qt::endl;
        return 2;
    }

    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: file not found: " << inputPath << Qt::endl; return 1;
    }
    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.skin_weights"),
        QString("skin .%1 maxInf=%2 falloff=%3")
            .arg(fi.suffix()).arg(maxInfluences).arg(falloff));
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
        QString("Importing %1").arg(fi.absoluteFilePath()));

    MeshImporterExporter::importer({fi.absoluteFilePath()});
    // Filter to real Ogre::Entity objects — Manager::getEntities()
    // can include helper ManualObjects, which would make a
    // single-entity file look multi-entity (and cast wrong).
    QList<Ogre::Entity*> meshEntities;
    for (Ogre::Entity* e : Manager::getSingleton()->getEntities()) {
        if (e && e->getMovableType() == "Entity")
            meshEntities.push_back(e);
    }
    if (meshEntities.isEmpty()) {
        err() << "Error: failed to load " << inputPath << Qt::endl; return 1;
    }
    if (meshEntities.size() > 1) {
        err() << "Error: " << inputPath
              << " contains multiple mesh entities. `qtmesh skin` "
                 "currently supports one entity per file."
              << Qt::endl;
        return 1;
    }
    Ogre::Entity* entity = meshEntities.first();

    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = maxInfluences;
    opts.falloff                = falloff;
    opts.maxInfluenceDistance   = maxDistance;
    opts.skipUnweightedBones    = skipUnweighted;
    opts.replaceExisting        = replaceExisting;

    const auto report = SkinWeights::computeAndApply(entity, opts);
    if (!report.applied) {
        err() << "Error: skin weights failed — " << report.error << Qt::endl;
        return 1;
    }

    auto* node = entity->getParentSceneNode();
    const QString fmt = formatForExtension(outputPath);
    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
        QString("Exporting %1").arg(QFileInfo(outputPath).absoluteFilePath()));
    if (MeshImporterExporter::exporter(node, QFileInfo(outputPath).absoluteFilePath(), fmt) != 0) {
        err() << "Error: export failed." << Qt::endl;
        return 1;
    }

    if (jsonOutput) {
        cliWrite(QString::fromUtf8(
            QJsonDocument(SkinWeights::reportToJson(report)).toJson(QJsonDocument::Indented)) + "\n");
    } else {
        cliWrite(SkinWeights::reportToText(report)
                 + QString("Wrote: %1\n").arg(QFileInfo(outputPath).fileName()));
    }
    return 0;
}

int CLIPipeline::cmdRig(int argc, char* argv[])
{
    // Parse: rig <file> [--skeleton humanoid|biped|quadruped|generic]
    //        [--algo pinocchio|unirig] [--skin] [--up-axis x|y|z] -o <out> [--json]
    QString inputPath, outputPath, templateName = QStringLiteral("humanoid");
    QString algoName = QStringLiteral("pinocchio");
    bool jsonOutput = false;
    bool alsoSkin = false;
    int upAxis = 1;   // +Y default

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "rig" || arg == "--cli") continue;
        if (arg == "--json") { jsonOutput = true; continue; }
        if (arg == "--skin") { alsoSkin = true; continue; }
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString::fromLocal8Bit(argv[++i]); continue;
        }
        if ((arg == "--skeleton" || arg == "--template") && i + 1 < argc) {
            templateName = QString::fromLocal8Bit(argv[++i]); continue;
        }
        if (arg == "--algo") {
            // Reject a missing value (e.g. `--algo -o out`) instead of silently
            // swallowing the next flag and falling back to the default.
            if (i + 1 >= argc || QString::fromLocal8Bit(argv[i + 1]).startsWith("-")) {
                err() << "Error: --algo requires 'pinocchio', 'unirig', or 'rignet'."
                      << Qt::endl;
                return 2;
            }
            algoName = QString::fromLocal8Bit(argv[++i]).toLower();
            if (algoName != "pinocchio" && algoName != "unirig" && algoName != "rignet") {
                err() << "Error: --algo must be 'pinocchio' or 'unirig' (got '"
                      << algoName << "')." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (arg == "--up-axis") {
            if (i + 1 >= argc) {
                err() << "Error: --up-axis requires a value (x, y, or z)." << Qt::endl;
                return 2;
            }
            const QString a = QString::fromLocal8Bit(argv[++i]).toLower();
            if (a == "x") upAxis = 0;
            else if (a == "y") upAxis = 1;
            else if (a == "z") upAxis = 2;
            else { err() << "Error: --up-axis must be x, y, or z." << Qt::endl; return 2; }
            continue;
        }
        if (!arg.startsWith("-") && inputPath.isEmpty()) {
            inputPath = arg; continue;
        }
    }

    if (inputPath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh rig <file> [--skeleton humanoid|biped|quadruped|generic] "
                 "[--algo pinocchio|unirig] [--skin] [--up-axis x|y|z] -o <out> [--json]" << Qt::endl;
        return 2;
    }
    if (outputPath.isEmpty()) {
        err() << "Error: -o <output> required." << Qt::endl;
        return 2;
    }

    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: file not found: " << inputPath << Qt::endl; return 1;
    }
    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.auto_rig"),
        QString("rig .%1 template=%2 algo=%3 skin=%4")
            .arg(fi.suffix(), templateName, algoName).arg(alsoSkin));
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
        QString("Importing %1").arg(fi.absoluteFilePath()));

    MeshImporterExporter::importer({fi.absoluteFilePath()});
    QList<Ogre::Entity*> meshEntities;
    for (Ogre::Entity* e : Manager::getSingleton()->getEntities()) {
        if (e && e->getMovableType() == "Entity")
            meshEntities.push_back(e);
    }
    if (meshEntities.isEmpty()) {
        err() << "Error: failed to load " << inputPath << Qt::endl; return 1;
    }
    if (meshEntities.size() > 1) {
        err() << "Error: " << inputPath
              << " contains multiple mesh entities. `qtmesh rig` supports one "
                 "entity per file." << Qt::endl;
        return 1;
    }
    Ogre::Entity* entity = meshEntities.first();

    AutoRig::Options opts;
    opts.tmpl      = AutoRig::templateFromString(templateName);
    opts.algorithm = AutoRig::algorithmFromString(algoName);
    opts.upAxis    = upAxis;

    AutoRig::Report report = AutoRig::rigEntity(entity, opts);
    // Surface a RigNet→Pinocchio fallback so the user knows why the result is
    // template-based (e.g. offline / model not yet hosted / non-ONNX build).
    if (!report.fallbackReason.isEmpty())
        err() << "Note: " << report.fallbackReason << Qt::endl;
    if (!report.applied) {
        err() << "Error: auto-rig failed — " << report.error << Qt::endl;
        return 1;
    }

    // Optionally chain skin weights so the exported asset deforms.
    bool skinned = false;
    if (alsoSkin) {
        const auto sw = SkinWeights::computeAndApply(entity, {});
        skinned = sw.applied;
        if (!sw.applied) {
            err() << "Error: rigged, but skinning failed — " << sw.error << Qt::endl;
            return 1;
        }
    }

    auto* node = entity->getParentSceneNode();
    const QString fmt = formatForExtension(outputPath);
    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
        QString("Exporting %1").arg(QFileInfo(outputPath).absoluteFilePath()));
    if (MeshImporterExporter::exporter(node, QFileInfo(outputPath).absoluteFilePath(), fmt) != 0) {
        err() << "Error: export failed." << Qt::endl;
        return 1;
    }

    if (jsonOutput) {
        QJsonObject j = AutoRig::reportToJson(report);
        j["skinned"] = skinned;
        cliWrite(QString::fromUtf8(
            QJsonDocument(j).toJson(QJsonDocument::Indented)) + "\n");
    } else {
        cliWrite(AutoRig::reportToText(report)
                 + (alsoSkin ? QString("  skinned: %1\n").arg(skinned ? "yes" : "no")
                             : QString())
                 + QString("Wrote: %1\n").arg(QFileInfo(outputPath).fileName()));
    }
    return 0;
}

int CLIPipeline::cmdGenerate3d(int argc, char* argv[])
{
    // Parse: generate3d <image> [-o out.glb] [--resolution N] [--no-color] [--no-model]
    QString inputPath, outputPath;
    int resolution = 256;
    bool vertexColor = true;
    bool noModel = false;
    bool removeBg = false;

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "generate3d" || arg == "--cli") continue;
        if (arg == "--no-color") { vertexColor = false; continue; }
        if (arg == "--no-model") { noModel = true; continue; }
        if (arg == "--remove-bg" || arg == "--rembg") { removeBg = true; continue; }
        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                err() << "Error: " << arg << " requires a value." << Qt::endl;
                return 2;
            }
            outputPath = QString::fromLocal8Bit(argv[++i]); continue;
        }
        if (arg == "--resolution") {
            if (i + 1 >= argc) {
                err() << "Error: --resolution requires a value (e.g. 128, 256)." << Qt::endl;
                return 2;
            }
            bool okNum = false;
            resolution = QString::fromLocal8Bit(argv[++i]).toInt(&okNum);
            if (!okNum || resolution < 16 || resolution > 512) {
                err() << "Error: --resolution must be an integer in [16..512]." << Qt::endl;
                return 2;
            }
            continue;
        }
        if (!arg.startsWith("-") && inputPath.isEmpty()) { inputPath = arg; continue; }
    }

    if (inputPath.isEmpty()) {
        err() << "Error: No input image specified." << Qt::endl;
        err() << "Usage: qtmesh generate3d <image> [-o out.glb] [--resolution 256] "
                 "[--no-color] [--remove-bg]" << Qt::endl;
        return 2;
    }
    QFileInfo fi(inputPath);
    if (!fi.exists()) {
        err() << "Error: image not found: " << inputPath << Qt::endl; return 1;
    }
    // Default output: <image>.glb next to the input.
    if (outputPath.isEmpty())
        outputPath = fi.absolutePath() + "/" + fi.completeBaseName() + ".glb";

#ifndef ENABLE_ONNX
    Q_UNUSED(resolution); Q_UNUSED(vertexColor); Q_UNUSED(noModel);
    err() << "Error: this build was compiled without AI image-to-3D generation "
             "(rebuild with -DENABLE_ONNX=ON)." << Qt::endl;
    return 1;
#else
    if (noModel) {
        err() << "Error: --no-model given but TripoSR has no non-model fallback "
                 "(unlike segmentation/in-betweening). Remove --no-model." << Qt::endl;
        return 2;
    }
    QImage image(fi.absoluteFilePath());
    if (image.isNull()) {
        err() << "Error: failed to read image: " << inputPath << Qt::endl; return 1;
    }

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.image_to_3d"),
        QString("generate3d .%1 res=%2 color=%3")
            .arg(fi.suffix()).arg(resolution).arg(vertexColor));

    // Download the model on first use (blocks; clear message when not hosted).
    const QString enc = MeshGenPredictor::ensureModelBlocking();
    if (enc.isEmpty() || !MeshGenPredictor::modelsPresent()) {
        err() << "  (looked for models in: "
              << QFileInfo(MeshGenPredictor::encoderModelPath()).absolutePath()
              << ")" << Qt::endl;
        err() << "Error: TripoSR model unavailable. It downloads on first use from "
                 "the QtMeshEditor models repo; if it is not hosted yet, export it "
                 "with scripts/export-triposr-onnx.py and point "
                 "QTMESH_TRIPOSR_MODEL_BASE_URL (or ai/triposrModelBaseUrl) at it, "
                 "or drop the files in the ai_models/triposr/ cache." << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    MeshGenPredictor::Options opts;
    opts.sdfResolution   = resolution;
    opts.vertexColor     = vertexColor;
    opts.removeBackground = removeBg;
    const MeshGenPredictor::Result res = MeshGenPredictor::predict(
        image, MeshGenPredictor::encoderModelPath(),
        MeshGenPredictor::decoderModelPath(), opts);
    if (!res.ok) {
        err() << "Error: image-to-3D failed: " << res.error << Qt::endl;
        return 1;
    }

    Ogre::SceneNode* node =
        MeshGenBuilder::buildSceneNode(res, QStringLiteral("qtmesh_gen3d"));
    if (!node) {
        err() << "Error: failed to build mesh from prediction." << Qt::endl;
        return 1;
    }

    const QString fmt = formatForExtension(outputPath);
    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
        QString("Exporting %1").arg(QFileInfo(outputPath).absoluteFilePath()));
    if (MeshImporterExporter::exporter(node, QFileInfo(outputPath).absoluteFilePath(), fmt) != 0) {
        err() << "Error: export failed." << Qt::endl;
        return 1;
    }

    cliWrite(QString("Generated 3D mesh: %1 verts, %2 tris%3\nWrote: %4\n")
                 .arg(res.vertexCount).arg(res.triangleCount)
                 .arg(res.colors.empty() ? QString() : QStringLiteral(" (+vertex color)"))
                 .arg(QFileInfo(outputPath).fileName()));
    return 0;
#endif
}

int CLIPipeline::cmdSegment(int argc, char* argv[])
{
    // Parse: segment <file> [--json] [--no-model] [--up-axis x|y|z]
    //                        [--dump-training-data <out.json>]
    QString inputPath;
    QString dumpPath;
    bool jsonOutput = false;
    bool noModel = false;
    int upAxis = 1;   // +Y default

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "segment" || arg == "--cli") continue;
        if (arg == "--json")     { jsonOutput = true; continue; }
        if (arg == "--no-model") { noModel = true; continue; }
        if (arg == "--dump-training-data") {
            if (i + 1 >= argc) {
                err() << "Error: --dump-training-data requires an output path." << Qt::endl;
                return 2;
            }
            dumpPath = QString::fromLocal8Bit(argv[++i]);
            continue;
        }
        if (arg == "--up-axis") {
            if (i + 1 >= argc) {
                err() << "Error: --up-axis requires a value (x, y, or z)." << Qt::endl;
                return 2;
            }
            const QString a = QString::fromLocal8Bit(argv[++i]).toLower();
            if (a == "x") upAxis = 0;
            else if (a == "y") upAxis = 1;
            else if (a == "z") upAxis = 2;
            else { err() << "Error: --up-axis must be x, y, or z." << Qt::endl; return 2; }
            continue;
        }
        if (!arg.startsWith("-") && inputPath.isEmpty()) { inputPath = arg; continue; }
    }

    if (inputPath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh segment <file> [--json] [--no-model] [--up-axis x|y|z] "
                 "[--dump-training-data <out.json>]" << Qt::endl;
        return 2;
    }
    QFileInfo fi(inputPath);
    if (!fi.exists()) { err() << "Error: file not found: " << inputPath << Qt::endl; return 1; }
    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.segment"),
        QString("segment .%1 noModel=%2").arg(fi.suffix()).arg(noModel));

    MeshImporterExporter::importer({fi.absoluteFilePath()});
    Ogre::Entity* entity = nullptr;
    for (Ogre::Entity* e : Manager::getSingleton()->getEntities()) {
        if (e && e->getMovableType() == "Entity") { entity = e; break; }
    }
    if (!entity) { err() << "Error: failed to load " << inputPath << Qt::endl; return 1; }

    std::vector<float> verts;
    std::vector<uint32_t> indices;
    if (!AutoRig::gatherGeometry(entity, verts, indices) || verts.empty()) {
        err() << "Error: no readable geometry in " << inputPath << Qt::endl; return 1;
    }
    const int vertexCount = static_cast<int>(verts.size() / 3);
    int rigResolved = 0;
    std::vector<int> rigLabels = AutoRig::rigPriorPartLabels(entity, vertexCount, &rigResolved);

    // --- Training-data miner ------------------------------------------------
    // `--dump-training-data out.json` writes the mesh's point cloud + EXACT
    // per-vertex part labels derived from the rig (bone weights → bone name →
    // part, via the same AutoRig::rigPriorPartLabels the GUI uses). Every
    // rigged mesh becomes one free, exactly-labelled training sample — the
    // segmentation model is retrained on a mix of synthetic humanoids + these
    // mined real meshes (scripts/export-meshseg-onnx.py --real-data), so the
    // MODEL path (used on UNrigged meshes) keeps improving as more rigged
    // assets are mined. Requires a skinned mesh.
    if (!dumpPath.isEmpty()) {
        if (rigLabels.empty()) {
            err() << "Error: --dump-training-data needs a SKINNED mesh (no skeleton in "
                  << inputPath << ")." << Qt::endl;
            return 1;
        }
        if (rigResolved < (vertexCount + 1) / 2) {
            err() << "Error: rig resolved only " << rigResolved << " / " << vertexCount
                  << " vertices to body parts — too sparse to be reliable training data."
                  << Qt::endl;
            return 1;
        }
        // Normalise positions into a centred unit box (the model's input frame),
        // matching MeshSegmenter::predict's normalisation so mined samples and
        // inference see the same coordinate convention.
        float mn[3] = { verts[0], verts[1], verts[2] }, mx[3] = { verts[0], verts[1], verts[2] };
        for (int v = 0; v < vertexCount; ++v)
            for (int a = 0; a < 3; ++a) {
                mn[a] = std::min(mn[a], verts[3*v+a]);
                mx[a] = std::max(mx[a], verts[3*v+a]);
            }
        const float ctr[3] = { 0.5f*(mn[0]+mx[0]), 0.5f*(mn[1]+mx[1]), 0.5f*(mn[2]+mx[2]) };
        float half = 0.0f;
        for (int a = 0; a < 3; ++a) half = std::max(half, 0.5f*(mx[a]-mn[a]));
        const float invs = half > 1e-6f ? 1.0f/half : 1.0f;

        QJsonArray pts, labs;
        for (int v = 0; v < vertexCount; ++v) {
            pts.append((verts[3*v+0]-ctr[0])*invs);
            pts.append((verts[3*v+1]-ctr[1])*invs);
            pts.append((verts[3*v+2]-ctr[2])*invs);
            labs.append(rigLabels[v] < 0 ? 0 : rigLabels[v]);   // bone-but-non-body → unknown(0)
        }
        QJsonObject root;
        root["schema"]      = "qtmesh-meshseg-training-v1";
        root["mesh"]        = fi.fileName();
        root["upAxis"]      = upAxis;     // miner records the source up axis
        root["vertexCount"] = vertexCount;
        root["resolved"]    = rigResolved;
        root["partCount"]   = MeshSegmenter::partCount();
        root["points"]      = pts;        // flat [x,y,z, …] normalised to unit box
        root["labels"]      = labs;       // per-vertex part index (0=unknown)
        QFile out(dumpPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            err() << "Error: cannot write " << dumpPath << Qt::endl; return 1;
        }
        out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        out.close();
        cliWrite(QString("Wrote training sample %1 — %2 verts, %3 resolved (%4%)\n")
                     .arg(dumpPath).arg(vertexCount).arg(rigResolved)
                     .arg(100.0 * rigResolved / std::max(1, vertexCount), 0, 'f', 1));
        return 0;
    }

    QString modelPath;
    if (!noModel) modelPath = MeshSegmenter::ensureModelBlocking();

    MeshSegmenter::Options opts;
    opts.upAxis = upAxis;
    opts.forceFallback = noModel;
    const MeshSegmenter::Result r = MeshSegmenter::predict(
        verts.data(), vertexCount, indices.data(), static_cast<int>(indices.size()),
        modelPath, opts, rigLabels.empty() ? nullptr : rigLabels.data());
    if (!r.ok) {
        err() << "Error: " << (r.error.isEmpty() ? QStringLiteral("segmentation failed") : r.error)
              << Qt::endl;
        return 1;
    }

    // Per-part counts.
    const int P = MeshSegmenter::partCount();
    std::vector<int> vCount(P, 0), fCount(P, 0);
    for (int l : r.vertexLabels) if (l >= 0 && l < P) ++vCount[l];
    for (int l : r.faceLabels)   if (l >= 0 && l < P) ++fCount[l];

    if (jsonOutput) {
        QJsonObject root;
        root["mesh"] = fi.fileName();
        root["method"] = r.usedModel ? "model" : "geometric_fallback";
        if (!r.usedModel && !r.fallbackReason.isEmpty())
            root["fallbackReason"] = r.fallbackReason;
        root["vertexCount"] = vertexCount;
        root["faceCount"] = static_cast<int>(r.faceLabels.size());
        QJsonObject parts;
        for (int p = 0; p < P; ++p) {
            QJsonObject pc;
            pc["vertices"] = vCount[p];
            pc["faces"] = fCount[p];
            parts[MeshSegmenter::partName(p)] = pc;
        }
        root["parts"] = parts;
        QJsonArray vl, fl;
        for (int l : r.vertexLabels) vl.append(l);
        for (int l : r.faceLabels)   fl.append(l);
        root["vertexLabels"] = vl;
        root["faceLabels"] = fl;
        cliWrite(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)) + "\n");
    } else {
        cliWrite(QString("Segmented %1 — %2 verts, %3 faces via %4\n")
                     .arg(fi.fileName()).arg(vertexCount).arg(r.faceLabels.size())
                     .arg(r.usedModel ? "model" : "geometric fallback"));
        for (int p = 0; p < P; ++p) {
            if (vCount[p] == 0 && fCount[p] == 0) continue;
            cliWrite(QString("  %1: %2 verts, %3 faces\n")
                         .arg(MeshSegmenter::partName(p), -10).arg(vCount[p]).arg(fCount[p]));
        }
        if (!r.usedModel && !r.fallbackReason.isEmpty())
            cliWrite(QString("Note: %1\n").arg(r.fallbackReason));
    }
    return 0;
}

int CLIPipeline::cmdMorph(int argc, char* argv[])
{
    // Parse: morph <file> --list [--json]
    QString filePath;
    bool listMode = false;
    bool jsonOutput = false;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "morph" || arg == "--cli") continue;
        if (arg == "--list") { listMode = true; continue; }
        if (arg == "--json") { jsonOutput = true; continue; }
        if (!arg.startsWith("-") && filePath.isEmpty()) {
            filePath = arg; continue;
        }
    }

    if (filePath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh morph <file> --list [--json]" << Qt::endl;
        return 2;
    }
    if (!listMode) {
        err() << "Error: morph subcommand requires --list (other modes land in follow-up slices)." << Qt::endl;
        return 2;
    }

    QFileInfo fi(filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << filePath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.morph",
        QString("Morph list .%1").arg(fi.suffix()));
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing %1 for morph list").arg(fi.absoluteFilePath()));

    MeshImporterExporter::importer({fi.absoluteFilePath()});

    // Walk every imported entity, not just the first — multi-entity
    // files (e.g. FBX with separate body + head meshes that both
    // carry blend shapes) would otherwise lose the targets on the
    // entities the loop missed.
    auto& movables = Manager::getSingleton()->getEntities();
    QList<Ogre::Entity*> entities;
    for (auto* obj : movables) {
        if (obj && obj->getMovableType() == "Entity")
            entities.append(static_cast<Ogre::Entity*>(obj));
    }
    if (entities.isEmpty()) {
        err() << "Error: Failed to load file: " << filePath << Qt::endl;
        return 1;
    }

    QStringList targets;
    QSet<QString> seen;
    for (Ogre::Entity* entity : entities) {
        const QStringList ents = MorphAnimationManager::instance()->morphTargetsFor(entity);
        for (const QString& n : ents) {
            if (!seen.contains(n)) {
                seen.insert(n);
                targets.append(n);
            }
        }
    }

    if (jsonOutput) {
        QJsonArray arr;
        for (const QString& n : targets) arr.append(n);
        QJsonObject root;
        root["file"]  = filePath;
        root["count"] = static_cast<int>(targets.size());
        root["morphTargets"] = arr;
        cliWrite(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)));
    } else {
        if (targets.isEmpty()) {
            cliWrite(QStringLiteral("No morph targets / blend shapes found.\n"));
        } else {
            cliWrite(QStringLiteral("Morph targets (%1):\n").arg(targets.size()));
            for (const QString& n : targets)
                cliWrite(QStringLiteral("  %1\n").arg(n));
        }
    }
    return 0;
}

int CLIPipeline::cmdNodeAnim(int argc, char* argv[])
{
    // Parse: nodeanim <file> --list [--json]
    QString filePath;
    bool listMode = false;
    bool jsonOutput = false;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "nodeanim" || arg == "--cli") continue;
        if (arg == "--list") { listMode = true; continue; }
        if (arg == "--json") { jsonOutput = true; continue; }
        if (!arg.startsWith("-") && filePath.isEmpty()) {
            filePath = arg; continue;
        }
    }

    if (filePath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh nodeanim <file> --list [--json]" << Qt::endl;
        return 2;
    }
    if (!listMode) {
        err() << "Error: nodeanim subcommand requires --list (other modes need C5 exporter round-trip)." << Qt::endl;
        return 2;
    }

    QFileInfo fi(filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << filePath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb("cli.nodeanim",
        QString("NodeAnim list .%1").arg(fi.suffix()));
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing %1 for nodeanim list").arg(fi.absoluteFilePath()));

    MeshImporterExporter::importer({fi.absoluteFilePath()});

    // The NodeAnimationManager reads from the SceneManager's animation
    // table, which is what `importer()` populated. Round-trip the
    // listClips() Q_INVOKABLE so the CLI output matches whatever an
    // MCP agent would see on the same file via list_node_animations.
    auto* m = NodeAnimationManager::instance();
    QStringList clips = m ? m->listClips() : QStringList();

    // Soft "import failed" guard: node-animation clips are scene-level
    // data and CAN exist independently of entities (animated empties,
    // pure transform-only scenes). So we can't use cmdMorph's strict
    // "no entities" check — that would false-positive on a valid
    // load that just happens to have neither meshes nor clips. The
    // softer signal is "neither dimension produced anything": no
    // entities AND no clips, which only happens for corrupt /
    // unsupported files. Valid empty scenes still exit 0 with
    // "No node animations found".
    if (clips.isEmpty()) {
        auto& movables = Manager::getSingleton()->getEntities();
        bool hasAnyEntity = false;
        for (auto* obj : movables) {
            if (obj && obj->getMovableType() == "Entity") {
                hasAnyEntity = true;
                break;
            }
        }
        if (!hasAnyEntity) {
            err() << "Error: Failed to load file: " << filePath << Qt::endl;
            return 1;
        }
    }

    if (jsonOutput) {
        QJsonArray arr;
        for (const QString& n : clips) arr.append(n);
        QJsonObject root;
        root["file"]  = filePath;
        root["count"] = static_cast<int>(clips.size());
        root["clips"] = arr;
        cliWrite(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)));
    } else {
        if (clips.isEmpty()) {
            cliWrite(QStringLiteral("No node animations found.\n"));
        } else {
            cliWrite(QStringLiteral("Node animations (%1):\n").arg(clips.size()));
            for (const QString& n : clips)
                cliWrite(QStringLiteral("  %1\n").arg(n));
        }
    }
    return 0;
}
