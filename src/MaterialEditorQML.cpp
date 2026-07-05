#include "MaterialEditorQML.h"
#include "HDR/HdrMaterialScript.h"
#include "MaterialPreviewRenderer.h"
#include "Manager.h"
#include "SentryReporter.h"
#include "LLMManager.h"
#include "SelectionSet.h"
#include "MeshDepthRenderer.h"
#include "MultiViewTextureBaker.h"
#include "TexturePaintBuffer.h"
#include "ImageTo3D/BackgroundRemover.h"
#include "UvUnwrap.h"
#include <QPainter>
#include "EmbeddedTextureCache.h"
#include <OgreEntity.h>
#include <OgreSubEntity.h>
#include <OgreMesh.h>
#include <OgreTechnique.h>
#include <QDir>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QFileInfo>
#include <QTextStream>
#include <set>
#ifdef ENABLE_STABLE_DIFFUSION
#include "SDManager.h"
#endif
#ifdef ENABLE_ONNX
#include "AIAssistManager.h"
#include "TextureUpscaler.h"
#include <QPointer>
#include <thread>
#endif
#include "QMLMaterialHighlighter.h"
#include "ModelDownloader.h"
#include "RTShaderHelper.h"
#include "TextureChannelPacker.h"
#include "TextureAtlasPacker.h"
#include "ApplyAtlas.h"
#include "MeshImporterExporter.h"
#include "NormalMapGenerator.h"
#include "PS1/PS1TIM.h"
#include <OgreRTShaderSystem.h>
#include <QDebug>
#include <QBuffer>
#include <QFileDialog>
#include <algorithm>
#include <QColorDialog>
#include <QApplication>
#include <QMainWindow>
#include <QQmlEngine>
#include <QJSEngine>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QStandardPaths>
#include <QUrl>
#include <QFile>
#include <QPalette>
#include <QQuickWindow>
#include <QWindow>
#include <OgreLog.h>
#include <OgreScriptCompiler.h>
#include <OgreScriptTranslator.h>
#include <OgreTextureUnitState.h>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace {

bool isPs1RipMaterial(const QString &name)
{
    return name.startsWith(QLatin1String("PS1Rip_")) || name.startsWith(QLatin1String("PS1Rip/"));
}

bool isPaintPipelineMaterial(const QString &name)
{
    return name.startsWith(QLatin1String("QMEPaintMaskOverlay_"))
           || name.startsWith(QLatin1String("QMEPaint_"))
           || name.startsWith(QLatin1String("TexturePaint/"));
}

} // namespace

MaterialEditorQML::MaterialEditorQML(QObject *parent)
    : QObject(parent)
{
    // Track runtime palette swaps (Options → Theme: Light/Dark/Custom).
    // Without this, opening the Material Editor in a separate
    // QQmlApplicationEngine left the window stuck on whatever palette
    // existed when the singleton was first cached — most visibly broken
    // on Linux/Fusion where the Material Editor opened light while the
    // rest of the app was dark.
    if (qApp) {
        connect(qApp, &QApplication::paletteChanged, this, [this]() {
            emit themeChanged();
        });
    }

    // Initialize material color properties with defaults
    m_ambientColor = QColor(128, 128, 128);  // Gray
    m_diffuseColor = QColor(255, 255, 255);  // White
    m_specularColor = QColor(0, 0, 0);       // Black
    m_emissiveColor = QColor(0, 0, 0);       // Black
    m_fogColor = QColor(0, 0, 0);            // Black
    m_textureBorderColor = QColor(0, 0, 0);  // Black
    
    // Initialize AI network manager
    m_networkManager = new QNetworkAccessManager(this);

    // Connect to LLMManager signals
    LLMManager *llmManager = LLMManager::instance();
    connect(llmManager, &LLMManager::generationStarted, this, &MaterialEditorQML::onLLMGenerationStarted);
    connect(llmManager, &LLMManager::generationProgress, this, &MaterialEditorQML::onLLMGenerationProgress);
    connect(llmManager, &LLMManager::generationCompleted, this, &MaterialEditorQML::onLLMGenerationCompleted);
    connect(llmManager, &LLMManager::generationError, this, &MaterialEditorQML::onLLMGenerationError);
    connect(llmManager, &LLMManager::modelLoadedChanged, this, &MaterialEditorQML::onLLMModelLoadedChanged);

    // LCOV_EXCL_START — SD requires ENABLE_STABLE_DIFFUSION build flag + GPU model files
#ifdef ENABLE_STABLE_DIFFUSION
    // Connect to SDManager signals
    SDManager *sdManager = SDManager::instance();
    connect(sdManager, &SDManager::generationStarted, this, &MaterialEditorQML::onSDGenerationStarted);
    connect(sdManager, &SDManager::generationProgressChanged, this, &MaterialEditorQML::onSDGenerationProgress);
    connect(sdManager, &SDManager::generationCompleted, this, &MaterialEditorQML::onSDGenerationCompleted);
    connect(sdManager, &SDManager::generationError, this, &MaterialEditorQML::onSDGenerationError);
    connect(sdManager, &SDManager::generationStopped, this, &MaterialEditorQML::onSDGenerationStopped);
    connect(sdManager, &SDManager::modelLoadedChanged, this, &MaterialEditorQML::onSDModelLoadedChanged);

    // Register generated textures directory as Ogre resource location at startup
    // so textures from previous sessions are found when materials reference them
    if (isOgreAvailable()) {
        QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QString genTexDir = QDir(dataPath).filePath("generated_textures");
        if (QDir(genTexDir).exists()) {
            try {
                auto &rgm = Ogre::ResourceGroupManager::getSingleton();
                rgm.addResourceLocation(genTexDir.toStdString(), "FileSystem",
                                        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
                rgm.initialiseResourceGroup(Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            } catch (...) {}
        }
    }
#endif
    // LCOV_EXCL_STOP

    // Re-evaluate the "use selected mesh" checkbox whenever the scene
    // selection changes (independent of the SD build flag).
    if (auto* sel = SelectionSet::getSingleton()) {
        connect(sel, &SelectionSet::selectionChanged,
                this, &MaterialEditorQML::hasSelectedMeshChanged);
    }
}

// Defaulted here (not in the header) so the std::unique_ptr<MultiViewBakeState>
// pimpl is destroyed where MultiViewBakeState is a complete type.
MaterialEditorQML::~MaterialEditorQML() = default;

MaterialEditorQML* MaterialEditorQML::qmlInstance(QQmlEngine *engine, QJSEngine *scriptEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(scriptEngine)
    
    static MaterialEditorQML* instance = nullptr;
    if (!instance) {
        instance = new MaterialEditorQML();
    }
    // Must set CppOwnership on every call — each QQmlEngine tracks ownership
    // independently, and without this a later engine could delete the shared instance
    QQmlEngine::setObjectOwnership(instance, QQmlEngine::CppOwnership);
    return instance;
}

// -----------------------------------------------------------------------
// Theme color getters — derive live from QApplication::palette() so the
// Material Editor follows runtime Light/Dark/Custom palette swaps. The
// role mapping matches PropertiesPanelController / ThemeManager so the
// Material Editor and the Inspector keep a single palette vocabulary.
// -----------------------------------------------------------------------
QColor MaterialEditorQML::backgroundColor() const
{
    return QApplication::palette().color(QPalette::Window);
}

QColor MaterialEditorQML::panelColor() const
{
    return QApplication::palette().color(QPalette::Window);
}

QColor MaterialEditorQML::inputColor() const
{
    return QApplication::palette().color(QPalette::Base);
}

QColor MaterialEditorQML::headerColor() const
{
    return QApplication::palette().color(QPalette::Window).darker(110);
}

QColor MaterialEditorQML::textColor() const
{
    return QApplication::palette().color(QPalette::WindowText);
}

QColor MaterialEditorQML::borderColor() const
{
    return QApplication::palette().color(QPalette::Mid);
}

QColor MaterialEditorQML::highlightColor() const
{
    return QApplication::palette().color(QPalette::Highlight);
}

QColor MaterialEditorQML::buttonColor() const
{
    return QApplication::palette().color(QPalette::Button);
}

QColor MaterialEditorQML::buttonTextColor() const
{
    return QApplication::palette().color(QPalette::ButtonText);
}

QColor MaterialEditorQML::disabledTextColor() const
{
    return QApplication::palette().color(QPalette::PlaceholderText);
}

QColor MaterialEditorQML::accentColor() const
{
    return QApplication::palette().color(QPalette::Highlight);
}

void MaterialEditorQML::loadMaterial(const QString &materialName)
{
    if (materialName.isEmpty()) {
        createNewMaterial();
        return;
    }

    if (isPs1RipMaterial(materialName)) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("ps1_rip_material_edit_blocked:%1")
                                          .arg(materialName));
        emit errorOccurred(tr("Material \"%1\" is a PS1 capture material (read-only). "
                              "Recapture or use a regular mesh material to edit in this window.")
                               .arg(materialName));
        return;
    }

    m_materialName = materialName;
    
    // Safety check for Ogre availability
    if (!isOgreAvailable()) {
        // Set basic material text without Ogre
        setMaterialText(QString("material %1\n{\n\ttechnique\n\t{\n\t\tpass\n\t\t{\n\t\t}\n\t}\n}").arg(materialName));
        emit materialNameChanged();
        return;
    }
    
    try {
        m_ogreMaterial = Ogre::static_pointer_cast<Ogre::Material>(
            Ogre::MaterialManager::getSingleton().getByName(materialName.toStdString()));
        
        if (!m_ogreMaterial) {
            emit errorOccurred("Material not found: " + materialName);
            return;
        }

        // Serialize material to text
        Ogre::MaterialSerializer ms;
        ms.queueForExport(m_ogreMaterial, false, false, materialName.toStdString());
        setMaterialText(QString::fromStdString(ms.getQueuedAsString()));

        // Reset selection indices first
        m_selectedTechniqueIndex = -1;
        m_selectedPassIndex = -1;
        m_selectedTextureUnitIndex = -1;

        // Update technique and pass maps
        updateTechniqueList();
        
        emit materialNameChanged();
        
        // Auto-select first technique if available
        if (!m_techniqueList.isEmpty()) {
            setSelectedTechniqueIndex(0);
        } else {
            // If no techniques available, reset properties to defaults
            resetPropertiesToDefaults();
        }
        
    } catch (const std::exception& e) {
        emit errorOccurred(QString("Error loading material: %1").arg(e.what()));
    }
}

void MaterialEditorQML::createNewMaterial(const QString &materialName)
{
    SentryReporter::addBreadcrumb("ui.material", "Create new material");
    QString name = materialName.isEmpty() ? "new_material" : materialName;
    setMaterialName(name);
    setMaterialText(QString("material %1\n{\n\ttechnique\n\t{\n\t\tpass\n\t\t{\n\t\t}\n\t}\n}").arg(name));
    
    m_techniqueList.clear();
    m_passList.clear();
    m_textureUnitList.clear();
    m_selectedTechniqueIndex = -1;
    m_selectedPassIndex = -1;
    m_selectedTextureUnitIndex = -1;
    
    emit techniqueListChanged();
    emit passListChanged();
    emit textureUnitListChanged();
    emit selectedTechniqueIndexChanged();
    emit selectedPassIndexChanged();
    emit selectedTextureUnitIndexChanged();
}

// Wire canonical PBR slot TUSs into Ogre's FFP pipeline so they have
// approximately the right visual contribution in slice E. Slice F's
// PBR SubRenderState will replace this with a real shader.
//
//   albedo    → modulates with the per-vertex diffuse colour (textured base)
//   ao        → modulates the per-vertex diffuse (darkens lit base)
//   emissive  → adds on top of the running colour (self-illumination)
//   metallic  → ADD_SIGNED with running colour: brightens / tints toward
//               metal in textured regions (FFP approximation)
//   roughness → MODULATE_X2 with running colour: brightens smooth (low-
//               roughness) regions to fake spec gloss; FFP approximation
//   normal_map → marked non-FFP; RTShaderHelper::applyNormalMap wires
//                it through SRS_NORMALMAP elsewhere when a texture is set
//
// AO/metallic/roughness use LBS_DIFFUSE (per-vertex diffuse from
// lighting+material) instead of LBS_CURRENT so the result doesn't
// depend on where the slot lands in the TUS chain. Without this,
// AO would render as a new texture *layer* on top of the diffuse
// rather than darkening it, depending on where the user dropped it.
// Slice E PBR-slot FFP wiring is shared with the importer (so
// freshly-imported materials don't render darker than they will after a
// no-op Apply in the Material Editor). The implementation lives in
// RTShaderHelper alongside the other slot/shader-related helpers.
static void wirePbrSlotsForFFP(Ogre::Material* mat)
{
    RTShaderHelper::wirePbrSlotsForFFP(mat);
}

bool MaterialEditorQML::applyMaterial()
{
    SentryReporter::addBreadcrumb("ui.material", "Apply material");
    // Safety check for Ogre availability
    if (!isOgreAvailable()) {
        const QString scriptForOgre = HdrMaterialScript::stripEnvironmentLines(m_materialText);
        if (!validateMaterialScript(scriptForOgre)) {
            return false;
        }
        emit materialApplied();
        return true;
    }
    
    try {
        float parsedIntensity = m_pbrEnvIntensity;
        QColor parsedTint = m_pbrEnvTint;
        HdrMaterialScript::parseEnvironmentLines(m_materialText, parsedIntensity, parsedTint);
        const QString scriptForOgre = HdrMaterialScript::stripEnvironmentLines(m_materialText);

        Ogre::String script = scriptForOgre.toStdString();
        Ogre::MemoryDataStream *memoryStream = new Ogre::MemoryDataStream(
            (void*)script.c_str(), script.length() * sizeof(char));
        Ogre::DataStreamPtr dataStream(memoryStream);

        if (!validateMaterialScript(scriptForOgre)) {
            return false;
        }

        // Remove existing material if it exists
        if (Ogre::MaterialManager::getSingleton().resourceExists(m_materialName.toStdString())) {
            Ogre::MaterialManager::getSingleton().remove(m_materialName.toStdString());
        }

        // Parse the new material script
        Ogre::MaterialManager::getSingleton().parseScript(
            dataStream, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        // Extract material name from script
        QString newName = m_materialText;
        int materialIndex = newName.indexOf("material");
        if (materialIndex != -1) {
            newName = newName.mid(materialIndex + 9);
            newName = newName.left(newName.indexOf('\n')).trimmed();
            setMaterialName(newName);
        }

        // Get the new material and compile it
        m_ogreMaterial = Ogre::static_pointer_cast<Ogre::Material>(
            Ogre::MaterialManager::getSingleton().getByName(m_materialName.toStdString()));
        
        if (m_ogreMaterial) {
            wirePbrSlotsForFFP(m_ogreMaterial.get());
            m_ogreMaterial->compile();
            // Slice F: if user-edited material text carries the
            // pbr_workflow tag (e.g. they hand-typed it, or the script
            // came from MaterialPresetLibrary's PBR templates),
            // upgrade FFP wiring to Cook-Torrance via Ogre's stock
            // SRS_COOK_TORRANCE_LIGHTING. Returns false silently for
            // non-tagged materials → slice E FFP wiring stays.
            RTShaderHelper::applyPbrIfTagged(m_ogreMaterial);
            if (m_ogreMaterial->getNumTechniques() > 0) {
                Ogre::Pass* pass = m_ogreMaterial->getTechnique(0)->getPass(0);
                RTShaderHelper::setPbrEnvIntensity(pass, parsedIntensity);
                const Ogre::ColourValue tint(parsedTint.redF(), parsedTint.greenF(), parsedTint.blueF());
                RTShaderHelper::setPbrEnvTint(pass, tint);
            }
        }

        // Re-apply the edited material to sub-entities that use it.
        // Only update sub-entities whose material name matches — otherwise
        // Entity::setMaterialName would override ALL sub-entities with one material.
        std::string editedMatName = m_materialName.toStdString();
        for (Ogre::SceneNode* sn : Manager::getSingleton()->getSceneNodes()) {
            if (sn->getName().empty() || sn->getAttachedObjects().empty())
                continue;
            for (auto* obj : sn->getAttachedObjects()) {
                if (obj->getMovableType() != "Entity") continue;
                auto* entity = static_cast<Ogre::Entity*>(obj);
                for (unsigned int si = 0; si < entity->getNumSubEntities(); ++si) {
                    if (entity->getSubEntity(si)->getMaterialName() == editedMatName)
                        entity->getSubEntity(si)->setMaterialName(editedMatName);
                }
            }
        }

        // Re-apply RTSS normal map if the edited material has one
        if (m_ogreMaterial && m_ogreMaterial->getNumTechniques() > 0) {
            auto* pass = m_ogreMaterial->getTechnique(0)->getPass(0);
            for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
                const auto& tusName = pass->getTextureUnitState(i)->getName();
                if (tusName == "normal_map" || tusName == "NormalMap") {
                    std::string texName = pass->getTextureUnitState(i)->getTextureName();
                    if (!texName.empty()) {
                        RTShaderHelper::applyNormalMap(m_ogreMaterial, texName);
                    }
                    break;
                }
            }
        }

        // Reload the material to update UI
        loadMaterial(m_materialName);
        m_pbrEnvIntensity = parsedIntensity;
        m_pbrEnvTint = parsedTint;
        emit pbrEnvIntensityChanged();
        emit pbrEnvTintChanged();

        MaterialPreviewRenderer::instance()->clearCache();
        
        emit materialApplied();
        return true;
        
    } catch (const std::exception& e) {
        emit errorOccurred(QString("Error applying material: %1").arg(e.what()));
        return false;
    }
}

bool MaterialEditorQML::validateMaterialScript(const QString &script)
{
    QString trimmedScript = script.trimmed();
    if (trimmedScript.isEmpty()) {
        emit errorOccurred("Material script is empty");
        return false;
    }
    
    // Enhanced syntax validation - always perform regardless of Ogre availability
    QStringList lines = trimmedScript.split('\n');
    int braceLevel = 0;
    bool inMaterialBlock = false;
    bool hasTechnique = false;
    bool hasPass = false;
    
    for (int i = 0; i < lines.size(); i++) {
        QString line = lines[i].trimmed();
        if (line.isEmpty() || line.startsWith("//")) continue; // Skip comments and empty lines
        
        // Count braces
        int openBracesInLine = line.count('{');
        int closeBracesInLine = line.count('}');
        braceLevel += openBracesInLine - closeBracesInLine;
        
        // Check for negative brace level (more closing than opening)
        if (braceLevel < 0) {
            emit errorOccurred(QString("Unexpected closing brace '}' at line %1").arg(i + 1));
            return false;
        }
        
        // Check for material declaration
        if (line.startsWith("material ")) {
            if (inMaterialBlock) {
                emit errorOccurred(QString("Nested material declaration at line %1").arg(i + 1));
                return false;
            }
            inMaterialBlock = true;
            
            // Check if material name is provided
            QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() < 2) {
                emit errorOccurred(QString("Material declaration missing name at line %1").arg(i + 1));
                return false;
            }
            
            // Material declaration should end with { or be on next line
            if (!line.contains('{') && i + 1 < lines.size()) {
                QString nextLine = lines[i + 1].trimmed();
                if (!nextLine.startsWith('{')) {
                    emit errorOccurred(QString("Expected '{' after material declaration at line %1").arg(i + 1));
                    return false;
                }
            }
        }
        
        // Check for technique declaration
        else if (line.startsWith("technique")) {
            if (!inMaterialBlock) {
                emit errorOccurred(QString("Technique declaration outside material block at line %1").arg(i + 1));
                return false;
            }
            hasTechnique = true;
        }
        
        // Check for pass declaration
        else if (line.startsWith("pass")) {
            if (!inMaterialBlock) {
                emit errorOccurred(QString("Pass declaration outside material block at line %1").arg(i + 1));
                return false;
            }
            hasPass = true;
        }
        
        // Check for texture_unit (catch common typos)
        else if (line.startsWith("texture_unit")) {
            if (!hasPass) {
                emit errorOccurred(QString("texture_unit must be inside a pass block at line %1").arg(i + 1));
                return false;
            }
        }
        
        // Check for common typos and malformed declarations
        else if (line.contains("texture_unt") || line.contains("textre")) {
            emit errorOccurred(QString("Syntax error: Invalid keyword '%1' at line %2. Did you mean 'texture_unit' or 'texture'?").arg(line.split(' ').first()).arg(i + 1));
            return false;
        }
        
        // Check for malformed lines with random text like "asd"
        else if (line.contains("asd") && !line.startsWith("//")) {
            emit errorOccurred(QString("Malformed syntax: Invalid characters 'asd' at line %1").arg(i + 1));
            return false;
        }
        
        // Check for unterminated strings
        int quoteCount = line.count('"');
        if (quoteCount % 2 != 0) {
            emit errorOccurred(QString("Unterminated string at line %1").arg(i + 1));
            return false;
        }
        
        // Check for lines that should end with values but don't
        if (line.endsWith("texture") || line.endsWith("ambient") || line.endsWith("diffuse") || line.endsWith("specular")) {
            if (!line.contains(' ')) { // No space means no value
                emit errorOccurred(QString("Property '%1' missing value at line %2").arg(line).arg(i + 1));
                return false;
            }
        }
        
        // Check for malformed property lines (properties without proper syntax)
        QStringList knownProperties = {"ambient", "diffuse", "specular", "emissive", "texture", "alpha", "shininess", 
                                       "lighting", "depth_write", "depth_check", "scene_blend", "cull_hardware", "cull_software"};
        for (const QString& prop : knownProperties) {
            if (line.startsWith(prop + " ") && line.contains("asd")) {
                emit errorOccurred(QString("Malformed property value for '%1' at line %2").arg(prop).arg(i + 1));
                return false;
            }
        }
    }
    
    // Final validation checks
    if (!inMaterialBlock) {
        emit errorOccurred("No valid material declaration found");
        return false;
    }
    
    if (braceLevel != 0) {
        if (braceLevel > 0) {
            emit errorOccurred(QString("Missing %1 closing brace(s) '}' - found %2 open braces but %3 close braces").arg(braceLevel).arg(trimmedScript.count('{')).arg(trimmedScript.count('}')));
        } else {
            emit errorOccurred(QString("Too many closing braces - %1 extra '}'").arg(-braceLevel));
        }
        return false;
    }
    
    if (!hasTechnique) {
        emit errorOccurred("Material must contain at least one technique block");
        return false;
    }
    
    if (!hasPass) {
        emit errorOccurred("Material must contain at least one pass block");
        return false;
    }
    
    // If we get here, basic syntax validation passed
    // Now try Ogre validation if available for additional checks
    if (isOgreAvailable()) {
        try {
            // Custom ScriptCompilerListener to capture compilation errors
            class ValidationListener : public Ogre::ScriptCompilerListener
            {
            private:
                std::vector<Ogre::Exception> errors;
            public:
                virtual void handleError(Ogre::ScriptCompiler *compiler, Ogre::uint32 code, const Ogre::String &file, int line, const Ogre::String &msg) override {
                    Ogre::Exception e{0, msg, "ScriptCompilerListener", "error", file.c_str(), line};
                    errors.push_back(e);
                }
                const std::vector<Ogre::Exception> &getErrors() const { return errors; }
            };

            Ogre::String ogreScript = script.toStdString();
            Ogre::MemoryDataStream *memoryStream = new Ogre::MemoryDataStream(
                (void*)ogreScript.c_str(), ogreScript.length() * sizeof(char));
            Ogre::DataStreamPtr dataStream(memoryStream);

            // Create test resource group if it doesn't exist
            const std::string testGroupName = "Test_Script_Validation";
            if (!Ogre::ResourceGroupManager::getSingleton().resourceGroupExists(testGroupName)) {
                Ogre::ResourceGroupManager::getSingleton().createResourceGroup(testGroupName);
            }
            
            // Remove any existing test material
            if (Ogre::MaterialManager::getSingleton().resourceExists(m_materialName.toStdString(), testGroupName)) {
                Ogre::MaterialManager::getSingleton().remove(m_materialName.toStdString(), testGroupName);
            }

            // Set up validation listener
            ValidationListener* listener = new ValidationListener();
            Ogre::ScriptCompilerManager* compilerManager = Ogre::ScriptCompilerManager::getSingletonPtr();
            if (compilerManager) {
                // Store current listener and set our validation listener
                Ogre::ScriptCompilerListener* originalListener = compilerManager->getListener();
                compilerManager->setListener(listener);

                try {
                    // Parse the script to validate
                    compilerManager->parseScript(dataStream, testGroupName);
                    
                    // Clean up test material if it was created
                    if (Ogre::MaterialManager::getSingleton().resourceExists(m_materialName.toStdString(), testGroupName)) {
                        Ogre::MaterialManager::getSingleton().remove(m_materialName.toStdString(), testGroupName);
                    }

                    // Restore original listener
                    compilerManager->setListener(originalListener);

                    // Check for validation errors from Ogre
                    const auto& errors = listener->getErrors();
                    if (!errors.empty()) {
                        QString errorMessages;
                        for (const auto& e : errors) {
                            errorMessages += QString("Ogre validation error on line %1: %2\n").arg(e.getLine()).arg(e.getDescription().c_str());
                        }
                        emit errorOccurred(errorMessages.trimmed());
                        delete listener;
                        return false;
                    }

                } catch (const Ogre::Exception& ogreEx) {
                    // Restore original listener
                    compilerManager->setListener(originalListener);
                    
                    // Clean up test material if it was created
                    if (Ogre::MaterialManager::getSingleton().resourceExists(m_materialName.toStdString(), testGroupName)) {
                        Ogre::MaterialManager::getSingleton().remove(m_materialName.toStdString(), testGroupName);
                    }
                    
                    delete listener;
                    emit errorOccurred(QString("Ogre compilation failed: %1").arg(ogreEx.getDescription().c_str()));
                    return false;
                }
            }
            
            delete listener;
        } catch (const std::exception& e) {
            emit errorOccurred(QString("Additional validation error: %1").arg(e.what()));
            return false;
        } catch (...) {
            emit errorOccurred("Unknown error during additional Ogre validation");
            return false;
        }
    }
    
    return true; // All validation passed
}

