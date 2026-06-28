import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MaterialEditorQML 1.0

ApplicationWindow {
    id: materialListModal
    title: "Material List"
    width: 700
    height: 550
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

    property string selectedMaterial: ""

    signal editMaterial(string materialName)
    signal exportMaterial(string materialName)
    signal createNewMaterial()
    signal importMaterials()

    // Filtered material list
    property var allMaterials: []
    property var filteredMaterials: []
    property var materialPreviewUris: ({})
    property string searchText: ""

    function rebuildMaterialPreviews(names) {
        var uris = {}
        for (var i = 0; i < names.length; ++i) {
            var n = names[i]
            if (n && n.length > 0)
                uris[n] = MaterialEditorQML.materialPreview(n)
        }
        materialPreviewUris = uris
    }

    function refreshMaterialList() {
        allMaterials = MaterialEditorQML.getMaterialList()
        rebuildMaterialPreviews(allMaterials)
        applyFilter()
    }

    function applyFilter() {
        if (searchText === "") {
            filteredMaterials = allMaterials
        } else {
            var result = []
            var lower = searchText.toLowerCase()
            for (var i = 0; i < allMaterials.length; i++) {
                if (allMaterials[i].toLowerCase().indexOf(lower) !== -1)
                    result.push(allMaterials[i])
            }
            filteredMaterials = result
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // Search bar
        Rectangle {
            Layout.fillWidth: true
            height: 32
            color: panelColor
            border.color: borderColor
            border.width: 1
            radius: 4

            RowLayout {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4

                Text {
                    text: "\uD83D\uDD0D"
                    font.pixelSize: 14
                    verticalAlignment: Text.AlignVCenter
                }

                TextInput {
                    id: searchField
                    Layout.fillWidth: true
                    color: textColor
                    font.pixelSize: 12
                    clip: true
                    onTextChanged: {
                        materialListModal.searchText = text
                        materialListModal.applyFilter()
                    }

                    Text {
                        anchors.fill: parent
                        text: "Search materials..."
                        color: disabledTextColor
                        font.pixelSize: 12
                        visible: !searchField.text && !searchField.activeFocus
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                Text {
                    text: filteredMaterials.length + " materials"
                    color: disabledTextColor
                    font.pixelSize: 10
                }
            }
        }

        // Material grid
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            GridView {
                id: materialGrid
                clip: true
                cellWidth: Math.max(90, (width - 20) / Math.floor((width - 20) / 90))
                cellHeight: 100

                model: filteredMaterials

                delegate: Item {
                    width: materialGrid.cellWidth
                    height: materialGrid.cellHeight

                    Rectangle {
                        id: card
                        anchors.centerIn: parent
                        width: parent.width - 6
                        height: parent.height - 6
                        radius: 6
                        color: selectedMaterial === modelData ? Qt.lighter(highlightColor, 1.6)
                             : cardMa.containsMouse ? Qt.lighter(panelColor, 1.3)
                             : panelColor
                        border.color: selectedMaterial === modelData ? highlightColor : borderColor
                        border.width: selectedMaterial === modelData ? 2 : 1

                        Column {
                            anchors.centerIn: parent
                            spacing: 4

                            // RTT sphere preview
                            Image {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 52; height: 52
                                source: materialListModal.materialPreviewUris[modelData] || ""
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                sourceSize.width: 52
                                sourceSize.height: 52

                                // Fallback circle if preview fails
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 44; height: 44; radius: 22
                                    color: Qt.darker(highlightColor, 1.5)
                                    visible: parent.status !== Image.Ready
                                    Text {
                                        anchors.centerIn: parent
                                        text: "\uD83D\uDD35"
                                        font.pixelSize: 20
                                    }
                                }
                            }

                            // Material name
                            Text {
                                width: card.width - 8
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData
                                color: textColor
                                font.pixelSize: 9
                                elide: Text.ElideMiddle
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }

                        MouseArea {
                            id: cardMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: selectedMaterial = modelData
                            onDoubleClicked: {
                                selectedMaterial = modelData
                                editMaterial(modelData)
                            }
                        }
                    }
                }
            }
        }

        // Button row
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                text: "New"
                background: Rectangle {
                    color: parent.hovered ? Qt.lighter(buttonColor, 1.2) : buttonColor
                    border.color: borderColor; border.width: 1; radius: 3
                }
                contentItem: Text { text: parent.text; color: buttonTextColor; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                onClicked: newMaterialDialog.open()
            }

            Button {
                text: "Import"
                background: Rectangle {
                    color: parent.hovered ? Qt.lighter(buttonColor, 1.2) : buttonColor
                    border.color: borderColor; border.width: 1; radius: 3
                }
                contentItem: Text { text: parent.text; color: buttonTextColor; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                onClicked: importMaterials()
            }

            Button {
                text: "Edit"
                enabled: selectedMaterial !== ""
                background: Rectangle {
                    color: parent.enabled ? (parent.hovered ? Qt.lighter(highlightColor, 1.2) : highlightColor) : Qt.darker(buttonColor, 1.5)
                    border.color: borderColor; border.width: 1; radius: 3
                }
                contentItem: Text { text: parent.text; color: parent.enabled ? "white" : disabledTextColor; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                onClicked: {
                    if (selectedMaterial !== "") {
                        editMaterial(selectedMaterial)
                        materialListModal.close()
                    }
                }
            }

            Button {
                text: "Export"
                enabled: selectedMaterial !== ""
                background: Rectangle {
                    color: parent.enabled ? (parent.hovered ? Qt.lighter(buttonColor, 1.2) : buttonColor) : Qt.darker(buttonColor, 1.5)
                    border.color: borderColor; border.width: 1; radius: 3
                }
                contentItem: Text { text: parent.text; color: parent.enabled ? buttonTextColor : disabledTextColor; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                onClicked: {
                    if (selectedMaterial !== "") exportMaterial(selectedMaterial)
                }
            }

            Item { Layout.fillWidth: true }
        }
    }

    // New Material Dialog
    Dialog {
        id: newMaterialDialog
        title: "New Material"
        modal: true
        anchors.centerIn: parent

        ColumnLayout {
            Label { text: "Material Name:"; color: textColor }
            TextField {
                id: newMaterialNameField
                placeholderText: "Enter material name"
                background: Rectangle { color: panelColor; border.color: borderColor; border.width: 1; radius: 3 }
                color: textColor
                placeholderTextColor: disabledTextColor
            }
        }

        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: {
            if (newMaterialNameField.text.trim() !== "") {
                MaterialEditorQML.createNewMaterial(newMaterialNameField.text)
                openMaterialEditor(newMaterialNameField.text)
                newMaterialNameField.text = ""
                materialListModal.close()
            }
        }
        onRejected: newMaterialNameField.text = ""
    }

    function openImportDialog() {
        var selectedFile = MaterialEditorQML.openMaterialImportDialog()
        if (selectedFile !== "") {
            MaterialEditorQML.importMaterialFile(selectedFile)
            refreshMaterialList()
        }
    }

    function openExportDialog() {
        if (selectedMaterial !== "") {
            var selectedFile = MaterialEditorQML.openMaterialExportDialog(selectedMaterial)
            if (selectedFile !== "") {
                MaterialEditorQML.exportMaterial(selectedFile, selectedMaterial)
            }
        }
    }

    function openMaterialEditor(materialName) {
        MaterialEditorQML.openMaterialEditorWindow(materialName)
    }

    Component.onCompleted: {
        refreshMaterialList()
        selectedMaterial = ""
    }

    onImportMaterials: openImportDialog()
    onExportMaterial: openExportDialog()
    onCreateNewMaterial: newMaterialDialog.open()
    onEditMaterial: openMaterialEditor(materialName)
}
