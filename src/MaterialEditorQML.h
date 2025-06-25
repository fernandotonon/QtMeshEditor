#ifndef MATERIALEDITORQML_H
#define MATERIALEDITORQML_H

#include <QObject>
#include <QQmlEngine>
#include <QColor>
#include <QStringList>
#include <QVariantMap>
#include <QQuickItem>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgreTextureManager.h>
#include <OgreMaterialSerializer.h>
#include <OgreTechnique.h>
#include <OgrePass.h>

class MaterialEditorQML : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString materialName READ materialName WRITE setMaterialName NOTIFY materialNameChanged)
    Q_PROPERTY(QString materialText READ materialText WRITE setMaterialText NOTIFY materialTextChanged)
    Q_PROPERTY(QStringList techniqueList READ techniqueList NOTIFY techniqueListChanged)
    Q_PROPERTY(QStringList passList READ passList NOTIFY passListChanged)
    Q_PROPERTY(QStringList textureUnitList READ textureUnitList NOTIFY textureUnitListChanged)
    Q_PROPERTY(int selectedTechniqueIndex READ selectedTechniqueIndex WRITE setSelectedTechniqueIndex NOTIFY selectedTechniqueIndexChanged)
    Q_PROPERTY(int selectedPassIndex READ selectedPassIndex WRITE setSelectedPassIndex NOTIFY selectedPassIndexChanged)
    Q_PROPERTY(int selectedTextureUnitIndex READ selectedTextureUnitIndex WRITE setSelectedTextureUnitIndex NOTIFY selectedTextureUnitIndexChanged)
    
    // Pass properties
    Q_PROPERTY(bool lightingEnabled READ lightingEnabled WRITE setLightingEnabled NOTIFY lightingEnabledChanged)
    Q_PROPERTY(bool depthWriteEnabled READ depthWriteEnabled WRITE setDepthWriteEnabled NOTIFY depthWriteEnabledChanged)
    Q_PROPERTY(bool depthCheckEnabled READ depthCheckEnabled WRITE setDepthCheckEnabled NOTIFY depthCheckEnabledChanged)
    Q_PROPERTY(QColor ambientColor READ ambientColor WRITE setAmbientColor NOTIFY ambientColorChanged)
    Q_PROPERTY(QColor diffuseColor READ diffuseColor WRITE setDiffuseColor NOTIFY diffuseColorChanged)
    Q_PROPERTY(QColor specularColor READ specularColor WRITE setSpecularColor NOTIFY specularColorChanged)
    Q_PROPERTY(QColor emissiveColor READ emissiveColor WRITE setEmissiveColor NOTIFY emissiveColorChanged)
    Q_PROPERTY(float diffuseAlpha READ diffuseAlpha WRITE setDiffuseAlpha NOTIFY diffuseAlphaChanged)
    Q_PROPERTY(float specularAlpha READ specularAlpha WRITE setSpecularAlpha NOTIFY specularAlphaChanged)
    Q_PROPERTY(float shininess READ shininess WRITE setShininess NOTIFY shininessChanged)
    Q_PROPERTY(int polygonMode READ polygonMode WRITE setPolygonMode NOTIFY polygonModeChanged)
    Q_PROPERTY(int sourceBlendFactor READ sourceBlendFactor WRITE setSourceBlendFactor NOTIFY sourceBlendFactorChanged)
    Q_PROPERTY(int destBlendFactor READ destBlendFactor WRITE setDestBlendFactor NOTIFY destBlendFactorChanged)
    
    // Vertex color tracking
    Q_PROPERTY(bool useVertexColorToAmbient READ useVertexColorToAmbient WRITE setUseVertexColorToAmbient NOTIFY useVertexColorToAmbientChanged)
    Q_PROPERTY(bool useVertexColorToDiffuse READ useVertexColorToDiffuse WRITE setUseVertexColorToDiffuse NOTIFY useVertexColorToDiffuseChanged)
    Q_PROPERTY(bool useVertexColorToSpecular READ useVertexColorToSpecular WRITE setUseVertexColorToSpecular NOTIFY useVertexColorToSpecularChanged)
    Q_PROPERTY(bool useVertexColorToEmissive READ useVertexColorToEmissive WRITE setUseVertexColorToEmissive NOTIFY useVertexColorToEmissiveChanged)
    
    // Texture properties
    Q_PROPERTY(QString textureName READ textureName WRITE setTextureName NOTIFY textureNameChanged)
    Q_PROPERTY(double scrollAnimUSpeed READ scrollAnimUSpeed WRITE setScrollAnimUSpeed NOTIFY scrollAnimUSpeedChanged)
    Q_PROPERTY(double scrollAnimVSpeed READ scrollAnimVSpeed WRITE setScrollAnimVSpeed NOTIFY scrollAnimVSpeedChanged)

