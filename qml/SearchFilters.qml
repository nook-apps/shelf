pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Shelf

GlassDialog {
    id: dlg
    property DirModel model: null
    readonly property var kinds: [
        {name: qsTr("Все типы"), value: ""}, {name: qsTr("Изображения"), value: "image"},
        {name: qsTr("Документы"), value: "document"}, {name: qsTr("Видео"), value: "video"},
        {name: qsTr("Аудио"), value: "audio"}, {name: qsTr("Архивы"), value: "archive"},
        {name: qsTr("Папки"), value: "folder"}
    ]
    readonly property var dates: [
        {name: qsTr("Любая дата"), value: 0}, {name: qsTr("Сегодня"), value: 1},
        {name: qsTr("Последние 7 дней"), value: 7}, {name: qsTr("Последние 30 дней"), value: 30},
        {name: qsTr("Последний год"), value: 365}
    ]
    width: Math.min(parent.width - 32, 400)
    padding: 20

    function showFor(current) {
        model = current
        const options = current.searchOptions
        kind.currentIndex = Math.max(0, kinds.findIndex(k => k.value === options.type))
        date.currentIndex = Math.max(0, dates.findIndex(d => d.value === options.days))
        minimum.text = options.min !== undefined && options.min >= 0 ? String(options.min / 1024) : ""
        maximum.text = options.max !== undefined && options.max >= 0 ? String(options.max / 1024) : ""
        recursive.checked = options.recursive !== false
        open()
        kind.forceActiveFocus()
    }

    component FieldLabel: Label {
        color: Theme.subtext0
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize - 1
    }
    component FilterSelect: ComboBox {
        id: select
        Layout.fillWidth: true
        implicitHeight: 32
        hoverEnabled: true
        padding: 10
        topPadding: 0
        bottomPadding: 0
        spacing: 8
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize - 1

        contentItem: Text {
            text: select.displayText
            color: Theme.text
            font: select.font
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        indicator: Text {
            x: select.mirrored ? select.padding : select.width - width - select.padding
            anchors.verticalCenter: parent.verticalCenter
            text: "\u{f078}"
            color: Theme.subtext0
            font.family: Theme.iconFont
            font.pixelSize: Theme.fontSize - 3
            Accessible.ignored: true
        }
        background: Rectangle {
            radius: Theme.btnRadius(height, select.down, Theme.widgetRadius)
            color: select.down ? Theme.pressedFill(Theme.accent) : select.hovered ? Theme.hoverFill : Theme.ink(0.04)
            border.width: 1
            border.color: select.activeFocus || select.popup.visible ? Theme.accent : Theme.restBorder
        }
        delegate: ItemDelegate {
            id: option
            required property var model
            required property int index
            width: ListView.view.width
            height: 32
            leftPadding: 10
            rightPadding: 10
            topPadding: 0
            bottomPadding: 0
            text: option.model[select.textRole]
            hoverEnabled: true
            highlighted: select.highlightedIndex === option.index
            palette.text: Theme.text
            palette.highlightedText: Theme.text
            font.family: select.font.family
            font.pixelSize: select.font.pixelSize
            font.weight: select.currentIndex === option.index ? Font.DemiBold : Font.Normal
            background: Rectangle {
                radius: Theme.widgetRadius
                color: option.highlighted ? Theme.activeFill(Theme.accent) : option.hovered ? Theme.hoverFill : "transparent"
            }
        }
        popup: Popup {
            y: select.height + 4
            width: select.width
            padding: 4
            margins: 6
            height: Math.min(select.count * 32 + topPadding + bottomPadding, (dlg.parent?.height ?? 400) - 2 * margins)
            contentItem: ListView {
                clip: true
                model: select.delegateModel
                currentIndex: select.highlightedIndex
                highlightMoveDuration: 0
                ScrollBar.vertical: ScrollBar { }
            }
            background: Rectangle {
                color: Theme.material ? Theme.surface1 : Theme.base
                radius: Theme.widgetRadius + 2
                border.width: 1
                border.color: Theme.material ? "transparent" : Theme.glassBorder
            }
        }
    }
    component SizeField: TextField {
        id: field
        Layout.fillWidth: true
        implicitHeight: 32
        leftPadding: 10
        rightPadding: 10
        color: Theme.text
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize - 1
        inputMethodHints: Qt.ImhDigitsOnly
        validator: IntValidator { bottom: 0 }
        background: Rectangle {
            color: Theme.ink(0.04)
            radius: Theme.btnRadius(height, false, Theme.widgetRadius)
            border.width: 1
            border.color: field.activeFocus ? Theme.accent : Theme.restBorder
        }
    }

    contentItem: ColumnLayout {
        spacing: 12
        FieldLabel {
            text: qsTr("Фильтры поиска")
            color: Theme.text
            font.pixelSize: Theme.fontSize + 1
            font.weight: Font.DemiBold
        }
        GridLayout {
            columns: 2
            rowSpacing: 8
            columnSpacing: 12
            Layout.fillWidth: true
            FieldLabel { text: qsTr("Тип") }
            FilterSelect {
                id: kind
                model: dlg.kinds
                textRole: "name"
                valueRole: "value"
                Accessible.name: qsTr("Тип")
            }
            FieldLabel { text: qsTr("Дата изменения") }
            FilterSelect {
                id: date
                model: dlg.dates
                textRole: "name"
                valueRole: "value"
                Accessible.name: qsTr("Дата изменения")
            }
            FieldLabel { text: qsTr("Больше, КиБ") }
            SizeField { id: minimum; Accessible.name: qsTr("Больше, КиБ") }
            FieldLabel { text: qsTr("Меньше, КиБ") }
            SizeField { id: maximum; Accessible.name: qsTr("Меньше, КиБ") }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.bottomMargin: 4
            spacing: 12
            FieldLabel {
                Layout.fillWidth: true
                text: qsTr("Искать во вложенных папках")
                color: Theme.text
                wrapMode: Text.WordWrap
                Accessible.ignored: true
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: recursive.toggle()
                }
            }
            ToggleSwitch {
                id: recursive
                activeFocusOnTab: true
                Accessible.role: Accessible.CheckBox
                Accessible.name: qsTr("Искать во вложенных папках")
                Accessible.checkable: true
                Accessible.checked: checked
                Accessible.onPressAction: toggle()
                Keys.onSpacePressed: toggle()
                Keys.onReturnPressed: toggle()
                Keys.onEnterPressed: toggle()
                onToggled: (on) => checked = on
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            TextButton { text: qsTr("Сбросить"); onClicked: { dlg.model.searchOptions = {}; dlg.close() } }
            TextButton {
                text: qsTr("Применить")
                primary: true
                enabled: (!minimum.text || minimum.acceptableInput) && (!maximum.text || maximum.acceptableInput)
                         && (!minimum.text || !maximum.text || Number(minimum.text) < Number(maximum.text))
                onClicked: {
                    dlg.model.searchOptions = {type: kind.currentValue, days: date.currentValue,
                        min: minimum.text ? Number(minimum.text) * 1024 : -1,
                        max: maximum.text ? Number(maximum.text) * 1024 : -1, recursive: recursive.checked}
                    dlg.close()
                }
            }
        }
    }
}