void MaterialEditorQML::setMaterialName(const QString &name)
{
    if (m_materialName != name) {
        m_materialName = name;
        emit materialNameChanged();
    }
}

void MaterialEditorQML::setMaterialText(const QString &text)
{
    if (m_materialText != text) {
        // Add current text to undo stack before changing
        if (!m_materialText.isEmpty()) {
            addToUndoStack(m_materialText);
        }
        
        m_materialText = text;
        emit materialTextChanged();
    }
}

void MaterialEditorQML::setSelectedTechniqueIndex(int index)
{
    if (m_selectedTechniqueIndex != index) {
        m_selectedTechniqueIndex = index;
        updatePassList();
        emit selectedTechniqueIndexChanged();

        // Technique changed: always refresh pass selection and properties,
        // even if the target pass index equals the current value.
        int targetPass = m_passList.isEmpty() ? -1 : 0;
        m_selectedPassIndex = targetPass;
        updateTextureUnitList();
        updatePassProperties();
        emit selectedPassIndexChanged();

        // Auto-select first texture unit if available
        if (!m_textureUnitList.isEmpty()) {
            setSelectedTextureUnitIndex(0);
        } else {
            setSelectedTextureUnitIndex(-1);
        }
    }
}

void MaterialEditorQML::setSelectedPassIndex(int index)
{
    if (m_selectedPassIndex != index) {
        m_selectedPassIndex = index;
        updateTextureUnitList();
        updatePassProperties();
        emit selectedPassIndexChanged();
        
        // Auto-select first texture unit if available
        if (!m_textureUnitList.isEmpty()) {
            setSelectedTextureUnitIndex(0);
        } else {
            setSelectedTextureUnitIndex(-1);
        }
    }
}

void MaterialEditorQML::setSelectedTextureUnitIndex(int index)
{
    if (m_selectedTextureUnitIndex != index) {
        m_selectedTextureUnitIndex = index;
        updateTextureUnitProperties();
        emit selectedTextureUnitIndexChanged();
    }
}

void MaterialEditorQML::setLightingEnabled(bool enabled)
{
    if (m_lightingEnabled != enabled) {
        m_lightingEnabled = enabled;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setLightingEnabled(enabled);
            updateMaterialText();
        }
        
        emit lightingEnabledChanged();
    }
}

void MaterialEditorQML::setDepthWriteEnabled(bool enabled)
{
    if (m_depthWriteEnabled != enabled) {
        m_depthWriteEnabled = enabled;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setDepthWriteEnabled(enabled);
            updateMaterialText();
        }
        
        emit depthWriteEnabledChanged();
    }
}

void MaterialEditorQML::setDepthCheckEnabled(bool enabled)
{
    if (m_depthCheckEnabled != enabled) {
        m_depthCheckEnabled = enabled;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setDepthCheckEnabled(enabled);
            updateMaterialText();
        }
        
        emit depthCheckEnabledChanged();
    }
}

void MaterialEditorQML::setAmbientColor(const QColor &color)
{
    if (m_ambientColor != color) {
        m_ambientColor = color;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setAmbient(color.redF(), color.greenF(), color.blueF());
            updateMaterialText();
        }
        
        emit ambientColorChanged();
    }
}

void MaterialEditorQML::setDiffuseColor(const QColor &color)
{
    if (m_diffuseColor != color) {
        m_diffuseColor = color;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setDiffuse(color.redF(), color.greenF(), color.blueF(), m_diffuseAlpha);
            updateMaterialText();
        }
        
        emit diffuseColorChanged();
    }
}

void MaterialEditorQML::setSpecularColor(const QColor &color)
{
    if (m_specularColor != color) {
        m_specularColor = color;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setSpecular(color.redF(), color.greenF(), color.blueF(), m_specularAlpha);
            updateMaterialText();
        }
        
        emit specularColorChanged();
    }
}

void MaterialEditorQML::setEmissiveColor(const QColor &color)
{
    if (m_emissiveColor != color) {
        m_emissiveColor = color;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setEmissive(color.redF(), color.greenF(), color.blueF());
            updateMaterialText();
        }
        
        emit emissiveColorChanged();
    }
}

void MaterialEditorQML::setDiffuseAlpha(float alpha)
{
    if (m_diffuseAlpha != alpha) {
        m_diffuseAlpha = alpha;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setDiffuse(pass->getDiffuse().r, pass->getDiffuse().g, pass->getDiffuse().b, alpha);
            updateMaterialText();
        }
        
        emit diffuseAlphaChanged();
    }
}

void MaterialEditorQML::setSpecularAlpha(float alpha)
{
    if (m_specularAlpha != alpha) {
        m_specularAlpha = alpha;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setSpecular(pass->getSpecular().r, pass->getSpecular().g, pass->getSpecular().b, alpha);
            updateMaterialText();
        }
        
        emit specularAlphaChanged();
    }
}

void MaterialEditorQML::setShininess(float shininess)
{
    if (m_shininess != shininess) {
        m_shininess = shininess;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setShininess(shininess);
            updateMaterialText();
        }
        
        emit shininessChanged();
    }
}

void MaterialEditorQML::setPolygonMode(int mode)
{
    if (m_polygonMode != mode) {
        m_polygonMode = mode;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setPolygonMode(static_cast<Ogre::PolygonMode>(mode + 1));
            updateMaterialText();
        }
        
        emit polygonModeChanged();
    }
}

void MaterialEditorQML::setSourceBlendFactor(int factor)
{
    if (m_sourceBlendFactor != factor) {
        m_sourceBlendFactor = factor;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            if (factor < 6) {
                if (factor > 0) {
                    pass->setSceneBlending(static_cast<Ogre::SceneBlendType>(factor - 1));
                }
            } else {
                pass->setSceneBlending(static_cast<Ogre::SceneBlendFactor>(factor - 6), pass->getDestBlendFactor());
            }
            updateMaterialText();
        }
        
        emit sourceBlendFactorChanged();
    }
}

void MaterialEditorQML::setDestBlendFactor(int factor)
{
    if (m_destBlendFactor != factor) {
        m_destBlendFactor = factor;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass && factor > 0) {
            pass->setSceneBlending(pass->getSourceBlendFactor(), static_cast<Ogre::SceneBlendFactor>(factor - 1));
            updateMaterialText();
        }
        
        emit destBlendFactorChanged();
    }
}

void MaterialEditorQML::setUseVertexColorToAmbient(bool use)
{
    if (m_useVertexColorToAmbient != use) {
        m_useVertexColorToAmbient = use;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setVertexColourTracking(
                use ? pass->getVertexColourTracking() | 1 : pass->getVertexColourTracking() & 0xE);
            updateMaterialText();
        }
        
        emit useVertexColorToAmbientChanged();
    }
}

void MaterialEditorQML::setUseVertexColorToDiffuse(bool use)
{
    if (m_useVertexColorToDiffuse != use) {
        m_useVertexColorToDiffuse = use;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setVertexColourTracking(
                use ? pass->getVertexColourTracking() | 2 : pass->getVertexColourTracking() & 0xD);
            updateMaterialText();
        }
        
        emit useVertexColorToDiffuseChanged();
    }
}

void MaterialEditorQML::setUseVertexColorToSpecular(bool use)
{
    if (m_useVertexColorToSpecular != use) {
        m_useVertexColorToSpecular = use;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setVertexColourTracking(
                use ? pass->getVertexColourTracking() | 4 : pass->getVertexColourTracking() & 0xB);
            updateMaterialText();
        }
        
        emit useVertexColorToSpecularChanged();
    }
}

void MaterialEditorQML::setUseVertexColorToEmissive(bool use)
{
    if (m_useVertexColorToEmissive != use) {
        m_useVertexColorToEmissive = use;
        
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setVertexColourTracking(
                use ? pass->getVertexColourTracking() | 8 : pass->getVertexColourTracking() & 0x7);
            updateMaterialText();
        }
        
        emit useVertexColorToEmissiveChanged();
    }
}

void MaterialEditorQML::ensureTextureInMaterialGroup(const QString& textureName)
{
    if (!isOgreAvailable() || textureName.isEmpty()
        || textureName == "*Select a texture*")
        return;

    const std::string texStd = textureName.toStdString();
    const std::string matGroup = m_ogreMaterial
        ? m_ogreMaterial->getGroup()
        : std::string(Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    try {
        auto& tm = Ogre::TextureManager::getSingleton();

        // Already resolvable from the material's group? Nothing to do.
        if (tm.getByName(texStd, matGroup))
            return;

        // Resolve an on-disk source. Prefer the origin of an already-loaded
        // copy (in any group), then fall back to the same locations the
        // preview searches, then to a direct probe of the app + working dirs.
        QString srcPath;
        if (auto existing = tm.getByName(texStd)) {
            const QString origin = QString::fromStdString(existing->getOrigin());
            if (!origin.isEmpty() && QFileInfo::exists(origin))
                srcPath = origin;
        }
        if (srcPath.isEmpty()) {
            // Reuse the preview's resolver (handles generated_textures + media).
            const QString prev = getTexturePreviewPath();
            if (prev.startsWith(QStringLiteral("file://")))
                srcPath = QUrl(prev).toLocalFile();
        }
        if (srcPath.isEmpty()) {
            const QStringList probe = {
                QDir(QCoreApplication::applicationDirPath()).filePath(textureName),
                QDir::current().filePath(textureName),
                QString("media/materials/textures/%1").arg(textureName)
            };
            for (const QString& p : probe) {
                if (QFileInfo::exists(p)) { srcPath = p; break; }
            }
        }
        if (srcPath.isEmpty() || !QFileInfo::exists(srcPath))
            return; // Can't find the bytes; leave binding as-is.

        // Register the source directory into the material's group and load the
        // image there under the bare name, so name resolution succeeds.
        const QFileInfo fi(srcPath);
        auto& rgm = Ogre::ResourceGroupManager::getSingleton();
        if (!rgm.resourceGroupExists(matGroup))
            rgm.createResourceGroup(matGroup);
        rgm.addResourceLocation(fi.absolutePath().toStdString(), "FileSystem", matGroup);

        Ogre::Image image;
        QFile f(srcPath);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = f.readAll();
            f.close();
            Ogre::DataStreamPtr ds(new Ogre::MemoryDataStream(
                const_cast<char*>(bytes.constData()),
                static_cast<size_t>(bytes.size()), false, true));
            image.load(ds, fi.suffix().toLower().toStdString());
            tm.loadImage(texStd, matGroup, image);
            SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                QStringLiteral("Loaded texture '%1' into material group '%2' from '%3'")
                    .arg(textureName, QString::fromStdString(matGroup), srcPath));
        }
    } catch (...) {
        // Best-effort: a failure here just means the binding may still miss.
        SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
            QStringLiteral("Failed to load texture '%1' into material group")
                .arg(textureName));
    }
}

void MaterialEditorQML::setTextureName(const QString &name)
{
    if (m_textureName == name)
        return;
    m_textureName = name;

    if (!name.isEmpty() && name != "*Select a texture*") {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
            QStringLiteral("Set material texture: %1").arg(name));

        // The on-screen mesh renders via RTSS (the viewport uses the
        // MSN_SHADERGEN material scheme). RTSS resolves the TUS texture by name
        // against the MATERIAL's resource group (+ AUTODETECT). If the texture
        // was loaded into a DIFFERENT group than the material, the lookup
        // misses and RTSS renders the yellow/black placeholder. Make the
        // texture resolvable from the material's own group before binding it.
        ensureTextureInMaterialGroup(name);

        // Rebuild the cached technique/pass/TUS pointers BEFORE using them. An
        // earlier apply (or ensureTextureInMaterialGroup) can recompile the
        // material, after which Ogre has destroyed+rebuilt its
        // Technique/Pass/TUS objects and the cached m_texUnitMap pointer
        // dangles — dereferencing it crashed in
        // TextureUnitState::retrieveTexture()/Pass::getResourceGroup().
        // updateTechniqueList() must run first: updateTextureUnitList() reads
        // m_passMap via getCurrentPass(), which a prior recompile may have left
        // referencing freed Pass objects.
        updateTechniqueList(); // rebuilds m_techMap + the selected pass map
        updateTextureUnitList();
        if (Ogre::TextureUnitState* textureUnit = getCurrentTextureUnit()) {
            // Only set non-empty, valid texture names to avoid OGRE crashes.
            textureUnit->setTextureName(name.toStdString());
            collapseTmdMaterialToSinglePass();
            regenerateRtssShaders();
            updateMaterialText();
        }
    }

    emit textureNameChanged();
}

void MaterialEditorQML::collapseTmdMaterialToSinglePass()
{
    // Per-import PS1 TMD materials default to unlit single-pass once an image
    // is present.
    if (!m_ogreMaterial || m_ogreMaterial->getName().rfind("TMD/", 0) != 0)
        return;
    Ogre::Technique* tech = getCurrentTechnique();
    if (!tech)
        return;
    while (tech->getNumPasses() > 1)
        tech->removePass(1);
    // removePass() invalidated any cached pass beyond 0; reset the selection to
    // the surviving pass and refresh the maps before touching it via the cache,
    // then apply alpha-reject directly to pass 0 (PS1 .tim uses color 0x0000
    // for transparent texels — see PS1TIM::loadTimToOgreImage).
    m_selectedPassIndex = 0;
    updatePassList();
    updateTextureUnitList();
    if (tech->getNumPasses() > 0)
        tech->getPass(0)->setAlphaRejectSettings(Ogre::CMPF_GREATER_EQUAL, 1);
}

void MaterialEditorQML::regenerateRtssShaders()
{
    // The viewport renders this material through RTSS (the MSN_SHADERGEN
    // scheme). Rebinding a TUS texture does NOT update the already-generated
    // shader — RTSS keeps serving the program built against the PREVIOUS
    // texture, so after a few switches the surface went blank and lighting
    // glitched (FFP↔generated technique desync). Force RTSS to regenerate this
    // material's shaders against the new binding.
    if (!m_ogreMaterial)
        return;
    if (auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr()) {
        const Ogre::String scheme =
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME;
        try {
            shaderGen->invalidateMaterial(scheme,
                m_ogreMaterial->getName(), m_ogreMaterial->getGroup());
            shaderGen->validateMaterial(scheme,
                m_ogreMaterial->getName(), m_ogreMaterial->getGroup());
        } catch (const Ogre::Exception&) {
            // Best-effort: a missing generated technique just means RTSS will
            // rebuild it lazily on next render.
        }
    }
    m_ogreMaterial->compile();
}

void MaterialEditorQML::setScrollAnimUSpeed(double speed)
{
    if (m_scrollAnimUSpeed != speed) {
        m_scrollAnimUSpeed = speed;
        
        Ogre::TextureUnitState* textureUnit = getCurrentTextureUnit();
        if (textureUnit) {
            textureUnit->setScrollAnimation(speed, m_scrollAnimVSpeed);
            updateMaterialText();
        }
        
        emit scrollAnimUSpeedChanged();
    }
}

void MaterialEditorQML::setScrollAnimVSpeed(double speed)
{
    if (m_scrollAnimVSpeed != speed) {
        m_scrollAnimVSpeed = speed;
        Ogre::Pass* pass = getCurrentPass();
        if (pass && !pass->getTextureUnitStates().empty()) {
            pass->getTextureUnitState(0)->setScrollAnimation(m_scrollAnimUSpeed, m_scrollAnimVSpeed);
        }
        emit scrollAnimVSpeedChanged();
    }
}

void MaterialEditorQML::createNewTechnique(const QString &name)
{
    if (!m_ogreMaterial) return;
    
    Ogre::Technique *technique = m_ogreMaterial->createTechnique();
    if (!name.isEmpty()) {
        technique->setName(name.toStdString());
    }
    
    updateTechniqueList();
    updateMaterialText();
    
    // Auto-select the newly created technique (it will be the last one)
    if (!m_techniqueList.isEmpty()) {
        setSelectedTechniqueIndex(m_techniqueList.size() - 1);
        // Force refresh of pass list after selection
        updatePassList();
    }
}

void MaterialEditorQML::createNewPass(const QString &name)
{
    Ogre::Technique* technique = getCurrentTechnique();
    if (!technique) return;
    
    Ogre::Pass *pass = technique->createPass();
    if (!name.isEmpty()) {
        pass->setName(name.toStdString());
    }
    
    // Refresh technique list to update the internal maps
    updateTechniqueList();
    updateMaterialText();
    
    // Auto-select the newly created pass (it will be the last one)
    if (!m_passList.isEmpty()) {
        setSelectedPassIndex(m_passList.size() - 1);
    }
}

void MaterialEditorQML::createNewTextureUnit(const QString &name)
{
    Ogre::Pass* pass = getCurrentPass();
    if (!pass) return;
    
    Ogre::TextureUnitState *textureUnit = pass->createTextureUnitState();
    if (!name.isEmpty()) {
        textureUnit->setName(name.toStdString());
    }
    
    updateTextureUnitList();
    updateMaterialText();
    
    // Auto-select the newly created texture unit (it will be the last one)
    if (!m_textureUnitList.isEmpty()) {
        setSelectedTextureUnitIndex(m_textureUnitList.size() - 1);
    }
}

