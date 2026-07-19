import QtQuick
import QtQuick.Controls
import QtQuick.Window
import PropertiesPanel 1.0
import AnimationControl 1.0

// Floating inspector-styled context menu for bone hierarchy ops.
// Opened from the Skeleton Tools bone picker or a viewport bone right-click.
Window {
    id: menu
    flags: Qt.Popup | Qt.FramelessWindowHint | Qt.NoDropShadowWindowHint
    color: "transparent"
    width: menuFrame.implicitWidth
    height: menuFrame.implicitHeight

    property bool closeOnDeactivate: false

    signal setParentRequested()
    signal detachRequested()
    signal splitRequested()
    signal connectToggleRequested()
    signal attachRequested()
    signal duplicateRequested()
    signal renameRequested()
    signal removeRequested()

    Timer {
        id: armCloseTimer
        interval: 200
        onTriggered: menu.closeOnDeactivate = true
    }

    function openAt(globalX, globalY) {
        menu.closeOnDeactivate = false
        menu.x = globalX
        menu.y = globalY
        menu.show()
        menu.raise()
        menu.requestActivate()
        armCloseTimer.restart()
    }

    function closeMenu() {
        armCloseTimer.stop()
        menu.closeOnDeactivate = false
        menu.close()
    }

    readonly property bool hasBone: AnimationControlController.selectedBone.length > 0

    Rectangle {
        id: menuFrame
        implicitWidth: Math.max(180, menuCol.implicitWidth + 8)
        implicitHeight: menuCol.implicitHeight + 8
        color: PropertiesPanelController.inputColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1
        radius: 4

        Column {
            id: menuCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 4
            spacing: 1

            component MenuRow: Rectangle {
                id: row
                property string label: ""
                property bool enabled: true
                property bool danger: false
                signal activated()

                width: parent.width
                height: 24
                radius: 3
                opacity: row.enabled ? 1.0 : 0.4
                color: rowMa.containsMouse && row.enabled
                    ? PropertiesPanelController.highlightColor
                    : "transparent"

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.label
                    color: rowMa.containsMouse && row.enabled
                        ? "white"
                        : (row.danger ? "#e07070" : PropertiesPanelController.textColor)
                    font.pixelSize: 11
                }
                MouseArea {
                    id: rowMa
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: row.enabled
                    cursorShape: row.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: {
                        row.activated()
                        menu.closeMenu()
                    }
                }
            }

            MenuRow {
                label: "Set parent…"
                enabled: menu.hasBone
                onActivated: menu.setParentRequested()
            }
            MenuRow {
                label: "Detach"
                enabled: menu.hasBone
                onActivated: menu.detachRequested()
            }
            MenuRow {
                label: "Split…"
                enabled: menu.hasBone
                onActivated: menu.splitRequested()
            }
            MenuRow {
                label: SkeletonEditor.isSelectedBoneConnected() ? "Disconnect" : "Connect"
                enabled: menu.hasBone
                onActivated: menu.connectToggleRequested()
            }
            MenuRow {
                label: "Attach to entity…"
                enabled: menu.hasBone
                onActivated: menu.attachRequested()
            }

            Rectangle {
                width: parent.width
                height: 1
                color: PropertiesPanelController.borderColor
                opacity: 0.6
            }

            MenuRow {
                label: "Duplicate"
                enabled: menu.hasBone
                onActivated: menu.duplicateRequested()
            }
            MenuRow {
                label: "Rename…"
                enabled: menu.hasBone
                onActivated: menu.renameRequested()
            }
            MenuRow {
                label: "Remove…"
                enabled: menu.hasBone
                danger: true
                onActivated: menu.removeRequested()
            }
        }
    }

    // Click outside closes — armed after a short delay so the viewport
    // keeping focus on open doesn't dismiss the menu immediately.
    onActiveChanged: {
        if (!active && visible && menu.closeOnDeactivate)
            Qt.callLater(menu.closeMenu)
    }
}
