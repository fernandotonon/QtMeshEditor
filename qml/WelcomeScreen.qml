import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import WelcomeScreen 1.0
import PropertiesPanel 1.0

Rectangle {
    id: root
    color: "#c0000000"  // semi-transparent dark overlay
    visible: WelcomeScreenController.visible

    // Click on the overlay background dismisses the welcome screen
    MouseArea {
        anchors.fill: parent
        onClicked: WelcomeScreenController.dismiss(dontShowCheckbox.checked)
    }

    // Centered card
    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(Math.max(parent.width - 60, 200), 560)
        height: Math.min(cardLayout.implicitHeight + 48, Math.max(parent.height - 40, 200))
        radius: 12
        color: PropertiesPanelController.panelColor
        border.color: PropertiesPanelController.borderColor
        border.width: 1

        // Prevent clicks on the card from dismissing
        MouseArea {
            anchors.fill: parent
            onClicked: {} // absorb click
        }

        Flickable {
            anchors {
                fill: parent; topMargin: 24; bottomMargin: 24
                leftMargin: 28; rightMargin: 28
            }
            contentHeight: cardLayout.implicitHeight
            clip: true
            flickableDirection: Flickable.VerticalFlick
            boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: cardLayout
            width: parent.width
            spacing: 16

            // ---- Header: Title ----
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                Layout.alignment: Qt.AlignHCenter

                Text {
                    text: "QtMeshEditor"
                    color: PropertiesPanelController.textColor
                    font.pixelSize: 22
                    font.bold: true
                    Layout.alignment: Qt.AlignHCenter
                }

                Text {
                    text: WelcomeScreenController.tagline
                    color: PropertiesPanelController.highlightColor
                    font.pixelSize: 13
                    font.bold: true
                    Layout.alignment: Qt.AlignHCenter
                }

                Text {
                    text: "Open-source 3D creation and asset pipeline"
                    color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                    font.pixelSize: 12
                    Layout.alignment: Qt.AlignHCenter
                }
            }

            // ---- QtMesh ID (optional account; one compact row) ----
            Rectangle {
                id: accountBar
                objectName: "qtmeshIdBar"
                readonly property string state_: WelcomeScreenController.accountState
                Layout.fillWidth: true
                implicitHeight: accountRow.implicitHeight + 16
                radius: 8
                color: "transparent"
                border.color: PropertiesPanelController.borderColor
                border.width: 1

                RowLayout {
                    id: accountRow
                    anchors { fill: parent; leftMargin: 10; rightMargin: 10; topMargin: 8; bottomMargin: 8 }
                    spacing: 10

                    // Avatar (initials) when signed in, else a neutral ID badge.
                    Rectangle {
                        width: 28; height: 28; radius: 14
                        color: accountBar.state_ === "signedIn"
                            ? PropertiesPanelController.highlightColor
                            : PropertiesPanelController.inputColor
                        border.color: PropertiesPanelController.borderColor
                        border.width: accountBar.state_ === "signedIn" ? 0 : 1
                        Text {
                            anchors.centerIn: parent
                            text: accountBar.state_ === "signedIn"
                                ? (WelcomeScreenController.accountInitials || "?") : "ID"
                            color: accountBar.state_ === "signedIn"
                                ? "#ffffff" : PropertiesPanelController.textColor
                            font.pixelSize: 11
                            font.bold: true
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Text {
                            text: "QtMesh ID"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                            font.pixelSize: 11
                            text: {
                                switch (accountBar.state_) {
                                case "signedIn":
                                    return "✓ Connected as " + WelcomeScreenController.accountName
                                case "connecting":
                                    return "Connecting… finish signing in in your browser."
                                case "expired":
                                    return "Your session expired. Sign in again to reconnect QtMesh Cloud."
                                default:
                                    return "Your account for the QtMesh ecosystem: QtMesh Cloud projects "
                                         + "and progress sync. Optional — the editor works without it."
                                }
                            }
                        }
                    }

                    // Actions: compact text links, secondary to New Scene / Open File.
                    Repeater {
                        model: {
                            switch (accountBar.state_) {
                            case "signedIn":
                                return [{ label: "Manage Account", act: "manage" },
                                        { label: "Sign out", act: "signout" }]
                            case "connecting":
                                return []
                            case "expired":
                                return [{ label: "Sign in with QtMesh ID", act: "signin" }]
                            default:
                                return [{ label: "Sign in with QtMesh ID", act: "signin" },
                                        { label: "Create a free account", act: "create" }]
                            }
                        }
                        delegate: Text {
                            required property var modelData
                            text: modelData.label
                            color: PropertiesPanelController.highlightColor
                            font.pixelSize: 12
                            font.bold: modelData.act === "signin"
                            font.underline: linkMA.containsMouse || activeFocus
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData.label
                            function trigger() {
                                switch (modelData.act) {
                                case "signin": WelcomeScreenController.signInWithQtMeshId(); break
                                case "create": WelcomeScreenController.createQtMeshIdAccount(); break
                                case "manage": WelcomeScreenController.manageQtMeshIdAccount(); break
                                case "signout": WelcomeScreenController.signOutOfQtMeshId(); break
                                }
                            }
                            Keys.onReturnPressed: trigger()
                            Keys.onEnterPressed: trigger()
                            Keys.onSpacePressed: trigger()
                            MouseArea {
                                id: linkMA
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: parent.trigger()
                            }
                        }
                    }
                }
            }

            // ---- Separator ----
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: PropertiesPanelController.borderColor
            }

            // ---- Quick Start Buttons ----
            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                // New Scene button
                Rectangle {
                    Layout.fillWidth: true
                    height: 40
                    radius: 6
                    color: newSceneMA.containsMouse
                        ? Qt.lighter(PropertiesPanelController.highlightColor, 1.1)
                        : PropertiesPanelController.highlightColor
                    Text {
                        anchors.centerIn: parent
                        text: "New Scene"
                        color: "#ffffff"
                        font.pixelSize: 13
                        font.bold: true
                    }
                    MouseArea {
                        id: newSceneMA
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: WelcomeScreenController.newScene()
                    }
                }

                // Open File button
                Rectangle {
                    Layout.fillWidth: true
                    height: 40
                    radius: 6
                    color: openFileMA.containsMouse
                        ? Qt.lighter(PropertiesPanelController.inputColor, 1.3)
                        : PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: "Open File..."
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 13
                        font.bold: true
                    }
                    MouseArea {
                        id: openFileMA
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: WelcomeScreenController.openFileDialog()
                    }
                }
            }

            // ---- Recent Files ----
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                visible: WelcomeScreenController.recentFiles.length > 0

                Text {
                    text: "Recent Files"
                    color: Qt.darker(PropertiesPanelController.textColor, 1.3)
                    font.pixelSize: 11
                    font.bold: true
                    font.capitalization: Font.AllUppercase
                }

                Repeater {
                    model: WelcomeScreenController.recentFiles

                    Rectangle {
                        Layout.fillWidth: true
                        height: 30
                        radius: 4
                        color: recentMA.containsMouse
                            ? Qt.lighter(PropertiesPanelController.headerColor, 1.2)
                            : "transparent"

                        RowLayout {
                            anchors {
                                fill: parent
                                leftMargin: 8
                                rightMargin: 8
                            }
                            spacing: 6

                            Text {
                                text: WelcomeScreenController.recentFileNames[index]
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 12
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Text {
                                text: modelData
                                color: Qt.darker(PropertiesPanelController.textColor, 1.6)
                                font.pixelSize: 10
                                elide: Text.ElideLeft
                                Layout.maximumWidth: 200
                            }
                        }

                        MouseArea {
                            id: recentMA
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: WelcomeScreenController.openFile(modelData)
                        }
                    }
                }
            }

            // ---- Separator ----
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: PropertiesPanelController.borderColor
            }

            // ---- Feature Tips ----
            Text {
                text: "Quick Tips"
                color: Qt.darker(PropertiesPanelController.textColor, 1.3)
                font.pixelSize: 11
                font.bold: true
                font.capitalization: Font.AllUppercase
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 3
                columnSpacing: 10
                rowSpacing: 8

                // Tip 1: AI Chat
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: tipCol1.implicitHeight + 16
                    radius: 6
                    color: PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1

                    ColumnLayout {
                        id: tipCol1
                        anchors {
                            left: parent.left; right: parent.right
                            top: parent.top
                            margins: 8
                        }
                        spacing: 2

                        Text {
                            text: "\u2728 AI Chat"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Text {
                            text: "Describe changes in natural language. Open from the toolbar or AI menu."
                            color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }

                // Tip 2: Keyboard Shortcuts
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: tipCol2.implicitHeight + 16
                    radius: 6
                    color: PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1

                    ColumnLayout {
                        id: tipCol2
                        anchors {
                            left: parent.left; right: parent.right
                            top: parent.top
                            margins: 8
                        }
                        spacing: 2

                        Text {
                            text: "\u2328 Shortcuts"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Text {
                            text: "Q Select, W Move, E Rotate, R Scale, F Frame, X Toggle space."
                            color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }

                // Tip 3: CLI Pipeline
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: tipCol3.implicitHeight + 16
                    radius: 6
                    color: PropertiesPanelController.headerColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1

                    ColumnLayout {
                        id: tipCol3
                        anchors {
                            left: parent.left; right: parent.right
                            top: parent.top
                            margins: 8
                        }
                        spacing: 2

                        Text {
                            text: "> CLI Pipeline"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Text {
                            text: "Use 'qtmesh' for batch ops: convert, validate, LOD, animations."
                            color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            // ---- Gamification: "try this next" discovery nudge (E-P5) ----
            Rectangle {
                id: suggestionCard
                property var suggestion: GamificationManager.suggestion
                visible: suggestion !== undefined && suggestion.featureKey !== undefined
                Layout.fillWidth: true
                implicitHeight: suggestionCol.implicitHeight + 20
                radius: 6
                color: PropertiesPanelController.headerColor
                border.color: PropertiesPanelController.highlightColor
                border.width: 1

                ColumnLayout {
                    id: suggestionCol
                    anchors {
                        left: parent.left; right: parent.right
                        top: parent.top
                        margins: 10
                    }
                    spacing: 3

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Text {
                            text: (suggestionCard.suggestion && suggestionCard.suggestion.personalized
                                   ? "✨ New to you: " : "✨ Try this: ")
                                  + (suggestionCard.suggestion ? suggestionCard.suggestion.title : "")
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 12
                            font.bold: true
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        // Rotate to another suggestion
                        Text {
                            text: "↻"
                            color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                            font.pixelSize: 13
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -4
                                cursorShape: Qt.PointingHandCursor
                                onClicked: GamificationManager.advanceSuggestion()
                            }
                        }

                        // Dismiss this suggestion permanently
                        Text {
                            text: "✕"
                            color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                            font.pixelSize: 12
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -4
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (suggestionCard.suggestion)
                                        GamificationManager.dismissSuggestion(suggestionCard.suggestion.featureKey)
                                }
                            }
                        }
                    }

                    Text {
                        text: suggestionCard.suggestion ? suggestionCard.suggestion.blurb : ""
                        color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                        font.pixelSize: 10
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }

            // ---- Made with QtMeshEditor (same catalog as WelcomeDialog) ----
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: PropertiesPanelController.borderColor
            }

            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: "Made with QtMeshEditor"
                    color: Qt.darker(PropertiesPanelController.textColor, 1.3)
                    font.pixelSize: 11
                    font.bold: true
                    font.capitalization: Font.AllUppercase
                    Layout.fillWidth: true
                }
                Text {
                    id: exploreLink
                    text: "Explore QtMesh Games \u2192"
                    color: PropertiesPanelController.highlightColor
                    font.pixelSize: 12
                    font.bold: true
                    font.underline: exploreMA.containsMouse || activeFocus
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Link
                    Accessible.name: text
                    Keys.onReturnPressed: WelcomeScreenController.openEcosystemLink("games")
                    Keys.onSpacePressed: WelcomeScreenController.openEcosystemLink("games")
                    MouseArea {
                        id: exploreMA
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: WelcomeScreenController.openEcosystemLink("games")
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 4
                columnSpacing: 8

                Repeater {
                    // Four cards keep the section compact; About shows them all.
                    model: WelcomeScreenController.showcaseGames.filter(
                               g => ["scrapfall", "ironfang", "mall-chase", "data-core-clash"].indexOf(g.id) >= 0)

                    Rectangle {
                        id: gameCard
                        Layout.fillWidth: true
                        implicitHeight: cardCol.implicitHeight + 12
                        radius: 6
                        color: PropertiesPanelController.headerColor
                        border.color: (gameMA.containsMouse || activeFocus)
                                      ? PropertiesPanelController.highlightColor
                                      : PropertiesPanelController.borderColor
                        border.width: activeFocus ? 2 : 1
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Link
                        Accessible.name: modelData.title + ", " + modelData.platform + " game"
                        Keys.onReturnPressed: WelcomeScreenController.openEcosystemLink(modelData.id)
                        Keys.onSpacePressed: WelcomeScreenController.openEcosystemLink(modelData.id)

                        ColumnLayout {
                            id: cardCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 6 }
                            spacing: 4
                            Image {
                                source: modelData.thumbnail
                                Layout.fillWidth: true
                                Layout.preferredHeight: width * 9 / 16
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                smooth: true
                                mipmap: true
                            }
                            Text {
                                text: modelData.title
                                color: PropertiesPanelController.textColor
                                font.pixelSize: 11
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Text {
                                text: modelData.platform
                                color: Qt.darker(PropertiesPanelController.textColor, 1.5)
                                font.pixelSize: 10
                            }
                        }
                        MouseArea {
                            id: gameMA
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: WelcomeScreenController.openEcosystemLink(modelData.id)
                        }
                    }
                }
            }

            Text {
                text: "Platforms: " + WelcomeScreenController.platformsSummary
                color: Qt.darker(PropertiesPanelController.textColor, 1.4)
                font.pixelSize: 10
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // ---- Separator ----
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: PropertiesPanelController.borderColor
            }

            // ---- Don't show again + Dismiss ----
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                CheckBox {
                    id: dontShowCheckbox
                    text: "Don't show again"
                    checked: false

                    contentItem: Text {
                        text: dontShowCheckbox.text
                        color: Qt.darker(PropertiesPanelController.textColor, 1.3)
                        font.pixelSize: 11
                        leftPadding: dontShowCheckbox.indicator.width + 6
                        verticalAlignment: Text.AlignVCenter
                    }

                    indicator: Rectangle {
                        implicitWidth: 16
                        implicitHeight: 16
                        x: dontShowCheckbox.leftPadding
                        y: (dontShowCheckbox.height - height) / 2
                        radius: 3
                        color: PropertiesPanelController.inputColor
                        border.color: PropertiesPanelController.borderColor
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "\u2713"
                            color: PropertiesPanelController.textColor
                            font.pixelSize: 12
                            visible: dontShowCheckbox.checked
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    width: 90
                    height: 28
                    radius: 4
                    color: dismissMA.containsMouse
                        ? Qt.lighter(PropertiesPanelController.inputColor, 1.3)
                        : PropertiesPanelController.inputColor
                    border.color: PropertiesPanelController.borderColor
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "Get Started"
                        color: PropertiesPanelController.textColor
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: dismissMA
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: WelcomeScreenController.dismiss(dontShowCheckbox.checked)
                    }
                }
            }

            // Bottom spacer for padding
            Item { height: 4 }
        }
        } // Flickable
    }

    // Dismiss on Escape key
    Keys.onEscapePressed: WelcomeScreenController.dismiss(dontShowCheckbox.checked)

    // Grab focus when visible so Escape works
    onVisibleChanged: {
        if (visible) forceActiveFocus()
    }
}
