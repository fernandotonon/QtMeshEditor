pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import PropertiesPanel 1.0

// ── MetaHuman body correctives (epic #1095, Phase 2) ─────────────────────────
// An imported MetaHuman BODY .dna carries RBF solvers that blend corrective
// joints in as the limbs bend (shoulder, elbow, hip, knee). This switch shows
// the corrected deformation against ordinary skinning, so the difference is
// visible on the live character.
Column {
    id: dnaSection
    width: parent ? parent.width : 300
    spacing: 6

    Label {
        width: parent.width
        wrapMode: Text.WordWrap
        color: palette.windowText
        opacity: 0.75
        font.pixelSize: 11
        text: DnaCorrectiveManager.characterCount > 0
              ? "Corrective joints driven by the DNA's RBF solvers, evaluated every frame on top of the animation."
              : "Import a MetaHuman body .dna (File ▸ Open) to use body correctives."
    }

    CheckBox {
        id: toggle
        enabled: DnaCorrectiveManager.characterCount > 0
        text: "Body correctives"
        checked: DnaCorrectiveManager.enabled
        onToggled: DnaCorrectiveManager.enabled = checked
    }

    Label {
        width: parent.width
        wrapMode: Text.WordWrap
        visible: DnaCorrectiveManager.characterCount > 0
        color: palette.windowText
        opacity: 0.7
        font.pixelSize: 11
        text: DnaCorrectiveManager.enabled ? DnaCorrectiveManager.summary : "Plain skinning (correctives off)"
    }
}
