import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MaterialEditorQML 1.0
import PropertiesPanel 1.0

// Issue #407: top-level Window for native auto-rigging. Same Inspector-styled
// idiom as SkinWeightsDialog / QuadRetopoDialog. Operates on the currently
// selected STATIC entity — the button disables on already-rigged or empty
// selections (AutoRigController.hasRiggableSelection).
Window {
    id: dialog
    title: "Auto-Rig"
    width: 560
    height: 420
    minimumWidth: 480
    minimumHeight: 380
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: PropertiesPanelController.panelColor

    property var    templates: ["humanoid", "biped", "quadruped", "generic"]
    property int    templateIndex: 0
    property var    upAxes: ["x", "y", "z"]
    property int    upAxisIndex: 1            // +Y default
    property bool   alsoSkin: true

    property string lastStatus: ""
    property bool   lastWasError: false

    function open() {
        dialog.lastStatus = ""
        dialog.lastWasError = false
        dialog.show()
        dialog.raise()
        dialog.requestActivate()
        keyCapture.forceActiveFocus()
    }

    function runRig() {
        if (AutoRigController.busy) return
        if (!AutoRigController.hasRiggableSelection) return
        const r = AutoRigController.autoRigSelected(
            dialog.templates[dialog.templateIndex],
            dialog.upAxes[dialog.upAxisIndex],
            dialog.alsoSkin)
        if (r && r.applied) {
            dialog.lastStatus =
                "Rigged: " + r.boneCount + " bones, "
                + r.verticesSampled + " verts sampled, "
                + r.jointsRecentered + " joints recentered"
                + (dialog.alsoSkin ? (r.skinned ? " (+ skinned)" : " (skin failed)") : "")
            dialog.lastWasError = false
        } else {
            dialog.lastStatus = "Failed: " + (r && r.error ? r.error : "unknown error")
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
                dialog.runRig()
                event.accepted = true
            }
        }
    }

    // ── Inline Inspector primitives (match SkinWeightsDialog) ───────────

    component InspectorButton: Rectangle {
        id: btn
        property string label: ""
        property bool buttonEnabled: true
        signal clicked()
        activeFocusOnTab: buttonEnabled
        Accessible.role: Accessible.Button
        Accessible.name: btn.label
        Keys.onSpacePressed: if (buttonEnabled) btn.clicked()
        Keys.onReturnPressed: if (buttonEnabled) btn.clicked()
        Keys.onEnterPressed: if (buttonEnabled) btn.clicked()
        height: 26
        radius: 3
        color: btnMa.containsMouse && buttonEnabled
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.headerColor
        border.color: btn.activeFocus
            ? PropertiesPanelController.highlightColor
            : PropertiesPanelController.borderColor
        border.width: btn.activeFocus ? 2 : 1
        opacity: buttonEnabled ? 1.0 : 0.45
        Text {
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

    component InspectorCheckbox: Rectangle {
        id: cb
        property string label: ""
        property bool checked: false
        signal toggled()
        activeFocusOnTab: true
        Accessible.role: Accessible.CheckBox
        Accessible.name: cb.label
        Accessible.checked: cb.checked
        Keys.onSpacePressed: cb.toggled()
        Keys.onReturnPressed: cb.toggled()
        Keys.onEnterPressed: cb.toggled()
        height: 16
        width: parent ? parent.width : 200
        color: "transparent"
        Row {
            spacing: 6
            Rectangle {
                width: 14; height: 14
                radius: 2
                color: PropertiesPanelController.inputColor
                border.color: cb.activeFocus
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.borderColor
                border.width: cb.activeFocus ? 2 : 1
                Text {
                    anchors.centerIn: parent
                    text: cb.checked ? "✓" : ""
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
            }
            InspectorLabel { text: cb.label }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: cb.toggled()
        }
    }

    // A minimal segmented picker (no ComboBox dependency, matches the
    // hand-rolled Inspector style).
    component InspectorSegments: Row {
        id: seg
        property var options: []
        property int index: 0
        signal picked(int i)
        spacing: 4
        Repeater {
            model: seg.options
            Rectangle {
                width: Math.max(60, segText.implicitWidth + 18)
                height: 24
                radius: 3
                color: index === seg.index
                    ? PropertiesPanelController.highlightColor
                    : PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.borderColor
                border.width: 1
                Text {
                    id: segText
                    anchors.centerIn: parent
                    text: modelData
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: seg.picked(index)
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        InspectorLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.85
            text: "Embed a skeleton template into the selected unrigged mesh. "
                + "Native heuristic (no external deps): maps a proportional joint "
                + "graph into the mesh bounds and recentres joints toward the "
                + "mesh's medial mass. Works best on roughly upright, manifold, "
                + "T/A-pose meshes with +Y up. Already-rigged meshes are not "
                + "eligible."
        }

        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Skeleton:"; Layout.preferredWidth: 80 }
            InspectorSegments {
                options: dialog.templates
                index: dialog.templateIndex
                onPicked: function(i) { dialog.templateIndex = i }
            }
        }

        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: "Up axis:"; Layout.preferredWidth: 80 }
            InspectorSegments {
                options: dialog.upAxes
                index: dialog.upAxisIndex
                onPicked: function(i) { dialog.upAxisIndex = i }
            }
            InspectorLabel {
                text: "(+Y is the in-app default after import)"
                opacity: 0.7
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            spacing: 8
            Layout.fillWidth: true
            InspectorLabel { text: ""; Layout.preferredWidth: 80 }
            InspectorCheckbox {
                Layout.fillWidth: true
                label: "Also compute skin weights (one-click rig + skin)"
                checked: dialog.alsoSkin
                onToggled: dialog.alsoSkin = !dialog.alsoSkin
            }
        }

        Item { Layout.fillHeight: true }

        InspectorLabel {
            Layout.fillWidth: true
            visible: dialog.lastStatus.length > 0
            text: dialog.lastStatus
            wrapMode: Text.WordWrap
            color: dialog.lastWasError ? "#cc4444" : "#3a8c3a"
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            InspectorButton {
                label: "Close"
                Layout.preferredWidth: 90
                onClicked: dialog.close()
            }
            InspectorButton {
                label: AutoRigController.busy ? "Rigging…" : "Auto-Rig"
                Layout.preferredWidth: 160
                buttonEnabled: !AutoRigController.busy
                    && AutoRigController.hasRiggableSelection
                onClicked: dialog.runRig()
            }
        }
    }

    Connections {
        target: AutoRigController
        function onError(msg) {
            dialog.lastStatus = "Failed: " + msg
            dialog.lastWasError = true
        }
    }
}
