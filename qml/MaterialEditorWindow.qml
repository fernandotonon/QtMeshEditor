import QtQuick 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0
import QtQuick.Dialogs
import Qt.labs.platform 1.1 as Labs
import MaterialEditorQML 1.0

ApplicationWindow {
    id: window
    width: 1400
    height: 900
    visible: true
    title: "QML Material Editor"

    property bool isLoading: true
    
    // Enhanced dynamic theme colors based on system palette
    readonly property color backgroundColor: palette.window
    readonly property color panelColor: palette.base
    readonly property color textColor: palette.windowText
    readonly property color borderColor: palette.mid
    readonly property color highlightColor: palette.highlight
    readonly property color buttonColor: palette.button
    readonly property color buttonTextColor: palette.buttonText
    readonly property color alternateColor: palette.alternateBase
    readonly property color lightColor: palette.light
    readonly property color darkColor: palette.dark
    readonly property color disabledTextColor: palette.placeholderText
    
    SystemPalette {
        id: palette
        colorGroup: SystemPalette.Active
    }

    // AI Status Management
    QtObject {
        id: aiStatusIndicator
        property bool isGenerating: false
        property bool hasError: false
        property string errorMessage: ""
    }

    // Connect to MaterialEditorQML AI signals
    Connections {
        target: MaterialEditorQML
        
        function onAiGenerationStarted() {
            aiStatusIndicator.isGenerating = true
            aiStatusIndicator.hasError = false
            aiStatusIndicator.errorMessage = ""
            statusText.text = "AI generating material..."
            statusText.color = "orange"
        }
        
        function onAiGenerationCompleted(generatedScript) {
            aiStatusIndicator.isGenerating = false
            aiStatusIndicator.hasError = false
            statusText.text = "AI generation completed"
            statusText.color = "green"
        }
        
        function onAiGenerationError(error) {
            aiStatusIndicator.isGenerating = false
            aiStatusIndicator.hasError = true
            aiStatusIndicator.errorMessage = error
            statusText.text = "AI error: " + error
            statusText.color = "red"
        }
    }

    // Simplified Button component
    component ThemedButton: Button {
        background: Rectangle {
            color: parent.down ? Qt.darker(buttonColor, 1.2) : 
                   parent.hovered ? Qt.lighter(buttonColor, 1.1) : buttonColor
            border.color: borderColor
            border.width: 1
            radius: 4
        }
        contentItem: Text {
            text: parent.text
            font: parent.font
            color: parent.enabled ? buttonTextColor : disabledTextColor
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    // Themed ComboBox - simplified approach
    component ThemedComboBox: ComboBox {
        id: themedCombo
        
        background: Rectangle {
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 4
        }
        contentItem: Text {
            text: themedCombo.displayText
            font: themedCombo.font
            color: textColor
            verticalAlignment: Text.AlignVCenter
            leftPadding: 8
            rightPadding: 30
        }
        indicator: Text {
            x: themedCombo.width - width - 8
            y: themedCombo.topPadding + (themedCombo.availableHeight - height) / 2
            text: "▼"
            color: textColor
            font.pointSize: 8
        }
        
        popup: Popup {
            y: themedCombo.height - 1
            width: themedCombo.width
            implicitHeight: contentItem.implicitHeight
            padding: 1
            
            background: Rectangle {
                color: panelColor
                border.color: borderColor
                border.width: 1
                radius: 4
            }
            
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: themedCombo.delegateModel
                currentIndex: themedCombo.highlightedIndex
                ScrollIndicator.vertical: ScrollIndicator { 
                    active: true
                }
            }
        }
        
        delegate: ItemDelegate {
            width: themedCombo.width
            height: 30
            
            contentItem: Text {
                text: modelData || ""
                color: textColor
                font.pointSize: 11
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                leftPadding: 8
            }
            
            background: Rectangle {
                color: parent.hovered ? highlightColor : panelColor
                radius: 2
                border.color: parent.hovered ? borderColor : "transparent"
                border.width: 1
            }
        }
    }

    // Simplified SpinBox component
    component ThemedSpinBox: SpinBox {
        background: Rectangle {
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 4
        }
        contentItem: TextInput {
            text: parent.textFromValue(parent.value, parent.locale)
            font: parent.font
            color: textColor
            selectionColor: highlightColor
            selectedTextColor: backgroundColor
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            readOnly: !parent.editable
            validator: parent.validator
            inputMethodHints: parent.inputMethodHints
        }
        up.indicator: Rectangle {
            x: parent.mirrored ? 0 : parent.width - width
            height: parent.height / 2
            color: parent.up.pressed ? Qt.darker(buttonColor, 1.2) : 
                   parent.up.hovered ? Qt.lighter(buttonColor, 1.1) : buttonColor
            border.color: borderColor
            border.width: 1
            radius: 4
            Text {
                text: "+"
                font.pointSize: 10
                color: buttonTextColor
                anchors.centerIn: parent
            }
        }
        down.indicator: Rectangle {
            x: parent.mirrored ? 0 : parent.width - width
            y: parent.height / 2
            height: parent.height / 2
            color: parent.down.pressed ? Qt.darker(buttonColor, 1.2) : 
                   parent.down.hovered ? Qt.lighter(buttonColor, 1.1) : buttonColor
            border.color: borderColor
            border.width: 1
            radius: 4
            Text {
                text: "-"
                font.pointSize: 10
                color: buttonTextColor
                anchors.centerIn: parent
            }
        }
    }

    // Simplified TextArea component
    component ThemedTextArea: TextArea {
        color: textColor
        selectionColor: highlightColor
        selectedTextColor: backgroundColor
        background: Rectangle {
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 4
        }
    }

    // Simplified Label component
    component ThemedLabel: Label {
        color: textColor
    }

    // Simplified TextField component
    component ThemedTextField: TextField {
        color: textColor
        selectionColor: highlightColor
        selectedTextColor: backgroundColor
        background: Rectangle {
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 4
        }
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
                            border.color: borderColor
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
                                        case "fog":
                                            MaterialEditorQML.setFogColor(selectedColor)
                                            break
                                        case "textureBorder":
                                            MaterialEditorQML.setTextureBorderColor(selectedColor)
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
                        color: (colorPickerPopup.currentColor.r + colorPickerPopup.currentColor.g + colorPickerPopup.currentColor.b) > 1.5 ? "black" : "white"
                        font.pointSize: 10
                    }
                }
                
                // Buttons
                RowLayout {
                    Layout.fillWidth: true
                    
                    ThemedButton {
                        text: "Cancel"
                        onClicked: colorPickerPopup.close()
                    }
                    
                    Item { Layout.fillWidth: true }
                    
                    ThemedButton {
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
                                case "fog":
                                    MaterialEditorQML.setFogColor(whiteColor)
                                    break
                                case "textureBorder":
                                    MaterialEditorQML.setTextureBorderColor(whiteColor)
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

    // Main content
    Rectangle {
        anchors.fill: parent
        color: backgroundColor

        SplitView {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 10

            // Left Panel - Text Editor
            Rectangle {
                SplitView.minimumWidth: 400
                SplitView.preferredWidth: 600
                SplitView.fillWidth: true
                color: panelColor
                border.color: borderColor
                border.width: 1
                radius: 4

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 15
                    spacing: 15

                    // Header with actions
                    RowLayout {
                        Layout.fillWidth: true

                        Text {
                            text: "Material Script Editor"
                            font.pointSize: 16
                            font.bold: true
                            color: textColor
                        }

                        Item { Layout.fillWidth: true }

                        ThemedButton {
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

                        ThemedButton {
                            text: "Apply"
                            enabled: materialTextArea.text !== MaterialEditorQML.materialText
                            onClicked: {
                                if (materialTextArea.text) {
                                    MaterialEditorQML.setMaterialText(materialTextArea.text)
                                    if (MaterialEditorQML.applyMaterial()) {
                                        statusText.text = "Applied successfully"
                                        statusText.color = "green"
                                    } else {
                                        statusText.text = "Apply failed"
                                        statusText.color = "red"
                                    }
                                }
                            }
                        }
                    }

                    // Status
                    RowLayout {
                        Layout.fillWidth: true
                        
                        Text {
                            text: "Material: " + (MaterialEditorQML.materialName || "None")
                            color: textColor
                            font.pointSize: 10
                        }
                        
                        Item { Layout.fillWidth: true }
                        
                        Text {
                            id: statusText
                            text: "Ready"
                            color: textColor
                            font.pointSize: 10
                        }
                    }

                    // AI Prompt Input Section
                    Rectangle {
                        Layout.fillWidth: true
                        height: 80
                        color: panelColor
                        border.color: borderColor
                        border.width: 1
                        radius: 4

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    text: "🤖 AI Assistant"
                                    font.pointSize: 12
                                    font.bold: true
                                    color: textColor
                                }

                                Item { Layout.fillWidth: true }

                                Rectangle {
                                    width: 12
                                    height: 12
                                    radius: 6
                                    color: aiStatusIndicator.isGenerating ? "orange" : 
                                           aiStatusIndicator.hasError ? "red" : "green"
                                    
                                    SequentialAnimation on opacity {
                                        running: aiStatusIndicator.isGenerating
                                        loops: Animation.Infinite
                                        NumberAnimation { to: 0.3; duration: 500 }
                                        NumberAnimation { to: 1.0; duration: 500 }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                ThemedTextField {
                                    id: aiPromptInput
                                    Layout.fillWidth: true
                                    placeholderText: "💡 Type a command like: 'add texture glow.png', 'make it transparent green', 'convert to PBR'"
                                    enabled: !aiStatusIndicator.isGenerating
                                    
                                    background: Rectangle {
                                        color: panelColor
                                        border.color: borderColor
                                        border.width: 1
                                        radius: 4
                                    }

                                    onAccepted: {
                                        if (text.trim() !== "") {
                                            generateButton.clicked()
                                        }
                                    }
                                }

                                ThemedButton {
                                    id: generateButton
                                    text: aiStatusIndicator.isGenerating ? "Generating..." : "Generate"
                                    enabled: !aiStatusIndicator.isGenerating && aiPromptInput.text.trim() !== ""
                                    
                                    onClicked: {
                                        if (aiPromptInput.text.trim() !== "") {
                                            MaterialEditorQML.generateMaterialFromPrompt(aiPromptInput.text.trim())
                                            aiPromptInput.text = ""
                                        }
                                    }
                                }
                            }
                        }
                    }

                            // Text editor
                            ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true

                                ThemedTextArea {
                                    id: materialTextArea
                                    text: MaterialEditorQML.materialText || "material default_material\n{\n\ttechnique\n\t{\n\t\tpass\n\t\t{\n\t\t}\n\t}\n}"
                                    selectByMouse: true
                                    font.family: "monospace"
                                    font.pointSize: 11
                                    wrapMode: TextArea.Wrap

                                    onTextChanged: {
                                        if (text !== MaterialEditorQML.materialText) {
                                            statusText.text = "Modified"
                                            statusText.color = "orange"
                                        }
                                    }
                                }
                            }
                }
            }

            // Right Panel - Properties Form
            Rectangle {
                SplitView.minimumWidth: 410
                SplitView.preferredWidth: 410
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

                                    ThemedComboBox {
                                        id: techniqueCombo
                                        Layout.fillWidth: true
                                        model: MaterialEditorQML.techniqueList
                                        currentIndex: MaterialEditorQML.selectedTechniqueIndex
                                        onCurrentIndexChanged: {
                                            MaterialEditorQML.setSelectedTechniqueIndex(currentIndex)
                                        }
                                    }

                                    ThemedButton {
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

                                    ThemedComboBox {
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

                                    ThemedButton {
                                        text: "New"
                                        onClicked: newPassDialog.open()
                                    }
                                }
                            }
                        }

                        // Pass Properties Panel
                        PassPropertiesPanel {
                            Layout.fillWidth: true
                            enabled: MaterialEditorQML.selectedPassIndex >= 0
                        }

                        // Texture Unit Management
                        GroupBox {
                            title: "Texture Units"
                            Layout.fillWidth: true
                            enabled: MaterialEditorQML.selectedPassIndex >= 0

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 10

                                RowLayout {
                                    Layout.fillWidth: true

                                    ThemedComboBox {
                                        Layout.fillWidth: true
                                        model: MaterialEditorQML.textureUnitList
                                        currentIndex: MaterialEditorQML.selectedTextureUnitIndex
                                        onCurrentIndexChanged: {
                                            if (currentIndex !== MaterialEditorQML.selectedTextureUnitIndex) {
                                                MaterialEditorQML.setSelectedTextureUnitIndex(currentIndex)
                                            }
                                        }
                                    }

                                    ThemedButton {
                                        text: "New"
                                        onClicked: newTextureUnitDialog.open()
                                    }
                                }
                            }
                        }

                        // Texture Properties Panel
                        TexturePropertiesPanel {
                            Layout.fillWidth: true
                            enabled: MaterialEditorQML.selectedTextureUnitIndex >= 0
                        }
                    }
                }
            }
        }
    }

    // Dialog components
    FileDialog {
        id: openMaterialDialog
        title: "Open Material File"
        nameFilters: ["Material files (*.material)", "All files (*)"]
        onAccepted: {
            console.log("Opening material file:", selectedFile)
            // Handle material file opening
        }
    }

    FileDialog {
        id: exportMaterialDialog
        title: "Export Material"
        fileMode: FileDialog.SaveFile
        nameFilters: ["Material files (*.material)", "All files (*)"]
        onAccepted: {
            MaterialEditorQML.exportMaterial(selectedFile.toString())
        }
    }

    Dialog {
        id: newMaterialDialog
        title: "New Material"
        modal: true
        anchors.centerIn: parent
        
        ColumnLayout {
            ThemedLabel { text: "Material Name:" }
            ThemedTextField {
                id: newMaterialNameField
                placeholderText: "Enter material name"
            }
        }
        
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: {
            MaterialEditorQML.createNewMaterial(newMaterialNameField.text)
            newMaterialNameField.text = ""
        }
    }

    Dialog {
        id: newTechniqueDialog
        title: "New Technique"
        modal: true
        anchors.centerIn: parent
        width: 350
        height: 200
        
        background: Rectangle {
            color: backgroundColor
            border.color: borderColor
            border.width: 2
            radius: 8
        }
        
        header: Rectangle {
            height: 40
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 8
            
            Text {
                text: "New Technique"
                font.pointSize: 12
                font.bold: true
                color: textColor
                anchors.centerIn: parent
            }
        }
        
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 15
            
            ThemedLabel { 
                text: "Technique Name:" 
                font.pointSize: 11
            }
            ThemedTextField {
                id: newTechniqueNameField
                Layout.fillWidth: true
                placeholderText: "Enter technique name"
            }
            
            Item { Layout.fillHeight: true }
            
            RowLayout {
                Layout.fillWidth: true
                
                Item { Layout.fillWidth: true }
                
                ThemedButton {
                    text: "Cancel"
                    onClicked: {
                        newTechniqueNameField.text = ""
                        newTechniqueDialog.close()
                    }
                }
                
                ThemedButton {
                    text: "OK"
                    enabled: newTechniqueNameField.text.trim() !== ""
                    onClicked: {
                        MaterialEditorQML.createNewTechnique(newTechniqueNameField.text)
                        newTechniqueNameField.text = ""
                        newTechniqueDialog.close()
                    }
                }
            }
        }
    }

    Dialog {
        id: newPassDialog
        title: "New Pass"
        modal: true
        anchors.centerIn: parent
        width: 350
        height: 200
        
        background: Rectangle {
            color: backgroundColor
            border.color: borderColor
            border.width: 2
            radius: 8
        }
        
        header: Rectangle {
            height: 40
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 8
            
            Text {
                text: "New Pass"
                font.pointSize: 12
                font.bold: true
                color: textColor
                anchors.centerIn: parent
            }
        }
        
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 15
            
            ThemedLabel { 
                text: "Pass Name:" 
                font.pointSize: 11
            }
            ThemedTextField {
                id: newPassNameField
                Layout.fillWidth: true
                placeholderText: "Enter pass name"
            }
            
            Item { Layout.fillHeight: true }
            
            RowLayout {
                Layout.fillWidth: true
                
                Item { Layout.fillWidth: true }
                
                ThemedButton {
                    text: "Cancel"
                    onClicked: {
                        newPassNameField.text = ""
                        newPassDialog.close()
                    }
                }
                
                ThemedButton {
                    text: "OK"
                    enabled: newPassNameField.text.trim() !== ""
                    onClicked: {
                        MaterialEditorQML.createNewPass(newPassNameField.text)
                        newPassNameField.text = ""
                        newPassDialog.close()
                    }
                }
            }
        }
    }

    Dialog {
        id: newTextureUnitDialog
        title: "New Texture Unit"
        modal: true
        anchors.centerIn: parent
        width: 350
        height: 200
        closePolicy: Popup.NoAutoClose

        background: Rectangle {
            color: backgroundColor
            border.color: borderColor
            border.width: 2
            radius: 8
        }
        
        header: Rectangle {
            height: 40
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 8
            
            Text {
                text: "New Texture Unit"
                font.pointSize: 12
                font.bold: true
                color: textColor
                anchors.centerIn: parent
            }
        }
        
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 15
            
            ThemedLabel { 
                text: "Texture Unit Name:" 
                font.pointSize: 11
            }
            ThemedTextField {
                id: newTextureUnitNameField
                Layout.fillWidth: true
                placeholderText: "Enter texture unit name"
            }
            
            Item { Layout.fillHeight: true }
            
            RowLayout {
                Layout.fillWidth: true
                
                Item { Layout.fillWidth: true }
                
                ThemedButton {
                    text: "Cancel"
                    onClicked: {
                        newTextureUnitNameField.text = ""
                        newTextureUnitDialog.close()
                    }
                }
                
                ThemedButton {
                    text: "OK"
                    enabled: newTextureUnitNameField.text.trim() !== ""
                    onClicked: {
                        MaterialEditorQML.createNewTextureUnit(newTextureUnitNameField.text)
                        newTextureUnitNameField.text = ""
                        newTextureUnitDialog.close()
                    }
                }
            }
        }
    }
} 