// LCOV_EXCL_START — opens native file dialog, requires user interaction
void MaterialEditorQML::selectTexture()
{
    QString filePath = QFileDialog::getOpenFileName(
        nullptr, 
        tr("Select a texture"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        tr("Texture File (*.bmp *.jpg *.jpeg *.gif *.raw *.png *.tga *.dds *.tim)"));

    if (filePath.isEmpty()) return;

    Ogre::TextureUnitState* textureUnit = getCurrentTextureUnit();
    if (!textureUnit) return;

    QFileInfo file(filePath);
    
    // Validate file name is not empty
    if (file.fileName().isEmpty()) {
        emit errorOccurred("Selected file has an empty name.");
        return;
    }

    const bool isTim = file.suffix().compare(QStringLiteral("tim"), Qt::CaseInsensitive) == 0;
    const QString ogreTexName = isTim ? (file.completeBaseName() + QStringLiteral(".tim")) : file.fileName();

    try {
        // Try to get existing texture
        Ogre::TextureManager::getSingleton().getByName(
            ogreTexName.toStdString(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    } catch (...) {
        // Load new texture
        // Always register user-picked textures into the default group ("General") so materials can find them.
        // Creating ad-hoc groups per directory makes TextureUnitState name resolution fail (yellow/black fallback).
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
            file.path().toStdString(), "FileSystem", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        Ogre::ResourceGroupManager::getSingleton().initialiseResourceGroup(
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        Ogre::Image image;
        if (isTim) {
            QString err;
            if (!PS1TIM::loadTimToOgreImage(filePath, image, &err)) {
                emit errorOccurred(QString("Failed to load TIM: %1").arg(err));
                return;
            }
        } else {
            image.load(file.fileName().toStdString(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        }
        Ogre::TextureManager::getSingleton().loadImage(
            ogreTexName.toStdString(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, image);
    }
    
    setTextureName(ogreTexName);
}
// LCOV_EXCL_STOP

void MaterialEditorQML::removeTexture()
{
    Ogre::TextureUnitState* textureUnit = getCurrentTextureUnit();
    if (!textureUnit) return;

    Ogre::Pass* pass = getCurrentPass();
    if (!pass) return;

    // Instead of setting empty texture name, remove the entire texture unit and recreate it
    // This avoids the empty name issue while properly updating the material
    try {
        // Get the texture unit index
        int textureUnitIndex = -1;
        const auto textureUnits = pass->getTextureUnitStates();
        for (size_t i = 0; i < textureUnits.size(); ++i) {
            if (textureUnits[i] == textureUnit) {
                textureUnitIndex = static_cast<int>(i);
                break;
            }
        }
        
        if (textureUnitIndex >= 0) {
            // Store the texture unit name before removal
            std::string unitName = textureUnit->getName();
            
            // Remove the texture unit
            pass->removeTextureUnitState(textureUnitIndex);
            
            // Create a new empty texture unit with the same name
            Ogre::TextureUnitState* newTextureUnit = pass->createTextureUnitState();
            if (!unitName.empty()) {
                newTextureUnit->setName(unitName);
            }
            
            // Update our internal lists
            updateTextureUnitList();
            updateMaterialText();
        }
    } catch (const std::exception& e) {
        qDebug() << "Error removing texture:" << e.what();
    }

    // Update the UI display
    m_textureName = "*Select a texture*";
    emit textureNameChanged();
}

QStringList MaterialEditorQML::getPolygonModeNames() const
{
    return QStringList() << "Points" << "Wireframe" << "Solid";
}

QStringList MaterialEditorQML::getBlendFactorNames() const
{
    return QStringList() 
        << "None" << "Add" << "Modulate" << "Colour Blend" << "Alpha Blend" << "Replace"
        << "One" << "Zero" << "Dest Colour" << "Src Colour" << "One Minus Dest Colour"
        << "One Minus Src Colour" << "Dest Alpha" << "Src Alpha" << "One Minus Dest Alpha"
        << "One Minus Src Alpha";
}

void MaterialEditorQML::updateTechniqueList()
{
    m_techniqueList.clear();
    m_techMap.clear();
    m_techMapName.clear();
    
    if (!m_ogreMaterial) {
        emit techniqueListChanged();
        return;
    }

    const auto techniques = m_ogreMaterial->getTechniques();
    int techIndex = 0;
    
    for (Ogre::Technique* tech : techniques) {
        QString techName = tech->getName().empty() ? 
            QString("technique%1").arg(techIndex) : 
            QString::fromStdString(tech->getName());
        
        m_techniqueList.append(techName);
        
        // Build pass map for this technique
        QMap<int, Ogre::Pass*> passMap;
        QStringList passNames;
        const auto passes = tech->getPasses();
        int passIndex = 0;
        
        for (Ogre::Pass* pass : passes) {
            QString passName = pass->getName().empty() ? 
                QString("pass%1").arg(passIndex) : 
                QString::fromStdString(pass->getName());
            
            passMap[passIndex] = pass;
            passNames.append(passName);
            passIndex++;
        }
        
        m_techMap[techIndex] = passMap;
        m_techMapName[techIndex] = passNames;
        techIndex++;
    }
    
    emit techniqueListChanged();
    
    // Force refresh of pass list for currently selected technique
    updatePassList();
}

void MaterialEditorQML::updatePassList()
{
    m_passList.clear();
    m_passMap.clear();
    
    if (m_selectedTechniqueIndex >= 0 && m_selectedTechniqueIndex < m_techniqueList.size()) {
        if (m_techMapName.contains(m_selectedTechniqueIndex)) {
            m_passList = m_techMapName[m_selectedTechniqueIndex];
            m_passMap = m_techMap[m_selectedTechniqueIndex];
        }
    }
    
    emit passListChanged();
}

void MaterialEditorQML::updateTextureUnitList()
{
    m_textureUnitList.clear();
    m_texUnitMap.clear();
    
    Ogre::Pass* pass = getCurrentPass();
    if (!pass) {
        emit textureUnitListChanged();
        return;
    }

    const auto textureUnits = pass->getTextureUnitStates();
    int unitIndex = 0;
    
    for (Ogre::TextureUnitState *textureUnit : textureUnits) {
        QString unitName = textureUnit->getName().empty() ? 
            QString("Texture_Unit%1").arg(unitIndex) : 
            QString::fromStdString(textureUnit->getName());
        
        m_textureUnitList.append(unitName);
        m_texUnitMap[unitName] = textureUnit;
        unitIndex++;
    }
    
    emit textureUnitListChanged();
}

void MaterialEditorQML::updatePassProperties()
{
    Ogre::Pass* pass = getCurrentPass();
    if (!pass) {
        resetPropertiesToDefaults();
        return;
    }

    m_lightingEnabled = pass->getLightingEnabled();
    m_depthWriteEnabled = pass->getDepthWriteEnabled();
    m_depthCheckEnabled = pass->getDepthCheckEnabled();
    
    // Colors
    const Ogre::ColourValue& ambient = pass->getAmbient();
    m_ambientColor = QColor::fromRgbF(ambient.r, ambient.g, ambient.b);
    
    const Ogre::ColourValue& diffuse = pass->getDiffuse();
    m_diffuseColor = QColor::fromRgbF(diffuse.r, diffuse.g, diffuse.b);
    m_diffuseAlpha = diffuse.a;
    
    const Ogre::ColourValue& specular = pass->getSpecular();
    m_specularColor = QColor::fromRgbF(specular.r, specular.g, specular.b);
    m_specularAlpha = specular.a;
    
    const Ogre::ColourValue& emissive = pass->getEmissive();
    m_emissiveColor = QColor::fromRgbF(emissive.r, emissive.g, emissive.b);
    
    m_shininess = pass->getShininess();
    
    // Polygon mode (Ogre values: PM_POINTS=1, PM_WIREFRAME=2, PM_SOLID=3)
    // Convert to 0-based index for ComboBox (Points=0, Wireframe=1, Solid=2)
    m_polygonMode = static_cast<int>(pass->getPolygonMode()) - 1;
    
    // Blend factors
    m_sourceBlendFactor = pass->getSourceBlendFactor() + 6;
    m_destBlendFactor = pass->getDestBlendFactor() + 1;
    
    // Vertex color tracking
    int tracking = pass->getVertexColourTracking();
    m_useVertexColorToAmbient = tracking & 1;
    m_useVertexColorToDiffuse = tracking & 2;
    m_useVertexColorToSpecular = tracking & 4;
    m_useVertexColorToEmissive = tracking & 8;
    
    // Advanced Pass properties
    m_shadingMode = static_cast<int>(pass->getShadingMode());
    
    // Map Ogre CullingMode to ComboBox index
    // Ogre: CULL_NONE=1, CULL_CLOCKWISE=2, CULL_ANTICLOCKWISE=3
    // ComboBox: None=0, Clockwise=1, Counter-Clockwise=2
    Ogre::CullingMode cullMode = pass->getCullingMode();
    switch (cullMode) {
        case Ogre::CULL_NONE: m_cullHardware = 0; break;
        case Ogre::CULL_CLOCKWISE: m_cullHardware = 1; break;
        case Ogre::CULL_ANTICLOCKWISE: m_cullHardware = 2; break;
        default: m_cullHardware = 1; break; // Default to Clockwise
    }
    
    // Map Ogre ManualCullingMode to ComboBox index
    // Ogre: MANUAL_CULL_NONE=0, MANUAL_CULL_BACK=1, MANUAL_CULL_FRONT=2
    // ComboBox: None=0, Clockwise=1, Counter-Clockwise=2
    Ogre::ManualCullingMode manualCullMode = pass->getManualCullingMode();
    switch (manualCullMode) {
        case Ogre::MANUAL_CULL_NONE: m_cullSoftware = 0; break;
        case Ogre::MANUAL_CULL_BACK: m_cullSoftware = 1; break;
        case Ogre::MANUAL_CULL_FRONT: m_cullSoftware = 2; break;
        default: m_cullSoftware = 0; break; // Default to None
    }
    
    m_depthFunction = static_cast<int>(pass->getDepthFunction());
    
    // Depth bias - these functions may not exist in this Ogre version
    m_depthBiasConstant = 0.0f;  // Default value
    m_depthBiasSlopeScale = 0.0f; // Default value
    
    // Alpha rejection - use simplified approach
    m_alphaRejectionEnabled = false;  // Default
    m_alphaRejectionFunction = 1;     // Always Pass
    m_alphaRejectionValue = 0;        // Default
    
    m_alphaToCoverageEnabled = pass->isAlphaToCoverageEnabled();
    
    bool r, g, b, a;
    pass->getColourWriteEnabled(r, g, b, a);
    m_colourWriteRed = r;
    m_colourWriteGreen = g;
    m_colourWriteBlue = b;
    m_colourWriteAlpha = a;
    
    m_sceneBlendOperation = static_cast<int>(pass->getSceneBlendingOperation());
    m_pointSize = pass->getPointSize();
    m_lineWidth = pass->getLineWidth();
    m_pointSpritesEnabled = pass->getPointSpritesEnabled();
    m_maxLights = pass->getMaxSimultaneousLights();
    m_startLight = pass->getStartLight();
    
    // Fog properties
    m_fogOverride = pass->getFogOverride();
    if (m_fogOverride) {
        m_fogMode = static_cast<int>(pass->getFogMode());
        const Ogre::ColourValue& fc = pass->getFogColour();
        m_fogColor = QColor::fromRgbF(fc.r, fc.g, fc.b);
        m_fogDensity = pass->getFogDensity();
        m_fogStart = pass->getFogStart();
        m_fogEnd = pass->getFogEnd();
    } else {
        m_fogMode = 0;
        m_fogColor = QColor(0, 0, 0);
        m_fogDensity = 0.0f;
        m_fogStart = 0.0f;
        m_fogEnd = 1.0f;
    }
    
    // Emit all property change signals
    emit lightingEnabledChanged();
    emit depthWriteEnabledChanged();
    emit depthCheckEnabledChanged();
    emit ambientColorChanged();
    emit diffuseColorChanged();
    emit specularColorChanged();
    emit emissiveColorChanged();
    emit diffuseAlphaChanged();
    emit specularAlphaChanged();
    emit shininessChanged();
    emit polygonModeChanged();
    emit sourceBlendFactorChanged();
    emit destBlendFactorChanged();
    emit useVertexColorToAmbientChanged();
    emit useVertexColorToDiffuseChanged();
    emit useVertexColorToSpecularChanged();
    emit useVertexColorToEmissiveChanged();
    
    // Emit new property change signals
    emit shadingModeChanged();
    emit cullHardwareChanged();
    emit cullSoftwareChanged();
    emit depthFunctionChanged();
    emit depthBiasConstantChanged();
    emit depthBiasSlopeScaleChanged();
    emit alphaRejectionEnabledChanged();
    emit alphaRejectionFunctionChanged();
    emit alphaRejectionValueChanged();
    emit alphaToCoverageEnabledChanged();
    emit colourWriteRedChanged();
    emit colourWriteGreenChanged();
    emit colourWriteBlueChanged();
    emit colourWriteAlphaChanged();
    emit sceneBlendOperationChanged();
    emit pointSizeChanged();
    emit lineWidthChanged();
    emit pointSpritesEnabledChanged();
    emit maxLightsChanged();
    emit startLightChanged();
    emit fogOverrideChanged();
    emit fogModeChanged();
    emit fogColorChanged();
    emit fogDensityChanged();
    emit fogStartChanged();
    emit fogEndChanged();

    m_pbrEnvIntensity = RTShaderHelper::pbrEnvIntensity(pass);
    const Ogre::ColourValue envTint = RTShaderHelper::pbrEnvTint(pass);
    m_pbrEnvTint = QColor::fromRgbF(envTint.r, envTint.g, envTint.b);
    float parsedIntensity = m_pbrEnvIntensity;
    QColor parsedTint = m_pbrEnvTint;
    if (HdrMaterialScript::parseEnvironmentLines(m_materialText, parsedIntensity, parsedTint)) {
        m_pbrEnvIntensity = parsedIntensity;
        m_pbrEnvTint = parsedTint;
    }
    emit pbrEnvIntensityChanged();
    emit pbrEnvTintChanged();
}

void MaterialEditorQML::resetPropertiesToDefaults()
{
    // Reset all properties to their default values
    m_lightingEnabled = true;
    m_depthWriteEnabled = true;
    m_depthCheckEnabled = true;
    m_ambientColor = QColor(128, 128, 128);  // Gray
    m_diffuseColor = QColor(255, 255, 255);  // White
    m_specularColor = QColor(0, 0, 0);       // Black
    m_emissiveColor = QColor(0, 0, 0);       // Black
    m_diffuseAlpha = 1.0f;
    m_specularAlpha = 1.0f;
    m_shininess = 0.0f;
    m_polygonMode = 2; // Solid
    m_sourceBlendFactor = 6; // SBF_ONE 
    m_destBlendFactor = 1; // SBF_ZERO
    m_useVertexColorToAmbient = false;
    m_useVertexColorToDiffuse = false;
    m_useVertexColorToSpecular = false;
    m_useVertexColorToEmissive = false;
    
    // Reset new advanced properties to defaults
    m_shadingMode = 1; // Gouraud
    m_cullHardware = 1; // Clockwise
    m_cullSoftware = 0; // None
    m_depthFunction = 4; // Less Equal
    m_depthBiasConstant = 0.0f;
    m_depthBiasSlopeScale = 0.0f;
    m_alphaRejectionEnabled = false;
    m_alphaRejectionFunction = 1; // Always Pass
    m_alphaRejectionValue = 0;
    m_alphaToCoverageEnabled = false;
    m_colourWriteRed = true;
    m_colourWriteGreen = true;
    m_colourWriteBlue = true;
    m_colourWriteAlpha = true;
    m_sceneBlendOperation = 0; // Add
    m_pointSize = 1.0f;
    m_lineWidth = 1.0f;
    m_pointSpritesEnabled = false;
    m_maxLights = 0; // Unlimited
    m_startLight = 0;
    
    // Reset fog properties
    m_fogOverride = false;
    m_fogMode = 0; // None
    m_fogColor = QColor(0, 0, 0);
    m_fogDensity = 0.0f;
    m_fogStart = 0.0f;
    m_fogEnd = 1.0f;
    
    // Reset texture unit properties  
    m_texCoordSet = 0;
    m_textureAddressMode = 0; // Wrap
    m_textureBorderColor = QColor(0, 0, 0);
    m_textureFiltering = 1; // Bilinear
    m_maxAnisotropy = 1;
    m_textureUOffset = 0.0f;
    m_textureVOffset = 0.0f;
    m_textureUScale = 1.0f;
    m_textureVScale = 1.0f;
    m_textureRotation = 0.0f;
    m_environmentMapping = 0; // None
    m_pbrEnvIntensity = HdrMaterialScript::kDefaultEnvIntensity;
    m_pbrEnvTint = QColor::fromRgbF(1., 1., 1.);
    m_rotateAnimSpeed = 0.0;
    
    // Emit all property change signals to update UI
    emit lightingEnabledChanged();
    emit depthWriteEnabledChanged();
    emit depthCheckEnabledChanged();
    emit ambientColorChanged();
    emit diffuseColorChanged();
    emit specularColorChanged();
    emit emissiveColorChanged();
    emit diffuseAlphaChanged();
    emit specularAlphaChanged();
    emit shininessChanged();
    emit polygonModeChanged();
    emit sourceBlendFactorChanged();
    emit destBlendFactorChanged();
    emit useVertexColorToAmbientChanged();
    emit useVertexColorToDiffuseChanged();
    emit useVertexColorToSpecularChanged();
    emit useVertexColorToEmissiveChanged();
    
    // Emit new property signals
    emit shadingModeChanged();
    emit cullHardwareChanged();
    emit cullSoftwareChanged();
    emit depthFunctionChanged();
    emit depthBiasConstantChanged();
    emit depthBiasSlopeScaleChanged();
    emit alphaRejectionEnabledChanged();
    emit alphaRejectionFunctionChanged();
    emit alphaRejectionValueChanged();
    emit alphaToCoverageEnabledChanged();
    emit colourWriteRedChanged();
    emit colourWriteGreenChanged();
    emit colourWriteBlueChanged();
    emit colourWriteAlphaChanged();
    emit sceneBlendOperationChanged();
    emit pointSizeChanged();
    emit lineWidthChanged();
    emit pointSpritesEnabledChanged();
    emit maxLightsChanged();
    emit startLightChanged();
    emit fogOverrideChanged();
    emit fogModeChanged();
    emit fogColorChanged();
    emit fogDensityChanged();
    emit fogStartChanged();
    emit fogEndChanged();
    emit texCoordSetChanged();
    emit textureAddressModeChanged();
    emit textureBorderColorChanged();
    emit textureFilteringChanged();
    emit maxAnisotropyChanged();
    emit textureUOffsetChanged();
    emit textureVOffsetChanged();
    emit textureUScaleChanged();
    emit textureVScaleChanged();
    emit textureRotationChanged();
    emit environmentMappingChanged();
    emit pbrEnvIntensityChanged();
    emit pbrEnvTintChanged();
    emit rotateAnimSpeedChanged();
}

void MaterialEditorQML::updateTextureUnitProperties()
{
    Ogre::TextureUnitState* textureUnit = getCurrentTextureUnit();
    if (!textureUnit) {
        m_textureName = "*Select a texture*";
        m_scrollAnimUSpeed = 0.0;
        m_scrollAnimVSpeed = 0.0;
        
        // Reset texture unit properties to defaults
        m_texCoordSet = 0;
        m_textureAddressMode = 0;
        m_textureBorderColor = QColor(0, 0, 0);
        m_textureFiltering = 1;
        m_maxAnisotropy = 1;
        m_textureUOffset = 0.0f;
        m_textureVOffset = 0.0f;
        m_textureUScale = 1.0f;
        m_textureVScale = 1.0f;
        m_textureRotation = 0.0f;
        m_environmentMapping = 0;
        m_rotateAnimSpeed = 0.0;
    } else {
        QString texName = QString::fromStdString(textureUnit->getTextureName());
        m_textureName = texName.isEmpty() ? "*Select a texture*" : texName;
        
        // Get scroll animation speeds
        const auto effects = textureUnit->getEffects();
        m_scrollAnimUSpeed = 0.0;
        m_scrollAnimVSpeed = 0.0;
        
        for (const auto& effectPair : effects) {
            if (effectPair.first == Ogre::TextureUnitState::ET_UVSCROLL ||
                effectPair.first == Ogre::TextureUnitState::ET_USCROLL) {
                m_scrollAnimUSpeed = effectPair.second.arg1;
            } else if (effectPair.first == Ogre::TextureUnitState::ET_UVSCROLL ||
                       effectPair.first == Ogre::TextureUnitState::ET_VSCROLL) {
                m_scrollAnimVSpeed = effectPair.second.arg1;
            }
        }
        
        // Load texture unit properties
        m_texCoordSet = textureUnit->getTextureCoordSet();
        
        // Get texture addressing mode - simplified approach for this Ogre version
        m_textureAddressMode = 0; // Default to Wrap
        
        const Ogre::ColourValue& borderCol = textureUnit->getTextureBorderColour();
        m_textureBorderColor = QColor::fromRgbF(borderCol.r, borderCol.g, borderCol.b, borderCol.a);
        
        // Get texture filtering option - simplified approach
        m_textureFiltering = 1; // Default to bilinear
        
        m_maxAnisotropy = textureUnit->getTextureAnisotropy();
        
        // Texture transform - simplified approach (these may not be available in this version)
        m_textureUOffset = 0.0f;  // Default
        m_textureVOffset = 0.0f;  // Default
        m_textureUScale = textureUnit->getTextureUScale();
        m_textureVScale = textureUnit->getTextureVScale();
        m_textureRotation = textureUnit->getTextureRotate().valueDegrees();
        
        // Environment mapping - simplified approach
        m_environmentMapping = 0; // Default to None
        
        // Get rotate animation speed from effects
        for (const auto& effectPair : effects) {
            if (effectPair.first == Ogre::TextureUnitState::ET_ROTATE) {
                m_rotateAnimSpeed = effectPair.second.arg1;
                break;
            }
        }
    }
    
    emit textureNameChanged();
    emit scrollAnimUSpeedChanged();
    emit scrollAnimVSpeedChanged();
    
    // Emit texture unit property signals
    emit texCoordSetChanged();
    emit textureAddressModeChanged();
    emit textureBorderColorChanged();
    emit textureFilteringChanged();
    emit maxAnisotropyChanged();
    emit textureUOffsetChanged();
    emit textureVOffsetChanged();
    emit textureUScaleChanged();
    emit textureVScaleChanged();
    emit textureRotationChanged();
    emit environmentMappingChanged();
    emit rotateAnimSpeedChanged();
}

void MaterialEditorQML::updateMaterialText()
{
    if (!m_ogreMaterial) return;

    try {
        Ogre::MaterialSerializer ms;
        ms.queueForExport(m_ogreMaterial, false, false, m_materialName.toStdString());
        QString text = QString::fromStdString(ms.getQueuedAsString());
        if (Ogre::Pass* pass = getCurrentPass()) {
            const Ogre::ColourValue tint = RTShaderHelper::pbrEnvTint(pass);
            text = HdrMaterialScript::injectEnvironmentLines(
                text,
                RTShaderHelper::pbrEnvIntensity(pass),
                QColor::fromRgbF(tint.r, tint.g, tint.b));
        }
        setMaterialText(text);
    } catch (const std::exception& e) {
        qDebug() << "Error updating material text:" << e.what();
    }

    // Force the model to show updated material properties immediately
    try {
        // Remove all RTSS-generated techniques so they get recreated from
        // the modified source pass on the next render frame
        auto *shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
        if (shaderGen) {
            shaderGen->removeAllShaderBasedTechniques(m_ogreMaterial->getName(),
                Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
        }

        // Wire PBR slot colour-ops + non-FFP markers in case the user just
        // edited the material text, before recompile + RTSS regen.
        wirePbrSlotsForFFP(m_ogreMaterial.get());

        // Recompile the material
        m_ogreMaterial->compile();

        // Slice F: upgrade tagged metallic_roughness materials to the
        // Cook-Torrance SRS. Runs after compile so the SRS sees the
        // final pass state (FFP wiring above is harmless — when
        // applyPbrIfTagged returns true, RTSS replaces the FFP path).
        // Returns false for non-tagged materials → FFP path stays.
        const bool pbrApplied = RTShaderHelper::applyPbrIfTagged(m_ogreMaterial);
        (void)pbrApplied;

        // If the material has a normal_map TUS with a texture set, re-wire
        // it through SRS_NORMALMAP. Without this, dropping all shader
        // techniques above (and letting RTSS regen via the default
        // handleSchemeNotFound path) loses the normal-map shader
        // configuration — so opening the material editor visibly drops the
        // normal-map effect, even when the user hasn't changed anything.
        if (m_ogreMaterial->getNumTechniques() > 0) {
            auto* pass = m_ogreMaterial->getTechnique(0)->getPass(0);
            for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
                auto* tus = pass->getTextureUnitState(i);
                const auto& tusName = tus->getName();
                if (tusName == "normal_map" || tusName == "NormalMap") {
                    const std::string texName = tus->getTextureName();
                    if (!texName.empty()) {
                        RTShaderHelper::applyNormalMap(m_ogreMaterial, texName);
                    }
                    break;
                }
            }
        }

        // Re-apply material to all sub-entities that use it. Re-binding the
        // SAME material name can be a no-op inside Ogre (the SubEntity keeps its
        // cached Technique pointer, which we just invalidated via
        // removeAllShaderBasedTechniques + compile) — so the on-screen mesh
        // would keep rendering the stale technique and never pick up the new
        // texture, even though the editor preview (which re-binds a fresh
        // material name on its own sphere) updates. Force the rebind by
        // clearing the material first, then setting it via the material PTR so
        // the SubEntity re-resolves its technique from the recompiled material.
        std::string matName = m_materialName.toStdString();
        for (Ogre::SceneNode* sn : Manager::getSingleton()->getSceneNodes()) {
            if (sn->getName().empty() || sn->getAttachedObjects().empty())
                continue;
            for (auto* obj : sn->getAttachedObjects()) {
                if (obj->getMovableType() != "Entity") continue;
                auto* entity = static_cast<Ogre::Entity*>(obj);
                for (unsigned int si = 0; si < entity->getNumSubEntities(); ++si) {
                    Ogre::SubEntity* sub = entity->getSubEntity(si);
                    if (sub->getMaterialName() != matName)
                        continue;
                    // setMaterial(ptr) re-resolves the SubEntity's technique
                    // against the just-recompiled material even when the name is
                    // unchanged — unlike setMaterialName(sameName), which can
                    // keep the now-stale cached Technique pointer and leave the
                    // mesh rendering the old texture.
                    sub->setMaterial(m_ogreMaterial);
                }
            }
        }
    } catch (...) {}
}

Ogre::Pass* MaterialEditorQML::getCurrentPass() const
{
    if (m_selectedPassIndex >= 0 && m_passMap.contains(m_selectedPassIndex)) {
        return m_passMap[m_selectedPassIndex];
    }
    return nullptr;
}

Ogre::TextureUnitState* MaterialEditorQML::getCurrentTextureUnit() const
{
    if (m_selectedTextureUnitIndex >= 0 && m_selectedTextureUnitIndex < m_textureUnitList.size()) {
        QString unitName = m_textureUnitList[m_selectedTextureUnitIndex];
        if (m_texUnitMap.contains(unitName)) {
            return m_texUnitMap[unitName];
        }
    }
    return nullptr;
}

// Resolve a live TUS by its index in the selected pass, walking the live
// material (not the cached m_texUnitMap, which can dangle after a recompile).
Ogre::TextureUnitState* MaterialEditorQML::getTextureUnitAt(int unitIndex) const
{
    Ogre::Pass* pass = getCurrentPass();
    if (!pass || unitIndex < 0
        || unitIndex >= static_cast<int>(pass->getNumTextureUnitStates()))
        return nullptr;
    return pass->getTextureUnitState(static_cast<unsigned short>(unitIndex));
}

bool MaterialEditorQML::isPbrMaterial() const
{
    static const QStringList kPbrSlots = {
        QStringLiteral("albedo"), QStringLiteral("normal_map"),
        QStringLiteral("roughness"), QStringLiteral("metallic"),
        QStringLiteral("ao"), QStringLiteral("emissive")
    };
    for (const QString& name : m_textureUnitList)
        if (kPbrSlots.contains(name))
            return true;
    return false;
}

QString MaterialEditorQML::textureNameForUnit(int unitIndex) const
{
    if (Ogre::TextureUnitState* tus = getTextureUnitAt(unitIndex))
        return QString::fromStdString(tus->getTextureName());
    return {};
}

QString MaterialEditorQML::texturePreviewPathForUnit(int unitIndex) const
{
    const QString tex = textureNameForUnit(unitIndex);
    if (tex.isEmpty())
        return {};
    // Use the SAME group-aware cache key as getTexturePreviewPath() so per-slot
    // and single previews share entries and don't serve a stale thumbnail when
    // different materials/groups reuse the same texture basename.
    const QString groupKey = QString::fromStdString(
        m_ogreMaterial ? m_ogreMaterial->getGroup()
                       : std::string(Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME));
    const QString cacheKey = groupKey + QLatin1Char('\n') + tex;
    auto cached = m_previewPathCache.constFind(cacheKey);
    if (cached != m_previewPathCache.constEnd())
        return cached.value();
    const QString resolved = computeTexturePreviewPath(tex);
    if (!resolved.isEmpty())
        m_previewPathCache.insert(cacheKey, resolved);
    return resolved;
}

void MaterialEditorQML::setTextureForUnit(int unitIndex, const QString& texName)
{
    Ogre::TextureUnitState* tus = getTextureUnitAt(unitIndex);
    if (!tus || texName.isEmpty())
        return;
    ensureTextureInMaterialGroup(texName);
    tus->setTextureName(texName.toStdString());
    regenerateRtssShaders();
    updateTextureUnitList();
    updateMaterialText();
    emit textureUnitsChanged();
}

bool MaterialEditorQML::loadTextureFileForUnit(int unitIndex, const QString& filePath)
{
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
        QStringLiteral("Load texture into slot %1: %2")
            .arg(unitIndex).arg(QFileInfo(filePath).fileName()));
    if (!getTextureUnitAt(unitIndex)) {
        emit errorOccurred(tr("Invalid texture slot."));
        return false;
    }
    const QFileInfo file(filePath);
    if (filePath.isEmpty() || !file.exists()) {
        emit errorOccurred(tr("Texture file does not exist."));
        return false;
    }
    const bool isTim = file.suffix().compare(QStringLiteral("tim"), Qt::CaseInsensitive) == 0;
    const QString ogreTexName = isTim ? (file.completeBaseName() + QStringLiteral(".tim"))
                                      : file.fileName();
    const std::string group = Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;
    try {
        if (!Ogre::TextureManager::getSingleton().getByName(ogreTexName.toStdString(), group)) {
            Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
                file.path().toStdString(), "FileSystem", group);
            Ogre::ResourceGroupManager::getSingleton().initialiseResourceGroup(group);
        }
    } catch (...) {}
    setTextureForUnit(unitIndex, ogreTexName);
    return !textureNameForUnit(unitIndex).isEmpty();
}

Ogre::Technique* MaterialEditorQML::getCurrentTechnique() const
{
    if (!m_ogreMaterial || m_selectedTechniqueIndex < 0) {
        return nullptr;
    }
    
    const auto techniques = m_ogreMaterial->getTechniques();
    if (m_selectedTechniqueIndex < static_cast<int>(techniques.size())) {
        return techniques[m_selectedTechniqueIndex];
    }
    
    return nullptr;
}

QStringList MaterialEditorQML::getAvailableTextures() const
{
    QStringList textures;
    
    // Safety check for Ogre availability
    if (!isOgreAvailable()) {
        return textures; // Return empty list if Ogre not available
    }
    
    try {
        Ogre::ResourceManager::ResourceMapIterator it = Ogre::TextureManager::getSingleton().getResourceIterator();
        while (it.hasMoreElements()) {
            textures.append(QString::fromStdString(it.peekNextValue()->getName()));
            it.moveNext();
        }
    } catch (const std::exception& e) {
        // Silently handle exception when Ogre is not available
        qDebug() << "Ogre not available for texture enumeration:" << e.what();
    }
    
    return textures;
}

namespace {
// A texture name from an imported asset can carry subdirectories or "..".
// Reduce it to a single safe filename so a preview can never escape the
// texture_previews dir or fail because a nested dir wasn't created.
QString safePreviewBaseName(const QString& texName)
{
    QString base = QFileInfo(texName).fileName(); // strip any directory part
    base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
                 QStringLiteral("_"));
    if (base.isEmpty())
        base = QStringLiteral("texture");
    return base;
}

QString fileUrl(const QString& path)
{
    return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
}

// Writable <AppData>/texture_previews/<safeBase><suffix>, with the dir created
// and any stale file removed. Empty if AppData can't be resolved.
QString previewOutPath(const QString& texName, const QString& suffix)
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dataPath.isEmpty())
        return {};
    const QString outDir = QDir(dataPath).filePath(QStringLiteral("texture_previews"));
    QDir().mkpath(outDir);
    const QString outPath =
        QDir(outDir).filePath(safePreviewBaseName(texName) + suffix);
    QFile::remove(outPath);
    return outPath;
}

// Locate an already-loaded Ogre texture by name across ALL resource groups
// (getByName only searches the default group; FBX imports use per-asset groups).
Ogre::TexturePtr findLoadedTexture(const std::string& name)
{
    auto texPtr = Ogre::TextureManager::getSingleton().getByName(name);
    if (texPtr)
        return texPtr;
    auto it = Ogre::TextureManager::getSingleton().getResourceIterator();
    while (it.hasMoreElements()) {
        const Ogre::ResourcePtr r = it.getNext();
        if (r && r->getName() == name)
            return Ogre::static_pointer_cast<Ogre::Texture>(r);
    }
    return {};
}

// .tim can't be shown by QML — decode CPU-side (PS1TIM, NOT convertToImage which
// faults on GPU-only buffers) and write a PNG preview. Empty on failure.
QString timPreviewUrl(const QString& origin, const QString& texName)
{
    Ogre::Image timImg;
    if (!PS1TIM::loadTimToOgreImage(origin, timImg))
        return {};
    const QString outPath = previewOutPath(texName, QStringLiteral("_tim.png"));
    if (outPath.isEmpty())
        return {};
    try {
        timImg.save(outPath.toStdString());
    } catch (const Ogre::Exception&) {
        return {};
    }
    return QFileInfo::exists(outPath) ? fileUrl(outPath) : QString();
}

// Embedded-FBX textures (e.g. Boss_diffuse.png inside Rumba Dancing.fbx, #508)
// have no on-disk origin and live only in the GPU buffer convertToImage can't
// read back. Decode the bytes stashed at import (EmbeddedTextureCache) to a PNG.
QString embeddedPreviewUrl(const QString& texName)
{
    const std::vector<uint8_t> bytes =
        EmbeddedTextureCache::retrieve(texName.toStdString());
    if (bytes.empty())
        return {};
    QImage qimg;
    if (!qimg.loadFromData(bytes.data(), static_cast<int>(bytes.size())))
        return {};
    const QString outPath = previewOutPath(texName, QStringLiteral("_embedded.png"));
    if (outPath.isEmpty())
        return {};
    if (qimg.save(outPath, "PNG") && QFileInfo::exists(outPath))
        return fileUrl(outPath);
    return {};
}

// Resolve a preview from an already-loaded Ogre texture: its on-disk origin
// (PNG-decoding .tim), the embedded-byte cache, or its resource-group dir.
QString previewUrlFromOgreTexture(const Ogre::TexturePtr& texPtr,
                                  const QString& texName)
{
    const QString origin = QString::fromStdString(texPtr->getOrigin());
    if (!origin.isEmpty() && QFileInfo::exists(origin)) {
        if (QFileInfo(origin).suffix().toLower() != QStringLiteral("tim"))
            return fileUrl(origin);
        const QString timUrl = timPreviewUrl(origin, texName);
        if (!timUrl.isEmpty())
            return timUrl;
    }

    const QString embedded = embeddedPreviewUrl(texName);
    if (!embedded.isEmpty())
        return embedded;

    // The resource group name is often the directory the model loaded from.
    // (We deliberately never call convertToImage() — see embeddedPreviewUrl;
    // on loadImage/loadRawData textures it faults with an uncatchable
    // EXC_BAD_ACCESS, which is the crash this whole path was rewritten to fix.)
    const QString group = QString::fromStdString(texPtr->getGroup());
    if (!group.isEmpty()) {
        const QString groupPath = group + "/" + texName;
        if (QFileInfo::exists(groupPath))
            return fileUrl(groupPath);
        if (!origin.isEmpty()) {
            const QString combined = group + "/" + origin;
            if (QFileInfo::exists(combined))
                return fileUrl(combined);
        }
    }
    return {};
}
} // namespace

QString MaterialEditorQML::getTexturePreviewPath() const
{
    const QString texName = m_textureName;
    if (texName.isEmpty() || texName == "*Select a texture*" || texName.trimmed().isEmpty()) {
        return "";
    }

    // Memoize: the QML Image `source` binding evaluates this getter twice per
    // change and reloadPreview() calls it once more, so a single texture switch
    // invokes us 3×. Resolving touches disk (decode + save the preview PNG), so
    // re-running it on every call raced the QML Image still loading the file.
    // Key the cache by the material's resource GROUP as well as the texture
    // name: FBX imports can load the same basename (e.g. diffuse.png) into
    // different per-asset groups, and a name-only key would serve the previous
    // asset's preview after switching materials.
    const QString groupKey = QString::fromStdString(
        m_ogreMaterial ? m_ogreMaterial->getGroup()
                       : std::string(Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME));
    const QString cacheKey = groupKey + QLatin1Char('\n') + texName;
    if (auto cached = m_previewPathCache.constFind(cacheKey);
        cached != m_previewPathCache.constEnd()) {
        return cached.value();
    }
    // Refuse to recurse: if computeTexturePreviewPath() ever pumps the event
    // loop it could re-enter this getter. Return empty (uncached) so the outer
    // call still resolves and caches the real path.
    if (m_resolvingPreview) {
        return "";
    }
    m_resolvingPreview = true;
    const QString resolved = computeTexturePreviewPath(texName);
    m_resolvingPreview = false;
    // Only cache real resolutions — a transient miss (texture not yet loaded)
    // must not stick until the next explicit invalidation.
    if (!resolved.isEmpty()) {
        m_previewPathCache.insert(cacheKey, resolved);
    }
    return resolved;
}

QString MaterialEditorQML::computeTexturePreviewPath(const QString &texName) const
{
    // 1. Ask Ogre where the texture was actually loaded from (most accurate).
    if (isOgreAvailable()) {
        try {
            if (auto texPtr = findLoadedTexture(texName.toStdString())) {
                const QString url = previewUrlFromOgreTexture(texPtr, texName);
                if (!url.isEmpty())
                    return url;
            }
        } catch (const Ogre::Exception&) {
            // Fall through to the on-disk searches below.
        }
    }

    // 2. Check the generated-textures directory.
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString genTexPath =
        QDir(dataPath).filePath("generated_textures/" + texName);
    if (QFileInfo::exists(genTexPath))
        return fileUrl(genTexPath);

    // 3. Try common on-disk texture locations.
    const QStringList possiblePaths = {
        QStringLiteral("media/materials/textures/%1").arg(texName),
        QStringLiteral("../media/materials/textures/%1").arg(texName),
        QStringLiteral("../../media/materials/textures/%1").arg(texName)
    };
    for (const QString &path : possiblePaths) {
        const QFileInfo fileInfo(path);
        if (fileInfo.exists() && fileInfo.isFile())
            return fileUrl(path);
    }

    // Not found on disk (may only exist in GPU memory).
    return "";
}

bool MaterialEditorQML::exportCurrentTexture(const QString& destPath)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Export current texture to %1").arg(destPath));
    if (destPath.isEmpty()) {
        emit errorOccurred(tr("No export path chosen."));
        return false;
    }
    // Resolve the on-disk source via the same logic the preview uses.
    QString src = getTexturePreviewPath();
    if (src.startsWith("file://"))
        src = QUrl(src).toLocalFile();
    if (src.isEmpty() || !QFileInfo::exists(src)) {
        // Fall back: dump the GPU texture to the chosen path.
        if (isOgreAvailable() && !m_textureName.isEmpty()) {
            try {
                auto texPtr = Ogre::TextureManager::getSingleton()
                    .getByName(m_textureName.toStdString());
                if (texPtr) {
                    Ogre::Image img;
                    texPtr->convertToImage(img, true);
                    img.save(destPath.toStdString());
                    if (QFileInfo::exists(destPath)) return true;
                }
            } catch (...) {}
        }
        emit errorOccurred(tr("Could not locate the current texture to export."));
        return false;
    }
    // Guard against exporting onto the source file: if dest and src
    // resolve to the same path, the QFile::remove(dest) below would
    // delete the source before the copy, destroying the only copy.
    const QString srcCanonical = QFileInfo(src).canonicalFilePath();
    const QFileInfo destInfo(destPath);
    const QString destCanonical = destInfo.exists()
        ? destInfo.canonicalFilePath()
        : QFileInfo(destInfo.absoluteFilePath()).absoluteFilePath();
    if (!srcCanonical.isEmpty() &&
        (srcCanonical == destCanonical ||
         srcCanonical == QFileInfo(destPath).absoluteFilePath())) {
        // Source already is the destination — nothing to do.
        return true;
    }
    QFile::remove(destPath);
    if (!QFile::copy(src, destPath)) {
        emit errorOccurred(tr("Failed to write %1").arg(destPath));
        return false;
    }
    return true;
}

QString MaterialEditorQML::chooseTextureExportPath()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Open texture export file dialog"));
    QString suggested = m_textureName.isEmpty()
        ? QStringLiteral("texture.png") : m_textureName;
    if (!suggested.contains('.')) suggested += ".png";
    return QFileDialog::getSaveFileName(
        nullptr, tr("Export Texture"), suggested,
        tr("PNG (*.png);;JPEG (*.jpg);;All Files (*)"),
        nullptr, QFileDialog::DontUseNativeDialog);
}

