import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

import StreamingPreferences 1.0

NavigableDialog {
    id: dialog

    property string appName
    property bool isResume
    property string initialOutputTarget
    property string initialFfmpegPath
    property string initialFfmpegExtraArgs
    property int initialWidth
    property int initialHeight
    property int initialFps
    property int initialBitrateKbps
    property int initialVideoCodecConfig
    property bool initialEnableHdr
    property bool initialEnableYUV444

    signal launchRequested(string outputTarget,
                           string ffmpegPath,
                           string ffmpegExtraArgs,
                           int width,
                           int height,
                           int fps,
                           int bitrateKbps,
                           int videoCodecConfig,
                           bool enableHdr,
                           bool enableYUV444)

    title: isResume ? qsTr("Resume and Save Stream") : qsTr("Save Stream")
    standardButtons: Dialog.Ok | Dialog.Cancel

    function isInputValid() {
        return outputTargetField.text.trim().length > 0 &&
               /^\d+x\d+$/i.test(resolutionField.text.trim()) &&
               parseInt(fpsField.text.trim()) > 0 &&
               parseInt(bitrateField.text.trim()) > 0
    }

    function refreshAcceptButton() {
        if (dialog.standardButton) {
            dialog.standardButton(Dialog.Ok).enabled = isInputValid()
        }
    }

    onOpened: {
        outputTargetField.text = initialOutputTarget
        ffmpegPathField.text = initialFfmpegPath
        ffmpegArgsField.text = initialFfmpegExtraArgs
        resolutionField.text = initialWidth + "x" + initialHeight
        fpsField.text = initialFps.toString()
        bitrateField.text = initialBitrateKbps.toString()
        hdrCheckBox.checked = initialEnableHdr
        yuv444CheckBox.checked = initialEnableYUV444

        for (var i = 0; i < codecComboBox.model.length; i++) {
            if (codecComboBox.model[i].value === initialVideoCodecConfig) {
                codecComboBox.currentIndex = i
                break
            }
        }

        outputTargetField.forceActiveFocus()
        refreshAcceptButton()
    }

    onAccepted: {
        if (!isInputValid()) {
            return
        }

        var resolutionMatch = /^(\d+)x(\d+)$/i.exec(resolutionField.text.trim())
        launchRequested(outputTargetField.text.trim(),
                        ffmpegPathField.text.trim(),
                        ffmpegArgsField.text.trim(),
                        parseInt(resolutionMatch[1]),
                        parseInt(resolutionMatch[2]),
                        parseInt(fpsField.text.trim()),
                        parseInt(bitrateField.text.trim()),
                        codecComboBox.currentValue,
                        hdrCheckBox.checked,
                        yuv444CheckBox.checked)
    }

    ColumnLayout {
        spacing: 8

        Label {
            text: isResume ?
                      qsTr("Resume %1 without local playback and send Moonlight's reordered video bitstream to FFmpeg.").arg(appName) :
                      qsTr("Start %1 without local playback and send Moonlight's reordered video bitstream to FFmpeg.").arg(appName)
            wrapMode: Text.Wrap
            Layout.maximumWidth: 460
        }

        Label {
            text: qsTr("Override codec, HDR, 4:4:4, resolution, FPS, and bitrate for this capture only. Normal Moonlight launches keep their usual behavior.")
            wrapMode: Text.Wrap
            Layout.maximumWidth: 460
        }

        Label {
            text: qsTr("Codec:")
            font.bold: true
        }

        ComboBox {
            id: codecComboBox
            Layout.fillWidth: true
            textRole: "text"
            valueRole: "value"
            model: [
                { text: qsTr("Auto (prefer host best match)"), value: StreamingPreferences.VCC_AUTO },
                { text: qsTr("AV1"), value: StreamingPreferences.VCC_FORCE_AV1 },
                { text: qsTr("HEVC"), value: StreamingPreferences.VCC_FORCE_HEVC },
                { text: qsTr("H.264"), value: StreamingPreferences.VCC_FORCE_H264 }
            ]
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            CheckBox {
                id: hdrCheckBox
                text: qsTr("HDR")
            }

            CheckBox {
                id: yuv444CheckBox
                text: qsTr("YUV 4:4:4")
            }
        }

        Label {
            text: qsTr("Resolution:")
            font.bold: true
        }

        TextField {
            id: resolutionField
            Layout.fillWidth: true
            placeholderText: "1920x1080"
            onTextChanged: dialog.refreshAcceptButton()
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Label {
                    text: qsTr("FPS:")
                    font.bold: true
                }

                TextField {
                    id: fpsField
                    Layout.fillWidth: true
                    inputMethodHints: Qt.ImhDigitsOnly
                    onTextChanged: dialog.refreshAcceptButton()
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Label {
                    text: qsTr("Bitrate (Kbps):")
                    font.bold: true
                }

                TextField {
                    id: bitrateField
                    Layout.fillWidth: true
                    inputMethodHints: Qt.ImhDigitsOnly
                    onTextChanged: dialog.refreshAcceptButton()
                }
            }
        }

        Label {
            text: qsTr("Output file, folder, or URL:")
            font.bold: true
        }

        TextField {
            id: outputTargetField
            Layout.fillWidth: true
            placeholderText: "C:/Users/Administrator/Desktop or C:/capture/session.mkv or rtmp://127.0.0.1/live/test"

            onTextChanged: {
                dialog.refreshAcceptButton()
            }

            Keys.onReturnPressed: {
                if (dialog.isInputValid()) {
                    dialog.accept()
                }
            }

            Keys.onEnterPressed: {
                if (dialog.isInputValid()) {
                    dialog.accept()
                }
            }
        }

        Label {
            text: qsTr("FFmpeg path (optional):")
            font.bold: true
        }

        TextField {
            id: ffmpegPathField
            Layout.fillWidth: true
            placeholderText: "ffmpeg"

            Keys.onReturnPressed: {
                if (dialog.isInputValid()) {
                    dialog.accept()
                }
            }

            Keys.onEnterPressed: {
                if (dialog.isInputValid()) {
                    dialog.accept()
                }
            }
        }

        Label {
            text: qsTr("Extra FFmpeg output args (optional):")
            font.bold: true
        }

        TextField {
            id: ffmpegArgsField
            Layout.fillWidth: true
            placeholderText: "-f matroska"

            Keys.onReturnPressed: {
                if (dialog.isInputValid()) {
                    dialog.accept()
                }
            }

            Keys.onEnterPressed: {
                if (dialog.isInputValid()) {
                    dialog.accept()
                }
            }
        }

        Label {
            text: qsTr("If you enter a folder, Moonlight will create an MKV file inside it. If extra FFmpeg args are left empty, Moonlight will auto-pick a container for RTMP, RTSP, SRT, and UDP outputs. AV1 is wrapped as IVF before feeding FFmpeg so host-side AV1/HDR streams can be copied out directly.")
            wrapMode: Text.Wrap
            Layout.maximumWidth: 460
        }
    }
}
