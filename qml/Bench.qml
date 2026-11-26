import QtQuick
import Shelf

Item {
    id: bench
    required property Window window
    required property var closePreview

    property var tasks: []
    property var results: ({})
    property var current: null
    property int frame: 0
    property real started: 0
    property real lastFrame: 0
    property var frameTimes: []

    function add(name, action, ready, steps, first) {
        tasks.push({name: name, action: action, ready: ready, steps: steps || 1, first: first})
    }

    function prepare() {
        const w = window
        const root = w.benchPath
        for (const n of [1000, 10000, 50000]) {
            add("open_" + n, () => w.go(root + "/" + n), () => w.dir.count === n, 1, () => w.dir.count > 0)
            add("sort_" + n, () => { w.dir.setViewSort("size", true, "", true) }, () => !w.dir.sorting)
            add("select_all_" + n, () => w.pane.selectAll())
            add("selection_size_" + n, () => w.dir.sizeOf(w.pane.selectedPaths()))
            add("band_" + n, () => {
                const view = w.pane.children.find(child => child.objectName === "fileView")
                w.pane.bandBase = ({})
                w.pane.bandFrom = Qt.point(0, view.originY)
                w.pane.bandPointer = Qt.point(view.width - 15, view.contentHeight - 1)
                w.pane.updateBand()
                w.pane.clearSelection()
            })
        }
        add("scroll", i => w.pane.positionFile(i * 47, 2), null, 24)
        add("wheel_scroll", () => w.pane.scrollBy(90), null, 60)
        add("zoom", i => w.zoom(i % 2 ? -1 : 1), null, 24)
        add("folder_views_512", () => {
            for (let i = 0; i < 512; ++i) FolderViews.get(root + "/views/" + i)
        })
        add("thumbnail_tab_cycles", i => {
            if (i % 2) w.closeTab(w.current)
            else w.newTab(root + "/images", true)
        }, null, 20)
        add("switch_tab", i => {
            if (i === 0) w.newTab(root + "/images", false)
            w.switchTo(i % 2)
        }, null, 20)
        add("quick_look_cycles", i => {
            if (i % 2) bench.closePreview()
            else w.quickLookAt((i / 2) % w.dir.count)
        }, null, 20)
        w.go(root + "/empty")
    }

    FrameAnimation {
        running: true
        onTriggered: {
            if (bench.started === 0 && !bench.current && bench.tasks.length === 0) {
                if (bench.window.dir.count !== 1000) return
                bench.prepare()
            }
            if (!bench.current) {
                bench.current = bench.tasks.shift() || null
                if (!bench.current) {
                    bench.window.benchResults = bench.results
                    Qt.quit()
                    return
                }
                bench.frame = 0
                bench.started = Date.now()
                bench.lastFrame = bench.started
                bench.frameTimes = []
            }
            const task = bench.current
            if (bench.frame > 0) {
                const firstKey = task.name + "_first_frame_ms"
                if (task.first && bench.results[firstKey] === undefined && task.first())
                    bench.results[firstKey] = Date.now() - bench.started
                if (task.ready && !task.ready()) return
                const now = Date.now()
                bench.frameTimes.push(now - bench.lastFrame)
                bench.lastFrame = now
            }
            if (bench.frame === task.steps) {
                bench.results[task.name + "_ms"] = Date.now() - bench.started
                if (task.steps > 1) {
                    const ordered = bench.frameTimes.sort((a, b) => a - b)
                    bench.results[task.name + "_p95_frame_ms"] = ordered[Math.ceil(ordered.length * 0.95) - 1]
                    bench.results[task.name + "_worst_frame_ms"] = ordered[ordered.length - 1]
                    bench.results[task.name + "_mean_frame_ms"] = (Date.now() - bench.started) / task.steps
                }
                bench.current = null
                return
            }
            task.action(bench.frame++)
        }
    }
}
