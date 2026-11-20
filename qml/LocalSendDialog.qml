import QtQuick
import QtQuick.Controls.Basic
import Shelf

GlassDialog {
    id: dlg

    readonly property string installCommand: "flatpak install flathub org.localsend.localsend_app"
    property bool copied: false

    padding: 22
    width: Math.min(520, parent ? parent.width - 32 : 520)
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: copyButton.forceActiveFocus()

    contentItem: Column {
        spacing: 16

        Column {
            width: parent.width
            spacing: 6
            Text {
                width: parent.width
                text: qsTr("Установите LocalSend")
                wrapMode: Text.Wrap
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSize + 4
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                text: qsTr("Отправляйте файлы с компьютера на телефон и другие устройства в одной сети.")
                wrapMode: Text.Wrap
                color: Theme.subtext0
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSize - 1
            }
        }

        Column {
            width: parent.width
            spacing: 8
            Text {
                width: parent.width
                text: qsTr("• Без кабеля, аккаунта и загрузки в облако")
                wrapMode: Text.Wrap
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSize - 1
            }
            Text {
                width: parent.width
                text: qsTr("• Между Linux, Windows, macOS, Android и iPhone")
                wrapMode: Text.Wrap
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSize - 1
            }
        }

        Rectangle {
            width: parent.width
            height: command.implicitHeight + 24
            radius: Theme.widgetRadius
            color: Theme.ink(0.04)
            border.width: 1
            border.color: Theme.restBorder
            TextEdit {
                id: command
                objectName: "localSendCommand"
                x: 12; y: 12
                width: parent.width - 24
                text: dlg.installCommand
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                color: Theme.text
                selectionColor: Theme.accent
                selectedTextColor: Theme.base
                font.family: "monospace"
                font.pixelSize: Theme.fontSize - 2
                Accessible.name: qsTr("Команда установки")
            }
        }
        Text {
            width: parent.width
            text: qsTr("Для команды нужны Flatpak и Flathub. Другие способы установки — на сайте LocalSend. Установите LocalSend и на принимающее устройство.")
            wrapMode: Text.Wrap
            color: Theme.subtext0
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize - 2
        }

        Flow {
            width: parent.width
            spacing: 8
            TextButton {
                id: copyButton
                objectName: "localSendCopy"
                text: dlg.copied ? qsTr("Скопировано") : qsTr("Скопировать команду")
                primary: true
                Accessible.role: Accessible.Button
                Accessible.name: text
                onClicked: { FileOps.copyText(dlg.installCommand); dlg.copied = true }
            }
            TextButton {
                text: qsTr("Сайт LocalSend")
                Accessible.role: Accessible.Button
                Accessible.name: text
                onClicked: Qt.openUrlExternally("https://localsend.org/download")
            }
            TextButton {
                objectName: "localSendClose"
                text: qsTr("Закрыть")
                Accessible.role: Accessible.Button
                Accessible.name: text
                onClicked: dlg.close()
            }
        }
    }
}
