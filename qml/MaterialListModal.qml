import QtQuick 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0
import MaterialEditorQML 1.0

ApplicationWindow {
    id: materialListModal
    title: "Material List"
    width: 600
    height: 500
    visible: true
    color: backgroundColor
    
    // Theme colors
    readonly property color backgroundColor: palette.window
    readonly property color panelColor: palette.base
    readonly property color textColor: palette.windowText
    readonly property color borderColor: palette.mid
    readonly property color highlightColor: palette.highlight
    readonly property color buttonColor: palette.button
    readonly property color buttonTextColor: palette.buttonText
    readonly property color alternateColor: palette.alternateBase
    readonly property color disabledTextColor: palette.placeholderText
    
    SystemPalette {
        id: palette
        colorGroup: SystemPalette.Active
    }
    
    // Material list model
    ListModel {
        id: materialListModel
    }
    
    // Selected material index
    property int selectedIndex: -1
    property string selectedMaterial: ""
    
    // Signals
    signal materialSelected(string materialName)
    signal editMaterial(string materialName)
    signal exportMaterial(string materialName)
    signal createNewMaterial()
    signal importMaterials()
    
    // Main content
    ColumnLayout {
        anchors.fill: parent
        spacing: 10
        
        // Material list
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: 300
            
            ListView {
                id: materialListView
                model: materialListModel
                clip: true
                
                delegate: Rectangle {
                    width: materialListView.width
                    height: 30
                    color: index === selectedIndex ? highlightColor : 
                           (index % 2 === 0 ? panelColor : alternateColor)
                    border.color: borderColor
                    border.width: 1
                    
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: model.name
                        color: textColor
                        font.pixelSize: 12
                    }
                    
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            selectedIndex = index
                            selectedMaterial = model.name
                            materialSelected(model.name)
                        }
                        onDoubleClicked: {
                            selectedIndex = index
                            selectedMaterial = model.name
                            editMaterial(model.name)
                        }
                    }
                }
            }
        }
        
        // Button row
        RowLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            spacing: 10
            
            Item { Layout.fillWidth: true }
            
            Button {
                text: "New"
                background: Rectangle {
                    color: parent.enabled ? (parent.hovered ? Qt.lighter(buttonColor, 1.2) : buttonColor) : Qt.darker(buttonColor, 1.5)
                    border.color: borderColor
                    border.width: 1
                    radius: 3
                }
                contentItem: Text {
                    text: parent.text
                    color: parent.enabled ? buttonTextColor : Qt.darker(buttonTextColor, 2.0)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: {
                    newMaterialDialog.open()
                }
            }
            
            Button {
                text: "Import"
                background: Rectangle {
                    color: parent.enabled ? (parent.hovered ? Qt.lighter(buttonColor, 1.2) : buttonColor) : Qt.darker(buttonColor, 1.5)
                    border.color: borderColor
                    border.width: 1
                    radius: 3
                }
                contentItem: Text {
                    text: parent.text
                    color: parent.enabled ? buttonTextColor : Qt.darker(buttonTextColor, 2.0)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: {
                    console.log("Import button clicked")
                    importMaterials()
                }
            }
            
            Button {
                text: "Edit"
                enabled: selectedIndex >= 0
                background: Rectangle {
                    color: parent.enabled ? (parent.hovered ? Qt.lighter(buttonColor, 1.2) : buttonColor) : Qt.darker(buttonColor, 1.5)
                    border.color: borderColor
                    border.width: 1
                    radius: 3
                }
                contentItem: Text {
                    text: parent.text
                    color: parent.enabled ? buttonTextColor : Qt.darker(buttonTextColor, 2.0)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: {
                    if (selectedIndex >= 0) {
                        editMaterial(selectedMaterial)
                        materialListModal.close()
                    }
                }
            }
            
            Button {
                text: "Export"
                enabled: selectedIndex >= 0
                background: Rectangle {
                    color: parent.enabled ? (parent.hovered ? Qt.lighter(buttonColor, 1.2) : buttonColor) : Qt.darker(buttonColor, 1.5)
                    border.color: borderColor
                    border.width: 1
                    radius: 3
                }
                contentItem: Text {
                    text: parent.text
                    color: parent.enabled ? buttonTextColor : Qt.darker(buttonTextColor, 2.0)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: {
                    console.log("Export button clicked, selectedMaterial:", selectedMaterial)
                    if (selectedIndex >= 0) {
                        exportMaterial(selectedMaterial)
                    }
                }
            }
            
            Item { Layout.fillWidth: true }
        }
    }
    
    // Note: Using C++ file dialogs instead of QML FileDialog for better compatibility
    
    // New Material Dialog
    Dialog {
        id: newMaterialDialog
        title: "New Material"
        modal: true
        anchors.centerIn: parent
        
        ColumnLayout {
            Label { 
                text: "Material Name:"
                color: textColor
            }
            TextField {
                id: newMaterialNameField
                placeholderText: "Enter material name"
                background: Rectangle {
                    color: panelColor
                    border.color: borderColor
                    border.width: 1
                    radius: 3
                }
                color: textColor
                placeholderTextColor: disabledTextColor
            }
        }
        
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: {
            if (newMaterialNameField.text.trim() !== "") {
                // Create the new material and open the editor
                MaterialEditorQML.createNewMaterial(newMaterialNameField.text)
                openMaterialEditor(newMaterialNameField.text)
                newMaterialNameField.text = ""
                materialListModal.close()
            } else {
                // Show error message for empty name
                console.log("Material name cannot be empty")
            }
        }
        onRejected: {
            // Clear the field when cancelled
            newMaterialNameField.text = ""
        }
    }
    
    // Functions
    function refreshMaterialList() {
        materialListModel.clear()
        var materials = MaterialEditorQML.getMaterialList()
        for (var i = 0; i < materials.length; i++) {
            materialListModel.append({"name": materials[i]})
        }
    }
    
    function openImportDialog() {
        console.log("Opening material import dialog...")
        var selectedFile = MaterialEditorQML.openMaterialImportDialog()
        if (selectedFile !== "") {
            console.log("Material file selected for import:", selectedFile)
            MaterialEditorQML.importMaterialFile(selectedFile)
            refreshMaterialList()
        } else {
            console.log("Import cancelled")
        }
    }
    
    function openExportDialog() {
        console.log("Opening material export dialog...")
        if (selectedMaterial !== "") {
            var selectedFile = MaterialEditorQML.openMaterialExportDialog(selectedMaterial)
            if (selectedFile !== "") {
                console.log("File selected for export:", selectedFile)
                MaterialEditorQML.exportMaterial(selectedFile, selectedMaterial)
            } else {
                console.log("Export cancelled")
            }
        } else {
            console.log("No material selected for export")
        }
    }
    
    function openMaterialEditor(materialName) {
        // Use the C++ function to open the material editor
        MaterialEditorQML.openMaterialEditorWindow(materialName);
    }
    
    // Initialize on component completion
    Component.onCompleted: {
        refreshMaterialList()
        selectedIndex = -1
        selectedMaterial = ""
    }
    
    // Connect signals
    onImportMaterials: openImportDialog()
    onExportMaterial: openExportDialog()
    onCreateNewMaterial: newMaterialDialog.open()
    onEditMaterial: openMaterialEditor(materialName)
    onMaterialSelected: {
        // Just update selection, no action needed
    }
}
