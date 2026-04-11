import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import PropertiesPanel 1.0

Rectangle {
    id: root
    color: backgroundColor

    SystemPalette {
        id: palette
        colorGroup: SystemPalette.Active
    }

    property color backgroundColor: palette.window
    property color panelColor: palette.base
    property color textColor: palette.windowText
    property color borderColor: palette.mid
    property color highlightColor: palette.highlight
    property color buttonColor: palette.button
    property color buttonTextColor: palette.buttonText
    property color dimTextColor: Qt.darker(textColor, 1.4)
    property color keyBgColor: Qt.lighter(panelColor, 1.15)

    property string searchText: ""
    property var shortcutData: PropertiesPanelController.shortcutData()

    function matchesSearch(entry) {
        if (searchText.length === 0) return true;
        var lower = searchText.toLowerCase();
        return entry.key.toLowerCase().indexOf(lower) >= 0
            || entry.description.toLowerCase().indexOf(lower) >= 0;
    }

    function categoryHasMatches(category) {
        for (var i = 0; i < shortcutData.length; i++) {
            if (shortcutData[i].category === category && matchesSearch(shortcutData[i]))
                return true;
        }
        return false;
    }

    function uniqueCategories() {
        var seen = {};
        var cats = [];
        for (var i = 0; i < shortcutData.length; i++) {
            var c = shortcutData[i].category;
            if (!seen[c]) {
                seen[c] = true;
                cats.push(c);
            }
        }
        return cats;
    }

    function entriesForCategory(category) {
        var result = [];
        for (var i = 0; i < shortcutData.length; i++) {
            if (shortcutData[i].category === category && matchesSearch(shortcutData[i]))
                result.push(shortcutData[i]);
        }
        return result;
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 46
            color: panelColor

            Text {
                text: "Keyboard Shortcuts"
                font.pointSize: 14
                font.bold: true
                color: textColor
                anchors.centerIn: parent
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: borderColor
            }
        }

        // Search bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 8
            Layout.bottomMargin: 4
            color: panelColor
            border.color: searchField.activeFocus ? highlightColor : borderColor
            border.width: 1
            radius: 4

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 6

                Text {
                    text: "\u2315"
                    font.pixelSize: 16
                    color: dimTextColor
                    Layout.alignment: Qt.AlignVCenter
                }

                TextInput {
                    id: searchField
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    font.pixelSize: 13
                    color: textColor
                    clip: true
                    selectByMouse: true
                    onTextChanged: root.searchText = text

                    Text {
                        anchors.fill: parent
                        text: "Search shortcuts..."
                        color: dimTextColor
                        font.pixelSize: 13
                        visible: !searchField.text && !searchField.activeFocus
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                Text {
                    text: "\u2715"
                    font.pixelSize: 12
                    color: clearMouse.containsMouse ? textColor : dimTextColor
                    visible: searchField.text.length > 0
                    Layout.alignment: Qt.AlignVCenter

                    MouseArea {
                        id: clearMouse
                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            searchField.text = "";
                            searchField.forceActiveFocus();
                        }
                    }
                }
            }
        }

        // Shortcut list
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Flickable {
                contentWidth: parent.width
                contentHeight: categoriesColumn.implicitHeight

                Column {
                    id: categoriesColumn
                    width: parent.width
                    spacing: 4

                    Repeater {
                        model: uniqueCategories()

                        Column {
                            width: categoriesColumn.width
                            visible: categoryHasMatches(modelData)
                            spacing: 0

                            // Category header
                            Rectangle {
                                width: parent.width
                                height: 30
                                color: Qt.lighter(backgroundColor, 1.05)

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 14
                                    anchors.rightMargin: 14
                                    spacing: 6

                                    Text {
                                        text: modelData
                                        font.pixelSize: 12
                                        font.bold: true
                                        font.capitalization: Font.AllUppercase
                                        color: highlightColor
                                        Layout.fillWidth: true
                                    }
                                }
                            }

                            // Entries for this category
                            Repeater {
                                model: entriesForCategory(modelData)

                                Rectangle {
                                    width: categoriesColumn.width
                                    height: 32
                                    color: entryMouse.containsMouse ? Qt.lighter(backgroundColor, 1.08) : "transparent"

                                    MouseArea {
                                        id: entryMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 20
                                        anchors.rightMargin: 14
                                        spacing: 12

                                        Text {
                                            text: modelData.description
                                            font.pixelSize: 12
                                            color: textColor
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                        }

                                        // Key badge(s)
                                        Row {
                                            spacing: 4
                                            Layout.alignment: Qt.AlignRight

                                            Repeater {
                                                model: modelData.key.split("+")

                                                Rectangle {
                                                    width: Math.max(keyLabel.implicitWidth + 12, 28)
                                                    height: 22
                                                    radius: 3
                                                    color: keyBgColor
                                                    border.color: borderColor
                                                    border.width: 1

                                                    Text {
                                                        id: keyLabel
                                                        anchors.centerIn: parent
                                                        text: modelData.trim()
                                                        font.pixelSize: 11
                                                        font.family: "monospace"
                                                        color: textColor
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // Subtle separator line
                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.leftMargin: 20
                                        anchors.rightMargin: 14
                                        height: 1
                                        color: Qt.lighter(borderColor, 1.3)
                                        opacity: 0.4
                                    }
                                }
                            }
                        }
                    }

                    // No results message
                    Text {
                        visible: {
                            var cats = uniqueCategories();
                            for (var i = 0; i < cats.length; i++) {
                                if (categoryHasMatches(cats[i])) return false;
                            }
                            return true;
                        }
                        text: "No shortcuts match your search."
                        font.pixelSize: 13
                        font.italic: true
                        color: dimTextColor
                        anchors.horizontalCenter: parent.horizontalCenter
                        topPadding: 30
                    }
                }
            }
        }

        // Footer
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            color: panelColor

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: borderColor
            }

            Text {
                anchors.centerIn: parent
                text: "Press Ctrl+/ to toggle  |  Esc to close"
                font.pixelSize: 11
                color: dimTextColor
            }
        }
    }

    Component.onCompleted: searchField.forceActiveFocus()
}
