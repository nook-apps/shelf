pragma Singleton

import QtCore

Settings {
    category: "process"
    property bool resident: false
    property bool newWindow: false
    property int idleMemoryKiB: 0
}
