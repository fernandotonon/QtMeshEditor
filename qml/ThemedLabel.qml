import QtQuick
import QtQuick.Controls

// Slice I: defaults match Inspector text — 11px, no bold.
Label {
    color: enabled ? MaterialEditorQML.textColor : MaterialEditorQML.disabledTextColor
    font.pixelSize: 11
}
