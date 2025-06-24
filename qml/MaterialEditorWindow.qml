import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Dialogs 1.3
import MaterialEditorQML 1.0

ApplicationWindow {
    id: materialEditorWindow
    title: "Material Editor - " + MaterialEditorQML.materialName
    width: 1200
    height: 800
    minimumWidth: 800
    minimumHeight: 600

    property bool isModified: false

    Component.onCompleted: {
        MaterialEditorQML.errorOccurred.connect(showError)
        MaterialEditorQML.materialApplied.connect(onMaterialApplied)
    }

    function showError(errorMessage) {
        errorDialog.text = errorMessage
        errorDialog.open()
    }

    function onMaterialApplied() {
        isModified = false
        statusBar.showMessage("Material applied successfully", 3000)
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            
            ToolButton {
                text: "New"
                icon.source: "qrc:/icons/new.png"
                onClicked: newMaterialDialog.open()
            }
            
            ToolButton {
                text: "Load"
                icon.source: "qrc:/icons/open.png"
                onClicked: loadMaterialDialog.open()
            }
            
            ToolSeparator {}
            
            ToolButton {
                text: "Apply"
                icon.source: "qrc:/icons/apply.png"
                enabled: MaterialEditorQML.materialText.length > 0
                onClicked: MaterialEditorQML.applyMaterial()
            }
            
            ToolButton {
                text: "Validate"
                icon.source: "qrc:/icons/validate.png"
                onClicked: {
                    if (MaterialEditorQML.validateMaterialScript(MaterialEditorQML.materialText)) {
                        statusBar.showMessage("Material script is valid", 2000)
                    }
                }
            }
            
            Item { Layout.fillWidth: true }
            
            Label {
                text: isModified ? "Modified" : ""
                color: "orange"
                font.bold: true
            }
        }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal
        
        // Left panel - Material Script
        Rectangle {
            SplitView.preferredWidth: 400
            SplitView.minimumWidth: 300
            color: "#f5f5f5"
            border.color: "#ddd"
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 10
                
                Label {
                    text: "Material Script"
                    font.bold: true
                    font.pixelSize: 14
                }
                
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    
                    TextArea {
                        id: materialTextArea
                        text: MaterialEditorQML.materialText
                        font.family: "Consolas, Monaco, monospace"
                        font.pixelSize: 12
                        selectByMouse: true
                        wrapMode: TextArea.Wrap
                        
                        background: Rectangle {
                            color: "white"
                            border.color: "#ccc"
                            border.width: 1
                        }
                        
                        onTextChanged: {
                            if (text !== MaterialEditorQML.materialText) {
                                MaterialEditorQML.materialText = text
                                isModified = true
                            }
                        }
                    }
                }
            }
        }
        
        // Right panel - Properties
        Rectangle {
            SplitView.fillWidth: true
            color: "#f9f9f9"
            border.color: "#ddd"
            
            ScrollView {
                anchors.fill: parent
                anchors.margins: 10
                
                ColumnLayout {
                    width: parent.width
                    spacing: 15
                    
                    // Material Info
                    GroupBox {
                        title: "Material Information"
                        Layout.fillWidth: true
                        
                        ColumnLayout {
                            anchors.fill: parent
                            
                            RowLayout {
                                Label { text: "Name:" }
                                TextField {
                                    Layout.fillWidth: true
                                    text: MaterialEditorQML.materialName
                                    onTextChanged: MaterialEditorQML.materialName = text
                                }
                            }
                        }
                    }
                    
                    // Technique Selection
                    GroupBox {
                        title: "Techniques"
                        Layout.fillWidth: true
                        
                        ColumnLayout {
                            anchors.fill: parent
                            
                            RowLayout {
                                ComboBox {
                                    id: techniqueCombo
                                    Layout.fillWidth: true
                                    model: MaterialEditorQML.techniqueList
                                    currentIndex: MaterialEditorQML.selectedTechniqueIndex
                                    onCurrentIndexChanged: {
                                        if (currentIndex !== MaterialEditorQML.selectedTechniqueIndex) {
                                            MaterialEditorQML.selectedTechniqueIndex = currentIndex
                                        }
                                    }
                                }
                                
                                Button {
                                    text: "New"
                                    onClicked: newTechniqueDialog.open()
                                }
                            }
                        }
                    }
                    
                    // Pass Selection  
                    GroupBox {
                        title: "Passes"
                        Layout.fillWidth: true
                        
                        ColumnLayout {
                            anchors.fill: parent
                            
                            RowLayout {
                                ComboBox {
                                    id: passCombo
                                    Layout.fillWidth: true
                                    model: MaterialEditorQML.passList
                                    currentIndex: MaterialEditorQML.selectedPassIndex
                                    onCurrentIndexChanged: {
                                        if (currentIndex !== MaterialEditorQML.selectedPassIndex) {
                                            MaterialEditorQML.selectedPassIndex = currentIndex
                                        }
                                    }
                                }
                                
                                Button {
                                    text: "New"
                                    enabled: MaterialEditorQML.selectedTechniqueIndex >= 0
                                    onClicked: newPassDialog.open()
                                }
                            }
                        }
                    }
                    
                    // Pass Properties
                    PassPropertiesPanel {
                        Layout.fillWidth: true
                        visible: MaterialEditorQML.selectedPassIndex >= 0
                    }
                    
                    // Texture Unit Selection
                    GroupBox {
                        title: "Texture Units"
                        Layout.fillWidth: true
                        visible: MaterialEditorQML.selectedPassIndex >= 0
                        
                        ColumnLayout {
                            anchors.fill: parent
                            
                            RowLayout {
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: MaterialEditorQML.textureUnitList
                                    currentIndex: MaterialEditorQML.selectedTextureUnitIndex
                                    onCurrentIndexChanged: {
                                        if (currentIndex !== MaterialEditorQML.selectedTextureUnitIndex) {
                                            MaterialEditorQML.selectedTextureUnitIndex = currentIndex
                                        }
                                    }
                                }
                                
                                Button {
                                    text: "New"
                                    enabled: MaterialEditorQML.selectedPassIndex >= 0
                                    onClicked: newTextureUnitDialog.open()
                                }
                            }
                        }
                    }
                    
                    // Texture Properties
                    TexturePropertiesPanel {
                        Layout.fillWidth: true
                        visible: MaterialEditorQML.selectedTextureUnitIndex >= 0
                    }
                    
                    Item { Layout.fillHeight: true }
                }
            }
        }
    }
    
    footer: StatusBar {
        id: statusBar
        
        property alias text: statusLabel.text
        
        function showMessage(message, timeout = 0) {
            statusLabel.text = message
            if (timeout > 0) {
                statusTimer.interval = timeout
                statusTimer.start()
            }
        }
        
        Label {
            id: statusLabel
            text: "Ready"
        }
        
        Timer {
            id: statusTimer
            onTriggered: statusLabel.text = "Ready"
        }
    }
    
    // Dialogs
    Dialog {
        id: newMaterialDialog
        title: "New Material"
        modal: true
        anchors.centerIn: parent
        
        ColumnLayout {
            Label { text: "Material Name:" }
            TextField {
                id: newMaterialNameField
                Layout.preferredWidth: 200
                text: "new_material"
            }
        }
        
        standardButtons: Dialog.Ok | Dialog.Cancel
        
        onAccepted: {
            MaterialEditorQML.createNewMaterial(newMaterialNameField.text)
            isModified = true
        }
    }
    
    Dialog {
        id: loadMaterialDialog
        title: "Load Material"
        modal: true
        anchors.centerIn: parent
        
        ColumnLayout {
            Label { text: "Select Material:" }
            ComboBox {
                id: materialListCombo
                Layout.preferredWidth: 200
                // This would be populated with available materials
                model: ListModel {
                    // Placeholder - would be populated from MaterialManager
                }
            }
        }
        
        standardButtons: Dialog.Ok | Dialog.Cancel
        
        onAccepted: {
            if (materialListCombo.currentText.length > 0) {
                MaterialEditorQML.loadMaterial(materialListCombo.currentText)
                isModified = false
            }
        }
    }
    
    Dialog {
        id: newTechniqueDialog
        title: "New Technique"
        modal: true
        anchors.centerIn: parent
        
        ColumnLayout {
            Label { text: "Technique Name:" }
            TextField {
                id: newTechniqueNameField
                Layout.preferredWidth: 200
                text: "technique_" + (MaterialEditorQML.techniqueList.length + 1)
            }
        }
        
        standardButtons: Dialog.Ok | Dialog.Cancel
        
        onAccepted: {
            MaterialEditorQML.createNewTechnique(newTechniqueNameField.text)
            isModified = true
        }
    }
    
    Dialog {
        id: newPassDialog
        title: "New Pass"
        modal: true
        anchors.centerIn: parent
        
        ColumnLayout {
            Label { text: "Pass Name:" }
            TextField {
                id: newPassNameField
                Layout.preferredWidth: 200
                text: "pass_" + (MaterialEditorQML.passList.length + 1)
            }
        }
        
        standardButtons: Dialog.Ok | Dialog.Cancel
        
        onAccepted: {
            MaterialEditorQML.createNewPass(newPassNameField.text)
            isModified = true
        }
    }
    
    Dialog {
        id: newTextureUnitDialog
        title: "New Texture Unit"
        modal: true
        anchors.centerIn: parent
        
        ColumnLayout {
            Label { text: "Texture Unit Name:" }
            TextField {
                id: newTextureUnitNameField
                Layout.preferredWidth: 200
                text: "texture_unit_" + (MaterialEditorQML.textureUnitList.length + 1)
            }
        }
        
        standardButtons: Dialog.Ok | Dialog.Cancel
        
        onAccepted: {
            MaterialEditorQML.createNewTextureUnit(newTextureUnitNameField.text)
            isModified = true
        }
    }
    
    MessageDialog {
        id: errorDialog
        title: "Error"
        icon: StandardIcon.Critical
    }
} 