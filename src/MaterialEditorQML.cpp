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
#include <OgreLog.h>
#include <OgreScriptCompiler.h>
#include <OgreScriptTranslator.h>

MaterialEditorQML::MaterialEditorQML(QObject *parent)
    : QObject(parent)
{
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
        if (textureUnit) {
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
        
        Ogre::TextureUnitState* textureUnit = getCurrentTextureUnit();
        if (textureUnit) {
            textureUnit->setScrollAnimation(m_scrollAnimUSpeed, speed);
            updateMaterialText();
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

    textureUnit->setTextureName("");
    setTextureName("*Select a texture*");
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
}

void MaterialEditorQML::resetPropertiesToDefaults()
{
    // Reset all properties to their default values
    m_lightingEnabled = true;
    m_depthWriteEnabled = true;
    m_depthCheckEnabled = true;
    m_ambientColor = QColor(0.5f * 255, 0.5f * 255, 0.5f * 255);
    m_diffuseColor = QColor(255, 255, 255);
    m_specularColor = QColor(0, 0, 0);
    m_emissiveColor = QColor(0, 0, 0);
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
}

void MaterialEditorQML::updateTextureUnitProperties()
{
    Ogre::TextureUnitState* textureUnit = getCurrentTextureUnit();
    if (!textureUnit) {
        m_textureName = "*Select a texture*";
        m_scrollAnimUSpeed = 0.0;
        m_scrollAnimVSpeed = 0.0;
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
    }
    
    emit textureNameChanged();
    emit scrollAnimUSpeedChanged();
    emit scrollAnimVSpeedChanged();
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
    textures << "Select from available textures..."; // Placeholder
    
    try {
        // Get available textures from Ogre TextureManager
        Ogre::ResourceManager::ResourceMapIterator textureIterator = Ogre::TextureManager::getSingleton().getResourceIterator();
        while (textureIterator.hasMoreElements()) {
            QString texName = QString::fromStdString(textureIterator.peekNextValue()->getName());
            if (!texName.isEmpty() && texName != "white" && !texName.startsWith("_")) { // Skip internal textures
                textures << texName;
            }
            textureIterator.moveNext();
        }
    } catch (const std::exception& e) {
        qDebug() << "Error getting available textures:" << e.what();
    }
    
    return textures;
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