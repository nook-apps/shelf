import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Shelf

GlassDialog {
    id: dlg
    property var paths: []
    readonly property var modes: [
        {label: qsTr("Заменить текст"), value: "replace"}, {label: qsTr("Префикс и суффикс"), value: "affix"},
        {label: qsTr("Нумерация"), value: "number"}, {label: qsTr("Верхний регистр"), value: "upper"},
        {label: qsTr("Нижний регистр"), value: "lower"}, {label: qsTr("Дата изменения"), value: "date"}
    ]
    readonly property var options: ({mode: mode.currentValue || "replace", pattern: pattern.text,
        replacement: replacement.text, regex: regexp.checked, keepExtension: extension.checked,
        start: start.value, step: increment.value, digits: digits.value})
    property var preview: []
    readonly property bool valid: preview.length > 0 && preview.every(r => !r.error)
        && preview.some(r => r.before !== r.after)
    width: Math.min(parent.width - 32, 680)
    height: Math.min(parent.height - 32, 520)
    padding: 20

    function showFor(selection) {
        paths = selection
        preview = FileOps.renamePreview(paths, options)
        open()
        mode.forceActiveFocus()
    }
    onOptionsChanged: if (visible) check.restart()
    Timer { id: check; interval: 120; onTriggered: dlg.preview = FileOps.renamePreview(dlg.paths, dlg.options) }

    component Field: TextField {
        Layout.fillWidth: true
        color: Theme.text
        placeholderTextColor: Theme.subtext0
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize
        background: Rectangle {
            color: Theme.ink(0.04)
            radius: Theme.widgetRadius
            border.color: parent.activeFocus ? Theme.accent : Theme.restBorder
        }
    }

    contentItem: ColumnLayout {
        spacing: 10
        Label {
            text: qsTr("Пакетное переименование")
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize + 2
            font.weight: Font.DemiBold
        }
        RowLayout {
            ComboBox {
                id: mode
                model: dlg.modes
                textRole: "label"
                valueRole: "value"
                Accessible.name: qsTr("Способ переименования")
            }
            Item { Layout.fillWidth: true }
            CheckBox { id: extension; text: qsTr("Сохранить расширения"); checked: true }
        }
        RowLayout {
            visible: ["replace", "number", "affix"].includes(mode.currentValue)
            Field {
                id: pattern
                placeholderText: mode.currentValue === "replace" ? qsTr("Найти") : qsTr("Префикс")
                Accessible.name: placeholderText
            }
            Field {
                id: replacement
                placeholderText: mode.currentValue === "replace" ? qsTr("Заменить на") : qsTr("Суффикс")
                Accessible.name: placeholderText
            }
        }
        CheckBox { id: regexp; visible: mode.currentValue === "replace"; text: qsTr("Регулярное выражение") }
        RowLayout {
            visible: mode.currentValue === "number"
            Label { text: qsTr("Начало"); color: Theme.text }
            SpinBox { id: start; from: 0; to: 1000000; value: 1; editable: true; Accessible.name: qsTr("Начало") }
            Label { text: qsTr("Шаг"); color: Theme.text }
            SpinBox { id: increment; from: 1; to: 1000; value: 1; editable: true; Accessible.name: qsTr("Шаг") }
            Label { text: qsTr("Цифр"); color: Theme.text }
            SpinBox { id: digits; from: 1; to: 12; value: 2; editable: true; Accessible.name: qsTr("Цифр") }
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dlg.preview
            ScrollBar.vertical: ScrollBar { }
            delegate: RowLayout {
                id: nameRow
                required property var modelData
                width: ListView.view.width - 12
                Label { Layout.fillWidth: true; Layout.preferredWidth: 1; text: nameRow.modelData.before; color: Theme.subtext0; elide: Text.ElideMiddle }
                Label { text: "→"; color: Theme.subtext0 }
                Label {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    text: nameRow.modelData.error ? nameRow.modelData.after + " · " + nameRow.modelData.error : nameRow.modelData.after
                    color: nameRow.modelData.error ? Theme.red : Theme.text
                    elide: Text.ElideRight
                }
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            TextButton { text: qsTr("Отмена"); onClicked: dlg.close() }
            TextButton {
                text: qsTr("Переименовать")
                primary: true
                enabled: dlg.valid && !check.running && !FileOps.busy
                onClicked: { FileOps.renameMany(dlg.paths, dlg.options); dlg.close() }
            }
        }
    }
}
