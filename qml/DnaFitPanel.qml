pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import AnimationControl 1.0
import PropertiesPanel 1.0

// ── Fit a MetaHuman body to another character (epic #1095, Phase 4) ─────────
// Select the imported MetaHuman body and the character to fit it to. "Fit" runs the same solver as
// `qtmesh dna fit`, shows the report below as the preview, and adds the fitted body as a NEW node
// beside the original (Ctrl+Z removes it): nothing existing is modified.
Column {
    id: fitSection
    width: parent ? parent.width : 300
    spacing: 6

    component Note: Text {
        width: fitSection.width
        wrapMode: Text.WordWrap
        color: AnimationControlController.textColor
        opacity: 0.75
        font.pixelSize: 11
    }

    Note { text: DnaFitController.hint }

    Row {
        spacing: 8
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Mode"; color: AnimationControlController.textColor; font.pixelSize: 11
        }
        ComboBox {
            id: modeBox
            width: 150
            model: ["Auto", "Same topology", "Skeleton (joints)"]
            readonly property var ids: ["auto", "topology", "joints"]
            font.pixelSize: 11
        }
    }

    Rectangle {
        width: fitSection.width
        height: 26
        radius: 3
        opacity: DnaFitController.canFit ? 1.0 : 0.45
        color: fitMouse.pressed ? AnimationControlController.highlightColor : AnimationControlController.inputColor
        border.color: AnimationControlController.borderColor
        Text {
            anchors.centerIn: parent
            text: "Fit body to selected character"
            color: AnimationControlController.textColor
            font.pixelSize: 11
        }
        MouseArea {
            id: fitMouse
            anchors.fill: parent
            enabled: DnaFitController.canFit
            onClicked: DnaFitController.fitSelected(modeBox.ids[modeBox.currentIndex], true)
        }
    }

    Note {
        visible: DnaFitController.status !== ""
        text: DnaFitController.status
        opacity: 1.0
    }
}
