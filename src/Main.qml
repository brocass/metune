import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1020
    height: 720
    minimumWidth: 720
    minimumHeight: 520
    visible: true
    title: qsTr("Metune")
    color: "#1a1b26"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: {
        var f = win.activeFocusItem
        if (f && f.hasOwnProperty("editing") && f.editing) return
        if (backend.selectedIndex >= 0) backend.process(backend.selectedIndex)
    } }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; onActivated: backend.scan() }
    Shortcut { sequence: "L"; context: Qt.ApplicationShortcut; onActivated: backend.lookupSelected() }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; onActivated: backend.processAll() }
    Shortcut { sequence: "Ctrl+Z"; context: Qt.ApplicationShortcut; onActivated: { } }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    text: qsTr("Metune")
                    font.pixelSize: 20
                    font.bold: true
                    color: theme.accent
                }
                TextField {
                    id: watchField
                    Layout.fillWidth: true
                    text: backend.watchDir
                    placeholderText: qsTr("Watch folder")
                    onAccepted: backend.setWatchDir(text)
                    onEditingFinished: backend.setWatchDir(text)
                }
                Button { text: qsTr("Choose…"); onClicked: backend.pickWatchDir() }
                Button { text: qsTr("Scan"); onClicked: backend.scan() }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                CheckBox { id: normalizeBox; text: qsTr("Normalize −14 LUFS"); checked: backend.normalize; onToggled: backend.setNormalize(checked) }
                CheckBox { id: cropBox; text: qsTr("Crop black bars"); checked: backend.cropBars; onToggled: backend.setCropBars(checked) }
                CheckBox { id: autoBox; text: qsTr("Auto-process new"); checked: backend.autoProcess; onToggled: backend.setAutoProcess(checked) }
                CheckBox { id: removeBox; text: qsTr("Remove original"); checked: backend.removeOriginal; onToggled: backend.setRemoveOriginal(checked) }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("Emby folder:") }
                TextField {
                    Layout.fillWidth: true
                    text: backend.destDir
                    placeholderText: qsTr("Destination folder")
                    onAccepted: backend.setDestDir(text)
                    onEditingFinished: backend.setDestDir(text)
                }
                Button { text: qsTr("Choose…"); onClicked: backend.pickDestDir() }
                Button { text: qsTr("Open"); onClicked: backend.openDestination() }
                Button { text: qsTr("Process all"); Material.accent: theme.accent; onClicked: backend.processAll(); enabled: !backend.busy }
                Button { text: qsTr("Stop"); onClicked: backend.stop(); enabled: backend.busy }
            }

            Label {
                Layout.fillWidth: true
                text: backend.statusText.length > 0 ? backend.statusText : qsTr("%n video(s) found.", "", backend.videos.length).replace("%n", backend.videos.length.toLocaleString(translator.language, 'f', 0))
                wrapMode: Text.Wrap
                font.pixelSize: 12
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12

                // File list
                Rectangle {
                    Layout.preferredWidth: 420
                    Layout.fillHeight: true
                    radius: Math.min(6, theme.radius)
                    color: "#24283b"
                    border.color: "#414868"

                    ListView {
                        id: fileList
                        anchors.fill: parent
                        anchors.margins: 6
                        model: backend.videos
                        clip: true
                        highlightRangeMode: ListView.StrictlyEnforceRange
                        preferredHighlightBegin: 0
                        preferredHighlightEnd: 0

                        delegate: Rectangle {
                            width: fileList.width
                            height: 58
                            radius: Math.min(4, theme.radius)
                            color: index === backend.selectedIndex ? "#2f3457" : "transparent"
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Label {
                                        Layout.fillWidth: true
                                        text: (modelData.artist.length > 0 ? modelData.artist + " - " : "") + modelData.title
                                        font.pixelSize: 13
                                        elide: Text.ElideRight
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.fileName + (modelData.year.length > 0 ? " · " + modelData.year + " (" + modelData.genre + ")" : "")
                                        font.pixelSize: 11
                                        color: "#a9b1d6"
                                        elide: Text.ElideRight
                                    }
                                    ProgressBar {
                                        Layout.fillWidth: true
                                        value: modelData.progress
                                        to: 100
                                        visible: modelData.progress > 0 && modelData.progress < 100
                                    }
                                }
                                Label {
                                    text: modelData.status
                                    font.pixelSize: 11
                                    color: modelData.status === "Done" ? "#9ece6a" : (modelData.status.startsWith("Failed") ? "#f7768e" : "#a9b1d6")
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: backend.select(index)
                                preventStealing: true
                            }
                        }
                    }
                }

                // Details panel
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 8

                    Label { text: qsTr("Tags"); font.bold: true }
                    TextField {
                        Layout.fillWidth: true
                        text: backend.selectedIndex >= 0 ? backend.videos[backend.selectedIndex].artist : ""
                        placeholderText: qsTr("Artist")
                        onEditingFinished: if (backend.selectedIndex >= 0) backend.updateField(backend.selectedIndex, "artist", text)
                    }
                    TextField {
                        Layout.fillWidth: true
                        text: backend.selectedIndex >= 0 ? backend.videos[backend.selectedIndex].title : ""
                        placeholderText: qsTr("Title")
                        onEditingFinished: if (backend.selectedIndex >= 0) backend.updateField(backend.selectedIndex, "title", text)
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            Layout.fillWidth: true
                            text: backend.selectedIndex >= 0 ? backend.videos[backend.selectedIndex].year : ""
                            placeholderText: qsTr("Year")
                            onEditingFinished: if (backend.selectedIndex >= 0) backend.updateField(backend.selectedIndex, "year", text)
                        }
                        TextField {
                            Layout.fillWidth: true
                            text: backend.selectedIndex >= 0 ? backend.videos[backend.selectedIndex].genre : ""
                            placeholderText: qsTr("Genre (decade)")
                            onEditingFinished: if (backend.selectedIndex >= 0) backend.updateField(backend.selectedIndex, "genre", text)
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button { text: qsTr("Look up year"); onClicked: backend.lookupSelected() }
                        Button { text: qsTr("Process (Space)"); onClicked: if (backend.selectedIndex >= 0) backend.process(backend.selectedIndex) }
                    }

                    Label { text: qsTr("Log"); font.bold: true }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: Math.min(6, theme.radius)
                        color: "#13141c"
                        border.color: "#414868"
                        ListView {
                            id: logView
                            anchors.fill: parent
                            anchors.margins: 8
                            clip: true
                            model: logModel
                            onCountChanged: currentIndex = count - 1
                            delegate: Label {
                                width: logView.width
                                text: model.text
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                color: "#b4bee6"
                            }
                        }
                    }
                }
            }
        }

        // Help overlay
        Rectangle {
            id: helpOverlay
            anchors.fill: parent
            color: "#000000cc"
            visible: win.helpVisible
            MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }
            Rectangle {
                anchors.centerIn: parent
                width: Math.min(460, parent.width - 40)
                height: 300
                radius: Math.min(8, theme.radius)
                color: "#24283b"
                border.color: "#414868"
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 8
                    Label { text: qsTr("Shortcuts"); font.bold: true; font.pixelSize: 16 }
                    Label { text: qsTr("Space — process selected"); font.pixelSize: 12 }
                    Label { text: qsTr("A — process all"); font.pixelSize: 12 }
                    Label { text: qsTr("S — scan"); font.pixelSize: 12 }
                    Label { text: qsTr("L — look up year"); font.pixelSize: 12 }
                    Label { text: qsTr("Q — quit"); font.pixelSize: 12 }
                    Label { text: qsTr("? — this help"); font.pixelSize: 12 }
                    Button { text: qsTr("Close"); onClicked: win.helpVisible = false }
                }
            }
        }
    }

    ListModel {
        id: logModel
    }
    Connections {
        target: backend
        function onLogLine(text) { logModel.append({ "text": text }) }
        function onPickerFailed(reason) { logModel.append({ "text": reason }) }
    }
}