public:
    explicit MaterialEditorQML(QObject *parent = nullptr);
    virtual ~MaterialEditorQML() = default;

    // Property getters
    QString materialName() const { return m_materialName; }
    QString materialText() const { return m_materialText; }
    QStringList techniqueList() const { return m_techniqueList; }
    QStringList passList() const { return m_passList; }
    QStringList textureUnitList() const { return m_textureUnitList; }
    int selectedTechniqueIndex() const { return m_selectedTechniqueIndex; }
    int selectedPassIndex() const { return m_selectedPassIndex; }
    int selectedTextureUnitIndex() const { return m_selectedTextureUnitIndex; }
    
    // Pass property getters
    bool lightingEnabled() const { return m_lightingEnabled; }
    bool depthWriteEnabled() const { return m_depthWriteEnabled; }
    bool depthCheckEnabled() const { return m_depthCheckEnabled; }
    QColor ambientColor() const { return m_ambientColor; }
    QColor diffuseColor() const { return m_diffuseColor; }
    QColor specularColor() const { return m_specularColor; }
    QColor emissiveColor() const { return m_emissiveColor; }
    float diffuseAlpha() const { return m_diffuseAlpha; }
    float specularAlpha() const { return m_specularAlpha; }
    float shininess() const { return m_shininess; }
    int polygonMode() const { return m_polygonMode; }
    int sourceBlendFactor() const { return m_sourceBlendFactor; }
    int destBlendFactor() const { return m_destBlendFactor; }
    
    // Vertex color tracking getters
    bool useVertexColorToAmbient() const { return m_useVertexColorToAmbient; }
    bool useVertexColorToDiffuse() const { return m_useVertexColorToDiffuse; }
    bool useVertexColorToSpecular() const { return m_useVertexColorToSpecular; }
    bool useVertexColorToEmissive() const { return m_useVertexColorToEmissive; }
    
    // Texture property getters
    QString textureName() const { return m_textureName; }
    double scrollAnimUSpeed() const { return m_scrollAnimUSpeed; }
    double scrollAnimVSpeed() const { return m_scrollAnimVSpeed; }

    // Static factory for QML singleton
    static MaterialEditorQML* qmlInstance(QQmlEngine *engine, QJSEngine *scriptEngine);

public slots:
    // Material management
    void loadMaterial(const QString &materialName);
    void createNewMaterial(const QString &materialName = "");
    bool applyMaterial();
    bool validateMaterialScript(const QString &script);
    
    // Property setters
    void setMaterialName(const QString &name);
    void setMaterialText(const QString &text);
    void setSelectedTechniqueIndex(int index);
    void setSelectedPassIndex(int index);
    void setSelectedTextureUnitIndex(int index);
    
    // Pass property setters
    void setLightingEnabled(bool enabled);
    void setDepthWriteEnabled(bool enabled);
    void setDepthCheckEnabled(bool enabled);
    void setAmbientColor(const QColor &color);
    void setDiffuseColor(const QColor &color);
    void setSpecularColor(const QColor &color);
    void setEmissiveColor(const QColor &color);
    void setDiffuseAlpha(float alpha);
    void setSpecularAlpha(float alpha);
    void setShininess(float shininess);
    void setPolygonMode(int mode);
    void setSourceBlendFactor(int factor);
    void setDestBlendFactor(int factor);
    
    // Vertex color tracking setters
    void setUseVertexColorToAmbient(bool use);
    void setUseVertexColorToDiffuse(bool use);
    void setUseVertexColorToSpecular(bool use);
    void setUseVertexColorToEmissive(bool use);
    
    // Texture property setters
    void setTextureName(const QString &name);
    void setScrollAnimUSpeed(double speed);
    void setScrollAnimVSpeed(double speed);
    
    // Actions
    void createNewTechnique(const QString &name);
    void createNewPass(const QString &name);
    void createNewTextureUnit(const QString &name);
    void selectTexture();
    void removeTexture();
    
    // Utility functions
    QStringList getPolygonModeNames() const;
    QStringList getBlendFactorNames() const;
    QStringList getAvailableTextures() const;
    
    // File operations
    void openTextureFileDialog();
    void exportMaterial(const QString &fileName);
    
    // Color picker
    void openColorPicker(const QString &colorType);

