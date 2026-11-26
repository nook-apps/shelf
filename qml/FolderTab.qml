import QtQuick
import Shelf

// Одна вкладка: своя папка, история, выделение и поиск.
// Что делать с файлами, по-прежнему решает Main — он подключается к pane.
Item {
    id: tab

    property alias dir: tabDir
    property alias pane: tabPane
    property string title: ""
    property FolderTab splitOwner: null
    property FolderTab secondary: null
    property bool secondActive: false
    readonly property bool split: !!splitOwner || !!secondary
    property var history: []
    property int histIndex: -1
    // Что выделить после ближайшего перечитывания папки
    property var pendingSelect: []
    property string pendingRename: ""
    property int pendingRow: -1

    signal message(string text)

    Loader {
        id: caption
        active: tab.split
        anchors { left: parent.left; right: parent.right; top: parent.top; leftMargin: 12; rightMargin: 12 }
        height: active ? 30 : 0
        sourceComponent: Text {
            text: tab.dir.path
            elide: Text.ElideMiddle
            verticalAlignment: Text.AlignVCenter
            color: Theme.subtext0
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize - 2
            MouseArea { anchors.fill: parent; onClicked: tab.pane.focusView() }
        }
    }

    DirModel {
        id: tabDir
        onPathChanged: tab.pendingRename = ""
        onReloaded: {
            if (tab.pendingSelect.length > 0) {
                tabPane.selectPaths(tab.pendingSelect)
                tab.pendingSelect = []
                const renamePath = tab.pendingRename
                tab.pendingRename = ""
                if (renamePath && tabDir.indexOfPath(renamePath) >= 0)
                    tabPane.renamingPath = renamePath
            } else if (tab.pendingRow >= 0) {
                tabPane.selectRow(tab.pendingRow)
                tab.pendingRow = -1
            } else {
                tabPane.prune()
            }
        }
        onErrorChanged: if (error) tab.message(error)
    }

    FilePane {
        id: tabPane
        anchors { left: parent.left; right: parent.right; top: caption.bottom; bottom: parent.bottom }
        model: tabDir
        focus: true
    }
}
