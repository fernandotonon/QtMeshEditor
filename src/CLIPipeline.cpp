#include "CLIPipeline.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "AnimationMerger.h"
#include "MeshValidator.h"
#include "MeshLodController.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "ScanConfig.h"
#include "ScanEngine.h"
#include "FBX/FBXExporter.h"
#include "MaterialPresetLibrary.h"
#include "TextureChannelPacker.h"
#include "TextureAtlasPacker.h"
#include "ApplyAtlas.h"
#include "NormalMapGenerator.h"
#include "MemoryEstimator.h"
#include "DrawCallAnalyzer.h"
#include "VertexCacheOptimizer.h"
#include "MeshDecimator.h"
#include "EditableMesh.h"
#include "TexturePaintBuffer.h"
#include "VertexColorBaker.h"
#include "VATBaker.h"
#include "MorphAnimationManager.h"
#include "NodeAnimationManager.h"
#include "PoseLibrary.h"
#include "QtMeshCloudClient.h"
#include <OgreMaterialSerializer.h>
#include <QApplication>
#include <QWidget>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
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
        "  lod <file> --count N [--reductions r,...] [-o output]\n"
        "                                    Generate N LOD levels; exports <base>_lod1.<ext> etc.\n"
        "  lod <file> --auto [-o output]     Auto-generate LOD levels\n"
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
        "  scan [path] [options]           Scan directory for 3D asset issues (default path: .)\n"
        "  material <file> --preset <name> [-o <output>]\n"
        "                                  Apply a built-in material preset to every sub-entity\n"
        "                                  (Plastic/Metal/Wood/Glass/Unlit/Wireframe + PBR templates:\n"
        "                                  Metallic-Roughness, Specular-Glossiness, Unlit PBR)\n"
        "  material --list-presets         List the built-in preset names\n"
        "\n"
        "Scan options:\n"
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
        "  Quality rules (config only — set in qtmesh.yml):\n"
        "    max_texture_resolution: <px>      Largest texture edge ceiling (e.g. 2048)\n"
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
        "  --config always wins. Scan JSON uploads when a token is set (unless --no-upload).\n"
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
        "  decimate <file> -o <output> (--reduction <r> | --target-tris N | --target-verts N) [--json]\n"
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
        "  vat <file> --anim <name> [--fps N] [-o <dir>] [--json]\n"
        "                                    Bake a skeletal animation into a Vertex Animation Texture\n"
        "                                    in OpenVAT (sharpen3d/openvat) format: a single 16-bit RGB\n"
        "                                    PNG (height = 2 × frames; top half positions, bottom half\n"
        "                                    normals) plus `<basename>-remap_info.json` with the\n"
        "                                    canonical `os-remap` sidecar shape. Off-the-shelf openvat\n"
        "                                    reference shaders for Godot / Unity / Unreal / Blender\n"
        "                                    consume the output unmodified. Drop-in shader templates\n"
        "                                    for Godot/Unity/Unreal live at `tools/vat-shaders/`.\n"
        "  morph <file> --list [--json]      List morph targets / blend shapes on a mesh. (Set/add/delete\n"
        "                                    land in follow-up slices once authoring is in place.)\n"
        "  nodeanim <file> --list [--json]   List node-animation clips on a scene (props, doors, machinery,\n"
        "                                    animated lights — anything non-skeletal). Authoring on the CLI\n"
        "                                    side needs the C5 glTF/FBX exporter round-trip first.\n"
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

    // Already have a render window (e.g. from tryInitOgre() in tests) — nothing to do.
    auto* root = Manager::getSingleton()->getRoot();
    if (root) {
        try {
            if (root->getRenderTarget("TestHidden") || root->getRenderTarget("CLIHidden"))
                return true;
        } catch (...) {
            // getRenderTarget may throw if not found in some Ogre versions
        }
    }

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
        return true;
    } catch (...) {
        err() << "Error: Failed to create render window." << Qt::endl;
        return false;
    }
}

