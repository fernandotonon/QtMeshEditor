import QtQuick
import QtQuick.Window
import PropertiesPanel 1.0
import ThemeManager 1.0

// Detached UV editor — full toolbar + canvas (Material Mode → UV Edit).
Window {
    id: uvEditorWindow
    title: UVEditorController.hasMesh
           ? ("UV Editor — " + UVEditorController.statusText)
           : "UV Editor"
    width: 920
    height: 760
    minimumWidth: 640
    minimumHeight: 520
    color: ThemeManager.panelColor
    flags: Qt.Window | Qt.WindowMinMaxButtonsHint | Qt.WindowCloseButtonHint

    function applyInitialView() {
        if (panelLoader.item && panelLoader.item.fitToViewWhenReady)
            panelLoader.item.fitToViewWhenReady()
    }

    Loader {
        id: panelLoader
        anchors.fill: parent
        source: "qrc:/UVEditor/UVEditorPanel.qml"
        onLoaded: {
            if (item) {
                item.embedded = false
                item.focus = true
            }
            UVEditorController.refresh()
            Qt.callLater(uvEditorWindow.applyInitialView)
        }
    }

    onVisibilityChanged: {
        if (visibility === Window.Windowed || visibility === Window.Maximized
                || visibility === Window.FullScreen)
            Qt.callLater(applyInitialView)
    }

    Connections {
        target: UVEditorController
        function onFitToViewRequested() {
            if (panelLoader.item && panelLoader.item.fitToViewWhenReady)
                panelLoader.item.fitToViewWhenReady()
        }
        function onMeshDataChanged() {
            if (uvEditorWindow.visibility !== Window.Hidden)
                Qt.callLater(uvEditorWindow.applyInitialView)
        }
    }
}
