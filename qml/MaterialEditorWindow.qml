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

                                            ThemedLabel { 
                                                text: "Polygon Mode:" 
                                                Layout.alignment: Qt.AlignVCenter
                                            }
                                            ThemedComboBox {
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
                                        ThemedLabel { 
                                            text: "Ambient:" 
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
                                        ThemedLabel { 
                                            text: "Diffuse:" 
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
                                        ThemedLabel { 
                                            text: "Specular:" 
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
                                        ThemedLabel { 
                                            text: "Emissive:" 
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

                                        ThemedLabel { text: "Diffuse Alpha:" }
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
                                            ThemedSpinBox {
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

                                        ThemedLabel { text: "Specular Alpha:" }
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
                                            ThemedSpinBox {
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

                                        ThemedLabel { text: "Shininess:" }
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
                                            ThemedSpinBox {
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

                                        ThemedLabel { text: "Source Blend:" }
                                        ThemedComboBox {
                                            Layout.fillWidth: true
                                            model: MaterialEditorQML.getBlendFactorNames()
                                            currentIndex: MaterialEditorQML.sourceBlendFactor
                                            onCurrentIndexChanged: MaterialEditorQML.setSourceBlendFactor(currentIndex)
                                        }

                                        ThemedLabel { text: "Dest Blend:" }
                                        ThemedComboBox {
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

                                    ThemedComboBox {
                                        id: textureUnitCombo
                                        Layout.fillWidth: true
                                        model: MaterialEditorQML.textureUnitList
                                        currentIndex: MaterialEditorQML.selectedTextureUnitIndex
                                        onCurrentIndexChanged: {
                                            MaterialEditorQML.setSelectedTextureUnitIndex(currentIndex)
                                        }
                                    }

                                    ThemedButton {
                                        text: "New"
                                        onClicked: newTextureUnitDialog.open()
                                    }
                                }
                            }
                        }

                        // Texture Properties
                        GroupBox {
                            title: "Texture Properties"
                            Layout.fillWidth: true
                            enabled: MaterialEditorQML.selectedTextureUnitIndex >= 0

                            GridLayout {
                                anchors.fill: parent
                                columns: 2
                                rowSpacing: 10

                                ThemedLabel { text: "Texture:" }
                                RowLayout {
                                    Layout.fillWidth: true
                                    
                                    Text {
                                        id: textureNameField
                                        Layout.fillWidth: true
                                        text: MaterialEditorQML.textureName || "*No texture*"
                                        color: textColor
                                        elide: Text.ElideRight
                                    }
                                    
                                    ThemedButton {
                                        text: "Select"
                                        onClicked: textureFileDialog.open()
                                    }
                                    
                                    ThemedButton {
                                        text: "Remove"
                                        onClicked: MaterialEditorQML.removeTexture()
                                    }
                                }

                                ThemedLabel { text: "U Scroll Speed:" }
                                RowLayout {
                                    Slider {
                                        from: -10.0
                                        to: 10.0
                                        value: MaterialEditorQML.scrollAnimUSpeed
                                        onValueChanged: MaterialEditorQML.setScrollAnimUSpeed(value)
                                        Layout.fillWidth: true
                                    }
                                    ThemedSpinBox {
                                        from: -1000
                                        to: 1000
                                        value: Math.round(MaterialEditorQML.scrollAnimUSpeed * 100)
                                        onValueChanged: MaterialEditorQML.setScrollAnimUSpeed(value / 100.0)
                                        textFromValue: function(value) { return (value / 100.0).toFixed(2) }
                                        valueFromText: function(text) { return Math.round(parseFloat(text) * 100) }
                                    }
                                }

                                ThemedLabel { text: "V Scroll Speed:" }
                                RowLayout {
                                    Slider {
                                        from: -10.0
                                        to: 10.0
                                        value: MaterialEditorQML.scrollAnimVSpeed
                                        onValueChanged: MaterialEditorQML.setScrollAnimVSpeed(value)
                                        Layout.fillWidth: true
                                    }
                                    ThemedSpinBox {
                                        from: -1000
                                        to: 1000
                                        value: Math.round(MaterialEditorQML.scrollAnimVSpeed * 100)
                                        onValueChanged: MaterialEditorQML.setScrollAnimVSpeed(value / 100.0)
                                        textFromValue: function(value) { return (value / 100.0).toFixed(2) }
                                        valueFromText: function(text) { return Math.round(parseFloat(text) * 100) }
                                    }
                                }
                            }
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

    // File browser dialog for texture selection
    Dialog {
        id: textureFileDialog
        title: "Select Texture File"
        modal: true
        anchors.centerIn: parent
        width: 700
        height: 500
        
        background: Rectangle {
            color: backgroundColor
            border.color: borderColor
            border.width: 2
            radius: 8
        }
        
        header: Rectangle {
            height: 45
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 8
            
            Text {
                text: "Select Texture File"
                font.pointSize: 14
                font.bold: true
                color: textColor
                anchors.centerIn: parent
            }
        }
        
        property string currentPath: "/media/materials/textures"
        property var fileList: []
        
        function refreshFileList() {
            // This would ideally call a C++ function to list real files
            // For now, we'll simulate a file browser with some common texture files
            var simulatedFiles = [
                {name: "..", type: "dir", size: "", path: getParentPath(currentPath)},
                {name: "textures", type: "dir", size: "", path: currentPath + "/textures"},
                {name: "materials", type: "dir", size: "", path: currentPath + "/materials"},
                {name: "concrete01.jpg", type: "file", size: "2.4 MB", path: currentPath + "/concrete01.jpg"},
                {name: "metal_brushed.png", type: "file", size: "1.8 MB", path: currentPath + "/metal_brushed.png"},
                {name: "wood_oak.dds", type: "file", size: "4.2 MB", path: currentPath + "/wood_oak.dds"},
                {name: "brick_red.tga", type: "file", size: "3.1 MB", path: currentPath + "/brick_red.tga"},
                {name: "grass_summer.jpg", type: "file", size: "1.9 MB", path: currentPath + "/grass_summer.jpg"},
                {name: "stone_cobble.png", type: "file", size: "2.7 MB", path: currentPath + "/stone_cobble.png"},
                {name: "water_normal.dds", type: "file", size: "5.5 MB", path: currentPath + "/water_normal.dds"},
                {name: "sand_desert.jpg", type: "file", size: "2.2 MB", path: currentPath + "/sand_desert.jpg"},
                {name: "fabric_canvas.png", type: "file", size: "1.6 MB", path: currentPath + "/fabric_canvas.png"},
                {name: "plastic_white.jpg", type: "file", size: "0.8 MB", path: currentPath + "/plastic_white.jpg"},
                {name: "rubber_black.dds", type: "file", size: "3.8 MB", path: currentPath + "/rubber_black.dds"},
                {name: "glass_clear.png", type: "file", size: "1.2 MB", path: currentPath + "/glass_clear.png"}
            ]
            
            fileListModel.clear()
            for (var i = 0; i < simulatedFiles.length; i++) {
                fileListModel.append(simulatedFiles[i])
            }
        }
        
        function getParentPath(path) {
            var parts = path.split('/')
            if (parts.length > 1) {
                parts.pop()
                return parts.join('/')
            }
            return path
        }
        
        function getFileName(fullPath) {
            return fullPath.split('/').pop()
        }
        
        Component.onCompleted: refreshFileList()
        
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 15
            spacing: 15
            
            // Navigation bar
            RowLayout {
                Layout.fillWidth: true
                
                ThemedLabel {
                    text: "Path:"
                }
                
                ThemedTextField {
                    id: pathField
                    Layout.fillWidth: true
                    text: textureFileDialog.currentPath
                    onTextChanged: {
                        if (text !== textureFileDialog.currentPath) {
                            textureFileDialog.currentPath = text
                        }
                    }
                }
                
                ThemedButton {
                    text: "↑ Up"
                    onClicked: {
                        textureFileDialog.currentPath = textureFileDialog.getParentPath(textureFileDialog.currentPath)
                        pathField.text = textureFileDialog.currentPath
                        textureFileDialog.refreshFileList()
                    }
                }
                
                ThemedButton {
                    text: "🔄 Refresh"
                    onClicked: textureFileDialog.refreshFileList()
                }
            }
            
            // File type filter
            RowLayout {
                Layout.fillWidth: true
                
                ThemedLabel {
                    text: "Filter:"
                }
                
                ThemedComboBox {
                    id: filterCombo
                    model: [
                        "All Image Files (*.jpg *.png *.dds *.tga *.bmp)",
                        "JPEG Files (*.jpg *.jpeg)",
                        "PNG Files (*.png)",
                        "DDS Files (*.dds)",
                        "TGA Files (*.tga)",
                        "All Files (*.*)"
                    ]
                    currentIndex: 0
                }
            }
            
            // File list
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: backgroundColor
                border.color: borderColor
                border.width: 1
                radius: 4
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 5
                    spacing: 0
                    
                    // Header row
                    Rectangle {
                        Layout.fillWidth: true
                        height: 30
                        color: alternateColor
                        border.color: borderColor
                        border.width: 1
                        
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 5
                            spacing: 10
                            
                            Text {
                                text: "Name"
                                font.bold: true
                                color: textColor
                                Layout.preferredWidth: 300
                            }
                            
                            Text {
                                text: "Size"
                                font.bold: true
                                color: textColor
                                Layout.preferredWidth: 80
                            }
                            
                            Text {
                                text: "Type"
                                font.bold: true
                                color: textColor
                                Layout.fillWidth: true
                            }
                        }
                    }
                    
                    // File list view
                    ListView {
                        id: fileListView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        
                        model: ListModel {
                            id: fileListModel
                        }
                        
                        delegate: ItemDelegate {
                            width: fileListView.width
                            height: 35
                            
                            property bool isDirectory: type === "dir"
                            property bool isImageFile: name.match(/\.(jpg|jpeg|png|dds|tga|bmp)$/i)
                            
                            Rectangle {
                                anchors.fill: parent
                                color: parent.hovered ? highlightColor : 
                                       (index % 2 === 0 ? "transparent" : Qt.darker(backgroundColor, 1.05))
                                radius: 2
                                
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 5
                                    spacing: 10
                                    
                                    // Icon and name
                                    RowLayout {
                                        Layout.preferredWidth: 300
                                        spacing: 5
                                        
                                        Text {
                                            text: isDirectory ? "📁" : (isImageFile ? "🖼️" : "📄")
                                            font.pointSize: 12
                                        }
                                        
                                        Text {
                                            text: name
                                            color: textColor
                                            font.pointSize: 11
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                    }
                                    
                                    // Size
                                    Text {
                                        text: size
                                        color: disabledTextColor
                                        font.pointSize: 10
                                        Layout.preferredWidth: 80
                                    }
                                    
                                    // Type
                                    Text {
                                        text: isDirectory ? "Folder" : "Image File"
                                        color: disabledTextColor
                                        font.pointSize: 10
                                        Layout.fillWidth: true
                                    }
                                }
                            }
                            
                            onClicked: {
                                if (isDirectory) {
                                    // Navigate to directory
                                    if (name === "..") {
                                        textureFileDialog.currentPath = textureFileDialog.getParentPath(textureFileDialog.currentPath)
                                    } else {
                                        textureFileDialog.currentPath = path
                                    }
                                    pathField.text = textureFileDialog.currentPath
                                    textureFileDialog.refreshFileList()
                                } else {
                                    // Select file
                                    selectedFileField.text = name
                                }
                            }
                            
                            onDoubleClicked: {
                                if (!isDirectory) {
                                    // Double-click on file to select and close
                                    MaterialEditorQML.setTextureName(name)
                                    textureFileDialog.close()
                                }
                            }
                        }
                        
                        ScrollIndicator.vertical: ScrollIndicator {
                            active: true
                        }
                    }
                }
            }
            
            // Selected file
            RowLayout {
                Layout.fillWidth: true
                
                ThemedLabel {
                    text: "Selected file:"
                }
                
                ThemedTextField {
                    id: selectedFileField
                    Layout.fillWidth: true
                    placeholderText: "Select a file from the list above..."
                }
            }
            
            // Buttons
            RowLayout {
                Layout.fillWidth: true
                
                ThemedButton {
                    text: "Create New Folder"
                    onClicked: {
                        // This would create a new folder in a real implementation
                        console.log("Create new folder functionality would go here")
                    }
                }
                
                Item { Layout.fillWidth: true }
                
                ThemedButton {
                    text: "Cancel"
                    onClicked: {
                        selectedFileField.text = ""
                        textureFileDialog.close()
                    }
                }
                
                ThemedButton {
                    text: "Open"
                    enabled: selectedFileField.text.trim() !== ""
                    onClicked: {
                        if (selectedFileField.text.trim() !== "") {
                            MaterialEditorQML.setTextureName(selectedFileField.text.trim())
                            selectedFileField.text = ""
                            textureFileDialog.close()
                        }
                    }
                }
            }
        }
    }
} 
