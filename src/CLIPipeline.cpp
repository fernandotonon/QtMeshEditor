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
#include "QtMeshCloudClient.h"
#include <QApplication>
#include <QWidget>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDebug>
#include <QTextStream>
#include <QSysInfo>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <OgreSubMesh.h>

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

static QString findingSeverityTag(Severity severity)
{
    switch (severity) {
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
        s << "         [" << findingSeverityTag(f.severity) << "] "
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
        "  scan [path] [options]           Scan directory for 3D asset issues (default path: .)\n"
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
        {".assbin", "Assimp Binary (*.assbin)"}
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

    // Textures
    std::set<std::string, std::less<>> seenTex;
    for (unsigned int i = 0; i < entity->getNumSubEntities(); ++i) {
        auto mat = entity->getSubEntity(i)->getMaterial();
        if (!mat) continue;
        for (auto* tech : mat->getTechniques()) {
            for (auto* pass : tech->getPasses()) {
                for (auto* tus : pass->getTextureUnitStates()) {
                    if (tus->getContentType() == Ogre::TextureUnitState::CONTENT_NAMED) {
                        auto name = tus->getTextureName();
                        if (seenTex.insert(name).second)
                            info.textures << QString::fromStdString(name);
                    }
                }
            }
        }
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
    //    or: anim <file> --rename <old> <new> [-o <output>]
    //    or: anim <file> --merge <f1> [f2...] [-o <output>]
    //    or: anim <file> --resample N [-o <output>] [--animation <name>]
    //    or: anim <file> --decimate-step S [-o <output>] [--animation <name>]
    QString filePath, oldName, newName, outputPath, animationFilter;
    bool listMode = false;
    bool renameMode = false;
    bool mergeMode = false;
    bool resampleMode = false;
    bool decimateMode = false;
    bool jsonOutput = false;
    int resampleCount = 0;
    int decimateStep = 0;
    QStringList mergeFiles;

    // Collect positional args (excluding flags)
    QStringList positional;
    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg == "anim" || arg == "--cli") continue;
        if (arg == "--list") { listMode = true; continue; }
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

    if (!listMode && !renameMode && !mergeMode && !resampleMode && !decimateMode) {
        err() << "Error: Specify --list, --rename, --merge, --resample, or --decimate-step." << Qt::endl;
        err() << "Usage: qtmesh anim <file> --list [--json]" << Qt::endl;
        err() << "       qtmesh anim <file> --rename <old> <new> [-o <output>]" << Qt::endl;
        err() << "       qtmesh anim <file> --merge <f1> [f2...] [-o <output>]" << Qt::endl;
        err() << "       qtmesh anim <file> --resample N [-o <output>] [--animation <name>]" << Qt::endl;
        err() << "       qtmesh anim <file> --decimate-step S [-o <output>] [--animation <name>]" << Qt::endl;
        return 2;
    }

    if ((renameMode || mergeMode || resampleMode || decimateMode) && outputPath.isEmpty()) {
        outputPath = filePath;  // overwrite in place
    }

    QFileInfo fi(filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << filePath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    QString animOp = listMode ? "list" : (renameMode ? "rename" : (resampleMode ? "resample" : (decimateMode ? "decimate" : "merge")));
    SentryReporter::addBreadcrumb("cli.anim", QString("Anim %1 .%2%3")
        .arg(animOp, fi.suffix(), mergeMode ? QString(" files=%1").arg(mergeFiles.size()) : ""));

    MeshImporterExporter::importer({fi.absoluteFilePath()});

    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty()) {
        SentryReporter::captureMessage(QString("CLI anim: import failed (.%1)").arg(fi.suffix()), "error");
        err() << "Error: Failed to load file: " << filePath << Qt::endl;
        return 1;
    }

    Ogre::Entity* entity = entities.first();
    if (!entity->hasSkeleton()) {
        err() << "Error: File has no skeleton/animations." << Qt::endl;
        return 1;
    }

    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
    if (!skel) {
        err() << "Error: No skeleton found." << Qt::endl;
        return 1;
    }

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

    // Merge mode
    if (mergeMode) {
        // Load animation files; animation-only files (no mesh) produce a skeleton instead of entity.
        QList<Ogre::SkeletonPtr> animOnlySkeletons;
        for (const auto& f : mergeFiles) {
            int entityCountBefore = Manager::getSingleton()->getEntities().size();
            int skelCountBefore = animOnlySkeletons.size();
            MeshImporterExporter::importer({f}, 0, &animOnlySkeletons);
            bool gotEntity = Manager::getSingleton()->getEntities().size() > entityCountBefore;
            bool gotSkeleton = animOnlySkeletons.size() > skelCountBefore;
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
        if (allEntities.size() < 2 && animOnlySkeletons.isEmpty()) {
            err() << "Error: Need at least one source file to merge (got none)." << Qt::endl;
            return 1;
        }

        QString mergeErr;
        Ogre::Entity* merged = AnimationMerger::mergeAnimations(allEntities.first(), allEntities, animOnlySkeletons, mergeErr);
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

        entity->refreshAvailableAnimationState();

        auto* node = entity->getParentSceneNode();
        QFileInfo outFi(outputPath);
        int result = MeshImporterExporter::exporter(node, outFi.absoluteFilePath(), formatForExtension(outputPath));
        if (result != 0) {
            SentryReporter::captureMessage(QString("CLI anim: resample export failed (.%1)").arg(outFi.suffix()), "error");
            err() << "Error: Export failed." << Qt::endl;
            return 1;
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

        entity->refreshAvailableAnimationState();

        auto* node = entity->getParentSceneNode();
        QFileInfo outFi(outputPath);
        int result = MeshImporterExporter::exporter(node, outFi.absoluteFilePath(), formatForExtension(outputPath));
        if (result != 0) {
            SentryReporter::captureMessage(QString("CLI anim: decimate export failed (.%1)").arg(outFi.suffix()), "error");
            err() << "Error: Export failed." << Qt::endl;
            return 1;
        }

        cliWrite(QString("Decimated %1 animation(s) with step %2 (removed %3 keyframes)\nOutput: %4\n")
            .arg(animsProcessed).arg(decimateStep).arg(totalRemoved).arg(outFi.fileName()));
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
    entity->refreshAvailableAnimationState();

    auto* node = entity->getParentSceneNode();
    QFileInfo outFi(outputPath);
    QString fmt = formatForExtension(outputPath);

    int result = MeshImporterExporter::exporter(node, outFi.absoluteFilePath(), fmt);
    if (result != 0) {
        SentryReporter::captureMessage(QString("CLI anim: rename export failed (.%1)").arg(outFi.suffix()), "error");
        err() << "Error: Export failed." << Qt::endl;
        return 1;
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
    // Parse: pose <file> --animation <name> --time <t> -o <output>
    //        pose <file> --animation <name> --count N -o <pattern>
    QString filePath, outputPath, animName;
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
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = QString(argv[++i]);
            continue;
        }
        if (!arg.startsWith("-") && filePath.isEmpty()) {
            filePath = arg;
            continue;
        }
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

    if (!includeArg.isEmpty()) {
        config.includePatterns.clear();
        for (const auto& p : includeArg.split(",")) {
            QString pattern = p.trimmed();
            // Normalize bare extension patterns: *.fbx → **/*.fbx
            if (!pattern.contains("/") && !pattern.startsWith("**/"))
                pattern = "**/" + pattern;
            config.includePatterns.append(pattern);
        }
    }
    if (!excludeArg.isEmpty()) {
        for (const auto& p : excludeArg.split(",")) {
            QString pattern = p.trimmed();
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