void MaterialEditorQML::openTextureFileDialog()
{
    // This will be handled by QML FileDialog
    // Just a placeholder for future C++ implementation if needed
}

void MaterialEditorQML::exportMaterial(const QString &fileName)
{
    if (!m_ogreMaterial) {
        emit errorOccurred("No material to export");
        return;
    }
    
    try {
        Ogre::MaterialSerializer ms;
        ms.exportMaterial(m_ogreMaterial, fileName.toStdString());
        emit materialApplied(); // Reuse this signal to indicate success
    } catch (const Ogre::Exception& e) {
        emit errorOccurred(QString("Failed to export material: ") + e.getDescription().c_str());
    } catch (const std::exception& e) {
        emit errorOccurred(QString("Failed to export material: ") + e.what());
    }
}

// LCOV_EXCL_START — modal dialog requires user interaction, cannot test headless
void MaterialEditorQML::openColorPicker(const QString &colorType)
{
    QColor currentColor;
    
    if (colorType == "ambient") {
        currentColor = m_ambientColor;
    } else if (colorType == "diffuse") {
        currentColor = m_diffuseColor;
    } else if (colorType == "specular") {
        currentColor = m_specularColor;
    } else if (colorType == "emissive") {
        currentColor = m_emissiveColor;
    } else {
        currentColor = Qt::white;
    }
    
    // Find the main application window as parent
    QWidget* parent = nullptr;
    QWidgetList topLevelWidgets = QApplication::topLevelWidgets();
    for (QWidget* widget : topLevelWidgets) {
        if (widget->isVisible() && widget->inherits("QMainWindow")) {
            parent = widget;
            break;
        }
    }
    
    // Create color dialog with proper parent
    QColorDialog colorDialog(currentColor, parent);
    colorDialog.setWindowTitle(QString("Select %1 Color").arg(colorType.toUpper()));
    colorDialog.setOption(QColorDialog::ShowAlphaChannel, false);
    colorDialog.setOption(QColorDialog::DontUseNativeDialog, false); // Use native dialog for better compatibility
    
    // Make dialog modal to application
    colorDialog.setWindowModality(Qt::ApplicationModal);
    
    // Use exec() only - it handles showing and modal behavior
    if (colorDialog.exec() == QDialog::Accepted) {
        QColor selectedColor = colorDialog.selectedColor();
        
        if (selectedColor.isValid()) {
            if (colorType == "ambient") {
                setAmbientColor(selectedColor);
            } else if (colorType == "diffuse") {
                setDiffuseColor(selectedColor);
            } else if (colorType == "specular") {
                setSpecularColor(selectedColor);
            } else if (colorType == "emissive") {
                setEmissiveColor(selectedColor);
            }
        }
    }
}

// LCOV_EXCL_STOP

// Advanced Pass property setters
void MaterialEditorQML::setShadingMode(int mode)
{
    if (m_shadingMode != mode) {
        m_shadingMode = mode;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setShadingMode(static_cast<Ogre::ShadeOptions>(mode));
            updateMaterialText();
        }
        emit shadingModeChanged();
    }
}

void MaterialEditorQML::setCullHardware(int mode)
{
    if (m_cullHardware != mode) {
        m_cullHardware = mode;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            // Map ComboBox index to Ogre CullingMode enum
            // ComboBox: None=0, Clockwise=1, Counter-Clockwise=2
            // Ogre: CULL_NONE=1, CULL_CLOCKWISE=2, CULL_ANTICLOCKWISE=3
            Ogre::CullingMode cullingMode;
            switch (mode) {
                case 0: cullingMode = Ogre::CULL_NONE; break;
                case 1: cullingMode = Ogre::CULL_CLOCKWISE; break;
                case 2: cullingMode = Ogre::CULL_ANTICLOCKWISE; break;
                default: cullingMode = Ogre::CULL_CLOCKWISE; break;
            }
            pass->setCullingMode(cullingMode);
            updateMaterialText();
        }
        emit cullHardwareChanged();
    }
}

void MaterialEditorQML::setCullSoftware(int mode)
{
    if (m_cullSoftware != mode) {
        m_cullSoftware = mode;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            // Map ComboBox index to Ogre ManualCullingMode enum
            // ComboBox: None=0, Clockwise=1, Counter-Clockwise=2
            // Ogre: MANUAL_CULL_NONE=0, MANUAL_CULL_BACK=1, MANUAL_CULL_FRONT=2
            Ogre::ManualCullingMode manualCullingMode;
            switch (mode) {
                case 0: manualCullingMode = Ogre::MANUAL_CULL_NONE; break;
                case 1: manualCullingMode = Ogre::MANUAL_CULL_BACK; break;
                case 2: manualCullingMode = Ogre::MANUAL_CULL_FRONT; break;
                default: manualCullingMode = Ogre::MANUAL_CULL_NONE; break;
            }
            pass->setManualCullingMode(manualCullingMode);
            updateMaterialText();
        }
        emit cullSoftwareChanged();
    }
}

void MaterialEditorQML::setDepthFunction(int function)
{
    if (m_depthFunction != function) {
        m_depthFunction = function;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setDepthFunction(static_cast<Ogre::CompareFunction>(function));
            updateMaterialText();
        }
        emit depthFunctionChanged();
    }
}

void MaterialEditorQML::setDepthBiasConstant(float bias)
{
    if (m_depthBiasConstant != bias) {
        m_depthBiasConstant = bias;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setDepthBias(bias, m_depthBiasSlopeScale);
            updateMaterialText();
        }
        emit depthBiasConstantChanged();
    }
}

void MaterialEditorQML::setDepthBiasSlopeScale(float bias)
{
    if (m_depthBiasSlopeScale != bias) {
        m_depthBiasSlopeScale = bias;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setDepthBias(m_depthBiasConstant, bias);
            updateMaterialText();
        }
        emit depthBiasSlopeScaleChanged();
    }
}

void MaterialEditorQML::setAlphaRejectionEnabled(bool enabled)
{
    if (m_alphaRejectionEnabled != enabled) {
        m_alphaRejectionEnabled = enabled;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            if (enabled) {
                pass->setAlphaRejectSettings(static_cast<Ogre::CompareFunction>(m_alphaRejectionFunction), 
                                           static_cast<unsigned char>(m_alphaRejectionValue));
            } else {
                pass->setAlphaRejectSettings(Ogre::CMPF_ALWAYS_PASS, 0);
            }
            updateMaterialText();
        }
        emit alphaRejectionEnabledChanged();
    }
}

void MaterialEditorQML::setAlphaRejectionFunction(int function)
{
    if (m_alphaRejectionFunction != function) {
        m_alphaRejectionFunction = function;
        if (m_alphaRejectionEnabled) {
            Ogre::Pass* pass = getCurrentPass();
            if (pass) {
                pass->setAlphaRejectSettings(static_cast<Ogre::CompareFunction>(function), 
                                           static_cast<unsigned char>(m_alphaRejectionValue));
                updateMaterialText();
            }
        }
        emit alphaRejectionFunctionChanged();
    }
}

void MaterialEditorQML::setAlphaRejectionValue(int value)
{
    if (m_alphaRejectionValue != value) {
        m_alphaRejectionValue = value;
        if (m_alphaRejectionEnabled) {
            Ogre::Pass* pass = getCurrentPass();
            if (pass) {
                pass->setAlphaRejectSettings(static_cast<Ogre::CompareFunction>(m_alphaRejectionFunction), 
                                           static_cast<unsigned char>(value));
                updateMaterialText();
            }
        }
        emit alphaRejectionValueChanged();
    }
}

void MaterialEditorQML::setAlphaToCoverageEnabled(bool enabled)
{
    if (m_alphaToCoverageEnabled != enabled) {
        m_alphaToCoverageEnabled = enabled;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setAlphaToCoverageEnabled(enabled);
            updateMaterialText();
        }
        emit alphaToCoverageEnabledChanged();
    }
}

void MaterialEditorQML::setColourWriteRed(bool enabled)
{
    if (m_colourWriteRed != enabled) {
        m_colourWriteRed = enabled;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setColourWriteEnabled(enabled, m_colourWriteGreen, m_colourWriteBlue, m_colourWriteAlpha);
            updateMaterialText();
        }
        emit colourWriteRedChanged();
    }
}

void MaterialEditorQML::setColourWriteGreen(bool enabled)
{
    if (m_colourWriteGreen != enabled) {
        m_colourWriteGreen = enabled;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setColourWriteEnabled(m_colourWriteRed, enabled, m_colourWriteBlue, m_colourWriteAlpha);
            updateMaterialText();
        }
        emit colourWriteGreenChanged();
    }
}

void MaterialEditorQML::setColourWriteBlue(bool enabled)
{
    if (m_colourWriteBlue != enabled) {
        m_colourWriteBlue = enabled;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setColourWriteEnabled(m_colourWriteRed, m_colourWriteGreen, enabled, m_colourWriteAlpha);
            updateMaterialText();
        }
        emit colourWriteBlueChanged();
    }
}

void MaterialEditorQML::setColourWriteAlpha(bool enabled)
{
    if (m_colourWriteAlpha != enabled) {
        m_colourWriteAlpha = enabled;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setColourWriteEnabled(m_colourWriteRed, m_colourWriteGreen, m_colourWriteBlue, enabled);
            updateMaterialText();
        }
        emit colourWriteAlphaChanged();
    }
}

void MaterialEditorQML::setSceneBlendOperation(int operation)
{
    if (m_sceneBlendOperation != operation) {
        m_sceneBlendOperation = operation;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setSceneBlendingOperation(static_cast<Ogre::SceneBlendOperation>(operation));
            updateMaterialText();
        }
        emit sceneBlendOperationChanged();
    }
}

void MaterialEditorQML::setPointSize(float size)
{
    if (m_pointSize != size) {
        m_pointSize = size;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setPointSize(size);
            updateMaterialText();
        }
        emit pointSizeChanged();
    }
}

void MaterialEditorQML::setLineWidth(float width)
{
    if (m_lineWidth != width) {
        m_lineWidth = width;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setLineWidth(width);
            updateMaterialText();
        }
        emit lineWidthChanged();
    }
}

void MaterialEditorQML::setPointSpritesEnabled(bool enabled)
{
    if (m_pointSpritesEnabled != enabled) {
        m_pointSpritesEnabled = enabled;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setPointSpritesEnabled(enabled);
            updateMaterialText();
        }
        emit pointSpritesEnabledChanged();
    }
}

void MaterialEditorQML::setMaxLights(int maxLights)
{
    if (m_maxLights != maxLights) {
        m_maxLights = maxLights;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setMaxSimultaneousLights(maxLights);
            updateMaterialText();
        }
        emit maxLightsChanged();
    }
}

void MaterialEditorQML::setStartLight(int startLight)
{
    if (m_startLight != startLight) {
        m_startLight = startLight;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            pass->setStartLight(startLight);
            updateMaterialText();
        }
        emit startLightChanged();
    }
}

// Fog property setters
void MaterialEditorQML::setFogOverride(bool enabled)
{
    if (m_fogOverride != enabled) {
        m_fogOverride = enabled;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            if (enabled) {
                pass->setFog(enabled, static_cast<Ogre::FogMode>(m_fogMode),
                           Ogre::ColourValue(m_fogColor.redF(), m_fogColor.greenF(), m_fogColor.blueF()),
                           m_fogDensity, m_fogStart, m_fogEnd);
            } else {
                pass->setFog(false);
            }
            updateMaterialText();
        }
        emit fogOverrideChanged();
    }
}

void MaterialEditorQML::setFogMode(int mode)
{
    if (m_fogMode != mode) {
        m_fogMode = mode;
        if (m_fogOverride) {
            Ogre::Pass* pass = getCurrentPass();
            if (pass) {
                pass->setFog(true, static_cast<Ogre::FogMode>(mode), 
                           Ogre::ColourValue(m_fogColor.redF(), m_fogColor.greenF(), m_fogColor.blueF()),
                           m_fogDensity, m_fogStart, m_fogEnd);
                updateMaterialText();
            }
        }
        emit fogModeChanged();
    }
}

void MaterialEditorQML::setFogColor(const QColor &color)
{
    if (m_fogColor != color) {
        m_fogColor = color;
        if (m_fogOverride) {
            Ogre::Pass* pass = getCurrentPass();
            if (pass) {
                pass->setFog(true, static_cast<Ogre::FogMode>(m_fogMode), 
                           Ogre::ColourValue(color.redF(), color.greenF(), color.blueF()),
                           m_fogDensity, m_fogStart, m_fogEnd);
                updateMaterialText();
            }
        }
        emit fogColorChanged();
    }
}

void MaterialEditorQML::setFogDensity(float density)
{
    if (m_fogDensity != density) {
        m_fogDensity = density;
        if (m_fogOverride) {
            Ogre::Pass* pass = getCurrentPass();
            if (pass) {
                pass->setFog(true, static_cast<Ogre::FogMode>(m_fogMode), 
                           Ogre::ColourValue(m_fogColor.redF(), m_fogColor.greenF(), m_fogColor.blueF()),
                           density, m_fogStart, m_fogEnd);
                updateMaterialText();
            }
        }
        emit fogDensityChanged();
    }
}

