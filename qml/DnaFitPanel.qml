pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import AnimationControl 1.0
import PropertiesPanel 1.0

// ── Fit a MetaHuman body to another character (epic #1095, Phase 4) ─────────
// Select the imported MetaHuman body and the character to fit it to. "Fit" runs the same solver as
// `qtmesh dna fit`, shows the report below as the preview, and adds the fitted body as a NEW node
// beside the original (Ctrl+Z removes it): nothing existing is modified. Controls are the same
// primitives the other Inspector panels use (ThemedComboBox, ToolBtn, 22 px rows, theme colours).
Column {
    id: fitSection
    width: parent ? parent.width : 300
    spacing: 6

    component Label: Text {
        color: AnimationControlController.textColor
        font.pixelSize: 11
        verticalAlignment: Text.AlignVCenter
    }

    component Note: Text {
        width: fitSection.width
        wrapMode: Text.WordWrap
        color: AnimationControlController.textColor
        opacity: 0.7
        font.pixelSize: 11
    }

    component ToolBtn: Rectangle {
        property string label: ""
        property bool enabled: true
        property bool accent: false
        property string tip: ""
        signal clicked()
        width: Math.max(28, lblT.implicitWidth + 12); height: 22; radius: 3
        color: accent ? AnimationControlController.highlightColor
             : maT.pressed ? Qt.darker(AnimationControlController.buttonColor, 1.3)
             : maT.containsMouse ? Qt.lighter(AnimationControlController.buttonColor, 1.15)
             : AnimationControlController.buttonColor
        border.color: AnimationControlController.borderColor; border.width: 1
        opacity: enabled ? 1.0 : 0.4
        Accessible.role: Accessible.Button
        Accessible.name: label
        ToolTip.visible: tip !== "" && maT.containsMouse
        ToolTip.text: tip
        ToolTip.delay: 600
        Text {
            id: lblT; anchors.centerIn: parent; text: parent.label
            color: AnimationControlController.buttonTextColor; font.pixelSize: 11
        }
        MouseArea {
            id: maT; anchors.fill: parent; hoverEnabled: true
            enabled: parent.enabled; onClicked: parent.clicked()
        }
    }

    Note { text: DnaFitController.hint }

    Row {
        spacing: 4
        Label { width: 52; height: 22; text: "Mode" }
        ThemedComboBox {
            id: modeBox
            width: fitSection.width - 60; height: 22; font.pixelSize: 11
            model: ["Auto", "Same topology", "Skeleton (joints)"]
            readonly property var ids: ["auto", "topology", "joints"]
        }
    }

    ToolBtn {
        label: "Fit body to selected character"
        accent: true
        enabled: DnaFitController.canFit
        tip: "Fits the MetaHuman body to the selected character and adds the result as a new node (Ctrl+Z removes it)."
        onClicked: DnaFitController.fitSelected(modeBox.ids[modeBox.currentIndex], true)
    }

    Note {
        visible: DnaFitController.status !== ""
        text: DnaFitController.status
        opacity: 1.0
    }
}
