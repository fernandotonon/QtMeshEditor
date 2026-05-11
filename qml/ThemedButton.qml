import QtQuick
import QtQuick.Controls

// Slice I: re-skinned to match the Inspector's flat compact look.
// Uses MaterialEditorQML palette colors that are bound to the same
// system palette as PropertiesPanelController.
Button {
    implicitHeight: 24
    leftPadding: 10
    rightPadding: 10
    topPadding: 2
    bottomPadding: 2
    font.pixelSize: 11

    background: Rectangle {
        radius: 3
        color: !parent.enabled
            ? Qt.darker(MaterialEditorQML.buttonColor, 1.4)
            : parent.pressed
                ? Qt.darker(MaterialEditorQML.highlightColor, 1.1)
                : parent.hovered
                    ? MaterialEditorQML.highlightColor
                    : MaterialEditorQML.buttonColor
        border.color: MaterialEditorQML.borderColor
        border.width: 1
    }

    contentItem: Text {
        text: parent.text
        font: parent.font
        color: parent.enabled
            ? MaterialEditorQML.buttonTextColor
            : Qt.darker(MaterialEditorQML.buttonTextColor, 2.0)
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
