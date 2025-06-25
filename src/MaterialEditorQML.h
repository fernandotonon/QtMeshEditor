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

    // Advanced Pass properties
    Q_PROPERTY(int shadingMode READ shadingMode WRITE setShadingMode NOTIFY shadingModeChanged)
    Q_PROPERTY(int cullHardware READ cullHardware WRITE setCullHardware NOTIFY cullHardwareChanged)
    Q_PROPERTY(int cullSoftware READ cullSoftware WRITE setCullSoftware NOTIFY cullSoftwareChanged)
    Q_PROPERTY(int depthFunction READ depthFunction WRITE setDepthFunction NOTIFY depthFunctionChanged)
    Q_PROPERTY(float depthBiasConstant READ depthBiasConstant WRITE setDepthBiasConstant NOTIFY depthBiasConstantChanged)
    Q_PROPERTY(float depthBiasSlopeScale READ depthBiasSlopeScale WRITE setDepthBiasSlopeScale NOTIFY depthBiasSlopeScaleChanged)
    Q_PROPERTY(bool alphaRejectionEnabled READ alphaRejectionEnabled WRITE setAlphaRejectionEnabled NOTIFY alphaRejectionEnabledChanged)
    Q_PROPERTY(int alphaRejectionFunction READ alphaRejectionFunction WRITE setAlphaRejectionFunction NOTIFY alphaRejectionFunctionChanged)
    Q_PROPERTY(int alphaRejectionValue READ alphaRejectionValue WRITE setAlphaRejectionValue NOTIFY alphaRejectionValueChanged)
    Q_PROPERTY(bool alphaToCoverageEnabled READ alphaToCoverageEnabled WRITE setAlphaToCoverageEnabled NOTIFY alphaToCoverageEnabledChanged)
    Q_PROPERTY(bool colourWriteRed READ colourWriteRed WRITE setColourWriteRed NOTIFY colourWriteRedChanged)
    Q_PROPERTY(bool colourWriteGreen READ colourWriteGreen WRITE setColourWriteGreen NOTIFY colourWriteGreenChanged)
    Q_PROPERTY(bool colourWriteBlue READ colourWriteBlue WRITE setColourWriteBlue NOTIFY colourWriteBlueChanged)
    Q_PROPERTY(bool colourWriteAlpha READ colourWriteAlpha WRITE setColourWriteAlpha NOTIFY colourWriteAlphaChanged)
    Q_PROPERTY(int sceneBlendOperation READ sceneBlendOperation WRITE setSceneBlendOperation NOTIFY sceneBlendOperationChanged)
    Q_PROPERTY(float pointSize READ pointSize WRITE setPointSize NOTIFY pointSizeChanged)
    Q_PROPERTY(float lineWidth READ lineWidth WRITE setLineWidth NOTIFY lineWidthChanged)
    Q_PROPERTY(bool pointSpritesEnabled READ pointSpritesEnabled WRITE setPointSpritesEnabled NOTIFY pointSpritesEnabledChanged)
    Q_PROPERTY(int maxLights READ maxLights WRITE setMaxLights NOTIFY maxLightsChanged)
    Q_PROPERTY(int startLight READ startLight WRITE setStartLight NOTIFY startLightChanged)
    
    // Fog properties
    Q_PROPERTY(bool fogOverride READ fogOverride WRITE setFogOverride NOTIFY fogOverrideChanged)
    Q_PROPERTY(int fogMode READ fogMode WRITE setFogMode NOTIFY fogModeChanged)
    Q_PROPERTY(QColor fogColor READ fogColor WRITE setFogColor NOTIFY fogColorChanged)
    Q_PROPERTY(float fogDensity READ fogDensity WRITE setFogDensity NOTIFY fogDensityChanged)
    Q_PROPERTY(float fogStart READ fogStart WRITE setFogStart NOTIFY fogStartChanged)
    Q_PROPERTY(float fogEnd READ fogEnd WRITE setFogEnd NOTIFY fogEndChanged)
    
    // Texture Unit properties
    Q_PROPERTY(int texCoordSet READ texCoordSet WRITE setTexCoordSet NOTIFY texCoordSetChanged)
    Q_PROPERTY(int textureAddressMode READ textureAddressMode WRITE setTextureAddressMode NOTIFY textureAddressModeChanged)
    Q_PROPERTY(QColor textureBorderColor READ textureBorderColor WRITE setTextureBorderColor NOTIFY textureBorderColorChanged)
    Q_PROPERTY(int textureFiltering READ textureFiltering WRITE setTextureFiltering NOTIFY textureFilteringChanged)
    Q_PROPERTY(int maxAnisotropy READ maxAnisotropy WRITE setMaxAnisotropy NOTIFY maxAnisotropyChanged)
    Q_PROPERTY(float textureUOffset READ textureUOffset WRITE setTextureUOffset NOTIFY textureUOffsetChanged)
    Q_PROPERTY(float textureVOffset READ textureVOffset WRITE setTextureVOffset NOTIFY textureVOffsetChanged)
    Q_PROPERTY(float textureUScale READ textureUScale WRITE setTextureUScale NOTIFY textureUScaleChanged)
    Q_PROPERTY(float textureVScale READ textureVScale WRITE setTextureVScale NOTIFY textureVScaleChanged)
    Q_PROPERTY(float textureRotation READ textureRotation WRITE setTextureRotation NOTIFY textureRotationChanged)
    Q_PROPERTY(int environmentMapping READ environmentMapping WRITE setEnvironmentMapping NOTIFY environmentMappingChanged)
    Q_PROPERTY(double rotateAnimSpeed READ rotateAnimSpeed WRITE setRotateAnimSpeed NOTIFY rotateAnimSpeedChanged)

    // Theme color properties
    Q_PROPERTY(QColor backgroundColor READ backgroundColor CONSTANT)
    Q_PROPERTY(QColor panelColor READ panelColor CONSTANT)
    Q_PROPERTY(QColor textColor READ textColor CONSTANT)
    Q_PROPERTY(QColor borderColor READ borderColor CONSTANT)
    Q_PROPERTY(QColor highlightColor READ highlightColor CONSTANT)
    Q_PROPERTY(QColor buttonColor READ buttonColor CONSTANT)
    Q_PROPERTY(QColor buttonTextColor READ buttonTextColor CONSTANT)
    Q_PROPERTY(QColor disabledTextColor READ disabledTextColor CONSTANT)
    Q_PROPERTY(QColor accentColor READ accentColor CONSTANT)

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

    // Advanced Pass property getters
    int shadingMode() const { return m_shadingMode; }
    int cullHardware() const { return m_cullHardware; }
    int cullSoftware() const { return m_cullSoftware; }
    int depthFunction() const { return m_depthFunction; }
    float depthBiasConstant() const { return m_depthBiasConstant; }
    float depthBiasSlopeScale() const { return m_depthBiasSlopeScale; }
    bool alphaRejectionEnabled() const { return m_alphaRejectionEnabled; }
    int alphaRejectionFunction() const { return m_alphaRejectionFunction; }
    int alphaRejectionValue() const { return m_alphaRejectionValue; }
    bool alphaToCoverageEnabled() const { return m_alphaToCoverageEnabled; }
    bool colourWriteRed() const { return m_colourWriteRed; }
    bool colourWriteGreen() const { return m_colourWriteGreen; }
    bool colourWriteBlue() const { return m_colourWriteBlue; }
    bool colourWriteAlpha() const { return m_colourWriteAlpha; }
    int sceneBlendOperation() const { return m_sceneBlendOperation; }
    float pointSize() const { return m_pointSize; }
    float lineWidth() const { return m_lineWidth; }
    bool pointSpritesEnabled() const { return m_pointSpritesEnabled; }
    int maxLights() const { return m_maxLights; }
    int startLight() const { return m_startLight; }
    
    // Fog property getters
    bool fogOverride() const { return m_fogOverride; }
    int fogMode() const { return m_fogMode; }
    QColor fogColor() const { return m_fogColor; }
    float fogDensity() const { return m_fogDensity; }
    float fogStart() const { return m_fogStart; }
    float fogEnd() const { return m_fogEnd; }
    
    // Texture Unit property getters
    int texCoordSet() const { return m_texCoordSet; }
    int textureAddressMode() const { return m_textureAddressMode; }
    QColor textureBorderColor() const { return m_textureBorderColor; }
    int textureFiltering() const { return m_textureFiltering; }
    int maxAnisotropy() const { return m_maxAnisotropy; }
    float textureUOffset() const { return m_textureUOffset; }
    float textureVOffset() const { return m_textureVOffset; }
    float textureUScale() const { return m_textureUScale; }
    float textureVScale() const { return m_textureVScale; }
    float textureRotation() const { return m_textureRotation; }
    int environmentMapping() const { return m_environmentMapping; }
    double rotateAnimSpeed() const { return m_rotateAnimSpeed; }

    // Theme color getters
    QColor backgroundColor() const { return m_backgroundColor; }
    QColor panelColor() const { return m_panelColor; }
    QColor textColor() const { return m_textColor; }
    QColor borderColor() const { return m_borderColor; }
    QColor highlightColor() const { return m_highlightColor; }
    QColor buttonColor() const { return m_buttonColor; }
    QColor buttonTextColor() const { return m_buttonTextColor; }
    QColor disabledTextColor() const { return m_disabledTextColor; }
    QColor accentColor() const { return m_accentColor; }

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
    
    // Advanced Pass property setters
    void setShadingMode(int mode);
    void setCullHardware(int mode);
    void setCullSoftware(int mode);
    void setDepthFunction(int function);
    void setDepthBiasConstant(float bias);
    void setDepthBiasSlopeScale(float bias);
    void setAlphaRejectionEnabled(bool enabled);
    void setAlphaRejectionFunction(int function);
    void setAlphaRejectionValue(int value);
    void setAlphaToCoverageEnabled(bool enabled);
    void setColourWriteRed(bool enabled);
    void setColourWriteGreen(bool enabled);
    void setColourWriteBlue(bool enabled);
    void setColourWriteAlpha(bool enabled);
    void setSceneBlendOperation(int operation);
    void setPointSize(float size);
    void setLineWidth(float width);
    void setPointSpritesEnabled(bool enabled);
    void setMaxLights(int maxLights);
    void setStartLight(int startLight);
    
    // Fog property setters
    void setFogOverride(bool override);
    void setFogMode(int mode);
    void setFogColor(const QColor &color);
    void setFogDensity(float density);
    void setFogStart(float start);
    void setFogEnd(float end);
    
    // Texture Unit property setters
    void setTexCoordSet(int set);
    void setTextureAddressMode(int mode);
    void setTextureBorderColor(const QColor &color);
    void setTextureFiltering(int filtering);
    void setMaxAnisotropy(int anisotropy);
    void setTextureUOffset(float offset);
    void setTextureVOffset(float offset);
    void setTextureUScale(float scale);
    void setTextureVScale(float scale);
    void setTextureRotation(float rotation);
    void setEnvironmentMapping(int mapping);
    void setRotateAnimSpeed(double speed);
    
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
    
    // Additional utility functions for new properties
    QStringList getShadingModeNames() const;
    QStringList getCullModeNames() const;
    QStringList getDepthFunctionNames() const;
    QStringList getAlphaRejectionFunctionNames() const;
    QStringList getSceneBlendOperationNames() const;
    QStringList getFogModeNames() const;
    QStringList getTextureAddressModeNames() const;
    QStringList getTextureFilteringNames() const;
    QStringList getEnvironmentMappingNames() const;
    
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
    
    // Advanced Pass property change signals
    void shadingModeChanged();
    void cullHardwareChanged();
    void cullSoftwareChanged();
    void depthFunctionChanged();
    void depthBiasConstantChanged();
    void depthBiasSlopeScaleChanged();
    void alphaRejectionEnabledChanged();
    void alphaRejectionFunctionChanged();
    void alphaRejectionValueChanged();
    void alphaToCoverageEnabledChanged();
    void colourWriteRedChanged();
    void colourWriteGreenChanged();
    void colourWriteBlueChanged();
    void colourWriteAlphaChanged();
    void sceneBlendOperationChanged();
    void pointSizeChanged();
    void lineWidthChanged();
    void pointSpritesEnabledChanged();
    void maxLightsChanged();
    void startLightChanged();
    
    // Fog property change signals
    void fogOverrideChanged();
    void fogModeChanged();
    void fogColorChanged();
    void fogDensityChanged();
    void fogStartChanged();
    void fogEndChanged();
    
    // Texture Unit property change signals
    void texCoordSetChanged();
    void textureAddressModeChanged();
    void textureBorderColorChanged();
    void textureFilteringChanged();
    void maxAnisotropyChanged();
    void textureUOffsetChanged();
    void textureVOffsetChanged();
    void textureUScaleChanged();
    void textureVScaleChanged();
    void textureRotationChanged();
    void environmentMappingChanged();
    void rotateAnimSpeedChanged();
    
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
    QColor m_ambientColor;
    QColor m_diffuseColor;
    QColor m_specularColor;
    QColor m_emissiveColor;
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

    // Advanced Pass properties
    int m_shadingMode = 0;
    int m_cullHardware = 0;
    int m_cullSoftware = 0;
    int m_depthFunction = 0;
    float m_depthBiasConstant = 0.0f;
    float m_depthBiasSlopeScale = 0.0f;
    bool m_alphaRejectionEnabled = false;
    int m_alphaRejectionFunction = 0;
    int m_alphaRejectionValue = 0;
    bool m_alphaToCoverageEnabled = false;
    bool m_colourWriteRed = true;
    bool m_colourWriteGreen = true;
    bool m_colourWriteBlue = true;
    bool m_colourWriteAlpha = true;
    int m_sceneBlendOperation = 0;
    float m_pointSize = 1.0f;
    float m_lineWidth = 1.0f;
    bool m_pointSpritesEnabled = false;
    int m_maxLights = 0;
    int m_startLight = 0;
    
    // Fog properties
    bool m_fogOverride = false;
    int m_fogMode = 0;
    QColor m_fogColor;
    float m_fogDensity = 0.0f;
    float m_fogStart = 0.0f;
    float m_fogEnd = 1.0f;
    
    // Texture Unit properties
    int m_texCoordSet = 0;
    int m_textureAddressMode = 0;
    QColor m_textureBorderColor;
    int m_textureFiltering = 0;
    int m_maxAnisotropy = 1;
    float m_textureUOffset = 0.0f;
    float m_textureVOffset = 0.0f;
    float m_textureUScale = 1.0f;
    float m_textureVScale = 1.0f;
    float m_textureRotation = 0.0f;
    int m_environmentMapping = 0;
    double m_rotateAnimSpeed = 0.0;

    // Theme color properties
    QColor m_backgroundColor;
    QColor m_panelColor;
    QColor m_textColor;
    QColor m_borderColor;
    QColor m_highlightColor;
    QColor m_buttonColor;
    QColor m_buttonTextColor;
    QColor m_disabledTextColor;
    QColor m_accentColor;
};

#endif // MATERIALEDITORQML_H 