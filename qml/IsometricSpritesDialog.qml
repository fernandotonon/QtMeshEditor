import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0

// Epic #724: in-app isometric / 8-direction sprite atlas export.
Window {
    id: dialog
    title: "Isometric Sprites"
    width: 560
    height: 580
    minimumWidth: 480
    minimumHeight: 520
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property string outputPath: ""
    property string animationName: ""
    property int directions: 8
    property int frames: 8
    property int resolution: 256
    property double elevation: 30
    property double padding: 1.25
    property double cameraDistance: 0
    property double startAzimuth: 0

    property string lastStatus: ""
    property bool lastWasError: false

    readonly property bool useAnimation: dialog.animationName.length > 0
    readonly property int labelColWidth: 100

    function open() {
        dialog.lastStatus = ""
        dialog.lastWasError = false
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
        keyCapture.forceActiveFocus()
    }

    function runExport() {
        if (IsometricSpritesController.isExporting) return
        if (!IsometricSpritesController.hasExportableSelection) return
        if (dialog.outputPath.length === 0) {
            dialog.lastStatus = "Choose an output PNG path first."
            dialog.lastWasError = true
            return
        }
        const r = IsometricSpritesController.exportSelected(
            dialog.outputPath,
            dialog.animationName,
            dialog.directions,
            dialog.frames,
            dialog.resolution,
            dialog.elevation,
            dialog.padding,
            dialog.cameraDistance,
            dialog.startAzimuth)
        if (r && r.ok) {
            dialog.lastStatus = "✓ " + r.outputPath
                + " (" + r.sheetWidth + "×" + r.sheetHeight + " px)"
            dialog.lastWasError = false
        } else {
            dialog.lastStatus = "✗ " + (r && r.error ? r.error : "export failed")
            dialog.lastWasError = true
        }
    }

    Item {
        id: keyCapture
        anchors.fill: parent
        focus: true
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape) {
                dialog.close()
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                dialog.runExport()
                event.accepted = true
            }
        }
    }

    Connections {
        target: IsometricSpritesController
        function onOutputPathPicked(path) {
            dialog.show()
            dialog.raise()
            dialog.requestActivate()
            if (path.length > 0)
                dialog.outputPath = path
        }
    }

    component InspectorButton: Rectangle {
        id: btn
        property string label: ""
        property bool buttonEnabled: true
        signal clicked()
        implicitWidth: btnLabel.implicitWidth + 16
        Layout.preferredWidth: Math.max(90, implicitWidth)
        activeFocusOnTab: buttonEnabled
        Keys.onSpacePressed: if (buttonEnabled) btn.clicked()
        Keys.onReturnPressed: if (buttonEnabled) btn.clicked()
        Keys.onEnterPressed: if (buttonEnabled) btn.clicked()
        height: 26
        radius: 3
        color: btnMa.containsMouse && buttonEnabled
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.headerColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1
        opacity: buttonEnabled ? 1.0 : 0.45
        Text {
            id: btnLabel
            anchors.centerIn: parent
            text: btn.label
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
        }
        MouseArea {
            id: btnMa
            anchors.fill: parent
            hoverEnabled: true
            enabled: btn.buttonEnabled
            cursorShape: btn.buttonEnabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
            onClicked: btn.clicked()
        }
    }

    component InspectorLabel: Text {
        color: PropertiesPanelController.textColor
        font.pixelSize: 11
    }

    component InspectorNumberField: Rectangle {
        id: nf
        property double value: 0
        property double minValue: 0
        property double maxValue: 1e9
        property bool isInt: false
        signal newValue(double v)
        implicitWidth: 80
        height: 24
        color: PropertiesPanelController.inputColor
        border.color: ni.activeFocus
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.borderColor
        border.width: 1
        radius: 3
        TextInput {
            id: ni
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            text: nf.isInt ? Math.round(nf.value).toString() : nf.value.toFixed(2)
            color: PropertiesPanelController.textColor
            font.pixelSize: 11
            verticalAlignment: TextInput.AlignVCenter
            selectByMouse: true
            enabled: !IsometricSpritesController.isExporting
            onEditingFinished: {
                const n = nf.isInt ? parseInt(text, 10) : parseFloat(text)
                if (isNaN(n)) {
                    text = nf.isInt ? Math.round(nf.value).toString() : nf.value.toFixed(2)
                    return
                }
                nf.newValue(Math.max(nf.minValue, Math.min(nf.maxValue, n)))
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        InspectorLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.85
            text: "Export the selected mesh as an isometric sprite atlas: rows are compass "
                + "directions, columns are animation frames. Row 0 is the front view (+Z)."
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            InspectorLabel { text: "Output:"; Layout.preferredWidth: dialog.labelColWidth }
            Rectangle {
                Layout.fillWidth: true
                height: 24
                color: PropertiesPanelController.inputColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                radius: 3
                Text {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    text: dialog.outputPath.length > 0
                        ? dialog.outputPath
                        : "(click Browse… to choose)"
                    color: PropertiesPanelController.textColor
                    opacity: dialog.outputPath.length > 0 ? 1.0 : 0.45
                    font.pixelSize: 11
                    elide: Text.ElideMiddle
                    verticalAlignment: Text.AlignVCenter
                }
            }
            InspectorButton {
                label: "Browse…"
                Layout.preferredWidth: 90
                buttonEnabled: !IsometricSpritesController.isExporting
                onClicked: {
                    dialog.hide()
                    IsometricSpritesController.requestOutputPathPick(dialog.outputPath)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            InspectorLabel { text: "Animation:"; Layout.preferredWidth: dialog.labelColWidth }
            ThemedComboBox {
                Layout.fillWidth: true
                height: 24
                font.pixelSize: 11
                model: ["(static mesh)"].concat(IsometricSpritesController.availableAnimations)
                currentIndex: dialog.animationName.length === 0
                    ? 0
                    : Math.max(0, model.indexOf(dialog.animationName))
                enabled: !IsometricSpritesController.isExporting
                onCurrentTextChanged: {
                    dialog.animationName = (currentText === "(static mesh)") ? "" : currentText
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            InspectorLabel { text: "Directions:"; Layout.preferredWidth: dialog.labelColWidth }
            InspectorNumberField {
                isInt: true
                value: dialog.directions
                minValue: 1
                maxValue: 64
                onNewValue: function(v) { dialog.directions = Math.round(v) }
            }
            InspectorLabel { text: "Frames:"; Layout.preferredWidth: 56 }
            InspectorNumberField {
                isInt: true
                value: dialog.useAnimation ? dialog.frames : 1
                minValue: 1
                maxValue: 360
                opacity: dialog.useAnimation ? 1.0 : 0.45
                enabled: dialog.useAnimation
                onNewValue: function(v) { dialog.frames = Math.round(v) }
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            InspectorLabel { text: "Cell px:"; Layout.preferredWidth: dialog.labelColWidth }
            InspectorNumberField {
                isInt: true
                value: dialog.resolution
                minValue: 16
                maxValue: 8192
                onNewValue: function(v) { dialog.resolution = Math.round(v) }
            }
            InspectorLabel { text: "Elevation°:"; Layout.preferredWidth: 56 }
            InspectorNumberField {
                value: dialog.elevation
                minValue: -80
                maxValue: 80
                onNewValue: function(v) { dialog.elevation = v }
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            InspectorLabel { text: "Padding:"; Layout.preferredWidth: dialog.labelColWidth }
            InspectorNumberField {
                value: dialog.padding
                minValue: 0.1
                maxValue: 10
                onNewValue: function(v) { dialog.padding = v }
            }
            InspectorLabel { text: "Cam dist:"; Layout.preferredWidth: 56 }
            InspectorNumberField {
                value: dialog.cameraDistance
                minValue: 0
                maxValue: 1e6
                onNewValue: function(v) { dialog.cameraDistance = v }
            }
            Item { Layout.fillWidth: true }
        }

        InspectorLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.65
            font.pixelSize: 10
            text: "Padding scales auto-fit framing. Camera distance 0 = auto-fit × padding."
        }

        Item { Layout.fillHeight: true }

        InspectorLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: dialog.lastStatus.length > 0
            color: dialog.lastWasError ? "#cc6666" : "#66aa66"
            text: dialog.lastStatus
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Item { Layout.fillWidth: true }
            InspectorButton {
                label: "Close"
                Layout.preferredWidth: 90
                onClicked: dialog.close()
            }
            InspectorButton {
                label: IsometricSpritesController.isExporting ? "Exporting…" : "Export PNG"
                Layout.preferredWidth: 110
                buttonEnabled: IsometricSpritesController.hasExportableSelection
                    && !IsometricSpritesController.isExporting
                onClicked: dialog.runExport()
            }
        }
    }
}
