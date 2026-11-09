import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import QtQuick.Dialogs

ApplicationWindow {
    id: root
    visible: true
    width: 1440
    height: 900
    minimumWidth: 420
    minimumHeight: 640
    title: "随剪"
    color: "#0c0d11"

    // 响应式布局（根据窗口宽度与横竖屏自动无缝切换，完全纯净无人工按钮）
    readonly property bool isMobileMode: root.width < 920 || (root.height > root.width && root.width < 1050)
    property int mobileTab: 1 // 默认在素材池，方便导入

    // 拖拽与素材源状态
    property bool isDragActive: false
    property string currentSourceLabel: "未选择"
    property string detectedDjiPath: ""
    property var recentDirs: []

    // 全局剪辑工程状态
    property var mediaPool: []
    property var datesList: ["全部"]
    property string activeDate: "全部"
    property var selectedPathsMap: ({})
    property int selectedCount: 0
    property string selectedDurationStr: "00:00"
    property real selectedTotalSeconds: 0.0
    property string mediaViewMode: "grid" // "grid" | "list"
    property bool isImporting: false
    property int importCurrent: 0
    property int importTotal: 0
    property string importFilename: ""

    property var timelineClips: []
    property int activeClipIndex: 0
    property bool isContinuousPlay: true
    property bool isTheaterMode: false

    property string masterCanvas: "horizontal"
    property string targetColor: "10bit"

    // 撤销 / 重做系统 (Undo / Redo System，全面支持触控大按钮与快捷键)
    property var undoStack: []
    property var redoStack: []
    readonly property bool canUndo: undoStack.length > 0
    readonly property bool canRedo: redoStack.length > 0

    // 可推出设备断开与热插拔审计状态
    property bool isDeviceDisconnected: false
    property string disconnectedDeviceName: ""
    property var missingPathsMap: ({})
    property var lastDiffReport: ({ missing: [], modified: [], new_count: 0 })

    // 口播识别与字幕状态
    property var speechAnnotations: ({})  // { videoPath: [annotated segments] }
    property string currentSubtitleText: ""  // 当前播放位置的字幕文本
    property var pendingUsbDevice: null

    function pushUndoState(actionName) {
        var snapshot = {
            action: actionName || "剪辑变动",
            clips: JSON.parse(JSON.stringify(root.timelineClips)),
            activeClipIndex: root.activeClipIndex
        }
        var newStack = undoStack.slice(Math.max(0, undoStack.length - 49))
        newStack.push(snapshot)
        undoStack = newStack
        redoStack = []
    }

    function undo() {
        if (!canUndo) {
            statusBanner.show("已到最早历史记录，无法撤销")
            return
        }
        var currentSnapshot = {
            action: "当前状态",
            clips: JSON.parse(JSON.stringify(root.timelineClips)),
            activeClipIndex: root.activeClipIndex
        }
        var newRedo = redoStack.slice()
        newRedo.push(currentSnapshot)
        redoStack = newRedo

        var prevSnapshot = undoStack[undoStack.length - 1]
        var newUndo = undoStack.slice(0, undoStack.length - 1)
        undoStack = newUndo

        root.timelineClips = prevSnapshot.clips
        root.activeClipIndex = Math.min(prevSnapshot.activeClipIndex, root.timelineClips.length - 1)
        if (root.activeClipIndex >= 0) {
            root.loadActiveClip(root.activeClipIndex, false)
        }
        statusBanner.show("↩ 已撤销: " + prevSnapshot.action)
    }

    function redo() {
        if (!canRedo) {
            statusBanner.show("已到最新状态，无法重做")
            return
        }
        var nextSnapshot = redoStack[redoStack.length - 1]
        var newRedo = redoStack.slice(0, redoStack.length - 1)
        redoStack = newRedo

        var currentSnapshot = {
            action: "撤销前状态",
            clips: JSON.parse(JSON.stringify(root.timelineClips)),
            activeClipIndex: root.activeClipIndex
        }
        var newUndo = undoStack.slice()
        newUndo.push(currentSnapshot)
        undoStack = newUndo

        root.timelineClips = nextSnapshot.clips
        root.activeClipIndex = Math.min(nextSnapshot.activeClipIndex, root.timelineClips.length - 1)
        if (root.activeClipIndex >= 0) {
            root.loadActiveClip(root.activeClipIndex, false)
        }
        statusBanner.show("↪ 已重做: " + nextSnapshot.action)
    }

    function purgeInvalidClips() {
        pushUndoState("一键清理失效镜头")
        var remaining = []
        for (var i = 0; i < root.timelineClips.length; i++) {
            var c = root.timelineClips[i]
            if (!root.missingPathsMap[c.src]) {
                remaining.push(c)
            }
        }
        var removedCount = root.timelineClips.length - remaining.length
        root.timelineClips = remaining
        root.activeClipIndex = Math.min(root.activeClipIndex, root.timelineClips.length - 1)
        if (root.activeClipIndex >= 0) {
            root.loadActiveClip(root.activeClipIndex, false)
        } else {
            player.stop()
        }
        reconciliationDialog.close()
        statusBanner.show("已清理 " + removedCount + " 个源素材丢失的失效片段")
    }

    // 快捷计算属性
    property real totalMovieDuration: {
        var dur = 0.0
        for (var i = 0; i < timelineClips.length; i++) {
            dur += Number(timelineClips[i].dur || 0.0)
        }
        return dur
    }

    function formatTime(sec) {
        if (!sec || isNaN(sec) || sec < 0) return "00:00.0"
        var m = Math.floor(sec / 60)
        var s = Math.floor(sec % 60)
        var ms = Math.floor((sec % 1) * 10)
        return (m < 10 ? "0" + m : m) + ":" + (s < 10 ? "0" + s : s) + "." + ms
    }

    function formatDuration(sec) {
        if (!sec || isNaN(sec) || sec < 0) return "0秒"
        var m = Math.floor(sec / 60)
        var s = Math.floor(sec % 60)
        if (m === 0) return s + "秒"
        return m + "分" + (s < 10 ? "0" + s : s) + "秒"
    }

    function getFilteredMedia() {
        if (activeDate === "全部") return mediaPool
        var res = []
        for (var i = 0; i < mediaPool.length; i++) {
            if (mediaPool[i].date === activeDate) {
                res.push(mediaPool[i])
            }
        }
        return res
    }

    function toggleItemSelection(path) {
        var newMap = Object.assign({}, root.selectedPathsMap)
        if (newMap[path]) {
            delete newMap[path]
        } else {
            newMap[path] = true
        }
        root.selectedPathsMap = newMap
        root.recalcSelectionStats()
    }

    function selectAllInActiveFilter() {
        var filtered = root.getFilteredMedia()
        var newMap = Object.assign({}, root.selectedPathsMap)
        for (var i = 0; i < filtered.length; i++) {
            newMap[filtered[i].path] = true
        }
        root.selectedPathsMap = newMap
        root.recalcSelectionStats()
    }

    function clearAllInActiveFilter() {
        var filtered = root.getFilteredMedia()
        var newMap = Object.assign({}, root.selectedPathsMap)
        for (var i = 0; i < filtered.length; i++) {
            delete newMap[filtered[i].path]
        }
        root.selectedPathsMap = newMap
        root.recalcSelectionStats()
    }

    function invertSelectionInActiveFilter() {
        var filtered = root.getFilteredMedia()
        var newMap = Object.assign({}, root.selectedPathsMap)
        for (var i = 0; i < filtered.length; i++) {
            var p = filtered[i].path
            if (newMap[p]) delete newMap[p]
            else newMap[p] = true
        }
        root.selectedPathsMap = newMap
        root.recalcSelectionStats()
    }

    function recalcSelectionStats() {
        var count = 0
        var totalSec = 0
        for (var i = 0; i < root.mediaPool.length; i++) {
            var m = root.mediaPool[i]
            if (root.selectedPathsMap[m.path]) {
                count++
                totalSec += (m.duration || 0)
            }
        }
        root.selectedCount = count
        root.selectedTotalSeconds = totalSec
        var mins = Math.floor(totalSec / 60)
        var secs = Math.floor(totalSec % 60)
        root.selectedDurationStr = (mins < 10 ? "0" + mins : "" + mins) + ":" + (secs < 10 ? "0" + secs : "" + secs)
    }

    function getEffectiveTotalSeconds() {
        if (root.selectedTotalSeconds > 0) return root.selectedTotalSeconds
        var total = 0
        for (var i = 0; i < root.mediaPool.length; i++) {
            total += (root.mediaPool[i].duration || 0)
        }
        return total > 0 ? total : 0
    }

    function getSelectedCount() {
        return root.selectedCount
    }

    function getSelectedPathsList() {
        var list = []
        for (var i = 0; i < root.mediaPool.length; i++) {
            var p = root.mediaPool[i].path
            if (root.selectedPathsMap[p]) list.push(p)
        }
        return list
    }

    function getSelectedList() {
        var list = []
        for (var i = 0; i < root.mediaPool.length; i++) {
            if (root.selectedPathsMap[root.mediaPool[i].path]) {
                list.push(root.mediaPool[i])
            }
        }
        return list
    }

    function addSelectedToTimeline() {
        var selected = root.getSelectedList()
        if (selected.length === 0) {
            statusBanner.show("请先勾选需要添加到时间线的视频素材")
            return
        }
        pushUndoState("添加 " + selected.length + " 个素材至时间线")
        var newClips = root.timelineClips.slice()
        for (var i = 0; i < selected.length; i++) {
            var m = selected[i]
            newClips.push({
                "src": m.path,
                "filename": m.filename,
                "start": 0.0,
                "end": m.duration,
                "dur": m.duration,
                "dur_str": m.duration_str,
                "orient": m.orient,
                "is_10bit": m.is_10bit,
                "thumb": m.thumb,
                "transpose": m.transpose,
                "text": "",
                "subtitles": []
            })
        }
        root.timelineClips = newClips
        if (root.activeClipIndex < 0 && root.timelineClips.length > 0) {
            root.loadActiveClip(0, false)
        }
        statusBanner.show("已将选中的 " + selected.length + " 个素材添加至时间线")
    }

    function loadActiveClip(index, autoPlay) {
        if (index < 0 || index >= timelineClips.length) return
        activeClipIndex = index
        var clip = timelineClips[index]
        player.stop()
        player.source = "file://" + clip.src
        player.position = Math.floor(clip.start * 1000)
        if (autoPlay) {
            player.play()
        }
    }

    function stepActiveIn(delta) {
        if (activeClipIndex < 0 || activeClipIndex >= timelineClips.length) return
        pushUndoState("调整镜头入点")
        var clip = Object.assign({}, timelineClips[activeClipIndex])
        var newStart = Math.max(0.0, clip.start + delta)
        if (newStart < clip.end - 0.5) {
            clip.start = Number(newStart.toFixed(2))
            clip.dur = Number((clip.end - clip.start).toFixed(2))
            clip.dur_str = clip.dur + "s"
            var list = timelineClips.slice()
            list[activeClipIndex] = clip
            timelineClips = list
            player.position = Math.floor(clip.start * 1000)
        }
    }

    function stepActiveOut(delta) {
        if (activeClipIndex < 0 || activeClipIndex >= timelineClips.length) return
        pushUndoState("调整镜头出点")
        var clip = Object.assign({}, timelineClips[activeClipIndex])
        var newEnd = Math.max(clip.start + 0.5, clip.end + delta)
        clip.end = Number(newEnd.toFixed(2))
        clip.dur = Number((clip.end - clip.start).toFixed(2))
        clip.dur_str = clip.dur + "s"
        var list = timelineClips.slice()
        list[activeClipIndex] = clip
        timelineClips = list
        player.position = Math.floor(Math.max(clip.start, clip.end - 1.0) * 1000)
    }

    function splitCurrentClip() {
        if (activeClipIndex < 0 || activeClipIndex >= timelineClips.length) return
        var curPos = player.position / 1000.0
        var clip = timelineClips[activeClipIndex]
        if (curPos <= clip.start + 0.5 || curPos >= clip.end - 0.5) {
            statusBanner.show("分割点距离片段边缘太近，无法分割")
            return
        }
        pushUndoState("分割镜头")
        var c1 = Object.assign({}, clip, {
            end: Number(curPos.toFixed(2)),
            dur: Number((curPos - clip.start).toFixed(2)),
            dur_str: (curPos - clip.start).toFixed(1) + "s"
        })
        var c2 = Object.assign({}, clip, {
            start: Number(curPos.toFixed(2)),
            dur: Number((clip.end - curPos).toFixed(2)),
            dur_str: (clip.end - curPos).toFixed(1) + "s"
        })
        var list = timelineClips.slice()
        list.splice(activeClipIndex, 1, c1, c2)
        timelineClips = list
        statusBanner.show("已在当前播放位置成功一分为二")
    }

    function duplicateCurrentClip() {
        if (activeClipIndex < 0 || activeClipIndex >= timelineClips.length) return
        pushUndoState("复制镜头")
        var clip = Object.assign({}, timelineClips[activeClipIndex])
        var list = timelineClips.slice()
        list.splice(activeClipIndex + 1, 0, clip)
        timelineClips = list
        activeClipIndex = activeClipIndex + 1
        statusBanner.show("已复制当前片段并插入后方")
    }

    function deleteCurrentClip() {
        if (activeClipIndex < 0 || activeClipIndex >= timelineClips.length) return
        pushUndoState("删除镜头")
        var list = timelineClips.slice()
        list.splice(activeClipIndex, 1)
        timelineClips = list
        if (timelineClips.length > 0) {
            activeClipIndex = Math.min(activeClipIndex, timelineClips.length - 1)
            loadActiveClip(activeClipIndex, false)
        } else {
            player.stop()
        }
        statusBanner.show("已从时间线删除该片段")
    }

    function moveCurrentClip(delta) {
        var target = activeClipIndex + delta
        if (target < 0 || target >= timelineClips.length) return
        pushUndoState("移动镜头顺序")
        var list = timelineClips.slice()
        var item = list.splice(activeClipIndex, 1)[0]
        list.splice(target, 0, item)
        timelineClips = list
        activeClipIndex = target
    }

    // 硬件加速播放器核心
    MediaPlayer {
        id: player
        videoOutput: root.isMobileMode ? mobileMonitorOutput : desktopMonitorOutput
        audioOutput: AudioOutput {}

        onPositionChanged: {
            if (activeClipIndex < 0 || activeClipIndex >= timelineClips.length) {
                root.currentSubtitleText = ""
                return
            }
            var curClip = timelineClips[activeClipIndex]
            var posSec = player.position / 1000.0

            // 实时口播字幕匹配
            if (curClip.subtitles && curClip.subtitles.length > 0) {
                var foundSub = false
                for (var s = 0; s < curClip.subtitles.length; s++) {
                    var sub = curClip.subtitles[s]
                    if (posSec >= sub.start && posSec <= sub.end) {
                        root.currentSubtitleText = sub.text
                        foundSub = true
                        break
                    }
                }
                if (!foundSub) root.currentSubtitleText = ""
            } else if (curClip.text) {
                root.currentSubtitleText = curClip.text
            } else {
                root.currentSubtitleText = ""
            }

            if (posSec >= curClip.end) {
                if (root.isContinuousPlay) {
                    if (activeClipIndex + 1 < timelineClips.length) {
                        root.loadActiveClip(activeClipIndex + 1, true)
                    } else {
                        player.pause()
                        player.position = Math.floor(curClip.start * 1000)
                    }
                } else {
                    player.position = Math.floor(curClip.start * 1000)
                }
            } else if (posSec < curClip.start) {
                player.position = Math.floor(curClip.start * 1000)
            }
        }
    }

    // 后端信号桥接
    Connections {
        target: bridge

        function onImportProgress(current, total, filename) {
            root.isImporting = true
            root.importCurrent = current
            root.importTotal = total
            root.importFilename = filename
        }

        function onMediaLoaded(dates, list, sourceLabel) {
            root.isImporting = false
            root.mediaPool = list
            root.currentSourceLabel = sourceLabel || "已加载"
            var d = ["全部"]
            for (var i = 0; i < dates.length; i++) d.push(dates[i])
            root.datesList = d
            root.activeDate = "全部"

            var initSel = {}
            for (var j = 0; j < list.length; j++) {
                initSel[list[j].path] = true
            }
            root.selectedPathsMap = initSel
            root.recalcSelectionStats()

            // 如果已有素材且时间线为空，自动加载第一个预览
            if (list.length > 0 && root.timelineClips.length === 0) {
                player.stop()
                player.source = "file://" + list[0].path
                player.position = 0
            }
            statusBanner.show("成功载入 " + list.length + " 个素材 (" + root.currentSourceLabel + ")")
        }

        function onDjiFound(path) {
            root.detectedDjiPath = path
            if (path && root.mediaPool.length === 0) {
                statusBanner.show("检测到相机外设已接入: " + path)
            }
        }

        function onRecentDirsLoaded(dirs) {
            root.recentDirs = dirs
        }

        function onStatusMessage(msg) {
            statusBanner.show(msg)
        }

        function onScanError(err) {
            root.isImporting = false
            statusBanner.show("提示: " + err)
        }

        function onUsbDeviceDetected(devName, mediaDir, fileCount) {
            if (root.mediaPool.length === 0) {
                statusBanner.show("已自动识别并载入相机外设 [" + devName + "] (" + fileCount + " 个视频)")
            } else {
                root.pendingUsbDevice = { name: devName, dir: mediaDir, count: fileCount }
                usbNewDeviceDialog.open()
            }
        }

        function onUsbDeviceRemoved(devName) {
            statusBanner.show("设备 [" + devName + "] 已从系统移除")
        }

        function onAiProgress(percent, stepText) {
            aiModal.progress = percent
            aiModal.statusText = stepText
        }

        function onAiFinished(tasks) {
            aiModal.close()
            root.pushUndoState("应用 AI 智能初筛结果")
            root.timelineClips = tasks
            if (tasks.length > 0) {
                root.loadActiveClip(0, false)
                statusBanner.show("AI 自动筛选完成，共剪辑出 " + tasks.length + " 段精彩镜头！")
            } else {
                statusBanner.show("未筛选到片段，请检查筛选参数")
            }
        }

        function onAiPaused(completed, total, reason) {
            aiModal.statusText = "任务已安全暂停 (" + reason + ") · 进度已留存 (" + completed + "/" + total + ")"
            statusBanner.show("初筛任务已安全暂存检查点，插回相机即可继续")
        }

        function onAiResumed() {
            statusBanner.show("已从断点继续执行 AI 任务")
        }

        function onDeviceDisconnected(devName) {
            root.isDeviceDisconnected = true
            root.disconnectedDeviceName = devName
            statusBanner.show("存储设备 [" + devName + "] 已断开（拔出录像中）· 任务已安全挂起")
            if (player.playbackState === MediaPlayer.PlayingState) player.pause()
        }

        function onDeviceReconnected(devName, diff) {
            root.isDeviceDisconnected = false
            root.lastDiffReport = diff
            if (diff.missing && diff.missing.length > 0) {
                var missingMap = {}
                for (var i = 0; i < diff.missing.length; i++) {
                    missingMap[diff.missing[i].path] = true
                }
                root.missingPathsMap = missingMap
                reconciliationDialog.open()
            } else if (diff.new_count > 0) {
                statusBanner.show("设备已重新连接！相机内发现 " + diff.new_count + " 个新视频（未打乱当前工程）")
            } else {
                statusBanner.show("设备已重新连接，全部 " + root.mediaPool.length + " 个素材完好在线！")
            }
        }

        function onExportProgress(percent, stepText) {
            exportModal.progress = percent
            exportModal.statusText = stepText
        }

        function onExportFinished(outPath) {
            exportModal.close()
            statusBanner.show("导出成功: " + outPath)
        }

        function onExportFailed(err) {
            exportModal.close()
            statusBanner.show("导出失败: " + err)
        }

        function onSpeechSegmentsReady(videoPath, segments) {
            var annots = root.speechAnnotations
            annots[videoPath] = segments
            root.speechAnnotations = annots
            statusBanner.show("口播识别完成: " + segments.length + " 个词段 (" + videoPath.split("/").pop() + ")")
        }
    }

    Component.onCompleted: {
        bridge.loadRecentDirs()
        bridge.detectDji()
    }

    // 快捷键
    Shortcut {
        sequence: "Space"
        onActivated: {
            if (player.playbackState === MediaPlayer.PlayingState) player.pause()
            else player.play()
        }
    }
    Shortcut { sequence: "Left"; onActivated: player.position = Math.max(0, player.position - 1000) }
    Shortcut { sequence: "Right"; onActivated: player.position = player.position + 1000 }
    Shortcut { sequence: "B"; onActivated: root.splitCurrentClip() }
    Shortcut { sequence: "Delete"; onActivated: root.deleteCurrentClip() }
    Shortcut { sequence: "Ctrl+Z"; onActivated: root.undo() }
    Shortcut { sequence: "Ctrl+Shift+Z"; onActivated: root.redo() }
    Shortcut { sequence: "Ctrl+Y"; onActivated: root.redo() }

    // ==========================================
    // 全局拖拽支持 (支持直接从 Finder / 资源管理器拖入)
    // ==========================================
    DropArea {
        anchors.fill: parent
        onEntered: (drag) => {
            if (drag.hasUrls) {
                drag.accept()
                root.isDragActive = true
            }
        }
        onExited: {
            root.isDragActive = false
        }
        onDropped: (drop) => {
            root.isDragActive = false
            if (drop.hasUrls) {
                var paths = []
                for (var i = 0; i < drop.urls.length; i++) {
                    var u = drop.urls[i].toString()
                    if (u.indexOf("file://") === 0) u = decodeURIComponent(u.substring(7))
                    paths.push(u)
                }
                bridge.importPaths(paths)
            }
        }
    }

    // 拖拽高亮浮层蒙层
    Rectangle {
        anchors.fill: parent
        color: "#e60b1120"
        border.color: "#38bdf8"
        border.width: 3
        visible: root.isDragActive
        z: 9999

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 14

            Text { Layout.alignment: Qt.AlignHCenter; text: "+"; font.pixelSize: 54 }
            Text { Layout.alignment: Qt.AlignHCenter; text: "松开鼠标即可瞬间导入视频素材"; color: "#38bdf8"; font.bold: true; font.pixelSize: 20 }
            Text { Layout.alignment: Qt.AlignHCenter; text: "支持拖入单个/多个 .MP4 / .MOV 视频文件，或直接拖入整个文件夹"; color: "#94a3b8"; font.pixelSize: 13 }
        }
    }

    // ==========================================
    // 主界面视图
    // ==========================================
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // 1. 顶部全局导航栏
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.isMobileMode ? 48 : 52
            color: "#14161c"
            border.color: "#222530"
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: root.isMobileMode ? 10 : 16
                anchors.rightMargin: root.isMobileMode ? 10 : 16
                spacing: root.isMobileMode ? 8 : 10

                Text {
                    text: root.isMobileMode ? "随剪" : "随剪"
                    color: "#ffffff"
                    font.bold: true
                    font.pixelSize: root.isMobileMode ? 14 : 16
                }

                Item { Layout.fillWidth: true }

                // 成片概览 Pill
                Rectangle {
                    color: "#1a1f2c"
                    border.color: "#38bdf8"
                    border.width: 1
                    radius: 13
                    Layout.preferredHeight: 26
                    Layout.preferredWidth: root.isMobileMode ? 130 : 250

                    RowLayout {
                        anchors.centerIn: parent
                        spacing: 4
                        Text {
                            text: root.isMobileMode
                                  ? ("成片总长: " + root.formatDuration(root.totalMovieDuration))
                                  : ("成片总长: " + root.formatDuration(root.totalMovieDuration) + " (" + root.timelineClips.length + " 段)")
                            color: "#f1f5f9"
                            font.pixelSize: 11
                            font.bold: true
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                // AI 智能初筛按钮
                Button {
                    text: "AI 智能初筛"
                    onClicked: aiModal.open()
                    background: Rectangle { color: parent.hovered ? "#4338ca" : "#3730a3"; radius: 5 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }

                // 导出大按钮
                Button {
                    text: root.isMobileMode ? "导出" : "导出 4K60 10-Bit 母带"
                    onClicked: exportModal.open()
                    background: Rectangle {
                        radius: 5
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: "#2563eb" }
                            GradientStop { position: 1.0; color: "#9333ea" }
                        }
                    }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
            }
        }

        // 设备断开 / 误拔防护横幅 (自动存盘并在重连后引导恢复)
        Rectangle {
            visible: root.isDeviceDisconnected
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            color: "#450a0a"
            border.color: "#ef4444"
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 10

                Text {
                    text: "存储设备 [" + (root.disconnectedDeviceName || "相机/SD卡") + "] 已断开（拔出/录像中）· 时间线与初筛断点已安全保护留存，插回即可一键恢复"
                    color: "#fecaca"
                    font.bold: true
                    font.pixelSize: 12
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "检测设备重连"
                    onClicked: bridge.detectDji()
                    background: Rectangle { color: "#991b1b"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
            }
        }

        // 2. 核心工作台视图分支：
        //    分支 A: 移动端布局 (isMobileMode == true)
        //    分支 B: 桌面端三栏布局 (isMobileMode == false)

        // ------------------------------------------
        // 分支 A: 移动端专业流体布局
        // ------------------------------------------
        ColumnLayout {
            visible: root.isMobileMode
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // 移动端监视器视窗
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.floor(root.height * 0.38)
                color: "#000000"
                clip: true

                VideoOutput {
                    id: mobileMonitorOutput
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectFit
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        if (player.playbackState === MediaPlayer.PlayingState) player.pause()
                        else player.play()
                    }
                }

                RowLayout {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 8
                    spacing: 6

                    Rectangle {
                        color: "#cc0f172a"; radius: 4; width: 54; height: 22
                        Text { anchors.centerIn: parent; text: "10-Bit"; color: "#a855f7"; font.bold: true; font.pixelSize: 10 }
                    }

                    Button {
                        text: "⛶"
                        Layout.preferredWidth: 28; Layout.preferredHeight: 28
                        onClicked: {
                            if (root.visibility === Window.FullScreen) root.visibility = Window.AutomaticVisibility
                            else root.visibility = Window.FullScreen
                        }
                        background: Rectangle { color: "#cc0f172a"; radius: 4 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }
                }

                // 移动端实时字幕悬浮展示
                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottomMargin: 10
                    color: "#d90b1120"
                    border.color: "#38bdf8"
                    border.width: 1
                    radius: 5
                    visible: root.currentSubtitleText.length > 0
                    width: Math.min(parent.width - 24, mobileSubText.implicitWidth + 20)
                    height: mobileSubText.implicitHeight + 10

                    Text {
                        id: mobileSubText
                        anchors.centerIn: parent
                        text: root.currentSubtitleText
                        color: "#ffffff"
                        font.pixelSize: 12
                        font.bold: true
                        elide: Text.ElideRight
                    }
                }
            }

            // 移动端紧凑播放控制器栏
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 42
                color: "#11131a"
                border.color: "#1e222d"
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10; anchors.rightMargin: 10
                    spacing: 8

                    Button {
                        text: "-1s"; Layout.preferredWidth: 38; Layout.preferredHeight: 28
                        onClicked: player.position = Math.max(0, player.position - 1000)
                        background: Rectangle { color: "#222533"; radius: 4 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }

                    Button {
                        text: player.playbackState === MediaPlayer.PlayingState ? "⏸" : "▶"
                        Layout.preferredWidth: 44; Layout.preferredHeight: 28
                        onClicked: {
                            if (player.playbackState === MediaPlayer.PlayingState) player.pause()
                            else player.play()
                        }
                        background: Rectangle { color: "#3b82f6"; radius: 4 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }

                    Button {
                        text: "+1s"; Layout.preferredWidth: 38; Layout.preferredHeight: 28
                        onClicked: player.position = player.position + 1000
                        background: Rectangle { color: "#222533"; radius: 4 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }

                    Text {
                        text: root.formatTime(player.position / 1000.0) + " / " + root.formatTime(root.totalMovieDuration)
                        color: "#38bdf8"; font.bold: true; font.pixelSize: 11
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: root.isContinuousPlay ? "连续" : "单段"
                        Layout.preferredHeight: 28
                        onClicked: root.isContinuousPlay = !root.isContinuousPlay
                        background: Rectangle { color: root.isContinuousPlay ? "#1d4ed8" : "#334155"; radius: 4 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }
                }
            }

            // 移动端中间切换内容区
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // Tab 0: 移动端剪辑与时间线微调
                ColumnLayout {
                    anchors.fill: parent
                    visible: root.mobileTab === 0
                    spacing: 6

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 115
                        color: "#0f1016"

                        ListView {
                            id: mobileTimelineView
                            anchors.fill: parent
                            anchors.margins: 6
                            orientation: ListView.Horizontal
                            clip: true
                            spacing: 8
                            model: root.timelineClips

                            delegate: Rectangle {
                                readonly property bool isInvalidClip: root.missingPathsMap[modelData.src] === true
                                width: Math.max(85, Math.min(200, Number(modelData.dur || 5) * 8))
                                height: 100
                                radius: 6; clip: true
                                color: isInvalidClip ? "#450a0a" : (root.activeClipIndex === index ? "#1e293b" : "#141620")
                                border.color: isInvalidClip ? "#ef4444" : (root.activeClipIndex === index ? "#38bdf8" : "#2d3345")
                                border.width: (isInvalidClip || root.activeClipIndex === index) ? 2 : 1

                                Image { anchors.fill: parent; source: modelData.thumb; fillMode: Image.PreserveAspectCrop; opacity: isInvalidClip ? 0.3 : 0.65 }

                                Rectangle {
                                    visible: isInvalidClip
                                    anchors.fill: parent
                                    color: "#99450a0a"
                                    ColumnLayout {
                                        anchors.centerIn: parent
                                        spacing: 2
                                        Text { text: "[离线]"; color: "#ef4444"; font.bold: true; font.pixelSize: 12; Layout.alignment: Qt.AlignHCenter }
                                        Text { text: "原素材已失效"; color: "#fca5a5"; font.bold: true; font.pixelSize: 9; Layout.alignment: Qt.AlignHCenter }
                                    }
                                }

                                Rectangle {
                                    anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                                    height: 20; color: "#cc0f172a"
                                    RowLayout {
                                        anchors.fill: parent; anchors.margins: 4
                                        Text { text: "#" + (index + 1); color: "#ffffff"; font.bold: true; font.pixelSize: 10 }
                                        Item { Layout.fillWidth: true }
                                        Text { text: modelData.dur_str; color: "#38bdf8"; font.bold: true; font.pixelSize: 10 }
                                    }
                                }

                                Rectangle {
                                    anchors.bottom: parent.bottom; anchors.bottomMargin: 4
                                    anchors.left: parent.left; anchors.right: parent.right
                                    anchors.margins: 4
                                    height: 16; radius: 3; color: "#e60f172a"
                                    visible: Boolean(modelData.text && modelData.text.length > 0)
                                    Text {
                                        anchors.fill: parent; anchors.margins: 2
                                        text: modelData.text || ""
                                        color: "#f8fafc"; font.pixelSize: 8
                                        elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter
                                    }
                                }

                                MouseArea { anchors.fill: parent; onClicked: root.loadActiveClip(index, false) }
                            }
                        }
                    }

                    // 剪辑快捷操作栏 (大拇指触控区)
                    RowLayout {
                        Layout.fillWidth: true; Layout.preferredHeight: 36
                        Layout.leftMargin: 8; Layout.rightMargin: 8
                        spacing: 6

                        Button {
                            text: "↩ 撤销"; Layout.fillWidth: true; Layout.preferredHeight: 36
                            enabled: root.canUndo; opacity: root.canUndo ? 1.0 : 0.35
                            onClicked: root.undo()
                            background: Rectangle { color: root.canUndo ? "#1e293b" : "#12131a"; radius: 6; border.color: root.canUndo ? "#475569" : "#1f2430" }
                            contentItem: Text { text: parent.text; color: root.canUndo ? "#ffffff" : "#64748b"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                        Button {
                            text: "↪ 重做"; Layout.fillWidth: true; Layout.preferredHeight: 36
                            enabled: root.canRedo; opacity: root.canRedo ? 1.0 : 0.35
                            onClicked: root.redo()
                            background: Rectangle { color: root.canRedo ? "#1e293b" : "#12131a"; radius: 6; border.color: root.canRedo ? "#475569" : "#1f2430" }
                            contentItem: Text { text: parent.text; color: root.canRedo ? "#ffffff" : "#64748b"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                        Button {
                            text: "分割"; Layout.fillWidth: true; Layout.preferredHeight: 36
                            onClicked: root.splitCurrentClip()
                            background: Rectangle { color: "#854d0e"; radius: 6 }
                            contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                        Button {
                            text: "复制"; Layout.fillWidth: true; Layout.preferredHeight: 36
                            onClicked: root.duplicateCurrentClip()
                            background: Rectangle { color: "#1e3a8a"; radius: 6 }
                            contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                        Button {
                            text: "删除"; Layout.fillWidth: true; Layout.preferredHeight: 36
                            onClicked: root.deleteCurrentClip()
                            background: Rectangle { color: "#7f1d1d"; radius: 6 }
                            contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                        Button {
                            text: "◀ 前移"; Layout.fillWidth: true; Layout.preferredHeight: 36
                            onClicked: root.moveCurrentClip(-1)
                            background: Rectangle { color: "#1e2230"; radius: 6 }
                            contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                        Button {
                            text: "后移 ▶"; Layout.fillWidth: true; Layout.preferredHeight: 36
                            onClicked: root.moveCurrentClip(1)
                            background: Rectangle { color: "#1e2230"; radius: 6 }
                            contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                    }

                    // 触控微调面板
                    Rectangle {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        Layout.margins: 8; color: "#14151d"; radius: 8; border.color: "#222533"

                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 8; spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "起点 In:"; color: "#cbd5e1"; font.pixelSize: 11; font.bold: true }
                                Text { text: root.timelineClips.length > root.activeClipIndex ? root.formatTime(root.timelineClips[root.activeClipIndex].start) : "00:00"; color: "#38bdf8"; font.bold: true; font.pixelSize: 12 }
                                Item { Layout.fillWidth: true }
                                Button { text: "-2s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30; onClicked: root.stepActiveIn(-2.0); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                                Button { text: "-0.5s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30; onClicked: root.stepActiveIn(-0.5); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                                Button { text: "+0.5s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30; onClicked: root.stepActiveIn(0.5); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                                Button { text: "+2s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30; onClicked: root.stepActiveIn(2.0); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "终点 Out:"; color: "#cbd5e1"; font.pixelSize: 11; font.bold: true }
                                Text { text: root.timelineClips.length > root.activeClipIndex ? root.formatTime(root.timelineClips[root.activeClipIndex].end) : "00:00"; color: "#38bdf8"; font.bold: true; font.pixelSize: 12 }
                                Item { Layout.fillWidth: true }
                                Button { text: "-2s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30; onClicked: root.stepActiveOut(-2.0); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                                Button { text: "-0.5s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30; onClicked: root.stepActiveOut(-0.5); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                                Button { text: "+0.5s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30; onClicked: root.stepActiveOut(0.5); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                                Button { text: "+2s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30; onClicked: root.stepActiveOut(2.0); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "镜头: " + (root.timelineClips.length > root.activeClipIndex ? root.timelineClips[root.activeClipIndex].filename : "无"); color: "#94a3b8"; font.pixelSize: 10; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                Text { text: "截取: " + (root.timelineClips.length > root.activeClipIndex ? root.timelineClips[root.activeClipIndex].dur_str : "0s"); color: "#60a5fa"; font.bold: true; font.pixelSize: 11 }
                            }
                        }
                    }
                }

                // Tab 1: 移动端素材池
                ColumnLayout {
                    anchors.fill: parent
                    visible: root.mobileTab === 1
                    spacing: 4

                    // 移动端素材池顶栏 (仅在有素材时展示；空态时不重复堆叠按键)
                    Rectangle {
                        visible: root.mediaPool.length > 0
                        Layout.fillWidth: true; Layout.preferredHeight: 38
                        color: "#161822"
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10
                            Text {
                                text: "素材池 (" + root.mediaPool.length + ")"
                                color: "#ffffff"; font.bold: true; font.pixelSize: 12
                            }
                            Item { Layout.fillWidth: true }
                            Button {
                                text: "+ 导入 ▾"
                                Layout.preferredHeight: 24
                                onClicked: mobileImportMenu.open()
                                background: Rectangle { color: "#1e293b"; radius: 4 }
                                contentItem: Text { text: parent.text; color: "#93c5fd"; font.pixelSize: 11; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }

                                Menu {
                                    id: mobileImportMenu
                                    MenuItem { text: "导入视频文件夹..."; onTriggered: folderDialog.open() }
                                    MenuItem { text: "选取视频文件 (可多选)..."; onTriggered: fileDialog.open() }
                                    MenuItem { text: "重新检测相机卡"; onTriggered: bridge.detectDji() }
                                }
                            }
                        }
                    }

                    // 空态大卡片 (素材池为空时显示)
                    Rectangle {
                        visible: root.mediaPool.length === 0
                        Layout.fillWidth: true; Layout.fillHeight: true
                        color: "#12131a"

                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 12
                            width: Math.min(360, parent.width - 24)

                            Text { Layout.alignment: Qt.AlignHCenter; text: "[素材库为空]"; color: "#64748b"; font.pixelSize: 13 }
                            Text { Layout.alignment: Qt.AlignHCenter; text: "素材库为空"; color: "#f8fafc"; font.bold: true; font.pixelSize: 15 }
                            Text { Layout.alignment: Qt.AlignHCenter; text: "请选择包含视频的目录，或直接选取视频文件"; color: "#94a3b8"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter }

                            Button {
                                text: "选择视频文件夹 (整个目录)"
                                Layout.fillWidth: true; Layout.preferredHeight: 40
                                onClicked: folderDialog.open()
                                background: Rectangle { color: "#2563eb"; radius: 6 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Button {
                                text: "选择特定视频文件 (可多选)"
                                Layout.fillWidth: true; Layout.preferredHeight: 40
                                onClicked: fileDialog.open()
                                background: Rectangle { color: "#1e293b"; radius: 6; border.color: "#334155" }
                                contentItem: Text { text: parent.text; color: "#e2e8f0"; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            // 相机检测提示
                            Button {
                                visible: root.detectedDjiPath !== ""
                                text: "检测到相机已接入 [一键载入]"
                                Layout.fillWidth: true; Layout.preferredHeight: 40
                                onClicked: bridge.scanDirectory(root.detectedDjiPath)
                                background: Rectangle { color: "#065f46"; radius: 6; border.color: "#10b981" }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }
                        }
                    }

                    // 日期过滤条 (有素材时显示)
                    Rectangle {
                        visible: root.mediaPool.length > 0
                        Layout.fillWidth: true; Layout.preferredHeight: 34
                        color: "#161822"
                        Flickable {
                            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                            contentWidth: mobDateRow.implicitWidth; boundsBehavior: Flickable.StopAtBounds
                            RowLayout {
                                id: mobDateRow; spacing: 6
                                Repeater {
                                    model: root.datesList
                                    delegate: Rectangle {
                                        Layout.preferredHeight: 24; Layout.preferredWidth: Math.max(58, mobDtText.implicitWidth + 12); radius: 4
                                        color: root.activeDate === modelData ? "#2563eb" : "#222533"
                                        Text {
                                            id: mobDtText
                                            anchors.centerIn: parent
                                            text: modelData + (modelData === "全部" ? " (" + root.mediaPool.length + ")" : "")
                                            color: "#ffffff"; font.pixelSize: 10; font.bold: root.activeDate === modelData
                                        }
                                        MouseArea { anchors.fill: parent; onClicked: root.activeDate = modelData }
                                    }
                                }
                            }
                        }
                    }

                    // 移动端快捷选择栏
                    Rectangle {
                        visible: root.mediaPool.length > 0
                        Layout.fillWidth: true; Layout.preferredHeight: 32
                        color: "#181a24"; border.color: "#1e2230"; border.width: 1

                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 6
                            Button {
                                text: "全选"
                                Layout.preferredHeight: 22
                                onClicked: root.selectAllInActiveFilter()
                                background: Rectangle { color: parent.hovered ? "#3b82f6" : "#272a38"; radius: 3 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }
                            Button {
                                text: "反选"
                                Layout.preferredHeight: 22
                                onClicked: root.invertSelectionInActiveFilter()
                                background: Rectangle { color: parent.hovered ? "#334155" : "#272a38"; radius: 3 }
                                contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 10; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }
                            Button {
                                text: "清空"
                                Layout.preferredHeight: 22
                                onClicked: root.clearAllInActiveFilter()
                                background: Rectangle { color: parent.hovered ? "#450a0a" : "#272a38"; radius: 3 }
                                contentItem: Text { text: parent.text; color: "#fca5a5"; font.pixelSize: 10; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: "已选 " + root.selectedCount + " / " + root.getFilteredMedia().length
                                color: root.selectedCount > 0 ? "#93c5fd" : "#64748b"
                                font.pixelSize: 10; font.bold: true
                            }
                        }
                    }

                    // 素材列表 (有素材时显示)
                    ListView {
                        id: mobileMediaList
                        visible: root.mediaPool.length > 0
                        Layout.fillWidth: true; Layout.fillHeight: true
                        clip: true; spacing: 4; model: root.getFilteredMedia()

                        delegate: Rectangle {
                            width: mobileMediaList.width; height: 64; radius: 5
                            color: root.selectedPathsMap[modelData.path] ? "#172033" : (mobRowMouse.containsMouse ? "#181a24" : "#14151d")
                            border.color: root.selectedPathsMap[modelData.path] ? "#3b82f6" : (mobRowMouse.containsMouse ? "#475569" : "#1f222e")
                            border.width: root.selectedPathsMap[modelData.path] ? 2 : 1

                            RowLayout {
                                anchors.fill: parent; anchors.margins: 6; spacing: 8

                                // 圆形勾选指示器
                                Rectangle {
                                    Layout.preferredWidth: 22; Layout.preferredHeight: 22; radius: 11
                                    color: root.selectedPathsMap[modelData.path] ? "#2563eb" : "#80000000"
                                    border.color: root.selectedPathsMap[modelData.path] ? "#ffffff" : "#94a3b8"
                                    border.width: 1.5

                                    Text {
                                        anchors.centerIn: parent
                                        text: "✓"
                                        visible: !!root.selectedPathsMap[modelData.path]
                                        color: "#ffffff"; font.bold: true; font.pixelSize: 13
                                    }
                                }

                                Rectangle {
                                    Layout.preferredWidth: 74; Layout.fillHeight: true; radius: 4; clip: true; color: "#000000"
                                    Image { anchors.fill: parent; source: modelData.thumb; fillMode: Image.PreserveAspectCrop }
                                    Rectangle {
                                        anchors.centerIn: parent; width: 22; height: 22; radius: 11
                                        color: "#b3000000"; border.color: "#38bdf8"; border.width: 1
                                        visible: mobRowMouse.containsMouse
                                        Text { anchors.centerIn: parent; text: "▶"; color: "#38bdf8"; font.pixelSize: 9 }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 2
                                    Text { text: modelData.filename; color: "#ffffff"; font.pixelSize: 11; font.bold: true; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                    RowLayout {
                                        spacing: 4
                                        Text { text: modelData.duration_str; color: "#94a3b8"; font.pixelSize: 10 }
                                        Rectangle {
                                            color: modelData.is_10bit ? "#8b5cf6" : "#334155"; radius: 2
                                            Layout.preferredHeight: 14; Layout.preferredWidth: modelData.is_10bit ? 58 : 36
                                            Text { anchors.centerIn: parent; text: modelData.is_10bit ? "10-Bit D-Log" : "8-Bit"; color: "#ffffff"; font.pixelSize: 9; font.bold: true }
                                        }
                                    }
                                }
                            }

                            MouseArea {
                                id: mobRowMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: root.toggleItemSelection(modelData.path)
                                onDoubleClicked: {
                                    player.stop()
                                    player.source = "file://" + modelData.path
                                    player.position = 0
                                    player.play()
                                }
                            }
                        }
                    }

                    // 移动端底部操作条
                    Rectangle {
                        visible: root.mediaPool.length > 0
                        Layout.fillWidth: true; Layout.preferredHeight: 44
                        color: "#161822"; border.color: "#1f222e"; border.width: 1

                        RowLayout {
                            anchors.fill: parent; anchors.margins: 6; spacing: 6

                            Button {
                                Layout.fillWidth: true; Layout.preferredHeight: 32
                                text: "+ 加到时间线"
                                enabled: root.selectedCount > 0
                                onClicked: root.addSelectedToTimeline()
                                background: Rectangle {
                                    color: root.selectedCount > 0 ? "#0f172a" : "#161922"
                                    border.color: root.selectedCount > 0 ? "#38bdf8" : "#334155"
                                    radius: 4
                                }
                                contentItem: Text {
                                    text: parent.text; color: root.selectedCount > 0 ? "#38bdf8" : "#64748b"
                                    font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                }
                            }

                            Button {
                                Layout.fillWidth: true; Layout.preferredHeight: 32
                                text: "AI 初筛 (" + root.selectedCount + ")"
                                enabled: root.selectedCount > 0
                                onClicked: aiModal.open()
                                background: Rectangle {
                                    color: root.selectedCount > 0 ? "#2563eb" : "#334155"; radius: 4
                                }
                                contentItem: Text {
                                    text: parent.text; color: root.selectedCount > 0 ? "#ffffff" : "#64748b"
                                    font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }
                    }
                }

                // Tab 2: 移动端 AI 智能初筛快速操作
                ColumnLayout {
                    anchors.fill: parent
                    visible: root.mobileTab === 2
                    anchors.margins: 14
                    spacing: 12

                    Text { text: "AI 智能剪辑策略："; color: "#ffffff"; font.bold: true; font.pixelSize: 13 }

                    Button {
                        text: "口播智能粗剪 (废句消除 / 连贯留声 / 生成字幕)"
                        Layout.fillWidth: true; Layout.preferredHeight: 44
                        onClicked: {
                            bridge.startAiScreening(root.mediaPool, root.getSelectedPathsList(), "4", 35.0, "horizontal", "10bit")
                            root.mobileTab = 0
                        }
                        background: Rectangle { color: "#065f46"; radius: 6; border.color: "#10b981" }
                        contentItem: Text { text: parent.text; color: "#6ee7b7"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }

                    Button {
                        text: "视觉审美优先 (保留优质镜头/过滤抖动)"
                        Layout.fillWidth: true; Layout.preferredHeight: 44
                        onClicked: {
                            bridge.startAiScreening(root.mediaPool, root.getSelectedPathsList(), "1", 35.0, "horizontal", "10bit")
                            root.mobileTab = 0
                        }
                        background: Rectangle { color: "#312e81"; radius: 6; border.color: "#4338ca" }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }

                    Button {
                        text: "语音静音切分 (无声剔除 / 智能留声)"
                        Layout.fillWidth: true; Layout.preferredHeight: 44
                        onClicked: {
                            bridge.startAiScreening(root.mediaPool, root.getSelectedPathsList(), "2", 35.0, "horizontal", "10bit")
                            root.mobileTab = 0
                        }
                        background: Rectangle { color: "#1e293b"; radius: 6; border.color: "#334155" }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }

                    Button {
                        text: "深度多模态综合混合 (多模态高光精选)"
                        Layout.fillWidth: true; Layout.preferredHeight: 44
                        onClicked: {
                            bridge.startAiScreening(root.mediaPool, root.getSelectedPathsList(), "3", 35.0, "horizontal", "10bit")
                            root.mobileTab = 0
                        }
                        background: Rectangle { color: "#1e3a8a"; radius: 6; border.color: "#2563eb" }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }

                    Item { Layout.fillHeight: true }

                    Text {
                        text: "提示：点击后将在后台全速进行高光初筛并自动将成片镜头排列至时间线。"
                        color: "#64748b"; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true
                    }
                }
            }

            // 移动端底部专用大拇指导航栏
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 52
                color: "#11131a"; border.color: "#1e222d"; border.width: 1

                RowLayout {
                    anchors.fill: parent; spacing: 0

                    // Tab 0
                    Rectangle {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        color: root.mobileTab === 0 ? "#1e2436" : "transparent"
                        ColumnLayout {
                            anchors.centerIn: parent; spacing: 2
                            Text { Layout.alignment: Qt.AlignHCenter; text: "剪辑"; color: parent.checked ? "#38bdf8" : "#94a3b8"; font.pixelSize: 12 }
                            Text { Layout.alignment: Qt.AlignHCenter; text: "时间线 (" + root.timelineClips.length + ")"; color: root.mobileTab === 0 ? "#38bdf8" : "#94a3b8"; font.pixelSize: 10; font.bold: true }
                        }
                        MouseArea { anchors.fill: parent; onClicked: root.mobileTab = 0 }
                    }

                    // Tab 1
                    Rectangle {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        color: root.mobileTab === 1 ? "#1e2436" : "transparent"
                        ColumnLayout {
                            anchors.centerIn: parent; spacing: 2
                            Text { Layout.alignment: Qt.AlignHCenter; text: "素材"; color: parent.checked ? "#38bdf8" : "#94a3b8"; font.pixelSize: 12 }
                            Text { Layout.alignment: Qt.AlignHCenter; text: "素材池 (" + root.getSelectedCount() + ")"; color: root.mobileTab === 1 ? "#38bdf8" : "#94a3b8"; font.pixelSize: 10; font.bold: true }
                        }
                        MouseArea { anchors.fill: parent; onClicked: root.mobileTab = 1 }
                    }

                    // Tab 2
                    Rectangle {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        color: root.mobileTab === 2 ? "#1e2436" : "transparent"
                        ColumnLayout {
                            anchors.centerIn: parent; spacing: 2
                            Text { Layout.alignment: Qt.AlignHCenter; text: "初筛"; color: parent.checked ? "#38bdf8" : "#94a3b8"; font.pixelSize: 12 }
                            Text { Layout.alignment: Qt.AlignHCenter; text: "AI初筛"; color: root.mobileTab === 2 ? "#38bdf8" : "#94a3b8"; font.pixelSize: 10; font.bold: true }
                        }
                        MouseArea { anchors.fill: parent; onClicked: root.mobileTab = 2 }
                    }
                }
            }
        }

        // ------------------------------------------
        // 分支 B: 桌面端三栏专业工作台
        // ------------------------------------------
        RowLayout {
            visible: !root.isMobileMode
            Layout.fillWidth: true; Layout.fillHeight: true
            spacing: 0

            // 2.1 左栏：素材池
            Rectangle {
                visible: !root.isTheaterMode
                Layout.preferredWidth: 320
                Layout.fillHeight: true
                color: "#121318"; border.color: "#1f222e"; border.width: 1

                ColumnLayout {
                    anchors.fill: parent; spacing: 0

                    // 仅在有素材时展示简洁顶栏与加号菜单 (空态时不重复堆叠按键)
                    Rectangle {
                        visible: root.mediaPool.length > 0
                        Layout.fillWidth: true; Layout.preferredHeight: 40
                        color: "#161822"; border.color: "#1f222e"; border.width: 1

                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 6
                            Text {
                                text: "素材媒体池 (" + root.mediaPool.length + ")"
                                color: "#ffffff"; font.bold: true; font.pixelSize: 12
                            }
                            Item { Layout.fillWidth: true }

                            // 视图模式切换 (网格 / 列表)
                            Rectangle {
                                Layout.preferredHeight: 24; Layout.preferredWidth: 84; radius: 4; color: "#1e2230"
                                RowLayout {
                                    anchors.fill: parent; spacing: 0
                                    Rectangle {
                                        Layout.fillWidth: true; Layout.fillHeight: true; radius: 4
                                        color: root.mediaViewMode === "grid" ? "#2563eb" : "transparent"
                                        Text { anchors.centerIn: parent; text: "网格"; color: root.mediaViewMode === "grid" ? "#ffffff" : "#94a3b8"; font.pixelSize: 10; font.bold: true }
                                        MouseArea { anchors.fill: parent; onClicked: root.mediaViewMode = "grid" }
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true; Layout.fillHeight: true; radius: 4
                                        color: root.mediaViewMode === "list" ? "#2563eb" : "transparent"
                                        Text { anchors.centerIn: parent; text: "列表"; color: root.mediaViewMode === "list" ? "#ffffff" : "#94a3b8"; font.pixelSize: 10; font.bold: true }
                                        MouseArea { anchors.fill: parent; onClicked: root.mediaViewMode = "list" }
                                    }
                                }
                            }

                            Button {
                                text: "+ 导入 ▾"
                                Layout.preferredHeight: 24
                                onClicked: desktopImportMenu.open()
                                background: Rectangle { color: "#1e293b"; radius: 4 }
                                contentItem: Text { text: parent.text; color: "#93c5fd"; font.pixelSize: 11; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }

                                Menu {
                                    id: desktopImportMenu
                                    MenuItem { text: "导入视频文件夹..."; onTriggered: folderDialog.open() }
                                    MenuItem { text: "选取视频文件 (可多选)..."; onTriggered: fileDialog.open() }
                                    MenuItem { text: "重新检测相机卡"; onTriggered: bridge.detectDji() }
                                }
                            }
                        }
                    }

                    // 极速并发解析动态进度指示条
                    Rectangle {
                        visible: root.isImporting
                        Layout.fillWidth: true; Layout.preferredHeight: 34
                        color: "#0f1f38"; border.color: "#38bdf8"; border.width: 1

                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
                            ProgressBar {
                                Layout.preferredWidth: 80; Layout.preferredHeight: 6
                                value: root.importTotal > 0 ? (root.importCurrent / root.importTotal) : 0.0
                            }
                            Text {
                                Layout.fillWidth: true
                                text: "正在多线程解析 [" + root.importCurrent + "/" + root.importTotal + "] " + root.importFilename
                                color: "#93c5fd"; font.pixelSize: 10; elide: Text.ElideMiddle
                            }
                        }
                    }

                    // 空态大卡片 (无素材时显示)
                    Rectangle {
                        visible: root.mediaPool.length === 0
                        Layout.fillWidth: true; Layout.fillHeight: true
                        color: "#12131a"

                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 14
                            width: Math.min(280, parent.width - 24)

                            Text { Layout.alignment: Qt.AlignHCenter; text: "[素材库为空]"; color: "#64748b"; font.pixelSize: 13 }
                            Text { Layout.alignment: Qt.AlignHCenter; text: "素材库为空"; color: "#f8fafc"; font.bold: true; font.pixelSize: 16 }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: "支持将视频或文件夹直接拖入窗口\n或点击下方按钮手动导入"
                                color: "#94a3b8"; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; lineHeight: 1.3
                            }

                            Button {
                                text: "导入视频文件夹"
                                Layout.fillWidth: true; Layout.preferredHeight: 42
                                onClicked: folderDialog.open()
                                background: Rectangle { color: "#2563eb"; radius: 6 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Button {
                                text: "选取视频文件 (支持多选)"
                                Layout.fillWidth: true; Layout.preferredHeight: 42
                                onClicked: fileDialog.open()
                                background: Rectangle { color: "#1e293b"; radius: 6; border.color: "#334155" }
                                contentItem: Text { text: parent.text; color: "#e2e8f0"; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            // 插入的相机提示
                            Button {
                                visible: root.detectedDjiPath !== ""
                                text: "发现相机外设 [一键导入]"
                                Layout.fillWidth: true; Layout.preferredHeight: 42
                                onClicked: bridge.scanDirectory(root.detectedDjiPath)
                                background: Rectangle { color: "#065f46"; radius: 6; border.color: "#10b981" }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            // 历史记录
                            ColumnLayout {
                                visible: root.recentDirs.length > 0
                                Layout.fillWidth: true; spacing: 6
                                Text { text: "最近使用的目录:"; color: "#64748b"; font.pixelSize: 11 }
                                Repeater {
                                    model: root.recentDirs.slice(0, 3)
                                    delegate: Button {
                                        Layout.fillWidth: true; Layout.preferredHeight: 26
                                        text: "" + modelData.split("/").pop()
                                        onClicked: bridge.scanDirectory(modelData)
                                        background: Rectangle { color: "#181a24"; radius: 4 }
                                        contentItem: Text { text: parent.text; color: "#93c5fd"; font.pixelSize: 10; elide: Text.ElideMiddle; verticalAlignment: Text.AlignVCenter }
                                    }
                                }
                            }
                        }
                    }

                    // 日期过滤选项卡 (有素材时显示)
                    Rectangle {
                        visible: root.mediaPool.length > 0
                        Layout.fillWidth: true; Layout.preferredHeight: 36
                        color: "#161822"
                        Flickable {
                            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                            contentWidth: dateRow.implicitWidth; boundsBehavior: Flickable.StopAtBounds
                            RowLayout {
                                id: dateRow; spacing: 6
                                Repeater {
                                    model: root.datesList
                                    delegate: Rectangle {
                                        Layout.preferredHeight: 24; Layout.preferredWidth: Math.max(62, dtText.implicitWidth + 14); radius: 4
                                        color: root.activeDate === modelData ? "#2563eb" : "#222533"
                                        Text {
                                            id: dtText
                                            anchors.centerIn: parent
                                            text: modelData + (modelData === "全部" ? " (" + root.mediaPool.length + ")" : "")
                                            color: "#ffffff"; font.pixelSize: 11; font.bold: root.activeDate === modelData
                                        }
                                        MouseArea { anchors.fill: parent; onClicked: root.activeDate = modelData }
                                    }
                                }
                            }
                        }
                    }

                    // 批量选择与统计状态条 (清晰、明了、好用)
                    Rectangle {
                        visible: root.mediaPool.length > 0
                        Layout.fillWidth: true; Layout.preferredHeight: 34
                        color: "#181a24"; border.color: "#1e2230"; border.width: 1

                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 6

                            Button {
                                text: "全选"
                                Layout.preferredHeight: 22
                                onClicked: root.selectAllInActiveFilter()
                                background: Rectangle { color: parent.hovered ? "#3b82f6" : "#272a38"; radius: 3 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Button {
                                text: "反选"
                                Layout.preferredHeight: 22
                                onClicked: root.invertSelectionInActiveFilter()
                                background: Rectangle { color: parent.hovered ? "#334155" : "#272a38"; radius: 3 }
                                contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 10; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Button {
                                text: "清空"
                                Layout.preferredHeight: 22
                                onClicked: root.clearAllInActiveFilter()
                                background: Rectangle { color: parent.hovered ? "#450a0a" : "#272a38"; radius: 3 }
                                contentItem: Text { text: parent.text; color: "#fca5a5"; font.pixelSize: 10; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Item { Layout.fillWidth: true }

                            Rectangle {
                                Layout.preferredHeight: 22
                                Layout.preferredWidth: selStatsText.implicitWidth + 12
                                radius: 3
                                color: root.selectedCount > 0 ? "#1e3a5f" : "#1e2230"
                                border.color: root.selectedCount > 0 ? "#3b82f6" : "#334155"

                                Text {
                                    id: selStatsText
                                    anchors.centerIn: parent
                                    text: "已选 " + root.selectedCount + " / " + root.getFilteredMedia().length + " (" + root.selectedDurationStr + ")"
                                    color: root.selectedCount > 0 ? "#93c5fd" : "#64748b"
                                    font.pixelSize: 10
                                    font.bold: true
                                }
                            }
                        }
                    }

                    // 视图容器
                    Item {
                        visible: root.mediaPool.length > 0
                        Layout.fillWidth: true; Layout.fillHeight: true

                        // 分支 A: 网格大图卡片视图 (默认)
                        GridView {
                            id: desktopMediaGrid
                            visible: root.mediaViewMode === "grid"
                            anchors.fill: parent; anchors.margins: 4
                            clip: true
                            cellWidth: Math.floor(desktopMediaGrid.width / 2)
                            cellHeight: 126
                            model: root.getFilteredMedia()

                            delegate: Item {
                                width: desktopMediaGrid.cellWidth; height: desktopMediaGrid.cellHeight

                                Rectangle {
                                    id: gridCard
                                    anchors.fill: parent; anchors.margins: 3
                                    radius: 6
                                    color: root.selectedPathsMap[modelData.path] ? "#172033" : (cardMouse.containsMouse ? "#181a24" : "#12141c")
                                    border.color: root.selectedPathsMap[modelData.path] ? "#3b82f6" : (cardMouse.containsMouse ? "#475569" : "#1f222e")
                                    border.width: root.selectedPathsMap[modelData.path] ? 2 : 1

                                    ColumnLayout {
                                        anchors.fill: parent; anchors.margins: 4; spacing: 4

                                        // 缩略图区域
                                        Rectangle {
                                            Layout.fillWidth: true; Layout.preferredHeight: 74; radius: 4; clip: true; color: "#000000"

                                            Image {
                                                anchors.fill: parent
                                                source: modelData.thumb
                                                fillMode: Image.PreserveAspectCrop
                                            }

                                            // 左上角勾选圆圈指示器 (极为清晰醒目)
                                            Rectangle {
                                                anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 4
                                                width: 22; height: 22; radius: 11
                                                color: root.selectedPathsMap[modelData.path] ? "#2563eb" : "#80000000"
                                                border.color: root.selectedPathsMap[modelData.path] ? "#ffffff" : "#94a3b8"
                                                border.width: 1.5

                                                Text {
                                                    anchors.centerIn: parent
                                                    text: "✓"
                                                    visible: !!root.selectedPathsMap[modelData.path]
                                                    color: "#ffffff"; font.bold: true; font.pixelSize: 13
                                                }
                                            }

                                            // 右下角时长标签
                                            Rectangle {
                                                anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 4
                                                height: 15; width: durText.implicitWidth + 8; radius: 2; color: "#b3000000"
                                                Text { id: durText; anchors.centerIn: parent; text: modelData.duration_str; color: "#ffffff"; font.pixelSize: 9; font.bold: true }
                                            }

                                            // 右上角 10-Bit HDR 标签
                                            Rectangle {
                                                visible: modelData.is_10bit
                                                anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 4
                                                height: 14; width: 34; radius: 2; color: "#8b5cf6"
                                                Text { anchors.centerIn: parent; text: "10-Bit"; color: "#ffffff"; font.pixelSize: 8; font.bold: true }
                                            }

                                            // 悬浮预览按钮
                                            Rectangle {
                                                anchors.centerIn: parent
                                                width: 32; height: 32; radius: 16
                                                color: "#b3000000"
                                                border.color: "#38bdf8"; border.width: 1
                                                visible: cardMouse.containsMouse
                                                Text { anchors.centerIn: parent; text: "▶"; color: "#38bdf8"; font.pixelSize: 12 }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    onClicked: (mouse) => {
                                                        player.stop()
                                                        player.source = "file://" + modelData.path
                                                        player.position = 0
                                                        player.play()
                                                        mouse.accepted = true
                                                    }
                                                }
                                            }
                                        }

                                        // 下方文件信息
                                        ColumnLayout {
                                            Layout.fillWidth: true; spacing: 1
                                            Text {
                                                text: modelData.filename
                                                color: root.selectedPathsMap[modelData.path] ? "#ffffff" : "#cbd5e1"
                                                font.pixelSize: 10; font.bold: true; elide: Text.ElideMiddle; Layout.fillWidth: true
                                            }
                                            RowLayout {
                                                spacing: 4
                                                Text { text: modelData.width + "×" + modelData.height; color: "#64748b"; font.pixelSize: 9 }
                                                Item { Layout.fillWidth: true }
                                                Text { text: modelData.date ? modelData.date.substring(5) : ""; color: "#64748b"; font.pixelSize: 9 }
                                            }
                                        }
                                    }

                                    // 单击整个卡片立即切换勾选状态，双击预览
                                    MouseArea {
                                        id: cardMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: root.toggleItemSelection(modelData.path)
                                        onDoubleClicked: {
                                            player.stop()
                                            player.source = "file://" + modelData.path
                                            player.position = 0
                                            player.play()
                                        }
                                    }
                                }
                            }
                        }

                        // 分支 B: 详细列表模式
                        ListView {
                            id: desktopMediaListView
                            visible: root.mediaViewMode === "list"
                            anchors.fill: parent; anchors.margins: 4
                            clip: true; spacing: 4
                            model: root.getFilteredMedia()

                            delegate: Rectangle {
                                width: desktopMediaListView.width; height: 64; radius: 5
                                color: root.selectedPathsMap[modelData.path] ? "#172033" : (listMouse.containsMouse ? "#181a24" : "#14151d")
                                border.color: root.selectedPathsMap[modelData.path] ? "#3b82f6" : (listMouse.containsMouse ? "#475569" : "#1f222e")
                                border.width: root.selectedPathsMap[modelData.path] ? 2 : 1

                                RowLayout {
                                    anchors.fill: parent; anchors.margins: 6; spacing: 8

                                    // 圆形勾选指示器
                                    Rectangle {
                                        Layout.preferredWidth: 22; Layout.preferredHeight: 22; radius: 11
                                        color: root.selectedPathsMap[modelData.path] ? "#2563eb" : "#80000000"
                                        border.color: root.selectedPathsMap[modelData.path] ? "#ffffff" : "#94a3b8"
                                        border.width: 1.5

                                        Text {
                                            anchors.centerIn: parent
                                            text: "✓"
                                            visible: !!root.selectedPathsMap[modelData.path]
                                            color: "#ffffff"; font.bold: true; font.pixelSize: 13
                                        }
                                    }

                                    // 缩略图
                                    Rectangle {
                                        Layout.preferredWidth: 84; Layout.fillHeight: true; radius: 4; clip: true; color: "#000000"
                                        Image { anchors.fill: parent; source: modelData.thumb; fillMode: Image.PreserveAspectCrop }
                                        Rectangle {
                                            anchors.centerIn: parent; width: 24; height: 24; radius: 12
                                            color: "#b3000000"; border.color: "#38bdf8"; border.width: 1
                                            visible: listMouse.containsMouse
                                            Text { anchors.centerIn: parent; text: "▶"; color: "#38bdf8"; font.pixelSize: 10 }
                                        }
                                    }

                                    // 信息列
                                    ColumnLayout {
                                        Layout.fillWidth: true; spacing: 2
                                        Text { text: modelData.filename; color: "#ffffff"; font.pixelSize: 11; font.bold: true; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                        RowLayout {
                                            spacing: 6
                                            Text { text: modelData.duration_str; color: "#94a3b8"; font.pixelSize: 10 }
                                            Text { text: modelData.width + "×" + modelData.height; color: "#64748b"; font.pixelSize: 10 }
                                            Rectangle {
                                                color: modelData.is_10bit ? "#8b5cf6" : "#334155"; radius: 2
                                                Layout.preferredHeight: 14; Layout.preferredWidth: modelData.is_10bit ? 58 : 36
                                                Text { anchors.centerIn: parent; text: modelData.is_10bit ? "10-Bit D-Log" : "8-Bit"; color: "#ffffff"; font.pixelSize: 9; font.bold: true }
                                            }
                                        }
                                    }
                                }

                                MouseArea {
                                    id: listMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: root.toggleItemSelection(modelData.path)
                                    onDoubleClicked: {
                                        player.stop()
                                        player.source = "file://" + modelData.path
                                        player.position = 0
                                        player.play()
                                    }
                                }
                            }
                        }
                    }

                    // 底部行动条 (有素材时常驻)
                    Rectangle {
                        visible: root.mediaPool.length > 0
                        Layout.fillWidth: true; Layout.preferredHeight: 48
                        color: "#161822"; border.color: "#1f222e"; border.width: 1

                        RowLayout {
                            anchors.fill: parent; anchors.margins: 8; spacing: 8

                            Button {
                                Layout.fillWidth: true; Layout.preferredHeight: 32
                                text: "+ 加入时间线"
                                enabled: root.selectedCount > 0
                                onClicked: root.addSelectedToTimeline()
                                background: Rectangle {
                                    color: root.selectedCount > 0 ? (parent.hovered ? "#1e293b" : "#0f172a") : "#161922"
                                    border.color: root.selectedCount > 0 ? "#38bdf8" : "#334155"
                                    radius: 4
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: root.selectedCount > 0 ? "#38bdf8" : "#64748b"
                                    font.bold: true; font.pixelSize: 11
                                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                }
                            }

                            Button {
                                Layout.fillWidth: true; Layout.preferredHeight: 32
                                text: "AI 智能初筛 (" + root.selectedCount + ")"
                                enabled: root.selectedCount > 0
                                onClicked: aiModal.open()
                                background: Rectangle {
                                    radius: 4
                                    gradient: Gradient {
                                        orientation: Gradient.Horizontal
                                        GradientStop { position: 0.0; color: root.selectedCount > 0 ? "#2563eb" : "#334155" }
                                        GradientStop { position: 1.0; color: root.selectedCount > 0 ? "#7c3aed" : "#1e293b" }
                                    }
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: root.selectedCount > 0 ? "#ffffff" : "#64748b"
                                    font.bold: true; font.pixelSize: 11
                                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }
                    }
                }
            }

            // 2.2 中栏：桌面大屏监视器
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true
                color: "#050608"

                ColumnLayout {
                    anchors.fill: parent; spacing: 0

                    Item {
                        Layout.fillWidth: true; Layout.fillHeight: true

                        VideoOutput {
                            id: desktopMonitorOutput
                            anchors.fill: parent
                            fillMode: VideoOutput.PreserveAspectFit
                        }

                        // 桌面大屏实时口播字幕悬浮框
                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottomMargin: 24
                            color: "#d90b1120"
                            border.color: "#38bdf8"
                            border.width: 1
                            radius: 6
                            visible: root.currentSubtitleText.length > 0
                            width: Math.min(parent.width - 48, desktopSubText.implicitWidth + 32)
                            height: desktopSubText.implicitHeight + 16

                            Text {
                                id: desktopSubText
                                anchors.centerIn: parent
                                text: root.currentSubtitleText
                                color: "#ffffff"
                                font.pixelSize: 15
                                font.bold: true
                                elide: Text.ElideRight
                            }
                        }
                    }

                    // 监视器控制栏
                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 48
                        color: "#11131a"; border.color: "#1e222d"; border.width: 1

                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16; spacing: 12

                            Button {
                                text: "-1s"; onClicked: player.position = Math.max(0, player.position - 1000)
                                background: Rectangle { color: "#222533"; radius: 4 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Button {
                                text: player.playbackState === MediaPlayer.PlayingState ? "⏸ 暂停" : "▶ 播放"
                                onClicked: {
                                    if (player.playbackState === MediaPlayer.PlayingState) player.pause()
                                    else player.play()
                                }
                                background: Rectangle { color: "#3b82f6"; radius: 4; Layout.preferredWidth: 64 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Button {
                                text: "+1s"; onClicked: player.position = player.position + 1000
                                background: Rectangle { color: "#222533"; radius: 4 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Text {
                                text: root.formatTime(player.position / 1000.0) + " / " + root.formatTime(root.totalMovieDuration) + " (镜头 #" + (root.activeClipIndex + 1) + ")"
                                color: "#38bdf8"; font.bold: true; font.pixelSize: 12
                            }

                            Item { Layout.fillWidth: true }

                            Button {
                                text: root.isContinuousPlay ? "连续播放成片" : "单镜头循环"
                                onClicked: root.isContinuousPlay = !root.isContinuousPlay
                                background: Rectangle { color: root.isContinuousPlay ? "#1d4ed8" : "#334155"; radius: 4 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Button {
                                text: root.isTheaterMode ? "恢复视窗" : "剧场大屏"
                                onClicked: root.isTheaterMode = !root.isTheaterMode
                                background: Rectangle { color: "#222533"; radius: 4 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Button {
                                text: "全屏"
                                onClicked: {
                                    if (root.visibility === Window.FullScreen) root.visibility = Window.AutomaticVisibility
                                    else root.visibility = Window.FullScreen
                                }
                                background: Rectangle { color: "#222533"; radius: 4 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }
                        }
                    }
                }
            }

            // 2.3 右栏：桌面镜头检查器
            Rectangle {
                visible: !root.isTheaterMode
                Layout.preferredWidth: 320; Layout.fillHeight: true
                color: "#121318"; border.color: "#1f222e"; border.width: 1

                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 14; spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: "镜头 #" + (root.activeClipIndex + 1); color: "#ffffff"; font.bold: true; font.pixelSize: 15 }
                        Item { Layout.fillWidth: true }
                        Rectangle { color: "#3b82f6"; radius: 3; Layout.preferredHeight: 18; Layout.preferredWidth: 64; Text { anchors.centerIn: parent; text: "横屏 16:9"; color: "#ffffff"; font.pixelSize: 10; font.bold: true } }
                        Rectangle { color: "#8b5cf6"; radius: 3; Layout.preferredHeight: 18; Layout.preferredWidth: 64; Text { anchors.centerIn: parent; text: "10-Bit HDR"; color: "#ffffff"; font.pixelSize: 10; font.bold: true } }
                    }

                    Text {
                        text: root.timelineClips.length > root.activeClipIndex ? root.timelineClips[root.activeClipIndex].filename : "未选中镜头"
                        color: "#94a3b8"; font.pixelSize: 11; elide: Text.ElideMiddle; Layout.fillWidth: true
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#1f222e" }

                    // 起点微调
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 6
                        RowLayout {
                            Text { text: "起点 (In Point):"; color: "#cbd5e1"; font.pixelSize: 12 }
                            Item { Layout.fillWidth: true }
                            Text { text: root.timelineClips.length > root.activeClipIndex ? root.formatTime(root.timelineClips[root.activeClipIndex].start) : "00:00"; color: "#38bdf8"; font.bold: true; font.pixelSize: 13 }
                        }

                        GridLayout {
                            columns: 4; Layout.fillWidth: true; rowSpacing: 6; columnSpacing: 6
                            Button { text: "-2s"; Layout.fillWidth: true; onClicked: root.stepActiveIn(-2.0); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                            Button { text: "-0.5s"; Layout.fillWidth: true; onClicked: root.stepActiveIn(-0.5); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                            Button { text: "+0.5s"; Layout.fillWidth: true; onClicked: root.stepActiveIn(0.5); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                            Button { text: "+2s"; Layout.fillWidth: true; onClicked: root.stepActiveIn(2.0); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                        }
                    }

                    // 终点微调
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 6
                        RowLayout {
                            Text { text: "终点 (Out Point):"; color: "#cbd5e1"; font.pixelSize: 12 }
                            Item { Layout.fillWidth: true }
                            Text { text: root.timelineClips.length > root.activeClipIndex ? root.formatTime(root.timelineClips[root.activeClipIndex].end) : "00:00"; color: "#38bdf8"; font.bold: true; font.pixelSize: 13 }
                        }

                        GridLayout {
                            columns: 4; Layout.fillWidth: true; rowSpacing: 6; columnSpacing: 6
                            Button { text: "-2s"; Layout.fillWidth: true; onClicked: root.stepActiveOut(-2.0); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                            Button { text: "-0.5s"; Layout.fillWidth: true; onClicked: root.stepActiveOut(-0.5); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                            Button { text: "+0.5s"; Layout.fillWidth: true; onClicked: root.stepActiveOut(0.5); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                            Button { text: "+2s"; Layout.fillWidth: true; onClicked: root.stepActiveOut(2.0); background: Rectangle { color: "#222533"; radius: 4 } contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                        }
                    }

                    Text {
                        text: "截取时长: " + (root.timelineClips.length > root.activeClipIndex ? root.timelineClips[root.activeClipIndex].dur_str : "0s")
                        color: "#60a5fa"; font.bold: true; font.pixelSize: 13
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#1f222e" }

                    Button {
                        text: "在指针位置分割 (Split)"; Layout.fillWidth: true; Layout.preferredHeight: 38
                        onClicked: root.splitCurrentClip()
                        background: Rectangle { color: "#854d0e"; radius: 6 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }
                    Button {
                        text: "此镜头再加一段 (Duplicate)"; Layout.fillWidth: true; Layout.preferredHeight: 38
                        onClicked: root.duplicateCurrentClip()
                        background: Rectangle { color: "#1e3a8a"; radius: 6 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }
                    Button {
                        text: "从时间线删掉 (Delete)"; Layout.fillWidth: true; Layout.preferredHeight: 38
                        onClicked: root.deleteCurrentClip()
                        background: Rectangle { color: "#7f1d1d"; radius: 6 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }

        // 桌面端底部胶片时间线
        Rectangle {
            visible: !root.isMobileMode
            Layout.fillWidth: true; Layout.preferredHeight: 180
            color: "#0f1016"; border.color: "#1e212d"; border.width: 1

            ColumnLayout {
                anchors.fill: parent; spacing: 4

                RowLayout {
                    Layout.fillWidth: true; Layout.preferredHeight: 28
                    anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8

                    Button {
                        text: "↩ 撤销 (Z)"
                        enabled: root.canUndo; opacity: root.canUndo ? 1.0 : 0.35
                        onClicked: root.undo()
                        background: Rectangle { color: root.canUndo ? "#222738" : "#141620"; radius: 4; border.color: root.canUndo ? "#475569" : "#1e2230" }
                        contentItem: Text { text: parent.text; color: root.canUndo ? "#ffffff" : "#64748b"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }
                    Button {
                        text: "↪ 重做 (⇧Z)"
                        enabled: root.canRedo; opacity: root.canRedo ? 1.0 : 0.35
                        onClicked: root.redo()
                        background: Rectangle { color: root.canRedo ? "#222738" : "#141620"; radius: 4; border.color: root.canRedo ? "#475569" : "#1e2230" }
                        contentItem: Text { text: parent.text; color: root.canRedo ? "#ffffff" : "#64748b"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }
                    Button { text: "分割 (B)"; onClicked: root.splitCurrentClip(); background: Rectangle { color: "#1e2230"; radius: 4 } contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                    Button { text: "删除 (Del)"; onClicked: root.deleteCurrentClip(); background: Rectangle { color: "#1e2230"; radius: 4 } contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                    Button { text: "◀ 前移"; onClicked: root.moveCurrentClip(-1); background: Rectangle { color: "#1e2230"; radius: 4 } contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                    Button { text: "后移 ▶"; onClicked: root.moveCurrentClip(1); background: Rectangle { color: "#1e2230"; radius: 4 } contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                    Item { Layout.fillWidth: true }
                    Text { text: "提示：双击/拖拽素材导入时间线 · 支持双指触控撤销"; color: "#64748b"; font.pixelSize: 11 }
                }

                ListView {
                    id: desktopTimelineView
                    Layout.fillWidth: true; Layout.fillHeight: true
                    orientation: ListView.Horizontal; clip: true
                    model: root.timelineClips; spacing: 8
                    anchors.leftMargin: 12; anchors.rightMargin: 12

                    delegate: Rectangle {
                        readonly property bool isInvalidClip: root.missingPathsMap[modelData.src] === true
                        width: Math.max(90, Math.min(260, Number(modelData.dur || 5) * 8))
                        height: 110; radius: 6; clip: true
                        color: isInvalidClip ? "#450a0a" : (root.activeClipIndex === index ? "#1e293b" : "#141620")
                        border.color: isInvalidClip ? "#ef4444" : (root.activeClipIndex === index ? "#38bdf8" : "#2d3345")
                        border.width: (isInvalidClip || root.activeClipIndex === index) ? 2 : 1

                        Image { anchors.fill: parent; source: modelData.thumb; fillMode: Image.PreserveAspectCrop; opacity: isInvalidClip ? 0.3 : 0.65 }

                        Rectangle {
                            visible: isInvalidClip
                            anchors.fill: parent
                            color: "#99450a0a"
                            ColumnLayout {
                                anchors.centerIn: parent
                                spacing: 2
                                Text { text: "[离线]"; color: "#ef4444"; font.bold: true; font.pixelSize: 12; Layout.alignment: Qt.AlignHCenter }
                                Text { text: "原素材已失效/被改动"; color: "#fca5a5"; font.bold: true; font.pixelSize: 10; Layout.alignment: Qt.AlignHCenter }
                            }
                        }

                        Rectangle {
                            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                            height: 22; color: "#cc0f172a"
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 4
                                Text { text: "#" + (index + 1); color: "#ffffff"; font.bold: true; font.pixelSize: 11 }
                                Item { Layout.fillWidth: true }
                                Text { text: modelData.dur_str; color: "#38bdf8"; font.bold: true; font.pixelSize: 10 }
                            }
                        }

                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.bottomMargin: 20
                            anchors.left: parent.left; anchors.right: parent.right
                            anchors.margins: 4
                            height: 18; radius: 3; color: "#e60f172a"
                            visible: Boolean(modelData.text && modelData.text.length > 0)
                            Text {
                                anchors.fill: parent; anchors.margins: 2
                                text: modelData.text || ""
                                color: "#f8fafc"; font.pixelSize: 9
                                elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter
                            }
                        }

                        Rectangle {
                            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                            height: 18; color: "#bb0f172a"
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 4
                                Text { text: root.formatTime(modelData.start); color: "#94a3b8"; font.pixelSize: 9 }
                                Item { Layout.fillWidth: true }
                                Text { text: root.formatTime(modelData.end); color: "#94a3b8"; font.pixelSize: 9 }
                            }
                        }

                        MouseArea { anchors.fill: parent; onClicked: root.loadActiveClip(index, false) }
                    }
                }
            }
        }
    }

    // 状态提示横幅
    Rectangle {
        id: statusBanner
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.isMobileMode ? 64 : 190
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.max(260, bannerText.implicitWidth + 36)
        height: 36; radius: 18
        color: "#f80f172a"; border.color: "#38bdf8"; border.width: 1
        opacity: 0; visible: opacity > 0

        Text { id: bannerText; anchors.centerIn: parent; color: "#ffffff"; font.bold: true; font.pixelSize: 12 }
        Timer { id: bannerTimer; interval: 3500; onTriggered: bannerFadeOut.start() }
        NumberAnimation { id: bannerFadeOut; target: statusBanner; property: "opacity"; to: 0; duration: 400 }
        function show(msg) { bannerText.text = msg; statusBanner.opacity = 1; bannerTimer.restart() }
    }

    // AI 筛选模态弹窗
    Dialog {
        id: aiModal
        anchors.centerIn: parent
        width: Math.min(520, root.width - 32)
        height: Math.min(targetDurationCombo.currentIndex === 2 ? 515 : 435, root.height - 24)
        modal: true; title: "AI 智能初筛参数设置"
        background: Rectangle { color: "#161822"; border.color: "#2a2f42"; radius: 10 }

        Behavior on height { NumberAnimation { duration: 150 } }

        property real progress: 0.0
        property string statusText: "准备就绪"

        onAboutToShow: {
            aiCanvasCombo.currentIndex = root.masterCanvas === "vertical" ? 1 : 0
        }

        ColumnLayout {
            anchors.fill: parent; spacing: 10

            Text { text: "选择智能剪辑模型策略："; color: "#cbd5e1"; font.pixelSize: 12 }
            ComboBox {
                id: aiModeCombo; Layout.fillWidth: true
                model: [
                    "深度多模态综合混合 (推荐 · 口播连贯 + 视觉防抖美学 + 静默剔除)",
                    "口播智能粗剪 (Vlog/访谈：剔除废话重录 / 连贯保留 / 自动生成字幕)",
                    "视觉审美优先 (航拍/风光/运动：保留优质稳定构图，过滤抖动与废片)",
                    "纯语音静音切分 (基础粗切：仅切除无声与死寂停顿，保留有声片段)"
                ]
            }

            Text { text: "视频方向与成片画幅："; color: "#cbd5e1"; font.pixelSize: 12 }
            ComboBox {
                id: aiCanvasCombo
                Layout.fillWidth: true
                model: [
                    "3840×2160 16:9 横屏母带 (YouTube / B站 / 电脑与电视大屏)",
                    "2160×3840 9:16 竖屏大片 (抖音 / 视频号 / 小红书 / 移动端)"
                ]
                currentIndex: root.masterCanvas === "vertical" ? 1 : 0
                onActivated: {
                    root.masterCanvas = currentIndex === 1 ? "vertical" : "horizontal"
                }
            }

            Text { text: "目标成片节奏 / 期望时长："; color: "#cbd5e1"; font.pixelSize: 12 }
            ComboBox {
                id: targetDurationCombo; Layout.fillWidth: true
                model: [
                    "标准成片节奏 (约 3 ~ 5 分钟 · 完整呈现事件叙事与精彩空镜)",
                    "短视频快节奏 (约 1 ~ 2 分钟 · 仅提取最高光核心镜头与金句)",
                    "宽松完整保留 (按比例保留 · 仅过滤废片与重复错句)"
                ]
            }

            // 百分比保留设置模块 (仅在选择“宽松完整保留”时展开显示)
            Rectangle {
                visible: targetDurationCombo.currentIndex === 2
                Layout.fillWidth: true
                Layout.preferredHeight: 82
                color: "#181a24"
                border.color: "#2a2f42"
                radius: 6

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: "保留总时长比例："; color: "#94a3b8"; font.pixelSize: 11 }
                        Text { text: Math.round(retentionSlider.value) + "%"; color: "#38bdf8"; font.bold: true; font.pixelSize: 12 }
                        Item { Layout.fillWidth: true }
                        RowLayout {
                            spacing: 4
                            Repeater {
                                model: [
                                    { label: "30%", val: 30 },
                                    { label: "50%", val: 50 },
                                    { label: "75%", val: 75 },
                                    { label: "100% 全部", val: 100 }
                                ]
                                delegate: Rectangle {
                                    height: 20
                                    width: pillText.implicitWidth + 12
                                    radius: 3
                                    color: Math.round(retentionSlider.value) === modelData.val ? "#2563eb" : "#222533"
                                    border.color: Math.round(retentionSlider.value) === modelData.val ? "#3b82f6" : "#334155"

                                    Text {
                                        id: pillText
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: Math.round(retentionSlider.value) === modelData.val ? "#ffffff" : "#94a3b8"
                                        font.pixelSize: 10
                                        font.bold: Math.round(retentionSlider.value) === modelData.val
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: retentionSlider.value = modelData.val
                                    }
                                }
                            }
                        }
                    }

                    Item {
                        id: retentionSlider
                        Layout.fillWidth: true
                        Layout.preferredHeight: 22
                        property real from: 10
                        property real to: 100
                        property real stepSize: 5
                        property real value: 100

                        Rectangle {
                            id: track
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            height: 6
                            radius: 3
                            color: "#2a2f42"

                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: Math.max(0, Math.min(parent.width, (retentionSlider.value - retentionSlider.from) / (retentionSlider.to - retentionSlider.from) * parent.width))
                                radius: 3
                                color: "#3b82f6"
                            }
                        }

                        Rectangle {
                            id: handle
                            width: 16
                            height: 16
                            radius: 8
                            color: sliderArea.pressed ? "#60a5fa" : "#3b82f6"
                            border.color: "#ffffff"
                            border.width: 2
                            anchors.verticalCenter: track.verticalCenter
                            x: Math.max(0, Math.min(retentionSlider.width - width, (retentionSlider.value - retentionSlider.from) / (retentionSlider.to - retentionSlider.from) * (retentionSlider.width - width)))
                        }

                        MouseArea {
                            id: sliderArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor

                            function updateVal(mouseX) {
                                var ratio = Math.max(0, Math.min(1.0, mouseX / retentionSlider.width))
                                var raw = retentionSlider.from + ratio * (retentionSlider.to - retentionSlider.from)
                                var stepped = Math.round(raw / retentionSlider.stepSize) * retentionSlider.stepSize
                                retentionSlider.value = Math.max(retentionSlider.from, Math.min(retentionSlider.to, stepped))
                            }

                            onPressed: function(mouse) { updateVal(mouse.x) }
                            onPositionChanged: function(mouse) { if (pressed) updateVal(mouse.x) }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: "预计成片时长："; color: "#94a3b8"; font.pixelSize: 11 }
                        Text {
                            property real effSec: root.getEffectiveTotalSeconds()
                            property real calcDur: Math.max(5.0, effSec * retentionSlider.value / 100.0)
                            text: (effSec <= 0)
                                  ? "请先选择或导入视频素材"
                                  : ((retentionSlider.value >= 100)
                                      ? ("100% 完整保留有效镜头 (约 " + root.formatDuration(effSec) + ")")
                                      : ("约 " + root.formatDuration(calcDur) + " · 压缩比 " + Math.round(retentionSlider.value) + "% (原素材总长 " + root.formatDuration(effSec) + ")"))
                            color: "#34d399"
                            font.bold: true
                            font.pixelSize: 11
                        }
                    }
                }
            }

            ProgressBar { Layout.fillWidth: true; value: aiModal.progress / 100.0 }
            Text { text: aiModal.statusText; color: "#94a3b8"; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideMiddle }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Button {
                    text: "取消"
                    onClicked: bridge.cancelCurrentJob()
                    background: Rectangle { color: "#334155"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }

                Button {
                    text: "暂停"
                    onClicked: bridge.pauseAiScreening("用户主动暂停")
                    background: Rectangle { color: "#475569"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }

                Button {
                    text: "安全拔卡去录像"
                    onClicked: bridge.safeEjectDevice()
                    background: Rectangle { color: "#065f46"; radius: 4; border.color: "#10b981" }
                    contentItem: Text { text: parent.text; color: "#6ee7b7"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }

                Item { Layout.fillWidth: true }

                Button {
                    text: "开始 / 续接初筛"
                    onClicked: {
                        var mode = "3"
                        if (aiModeCombo.currentIndex === 0) mode = "3"
                        else if (aiModeCombo.currentIndex === 1) mode = "4"
                        else if (aiModeCombo.currentIndex === 2) mode = "1"
                        else if (aiModeCombo.currentIndex === 3) mode = "2"

                        var maxDur = 240.0
                        if (targetDurationCombo.currentIndex === 0) {
                            maxDur = 240.0
                        } else if (targetDurationCombo.currentIndex === 1) {
                            maxDur = 90.0
                        } else if (targetDurationCombo.currentIndex === 2) {
                            if (retentionSlider.value >= 100) {
                                maxDur = 0.0
                            } else {
                                var baseSec = root.getEffectiveTotalSeconds()
                                maxDur = Math.max(10.0, baseSec * (retentionSlider.value / 100.0))
                            }
                        }

                        var canvasMode = aiCanvasCombo.currentIndex === 1 ? "vertical" : "horizontal"
                        root.masterCanvas = canvasMode

                        bridge.startAiScreening(root.mediaPool, root.getSelectedPathsList(), mode, maxDur, canvasMode, root.targetColor)
                    }
                    background: Rectangle { color: "#4f46e5"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
            }
        }
    }

    // 新增外部相机接入确认弹窗 (当当前已有工程素材时提示)
    Dialog {
        id: usbNewDeviceDialog
        anchors.centerIn: parent
        width: Math.min(460, root.width - 32)
        height: 220
        modal: true
        title: "检测到新相机设备已接入"
        background: Rectangle { color: "#161822"; border.color: "#3b82f6"; radius: 10 }

        ColumnLayout {
            anchors.fill: parent; spacing: 14
            Text {
                text: "检测到相机外设 [" + (root.pendingUsbDevice ? root.pendingUsbDevice.name : "") + "] 接入，包含 " + (root.pendingUsbDevice ? root.pendingUsbDevice.count : 0) + " 个视频素材。"
                color: "#f8fafc"; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true
            }
            Text {
                text: "是否载入此相机素材到工作区？"
                color: "#94a3b8"; font.pixelSize: 12
            }
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                Button {
                    text: "暂不处理"
                    onClicked: usbNewDeviceDialog.close()
                    background: Rectangle { color: "#334155"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#cbd5e1"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "追加载入"
                    onClicked: {
                        if (root.pendingUsbDevice && root.pendingUsbDevice.dir) {
                            bridge.importPaths([root.pendingUsbDevice.dir])
                        }
                        usbNewDeviceDialog.close()
                    }
                    background: Rectangle { color: "#0ea5e9"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
                Button {
                    text: "切换到新相机"
                    onClicked: {
                        if (root.pendingUsbDevice && root.pendingUsbDevice.dir) {
                            root.timelineClips = []
                            bridge.importPaths([root.pendingUsbDevice.dir])
                        }
                        usbNewDeviceDialog.close()
                    }
                    background: Rectangle { color: "#2563eb"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
            }
        }
    }

    // 设备重连资产比对与失效处理弹窗
    Dialog {
        id: reconciliationDialog
        anchors.centerIn: parent
        width: Math.min(540, root.width - 32)
        height: Math.min(460, root.height - 40)
        modal: true
        title: "相机存储设备已重连 · 媒体资产审计报告"
        background: Rectangle { color: "#161822"; border.color: "#3b82f6"; radius: 10 }

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Text {
                text: "设备 [" + (root.lastDiffReport.device || "相机存储卡") + "] 已重新接入！"
                color: "#f8fafc"; font.bold: true; font.pixelSize: 14
            }

            // 1. 删与改报警区域 (有失效文件时警示)
            Rectangle {
                visible: root.lastDiffReport.missing && root.lastDiffReport.missing.length > 0
                Layout.fillWidth: true; Layout.preferredHeight: 140
                color: "#2a1215"; border.color: "#ef4444"; radius: 6

                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 10; spacing: 6
                    RowLayout {
                        Text { text: "警告：检测到以下源素材已被移除或被覆写修改（等同移除）："; color: "#fca5a5"; font.bold: true; font.pixelSize: 11 }
                    }
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        model: root.lastDiffReport.missing || []
                        spacing: 4
                        delegate: RowLayout {
                            spacing: 6
                            Text { text: "• " + modelData.filename; color: "#f87171"; font.bold: true; font.pixelSize: 11 }
                            Text { text: "(" + modelData.reason + ")"; color: "#fca5a5"; font.pixelSize: 10 }
                        }
                    }
                    Text { text: "时间线上相关镜头已自动标红失效 [Media Offline]，避免损坏成片。"; color: "#cbd5e1"; font.pixelSize: 10 }
                }
            }

            // 2. 新增素材提示区域 (不自动打乱当前工程)
            Rectangle {
                visible: root.lastDiffReport.new_count > 0
                Layout.fillWidth: true; Layout.preferredHeight: 46
                color: "#0f231e"; border.color: "#10b981"; radius: 6
                RowLayout {
                    anchors.fill: parent; anchors.margins: 10; spacing: 8
                    Text {
                        text: "相机在断开期间新录制了 " + root.lastDiffReport.new_count + " 个视频（已为你保留，未自动强行打乱当前时间线）"
                        color: "#6ee7b7"; font.pixelSize: 11
                    }
                }
            }

            Item { Layout.fillHeight: true }

            RowLayout {
                Layout.fillWidth: true; spacing: 8

                Button {
                    visible: root.lastDiffReport.missing && root.lastDiffReport.missing.length > 0
                    text: "一键清除失效片段"
                    onClicked: root.purgeInvalidClips()
                    background: Rectangle { color: "#7f1d1d"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }

                Button {
                    text: "保留占位稍后处理"
                    onClicked: reconciliationDialog.close()
                    background: Rectangle { color: "#334155"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }

                Item { Layout.fillWidth: true }

                Button {
                    text: "确定并继续"
                    onClicked: {
                        reconciliationDialog.close()
                        bridge.resumeAiScreening()
                    }
                    background: Rectangle { color: "#2563eb"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
            }
        }
    }

    // 导出模态弹窗
    Dialog {
        id: exportModal
        anchors.centerIn: parent
        width: Math.min(540, root.width - 32); height: 420
        modal: true; title: "导出 4K 60fps 最终母带"
        background: Rectangle { color: "#161822"; border.color: "#2a2f42"; radius: 10 }

        property real progress: 0.0
        property string statusText: "准备压制..."

        ColumnLayout {
            anchors.fill: parent; spacing: 10
            Text {
                text: "规范：HEVC Main 10 (p010le) · BT.2020 · ARIB STD-B67 (HLG) · 65Mbps\n100% 符合 YouTube / B站 原生 4K60 HDR 审核标准"
                color: "#38bdf8"; font.pixelSize: 11; lineHeight: 1.3
            }

            Text { text: "导出画幅母带标准："; color: "#cbd5e1"; font.pixelSize: 12 }
            ComboBox {
                id: exportCanvasCombo; Layout.fillWidth: true
                model: [
                    "3840×2160 16:9 横屏母带 (YouTube / B站 / 电脑大屏)",
                    "2160×3840 9:16 竖屏大片 (抖音 / 视频号 / 小红书 / 移动端)"
                ]
                currentIndex: root.masterCanvas === "vertical" ? 1 : 0
                onActivated: {
                    root.masterCanvas = currentIndex === 1 ? "vertical" : "horizontal"
                }
            }

            Text { text: "色彩与编码规格："; color: "#cbd5e1"; font.pixelSize: 12 }
            ComboBox {
                id: exportColorCombo; Layout.fillWidth: true
                model: [
                    "10-Bit HDR (Apple Silicon ProRes / BT.2020 HLG / 65Mbps 原色母带)",
                    "8-Bit SDR (Rec.709 / 35Mbps 标准网络快速分享)"
                ]
                currentIndex: root.targetColor === "8bit" ? 1 : 0
                onActivated: {
                    root.targetColor = currentIndex === 1 ? "8bit" : "10bit"
                }
            }

            ProgressBar { Layout.fillWidth: true; value: exportModal.progress / 100.0 }
            Text { text: exportModal.statusText; color: "#f1f5f9"; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideMiddle }
            RowLayout {
                Layout.fillWidth: true
                Button {
                    text: "中止导出"
                    onClicked: bridge.cancelCurrentJob()
                    background: Rectangle { color: "#ef4444"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "导出 SRT 字幕"
                    onClicked: { bridge.generateSrt(root.timelineClips, "") }
                    background: Rectangle { color: "#1e293b"; radius: 4; border.color: "#38bdf8" }
                    contentItem: Text { text: parent.text; color: "#38bdf8"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
                Button {
                    text: "开始硬件加速渲染"
                    onClicked: { bridge.startExport(root.timelineClips, root.masterCanvas, root.targetColor) }
                    background: Rectangle { color: "#2563eb"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
            }
        }
    }

    // 文件夹选择器
    FolderDialog {
        id: folderDialog
        title: "选择包含视频素材的文件夹"
        onAccepted: {
            var path = folderDialog.selectedFolder.toString()
            if (path.indexOf("file://") === 0) path = decodeURIComponent(path.substring(7))
            bridge.scanDirectory(path)
        }
    }

    // 视频文件选择器 (支持多选)
    FileDialog {
        id: fileDialog
        title: "选择特定视频文件 (支持按住 Cmd / Shift 多选)"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["视频文件 (*.mp4 *.MP4 *.mov *.MOV *.mkv)", "所有文件 (*)"]
        onAccepted: {
            var paths = []
            for (var i = 0; i < selectedFiles.length; i++) {
                var u = selectedFiles[i].toString()
                if (u.indexOf("file://") === 0) u = decodeURIComponent(u.substring(7))
                paths.push(u)
            }
            bridge.importPaths(paths)
        }
    }
}