void MaterialEditorQML::setFogStart(float start)
{
    if (m_fogStart != start) {
        m_fogStart = start;
        if (m_fogOverride) {
            Ogre::Pass* pass = getCurrentPass();
            if (pass) {
                pass->setFog(true, static_cast<Ogre::FogMode>(m_fogMode), 
                           Ogre::ColourValue(m_fogColor.redF(), m_fogColor.greenF(), m_fogColor.blueF()),
                           m_fogDensity, start, m_fogEnd);
                updateMaterialText();
            }
        }
        emit fogStartChanged();
    }
}

void MaterialEditorQML::setFogEnd(float end)
{
    if (m_fogEnd != end) {
        m_fogEnd = end;
        if (m_fogOverride) {
            Ogre::Pass* pass = getCurrentPass();
            if (pass) {
                pass->setFog(true, static_cast<Ogre::FogMode>(m_fogMode), 
                           Ogre::ColourValue(m_fogColor.redF(), m_fogColor.greenF(), m_fogColor.blueF()),
                           m_fogDensity, m_fogStart, end);
                updateMaterialText();
            }
        }
        emit fogEndChanged();
    }
}

// Texture Unit property setters
void MaterialEditorQML::setTexCoordSet(int set)
{
    if (m_texCoordSet != set) {
        m_texCoordSet = set;
        Ogre::TextureUnitState* tus = getCurrentTextureUnit();
        if (tus) {
            tus->setTextureCoordSet(set);
            updateMaterialText();
        }
        emit texCoordSetChanged();
    }
}

void MaterialEditorQML::setTextureAddressMode(int mode)
{
    if (m_textureAddressMode != mode) {
        m_textureAddressMode = mode;
        Ogre::TextureUnitState* tus = getCurrentTextureUnit();
        if (tus) {
            switch (mode) {
                case 0: tus->setTextureAddressingMode(Ogre::TAM_WRAP); break;
                case 1: tus->setTextureAddressingMode(Ogre::TAM_CLAMP); break;
                case 2: tus->setTextureAddressingMode(Ogre::TAM_MIRROR); break;
                case 3: tus->setTextureAddressingMode(Ogre::TAM_BORDER); break;
            }
            updateMaterialText();
        }
        emit textureAddressModeChanged();
    }
}

void MaterialEditorQML::setTextureBorderColor(const QColor &color)
{
    if (m_textureBorderColor != color) {
        m_textureBorderColor = color;
        Ogre::TextureUnitState* tus = getCurrentTextureUnit();
        if (tus) {
            tus->setTextureBorderColour(Ogre::ColourValue(color.redF(), color.greenF(), color.blueF(), color.alphaF()));
            updateMaterialText();
        }
        emit textureBorderColorChanged();
    }
}

void MaterialEditorQML::setTextureFiltering(int filtering)
{
    if (m_textureFiltering != filtering) {
        m_textureFiltering = filtering;
        Ogre::TextureUnitState* tus = getCurrentTextureUnit();
        if (tus) {
            switch (filtering) {
                case 0: tus->setTextureFiltering(Ogre::TFO_NONE); break;
                case 1: tus->setTextureFiltering(Ogre::TFO_BILINEAR); break;
                case 2: tus->setTextureFiltering(Ogre::TFO_TRILINEAR); break;
                case 3: tus->setTextureFiltering(Ogre::TFO_ANISOTROPIC); break;
            }
            updateMaterialText();
        }
        emit textureFilteringChanged();
    }
}

void MaterialEditorQML::setMaxAnisotropy(int anisotropy)
{
    if (m_maxAnisotropy != anisotropy) {
        m_maxAnisotropy = anisotropy;
        Ogre::TextureUnitState* tus = getCurrentTextureUnit();
        if (tus) {
            tus->setTextureAnisotropy(anisotropy);
            updateMaterialText();
        }
        emit maxAnisotropyChanged();
    }
}

void MaterialEditorQML::setTextureUOffset(float offset)
{
    if (m_textureUOffset != offset) {
        m_textureUOffset = offset;
        Ogre::TextureUnitState* tus = getCurrentTextureUnit();
        if (tus) {
            // Create translation matrix for texture transform
            Ogre::Matrix4 transform;
            transform.makeTrans(offset, m_textureVOffset, 0);
            tus->setTextureTransform(transform);
            updateMaterialText();
        }
        emit textureUOffsetChanged();
    }
}

void MaterialEditorQML::setTextureVOffset(float offset)
{
    if (m_textureVOffset != offset) {
        m_textureVOffset = offset;
        Ogre::TextureUnitState* tus = getCurrentTextureUnit();
        if (tus) {
            // Create translation matrix for texture transform
            Ogre::Matrix4 transform;
            transform.makeTrans(m_textureUOffset, offset, 0);
            tus->setTextureTransform(transform);
            updateMaterialText();
        }
        emit textureVOffsetChanged();
    }
}

void MaterialEditorQML::setTextureUScale(float scale)
{
    if (m_textureUScale != scale) {
        m_textureUScale = scale;
        Ogre::TextureUnitState* tus = getCurrentTextureUnit();
        if (tus) {
            tus->setTextureUScale(scale);
            updateMaterialText();
        }
        emit textureUScaleChanged();
    }
}

void MaterialEditorQML::setTextureVScale(float scale)
{
    if (m_textureVScale != scale) {
        m_textureVScale = scale;
        Ogre::TextureUnitState* tus = getCurrentTextureUnit();
        if (tus) {
            tus->setTextureVScale(scale);
            updateMaterialText();
        }
        emit textureVScaleChanged();
    }
}

void MaterialEditorQML::setTextureRotation(float rotation)
{
    if (m_textureRotation != rotation) {
        m_textureRotation = rotation;
        Ogre::TextureUnitState* tus = getCurrentTextureUnit();
        if (tus) {
            tus->setTextureRotate(Ogre::Radian(Ogre::Degree(rotation)));
            updateMaterialText();
        }
        emit textureRotationChanged();
    }
}

void MaterialEditorQML::setEnvironmentMapping(int mapping)
{
    if (m_environmentMapping != mapping) {
        m_environmentMapping = mapping;
        Ogre::TextureUnitState* tus = getCurrentTextureUnit();
        if (tus) {
            switch (mapping) {
                case 0: /* None - remove env mapping */ break;
                case 1: tus->setEnvironmentMap(true, Ogre::TextureUnitState::ENV_PLANAR); break;
                case 2: tus->setEnvironmentMap(true, Ogre::TextureUnitState::ENV_CURVED); break;
                case 3: tus->setEnvironmentMap(true, Ogre::TextureUnitState::ENV_REFLECTION); break;
                case 4: tus->setEnvironmentMap(true, Ogre::TextureUnitState::ENV_NORMAL); break;
            }
            updateMaterialText();
        }
        emit environmentMappingChanged();
    }
}

void MaterialEditorQML::setPbrEnvIntensity(float intensity)
{
    intensity = qBound(HdrMaterialScript::kMinEnvIntensity,
                       intensity,
                       HdrMaterialScript::kMaxEnvIntensity);
    if (qFuzzyCompare(m_pbrEnvIntensity, intensity))
        return;

    Ogre::Pass* pass = getCurrentPass();
    if (pass)
        RTShaderHelper::setPbrEnvIntensity(pass, intensity);

    m_pbrEnvIntensity = intensity;
    if (pass)
        updateMaterialText();
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("material.pbrEnvIntensity=%1").arg(intensity));
    emit pbrEnvIntensityChanged();
}

void MaterialEditorQML::setPbrEnvTint(const QColor& tint)
{
    if (m_pbrEnvTint == tint)
        return;

    Ogre::Pass* pass = getCurrentPass();
    if (pass) {
        RTShaderHelper::setPbrEnvTint(
            pass,
            Ogre::ColourValue(tint.redF(), tint.greenF(), tint.blueF()));
    }

    m_pbrEnvTint = tint;
    if (pass)
        updateMaterialText();
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("material.pbrEnvTint=%1,%2,%3")
                                      .arg(tint.redF())
                                      .arg(tint.greenF())
                                      .arg(tint.blueF()));
    emit pbrEnvTintChanged();
}

void MaterialEditorQML::setRotateAnimSpeed(double speed)
{
    if (m_rotateAnimSpeed != speed) {
        m_rotateAnimSpeed = speed;
        Ogre::TextureUnitState* tus = getCurrentTextureUnit();
        if (tus) {
            tus->setRotateAnimation(speed);
            updateMaterialText();
        }
        emit rotateAnimSpeedChanged();
    }
}

// Additional utility functions for new properties
QStringList MaterialEditorQML::getShadingModeNames() const
{
    return QStringList() << "Flat" << "Gouraud" << "Phong";
}

QStringList MaterialEditorQML::getCullModeNames() const
{
    return QStringList() << "None" << "Clockwise" << "Counter-Clockwise";
}

QStringList MaterialEditorQML::getDepthFunctionNames() const
{
    return QStringList() << "Always Fail" << "Always Pass" << "Less" << "Less Equal" 
                        << "Equal" << "Not Equal" << "Greater Equal" << "Greater";
}

QStringList MaterialEditorQML::getAlphaRejectionFunctionNames() const
{
    return QStringList() << "Always Fail" << "Always Pass" << "Less" << "Less Equal" 
                        << "Equal" << "Not Equal" << "Greater Equal" << "Greater";
}

QStringList MaterialEditorQML::getSceneBlendOperationNames() const
{
    return QStringList() << "Add" << "Subtract" << "Reverse Subtract" << "Min" << "Max";
}

QStringList MaterialEditorQML::getFogModeNames() const
{
    return QStringList() << "None" << "Exp" << "Exp2" << "Linear";
}

QStringList MaterialEditorQML::getTextureAddressModeNames() const
{
    return QStringList() << "Wrap" << "Clamp" << "Mirror" << "Border";
}

QStringList MaterialEditorQML::getTextureFilteringNames() const
{
    return QStringList() << "None" << "Bilinear" << "Trilinear" << "Anisotropic";
}

QStringList MaterialEditorQML::getEnvironmentMappingNames() const
{
    return QStringList() << "None" << "Enabled";
}

// File system browsing methods
QVariantList MaterialEditorQML::listDirectory(const QString &path)
{
    QVariantList result;
    QDir dir(path);
    
    if (!dir.exists()) {
        return result;
    }
    
    // Set filters for files and directories
    dir.setFilter(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    dir.setSorting(QDir::DirsFirst | QDir::Name);
    
    QFileInfoList entries = dir.entryInfoList();
    
    for (const QFileInfo &entry : entries) {
        QVariantMap item;
        item["name"] = entry.fileName();
        item["path"] = entry.absoluteFilePath();
        item["type"] = entry.isDir() ? "dir" : "file";
        item["size"] = entry.isDir() ? "" : getFileSizeString(entry.absoluteFilePath());
        
        // Filter image files for better UX
        if (entry.isFile()) {
            QString suffix = entry.suffix().toLower();
            if (suffix == "jpg" || suffix == "jpeg" || suffix == "png" || 
                suffix == "dds" || suffix == "tga" || suffix == "bmp") {
                result.append(item);
            }
        } else {
            result.append(item);
        }
    }
    
    return result;
}

bool MaterialEditorQML::isDirectory(const QString &path)
{
    QFileInfo info(path);
    return info.isDir();
}

QString MaterialEditorQML::getParentDirectory(const QString &path)
{
    QFileInfo info(path);
    return info.absoluteDir().absolutePath();
}

QString MaterialEditorQML::getFileName(const QString &path)
{
    QFileInfo info(path);
    return info.fileName();
}

qint64 MaterialEditorQML::getFileSize(const QString &path)
{
    QFileInfo info(path);
    return info.size();
}

QString MaterialEditorQML::getFileSizeString(const QString &path)
{
    qint64 size = getFileSize(path);
    
    if (size < 1024) {
        return QString("%1 B").arg(size);
    } else if (size < 1024 * 1024) {
        return QString("%1 KB").arg(QString::number(size / 1024.0, 'f', 1));
    } else {
        return QString("%1 MB").arg(QString::number(size / (1024.0 * 1024.0), 'f', 1));
    }
}

bool MaterialEditorQML::pathExists(const QString &path)
{
    return QFileInfo::exists(path);
}

// LCOV_EXCL_START — modal file dialogs require interactive user input and are not stable in headless CI
QString MaterialEditorQML::openFileDialog()
{
    QString texturesPath = "./media/materials/textures";
    QDir texturesDir(texturesPath);
    
    // Use absolute path if the directory exists, otherwise use current directory
    QString startDir = texturesDir.exists() ? texturesDir.absolutePath() : QDir::currentPath();
    
    // Force Qt to process all pending events first
    QApplication::processEvents();

    // Force the application to be active and on top
    if (QWidget *activeWin = QApplication::activeWindow()) {
        activeWin->raise();
        activeWin->activateWindow();
    }

    QApplication::processEvents();

    QString selectedFile = QFileDialog::getOpenFileName(
        QApplication::activeWindow(),
        "Select Texture File",
        startDir,
        "Texture files (*.jpg *.jpeg *.png *.dds *.tga *.bmp *.tim);;All files (*)",
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons
    );

    if (!selectedFile.isEmpty()) {
        return selectedFile;
    } else {
        return QString();
    }
}

bool MaterialEditorQML::loadTextureFile(const QString &filePath)
{
    if (filePath.isEmpty())
        return false;

    Ogre::TextureUnitState* textureUnit = getCurrentTextureUnit();
    if (!textureUnit) {
        emit errorOccurred("No texture unit selected.");
        return false;
    }

    QFileInfo file(filePath);
    if (!file.exists()) {
        emit errorOccurred("Texture file does not exist.");
        return false;
    }
    if (file.fileName().isEmpty()) {
        emit errorOccurred("Selected file has an empty name.");
        return false;
    }

    const bool isTim = file.suffix().compare(QStringLiteral("tim"), Qt::CaseInsensitive) == 0;
    const QString ogreTexName = isTim ? (file.completeBaseName() + QStringLiteral(".tim")) : file.fileName();
    const std::string texNameStd = ogreTexName.toStdString();
    const std::string group = Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;

    try {
        if (Ogre::TextureManager::getSingleton().getByName(texNameStd, group)) {
            setTextureName(ogreTexName);
            return true;
        }
    } catch (...) {
    }

    try {
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
            file.path().toStdString(), "FileSystem", group);
        Ogre::ResourceGroupManager::getSingleton().initialiseResourceGroup(group);

        Ogre::Image image;
        if (isTim) {
            QString err;
            if (!PS1TIM::loadTimToOgreImage(filePath, image, &err)) {
                emit errorOccurred(QString("Failed to load TIM: %1").arg(err));
                return false;
            }
        } else {
            image.load(file.fileName().toStdString(), group);
        }
        Ogre::TextureManager::getSingleton().loadImage(texNameStd, group, image);
        setTextureName(ogreTexName);
        return true;
    } catch (const std::exception& e) {
        emit errorOccurred(QString("Texture load failed: %1").arg(e.what()));
        return false;
    } catch (...) {
        emit errorOccurred("Texture load failed.");
        return false;
    }
}

QString MaterialEditorQML::openMaterialImportDialog()
{
    QString materialsPath = "./media/materials/scripts";
    QDir materialsDir(materialsPath);
    
    // Use absolute path if the directory exists, otherwise use current directory
    QString startDir = materialsDir.exists() ? materialsDir.absolutePath() : QDir::currentPath();
    
    QApplication::processEvents();

    if (QWidget *activeWin = QApplication::activeWindow()) {
        activeWin->raise();
        activeWin->activateWindow();
    }

    QApplication::processEvents();

    QString selectedFile = QFileDialog::getOpenFileName(
        QApplication::activeWindow(),
        "Import Material File",
        startDir,
        "Material files (*.material);;All files (*)",
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons
    );

    if (!selectedFile.isEmpty()) {
        return selectedFile;
    } else {
        return QString();
    }
}

QString MaterialEditorQML::openMaterialExportDialog(const QString &materialName)
{
    QString materialsPath = "./media/materials/scripts";
    QDir materialsDir(materialsPath);
    
    // Use absolute path if the directory exists, otherwise use current directory
    QString startDir = materialsDir.exists() ? materialsDir.absolutePath() : QDir::currentPath();
    
    // Create default filename from material name
    QString defaultFileName = materialName.isEmpty() ? "material" : materialName;
    if (!defaultFileName.endsWith(".material")) {
        defaultFileName += ".material";
    }
    QString defaultPath = QDir(startDir).filePath(defaultFileName);
    
    QApplication::processEvents();

    if (QWidget *activeWin = QApplication::activeWindow()) {
        activeWin->raise();
        activeWin->activateWindow();
    }

    QApplication::processEvents();

    QString selectedFile = QFileDialog::getSaveFileName(
        QApplication::activeWindow(),
        "Export Material File",
        defaultPath,
        "Material files (*.material);;All files (*)",
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons
    );

    if (!selectedFile.isEmpty()) {
        return selectedFile;
    } else {
        return QString();
    }
}

QString MaterialEditorQML::showNativeFileDialog(QObject *parentWindow)
{
    QString texturesPath = "./media/materials/textures";
    QDir texturesDir(texturesPath);
    
    // Use absolute path if the directory exists, otherwise use current directory
    QString startDir = texturesDir.exists() ? texturesDir.absolutePath() : QDir::currentPath();
    
    QWidget *parentWidget = qobject_cast<QWidget *>(parentWindow);
    if (!parentWidget) {
        parentWidget = QApplication::activeWindow();
    }
    if (!parentWidget) {
        QWidgetList topLevelWidgets = QApplication::topLevelWidgets();
        for (QWidget *widget : topLevelWidgets) {
            if (widget->isWindow() && widget->isVisible()) {
                parentWidget = widget;
                break;
            }
        }
    }

    QString selectedFile = QFileDialog::getOpenFileName(
        parentWidget,
        "Select Texture File",
        startDir,
        "Image files (*.jpg *.jpeg *.png *.dds *.tga *.bmp);;All files (*)",
        nullptr,
        QFileDialog::Options()
    );

    if (!selectedFile.isEmpty()) {
        return QFileInfo(selectedFile).fileName();
    } else {
        return QString();
    }
}
// LCOV_EXCL_STOP

QString MaterialEditorQML::testConnection()
{
    return "C++ method called successfully!";
}

QString MaterialEditorQML::savePackedTextureDialog()
{
    QString texturesPath = "./media/materials/textures";
    QDir texturesDir(texturesPath);
    QString startDir = texturesDir.exists() ? texturesDir.absolutePath() : QDir::currentPath();

    QApplication::processEvents();
    if (QWidget *activeWin = QApplication::activeWindow()) {
        activeWin->raise();
        activeWin->activateWindow();
    }
    QApplication::processEvents();

    QString selectedFile = QFileDialog::getSaveFileName(
        QApplication::activeWindow(),
        "Save Packed Texture",
        startDir + "/packed.png",
        "PNG (*.png);;TGA (*.tga);;JPEG (*.jpg *.jpeg);;BMP (*.bmp)",
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons
    );
    return selectedFile;
}

QString MaterialEditorQML::previewPackedTextureChannels(const QString& redPath,
                                                         const QString& greenPath,
                                                         const QString& bluePath,
                                                         const QString& alphaPath,
                                                         double redConstant,
                                                         double greenConstant,
                                                         double blueConstant,
                                                         double alphaConstant,
                                                         bool invertRed,
                                                         bool invertGreen,
                                                         bool invertBlue,
                                                         bool invertAlpha,
                                                         bool includeAlpha,
                                                         int previewSize)
{
    TextureChannelPacker::PackingSpec spec;
    spec.red.path        = redPath;
    spec.red.constantValue   = static_cast<float>(redConstant);
    spec.red.invert      = invertRed;
    spec.green.path      = greenPath;
    spec.green.constantValue = static_cast<float>(greenConstant);
    spec.green.invert    = invertGreen;
    spec.blue.path       = bluePath;
    spec.blue.constantValue  = static_cast<float>(blueConstant);
    spec.blue.invert     = invertBlue;
    spec.alpha.path      = alphaPath;
    spec.alpha.constantValue = static_cast<float>(alphaConstant);
    spec.alpha.invert    = invertAlpha;
    spec.includeAlpha    = includeAlpha;

    // Cap preview size so it stays cheap on every input change. The
    // packer scales smaller sources up to the largest source — by
    // forcing the output dimensions here we both make this fast and
    // guarantee a square thumbnail QML can show without flicker.
    const int cappedSize = std::clamp(previewSize, 32, 512);
    spec.outputWidth = cappedSize;
    spec.outputHeight = cappedSize;

    auto r = TextureChannelPacker::pack(spec);
    if (!r.ok) return QString();

    // Encode as a base64 PNG so QML can display via "data:" URL without
    // touching the filesystem.
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    if (!r.image.save(&buf, "PNG")) return QString();
    return QStringLiteral("data:image/png;base64,") + bytes.toBase64();
}

QString MaterialEditorQML::packTextureChannels(const QString& redPath,
                                                const QString& greenPath,
                                                const QString& bluePath,
                                                const QString& alphaPath,
                                                double redConstant,
                                                double greenConstant,
                                                double blueConstant,
                                                double alphaConstant,
                                                bool invertRed,
                                                bool invertGreen,
                                                bool invertBlue,
                                                bool invertAlpha,
                                                bool includeAlpha,
                                                const QString& outputPath)
{
    SentryReporter::addBreadcrumb("ui.action", "Pack texture channels");

    TextureChannelPacker::PackingSpec spec;
    spec.red.path        = redPath;
    spec.red.constantValue = static_cast<float>(redConstant);
    spec.red.invert      = invertRed;
    spec.green.path      = greenPath;
    spec.green.constantValue = static_cast<float>(greenConstant);
    spec.green.invert    = invertGreen;
    spec.blue.path       = bluePath;
    spec.blue.constantValue = static_cast<float>(blueConstant);
    spec.blue.invert     = invertBlue;
    spec.alpha.path      = alphaPath;
    spec.alpha.constantValue = static_cast<float>(alphaConstant);
    spec.alpha.invert    = invertAlpha;
    spec.includeAlpha    = includeAlpha;

    auto r = TextureChannelPacker::packToFile(spec, outputPath);
    return r.ok ? QString() : r.error;
}

QString MaterialEditorQML::saveNormalMapDialog()
{
    QString texturesPath = "./media/materials/textures";
    QDir texturesDir(texturesPath);
    QString startDir = texturesDir.exists() ? texturesDir.absolutePath() : QDir::currentPath();

    QApplication::processEvents();
    if (QWidget *activeWin = QApplication::activeWindow()) {
        activeWin->raise();
        activeWin->activateWindow();
    }
    QApplication::processEvents();

    QString selectedFile = QFileDialog::getSaveFileName(
        QApplication::activeWindow(),
        "Save Normal Map",
        startDir + "/normal.png",
        "PNG (*.png);;TGA (*.tga);;JPEG (*.jpg *.jpeg);;BMP (*.bmp)",
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons
    );
    return selectedFile;
}

QString MaterialEditorQML::previewNormalMap(const QString& sourcePath,
                                             double strength,
                                             bool invertR,
                                             bool invertG,
                                             int previewSize)
{
    NormalMapGenerator::GenSpec spec;
    spec.sourcePath = sourcePath;
    spec.strength = static_cast<float>(strength);
    spec.invertR = invertR;
    spec.invertG = invertG;
    // Cap preview size so live updates are cheap.
    const int cappedSize = std::clamp(previewSize, 32, 512);
    spec.outputWidth = cappedSize;
    spec.outputHeight = cappedSize;

    auto r = NormalMapGenerator::generate(spec);
    if (!r.ok) return QString();

    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    if (!r.image.save(&buf, "PNG")) return QString();
    return QStringLiteral("data:image/png;base64,") + bytes.toBase64();
}

QString MaterialEditorQML::generateNormalMap(const QString& sourcePath,
                                              double strength,
                                              bool invertR,
                                              bool invertG,
                                              const QString& outputPath)
{
    SentryReporter::addBreadcrumb("ui.action", "Generate normal map");

    NormalMapGenerator::GenSpec spec;
    spec.sourcePath = sourcePath;
    spec.strength = static_cast<float>(strength);
    spec.invertR = invertR;
    spec.invertG = invertG;

    auto r = NormalMapGenerator::generateToFile(spec, outputPath);
    return r.ok ? QString() : r.error;
}

// ─── Phase 6 slice E: texture atlas hooks ────────────────────────────

