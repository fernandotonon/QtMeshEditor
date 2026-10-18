pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import AnimationControl 1.0
import PropertiesPanel 1.0

// ── MetaHuman body correctives (epic #1095, Phase 2) ─────────────────────────
// An imported MetaHuman BODY .dna carries RBF solvers that blend corrective
// joints in as the limbs bend (shoulder, elbow, hip, knee). This switch shows
// the corrected deformation against ordinary skinning, so the difference is
// visible on the live character. Styled with the same Inspector primitives and
// theme colours as the other Animation-mode panels.
Column {
    id: dnaSection
    width: parent ? parent.width : 300
    spacing: 6

    component InspectorCheckBox: CheckBox {
        id: icb
        spacing: 6
        indicator: Rectangle {
            x: icb.leftPadding
            y: icb.height / 2 - height / 2
            implicitWidth: 16; implicitHeight: 16; radius: 2
            color: icb.checked ? AnimationControlController.highlightColor : AnimationControlController.inputColor
            border.color: icb.activeFocus ? AnimationControlController.highlightColor : AnimationControlController.borderColor
            opacity: icb.enabled ? 1.0 : 0.45
            Text {
                anchors.centerIn: parent; visible: icb.checked; text: "✓"
                color: AnimationControlController.textColor; font.pixelSize: 12; font.bold: true
            }
        }
        contentItem: Text {
            visible: icb.text !== ""
            text: icb.text
            color: AnimationControlController.textColor
            font.pixelSize: 11
            leftPadding: icb.indicator.width + icb.spacing
            verticalAlignment: Text.AlignVCenter
            opacity: icb.enabled ? 1.0 : 0.45
        }
    }

    component Note: Text {
        width: dnaSection.width
        wrapMode: Text.WordWrap
        color: AnimationControlController.textColor
        opacity: 0.7
        font.pixelSize: 11
    }

    Note {
        text: DnaCorrectiveManager.characterCount > 0
              ? "Corrective joints driven by the DNA's RBF solvers, evaluated every frame on top of the animation."
              : "Import a MetaHuman body .dna (File ▸ Open) to use body correctives."
    }

    InspectorCheckBox {
        enabled: DnaCorrectiveManager.characterCount > 0
        text: "Body correctives"
        checked: DnaCorrectiveManager.enabled
        onToggled: DnaCorrectiveManager.enabled = checked
    }

    Note {
        visible: DnaCorrectiveManager.characterCount > 0
        text: DnaCorrectiveManager.enabled ? DnaCorrectiveManager.summary : "Plain skinning (correctives off)"
    }
}
