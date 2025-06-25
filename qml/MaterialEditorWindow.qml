import QtQuick 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0
import QtQuick.Dialogs
import MaterialEditorQML 1.0

ApplicationWindow {
    id: window
    width: 1400
    height: 900
    visible: true
    title: "QML Material Editor"

    property bool isLoading: true
    
    // Dynamic theme colors based on system palette
    readonly property color backgroundColor: palette.window
    readonly property color panelColor: palette.base
    readonly property color textColor: palette.windowText
    readonly property color borderColor: palette.mid
    readonly property color highlightColor: palette.highlight
    
    SystemPalette {
        id: palette
        colorGroup: SystemPalette.Active
    }

    // Custom color picker popup - working alternative to ColorDialog
    Popup {
        id: colorPickerPopup
        width: 350
        height: 300
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        
        property string colorType: ""
        property color currentColor: "white"
        
        function openForColor(type, color) {
            colorType = type
            currentColor = color
            open()
        }
        
        Rectangle {
            anchors.fill: parent
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 4
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 15
                spacing: 15
                
                Text {
                    text: "Select " + colorPickerPopup.colorType.charAt(0).toUpperCase() + colorPickerPopup.colorType.slice(1) + " Color"
                    font.pointSize: 12
                    font.bold: true
                    color: textColor
                }
                
                // Predefined colors grid
                GridLayout {
                    Layout.fillWidth: true
                    columns: 8
                    columnSpacing: 5
                    rowSpacing: 5
                    
                    property var colors: [
                        "#ffffff", "#f0f0f0", "#d0d0d0", "#808080", "#404040", "#202020", "#000000", "#800000",
                        "#ff0000", "#ff8080", "#ffff00", "#80ff00", "#00ff00", "#00ff80", "#00ffff", "#0080ff",
                        "#0000ff", "#8000ff", "#ff00ff", "#ff0080", "#800080", "#008000", "#008080", "#000080",
                        "#ffa500", "#ff6347", "#ffd700", "#90ee90", "#87ceeb", "#dda0dd", "#f0e68c", "#ffc0cb"
                    ]
                    
                    Repeater {
                        model: parent.colors
                        
                        Rectangle {
                            width: 30
                            height: 30
                            color: modelData
                            border.color: "#404040"
                            border.width: 1
                            radius: 3
                            
                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    var selectedColor = Qt.color(modelData)
                                    
                                    // Set the selected color based on type
                                    switch(colorPickerPopup.colorType) {
                                        case "ambient":
                                            MaterialEditorQML.setAmbientColor(selectedColor)
                                            break
                                        case "diffuse":
                                            MaterialEditorQML.setDiffuseColor(selectedColor)
                                            break
                                        case "specular":
                                            MaterialEditorQML.setSpecularColor(selectedColor)
                                            break
                                        case "emissive":
                                            MaterialEditorQML.setEmissiveColor(selectedColor)
                                            break
                                    }
                                    
                                    colorPickerPopup.close()
                                }
                                cursorShape: Qt.PointingHandCursor
                            }
                        }
                    }
                }
                
                // Current color display
                Rectangle {
                    Layout.fillWidth: true
                    height: 40
                    color: colorPickerPopup.currentColor
                    border.color: borderColor
                    border.width: 2
                    radius: 4
                    
                    Text {
                        anchors.centerIn: parent
                        text: "Current: " + colorPickerPopup.currentColor
                        color: Qt.colorDistance(colorPickerPopup.currentColor, "white") > 0.5 ? "white" : "black"
                        font.pointSize: 10
                    }
                }
                
                // Buttons
                RowLayout {
                    Layout.fillWidth: true
                    
                    Button {
                        text: "Cancel"
                        onClicked: colorPickerPopup.close()
                    }
                    
                    Item { Layout.fillWidth: true }
                    
                    Button {
                        text: "Reset to White"
                        onClicked: {
                            var whiteColor = Qt.color("white")
                            switch(colorPickerPopup.colorType) {
                                case "ambient":
                                    MaterialEditorQML.setAmbientColor(whiteColor)
                                    break
                                case "diffuse":
                                    MaterialEditorQML.setDiffuseColor(whiteColor)
                                    break
                                case "specular":
                                    MaterialEditorQML.setSpecularColor(whiteColor)
                                    break
                                case "emissive":
                                    MaterialEditorQML.setEmissiveColor(whiteColor)
                                    break
                            }
                            colorPickerPopup.close()
                        }
                    }
                }
            }
        }
    }

    Component.onCompleted: {
        console.log("MaterialEditorWindow loaded")
        console.log("MaterialEditorQML.materialName:", MaterialEditorQML.materialName)
        console.log("MaterialEditorQML.materialText length:", MaterialEditorQML.materialText.length)
        
        // Test if MaterialEditorQML is available
        try {
            console.log("Testing MaterialEditorQML functions...")
            console.log("Polygon modes:", MaterialEditorQML.getPolygonModeNames())
            console.log("Blend factors:", MaterialEditorQML.getBlendFactorNames())
            
            // If no material is loaded, create a default one
            if (!MaterialEditorQML.materialName || MaterialEditorQML.materialName === "") {
                console.log("No material loaded, creating default material")
                MaterialEditorQML.createNewMaterial("default_material")
            }
            
            isLoading = false
            
        } catch (error) {
            console.error("Error with MaterialEditorQML:", error)
            isLoading = false
        }
    }

    // Helper function to get current cursor context
    function getCurrentContext() {
        var text = materialTextArea.text
        var cursorPos = materialTextArea.cursorPosition
        var beforeCursor = text.substring(0, cursorPos)
        
        // Count techniques and passes before cursor
        var techniqueMatches = beforeCursor.match(/technique/g)
        var passMatches = beforeCursor.match(/pass(?!\s*{)/g) // pass not followed by {
        
        var currentTechnique = techniqueMatches ? techniqueMatches.length - 1 : 0
        var currentPass = passMatches ? passMatches.length - 1 : 0
        
        return {
            technique: Math.max(0, currentTechnique),
            pass: Math.max(0, currentPass)
        }
    }

    // Main content
    Rectangle {
        anchors.fill: parent
        color: backgroundColor

        SplitView {
            anchors.fill: parent
            anchors.margins: 10
            orientation: Qt.Horizontal
            visible: !isLoading

            // Left Panel - Script Editor
            Rectangle {
                SplitView.minimumWidth: 400
                SplitView.preferredWidth: 600
                color: panelColor
                border.color: borderColor
                border.width: 1
                radius: 4

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 15
                    spacing: 15

                    // Header
                    Text {
                        text: "Material Script Editor"
                        font.pointSize: 16
                        font.bold: true
                        color: textColor
                    }

                    // Material Info
                    Rectangle {
                        Layout.fillWidth: true
                        height: 80
                        color: Qt.darker(panelColor, 1.1)
                        border.color: borderColor
                        border.width: 1
                        radius: 4

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 5

                            Text {
                                text: "Material: " + (MaterialEditorQML.materialName || "No material")
                                font.pointSize: 12
                                font.bold: true
                                color: textColor
                            }

                            RowLayout {
                                Text {
                                    text: "Techniques: " + (MaterialEditorQML.techniqueList ? MaterialEditorQML.techniqueList.length : 0)
                                    color: Qt.darker(textColor, 1.3)
                                }
                                Text {
                                    text: "Passes: " + (MaterialEditorQML.passList ? MaterialEditorQML.passList.length : 0)
                                    color: Qt.darker(textColor, 1.3)
                                }
                                Text {
                                    text: "Size: " + (MaterialEditorQML.materialText ? MaterialEditorQML.materialText.length : 0) + " chars"
                                    color: Qt.darker(textColor, 1.3)
                                }
                            }
                        }
                    }

                    // Toolbar
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Button {
                            text: "Apply"
                            enabled: materialTextArea.text !== MaterialEditorQML.materialText
                            onClicked: {
                                if (materialTextArea.text) {
                                    MaterialEditorQML.setMaterialText(materialTextArea.text)
                                    MaterialEditorQML.applyMaterial()
                                }
                            }
                        }

                        Button {
                            text: "Validate"
                            onClicked: {
                                if (MaterialEditorQML.validateMaterialScript(materialTextArea.text || "")) {
                                    statusText.text = "Material script is valid"
                                    statusText.color = "green"
                                } else {
                                    statusText.text = "Material script has errors"
                                    statusText.color = "red"
                                }
                            }
                        }

                        Button {
                            text: "Smart Technique"
                            ToolTip.text: "Add technique at cursor position"
                            onClicked: {
                                var context = getCurrentContext()
                                // Create the new technique directly (this updates the Ogre material)
                                MaterialEditorQML.createNewTechnique("technique" + (context.technique + 1))
                                // Force refresh of the material text from the updated Ogre material
                                materialTextArea.text = MaterialEditorQML.materialText
                                statusText.text = "Added technique at position " + (context.technique + 1)
                                statusText.color = "blue"
                            }
                        }

                        Button {
                            text: "Smart Pass"
                            ToolTip.text: "Add pass to current technique"
                            onClicked: {
                                var context = getCurrentContext()
                                // Create the new pass directly (this updates the Ogre material)
                                MaterialEditorQML.createNewPass("pass" + (context.pass + 1))
                                // Force refresh of the material text from the updated Ogre material
                                materialTextArea.text = MaterialEditorQML.materialText
                                statusText.text = "Added pass to technique " + context.technique
                                statusText.color = "blue"
                            }
                        }

                        Item { Layout.fillWidth: true }

                        Text {
                            id: statusText
                            text: "Ready"
                            color: "black"
                        }
                    }

                    // Text editor
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        TextArea {
                            id: materialTextArea
                            text: MaterialEditorQML.materialText || "material default_material\n{\n\ttechnique\n\t{\n\t\tpass\n\t\t{\n\t\t}\n\t}\n}"
                            selectByMouse: true
                            font.family: "monospace"
                            font.pointSize: 11
                            wrapMode: TextArea.Wrap
                            background: Rectangle {
                                color: "white"
                                border.color: "#ccc"
                                border.width: 1
                                radius: 2
                            }

                            onTextChanged: {
                                if (text !== MaterialEditorQML.materialText) {
                                    statusText.text = "Modified"
                                    statusText.color = "orange"
                                }
                            }

                            onCursorPositionChanged: {
                                var context = getCurrentContext()
                                cursorInfoText.text = "Cursor: T" + context.technique + " P" + context.pass
                            }
                        }
                    }

                    // Cursor info
                    Text {
                        id: cursorInfoText
                        text: "Cursor: T0 P0"
                        color: "#666"
                        font.pointSize: 9
                    }
                }
            }

            // Right Panel - Properties Form
            Rectangle {
                SplitView.minimumWidth: 350
                SplitView.preferredWidth: 450
                color: panelColor
                border.color: borderColor
                border.width: 1
                radius: 4

                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 15
                    clip: true

                    ColumnLayout {
                        width: parent.width - 30
                        spacing: 20

                        // Header
                        Text {
                            text: "Material Properties"
                            font.pointSize: 16
                            font.bold: true
                            color: textColor
                        }

                        // Technique Management
                        GroupBox {
                            title: "Techniques"
                            Layout.fillWidth: true

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 10

                                RowLayout {
                                    Layout.fillWidth: true

                                    ComboBox {
                                        id: techniqueCombo
                                        Layout.fillWidth: true
                                        model: MaterialEditorQML.techniqueList
                                        currentIndex: MaterialEditorQML.selectedTechniqueIndex
                                        onCurrentIndexChanged: {
                                            MaterialEditorQML.setSelectedTechniqueIndex(currentIndex)
                                        }
                                    }

                                    Button {
                                        text: "New"
                                        onClicked: newTechniqueDialog.open()
                                    }
                                }
                            }
                        }

                        // Pass Management
                        GroupBox {
                            title: "Passes"
                            Layout.fillWidth: true
                            enabled: MaterialEditorQML.selectedTechniqueIndex >= 0

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 10

                                RowLayout {
                                    Layout.fillWidth: true

                                    ComboBox {
                                        id: passCombo
                                        Layout.fillWidth: true
                                        model: MaterialEditorQML.passList
                                        currentIndex: MaterialEditorQML.selectedPassIndex
                                        onCurrentIndexChanged: {
                                            if (currentIndex !== MaterialEditorQML.selectedPassIndex) {
                                                MaterialEditorQML.setSelectedPassIndex(currentIndex)
                                            }
                                        }
                                    }

                                    Button {
                                        text: "New"
                                        onClicked: newPassDialog.open()
                                    }
                                }
                            }
                        }

                        // Pass Properties Panel
                        GroupBox {
                            title: "Pass Properties"
                            Layout.fillWidth: true
                            enabled: MaterialEditorQML.selectedPassIndex >= 0

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 15

                                // Lighting and Depth Settings
                                GroupBox {
                                    title: "Lighting & Depth"
                                    Layout.fillWidth: true

                                    ColumnLayout {
                                        anchors.fill: parent
                                        spacing: 10

                                        // First row: Lighting, Depth Write, Depth Check
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 15

                                            CheckBox {
                                                text: "Lighting"
                                                checked: MaterialEditorQML.lightingEnabled
                                                onCheckedChanged: MaterialEditorQML.setLightingEnabled(checked)
                                            }

                                            CheckBox {
                                                text: "Depth Write"
                                                checked: MaterialEditorQML.depthWriteEnabled
                                                onCheckedChanged: MaterialEditorQML.setDepthWriteEnabled(checked)
                                            }

                                            CheckBox {
                                                text: "Depth Check"
                                                checked: MaterialEditorQML.depthCheckEnabled
                                                onCheckedChanged: MaterialEditorQML.setDepthCheckEnabled(checked)
                                            }

                                            // Spacer to push everything to the left
                                            Item { Layout.fillWidth: true }
                                        }

                                        // Second row: Polygon Mode
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 15

                                            Label { 
                                                text: "Polygon Mode:" 
                                                color: textColor
                                                Layout.alignment: Qt.AlignVCenter
                                            }
                                            ComboBox {
                                                id: polygonModeComboMain
                                                model: MaterialEditorQML.getPolygonModeNames()
                                                currentIndex: MaterialEditorQML.polygonMode
                                                onCurrentIndexChanged: {
                                                    if (currentIndex !== MaterialEditorQML.polygonMode) {
                                                        MaterialEditorQML.setPolygonMode(currentIndex)
                                                    }
                                                }
                                                
                                                // Ensure the ComboBox updates when the backend changes
                                                Connections {
                                                    target: MaterialEditorQML
                                                    function onPolygonModeChanged() {
                                                        polygonModeComboMain.currentIndex = MaterialEditorQML.polygonMode
                                                    }
                                                }
                                            }

                                            // Spacer to push everything to the left
                                            Item { Layout.fillWidth: true }
                                        }
                                    }
                                }

                                // Colors
                                GroupBox {
                                    title: "Colors"
                                    Layout.fillWidth: true

                                    GridLayout {
                                        anchors.fill: parent
                                        columns: 3
                                        rowSpacing: 10
                                        columnSpacing: 10

                                        // Ambient
                                        Label { 
                                            text: "Ambient:" 
                                            color: textColor
                                        }
                                        Rectangle {
                                            width: 60
                                            height: 25
                                            color: MaterialEditorQML.ambientColor
                                            border.color: borderColor
                                            border.width: 1
                                            radius: 3

                                            MouseArea {
                                                anchors.fill: parent
                                                onClicked: {
                                                    console.log("Opening ambient color picker")
                                                    colorPickerPopup.openForColor("ambient", MaterialEditorQML.ambientColor)
                                                }
                                                cursorShape: Qt.PointingHandCursor
                                            }
                                        }
                                        CheckBox {
                                            text: "Use Vertex"
                                            checked: MaterialEditorQML.useVertexColorToAmbient
                                            onCheckedChanged: MaterialEditorQML.setUseVertexColorToAmbient(checked)
                                        }

                                        // Diffuse
                                        Label { 
                                            text: "Diffuse:" 
                                            color: textColor
                                        }
                                        Rectangle {
                                            width: 60
                                            height: 25
                                            color: MaterialEditorQML.diffuseColor
                                            border.color: borderColor
                                            border.width: 1
                                            radius: 3

                                            MouseArea {
                                                anchors.fill: parent
                                                onClicked: {
                                                    console.log("Opening diffuse color picker")
                                                    colorPickerPopup.openForColor("diffuse", MaterialEditorQML.diffuseColor)
                                                }
                                                cursorShape: Qt.PointingHandCursor
                                            }
                                        }
                                        CheckBox {
                                            text: "Use Vertex"
                                            checked: MaterialEditorQML.useVertexColorToDiffuse
                                            onCheckedChanged: MaterialEditorQML.setUseVertexColorToDiffuse(checked)
                                        }

                                        // Specular
                                        Label { 
                                            text: "Specular:" 
                                            color: textColor
                                        }
                                        Rectangle {
                                            width: 60
                                            height: 25
                                            color: MaterialEditorQML.specularColor
                                            border.color: borderColor
                                            border.width: 1
                                            radius: 3

                                            MouseArea {
                                                anchors.fill: parent
                                                onClicked: {
                                                    console.log("Opening specular color picker")
                                                    colorPickerPopup.openForColor("specular", MaterialEditorQML.specularColor)
                                                }
                                                cursorShape: Qt.PointingHandCursor
                                            }
                                        }
                                        CheckBox {
                                            text: "Use Vertex"
                                            checked: MaterialEditorQML.useVertexColorToSpecular
                                            onCheckedChanged: MaterialEditorQML.setUseVertexColorToSpecular(checked)
                                        }

                                        // Emissive
                                        Label { 
                                            text: "Emissive:" 
                                            color: textColor
                                        }
                                        Rectangle {
                                            width: 60
                                            height: 25
                                            color: MaterialEditorQML.emissiveColor
                                            border.color: borderColor
                                            border.width: 1
                                            radius: 3

                                            MouseArea {
                                                anchors.fill: parent
                                                onClicked: {
                                                    console.log("Opening emissive color picker")
                                                    colorPickerPopup.openForColor("emissive", MaterialEditorQML.emissiveColor)
                                                }
                                                cursorShape: Qt.PointingHandCursor
                                            }
                                        }
                                        CheckBox {
                                            text: "Use Vertex"
                                            checked: MaterialEditorQML.useVertexColorToEmissive
                                            onCheckedChanged: MaterialEditorQML.setUseVertexColorToEmissive(checked)
                                        }
                                    }
                                }

                                // Alpha and Material Properties
                                GroupBox {
                                    title: "Alpha & Material"
                                    Layout.fillWidth: true

                                    GridLayout {
                                        anchors.fill: parent
                                        columns: 2
                                        rowSpacing: 10

                                        Label { text: "Diffuse Alpha:" }
                                        RowLayout {
                                            Slider {
                                                id: diffuseAlphaSlider
                                                from: 0.0
                                                to: 1.0
                                                property bool updating: false
                                                value: MaterialEditorQML.diffuseAlpha
                                                onValueChanged: {
                                                    if (!updating && Math.abs(value - MaterialEditorQML.diffuseAlpha) > 0.001) {
                                                        updating = true
                                                        MaterialEditorQML.setDiffuseAlpha(value)
                                                        updating = false
                                                    }
                                                }
                                                Layout.fillWidth: true
                                            }
                                            SpinBox {
                                                id: diffuseAlphaSpinBox
                                                from: 0
                                                to: 100
                                                property bool updating: false
                                                
                                                Component.onCompleted: {
                                                    value = Math.round(MaterialEditorQML.diffuseAlpha * 100)
                                                }
                                                
                                                Connections {
                                                    target: MaterialEditorQML
                                                    function onDiffuseAlphaChanged() {
                                                        if (!diffuseAlphaSpinBox.updating) {
                                                            diffuseAlphaSpinBox.value = Math.round(MaterialEditorQML.diffuseAlpha * 100)
                                                        }
                                                    }
                                                }
                                                
                                                onValueChanged: {
                                                    if (!updating) {
                                                        updating = true
                                                        MaterialEditorQML.setDiffuseAlpha(value / 100.0)
                                                        updating = false
                                                    }
                                                }
                                                textFromValue: function(value) { return value + "%" }
                                                valueFromText: function(text) { return parseInt(text.replace("%", "")) }
                                            }
                                        }

                                        Label { text: "Specular Alpha:" }
                                        RowLayout {
                                            Slider {
                                                id: specularAlphaSlider
                                                from: 0.0
                                                to: 1.0
                                                property bool updating: false
                                                value: MaterialEditorQML.specularAlpha
                                                onValueChanged: {
                                                    if (!updating && Math.abs(value - MaterialEditorQML.specularAlpha) > 0.001) {
                                                        updating = true
                                                        MaterialEditorQML.setSpecularAlpha(value)
                                                        updating = false
                                                    }
                                                }
                                                Layout.fillWidth: true
                                            }
                                            SpinBox {
                                                id: specularAlphaSpinBox
                                                from: 0
                                                to: 100
                                                property bool updating: false
                                                
                                                Component.onCompleted: {
                                                    value = Math.round(MaterialEditorQML.specularAlpha * 100)
                                                }
                                                
                                                Connections {
                                                    target: MaterialEditorQML
                                                    function onSpecularAlphaChanged() {
                                                        if (!specularAlphaSpinBox.updating) {
                                                            specularAlphaSpinBox.value = Math.round(MaterialEditorQML.specularAlpha * 100)
                                                        }
                                                    }
                                                }
                                                
                                                onValueChanged: {
                                                    if (!updating) {
                                                        updating = true
                                                        MaterialEditorQML.setSpecularAlpha(value / 100.0)
                                                        updating = false
                                                    }
                                                }
                                                textFromValue: function(value) { return value + "%" }
                                                valueFromText: function(text) { return parseInt(text.replace("%", "")) }
                                            }
                                        }

                                        Label { text: "Shininess:" }
                                        RowLayout {
                                            Slider {
                                                id: shininessSlider
                                                from: 0.0
                                                to: 128.0
                                                property bool updating: false
                                                value: MaterialEditorQML.shininess
                                                onValueChanged: {
                                                    if (!updating && Math.abs(value - MaterialEditorQML.shininess) > 0.1) {
                                                        updating = true
                                                        MaterialEditorQML.setShininess(value)
                                                        updating = false
                                                    }
                                                }
                                                Layout.fillWidth: true
                                            }
                                            SpinBox {
                                                id: shininessSpinBox
                                                from: 0
                                                to: 128
                                                property bool updating: false
                                                
                                                Component.onCompleted: {
                                                    value = Math.round(MaterialEditorQML.shininess)
                                                }
                                                
                                                Connections {
                                                    target: MaterialEditorQML
                                                    function onShininessChanged() {
                                                        if (!shininessSpinBox.updating) {
                                                            shininessSpinBox.value = Math.round(MaterialEditorQML.shininess)
                                                        }
                                                    }
                                                }
                                                
                                                onValueChanged: {
                                                    if (!updating) {
                                                        updating = true
                                                        MaterialEditorQML.setShininess(value)
                                                        updating = false
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                // Blending
                                GroupBox {
                                    title: "Blending"
                                    Layout.fillWidth: true

                                    GridLayout {
                                        anchors.fill: parent
                                        columns: 2
                                        rowSpacing: 10

                                        Label { text: "Source Blend:" }
                                        ComboBox {
                                            Layout.fillWidth: true
                                            model: MaterialEditorQML.getBlendFactorNames()
                                            currentIndex: MaterialEditorQML.sourceBlendFactor
                                            onCurrentIndexChanged: MaterialEditorQML.setSourceBlendFactor(currentIndex)
                                        }

                                        Label { text: "Dest Blend:" }
                                        ComboBox {
                                            Layout.fillWidth: true
                                            model: MaterialEditorQML.getBlendFactorNames()
                                            currentIndex: MaterialEditorQML.destBlendFactor
                                            onCurrentIndexChanged: MaterialEditorQML.setDestBlendFactor(currentIndex)
                                        }
                                    }
                                }
                            }
                        }

                        // Texture Unit Management
                        GroupBox {
                            title: "Texture Units"
                            Layout.fillWidth: true

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 10

                                RowLayout {
                                    Layout.fillWidth: true

                                    ComboBox {
                                        id: textureUnitCombo
                                        Layout.fillWidth: true
                                        model: MaterialEditorQML.textureUnitList
                                        currentIndex: MaterialEditorQML.selectedTextureUnitIndex
                                        onCurrentIndexChanged: {
                                            MaterialEditorQML.setSelectedTextureUnitIndex(currentIndex)
                                        }
                                    }

                                    Button {
                                        text: "New"
                                        onClicked: newTextureUnitDialog.open()
                                    }

                                    Button {
                                        text: "Remove"
                                        enabled: MaterialEditorQML.selectedTextureUnitIndex >= 0
                                        onClicked: MaterialEditorQML.removeTexture()
                                    }
                                }

                                // Texture Properties
                                GroupBox {
                                    title: "Texture Properties"
                                    Layout.fillWidth: true
                                    enabled: MaterialEditorQML.selectedTextureUnitIndex >= 0

                                    ColumnLayout {
                                        anchors.fill: parent
                                        spacing: 10

                                        RowLayout {
                                            TextField {
                                                id: textureNameField
                                                text: MaterialEditorQML.textureName
                                                placeholderText: "Texture filename"
                                                Layout.fillWidth: true
                                                onTextChanged: {
                                                    if (text !== MaterialEditorQML.textureName) {
                                                        MaterialEditorQML.setTextureName(text)
                                                    }
                                                }
                                            }

                                            Button {
                                                text: "Browse"
                                                onClicked: textureFileDialog.open()
                                            }
                                        }

                                        // Animation Controls
                                                                                 Label { text: "U Scroll Speed:" }
                                         RowLayout {
                                             Slider {
                                                 id: uScrollSlider
                                                 from: -10.0
                                                 to: 10.0
                                                 property bool updating: false
                                                 value: MaterialEditorQML.scrollAnimUSpeed
                                                 onValueChanged: {
                                                     if (!updating && Math.abs(value - MaterialEditorQML.scrollAnimUSpeed) > 0.01) {
                                                         updating = true
                                                         MaterialEditorQML.setScrollAnimUSpeed(value)
                                                         updating = false
                                                     }
                                                 }
                                                 Layout.fillWidth: true
                                             }
                                             SpinBox {
                                                 id: uScrollSpinBox
                                                 from: -1000
                                                 to: 1000
                                                 property bool updating: false
                                                 
                                                 Component.onCompleted: {
                                                     value = Math.round(MaterialEditorQML.scrollAnimUSpeed * 100)
                                                 }
                                                 
                                                 Connections {
                                                     target: MaterialEditorQML
                                                     function onScrollAnimUSpeedChanged() {
                                                         if (!uScrollSpinBox.updating) {
                                                             uScrollSpinBox.value = Math.round(MaterialEditorQML.scrollAnimUSpeed * 100)
                                                         }
                                                     }
                                                 }
                                                 
                                                 onValueChanged: {
                                                     if (!updating) {
                                                         updating = true
                                                         MaterialEditorQML.setScrollAnimUSpeed(value / 100.0)
                                                         updating = false
                                                     }
                                                 }
                                                 textFromValue: function(value) { return (value / 100).toFixed(2) }
                                                 valueFromText: function(text) { return parseFloat(text) * 100 }
                                             }
                                         }

                                         Label { text: "V Scroll Speed:" }
                                         RowLayout {
                                             Slider {
                                                 id: vScrollSlider
                                                 from: -10.0
                                                 to: 10.0
                                                 property bool updating: false
                                                 value: MaterialEditorQML.scrollAnimVSpeed
                                                 onValueChanged: {
                                                     if (!updating && Math.abs(value - MaterialEditorQML.scrollAnimVSpeed) > 0.01) {
                                                         updating = true
                                                         MaterialEditorQML.setScrollAnimVSpeed(value)
                                                         updating = false
                                                     }
                                                 }
                                                 Layout.fillWidth: true
                                             }
                                             SpinBox {
                                                 id: vScrollSpinBox
                                                 from: -1000
                                                 to: 1000
                                                 property bool updating: false
                                                 
                                                 Component.onCompleted: {
                                                     value = Math.round(MaterialEditorQML.scrollAnimVSpeed * 100)
                                                 }
                                                 
                                                 Connections {
                                                     target: MaterialEditorQML
                                                     function onScrollAnimVSpeedChanged() {
                                                         if (!vScrollSpinBox.updating) {
                                                             vScrollSpinBox.value = Math.round(MaterialEditorQML.scrollAnimVSpeed * 100)
                                                         }
                                                     }
                                                 }
                                                 
                                                 onValueChanged: {
                                                     if (!updating) {
                                                         updating = true
                                                         MaterialEditorQML.setScrollAnimVSpeed(value / 100.0)
                                                         updating = false
                                                     }
                                                 }
                                                 textFromValue: function(value) { return (value / 100).toFixed(2) }
                                                 valueFromText: function(text) { return parseFloat(text) * 100 }
                                             }
                                         }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Loading overlay
        Rectangle {
            anchors.fill: parent
            color: "#80000000"
            visible: isLoading

            Text {
                anchors.centerIn: parent
                text: "Loading Material Editor..."
                color: "white"
                font.pointSize: 18
            }
        }
    }

    // Dialogs
    Dialog {
        id: newTechniqueDialog
        title: "Create New Technique"
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: 300
        height: 150

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10

            Text { text: "Technique name:" }
            TextField {
                id: techniqueNameField
                Layout.fillWidth: true
                placeholderText: "Enter technique name"
            }
        }

        onAccepted: {
            if (techniqueNameField.text.trim() !== "") {
                // Create the new technique directly (this updates the Ogre material)
                MaterialEditorQML.createNewTechnique(techniqueNameField.text.trim())
                // Force refresh of the material text from the updated Ogre material
                materialTextArea.text = MaterialEditorQML.materialText
                techniqueNameField.text = ""
            }
        }
    }

    Dialog {
        id: newPassDialog
        title: "Create New Pass"
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: 300
        height: 150

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10

            Text { text: "Pass name:" }
            TextField {
                id: passNameField
                Layout.fillWidth: true
                placeholderText: "Enter pass name"
            }
        }

        onAccepted: {
            if (passNameField.text.trim() !== "") {
                // Create the new pass directly (this updates the Ogre material)
                MaterialEditorQML.createNewPass(passNameField.text.trim())
                // Force refresh of the material text from the updated Ogre material
                materialTextArea.text = MaterialEditorQML.materialText
                passNameField.text = ""
            }
        }
    }

    Dialog {
        id: newTextureUnitDialog
        title: "Create New Texture Unit"
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: 300
        height: 150

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10

            Text { text: "Texture unit name:" }
            TextField {
                id: textureUnitNameField
                Layout.fillWidth: true
                placeholderText: "Enter texture unit name"
            }
        }

        onAccepted: {
            if (textureUnitNameField.text.trim() !== "") {
                MaterialEditorQML.createNewTextureUnit(textureUnitNameField.text.trim())
                textureUnitNameField.text = ""
            }
        }
    }

    // Color pickers are now declared at the top of the window for proper accessibility

    // File dialog for texture selection
    FileDialog {
        id: textureFileDialog
        title: "Select Texture"
        fileMode: FileDialog.OpenFile
        nameFilters: [
            "Image files (*.png *.jpg *.jpeg *.bmp *.tga *.dds)",
            "All files (*)"
        ]
        onAccepted: {
            var path = selectedFile.toString()
            var fileName = path.substring(path.lastIndexOf('/') + 1)
            MaterialEditorQML.setTextureName(fileName)
            textureNameField.text = fileName
        }
    }

    // Update connections
    Connections {
        target: MaterialEditorQML
        function onMaterialTextChanged() {
            if (materialTextArea.text !== MaterialEditorQML.materialText) {
                materialTextArea.text = MaterialEditorQML.materialText
            }
        }
        
        function onErrorOccurred(error) {
            statusText.text = "Error: " + error
            statusText.color = "red"
        }
        
        function onMaterialApplied() {
            statusText.text = "Material applied successfully"
            statusText.color = "green"
        }
        
        function onTextureNameChanged() {
            if (textureNameField.text !== MaterialEditorQML.textureName) {
                textureNameField.text = MaterialEditorQML.textureName
            }
        }
        
        // Hierarchy update signals
        function onSelectedTechniqueIndexChanged() {
            if (techniqueCombo.currentIndex !== MaterialEditorQML.selectedTechniqueIndex) {
                techniqueCombo.currentIndex = MaterialEditorQML.selectedTechniqueIndex
            }
        }
        
        function onSelectedPassIndexChanged() {
            if (passCombo.currentIndex !== MaterialEditorQML.selectedPassIndex) {
                passCombo.currentIndex = MaterialEditorQML.selectedPassIndex
            }
        }
        
        function onSelectedTextureUnitIndexChanged() {
            if (textureUnitCombo.currentIndex !== MaterialEditorQML.selectedTextureUnitIndex) {
                textureUnitCombo.currentIndex = MaterialEditorQML.selectedTextureUnitIndex
            }
        }
        
        // List update signals
        function onTechniqueListChanged() {
            console.log("Technique list updated:", MaterialEditorQML.techniqueList)
        }
        
        function onPassListChanged() {
            console.log("Pass list updated:", MaterialEditorQML.passList)
        }
        
        function onTextureUnitListChanged() {
            console.log("Texture unit list updated:", MaterialEditorQML.textureUnitList)
        }
    }
} 