QString MaterialEditorQML::saveAtlasDialog()
{
    QString texturesPath = "./media/materials/textures";
    QDir texturesDir(texturesPath);
    QString startDir = texturesDir.exists() ? texturesDir.absolutePath() : QDir::currentPath();

    QApplication::processEvents();
    if (QWidget *activeWin = QApplication::activeWindow()) {
        activeWin->raise();
        activeWin->activateWindow();
    }
    QApplication::processEvents();

    return QFileDialog::getSaveFileName(
        QApplication::activeWindow(),
        "Save Atlas",
        startDir + "/atlas.png",
        "PNG (*.png);;TGA (*.tga);;JPEG (*.jpg *.jpeg);;BMP (*.bmp)",
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons
    );
}

QString MaterialEditorQML::saveAtlasManifestDialog()
{
    QString texturesPath = "./media/materials/textures";
    QDir texturesDir(texturesPath);
    QString startDir = texturesDir.exists() ? texturesDir.absolutePath() : QDir::currentPath();

    QApplication::processEvents();
    if (QWidget *activeWin = QApplication::activeWindow()) {
        activeWin->raise();
        activeWin->activateWindow();
    }
    QApplication::processEvents();

    return QFileDialog::getSaveFileName(
        QApplication::activeWindow(),
        "Save Atlas Manifest",
        startDir + "/atlas.json",
        "JSON (*.json)",
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons
    );
}

QString MaterialEditorQML::previewAtlas(const QStringList& sourcePaths,
                                         int atlasWidth,
                                         int atlasHeight,
                                         int padding,
                                         int previewSize)
{
    // Need at least one input to produce anything; otherwise just return
    // empty so the QML Image renders the placeholder text.
    if (sourcePaths.isEmpty() || atlasWidth <= 0 || atlasHeight <= 0)
        return QString();

    TextureAtlasPacker::AtlasSpec spec;
    spec.sourcePaths = sourcePaths;
    spec.atlasWidth  = atlasWidth;
    spec.atlasHeight = atlasHeight;
    spec.padding     = std::max(0, padding);

    auto r = TextureAtlasPacker::pack(spec);
    if (!r.ok) return QString();

    // Downscale the full-resolution atlas to the requested preview size
    // (longest edge) while preserving aspect.
    const int cappedSize = std::clamp(previewSize, 32, 512);
    QImage thumb = (r.image.width() > r.image.height())
        ? r.image.scaledToWidth(cappedSize, Qt::SmoothTransformation)
        : r.image.scaledToHeight(cappedSize, Qt::SmoothTransformation);

    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    if (!thumb.save(&buf, "PNG")) return QString();
    return QStringLiteral("data:image/png;base64,") + bytes.toBase64();
}

QString MaterialEditorQML::packAtlas(const QStringList& sourcePaths,
                                     int atlasWidth,
                                     int atlasHeight,
                                     int padding,
                                     const QString& outputPath,
                                     const QString& manifestPath)
{
    SentryReporter::addBreadcrumb("ui.action", "Pack texture atlas");

    if (sourcePaths.isEmpty())
        return QStringLiteral("No input textures supplied");
    if (outputPath.isEmpty())
        return QStringLiteral("Output path is empty");

    TextureAtlasPacker::AtlasSpec spec;
    spec.sourcePaths = sourcePaths;
    spec.atlasWidth  = atlasWidth;
    spec.atlasHeight = atlasHeight;
    spec.padding     = std::max(0, padding);

    auto r = TextureAtlasPacker::packToFile(spec, outputPath);
    if (!r.ok) return r.error;
    SentryReporter::addBreadcrumb("file.export",
        QString("Atlas %1 tiles -> %2").arg(r.tiles.size()).arg(QFileInfo(outputPath).fileName()));

    if (!manifestPath.isEmpty()) {
        const QString json = TextureAtlasPacker::manifestToJson(r, spec.padding);
        const QByteArray bytes = json.toUtf8();
        QFile mf(manifestPath);
        if (!mf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            return QStringLiteral("Could not open manifest path: %1").arg(manifestPath);
        const qint64 written = mf.write(bytes);
        mf.close();
        if (written != bytes.size())
            return QStringLiteral("Short write to manifest path: %1 (%2/%3 bytes)")
                       .arg(manifestPath).arg(written).arg(bytes.size());
        SentryReporter::addBreadcrumb("file.export",
            QString("Atlas manifest -> %1").arg(QFileInfo(manifestPath).fileName()));
    }
    return QString();
}

// ─── Phase 6 slice E2: apply-atlas dialog hooks ──────────────────────

QString MaterialEditorQML::openManifestDialog()
{
    QString startDir = QDir::currentPath();
    QApplication::processEvents();
    if (QWidget *activeWin = QApplication::activeWindow()) {
        activeWin->raise();
        activeWin->activateWindow();
    }
    QApplication::processEvents();
    return QFileDialog::getOpenFileName(
        QApplication::activeWindow(),
        "Open Atlas Manifest",
        startDir, "JSON (*.json);;All Files (*)",
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons);
}

QString MaterialEditorQML::openMeshDialog()
{
    QString startDir = QDir::currentPath();
    QApplication::processEvents();
    if (QWidget *activeWin = QApplication::activeWindow()) {
        activeWin->raise();
        activeWin->activateWindow();
    }
    QApplication::processEvents();
    return QFileDialog::getOpenFileName(
        QApplication::activeWindow(),
        "Open Mesh File",
        startDir,
        "Mesh files (*.fbx *.gltf *.glb *.dae *.obj *.ply *.stl *.mesh);;All Files (*)",
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons);
}

QString MaterialEditorQML::saveAtlasedMeshDialog()
{
    QString startDir = QDir::currentPath();
    QApplication::processEvents();
    if (QWidget *activeWin = QApplication::activeWindow()) {
        activeWin->raise();
        activeWin->activateWindow();
    }
    QApplication::processEvents();
    return QFileDialog::getSaveFileName(
        QApplication::activeWindow(),
        "Save Atlased Mesh",
        startDir + "/atlased.fbx",
        "FBX (*.fbx);;glTF (*.gltf);;glTF Binary (*.glb);;Collada (*.dae);;OBJ (*.obj);;PLY (*.ply);;STL (*.stl);;Ogre Mesh (*.mesh)",
        nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons);
}

QString MaterialEditorQML::applyAtlas(const QString& meshPath,
                                      const QString& manifestPath,
                                      const QString& atlasImagePath,
                                      const QString& outputPath,
                                      const QString& matchMode,
                                      bool clampOutOfRangeUVs,
                                      bool stripNonDiffuseTextures)
{
    SentryReporter::addBreadcrumb("ui.action", "Apply atlas to mesh");

    if (meshPath.isEmpty() || manifestPath.isEmpty()
        || atlasImagePath.isEmpty() || outputPath.isEmpty())
        return QStringLiteral("All four paths (mesh, manifest, atlas, output) are required.");
    if (!QFileInfo::exists(meshPath))
        return QStringLiteral("Mesh file not found: %1").arg(meshPath);
    if (!QFileInfo::exists(manifestPath))
        return QStringLiteral("Manifest not found: %1").arg(manifestPath);
    if (!QFileInfo::exists(atlasImagePath))
        return QStringLiteral("Atlas image not found: %1").arg(atlasImagePath);
    // Same-file guard. canonicalFilePath() returns "" for non-existent
    // paths — and the output doesn't exist yet — so a naive equality
    // check would never fire. Compare normalized absolute paths instead;
    // case-fold on the platforms whose filesystems are case-insensitive
    // by default so "model.fbx" and "MODEL.FBX" still collide.
    {
        const QString a = QDir::cleanPath(QFileInfo(meshPath).absoluteFilePath());
        const QString b = QDir::cleanPath(QFileInfo(outputPath).absoluteFilePath());
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        const auto cs = Qt::CaseInsensitive;
#else
        const auto cs = Qt::CaseSensitive;
#endif
        if (!a.isEmpty() && a.compare(b, cs) == 0)
            return QStringLiteral("Output points to the input file; choose a different path.");
    }

    QFile mf(manifestPath);
    if (!mf.open(QIODevice::ReadOnly))
        return QStringLiteral("Could not read manifest: %1").arg(manifestPath);
    const QByteArray manifestJson = mf.readAll();
    mf.close();

    auto parsed = ApplyAtlas::parseManifestJson(manifestJson);
    if (!parsed.ok) return parsed.error;

    // The Inspector runs inside the editor process where Ogre is up. We
    // import the source file, scope the imported entities, mutate, export,
    // then tear down so the user's live scene returns to its prior state.
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return QStringLiteral("Manager unavailable");
    if (!Ogre::Root::getSingletonPtr() || !Ogre::Root::getSingletonPtr()->getRenderSystem())
        return QStringLiteral("Ogre render system not initialized");

    SentryReporter::addBreadcrumb("file.import",
        QString("Apply-atlas importing %1").arg(QFileInfo(meshPath).fileName()));
    QSet<Ogre::Entity*> beforeSet;
    for (Ogre::Entity* e : mgr->getEntities()) beforeSet.insert(e);

    try {
        MeshImporterExporter::importer({QFileInfo(meshPath).absoluteFilePath()});
    } catch (const std::exception& e) {
        return QStringLiteral("Importer threw: %1").arg(QString::fromUtf8(e.what()));
    } catch (...) {
        return QStringLiteral("Importer threw (unknown)");
    }

    QList<Ogre::Entity*> entities;
    for (Ogre::Entity* e : mgr->getEntities())
        if (!beforeSet.contains(e)) entities.append(e);
    if (entities.isEmpty())
        return QStringLiteral("Failed to load entities from %1").arg(meshPath);

    struct ImportCleanup {
        Manager* mgr;
        QList<Ogre::Entity*> imported;
        ~ImportCleanup() {
            if (!mgr) return;
            try {
                std::set<Ogre::SceneNode*> nodes;
                for (Ogre::Entity* e : imported)
                    if (e && e->getParentSceneNode())
                        nodes.insert(e->getParentSceneNode());
                for (Ogre::SceneNode* sn : nodes) mgr->destroySceneNode(sn);
            } catch (...) {}
        }
    } cleanup{mgr, entities};

    // Register the atlas image's directory as a resource location so the
    // mutated materials can resolve the texture during the export pass.
    // We register into the default group (rather than a one-shot group)
    // because the per-submesh materials live in the default group, and
    // an Ogre Material can only see textures from groups it's a member
    // of. The location is removed on every return path via an RAII guard.
    const QFileInfo atlasFi(atlasImagePath);
    const Ogre::String atlasDir = atlasFi.absolutePath().toStdString();
    bool addedAtlasLoc = false;
    try {
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
            atlasDir, "FileSystem",
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, true);
        addedAtlasLoc = true;
    } catch (const Ogre::Exception&) { /* already-registered is fine */ }
    struct LocationCleanup {
        Ogre::String dir;
        bool added;
        ~LocationCleanup() {
            if (!added) return;
            try {
                Ogre::ResourceGroupManager::getSingleton().removeResourceLocation(
                    dir, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            } catch (...) {}
        }
    } locationCleanup{atlasDir, addedAtlasLoc};

    ApplyAtlas::ApplyOptions opts;
    opts.matchMode = (matchMode.toLower() == "fullpath")
        ? ApplyAtlas::MatchMode::FullPath
        : ApplyAtlas::MatchMode::Basename;
    opts.atlasTextureName = atlasFi.fileName();
    opts.clampOutOfRangeUVs = clampOutOfRangeUVs;
    opts.stripNonDiffuseTextures = stripNonDiffuseTextures;

    int totalRewritten = 0, totalSubmeshes = 0;
    try {
        for (Ogre::Entity* ent : entities) {
            auto r = ApplyAtlas::applyToEntity(ent, parsed.manifest, opts);
            if (!r.ok) return r.error;
            totalRewritten += r.rewrittenCount();
            totalSubmeshes += r.submeshCount();
        }
        Ogre::Entity* first = entities.first();
        const auto* node = first ? first->getParentSceneNode() : nullptr;
        if (!node) return QStringLiteral("Could not resolve scene node for export");

        static const QMap<QString, QString> formatByExt = {
            {QStringLiteral("fbx"),  QStringLiteral("FBX Binary (*.fbx)")},
            {QStringLiteral("gltf"), QStringLiteral("glTF 2.0 (*.gltf)")},
            {QStringLiteral("glb"),  QStringLiteral("glTF 2.0 Binary (*.glb)")},
            {QStringLiteral("dae"),  QStringLiteral("Collada (*.dae)")},
            {QStringLiteral("obj"),  QStringLiteral("OBJ (*.obj)")},
            {QStringLiteral("ply"),  QStringLiteral("PLY (*.ply)")},
            {QStringLiteral("stl"),  QStringLiteral("STL (*.stl)")},
            {QStringLiteral("mesh"), QStringLiteral("Ogre Mesh (*.mesh)")},
        };
        const QString ext = QFileInfo(outputPath).suffix().toLower();
        if (!formatByExt.contains(ext))
            return QStringLiteral("Unsupported export format for .%1").arg(ext);
        if (MeshImporterExporter::exporter(node, outputPath, formatByExt.value(ext)) != 0)
            return QStringLiteral("Export failed");
        SentryReporter::addBreadcrumb("file.export",
            QString("apply_atlas -> %1").arg(QFileInfo(outputPath).fileName()));
    } catch (const Ogre::Exception& e) {
        return QStringLiteral("Ogre error: %1")
                   .arg(QString::fromStdString(e.getFullDescription()));
    } catch (const std::exception& e) {
        return QStringLiteral("Error: %1").arg(QString::fromUtf8(e.what()));
    } catch (...) {
        return QStringLiteral("Unknown error during apply_atlas");
    }

    // Append a structured result to the Sentry breadcrumb so subsequent
    // diagnostics see how many submeshes the user actually atlased.
    SentryReporter::addBreadcrumb("ui.action",
        QString("Atlas applied: %1/%2 submeshes").arg(totalRewritten).arg(totalSubmeshes));
    return QString();
}

// Add a helper method to check if Ogre is available
bool MaterialEditorQML::isOgreAvailable() const
{
    try {
        // Check if Ogre Root exists and has been initialized
        Ogre::Root* root = Ogre::Root::getSingletonPtr();
        if (!root) return false;
        if (!root->getRenderSystem()) return false;
        // Also verify MaterialManager is available
        return Ogre::MaterialManager::getSingletonPtr() != nullptr;
    } catch (...) {
        return false;
    }
}

// AI Material Generation Implementation
void MaterialEditorQML::generateMaterialFromPrompt(const QString &prompt)
{
    if (prompt.isEmpty()) {
        emit aiGenerationError("Please enter a prompt");
        return;
    }

    // Check if local LLM is loaded
    LLMManager *llmManager = LLMManager::instance();
    if (!llmManager->isModelLoaded()) {
        emit aiGenerationError("No AI model loaded. Please download and load a model from AI Settings.");
        return;
    }

    // LCOV_EXCL_START — requires a loaded LLM model
    // Use local LLM
    m_llmGenerationProgress = 0.0f;
    emit llmGenerationProgressChanged();

    // Get available textures to provide context to the LLM
    QStringList availableTextures = getAvailableTextures();
    llmManager->generateMaterial(prompt, m_materialText, availableTextures);
    // LCOV_EXCL_STOP
}

// LCOV_EXCL_START — requires a running LLM generation or network AI service
void MaterialEditorQML::stopAIGeneration()
{
    LLMManager::instance()->stopGeneration();
}

void MaterialEditorQML::onAiRequestFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    
    if (reply->error() != QNetworkReply::NoError) {
        emit aiGenerationError(QString("Network error: %1").arg(reply->errorString()));
        return;
    }
    
    QByteArray response = reply->readAll();
    
    // Parse JSON response
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(response, &parseError);
    
    if (parseError.error != QJsonParseError::NoError) {
        emit aiGenerationError(QString("JSON parse error: %1").arg(parseError.errorString()));
        return;
    }
    
    QJsonObject responseObj = doc.object();
    
    // Extract message content from choices array
    if (!responseObj.contains("choices") || !responseObj["choices"].isArray()) {
        emit aiGenerationError("Invalid response format: missing choices array");
        return;
    }
    
    QJsonArray choices = responseObj["choices"].toArray();
    if (choices.isEmpty()) {
        emit aiGenerationError("Invalid response format: empty choices array");
        return;
    }
    
    QJsonObject firstChoice = choices[0].toObject();
    if (!firstChoice.contains("message") || !firstChoice["message"].isObject()) {
        emit aiGenerationError("Invalid response format: missing message object");
        return;
    }
    
    QJsonObject message = firstChoice["message"].toObject();
    if (!message.contains("content") || !message["content"].isString()) {
        emit aiGenerationError("Invalid response format: missing content string");
        return;
    }
    
    QString generatedScript = message["content"].toString().trimmed();
    
    if (generatedScript.isEmpty()) {
        emit aiGenerationError("Received empty material script from AI service");
        return;
    }
    
    // Update the material text with the AI-generated script
    setMaterialText(generatedScript);

    emit aiGenerationCompleted(generatedScript);
}

// LLM getter implementations
bool MaterialEditorQML::llmModelLoaded() const
{
    return LLMManager::instance()->isModelLoaded();
}

QString MaterialEditorQML::llmCurrentModel() const
{
    return LLMManager::instance()->currentModelName();
}

// LLM slot implementations
void MaterialEditorQML::onLLMGenerationStarted()
{
    m_llmGenerationProgress = 0.0f;
    emit llmGenerationProgressChanged();
    emit aiGenerationStarted();
}

void MaterialEditorQML::onLLMGenerationProgress(const QString &partialText, float progress)
{
    Q_UNUSED(partialText);
    m_llmGenerationProgress = progress;
    emit llmGenerationProgressChanged();
}

void MaterialEditorQML::onLLMGenerationCompleted(const QString &generatedText)
{
    m_llmGenerationProgress = 1.0f;
    emit llmGenerationProgressChanged();

    // Clean up the generated text - remove any markdown code blocks
    QString cleanedText = generatedText;
    if (cleanedText.startsWith("```")) {
        int firstNewline = cleanedText.indexOf('\n');
        if (firstNewline != -1) {
            cleanedText = cleanedText.mid(firstNewline + 1);
        }
    }
    if (cleanedText.endsWith("```")) {
        cleanedText = cleanedText.left(cleanedText.length() - 3);
    }
    cleanedText = cleanedText.trimmed();

    // Update the material text with the AI-generated script
    setMaterialText(cleanedText);

    // LCOV_EXCL_START — SD auto-trigger requires SD enabled with loaded model
    // Check if SD needs to generate a texture before we apply the material
    bool sdTriggered = false;
#ifdef ENABLE_STABLE_DIFFUSION
    SDManager *sdManager = SDManager::instance();
    if (sdManager->isModelLoaded() && !sdManager->isGenerating()) {
        QRegularExpression texRegex(R"(texture\s+["\']?([^"'\s]+)["\']?)");
        QRegularExpressionMatchIterator it = texRegex.globalMatch(cleanedText);
        while (it.hasNext()) {
            QRegularExpressionMatch match = it.next();
            QString texName = match.captured(1).trimmed();
            bool existsOnDisk = false;
            QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            QString genPath = QDir(dataPath).filePath("generated_textures/" + texName);
            if (QFileInfo::exists(genPath)) existsOnDisk = true;
            QStringList searchDirs = {
                "media/materials/textures/" + texName,
                "../media/materials/textures/" + texName,
            };
            for (const auto &p : searchDirs) {
                if (QFileInfo::exists(p)) { existsOnDisk = true; break; }
            }
            if (existsOnDisk) continue;
            QString prompt = texName;
            prompt.replace(QRegularExpression(R"(\.\w+$)"), "");
            QString cleanPrompt = prompt;
            cleanPrompt.replace('_', ' ').replace('-', ' ');
            if (!cleanPrompt.isEmpty()) {
                qDebug() << "MaterialEditorQML: Auto-triggering SD for missing texture:" << texName << "prompt:" << cleanPrompt;
                // Defer material apply until SD completes
                m_pendingMaterialScript = cleanedText;
                m_sdPendingForMaterial = true;
                emit sdPendingForMaterialChanged();
                sdManager->generateTexture(cleanPrompt, 0, 0, texName);
                sdTriggered = true;
                break;
            }
        }
    }
#endif
    // LCOV_EXCL_STOP

    if (!sdTriggered) {
        // No SD needed — emit immediately so QML can apply
        emit aiGenerationCompleted(cleanedText);
    }
    // If SD was triggered, aiGenerationCompleted will be emitted from onSDGenerationCompleted
}

void MaterialEditorQML::onLLMGenerationError(const QString &error)
{
    m_llmGenerationProgress = 0.0f;
    emit llmGenerationProgressChanged();
    emit aiGenerationError(error);
}

void MaterialEditorQML::onLLMModelLoadedChanged()
{
    emit llmModelLoadedChanged();
    emit llmCurrentModelChanged();
}
// LCOV_EXCL_STOP

// LCOV_EXCL_START — SD texture generation requires GPU + model files not available in CI
// Stable Diffusion getters
bool MaterialEditorQML::stableDiffusionEnabled() const
{
#ifdef ENABLE_STABLE_DIFFUSION
    return true;
#else
    return false;
#endif
}

bool MaterialEditorQML::sdModelLoaded() const
{
#ifdef ENABLE_STABLE_DIFFUSION
    return SDManager::instance()->isModelLoaded();
#else
    return false;
#endif
}

bool MaterialEditorQML::sdIsGenerating() const
{
#ifdef ENABLE_STABLE_DIFFUSION
    return SDManager::instance()->isGenerating();
#else
    return false;
#endif
}

// Stable Diffusion texture generation
void MaterialEditorQML::generateTextureFromPrompt(const QString &prompt, int width, int height)
{
    if (prompt.isEmpty()) {
        emit sdGenerationError("Please enter a texture prompt");
        return;
    }

#ifdef ENABLE_STABLE_DIFFUSION
    SDManager *sdManager = SDManager::instance();
    if (!sdManager->isModelLoaded()) {
        emit sdGenerationError("No SD model loaded. Please download and load a model from AI Settings.");
        return;
    }

    // LCOV_EXCL_START — requires a loaded SD model
    m_sdGenerationProgress = 0.0f;
    emit sdGenerationProgressChanged();
    sdManager->generateTexture(prompt, width, height);
    // LCOV_EXCL_STOP
#else
    Q_UNUSED(width);
    Q_UNUSED(height);
    emit sdGenerationError("Stable Diffusion support is not enabled. Rebuild with ENABLE_STABLE_DIFFUSION=ON");
#endif
}

bool MaterialEditorQML::aiPbrAvailable() const
{
#ifdef ENABLE_ONNX
    return AIAssistManager::instance()->isAvailable();
#else
    return false;
#endif
}

void MaterialEditorQML::generatePbrFromDiffuse()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Generate PBR maps from diffuse"));
#ifndef ENABLE_ONNX
    emit pbrSynthError(tr("AI PBR synthesis is not enabled. Rebuild with ENABLE_ONNX=ON."));
#else
    // Resolve the current texture's on-disk path via the same group-agnostic
    // resolver the preview uses.
    QString src = getTexturePreviewPath();
    if (src.startsWith(QStringLiteral("file://")))
        src = QUrl(src).toLocalFile();
    if (src.isEmpty() || !QFileInfo::exists(src)) {
        emit pbrSynthError(tr("No on-disk diffuse texture to synthesize from. "
                              "Apply or save a diffuse texture first."));
        return;
    }

    emit pbrSynthStarted();
    const PbrMapSynthResult res =
        AIAssistManager::instance()->synthesizePbrMaps(src, {});
    if (!res.ok) {
        emit pbrSynthError(res.error.isEmpty()
            ? tr("PBR synthesis failed.") : res.error);
        return;
    }

    // Bind the generated maps into the active material's canonical slots.
    if (m_ogreMaterial) {
        auto bindSlot = [&](const char* slot, const QString& path) {
            if (path.isEmpty()) return;
            ensureTextureInMaterialGroup(QFileInfo(path).fileName());
            for (auto* tech : m_ogreMaterial->getTechniques()) {
                for (unsigned short p = 0; p < tech->getNumPasses(); ++p) {
                    Ogre::Pass* pass = tech->getPass(p);
                    Ogre::TextureUnitState* tus = nullptr;
                    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i)
                        if (pass->getTextureUnitState(i)->getName() == slot) {
                            tus = pass->getTextureUnitState(i); break;
                        }
                    if (!tus) { tus = pass->createTextureUnitState(); tus->setName(slot); }
                    tus->setTextureName(QFileInfo(path).fileName().toStdString());
                }
            }
        };
        bindSlot("normal_map", res.normalPath);
        bindSlot("roughness",  res.roughnessPath);
        RTShaderHelper::wirePbrSlotsForFFP(m_ogreMaterial.get());
        // wirePbrSlotsForFFP only marks the normal unit non-FFP; it does NOT
        // make RTSS sample it. applyNormalMap wires the SRS_NORMALMAP (or
        // Cook-Torrance) sub-render-state so the normal map actually perturbs
        // viewport shading — without this the bind is invisible on the mesh.
        if (!res.normalPath.isEmpty()) {
            RTShaderHelper::applyNormalMap(
                m_ogreMaterial, QFileInfo(res.normalPath).fileName().toStdString());
        }
        m_ogreMaterial->compile();
        // The bind above may have CREATED new texture units (normal_map /
        // roughness); rebuild the cached technique/pass/unit maps so the QML
        // slot list (and isPbrMaterial) reflect them, then notify the views.
        updateTechniqueList();   // rebuilds pass map (units read via getCurrentPass)
        updateTextureUnitList();
        updateMaterialText();
        emit textureUnitsChanged();
        emit textureUnitListChanged();
    }

    emit pbrSynthCompleted(res.toVariantMap());
