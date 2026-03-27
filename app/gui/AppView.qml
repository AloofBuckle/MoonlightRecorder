import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.2

import AppModel 1.0
import ComputerManager 1.0
import SdlGamepadKeyNavigation 1.0

CenteredGridView {
    property int computerIndex
    property AppModel appModel : createModel()
    property bool activated
    property bool showHiddenGames
    property bool showGames

    id: appGrid
    focus: true
    activeFocusOnTab: true
    topMargin: 20
    bottomMargin: 5
    cellWidth: 230; cellHeight: 297;

    function computerLost()
    {
        // Go back to the PC view on PC loss
        stackView.pop()
    }

    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        // We do this here instead of onActivated to avoid losing the user's
        // selection when backing out of a different page of the app.
        currentIndex = -1
    }

    StackView.onActivated: {
        appModel.computerLost.connect(computerLost)
        activated = true

        // Highlight the first item if a gamepad is connected
        if (currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            currentIndex = 0
        }
    }

    StackView.onDeactivating: {
        appModel.computerLost.disconnect(computerLost)
        activated = false
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import AppModel 1.0; AppModel {}', parent, '')
        model.initialize(ComputerManager, computerIndex, showHiddenGames)
        return model
    }

    function pushStreamSegue(appName, session, isResume)
    {
        var component = Qt.createComponent("StreamSegue.qml")
        var segue = component.createObject(stackView, {
                                               "appName": appName,
                                               "session": session,
                                               "isResume": isResume
                                           })
        stackView.push(segue)
    }

    function createLaunchSession(appIndex,
                                 enableBitstreamPassthrough,
                                 outputTarget,
                                 ffmpegPath,
                                 ffmpegExtraArgs,
                                 width,
                                 height,
                                 fps,
                                 bitrateKbps,
                                 videoCodecConfig,
                                 enableHdr,
                                 enableYUV444)
    {
        if (enableBitstreamPassthrough) {
            return appModel.createBitstreamSessionForApp(appIndex,
                                                         outputTarget,
                                                         ffmpegPath,
                                                         ffmpegExtraArgs,
                                                         width,
                                                         height,
                                                         fps,
                                                         bitrateKbps,
                                                         videoCodecConfig,
                                                         enableHdr,
                                                         enableYUV444)
        }

        return appModel.createSessionForApp(appIndex)
    }

    function queueLaunch(appIndex,
                         appName,
                         appId,
                         quitExistingApp,
                         enableBitstreamPassthrough,
                         outputTarget,
                         ffmpegPath,
                         ffmpegExtraArgs,
                         width,
                         height,
                         fps,
                         bitrateKbps,
                         videoCodecConfig,
                         enableHdr,
                         enableYUV444)
    {
        var runningId = appModel.getRunningAppId()
        if (runningId !== 0 && runningId !== appId) {
            if (quitExistingApp) {
                quitAppDialog.appName = appModel.getRunningAppName()
                quitAppDialog.segueToStream = true
                quitAppDialog.nextAppName = appName
                quitAppDialog.nextAppIndex = appIndex
                quitAppDialog.nextLaunchBitstream = enableBitstreamPassthrough
                quitAppDialog.nextOutputTarget = outputTarget
                quitAppDialog.nextFfmpegPath = ffmpegPath
                quitAppDialog.nextFfmpegExtraArgs = ffmpegExtraArgs
                quitAppDialog.nextWidth = width
                quitAppDialog.nextHeight = height
                quitAppDialog.nextFps = fps
                quitAppDialog.nextBitrateKbps = bitrateKbps
                quitAppDialog.nextVideoCodecConfig = videoCodecConfig
                quitAppDialog.nextEnableHdr = enableHdr
                quitAppDialog.nextEnableYUV444 = enableYUV444
                quitAppDialog.open()
            }

            return
        }

        pushStreamSegue(appName,
                        createLaunchSession(appIndex,
                                            enableBitstreamPassthrough,
                                            outputTarget,
                                            ffmpegPath,
                                            ffmpegExtraArgs,
                                            width,
                                            height,
                                            fps,
                                            bitrateKbps,
                                            videoCodecConfig,
                                            enableHdr,
                                            enableYUV444),
                        runningId === appId)
    }

    model: appModel

    delegate: NavigableItemDelegate {
        width: 220; height: 287;
        grid: appGrid

        property alias appContextMenu: appContextMenuLoader.item
        property alias appNameText: appNameTextLoader.item

        // Dim the app if it's hidden
        opacity: model.hidden ? 0.4 : 1.0

        Image {
            property bool isPlaceholder: false

            id: appIcon
            anchors.horizontalCenter: parent.horizontalCenter
            y: 10
            source: model.boxart

            onSourceSizeChanged: {
                // Nearly all of Nvidia's official box art does not match the dimensions of placeholder
                // images, however the one known exception is Overcooked. Therefore, we only execute
                // the image size checks if this is not an app collector game. We know the officially
                // supported games all have box art, so this check is not required.
                if (!model.isAppCollectorGame &&
                    ((sourceSize.width === 130 && sourceSize.height === 180) || // GFE 2.0 placeholder image
                     (sourceSize.width === 628 && sourceSize.height === 888) || // GFE 3.0 placeholder image
                     (sourceSize.width === 200 && sourceSize.height === 266)))  // Our no_app_image.png
                {
                    isPlaceholder = true
                }
                else
                {
                    isPlaceholder = false
                }

                width = 200
                height = 267
            }

            // Display a tooltip with the full name if it's truncated
            ToolTip.text: model.name
            ToolTip.delay: 1000
            ToolTip.timeout: 5000
            ToolTip.visible: (parent.hovered || parent.highlighted) && (!appNameText || appNameText.truncated)
        }

        RoundButton {
            focusPolicy: Qt.NoFocus
            enabled: false

            anchors.top: appIcon.top
            anchors.right: appIcon.right
            anchors.topMargin: 8
            anchors.rightMargin: 8
            implicitWidth: 42
            implicitHeight: 42
            visible: false

            icon.source: "qrc:/res/settings.svg"
            icon.width: 22
            icon.height: 22

            Material.background: "#D0808080"
        }

        Loader {
            active: model.running
            asynchronous: true
            anchors.fill: appIcon

            sourceComponent: Item {
                width: parent.width
                height: parent.height

                Column {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 16
                    spacing: 12

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: runningStatusText.implicitWidth + 22
                        height: 32
                        radius: 16
                        color: "#B0202020"
                        border.color: "#D84D4D"
                        border.width: 1

                        Label {
                            id: runningStatusText
                            anchors.centerIn: parent
                            text: qsTr("Source Running")
                            color: "#F2F2F2"
                            font.pointSize: 11
                            font.bold: true
                        }
                    }

                    Button {
                        focusPolicy: Qt.NoFocus
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Continue Recording")

                        icon.source: "qrc:/res/play_arrow_FILL1_wght700_GRAD200_opsz48.svg"
                        icon.width: 28
                        icon.height: 28

                        onClicked: {
                            launchOrResumeSelectedApp(true)
                        }

                        ToolTip.text: qsTr("Open the recorder for this running source")
                        ToolTip.delay: 1000
                        ToolTip.timeout: 3000
                        ToolTip.visible: hovered
                    }
                }
            }
        }

        Loader {
            id: appNameTextLoader
            active: appIcon.isPlaceholder

            // This loader is not asynchronous to avoid noticeable differences
            // in the time in which the text loads for each game.

            width: appIcon.width
            height: model.running ? 175 : appIcon.height

            anchors.left: appIcon.left
            anchors.right: appIcon.right
            anchors.bottom: appIcon.bottom

            sourceComponent: Label {
                id: appNameText
                text: model.name
                font.pointSize: 22
                leftPadding: 20
                rightPadding: 20
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                elide: Text.ElideRight
            }
        }

        function launchOrResumeSelectedApp(quitExistingApp)
        {
            appGrid.queueLaunch(index,
                                model.name,
                                model.appid,
                                quitExistingApp,
                                false,
                                "",
                                "",
                                "",
                                0,
                                0,
                                0,
                                0,
                                0,
                                false,
                                false)
        }

        onClicked: {
            launchOrResumeSelectedApp(true)
        }

        MouseArea {
            anchors.fill: parent
            enabled: false
            acceptedButtons: Qt.NoButton
        }

        Keys.onReturnPressed: {
            launchOrResumeSelectedApp(true)
        }

        Keys.onEnterPressed: {
            launchOrResumeSelectedApp(true)
        }

        Keys.onMenuPressed: {
            launchOrResumeSelectedApp(true)
        }

        Loader {
            id: appContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: appContextMenu
                initiator: appContextMenuLoader.parent
                NavigableMenuItem {
                    text: model.running ? qsTr("Resume Recording") : qsTr("Start Recording")
                    onTriggered: launchOrResumeSelectedApp(true)
                }
            }
        }
    }

    Row {
        anchors.centerIn: parent
        spacing: 5
        visible: appGrid.count === 0

        Label {
            text: qsTr("This host doesn't currently expose any recording sources.")
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    NavigableMessageDialog {
        id: quitAppDialog
        property string appName : ""
        property bool segueToStream : false
        property string nextAppName: ""
        property int nextAppIndex: 0
        property bool nextLaunchBitstream : false
        property string nextOutputTarget : ""
        property string nextFfmpegPath : ""
        property string nextFfmpegExtraArgs : ""
        property int nextWidth : 0
        property int nextHeight : 0
        property int nextFps : 0
        property int nextBitrateKbps : 0
        property int nextVideoCodecConfig : 0
        property bool nextEnableHdr : false
        property bool nextEnableYUV444 : false
        text: qsTr("Stop '%1' on the host before switching recording sources?").arg(appName)
        standardButtons: Dialog.Yes | Dialog.No

        function quitApp() {
            var component = Qt.createComponent("QuitSegue.qml")
            var params = {"appName": appName, "quitRunningAppFn": function() { appModel.quitRunningApp() }}
            if (segueToStream) {
                // Store the session and app name if we're going to stream after
                // successfully quitting the old app.
                params.nextAppName = nextAppName
                params.nextSession = appGrid.createLaunchSession(nextAppIndex,
                                                                 nextLaunchBitstream,
                                                                 nextOutputTarget,
                                                                 nextFfmpegPath,
                                                                 nextFfmpegExtraArgs,
                                                                 nextWidth,
                                                                 nextHeight,
                                                                 nextFps,
                                                                 nextBitrateKbps,
                                                                 nextVideoCodecConfig,
                                                                 nextEnableHdr,
                                                                 nextEnableYUV444)
            }
            else {
                params.nextAppName = null
                params.nextSession = null
            }

            stackView.push(component.createObject(stackView, params))
        }

        onAccepted: quitApp()
    }

    ScrollBar.vertical: ScrollBar {}
}
