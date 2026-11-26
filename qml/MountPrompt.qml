import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Shelf

GlassDialog {
    id: dlg
    property int request: 0
    property var prompt: ({})
    property bool answered: false
    width: Math.min(parent.width - 32, 460)
    padding: 20

    function ask(id, details) {
        answered = false
        request = id
        prompt = details
        username.text = details.user || ""
        domain.text = details.domain || ""
        password.text = ""
        anonymous.checked = false
        open()
        if (username.visible) username.forceActiveFocus()
        else if (password.visible) password.forceActiveFocus()
    }
    function answer(result) {
        answered = true
        Mounts.reply(request, result)
        close()
    }
    function dismiss() { answered = true; close() }
    onAboutToHide: if (!answered) Mounts.cancel(request)
    onClosed: password.text = ""

    contentItem: ColumnLayout {
        spacing: 10
        Label {
            Layout.fillWidth: true
            text: dlg.prompt.text || ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize
        }
        CheckBox { id: anonymous; visible: !!dlg.prompt.anonymous; text: qsTr("Анонимный вход") }
        TextField {
            id: username
            Layout.fillWidth: true
            visible: !!dlg.prompt.needUser && !anonymous.checked
            placeholderText: qsTr("Имя пользователя")
            Accessible.name: placeholderText
        }
        TextField {
            id: domain
            Layout.fillWidth: true
            visible: !!dlg.prompt.needDomain && !anonymous.checked
            placeholderText: qsTr("Домен")
            Accessible.name: placeholderText
        }
        TextField {
            id: password
            Layout.fillWidth: true
            visible: !!dlg.prompt.needPassword && !anonymous.checked
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData
            placeholderText: qsTr("Пароль")
            Accessible.name: placeholderText
            onAccepted: dlg.answer({user: username.text, domain: domain.text, password: text})
        }
        Repeater {
            model: dlg.prompt.choices || []
            TextButton {
                id: choiceButton
                required property string modelData
                required property int index
                Layout.fillWidth: true
                text: modelData
                onClicked: dlg.answer({choice: choiceButton.index})
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            TextButton { text: qsTr("Отмена"); onClicked: dlg.close() }
            TextButton {
                visible: !dlg.prompt.choices
                text: qsTr("Подключиться")
                primary: true
                onClicked: dlg.answer({user: username.text, domain: domain.text, password: password.text, anonymous: anonymous.checked})
            }
        }
    }
}