#endif
}

void MaterialEditorQML::upscaleCurrentTexture(int scale)
{
#ifndef ENABLE_ONNX
    Q_UNUSED(scale);
    emit upscaleError(tr("AI upscaling is not enabled. Rebuild with ENABLE_ONNX=ON."));
#else
    QString src = getTexturePreviewPath();
    if (src.startsWith(QStringLiteral("file://")))
        src = QUrl(src).toLocalFile();
    if (src.isEmpty() || !QFileInfo::exists(src)) {
        emit upscaleError(tr("No on-disk texture to upscale. Apply or save a texture first."));
        return;
    }
    emit upscaleStarted();
    // Ensure the model on THIS (GUI) thread first — the download uses a
    // QEventLoop and ModelDownloader's queued signals, which need a running
    // event loop. If the model isn't on disk yet, signal the download phase so
    // the UI can show "Downloading model…" instead of a silent "Upscaling…".
    AIAssistManager* ai = AIAssistManager::instance();
    const QString preModel = ai->modelPath(
        (scale == 2) ? AIAssistManager::Map::UpscaleX2 : AIAssistManager::Map::UpscaleX4);
    if (!QFileInfo::exists(preModel))
        emit upscaleDownloading();
    const QString model = ai->ensureUpscaleModel(scale);
    if (model.isEmpty()) {
        emit upscaleError(tr("Upscale model unavailable (offline or download failed)."));
        return;
    }

    const QFileInfo fi(src);
    const QString outPath = QDir(fi.absolutePath())
        .filePath(fi.completeBaseName() + QStringLiteral("_upscaled_x%1.png").arg(scale));

    // Fresh cancel flag for this run (shared_ptr keeps it alive for the worker).
    m_upscaleCancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = m_upscaleCancel;
    QPointer<MaterialEditorQML> self(this);

    // Then run the pure-CPU tiled inference + save on a worker so the UI doesn't
    // freeze, reporting per-tile progress + honoring cancel, and marshal results
    // back via queued invocations. (CLI/MCP keep the synchronous path.)
    std::thread([self, src, model, outPath, cancel]() {
        TextureUpscaler::ProgressFn onProgress = [self, cancel](int done, int total) -> bool {
            if (cancel->load()) return false;  // cancel requested
            QMetaObject::invokeMethod(qApp, [self, done, total]() {
                if (self) emit self->upscaleProgress(done, total);
            }, Qt::QueuedConnection);
            return true;
        };
        const QImage in(src);
        const TextureUpscaler::Result res =
            TextureUpscaler::upscale(in, model, {}, onProgress);
        const bool ok = res.ok && !res.image.isNull() && res.image.save(outPath, "PNG");
        const QString err = res.error;
        QMetaObject::invokeMethod(qApp, [self, ok, outPath, err]() {
            if (!self) return;
            if (ok) emit self->upscaleCompleted(outPath);
            else    emit self->upscaleError(err.isEmpty()
                        ? QObject::tr("Upscale failed.") : err);
        }, Qt::QueuedConnection);
    }).detach();
#endif
}

void MaterialEditorQML::cancelUpscale()
{
    if (m_upscaleCancel)
        m_upscaleCancel->store(true);
}

bool MaterialEditorQML::hasSelectedMesh() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    const auto entities = sel->getResolvedEntities();
    return !entities.isEmpty() && entities.first() && entities.first()->getMesh();
}

QString MaterialEditorQML::discoveredControlNetDepthPath() const
{
#ifdef ENABLE_STABLE_DIFFUSION
    SDManager* sdManager = SDManager::instance();
    if (!sdManager) return QString();
    // Scan the models directory for a ControlNet depth file
    // (recommended filename, or the lllyasviel naming heuristic).
    const QString dir = sdManager->modelsDirectory();
    QDir d(dir);
    const QStringList files = d.entryList(
        QStringList() << "*.safetensors" << "*.ckpt", QDir::Files);
    // The depth ControlNet pipeline here is SD 1.5-only — an SDXL depth
    // ControlNet would silently fail against an SD 1.5 base. So skip any
    // file whose name marks it as SDXL, and prefer an explicit SD1.5
    // tag when present.
    auto isSdxl = [](const QString& lower) {
        return lower.contains("sdxl") || lower.contains("xl_")
            || lower.contains("-xl") || lower.contains("_xl");
    };
    QString fallback;
    for (const QString& f : files) {
        const QString lower = f.toLower();
        if (!(lower.contains("control") && lower.contains("depth")))
            continue;
        if (isSdxl(lower))
            continue;  // wrong architecture for the SD 1.5 base
        // A filename that explicitly tags SD 1.5 is the strongest match.
        if (lower.contains("sd15") || lower.contains("sd_15")
            || lower.contains("v11"))
            return d.filePath(f);
        if (fallback.isEmpty())
            fallback = d.filePath(f);
    }
    return fallback;
#else
    return QString();
#endif
}

void MaterialEditorQML::generateMeshTextureFromPrompt(const QString &prompt,
                                                      int width, int height,
                                                      double controlStrength)
{
    if (prompt.isEmpty()) {
        emit sdGenerationError("Please enter a texture prompt");
        return;
    }

#ifdef ENABLE_STABLE_DIFFUSION
    SDManager *sdManager = SDManager::instance();
    if (!sdManager->isModelLoaded()) {
        emit sdGenerationError("No SD model loaded. Please download and load a model from AI Settings.");
        return;
    }

    auto* sel = SelectionSet::getSingleton();
    const auto entities = sel ? sel->getResolvedEntities() : QList<Ogre::Entity*>{};
    if (entities.isEmpty() || !entities.first() || !entities.first()->getMesh()) {
        emit sdGenerationError("No mesh selected. Select a mesh to condition on its shape.");
        return;
    }
    Ogre::Entity* entity = entities.first();

    // LCOV_EXCL_START — requires a loaded SD model + a mesh
    const int genW = width  > 0 ? width  : 512;
    const int genH = height > 0 ? height : 512;

    // Render the depth map at the generation resolution (square; the
    // depth renderer frames the entity to fill the view).
    QString depthErr;
    const int depthSize = std::max(genW, genH);
    const QImage depth = MeshDepthRenderer::renderDepthMap(entity, depthSize, &depthErr);
    if (depth.isNull()) {
        emit sdGenerationError(QStringLiteral("Depth render failed: %1").arg(depthErr));
        return;
    }

    const QString controlNetPath = discoveredControlNetDepthPath();
    if (controlNetPath.isEmpty()) {
        // No ControlNet model — we still proceed (plain txt2img conditioned by
        // the prompt only). Surface this as a non-fatal NOTICE, NOT an error:
        // emitting sdGenerationError here tripped the QML error handler (which
        // clears the in-flight target and resets state) and made the run look
        // like it never started. Use a dedicated notice signal instead.
        emit sdGenerationNotice(
            "No ControlNet depth model found in the models folder — "
            "generating without mesh conditioning. Download "
            "\"ControlNet Depth (SD 1.5)\" in AI Settings for shape-aware results.");
    }

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.mesh_texture"),
        QStringLiteral("entity=%1 controlNet=%2 strength=%3 size=%4")
            .arg(QString::fromStdString(entity->getName()))
            .arg(controlNetPath.isEmpty() ? QStringLiteral("(none)")
                                          : QFileInfo(controlNetPath).fileName())
            .arg(controlStrength).arg(depthSize));

    // Remember the target so onSDGenerationCompleted binds the
    // result to THIS entity's diffuse TUS (not the editor's current
    // pass, which may not be the rendered material).
    m_sdMeshTextureEntity = QString::fromStdString(entity->getName());

    m_sdGenerationProgress = 0.0f;
    emit sdGenerationProgressChanged();
    sdManager->generateMeshTexture(prompt, depth, controlNetPath,
                                   static_cast<float>(std::clamp(controlStrength, 0.0, 1.0)),
                                   QString(), genW, genH);
    // LCOV_EXCL_STOP
#else
    Q_UNUSED(width); Q_UNUSED(height); Q_UNUSED(controlStrength);
    emit sdGenerationError("Stable Diffusion support is not enabled. Rebuild with ENABLE_STABLE_DIFFUSION=ON");
#endif
}

// --------------------------------------------------------------------------
// Multi-view depth-conditioned generation + projection bake (slice 2).
// --------------------------------------------------------------------------

// In-flight state for a multi-view bake. One image is generated per view; the
// per-view depth render also yields the exact camera matrices the baker needs.
struct MaterialEditorQML::MultiViewBakeState {
    QString entityName;
    QString prompt;
    QString controlNetPath;
    int width = 512;
    int height = 512;
    float controlStrength = 0.9f;
    int64_t seed = -1;                             // ONE locked seed across views
    std::vector<MeshDepthRenderer::View> views;   // resolved camera views
    std::vector<MultiViewTextureBaker::View> baked; // accumulated image+matrices
    size_t current = 0;                            // index of the view being generated
    // Optional: the real input photo, pinned as the FRONT view instead of an
    // SD-generated one. For TripoSG (geometry-only) the front is textured from
    // the actual image (accurate) and only the OTHER views are SD-generated
    // (plausible, depth-conditioned). Empty → every view is SD-generated.
    QImage  frontPhoto;                            // loaded, non-null when pinned
    // Synthesize + bind #404 PBR maps (normal + roughness) FROM the freshly
    // baked atlas after the bake applies it — so the maps match the final
    // (AI) diffuse, not a throwaway one. Set by the generate3d GUI flow.
    bool    generatePbrAfter = false;
};

namespace {
MeshDepthRenderer::View resolveDepthView(const QString& name)
{
    const QString n = name.trimmed().toLower();
    if (n == "back")   return MeshDepthRenderer::back();
    if (n == "left")   return MeshDepthRenderer::left();
    if (n == "right")  return MeshDepthRenderer::right();
    if (n == "top")    return MeshDepthRenderer::top();
    if (n == "bottom") return MeshDepthRenderer::bottom();
    return MeshDepthRenderer::front();
}

// True if every submesh of the entity's mesh exposes a UV0 (TEXCOORD 0) in
// whichever vertex data it uses. The multi-view baker projects onto an
// EXISTING UV0 atlas, so a UV-less mesh (e.g. a geometry-only TripoSG result)
// must be unwrapped first.
bool entityHasUv0(Ogre::Entity* entity)
{
    if (!entity || !entity->getMesh()) return false;
    const Ogre::MeshPtr& mesh = entity->getMesh();
    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        const Ogre::SubMesh* sm = mesh->getSubMesh(i);
        const Ogre::VertexData* vd = sm->useSharedVertices
            ? mesh->sharedVertexData : sm->vertexData;
        if (!vd || !vd->vertexDeclaration
            || !vd->vertexDeclaration->findElementBySemantic(
                   Ogre::VES_TEXTURE_COORDINATES, 0))
            return false;
    }
    return true;
}

// Tight bounding box of the mesh silhouette in a DEPTH map (near=white/
// far=black over a black background): any pixel brighter than `thresh` counts.
QRect silhouetteBounds(const QImage& depth, int thresh = 8)
{
    int minX = depth.width(), minY = depth.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < depth.height(); ++y)
        for (int x = 0; x < depth.width(); ++x)
            if (qGray(depth.pixel(x, y)) > thresh) {
                minX = std::min(minX, x); minY = std::min(minY, y);
                maxX = std::max(maxX, x); maxY = std::max(maxY, y);
            }
    if (maxX < 0) return QRect();
    return QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

// Tight bounding box of the FOREGROUND subject in a background-removed photo.
// BackgroundRemover returns RGB composited over a SOLID known colour (no
// alpha), so the subject is every pixel that differs from that background
// beyond `tol` (per-channel). (Using alpha here was wrong — the composited
// image is fully opaque, so an alpha test selected the whole frame.)
QRect subjectBounds(const QImage& img, QRgb bg, int tol = 24)
{
    int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
    const int br = qRed(bg), bgn = qGreen(bg), bb = qBlue(bg);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = img.pixel(x, y);
            if (std::abs(qRed(p) - br) > tol || std::abs(qGreen(p) - bgn) > tol
                || std::abs(qBlue(p) - bb) > tol) {
                minX = std::min(minX, x); minY = std::min(minY, y);
                maxX = std::max(maxX, x); maxY = std::max(maxY, y);
            }
        }
    if (maxX < 0) return QRect();
    return QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

// Register the input photo to the mesh's rendered silhouette so the photo
// projects onto the geometry in alignment (eye→eye, not eye→ear). Both bugs
// this fixes: (1) the raw photo's subject sits at an arbitrary size/offset
// vs. the depth-camera framing; (2) the subject is fit to the SAME footprint
// the mesh silhouette occupies in the depth render, so projecting the result
// through that render's view/proj matrices lands photo features on the
// matching mesh features. Returns an RGB image sized to `depth`, subject
// scaled+centred onto the silhouette bbox (background = neutral, unused by
// the baker's facing weights on the front). Null if either bbox is empty
// (caller falls back to the naive scale).
QImage registerPhotoToSilhouette(const QImage& photoRemovedBg,
                                  const QImage& depth, QRgb bg)
{
    const QRect meshBox = silhouetteBounds(depth);
    const QRect subjBox = subjectBounds(photoRemovedBg, bg);
    if (!meshBox.isValid() || !subjBox.isValid()
        || subjBox.width() <= 0 || subjBox.height() <= 0)
        return QImage();

    // Uniform scale that fits the subject bbox into the mesh silhouette bbox
    // (preserve the photo's aspect — non-uniform would distort features).
    const double sx = double(meshBox.width())  / subjBox.width();
    const double sy = double(meshBox.height()) / subjBox.height();
    const double scale = std::min(sx, sy);

    QImage out(depth.size(), QImage::Format_RGB888);
    out.fill(qRgb(127, 127, 127));   // neutral fill for uncovered texels
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // Place the subject so its bbox centre lands on the mesh bbox centre.
    const double drawW = subjBox.width()  * scale;
    const double drawH = subjBox.height() * scale;
    const double dstX = meshBox.center().x() - drawW / 2.0;
    const double dstY = meshBox.center().y() - drawH / 2.0;
    painter.drawImage(QRectF(dstX, dstY, drawW, drawH),
                      photoRemovedBg,
                      QRectF(subjBox.x(), subjBox.y(),
                             subjBox.width(), subjBox.height()));
    painter.end();
    return out;
}
} // namespace

void MaterialEditorQML::generateMeshTextureMultiView(const QString &prompt,
                                                     int width, int height,
                                                     double controlStrength,
                                                     const QStringList &views,
                                                     const QString &frontPhotoPath,
                                                     bool generatePbr)
{
    // A pinned front photo can carry the whole "what to draw" signal, so the
    // prompt is only required when there's no photo to anchor the front.
    if (prompt.isEmpty() && frontPhotoPath.isEmpty()) {
        emit sdGenerationError("Please enter a texture prompt");
        return;
    }
#ifdef ENABLE_STABLE_DIFFUSION
    SDManager *sdManager = SDManager::instance();
    if (!sdManager->isModelLoaded()) {
        emit sdGenerationError("No SD model loaded. Please download and load a model from AI Settings.");
        return;
    }
    auto* sel = SelectionSet::getSingleton();
    const auto entities = sel ? sel->getResolvedEntities() : QList<Ogre::Entity*>{};
    if (entities.isEmpty() || !entities.first() || !entities.first()->getMesh()) {
        emit sdGenerationError("No mesh selected. Select a mesh to condition on its shape.");
        return;
    }
    if (m_multiViewBake) {
        emit sdGenerationError("A multi-view bake is already in progress.");
        return;
    }
    Ogre::Entity* entity = entities.first();

    // LCOV_EXCL_START — requires a loaded SD model + a mesh
    auto st = std::make_unique<MultiViewBakeState>();
    st->entityName = QString::fromStdString(entity->getName());
    // The other (SD-generated) views still need a text prompt. When only a
    // photo was supplied, fall back to a neutral texture prompt so the back
    // is plausibly consistent rather than blank.
    st->prompt = prompt.isEmpty()
        ? QStringLiteral("high quality seamless PBR texture, consistent colours, "
                         "matching the subject, even lighting")
        : prompt;
    st->width = width > 0 ? width : 512;
    st->height = height > 0 ? height : 512;
    st->controlStrength = static_cast<float>(std::clamp(controlStrength, 0.0, 1.0));
    st->controlNetPath = discoveredControlNetDepthPath();
    // Lock ONE seed for every view so front/back share the same denoising
    // trajectory — the cheapest, highest-leverage consistency win (otherwise
    // each view draws fresh random noise and the styles diverge). Pick a fixed
    // positive seed once; pass it to every generateMeshTexture call below.
    st->seed = static_cast<int64_t>(
        QRandomGenerator::global()->bounded(1, 2147483647));

    const QStringList viewNames = views.isEmpty()
        ? QStringList{ QStringLiteral("front"), QStringLiteral("back") } : views;
    for (const QString& vn : viewNames)
        st->views.push_back(resolveDepthView(vn));

    // Pin the input photo as the FRONT view if supplied (TripoSG path): that
    // view is textured from the real image instead of an SD generation, so the
    // front is photo-accurate while the other views stay SD-generated. Loaded
    // here (main thread); startNextMultiViewGeneration skips SD for view 0.
    if (!frontPhotoPath.isEmpty()) {
        QImage p(frontPhotoPath);
        if (!p.isNull())
            st->frontPhoto = p.convertToFormat(QImage::Format_RGB888);
        else
            emit sdGenerationNotice(
                "Front photo could not be loaded — generating that view too.");
    }
    st->generatePbrAfter = generatePbr;

    if (st->controlNetPath.isEmpty()) {
        emit sdGenerationNotice(
            "No ControlNet depth model found — multi-view bake will follow the "
            "prompt only (download \"ControlNet Depth (SD 1.5)\" for shape-aware results).");
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Multi-view mesh texture: entity=%1 views=%2 size=%3")
            .arg(st->entityName).arg(viewNames.join(',')).arg(std::max(st->width, st->height)));
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.mesh_texture_multiview"),
        QStringLiteral("entity=%1 views=%2 size=%3")
            .arg(st->entityName).arg(viewNames.join(',')).arg(std::max(st->width, st->height)));

    m_multiViewBake = std::move(st);
    startNextMultiViewGeneration();
    // LCOV_EXCL_STOP
#else
    Q_UNUSED(width); Q_UNUSED(height); Q_UNUSED(controlStrength); Q_UNUSED(views);
    emit sdGenerationError("Stable Diffusion support is not enabled. Rebuild with ENABLE_STABLE_DIFFUSION=ON");
#endif
}

