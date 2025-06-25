#include "MaterialEditorQML.h"
#include "Manager.h"
#include <QDebug>
#include <QFileDialog>
#include <QColorDialog>
#include <QApplication>
#include <QMainWindow>
#include <QQmlEngine>
#include <QJSEngine>
#include <QStandardPaths>
#include <QPalette>
#include <OgreLog.h>
#include <OgreScriptCompiler.h>
#include <OgreScriptTranslator.h>

MaterialEditorQML::MaterialEditorQML(QObject *parent)
    : QObject(parent)
{
    // Initialize theme colors from system palette
    QPalette palette = QApplication::palette();
    m_backgroundColor = palette.color(QPalette::Window);
    m_panelColor = palette.color(QPalette::Base);
    m_textColor = palette.color(QPalette::WindowText);
    m_borderColor = palette.color(QPalette::Mid);
    m_highlightColor = palette.color(QPalette::Highlight);
    m_buttonColor = palette.color(QPalette::Button);
    m_buttonTextColor = palette.color(QPalette::ButtonText);
    m_disabledTextColor = palette.color(QPalette::PlaceholderText);
    m_accentColor = palette.color(QPalette::Highlight);
    
    // Initialize material color properties with defaults
    m_ambientColor = QColor(128, 128, 128);  // Gray
    m_diffuseColor = QColor(255, 255, 255);  // White
    m_specularColor = QColor(0, 0, 0);       // Black
    m_emissiveColor = QColor(0, 0, 0);       // Black
    m_fogColor = QColor(0, 0, 0);            // Black
    m_textureBorderColor = QColor(0, 0, 0);  // Black
}

MaterialEditorQML* MaterialEditorQML::qmlInstance(QQmlEngine *engine, QJSEngine *scriptEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(scriptEngine)
    
    static MaterialEditorQML* instance = new MaterialEditorQML();
    return instance;
}

