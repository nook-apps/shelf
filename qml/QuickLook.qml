import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window
import Shelf

// Быстрый просмотр по пробелу, как Quick Look: картинка, начало текста,
// первая страница документа, миниатюра или значок со сведениями.
// Стрелки листают соседние файлы, Space и Esc закрывают, Enter открывает
GlassDialog {
    id: dlg

    property DirModel model: null
    property int row: -1
    property var info: ({})
    property string kind: ""
    Preview { id: preview }
    onAboutToHide: preview.cancel()

    // Открыть файл программой (Enter) — решает окно, как при двойном щелчке
    signal openRequested(int row)

    function show(m, r) {
        model = m
        if (!load(r)) return
        open()
    }
    function load(r) {
        if (!model || r < 0 || r >= model.count) return false
        row = r
        info = model.rowInfo(r)
        kind = preview.kindFor(info.path, info.mimeType, info.isDir)
        preview.open(info.path, kind)
        return true
    }
    function step(d) {
        if (model) load(Math.max(0, Math.min(model.count - 1, row + d)))
    }

    Connections {
        target: dlg.visible ? dlg.model : null
        function onDataChanged() {
            const current = dlg.model.indexOfPath(dlg.info.path || "")
            if (current >= 0 && dlg.model.mimeTypeAt(current) !== dlg.info.mimeType)
                dlg.load(current)
        }
    }

    width: Math.min(parent.width - 80, 980)
    height: Math.min(parent.height - 80, 760)
    padding: 16
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property bool document: kind === "pdf" || kind === "office"
    readonly property bool fallback: preview.status === "unavailable" || preview.status === "failed" || preview.status === "timeout"
    readonly property bool showsIcon: kind === "dir" || ((kind === "other" || fallback) && thumbnail.status !== Image.Ready)
        || picture.status === Image.Error || preview.status === "binary"
    readonly property string previewMessage: {
        if (preview.status === "loading" || picture.status === Image.Loading) return qsTr("Загрузка просмотра…")
        if (preview.status === "unavailable") return document
            ? (kind === "office" ? qsTr("Для просмотра нужны LibreOffice и Poppler") : qsTr("Для просмотра PDF нужен Poppler"))
            : qsTr("Просмотр недоступен")
        if (preview.status === "timeout") return qsTr("Просмотр не успел загрузиться")
        if (preview.status === "failed" || picture.status === Image.Error) return qsTr("Не удалось прочитать файл")
        if (preview.status === "binary") return qsTr("Двоичный файл — просмотра нет")
        if (kind === "other") return qsTr("Просмотр недоступен")
        return ""
    }

    contentItem: Item {
        focus: true
        Keys.onPressed: (e) => {
            switch (e.key) {
            case Qt.Key_Left:
            case Qt.Key_Up:
                dlg.step(-1); break
            case Qt.Key_Right:
            case Qt.Key_Down:
                dlg.step(1); break
            case Qt.Key_Space:
                dlg.close(); break
            case Qt.Key_Return:
            case Qt.Key_Enter:
                dlg.close()
                dlg.openRequested(dlg.row)
                break
            default:
                return
            }
            e.accepted = true
        }

        // Заголовок: имя и сведения одной строкой
        Column {
            id: head
            width: parent.width
            spacing: 2
            Text {
                width: parent.width
                text: dlg.info.name || ""
                elide: Text.ElideMiddle
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSize + 1
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                text: [dlg.info.mimeComment, dlg.info.sizeText, dlg.info.dateText].filter(function (s) { return s }).join("  ·  ")
                elide: Text.ElideRight
                color: Theme.subtext0
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSize - 2
            }
        }

        Item {
            id: body
            anchors { left: parent.left; right: parent.right; top: head.bottom; topMargin: 12; bottom: parent.bottom }

            ThumbnailSource {
                id: thumb
                enabled: dlg.visible && (dlg.kind === "other" || dlg.document && dlg.fallback)
                path: dlg.info.path || ""
                mime: dlg.info.mimeType || ""
                mtime: dlg.info.mtime ?? -1
                size: Math.min(body.width, body.height)
            }

            // Картинка — в фоне и не крупнее окна: большие фото не держат интерфейс
            Image {
                id: picture
                anchors.fill: parent
                visible: status === Image.Ready
                // «#», «%» и пробелы в имени — экранируем, иначе это не тот URL
                source: !dlg.visible ? "" : dlg.kind === "image"
                    ? "file://" + encodeURIComponent(dlg.info.path).replace(/%2F/g, "/") : preview.image
                asynchronous: true
                fillMode: Image.PreserveAspectFit
                sourceSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)
                autoTransform: true
                smooth: true
                mipmap: true
            }
            Image {
                id: thumbnail
                anchors.fill: parent
                visible: thumb.source !== "" && status === Image.Ready
                source: thumb.source
                asynchronous: true
                sourceSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)
                fillMode: Image.PreserveAspectFit
                smooth: true
            }

            ScrollView {
                anchors.fill: parent
                visible: dlg.kind === "text" && preview.status === "ready"
                clip: true
                TextArea {
                    text: preview.text
                    readOnly: true
                    selectByMouse: true
                    // Клавиши остаются у просмотра: стрелки листают файлы, а не двигают курсор
                    activeFocusOnPress: false
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.WrapAnywhere
                    color: Theme.text
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.base
                    font.family: "monospace"
                    font.pixelSize: Theme.fontSize - 1
                    background: null
                }
            }

            Column {
                anchors.centerIn: parent
                visible: dlg.showsIcon || preview.status === "loading" || picture.status === Image.Loading
                spacing: 14
                FolderIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: dlg.kind === "dir"
                    size: 128
                    iconName: dlg.info.icon || "folder"
                }
                FileIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: dlg.showsIcon && dlg.kind !== "dir"
                    size: 128
                    mimeType: dlg.info.mimeType || ""
                    fileName: dlg.info.name || ""
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: text !== ""
                    text: dlg.previewMessage
                    color: Theme.subtext0
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSize - 1
                }
            }
        }

        Text {
            anchors { right: parent.right; bottom: parent.bottom; bottomMargin: -12 }
            visible: dlg.document && picture.status === Image.Ready || preview.truncated && preview.status === "ready"
            text: dlg.document ? qsTr("Первая страница") : qsTr("Показано начало файла")
            color: Theme.subtext0
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize - 3
        }
    }
}