MeshInfo CLIPipeline::extractMeshInfo(const Ogre::Entity* entity, const QString& fileName)
{
    MeshInfo info;
    info.file = fileName;

    if (!entity) return info;

    const Ogre::MeshPtr& mesh = entity->getMesh();
    if (!mesh) return info;

    info.submeshes = mesh->getNumSubMeshes();

    // Count vertices
    if (mesh->sharedVertexData)
        info.vertices += mesh->sharedVertexData->vertexCount;
    for (unsigned int i = 0; i < info.submeshes; ++i) {
        Ogre::SubMesh* sub = mesh->getSubMesh(i);
        if (sub->vertexData)
            info.vertices += sub->vertexData->vertexCount;
        if (sub->indexData)
            info.triangles += sub->indexData->indexCount / 3;
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
    else if (cmd == "morph") rc = cmdMorph(argc, argv);
    else if (cmd == "nodeanim") rc = cmdNodeAnim(argc, argv);

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
    bool jsonOutput = false;
    int resampleCount = 0;
    int decimateStep = 0;
    int bakeFps      = 0;
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

    if (!listMode && !renameMode && !mergeMode && !resampleMode && !decimateMode
        && !simplifyMode && !analyzeMode && !bakeFpsMode) {
        err() << "Error: Specify --list, --rename, --merge, --resample, --decimate-step, --simplify, --bake-fps, or --analyze." << Qt::endl;
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
        return 2;
    }

    if ((renameMode || mergeMode || resampleMode || decimateMode || simplifyMode
         || bakeFpsMode) && outputPath.isEmpty()) {
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
    // Parse: lod <file> --count N [--reductions r,...] [-o output]
    //    or: lod <file> --auto [-o output]
    //    or: lod <file> --remove [-o output]
    //    or: lod <file> --info [--json]
    QString inputPath, outputPath;
    int lodCount = 0;
    bool autoMode   = false;
    bool removeMode = false;
    bool infoMode   = false;
    bool jsonOutput = false;
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
        err() << "Usage: qtmesh lod <file> --count N [--reductions r,...] [-o output]" << Qt::endl;
        err() << "       qtmesh lod <file> --auto [-o output]" << Qt::endl;
        err() << "       qtmesh lod <file> --remove [-o output]" << Qt::endl;
        err() << "       qtmesh lod <file> --info [--json]" << Qt::endl;
        return 2;
    }

    if (!autoMode && !removeMode && !infoMode && lodCount <= 0) {
        err() << "Error: Specify --count N, --auto, --remove, or --info." << Qt::endl;
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
    if (autoMode) {
        MeshLodController::instance()->generateAutoLods();
    } else {
        lodCount = std::max(1, std::min(lodCount, 4));
        MeshLodController::instance()->generateLods(lodCount, reductions);
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
    Ogre::SceneNode* sn = entity->getParentSceneNode();
    const unsigned int numSubs = mesh->getNumSubMeshes();
    int exported = 0;

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

int CLIPipeline::cmdMaterial(int argc, char* argv[])
{
    // Parse:
    //   material <file> --preset <name> [-o <output>]
    //   material <file> --list-presets
    //   material --list-presets
    QString inputPath, outputPath, presetName;
    bool listPresets = false;

    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "material" || arg == "--cli") continue;
        if (arg == "--list-presets") { listPresets = true; continue; }
        if (arg == "--preset" && i + 1 < argc) {
            presetName = QString(argv[++i]);
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
        QString value;
        ParseValueResult parseResult = parseValueArg(arg, "--config", i, value);
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

    // Load config (precedence):
    // 1) --config path (never fetch remote rules)
    // 2) Else if ingest token set → GET /v1/ingest/rules (skips local qtmesh.yml|yaml|json)
    // 3) Else local qtmesh.yml | yaml | json in cwd
    // 4) Else built-in defaults
    ScanConfig config;
    if (!configPath.isEmpty()) {
        if (!QFileInfo::exists(configPath)) {
            err() << "Error: Config file not found: " << configPath << Qt::endl;
            return 2;
        }
        config = ScanConfig::loadFromFile(configPath);
        if (!resolveIngestToken(tokenArg).isEmpty()) {
            err() << "Note: Using --config file; remote cloud rules were not fetched."
                 << " Scan JSON is still uploaded when an ingest token is set (unless --no-upload)."
                 << Qt::endl;
        }
    } else {
        const QString ingestForRules = resolveIngestToken(tokenArg);
        if (!ingestForRules.isEmpty()) {
            SentryReporter::addBreadcrumb(QStringLiteral("cli.scan"),
                QStringLiteral("QtMesh Cloud fetchRules: requested"));
            const auto rules = QtMeshCloudClient::fetchRules(ingestForRules);
            if (rules.ok) {
                config = ScanConfig::fromJson(rules.config);
                err() << "Note: Using QtMesh Cloud rules (source: " << rules.source << ")." << Qt::endl;
                SentryReporter::addBreadcrumb(QStringLiteral("cli.scan"),
                    QStringLiteral("QtMesh Cloud fetchRules: ok source=%1").arg(rules.source));
            } else {
                err() << "Warning: Could not load QtMesh Cloud rules (" << rules.errorString
                     << "). Using built-in defaults." << Qt::endl;
                config = ScanConfig::defaults();
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
                config = ScanConfig::loadFromFile(localAutoPath);
                err() << "Note: Using local " << localAutoPath << "." << Qt::endl;
            } else {
                config = ScanConfig::defaults();
            }
        }
    }

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

    const QJsonObject reportJson = ScanEngine::scanReportToJsonObject(result);

    // Output to terminal
    if (jsonOutput) {
        cliWrite(QString::fromUtf8(QJsonDocument(reportJson).toJson(QJsonDocument::Indented)) + "\n");
    } else {
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
            f.write(ScanEngine::formatSarif(result).toUtf8());
        else
            err() << "Warning: Could not write SARIF report to " << sarifPath << Qt::endl;
    }

    // Also write reports if configured in the config file
    if (reportPath.isEmpty() && !config.reportOutput.isEmpty()) {
        QFile f(config.reportOutput);
        QDir().mkpath(QFileInfo(config.reportOutput).path());
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            if (config.reportFormat == "text")
                f.write(ScanEngine::formatText(result, config, false).toUtf8());
            else
                f.write(QJsonDocument(reportJson).toJson(QJsonDocument::Indented));
        }
    }
    if (sarifPath.isEmpty() && !config.sarifOutput.isEmpty()) {
        QFile f(config.sarifOutput);
        QDir().mkpath(QFileInfo(config.sarifOutput).path());
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write(ScanEngine::formatSarif(result).toUtf8());
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
                     "(--reduction <r> | --target-tris N | --target-verts N) [--json]"
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

    const DecimationReport report = MeshDecimator::decimateEntity(entity, reduction);
    if (!report.applied && reduction > 0.0) {
        err() << "Error: Decimation failed (MeshLodGenerator). The mesh may not be "
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

int CLIPipeline::cmdVat(int argc, char* argv[])
{
    // Parse: vat <file> --anim <name> [--fps N] [-o <dir>] [--json]
    //
    // Output is always OpenVAT (sharpen3d/openvat) — a single packed
    // 16-bit RGB PNG (`<basename>_pos.png`, height = 2*frames, top half
    // positions, bottom half normals) plus `<basename>-remap_info.json`
    // with the canonical `os-remap` schema. Consumed unmodified by the
    // openvat reference shaders for Godot / Unity / Unreal / Blender.
    QString filePath, animName, outDir;
    double fps = 30.0;
    bool jsonOutput = false;

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
        if (!arg.startsWith("-") && filePath.isEmpty()) {
            filePath = arg; continue;
        }
    }

    if (filePath.isEmpty()) {
        err() << "Error: No input file specified." << Qt::endl;
        err() << "Usage: qtmesh vat <file> --anim <name> [--fps N] [-o <dir>] [--json]" << Qt::endl;
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

    VATBaker::Options opts;
    opts.animationName = animName;
    opts.fps           = fps;
    opts.outputDir     = outDir;
    opts.basename      = animName;

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

    if (jsonOutput) {
        QJsonObject obj;
        obj["ok"]          = true;
        obj["texture"]     = result.posTexPath;
        obj["sidecar"]     = result.jsonPath;
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
        cliWrite(QStringLiteral("  bounds:   min=(%1, %2, %3) max=(%4, %5, %6)\n")
                     .arg(result.minBound.x, 0, 'f', 3)
                     .arg(result.minBound.y, 0, 'f', 3)
                     .arg(result.minBound.z, 0, 'f', 3)
                     .arg(result.maxBound.x, 0, 'f', 3)
                     .arg(result.maxBound.y, 0, 'f', 3)
                     .arg(result.maxBound.z, 0, 'f', 3));
        cliWrite(QStringLiteral(
            "  shaders:  drop-in Godot/Unity/Unreal templates at tools/vat-shaders/\n"));
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