void MaterialEditorQML::loadMaterial(const QString &materialName)
{
    if (materialName.isEmpty()) {
        createNewMaterial();
        return;
    }

    m_materialName = materialName;
    
    try {
        m_ogreMaterial = Ogre::static_pointer_cast<Ogre::Material>(
            Ogre::MaterialManager::getSingleton().getByName(materialName.toStdString()));
        
        if (m_ogreMaterial.isNull()) {
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

bool MaterialEditorQML::applyMaterial()
{
    try {
        Ogre::String script = m_materialText.toStdString();
        Ogre::MemoryDataStream *memoryStream = new Ogre::MemoryDataStream(
            (void*)script.c_str(), script.length() * sizeof(char));
        Ogre::DataStreamPtr dataStream(memoryStream);

        if (!validateMaterialScript(m_materialText)) {
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
        
        if (!m_ogreMaterial.isNull()) {
            m_ogreMaterial->compile();
        }

        // Reload all materials and meshes
        Ogre::MaterialManager::getSingleton().reloadAll(true);
        Ogre::MeshManager::getSingleton().reloadAll(true);

        // Reapply materials to all scene nodes
        for (Ogre::SceneNode* sn : Manager::getSingleton()->getSceneNodes()) {
            if (!sn->getName().empty() && !sn->getAttachedObjects().empty()) {
                Ogre::Entity *e = static_cast<Ogre::Entity *>(sn->getAttachedObject(0));
                e->setMaterialName(e->getSubEntity(0)->getMaterialName());
            }
        }

        // Reload the material to update UI
        loadMaterial(m_materialName);
        
        emit materialApplied();
        return true;
        
    } catch (const std::exception& e) {
        emit errorOccurred(QString("Error applying material: %1").arg(e.what()));
        return false;
    }
}

bool MaterialEditorQML::validateMaterialScript(const QString &script)
{
    try {
        Ogre::String ogreScript = script.toStdString();
        Ogre::MemoryDataStream *memoryStream = new Ogre::MemoryDataStream(
            (void*)ogreScript.c_str(), ogreScript.length() * sizeof(char));
        Ogre::DataStreamPtr dataStream(memoryStream);

        Ogre::ScriptCompilerManager* compilerManager = Ogre::ScriptCompilerManager::getSingletonPtr();
        if (!compilerManager) {
            emit errorOccurred("Script compiler not available");
            return false;
        }

        return true; // Basic validation - more sophisticated validation could be added
        
    } catch (const std::exception& e) {
        emit errorOccurred(QString("Script validation error: %1").arg(e.what()));
        return false;
    }
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
        
        // Auto-select first pass if available
        if (!m_passList.isEmpty()) {
            setSelectedPassIndex(0);
        } else {
            setSelectedPassIndex(-1);
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

void MaterialEditorQML::setTextureName(const QString &name)
{
    if (m_textureName != name) {
        m_textureName = name;
        
        Ogre::TextureUnitState* textureUnit = getCurrentTextureUnit();
        if (textureUnit && !name.isEmpty() && name != "*Select a texture*") {
            // Only set non-empty, valid texture names to avoid OGRE crashes
            textureUnit->setTextureName(name.toStdString());
            updateMaterialText();
        }
        
        emit textureNameChanged();
    }
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
    if (m_ogreMaterial.isNull()) return;
    
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

void MaterialEditorQML::selectTexture()
{
    QString filePath = QFileDialog::getOpenFileName(
        nullptr, 
        tr("Select a texture"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        tr("Image File (*.bmp *.jpg *.gif *.raw *.png *.tga *.dds)"));

    if (filePath.isEmpty()) return;

    Ogre::TextureUnitState* textureUnit = getCurrentTextureUnit();
    if (!textureUnit) return;

    QFileInfo file(filePath);
    
    // Validate file name is not empty
    if (file.fileName().isEmpty()) {
        emit errorOccurred("Selected file has an empty name.");
        return;
    }

    try {
        // Try to get existing texture
        Ogre::TextureManager::getSingleton().getByName(
            file.fileName().toStdString(), file.path().toStdString());
    } catch (...) {
        // Load new texture
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
            file.path().toStdString(), "FileSystem", file.path().toStdString());
        Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();

        Ogre::Image image;
        image.load(file.fileName().toStdString(), file.path().toStdString());
        Ogre::TextureManager::getSingleton().loadImage(
            file.fileName().toStdString(), file.path().toStdString(), image);
    }
    
    setTextureName(file.fileName());
}

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
    
    if (m_ogreMaterial.isNull()) {
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
    
    // Fog properties - simplified approach
    m_fogOverride = pass->getFogOverride();
    if (m_fogOverride) {
        // Get fog settings if available
        m_fogMode = 0;           // Default to None
        m_fogColor = QColor(0, 0, 0);
        m_fogDensity = 0.0f;
        m_fogStart = 0.0f;
        m_fogEnd = 1.0f;
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
    if (m_ogreMaterial.isNull()) return;

    try {
        Ogre::MaterialSerializer ms;
        ms.queueForExport(m_ogreMaterial, false, false, m_materialName.toStdString());
        setMaterialText(QString::fromStdString(ms.getQueuedAsString()));
    } catch (const std::exception& e) {
        qDebug() << "Error updating material text:" << e.what();
    }
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

Ogre::Technique* MaterialEditorQML::getCurrentTechnique() const
{
    if (m_ogreMaterial.isNull() || m_selectedTechniqueIndex < 0) {
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
    
    Ogre::ResourceManager::ResourceMapIterator it = Ogre::TextureManager::getSingleton().getResourceIterator();
    while (it.hasMoreElements()) {
        textures.append(QString::fromStdString(it.peekNextValue()->getName()));
        it.moveNext();
    }
    
    return textures;
}

QString MaterialEditorQML::getTexturePreviewPath() const
{
    QString texName = m_textureName;
    if (texName.isEmpty() || texName == "*Select a texture*" || texName.trimmed().isEmpty()) {
        return "";
    }
    
    // Construct relative path from working directory
    // Try common texture locations
    QStringList possiblePaths = {
        QString("media/materials/textures/%1").arg(texName),
        QString("../media/materials/textures/%1").arg(texName),
        QString("../../media/materials/textures/%1").arg(texName)
    };
    
    for (const QString& path : possiblePaths) {
        QFileInfo fileInfo(path);
        if (fileInfo.exists() && fileInfo.isFile()) {
            return QString("file:///%1").arg(fileInfo.absoluteFilePath());
        }
    }
    
    // If not found in expected locations, try working directory relative
    QString workingDirPath = QString("media/materials/textures/%1").arg(texName);
    QFileInfo workingDirFile(workingDirPath);
    if (workingDirFile.exists()) {
        return QString("file:///%1").arg(workingDirFile.absoluteFilePath());
    }
    
    // Return empty if texture file not found
    return "";
}

void MaterialEditorQML::openTextureFileDialog()
{
    // This will be handled by QML FileDialog
    // Just a placeholder for future C++ implementation if needed
}

void MaterialEditorQML::exportMaterial(const QString &fileName)
{
    if (m_ogreMaterial.isNull()) {
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
void MaterialEditorQML::setFogOverride(bool override)
{
    if (m_fogOverride != override) {
        m_fogOverride = override;
        Ogre::Pass* pass = getCurrentPass();
        if (pass) {
            if (override) {
                pass->setFog(override, static_cast<Ogre::FogMode>(m_fogMode), 
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