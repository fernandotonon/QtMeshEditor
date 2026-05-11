import QtQuick
import QtQuick.Controls

// Slice I: Inspector-style checkbox. Uses a flat 16x16 box with a
// checkmark glyph instead of QtQuickControls' default round-edged
// indicator. Same idiom as the InspectorCheckBox primitives in the
// Texture Channel Packer / Normal Map dialogs.
CheckBox {
    id: control
    implicitHeight: 22
    spacing: 6
    font.pixelSize: 11

    indicator: Rectangle {
        x: control.leftPadding
        y: parent.height / 2 - height / 2
        implicitWidth: 16
        implicitHeight: 16
        radius: 2
        color: control.checked
            ? MaterialEditorQML.highlightColor
            : MaterialEditorQML.inputColor
        border.color: MaterialEditorQML.borderColor
        border.width: 1
        opacity: control.enabled ? 1.0 : 0.45
        Text {
            anchors.centerIn: parent
            visible: control.checked
            text: "✓"
            color: MaterialEditorQML.textColor
            font.pixelSize: 12
            font.bold: true
        }
    }

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.enabled
            ? MaterialEditorQML.textColor
            : MaterialEditorQML.disabledTextColor
        verticalAlignment: Text.AlignVCenter
        leftPadding: control.indicator.width + control.spacing
    }
}
