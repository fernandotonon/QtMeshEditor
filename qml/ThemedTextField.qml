import QtQuick 2.15
import QtQuick.Controls 2.15

TextField {
    color: MaterialEditorQML.textColor
    selectionColor: Qt.rgba(0.4, 0.4, 0.6, 1.0)
    selectedTextColor: MaterialEditorQML.textColor
    placeholderTextColor: Qt.rgba(0.6, 0.6, 0.6, 1.0)
    
    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 30
        color: MaterialEditorQML.panelColor
        border.color: parent.activeFocus ? Qt.lighter(MaterialEditorQML.borderColor, 1.5) : MaterialEditorQML.borderColor
        border.width: parent.activeFocus ? 2 : 1
        radius: 3
    }
} 