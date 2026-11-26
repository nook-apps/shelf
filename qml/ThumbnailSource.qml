import QtQuick
import Shelf

QtObject {
    id: req
    required property string path
    required property string mime
    required property int size
    required property double mtime
    property string watched: ""
    property int revision: 0
    property bool enabled: true

    readonly property string source: watched && Thumbnailer.revision >= 0 && revision >= 0
        ? Thumbnailer.thumbnail(watched, mime, size, mtime) : ""
    readonly property string wanted: enabled ? path : ""
    onWantedChanged: sync()
    Component.onCompleted: sync()
    Component.onDestruction: if (watched) Thumbnailer.unwatch(watched)

    function sync() {
        if (watched === wanted) return
        if (watched) Thumbnailer.unwatch(watched)
        if (wanted) Thumbnailer.watch(wanted)
        watched = wanted
    }
    property Connections updates: Connections {
        target: req.watched ? Thumbnailer : null
        function onPathChanged(path) { if (path === req.watched) ++req.revision }
    }
}
