#include "CLIPipeline.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "AnimationMerger.h"
#include "SentryReporter.h"
#include <QApplication>
#include <QWidget>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDebug>
#include <QTextStream>
#include <QSysInfo>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <set>
#include <cstdio>

#ifndef Q_OS_WIN
#include <unistd.h>
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

static QTextStream& err()
{
    static QTextStream s(stderr);
    return s;
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
    if (path.endsWith(".fbx", Qt::CaseInsensitive)) return "FBX Binary (*.fbx)";
    if (path.endsWith(".glb2", Qt::CaseInsensitive)) return "glTF 2.0 Binary (*.glb2)";
    if (path.endsWith(".gltf2", Qt::CaseInsensitive)) return "glTF 2.0 (*.gltf2)";
    if (path.endsWith(".dae", Qt::CaseInsensitive)) return "Collada (*.dae)";
    if (path.endsWith(".obj", Qt::CaseInsensitive)) return "OBJ (*.obj)";
    if (path.endsWith(".stl", Qt::CaseInsensitive)) return "STL (*.stl)";
    if (path.endsWith(".ply", Qt::CaseInsensitive)) return "PLY (*.ply)";
    if (path.endsWith(".3ds", Qt::CaseInsensitive)) return "3DS (*.3ds)";
    if (path.endsWith(".x", Qt::CaseInsensitive)) return "X (*.x)";
    if (path.endsWith(".mesh.xml", Qt::CaseInsensitive)) return "Ogre XML (*.mesh.xml)";
    if (path.endsWith(".mesh", Qt::CaseInsensitive)) return "Ogre Mesh (*.mesh)";
    if (path.endsWith(".assbin", Qt::CaseInsensitive)) return "Assimp Binary (*.assbin)";
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
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            printVersion();
            return 0;
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
    MeshImporterExporter::importer({fi.absoluteFilePath()}, 0, &animOnlySkeletons);

    auto& entities = Manager::getSingleton()->getEntities();

    // Animation-only file: no entities, but skeleton was loaded.
    if (entities.isEmpty() && !animOnlySkeletons.isEmpty()) {
        QList<MeshInfo> infos;
        for (const Ogre::SkeletonPtr& skel : animOnlySkeletons) {
            if (!skel) continue;
            MeshInfo info;
            info.file = fi.fileName();
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
            cliWrite(formatMeshInfoText(info));
        }
    }

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
    QString filePath, oldName, newName, outputPath;
    bool listMode = false;
    bool renameMode = false;
    bool mergeMode = false;
    bool jsonOutput = false;
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

    if (!listMode && !renameMode && !mergeMode) {
        err() << "Error: Specify --list, --rename <old> <new>, or --merge <files...>." << Qt::endl;
        err() << "Usage: qtmesh anim <file> --list [--json]" << Qt::endl;
        err() << "       qtmesh anim <file> --rename <old> <new> [-o <output>]" << Qt::endl;
        err() << "       qtmesh anim <file> --merge <f1> [f2...] [-o <output>]" << Qt::endl;
        return 2;
    }

    if ((renameMode || mergeMode) && outputPath.isEmpty()) {
        outputPath = filePath;  // overwrite in place
    }

    QFileInfo fi(filePath);
    if (!fi.exists()) {
        err() << "Error: File not found: " << filePath << Qt::endl;
        return 1;
    }

    if (!initOgreHeadless()) return 1;

    QString animOp = listMode ? "list" : (renameMode ? "rename" : "merge");
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

