import QtQuick
import QtQuick.Controls

// Slice I: compact Inspector-style text field.
TextField {
    implicitHeight: 24
    leftPadding: 6
    rightPadding: 6
    topPadding: 2
    bottomPadding: 2
    font.pixelSize: 11

    color: MaterialEditorQML.textColor
    selectionColor: Qt.rgba(0.4, 0.4, 0.6, 1.0)
    selectedTextColor: MaterialEditorQML.textColor
    placeholderTextColor: MaterialEditorQML.disabledTextColor

    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 24
        color: MaterialEditorQML.inputColor
        border.color: parent.activeFocus
            ? MaterialEditorQML.highlightColor
            : MaterialEditorQML.borderColor
        border.width: parent.activeFocus ? 2 : 1
        radius: 3
    }
}