signals:
    // Property change signals
    void materialNameChanged();
    void materialTextChanged();
    void techniqueListChanged();
    void passListChanged();
    void textureUnitListChanged();
    void selectedTechniqueIndexChanged();
    void selectedPassIndexChanged();
    void selectedTextureUnitIndexChanged();
    
    // Pass property change signals
    void lightingEnabledChanged();
    void depthWriteEnabledChanged();
    void depthCheckEnabledChanged();
    void ambientColorChanged();
    void diffuseColorChanged();
    void specularColorChanged();
    void emissiveColorChanged();
    void diffuseAlphaChanged();
    void specularAlphaChanged();
    void shininessChanged();
    void polygonModeChanged();
    void sourceBlendFactorChanged();
    void destBlendFactorChanged();
    
    // Vertex color tracking change signals
    void useVertexColorToAmbientChanged();
    void useVertexColorToDiffuseChanged();
    void useVertexColorToSpecularChanged();
    void useVertexColorToEmissiveChanged();
    
    // Texture property change signals
    void textureNameChanged();
    void scrollAnimUSpeedChanged();
    void scrollAnimVSpeedChanged();
    
    // Error and status signals
    void errorOccurred(const QString &error);
    void materialApplied();

private:
    void updateTechniqueList();
    void updatePassList();
    void updateTextureUnitList();
    void updatePassProperties();
    void updateTextureUnitProperties();
    void updateMaterialText();
    void resetPropertiesToDefaults();
    Ogre::Pass* getCurrentPass() const;
    Ogre::TextureUnitState* getCurrentTextureUnit() const;
    Ogre::Technique* getCurrentTechnique() const;

private:
    QString m_materialName;
    QString m_materialText;
    QStringList m_techniqueList;
    QStringList m_passList;
    QStringList m_textureUnitList;
    int m_selectedTechniqueIndex = -1;
    int m_selectedPassIndex = -1;
    int m_selectedTextureUnitIndex = -1;
    
    // Pass properties
    bool m_lightingEnabled = true;
    bool m_depthWriteEnabled = true;
    bool m_depthCheckEnabled = true;
    QColor m_ambientColor = QColor(0.5f * 255, 0.5f * 255, 0.5f * 255);
    QColor m_diffuseColor = QColor(255, 255, 255);
    QColor m_specularColor = QColor(0, 0, 0);
    QColor m_emissiveColor = QColor(0, 0, 0);
    float m_diffuseAlpha = 1.0f;
    float m_specularAlpha = 1.0f;
    float m_shininess = 0.0f;
    int m_polygonMode = 2; // PM_SOLID (0=Points, 1=Wireframe, 2=Solid)
    int m_sourceBlendFactor = 6; // SBF_ONE 
    int m_destBlendFactor = 1; // SBF_ZERO
    
    // Vertex color tracking
    bool m_useVertexColorToAmbient = false;
    bool m_useVertexColorToDiffuse = false;
    bool m_useVertexColorToSpecular = false;
    bool m_useVertexColorToEmissive = false;
    
    // Texture properties
    QString m_textureName = "*Select a texture*";
    double m_scrollAnimUSpeed = 0.0;
    double m_scrollAnimVSpeed = 0.0;
    
    // Internal Ogre pointers
    Ogre::MaterialPtr m_ogreMaterial;
    QMap<int, QMap<int, Ogre::Pass*>> m_techMap;
    QMap<int, QStringList> m_techMapName;
    QMap<int, Ogre::Pass*> m_passMap;
    QMap<QString, Ogre::TextureUnitState*> m_texUnitMap;
};

#endif // MATERIALEDITORQML_H 