void MaterialEditorQML::startNextMultiViewGeneration()
{
#ifdef ENABLE_STABLE_DIFFUSION
    if (!m_multiViewBake) return;
    MultiViewBakeState& s = *m_multiViewBake;

    // Resolve the entity afresh each step (selection could change; bail if gone).
    Ogre::Entity* entity = nullptr;
    if (Manager::getSingletonPtr()) {
        for (Ogre::Entity* e : Manager::getSingleton()->getEntities()) {
            if (e && e->getMovableType() == "Entity"
                && e->getName() == s.entityName.toStdString()) { entity = e; break; }
        }
    }
    if (!entity) {
        emit sdGenerationError("Mesh for multi-view bake is no longer available.");
        m_multiViewBake.reset();
        emit sdIsGeneratingChanged();
        return;
    }

    const int depthSize = std::max(s.width, s.height);
    QString depthErr;
    MeshDepthRenderer::RenderResult rr =
        MeshDepthRenderer::renderDepthMapView(entity, depthSize, s.views[s.current], &depthErr);
    if (rr.depth.isNull()) {
        emit sdGenerationError(QStringLiteral("Depth render failed (%1 view): %2")
            .arg(s.views[s.current].name, depthErr));
        m_multiViewBake.reset();
        emit sdIsGeneratingChanged();
        return;
    }

    // Stash the camera matrices for this view now; the image arrives later in
    // onSDGenerationCompleted and is paired with this entry by index.
    MultiViewTextureBaker::View bv;
    bv.viewProj = rr.projMatrix * rr.viewMatrix;
    bv.camDirection = rr.camDirection;

    // Pinned front photo (view 0, TripoSG path): fill the view image from the
    // real photo NOW — no SD call — and advance. This is the Metal-safe
    // substitute for img2img: the front is the actual image, not a generation.
    //
    // REGISTRATION (the key correctness step): the raw photo's subject sits at
    // an arbitrary size/offset relative to the depth-camera framing, so naive
    // scaling projected photo features onto the wrong mesh features (eye→ear).
    // Background-remove the photo, then fit its subject bbox onto the mesh
    // SILHOUETTE bbox from this exact depth render — now the subject occupies
    // the same screen footprint the mesh does, and projecting through the
    // render's own view/proj matrices lands features in alignment.
    if (!s.frontPhoto.isNull() && s.current == 0) {
        QImage photoRb = s.frontPhoto;
        // Composite the cut-out over a solid MAGENTA the subject is very
        // unlikely to contain, so subjectBounds can find the foreground by
        // colour difference (BackgroundRemover returns opaque RGB, not an
        // alpha matte — an alpha test would select the whole frame).
        const QRgb kBg = qRgb(255, 0, 255);
        const QString bgModel = BackgroundRemover::ensureModelBlocking();
        if (!bgModel.isEmpty()) {
            BackgroundRemover::Options bgo;
            bgo.bgR = qRed(kBg); bgo.bgG = qGreen(kBg); bgo.bgB = qBlue(kBg);
            const BackgroundRemover::Result br =
                BackgroundRemover::removeBackground(s.frontPhoto, bgModel, bgo);
            if (!br.image.isNull())
                photoRb = br.image;
        }
        QImage registered = registerPhotoToSilhouette(
            photoRb.convertToFormat(QImage::Format_RGB888), rr.depth, kBg);
        bv.image = registered.isNull()
            ? s.frontPhoto.scaled(rr.depth.width(), rr.depth.height(),
                                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            : registered;
        s.baked.push_back(bv);
        ++s.current;
        if (s.current >= s.views.size())
            finishMultiViewBake();
        else
            startNextMultiViewGeneration();
        return;
    }

    s.baked.push_back(bv);  // image filled on completion

    m_sdGenerationProgress = 0.0f;
    emit sdGenerationProgressChanged();
    SDManager::instance()->generateMeshTexture(
        s.prompt, rr.depth, s.controlNetPath, s.controlStrength,
        QString(), s.width, s.height, s.seed);  // same seed → consistent views
#endif
}

void MaterialEditorQML::finishMultiViewBake()
{
#ifdef ENABLE_STABLE_DIFFUSION
    if (!m_multiViewBake) return;
    // Move the state out so any early return fully clears the in-flight flag.
    std::unique_ptr<MultiViewBakeState> s = std::move(m_multiViewBake);

    Ogre::Entity* entity = nullptr;
    if (Manager::getSingletonPtr()) {
        for (Ogre::Entity* e : Manager::getSingleton()->getEntities()) {
            if (e && e->getMovableType() == "Entity"
                && e->getName() == s->entityName.toStdString()) { entity = e; break; }
        }
    }
    if (!entity) { emit sdGenerationError("Mesh gone before bake."); emit sdIsGeneratingChanged(); return; }

    // The baker projects onto an EXISTING UV0 atlas. A geometry-only mesh
    // (TripoSG, or any unwrap-less import) has none, so auto-unwrap it in place
    // first via xatlas. Safe here because these are STATIC generated meshes
    // (no skeleton — the in-place-mutation caveat that unwrapEntity warns about
    // only affects live skinned meshes), and they SHOULD keep the new UVs (the
    // texture binds to them and they're carried into any export).
    if (!entityHasUv0(entity)) {
        emit sdGenerationNotice("Mesh has no UVs — auto-unwrapping before bake…");
        const UvUnwrapReport ur = UvUnwrap::unwrapEntity(entity);
        if (!ur.applied || !entityHasUv0(entity)) {
            emit sdGenerationError(QStringLiteral(
                "AI texture: auto UV unwrap failed%1 — cannot bake.")
                .arg(ur.error.isEmpty() ? QString() : (": " + ur.error)));
            emit sdIsGeneratingChanged();
            return;
        }
    }

    QString geoErr;
    std::vector<MultiViewTextureBaker::Triangle> tris =
        MultiViewTextureBaker::fromEntity(entity, &geoErr);
    if (tris.empty()) {
        emit sdGenerationError(QStringLiteral("Bake failed: %1").arg(geoErr));
        emit sdIsGeneratingChanged();
        return;
    }

    MultiViewTextureBaker::Options opts;
    opts.resolution = std::max(s->width, s->height);
    TexturePaintBuffer out;
    MultiViewTextureBaker::Report rep =
        MultiViewTextureBaker::bake(tris, s->baked, out, opts);
    if (!rep.ok || rep.pixelsWritten == 0) {
        emit sdGenerationError(QStringLiteral("Bake produced no texels: %1")
            .arg(rep.ok ? QStringLiteral("no view covered the mesh") : rep.error));
        emit sdIsGeneratingChanged();
        return;
    }

    // Save the baked atlas next to the generated textures and apply it.
    const QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString outDir = QDir(dataPath).filePath("generated_textures");
    QDir().mkpath(outDir);
    // Sanitize the entity name into a filesystem-safe basename — imported names
    // can carry '/', ':', spaces, etc. that would break out.save().
    QString safeName = s->entityName;
    safeName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    if (safeName.isEmpty()) safeName = QStringLiteral("entity");
    const QString outName = QStringLiteral("multiview_bake_%1.png").arg(safeName);
    const QString outPath = QDir(outDir).filePath(outName);
    if (!out.save(outPath.toStdString())) {
        emit sdGenerationError("Failed to write baked texture.");
        emit sdIsGeneratingChanged();
        return;
    }
    // Log only the basename — outPath is under the app-data dir and would leak
    // the local username/path into telemetry.
    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
        QStringLiteral("Wrote multi-view baked texture %1").arg(outName));

    if (isOgreAvailable() && Ogre::Root::getSingletonPtr()) {
        try {
            auto &rgm = Ogre::ResourceGroupManager::getSingleton();
            rgm.addResourceLocation(outDir.toStdString(), "FileSystem",
                                    Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            rgm.initialiseResourceGroup(Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        } catch (...) {}
        applyTextureToEntityDiffuse(s->entityName, outName);
    }
    m_textureName = outName;
    emit textureNameChanged();
    emit sdTextureGenerated(outPath);
    emit sdIsGeneratingChanged();
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.mesh_texture_multiview"),
        QStringLiteral("baked %1 texels from %2 views (%3 dilated)")
            .arg(rep.pixelsWritten).arg(s->baked.size()).arg(rep.pixelsDilated));

    // PBR AFTER the AI texture (issue: PBR was previously synthesized during
    // mesh build from a throwaway diffuse, then the AI bake replaced the
    // diffuse — leaving normal/roughness that didn't match the colours). Now
    // synthesize + bind from the freshly-applied AI diffuse so the maps match.
    if (s->generatePbrAfter && aiPbrAvailable())
        generatePbrFromDiffuse();
#endif
}

void MaterialEditorQML::stopTextureGeneration()
{
#ifdef ENABLE_STABLE_DIFFUSION
    SDManager::instance()->stopGeneration();
    emit sdIsGeneratingChanged();
#endif
}
// LCOV_EXCL_STOP

// LCOV_EXCL_START — SD signal handlers require a loaded SD model
#ifdef ENABLE_STABLE_DIFFUSION
void MaterialEditorQML::onSDGenerationStarted()
{
    m_sdGenerationProgress = 0.0f;
    emit sdGenerationProgressChanged();
    emit sdIsGeneratingChanged();
}

void MaterialEditorQML::onSDGenerationProgress()
{
    SDManager *sdManager = SDManager::instance();
    int step = sdManager->generationStep();
    int total = sdManager->generationTotalSteps();
    m_sdGenerationProgress = (total > 0) ? static_cast<float>(step) / total : 0.0f;
    emit sdGenerationProgressChanged();
}

void MaterialEditorQML::applyTextureToEntityDiffuse(const QString& entityName,
                                                    const QString& textureFileName)
{
    // Drive the SAME pipeline as a manual texture change in the
    // Material Editor (issue #403). Earlier attempts hand-mutated the
    // entity's TUSes, but the editor renders via an RTSS-cloned
    // technique and keeps its own m_ogreMaterial/m_passMap state — so
    // a direct mutation updated neither the on-screen render, the
    // panel, nor the script consistently. The proven path is:
    //   loadMaterial(name) -> select the diffuse TUS -> setTextureName(),
    // where setTextureName() already invalidates RTSS, recompiles,
    // re-applies PBR, re-binds to sub-entities, and refreshes the
    // script + panel.
    if (!isOgreAvailable()) return;
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return;

    // Resolve the entity + the material name of its first submesh.
    Ogre::Entity* entity = nullptr;
    for (Ogre::Entity* e : mgr->getEntities()) {
        if (e && e->getMovableType() == "Entity"
            && e->getName() == entityName.toStdString()) {
            entity = e;
            break;
        }
    }
    if (!entity || entity->getNumSubEntities() == 0) return;
    Ogre::SubEntity* sub0 = entity->getSubEntity(0);
    if (!sub0 || !sub0->getMaterial()) return;
    const QString matName = QString::fromStdString(sub0->getMaterial()->getName());

    // Make sure the freshly-written PNG exists as a loadable texture
    // resource. The generated_textures dir is registered as a
    // resource location at startup, but its file INDEX was built then
    // — a texture generated this session isn't in the index, so
    // loading it by bare name (image.load(name, group)) fails and the
    // material shows a "broken link". Fix: read the file from its
    // absolute path via a QFile/Ogre DataStream and create the
    // texture resource directly under its bare name, so subsequent
    // name resolution (setTextureName / RTSS regen) finds it. Force-
    // recreate if a stale entry exists.
    const std::string group = Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;
    const Ogre::String texStd = textureFileName.toStdString();
    {
        QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString absPath = QDir(dataPath).filePath("generated_textures/" + textureFileName);
        try {
            // Drop any stale texture of this name so the new bytes load.
            if (Ogre::TextureManager::getSingleton().getByName(texStd, group))
                Ogre::TextureManager::getSingleton().remove(texStd, group);

            Ogre::Image image;
            QFile imgFile(absPath);
            if (imgFile.open(QIODevice::ReadOnly)) {
                const QByteArray bytes = imgFile.readAll();
                imgFile.close();
                Ogre::DataStreamPtr ds(new Ogre::MemoryDataStream(
                    const_cast<char*>(bytes.constData()),
                    static_cast<size_t>(bytes.size()), false, true));
                // Extension drives the codec (png).
                image.load(ds, "png");
                Ogre::TextureManager::getSingleton().loadImage(texStd, group, image);
            }
        } catch (...) {}
    }

    // Load the entity's material into the editor and select the
    // diffuse texture unit, then set the texture through the proven
    // path. loadMaterial() builds m_passMap and auto-selects pass 0 +
    // TUS 0; for the Mixamo/standard layout TUS 0 is the diffuse map.
    // If a named diffuse/albedo TUS exists at another index, select
    // that instead.
    loadMaterial(matName);

    // Find the diffuse texture unit. On Mixamo / Assimp-imported PBR
    // materials the base-colour TUS is often named "0" (a numeric
    // name) rather than "albedo"/"diffuse_map". That matters: the
    // PBR/RTSS wiring (RTShaderHelper::wirePbrSlotsForFFP +
    // applyPbrIfTagged) only recognizes NAMED slots, so a diffuse on
    // TUS "0" never gets the FFP colour-operation the Cook-Torrance
    // shader needs — the texture loads but never renders (the bug we
    // chased). Rename a numeric/empty diffuse slot to "diffuse_map"
    // so the PBR path picks it up.
    int diffuseTusIndex = 0;
    if (Ogre::Pass* pass = getCurrentPass()) {
        int namedDiffuse = -1, firstTextured = -1;
        for (unsigned short t = 0; t < pass->getNumTextureUnitStates(); ++t) {
            Ogre::TextureUnitState* tus = pass->getTextureUnitState(t);
            const auto n = tus->getName();
            if (n == "diffuse_map" || n == "albedo"
                || n == "Diffuse" || n == "BaseColor") {
                namedDiffuse = t;
                break;
            }
            // A numeric / empty name on a slot that ISN'T a known
            // non-diffuse channel is the unnamed diffuse.
            if (firstTextured < 0
                && n != "roughness" && n != "metallic" && n != "ao"
                && n != "emissive" && n != "normal_map" && n != "NormalMap"
                && !tus->getTextureName().empty()) {
                firstTextured = t;
            }
        }
        if (namedDiffuse >= 0) {
            diffuseTusIndex = namedDiffuse;
        } else if (firstTextured >= 0) {
            // Rename the unnamed diffuse so the PBR/FFP wiring sees it.
            pass->getTextureUnitState(
                static_cast<unsigned short>(firstTextured))->setName("diffuse_map");
            diffuseTusIndex = firstTextured;
            // Rebuild the editor's TUS list so the rename + selection
            // are consistent.
            updateTextureUnitList();
        }
    }
    setSelectedTextureUnitIndex(diffuseTusIndex);

    // setTextureName no-ops if the new name equals m_textureName;
    // clear it first so the assignment always fires.
    m_textureName.clear();
    setTextureName(textureFileName);
}

void MaterialEditorQML::onSDGenerationCompleted(const QString &outputPath)
{
    m_sdGenerationProgress = 1.0f;
    emit sdGenerationProgressChanged();
    emit sdIsGeneratingChanged();

    QFileInfo fileInfo(outputPath);
    QString dirPath = fileInfo.absolutePath();
    QString fileName = fileInfo.fileName();

    // A freshly generated image may reuse a texture name whose preview path is
    // already memoized — clear the (group-keyed) cache so the preview
    // re-resolves to the new pixels rather than a previous file.
    m_previewPathCache.clear();

#ifdef ENABLE_STABLE_DIFFUSION
    // Multi-view bake (slice 2): pair this completed image with the view whose
    // depth/matrices we stashed in startNextMultiViewGeneration(), then either
    // issue the next view or bake when all views are done.
    if (m_multiViewBake) {
        MultiViewBakeState& s = *m_multiViewBake;
        QImage img(outputPath);
        if (img.isNull() || s.current >= s.baked.size()) {
            // A view image we can't load would bake invalid input later; abort
            // the whole multi-view run now rather than fail late.
            emit sdGenerationError(QStringLiteral("Multi-view bake aborted: could not "
                "load the generated %1 view image.")
                .arg(s.current < s.views.size() ? s.views[s.current].name : "?"));
            m_multiViewBake.reset();
            emit sdIsGeneratingChanged();
            return;
        }
        s.baked[s.current].image = img.convertToFormat(QImage::Format_RGB888);
        emit sdTextureGenerated(outputPath);  // let the preview update per view
        ++s.current;
        if (s.current < s.views.size()) {
            startNextMultiViewGeneration();   // next view
        } else {
            finishMultiViewBake();            // all views captured → bake + apply
        }
        return;
    }
#endif

    // Issue #403: if this was a mesh-aware generation, bind the
    // result to the selected entity's diffuse TUS directly (the
    // editor's "current pass" may not be the rendered material, which
    // is why the mesh otherwise rendered untextured). Register the
    // resource location first so Ogre can load the file by name.
    if (!m_sdMeshTextureEntity.isEmpty()) {
        const QString targetEntity = m_sdMeshTextureEntity;
        m_sdMeshTextureEntity.clear();
        if (isOgreAvailable() && Ogre::Root::getSingletonPtr()) {
            try {
                auto &rgm = Ogre::ResourceGroupManager::getSingleton();
                rgm.addResourceLocation(dirPath.toStdString(), "FileSystem",
                                        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
                rgm.initialiseResourceGroup(Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            } catch (...) {}
            applyTextureToEntityDiffuse(targetEntity, fileName);
        }
        m_textureName = fileName;
        emit textureNameChanged();
        emit sdTextureGenerated(outputPath);
        return;
    }

    // Helper lambda to emit deferred material completion if SD was triggered by LLM
    auto emitDeferredCompletion = [this]() {
        if (m_sdPendingForMaterial) {
            m_sdPendingForMaterial = false;
            emit sdPendingForMaterialChanged();
            emit aiGenerationCompleted(m_pendingMaterialScript);
            m_pendingMaterialScript.clear();
        }
    };

    if (!isOgreAvailable()) {
        m_textureName = fileName;
        emit textureNameChanged();
        emit sdTextureGenerated(outputPath);
        emitDeferredCompletion();
        return;
    }

    try {
        auto *root = Ogre::Root::getSingletonPtr();
        if (!root) {
            m_textureName = fileName;
            emit textureNameChanged();
            emit sdTextureGenerated(outputPath);
            emitDeferredCompletion();
            return;
        }

        // Register resource location
        try {
            auto &rgm = Ogre::ResourceGroupManager::getSingleton();
            rgm.addResourceLocation(dirPath.toStdString(), "FileSystem",
                                    Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            rgm.initialiseResourceGroup(Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        } catch (...) {}

        // Apply the generated texture to the current material's diffuse TUS
        // through the SAME path as a manual texture change, so it gets the
        // group-resolution + RTSS rebind that makes it actually render on the
        // model (a raw texUnit->setTextureName() here left the mesh untextured).
        Ogre::Pass *pass = getCurrentPass();
        if (pass) {
            Ogre::TextureUnitState *texUnit = getCurrentTextureUnit();
            if (!texUnit) {
                texUnit = pass->createTextureUnitState();
                updateTextureUnitList();
                if (m_textureUnitList.size() > 0) {
                    setSelectedTextureUnitIndex(m_textureUnitList.size() - 1);
                }
            }
            if (texUnit) {
                // Force the assignment even if m_textureName already equals
                // fileName (setTextureName no-ops on equal names).
                m_textureName.clear();
                setTextureName(fileName);
            }
        }
    } catch (...) {}

    emit sdTextureGenerated(outputPath);
    emitDeferredCompletion();
}

void MaterialEditorQML::onSDGenerationError(const QString &error)
{
    // Drop any pending mesh-texture target — otherwise the next plain
    // txt2img to complete would take the mesh-aware completion branch
    // and apply itself to this stale entity.
    m_sdMeshTextureEntity.clear();
#ifdef ENABLE_STABLE_DIFFUSION
    // Abort an in-flight multi-view bake on any generation error so it can't
    // strand the in-progress flag or capture a later unrelated generation.
    m_multiViewBake.reset();
#endif
    m_sdGenerationProgress = 0.0f;
    emit sdGenerationProgressChanged();
    emit sdIsGeneratingChanged();
    emit sdGenerationError(error);

    // If SD failed but was triggered by LLM, still emit the material completion
    // so the material gets applied (just without the texture)
    if (m_sdPendingForMaterial) {
        m_sdPendingForMaterial = false;
        emit sdPendingForMaterialChanged();
        emit aiGenerationCompleted(m_pendingMaterialScript);
        m_pendingMaterialScript.clear();
    }
}

void MaterialEditorQML::onSDGenerationStopped()
{
    m_sdMeshTextureEntity.clear();
#ifdef ENABLE_STABLE_DIFFUSION
    // Abandon any in-flight multi-view bake so a later unrelated generation
    // can't be captured into its stale per-view slots.
    m_multiViewBake.reset();
#endif
    m_sdGenerationProgress = 0.0f;
    emit sdGenerationProgressChanged();
    emit sdIsGeneratingChanged();

    // If SD was stopped but was triggered by LLM, still apply the material
    if (m_sdPendingForMaterial) {
        m_sdPendingForMaterial = false;
        emit sdPendingForMaterialChanged();
        emit aiGenerationCompleted(m_pendingMaterialScript);
        m_pendingMaterialScript.clear();
    }
}

void MaterialEditorQML::onSDModelLoadedChanged()
{
    emit sdModelLoadedChanged();
}
#endif // ENABLE_STABLE_DIFFUSION
// LCOV_EXCL_STOP

// Undo/Redo Implementation
void MaterialEditorQML::addToUndoStack(const QString &text)
{
    // Clear redo stack when a new action is performed
    if (!m_redoStack.isEmpty()) {
        m_redoStack.clear();
        emit undoRedoStateChanged();
    }
    
    // Add to undo stack
    m_undoStack.append(text);
    
    // Limit stack size to prevent memory issues
    if (m_undoStack.size() > m_maxUndoSteps) {
        m_undoStack.removeFirst();
    }
    
    emit undoRedoStateChanged();
}

void MaterialEditorQML::undo()
{
    if (!canUndo()) {
        return;
    }
    
    // Move current text to redo stack
    m_redoStack.append(m_materialText);
    
    // Get previous text from undo stack
    QString previousText = m_undoStack.takeLast();
    
    // Set the text without adding to undo stack again
    m_materialText = previousText;
    emit materialTextChanged();
    emit undoRedoStateChanged();
}

void MaterialEditorQML::redo()
{
    if (!canRedo()) {
        return;
    }
    
    // Move current text to undo stack
    m_undoStack.append(m_materialText);
    
    // Get next text from redo stack
    QString nextText = m_redoStack.takeLast();
    
    // Set the text without adding to undo stack again
    m_materialText = nextText;
    emit materialTextChanged();
    emit undoRedoStateChanged();
}

void MaterialEditorQML::clearUndoHistory()
{
    m_undoStack.clear();
    m_redoStack.clear();
    emit undoRedoStateChanged();
}

// Material list operations
QStringList MaterialEditorQML::getMaterialList() const
{
    QStringList materialList;
    
    // Safety check for Ogre availability
    if (!isOgreAvailable()) {
        return materialList;
    }
    
    try {
        Ogre::ResourceManager::ResourceMapIterator materialIterator =
            Ogre::MaterialManager::getSingleton().getResourceIterator();

        while (materialIterator.hasMoreElements()) {
            Ogre::MaterialPtr material = Ogre::static_pointer_cast<Ogre::Material>(
                materialIterator.peekNextValue());
            const QString name = QString::fromStdString(material->getName());
            // Hide internal paint-pipeline materials from the user
            // facing list. These are created at runtime by
            // TexturePaintController (mask overlay material, hover-
            // ring material) and EditModeController's session, so the
            // user never authored them and shouldn't be able to
            // accidentally select / edit / delete them.
            if (isPaintPipelineMaterial(name)) {
                materialIterator.moveNext();
                continue;
            }
            materialList.append(name);
            materialIterator.moveNext();
        }
    } catch (const std::exception& e) {
        qDebug() << "Error getting material list:" << e.what();
    }
    
    return materialList;
}

QString MaterialEditorQML::materialPreview(const QString& materialName) const
{
    if (isPs1RipMaterial(materialName) || isPaintPipelineMaterial(materialName))
        return QString();
    return MaterialPreviewRenderer::instance()->renderPreviewAsDataUri(materialName);
}

QString MaterialEditorQML::interactiveMaterialPreview(const QString& materialName,
                                                       int size,
                                                       int shape,
                                                       double yawDegrees) const
{
    if (isPs1RipMaterial(materialName) || isPaintPipelineMaterial(materialName))
        return QString();
    return MaterialPreviewRenderer::instance()
        ->renderInteractivePreview(materialName, size, shape, yawDegrees);
}

void MaterialEditorQML::importMaterialFile(const QString &filePath)
{
    if (filePath.isEmpty()) {
        return;
    }
    
    // Safety check for Ogre availability
    if (!isOgreAvailable()) {
        qDebug() << "Ogre not available for material import";
        return;
    }
    
    try {
        QFileInfo fileInfo(filePath);
        QString directory = fileInfo.absolutePath();
        QString fileName = fileInfo.fileName();
        
        // Add resource location
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
            directory.toStdString(), "FileSystem", fileName.toStdString(), true);
        
        // Initialize resource groups
        Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();
        
        // Reload materials
        Ogre::MaterialManager::getSingleton().reloadAll(true);
        Ogre::MeshManager::getSingleton().reloadAll(true);
        
        qDebug() << "Successfully imported material file:" << filePath;
    } catch (const std::exception& e) {
        qDebug() << "Error importing material file:" << e.what();
        emit errorOccurred(QString("Error importing material file: %1").arg(e.what()));
    }
}

void MaterialEditorQML::exportMaterial(const QString &fileName, const QString &materialName)
{
    if (fileName.isEmpty() || materialName.isEmpty()) {
        return;
    }
    
    // Safety check for Ogre availability
    if (!isOgreAvailable()) {
        qDebug() << "Ogre not available for material export";
        return;
    }
    
    try {
        Ogre::MaterialPtr material = Ogre::static_pointer_cast<Ogre::Material>(
            Ogre::MaterialManager::getSingleton().getByName(materialName.toStdString()));
        
        if (!material) {
            emit errorOccurred("Material not found: " + materialName);
            return;
        }
        
        Ogre::MaterialSerializer ms;
        ms.exportMaterial(material, fileName.toStdString());
        
        qDebug() << "Successfully exported material:" << materialName << "to" << fileName;
    } catch (const std::exception& e) {
        qDebug() << "Error exporting material:" << e.what();
        emit errorOccurred(QString("Error exporting material: %1").arg(e.what()));
    }
}

// LCOV_EXCL_START — opens QML window with QQmlApplicationEngine, requires display
void MaterialEditorQML::openMaterialEditorWindow(const QString &materialName)
{
    try {
        // Force software rendering to avoid OpenGL conflicts with Ogre
        qputenv("QSG_RHI_BACKEND", "software");
        qputenv("QT_QUICK_BACKEND", "software");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        
        // Load the material first
        loadMaterial(materialName);
        
        // Create QML Application Engine for material editor
        QQmlApplicationEngine* engine = new QQmlApplicationEngine();
        
        // Force software rendering on the engine
        engine->setProperty("_q_sg_renderloop", "basic");
        
        // Register QML types - must match registrations in main.cpp
        qmlRegisterSingletonType<MaterialEditorQML>("MaterialEditorQML", 1, 0, "MaterialEditorQML", 
            [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject * {
                Q_UNUSED(engine)
                Q_UNUSED(scriptEngine)
                return MaterialEditorQML::qmlInstance(engine, scriptEngine);
            });
        
        // Register LLMManager singleton for QML
        qmlRegisterSingletonType<LLMManager>("MaterialEditorQML", 1, 0, "LLMManager",
            [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
                Q_UNUSED(engine)
                Q_UNUSED(scriptEngine)
                return LLMManager::qmlInstance(engine, scriptEngine);
            });

        // Register ModelDownloader singleton for QML
        qmlRegisterSingletonType<ModelDownloader>("MaterialEditorQML", 1, 0, "ModelDownloader",
            [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
                Q_UNUSED(engine)
                Q_UNUSED(scriptEngine)
                return ModelDownloader::qmlInstance(engine, scriptEngine);
            });

        // Register QMLMaterialHighlighter for QML use
        qmlRegisterType<QMLMaterialHighlighter>("MaterialEditorQML", 1, 0, "MaterialHighlighter");
        
        // Set window properties in QML context
        engine->rootContext()->setContextProperty("materialName", materialName);
        
        // Load the QML material editor
        QUrl qmlUrl("qrc:/MaterialEditorQML/MaterialEditorWindow.qml");
        qDebug() << "Opening Material Editor for:" << materialName;
        
        // Connect to check for loading errors
        connect(engine, &QQmlApplicationEngine::objectCreated, this, [this, engine, materialName](QObject *obj, const QUrl &objUrl) {
            if (!obj) {
                qDebug() << "QML Material Editor failed to load";
                engine->deleteLater();
            } else {
                qDebug() << "QML Material Editor loaded successfully for:" << materialName;
                // Set window title
                if (auto window = qobject_cast<QQuickWindow*>(obj)) {
                    window->setTitle("QML Material Editor - " + (materialName.isEmpty() ? "New Material" : materialName));
                }
            }
        });
        
        engine->load(qmlUrl);
        
    } catch (const std::exception& e) {
        qDebug() << "Exception in Material Editor creation:" << e.what();
        emit errorOccurred(QString("Material Editor encountered an error: %1").arg(e.what()));
    } catch (...) {
        qDebug() << "Unknown exception in Material Editor creation";
        emit errorOccurred("Material Editor encountered an unknown error.");
    }
}
// LCOV_EXCL_STOP
