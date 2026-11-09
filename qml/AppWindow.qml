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
    title: "DJI 智能剪辑大师 Pro (4K 10-Bit HDR · 全平台响应式)"
    color: "#0c0d11"

    // 响应式移动端状态
    property bool isForceMobile: false
    readonly property bool isMobileMode: isForceMobile || (root.width < 920)
    property int mobileTab: 0 // 0: 🎞️ 剪辑微调, 1: 📁 素材媒体池, 2: ✨ AI初筛

    // 全局剪辑工程状态
    property var mediaPool: []
    property var datesList: ["全部"]
    property string activeDate: "全部"
    property var selectedPaths: ({})

    property var timelineClips: []
    property int activeClipIndex: 0
    property bool isContinuousPlay: true
    property bool isTheaterMode: false

    property string masterCanvas: "horizontal"
    property string targetColor: "10bit"

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

    function toggleSelectAllInActiveDate() {
        var filtered = getFilteredMedia()
        var allSelected = true
        for (var i = 0; i < filtered.length; i++) {
            if (!selectedPaths[filtered[i].path]) {
                allSelected = false
                break
            }
        }
        var newSelected = Object.assign({}, selectedPaths)
        for (var j = 0; j < filtered.length; j++) {
            var p = filtered[j].path
            if (allSelected) {
                delete newSelected[p]
            } else {
                newSelected[p] = true
            }
        }
        selectedPaths = newSelected
    }

    function getSelectedCount() {
        var count = 0
        for (var k in selectedPaths) {
            if (selectedPaths[k]) count++
        }
        return count
    }

    function getSelectedPathsList() {
        var list = []
        for (var k in selectedPaths) {
            if (selectedPaths[k]) list.push(k)
        }
        return list
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
        var clip = Object.assign({}, timelineClips[activeClipIndex])
        var list = timelineClips.slice()
        list.splice(activeClipIndex + 1, 0, clip)
        timelineClips = list
        activeClipIndex = activeClipIndex + 1
        statusBanner.show("已复制当前片段并插入后方")
    }

    function deleteCurrentClip() {
        if (activeClipIndex < 0 || activeClipIndex >= timelineClips.length) return
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
            if (activeClipIndex < 0 || activeClipIndex >= timelineClips.length) return
            var curClip = timelineClips[activeClipIndex]
            var posSec = player.position / 1000.0

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

        function onMediaLoaded(dates, list) {
            root.mediaPool = list
            var d = ["全部"]
            for (var i = 0; i < dates.length; i++) d.push(dates[i])
            root.datesList = d
            root.activeDate = "全部"

            var initSel = {}
            for (var j = 0; j < list.length; j++) {
                initSel[list[j].path] = true
            }
            root.selectedPaths = initSel
            statusBanner.show("成功扫描到 " + list.length + " 个素材文件")
        }

        function onStatusMessage(msg) {
            statusBanner.show(msg)
        }

        function onScanError(err) {
            statusBanner.show("⚠️ 扫描提示: " + err)
        }

        function onAiProgress(percent, stepText) {
            aiModal.progress = percent
            aiModal.statusText = stepText
        }

        function onAiFinished(tasks) {
            aiModal.close()
            root.timelineClips = tasks
            if (tasks.length > 0) {
                root.loadActiveClip(0, false)
                statusBanner.show("✨ AI 自动筛选完成，共剪辑出 " + tasks.length + " 段精彩镜头！")
            } else {
                statusBanner.show("未筛选到片段，请检查筛选参数")
            }
        }

        function onExportProgress(percent, stepText) {
            exportModal.progress = percent
            exportModal.statusText = stepText
        }

        function onExportFinished(outPath) {
            exportModal.close()
            statusBanner.show("🎉 导出成功: " + outPath)
        }

        function onExportFailed(err) {
            exportModal.close()
            statusBanner.show("❌ 导出失败: " + err)
        }
    }

    Component.onCompleted: {
        bridge.scanDirectory("")
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

    // 主视口容器
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ==========================================
        // 1. 顶部全局导航栏 (自适应桌面 & 移动端)
        // ==========================================
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
                spacing: root.isMobileMode ? 8 : 12

                Text {
                    text: root.isMobileMode ? "🎬 DJI 剪辑 Pro" : "🎬 DJI 智能剪辑大师 Pro"
                    color: "#ffffff"
                    font.bold: true
                    font.pixelSize: root.isMobileMode ? 14 : 15
                }

                Item { Layout.fillWidth: true }

                // 成片概览 Pill (移动端以紧凑显示)
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
                                  ? ("⏱️ " + root.formatDuration(root.totalMovieDuration))
                                  : ("⏱️ 成片总长: " + root.formatDuration(root.totalMovieDuration) + " (" + root.timelineClips.length + " 段)")
                            color: "#f1f5f9"
                            font.pixelSize: 11
                            font.bold: true
                        }
                    }
                }

                // 响应式布局快捷切换测试开关 (可一键切换手机/桌面视图)
                Button {
                    text: root.isForceMobile ? "💻 切回桌面" : "📱 移动端布局"
                    onClicked: root.isForceMobile = !root.isForceMobile
                    background: Rectangle { color: root.isForceMobile ? "#1e3a8a" : "#222533"; radius: 5 }
                    contentItem: Text { text: parent.text; color: "#93c5fd"; font.pixelSize: 11; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }

                // 导出大按钮
                Button {
                    text: root.isMobileMode ? "🚀 导出" : "🚀 导出 4K60 10-Bit 母带"
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

        // ==========================================
        // 2. 核心工作台视图分支：
        //    分支 A: 📱 移动端紧凑专业流体布局 (isMobileMode == true)
        //    分支 B: 💻 桌面端三栏专业工作台 (isMobileMode == false)
        // ==========================================

        // ------------------------------------------
        // 分支 A: 📱 移动端专业布局 (高度自适应、触控友善)
        // ------------------------------------------
        ColumnLayout {
            visible: root.isMobileMode
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // 移动端监视器视窗 (占比约 36% 黄金比例)
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

                // 触摸视频画面快速播放/暂停
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        if (player.playbackState === MediaPlayer.PlayingState) player.pause()
                        else player.play()
                    }
                }

                // 浮层：全屏 & 剧场模式按钮
                RowLayout {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 8
                    spacing: 6

                    Rectangle {
                        color: "#cc0f172a"
                        radius: 4
                        width: 54; height: 22
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
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 8

                    Button {
                        text: "-1s"
                        Layout.preferredWidth: 38; Layout.preferredHeight: 28
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
                        text: "+1s"
                        Layout.preferredWidth: 38; Layout.preferredHeight: 28
                        onClicked: player.position = player.position + 1000
                        background: Rectangle { color: "#222533"; radius: 4 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }

                    // 码表
                    Text {
                        text: root.formatTime(player.position / 1000.0) + " / " + root.formatTime(root.totalMovieDuration)
                        color: "#38bdf8"
                        font.bold: true
                        font.pixelSize: 11
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: root.isContinuousPlay ? "🔁 连续" : "🔂 单段"
                        Layout.preferredHeight: 28
                        onClicked: root.isContinuousPlay = !root.isContinuousPlay
                        background: Rectangle { color: root.isContinuousPlay ? "#1d4ed8" : "#334155"; radius: 4 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }
                }
            }

            // 移动端中间可切换工作区 (由底部选项卡切换)
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // Tab 0: 🎞️ 移动端剪辑与时间线微调
                ColumnLayout {
                    anchors.fill: parent
                    visible: root.mobileTab === 0
                    spacing: 6

                    // A1. 胶片时间线横向滑动轨道 (触控大卡片)
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
                                width: Math.max(85, Math.min(200, Number(modelData.dur || 5) * 8))
                                height: 100
                                radius: 6
                                clip: true
                                color: root.activeClipIndex === index ? "#1e293b" : "#141620"
                                border.color: root.activeClipIndex === index ? "#38bdf8" : "#2d3345"
                                border.width: root.activeClipIndex === index ? 2 : 1

                                Image {
                                    anchors.fill: parent
                                    source: modelData.thumb
                                    fillMode: Image.PreserveAspectCrop
                                    opacity: 0.65
                                }

                                Rectangle {
                                    anchors.top: parent.top
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    height: 20
                                    color: "#cc0f172a"
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 4
                                        Text { text: "#" + (index + 1); color: "#ffffff"; font.bold: true; font.pixelSize: 10 }
                                        Item { Layout.fillWidth: true }
                                        Text { text: modelData.dur_str; color: "#38bdf8"; font.bold: true; font.pixelSize: 10 }
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: root.loadActiveClip(index, false)
                                }
                            }
                        }
                    }

                    // A2. 剪辑快捷操作栏 (分割/复制/删除/微调)
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        Layout.leftMargin: 8
                        Layout.rightMargin: 8
                        spacing: 6

                        Button {
                            text: "✂️ 分割"
                            Layout.fillWidth: true; Layout.preferredHeight: 36
                            onClicked: root.splitCurrentClip()
                            background: Rectangle { color: "#854d0e"; radius: 6 }
                            contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                        Button {
                            text: "➕ 复制"
                            Layout.fillWidth: true; Layout.preferredHeight: 36
                            onClicked: root.duplicateCurrentClip()
                            background: Rectangle { color: "#1e3a8a"; radius: 6 }
                            contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                        Button {
                            text: "🗑️ 删除"
                            Layout.fillWidth: true; Layout.preferredHeight: 36
                            onClicked: root.deleteCurrentClip()
                            background: Rectangle { color: "#7f1d1d"; radius: 6 }
                            contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                        Button {
                            text: "◀ 前移"
                            Layout.fillWidth: true; Layout.preferredHeight: 36
                            onClicked: root.moveCurrentClip(-1)
                            background: Rectangle { color: "#1e2230"; radius: 6 }
                            contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                        Button {
                            text: "后移 ▶"
                            Layout.fillWidth: true; Layout.preferredHeight: 36
                            onClicked: root.moveCurrentClip(1)
                            background: Rectangle { color: "#1e2230"; radius: 6 }
                            contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                    }

                    // A3. 大按钮 In/Out 毫秒级触控微调面板
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.margins: 8
                        color: "#14151d"
                        radius: 8
                        border.color: "#222533"

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 6

                            // 起点微调行
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "起点 In:"; color: "#cbd5e1"; font.pixelSize: 11; font.bold: true }
                                Text {
                                    text: root.timelineClips.length > root.activeClipIndex ? root.formatTime(root.timelineClips[root.activeClipIndex].start) : "00:00"
                                    color: "#38bdf8"; font.bold: true; font.pixelSize: 12
                                }
                                Item { Layout.fillWidth: true }
                                Button {
                                    text: "-2s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30
                                    onClicked: root.stepActiveIn(-2.0)
                                    background: Rectangle { color: "#222533"; radius: 4 }
                                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                }
                                Button {
                                    text: "-0.5s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30
                                    onClicked: root.stepActiveIn(-0.5)
                                    background: Rectangle { color: "#222533"; radius: 4 }
                                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                }
                                Button {
                                    text: "+0.5s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30
                                    onClicked: root.stepActiveIn(0.5)
                                    background: Rectangle { color: "#222533"; radius: 4 }
                                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                }
                                Button {
                                    text: "+2s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30
                                    onClicked: root.stepActiveIn(2.0)
                                    background: Rectangle { color: "#222533"; radius: 4 }
                                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                }
                            }

                            // 终点微调行
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "终点 Out:"; color: "#cbd5e1"; font.pixelSize: 11; font.bold: true }
                                Text {
                                    text: root.timelineClips.length > root.activeClipIndex ? root.formatTime(root.timelineClips[root.activeClipIndex].end) : "00:00"
                                    color: "#38bdf8"; font.bold: true; font.pixelSize: 12
                                }
                                Item { Layout.fillWidth: true }
                                Button {
                                    text: "-2s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30
                                    onClicked: root.stepActiveOut(-2.0)
                                    background: Rectangle { color: "#222533"; radius: 4 }
                                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                }
                                Button {
                                    text: "-0.5s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30
                                    onClicked: root.stepActiveOut(-0.5)
                                    background: Rectangle { color: "#222533"; radius: 4 }
                                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                }
                                Button {
                                    text: "+0.5s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30
                                    onClicked: root.stepActiveOut(0.5)
                                    background: Rectangle { color: "#222533"; radius: 4 }
                                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                }
                                Button {
                                    text: "+2s"; Layout.preferredWidth: 46; Layout.preferredHeight: 30
                                    onClicked: root.stepActiveOut(2.0)
                                    background: Rectangle { color: "#222533"; radius: 4 }
                                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                }
                            }

                            // 镜头详情提示
                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    text: "镜头: " + (root.timelineClips.length > root.activeClipIndex ? root.timelineClips[root.activeClipIndex].filename : "无")
                                    color: "#94a3b8"; font.pixelSize: 10; elide: Text.ElideMiddle; Layout.fillWidth: true
                                }
                                Text {
                                    text: "截取: " + (root.timelineClips.length > root.activeClipIndex ? root.timelineClips[root.activeClipIndex].dur_str : "0s")
                                    color: "#60a5fa"; font.bold: true; font.pixelSize: 11
                                }
                            }
                        }
                    }
                }

                // Tab 1: 📁 移动端素材池 (大缩略图触控列表)
                ColumnLayout {
                    anchors.fill: parent
                    visible: root.mobileTab === 1
                    spacing: 4

                    // 日期过滤条
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        color: "#161822"
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8; anchors.rightMargin: 8
                            spacing: 6
                            Repeater {
                                model: root.datesList
                                delegate: Rectangle {
                                    Layout.preferredHeight: 24; Layout.preferredWidth: 64
                                    radius: 4
                                    color: root.activeDate === modelData ? "#3b82f6" : "#222533"
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData
                                        color: "#ffffff"; font.pixelSize: 10; font.bold: root.activeDate === modelData
                                    }
                                    MouseArea { anchors.fill: parent; onClicked: root.activeDate = modelData }
                                }
                            }
                            Item { Layout.fillWidth: true }
                            Button {
                                text: "全选当前日期"
                                Layout.preferredHeight: 24
                                onClicked: root.toggleSelectAllInActiveDate()
                                background: Rectangle { color: "#272a38"; radius: 4 }
                                contentItem: Text { text: parent.text; color: "#93c5fd"; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }
                        }
                    }

                    // 移动端素材大列表
                    ListView {
                        id: mobileMediaList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 4
                        model: root.getFilteredMedia()

                        delegate: Rectangle {
                            width: mobileMediaList.width
                            height: 64
                            color: root.selectedPaths[modelData.path] ? "#1e2436" : "#14151d"
                            border.color: root.selectedPaths[modelData.path] ? "#3b82f6" : "#1f222e"
                            border.width: 1

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 8

                                CheckBox {
                                    checked: !!root.selectedPaths[modelData.path]
                                    onToggled: {
                                        var map = Object.assign({}, root.selectedPaths)
                                        if (checked) map[modelData.path] = true
                                        else delete map[modelData.path]
                                        root.selectedPaths = map
                                    }
                                }

                                Rectangle {
                                    Layout.preferredWidth: 70
                                    Layout.fillHeight: true
                                    radius: 4; clip: true; color: "#000000"
                                    Image { anchors.fill: parent; source: modelData.thumb; fillMode: Image.PreserveAspectCrop }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 2
                                    Text { text: modelData.filename; color: "#ffffff"; font.pixelSize: 11; font.bold: true; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                    RowLayout {
                                        spacing: 4
                                        Text { text: modelData.duration_str; color: "#94a3b8"; font.pixelSize: 10 }
                                        Rectangle {
                                            color: modelData.is_10bit ? "#8b5cf6" : "#334155"
                                            radius: 2
                                            Layout.preferredHeight: 14; Layout.preferredWidth: modelData.is_10bit ? 58 : 36
                                            Text { anchors.centerIn: parent; text: modelData.is_10bit ? "10-Bit D-Log" : "8-Bit"; color: "#ffffff"; font.pixelSize: 9; font.bold: true }
                                        }
                                    }
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                propagateComposedEvents: true
                                onClicked: (mouse) => {
                                    player.stop()
                                    player.source = "file://" + modelData.path
                                    player.position = 0
                                    player.play()
                                    mouse.accepted = false
                                }
                            }
                        }
                    }
                }

                // Tab 2: ✨ 移动端 AI 智能初筛快速操作
                ColumnLayout {
                    anchors.fill: parent
                    visible: root.mobileTab === 2
                    anchors.margins: 14
                    spacing: 12

                    Text { text: "✨ AI 一键智能剪辑策略："; color: "#ffffff"; font.bold: true; font.pixelSize: 13 }

                    Button {
                        text: "🏔️ 审美风景优先 (保留优质镜头/过滤抖动)"
                        Layout.fillWidth: true; Layout.preferredHeight: 44
                        onClicked: {
                            bridge.startAiScreening(root.mediaPool, root.getSelectedPathsList(), "1", 35.0, "horizontal", "10bit")
                            root.mobileTab = 0
                        }
                        background: Rectangle { color: "#312e81"; radius: 6; border.color: "#4338ca" }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }

                    Button {
                        text: "🎙️ 语音静音切分 (无声剔除 / 智能留声)"
                        Layout.fillWidth: true; Layout.preferredHeight: 44
                        onClicked: {
                            bridge.startAiScreening(root.mediaPool, root.getSelectedPathsList(), "2", 35.0, "horizontal", "10bit")
                            root.mobileTab = 0
                        }
                        background: Rectangle { color: "#1e293b"; radius: 6; border.color: "#334155" }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }

                    Button {
                        text: "⚡ 深度混合推荐 (多模态高光精选)"
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
                        text: "💡 提示：点击后将在后台全速进行高光初筛并自动将成片镜头排列至时间线。"
                        color: "#64748b"; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true
                    }
                }
            }

            // 移动端底部专用大拇指导航栏 (高度 52，触控防误触)
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                color: "#11131a"
                border.color: "#1e222d"
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    spacing: 0

                    // Tab 0
                    Rectangle {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        color: root.mobileTab === 0 ? "#1e2436" : "transparent"
                        ColumnLayout {
                            anchors.centerIn: parent; spacing: 2
                            Text { Layout.alignment: Qt.AlignHCenter; text: "🎞️"; font.pixelSize: 16 }
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
                            Text { Layout.alignment: Qt.AlignHCenter; text: "📁"; font.pixelSize: 16 }
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
                            Text { Layout.alignment: Qt.AlignHCenter; text: "✨"; font.pixelSize: 16 }
                            Text { Layout.alignment: Qt.AlignHCenter; text: "AI初筛"; color: root.mobileTab === 2 ? "#38bdf8" : "#94a3b8"; font.pixelSize: 10; font.bold: true }
                        }
                        MouseArea { anchors.fill: parent; onClicked: root.mobileTab = 2 }
                    }
                }
            }
        }

        // ------------------------------------------
        // 分支 B: 💻 桌面端三栏专业工作台 (当 !isMobileMode 时展示)
        // ------------------------------------------
        RowLayout {
            visible: !root.isMobileMode
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // 2.1 左栏：素材池
            Rectangle {
                visible: !root.isTheaterMode
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                color: "#121318"
                border.color: "#1f222e"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    // 日期过滤选项卡
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        color: "#161822"
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 6
                            Repeater {
                                model: root.datesList
                                delegate: Rectangle {
                                    Layout.preferredHeight: 26
                                    Layout.preferredWidth: 70
                                    radius: 4
                                    color: root.activeDate === modelData ? "#3b82f6" : "#222533"
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData
                                        color: "#ffffff"
                                        font.pixelSize: 11
                                        font.bold: root.activeDate === modelData
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: root.activeDate = modelData
                                    }
                                }
                            }
                        }
                    }

                    // 全选与已选计数
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        color: "#181a24"
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8
                            Button {
                                text: "全选当前日期素材"
                                onClicked: root.toggleSelectAllInActiveDate()
                                background: Rectangle { color: "#272a38"; radius: 4 }
                                contentItem: Text { text: parent.text; color: "#93c5fd"; font.pixelSize: 11; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: "已选 " + root.getSelectedCount() + " 个"
                                color: "#94a3b8"
                                font.pixelSize: 11
                            }
                        }
                    }

                    // 素材列表
                    ListView {
                        id: desktopMediaListView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: root.getFilteredMedia()
                        spacing: 4

                        delegate: Rectangle {
                            width: desktopMediaListView.width
                            height: 68
                            color: root.selectedPaths[modelData.path] ? "#1e2436" : "#14151d"
                            border.color: root.selectedPaths[modelData.path] ? "#3b82f6" : "transparent"
                            border.width: 1

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 8

                                CheckBox {
                                    checked: !!root.selectedPaths[modelData.path]
                                    onToggled: {
                                        var map = Object.assign({}, root.selectedPaths)
                                        if (checked) map[modelData.path] = true
                                        else delete map[modelData.path]
                                        root.selectedPaths = map
                                    }
                                }

                                Rectangle {
                                    Layout.preferredWidth: 80
                                    Layout.fillHeight: true
                                    radius: 4; clip: true; color: "#000000"
                                    Image {
                                        anchors.fill: parent
                                        source: modelData.thumb
                                        fillMode: Image.PreserveAspectCrop
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 2
                                    Text {
                                        text: modelData.filename
                                        color: "#ffffff"; font.pixelSize: 11; font.bold: true
                                        elide: Text.ElideMiddle; Layout.fillWidth: true
                                    }
                                    RowLayout {
                                        spacing: 4
                                        Text { text: modelData.duration_str; color: "#94a3b8"; font.pixelSize: 10 }
                                        Rectangle {
                                            color: modelData.is_10bit ? "#8b5cf6" : "#334155"
                                            radius: 2
                                            Layout.preferredHeight: 14; Layout.preferredWidth: modelData.is_10bit ? 58 : 36
                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData.is_10bit ? "10-Bit D-Log" : "8-Bit"
                                                color: "#ffffff"; font.pixelSize: 9; font.bold: true
                                            }
                                        }
                                    }
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                propagateComposedEvents: true
                                onClicked: (mouse) => {
                                    player.stop()
                                    player.source = "file://" + modelData.path
                                    player.position = 0
                                    player.play()
                                    mouse.accepted = false
                                }
                            }
                        }
                    }
                }
            }

            // 2.2 中栏：桌面大屏监视器
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#050608"

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        VideoOutput {
                            id: desktopMonitorOutput
                            anchors.fill: parent
                            fillMode: VideoOutput.PreserveAspectFit
                        }
                    }

                    // 监视器控制栏
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 48
                        color: "#11131a"
                        border.color: "#1e222d"
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 16
                            anchors.rightMargin: 16
                            spacing: 12

                            Button {
                                text: "-1s"
                                onClicked: player.position = Math.max(0, player.position - 1000)
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
                                text: "+1s"
                                onClicked: player.position = player.position + 1000
                                background: Rectangle { color: "#222533"; radius: 4 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Text {
                                text: root.formatTime(player.position / 1000.0) + " / " + root.formatTime(root.totalMovieDuration) + " (镜头 #" + (root.activeClipIndex + 1) + ")"
                                color: "#38bdf8"
                                font.bold: true
                                font.pixelSize: 12
                            }

                            Item { Layout.fillWidth: true }

                            Button {
                                text: root.isContinuousPlay ? "🔁 连续播放成片" : "🔂 单镜头循环"
                                onClicked: root.isContinuousPlay = !root.isContinuousPlay
                                background: Rectangle { color: root.isContinuousPlay ? "#1d4ed8" : "#334155"; radius: 4 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Button {
                                text: root.isTheaterMode ? "🗖 恢复视窗" : "🗖 剧场大屏"
                                onClicked: root.isTheaterMode = !root.isTheaterMode
                                background: Rectangle { color: "#222533"; radius: 4 }
                                contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }

                            Button {
                                text: "⛶ 全屏"
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
                Layout.preferredWidth: 320
                Layout.fillHeight: true
                color: "#121318"
                border.color: "#1f222e"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "镜头 #" + (root.activeClipIndex + 1)
                            color: "#ffffff"
                            font.bold: true
                            font.pixelSize: 15
                        }
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            color: "#3b82f6"; radius: 3
                            Layout.preferredHeight: 18; Layout.preferredWidth: 64
                            Text { anchors.centerIn: parent; text: "横屏 16:9"; color: "#ffffff"; font.pixelSize: 10; font.bold: true }
                        }
                        Rectangle {
                            color: "#8b5cf6"; radius: 3
                            Layout.preferredHeight: 18; Layout.preferredWidth: 64
                            Text { anchors.centerIn: parent; text: "10-Bit HDR"; color: "#ffffff"; font.pixelSize: 10; font.bold: true }
                        }
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
                            Text {
                                text: root.timelineClips.length > root.activeClipIndex ? root.formatTime(root.timelineClips[root.activeClipIndex].start) : "00:00"
                                color: "#38bdf8"; font.bold: true; font.pixelSize: 13
                            }
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
                            Text {
                                text: root.timelineClips.length > root.activeClipIndex ? root.formatTime(root.timelineClips[root.activeClipIndex].end) : "00:00"
                                color: "#38bdf8"; font.bold: true; font.pixelSize: 13
                            }
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
                        text: "✂️ 在指针位置分割 (Split)"; Layout.fillWidth: true; Layout.preferredHeight: 38
                        onClicked: root.splitCurrentClip()
                        background: Rectangle { color: "#854d0e"; radius: 6 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }
                    Button {
                        text: "➕ 此镜头再加一段 (Duplicate)"; Layout.fillWidth: true; Layout.preferredHeight: 38
                        onClicked: root.duplicateCurrentClip()
                        background: Rectangle { color: "#1e3a8a"; radius: 6 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }
                    Button {
                        text: "🗑️ 从时间线删掉 (Delete)"; Layout.fillWidth: true; Layout.preferredHeight: 38
                        onClicked: root.deleteCurrentClip()
                        background: Rectangle { color: "#7f1d1d"; radius: 6 }
                        contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }

        // ------------------------------------------
        // 桌面端底部胶片时间线 (只在 !isMobileMode 时展示)
        // ------------------------------------------
        Rectangle {
            visible: !root.isMobileMode
            Layout.fillWidth: true
            Layout.preferredHeight: 180
            color: "#0f1016"
            border.color: "#1e212d"
            border.width: 1

            ColumnLayout {
                anchors.fill: parent
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true; Layout.preferredHeight: 28
                    anchors.leftMargin: 12; anchors.rightMargin: 12
                    spacing: 8

                    Button { text: "✂️ 分割 (B)"; onClicked: root.splitCurrentClip(); background: Rectangle { color: "#1e2230"; radius: 4 } contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                    Button { text: "🗑️ 删除 (Del)"; onClicked: root.deleteCurrentClip(); background: Rectangle { color: "#1e2230"; radius: 4 } contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                    Button { text: "◀ 前移"; onClicked: root.moveCurrentClip(-1); background: Rectangle { color: "#1e2230"; radius: 4 } contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                    Button { text: "后移 ▶"; onClicked: root.moveCurrentClip(1); background: Rectangle { color: "#1e2230"; radius: 4 } contentItem: Text { text: parent.text; color: "#cbd5e1"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } }
                    Item { Layout.fillWidth: true }
                    Text { text: "💡 提示：点击时间线镜头选中并自动就位，双端把手微调长短"; color: "#64748b"; font.pixelSize: 11 }
                }

                ListView {
                    id: desktopTimelineView
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    orientation: ListView.Horizontal
                    clip: true
                    model: root.timelineClips
                    spacing: 8
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12

                    delegate: Rectangle {
                        width: Math.max(90, Math.min(260, Number(modelData.dur || 5) * 8))
                        height: 110
                        radius: 6; clip: true
                        color: root.activeClipIndex === index ? "#1e293b" : "#141620"
                        border.color: root.activeClipIndex === index ? "#38bdf8" : "#2d3345"
                        border.width: root.activeClipIndex === index ? 2 : 1

                        Image {
                            anchors.fill: parent
                            source: modelData.thumb
                            fillMode: Image.PreserveAspectCrop
                            opacity: 0.65
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
                            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                            height: 18; color: "#bb0f172a"
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 4
                                Text { text: root.formatTime(modelData.start); color: "#94a3b8"; font.pixelSize: 9 }
                                Item { Layout.fillWidth: true }
                                Text { text: root.formatTime(modelData.end); color: "#94a3b8"; font.pixelSize: 9 }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.loadActiveClip(index, false)
                        }
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
        height: 36
        radius: 18
        color: "#f80f172a"
        border.color: "#38bdf8"
        border.width: 1
        opacity: 0
        visible: opacity > 0

        Text {
            id: bannerText
            anchors.centerIn: parent
            color: "#ffffff"
            font.bold: true
            font.pixelSize: 12
        }

        Timer { id: bannerTimer; interval: 3500; onTriggered: bannerFadeOut.start() }
        NumberAnimation { id: bannerFadeOut; target: statusBanner; property: "opacity"; to: 0; duration: 400 }
        function show(msg) { bannerText.text = msg; statusBanner.opacity = 1; bannerTimer.restart() }
    }

    // 桌面端使用的 AI 筛选模态弹窗
    Dialog {
        id: aiModal
        anchors.centerIn: parent
        width: Math.min(480, root.width - 32)
        height: 340
        modal: true
        title: "✨ AI 智能初筛参数设置"
        background: Rectangle { color: "#161822"; border.color: "#2a2f42"; radius: 10 }

        property real progress: 0.0
        property string statusText: "准备就绪"

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Text { text: "选择智能剪辑模型策略："; color: "#cbd5e1"; font.pixelSize: 12 }
            ComboBox {
                id: aiModeCombo
                Layout.fillWidth: true
                model: ["Apple 神经网络视觉审美优先 (保留好风光/抖动过滤)", "纯语音静音切分 (无声剔除)", "深度混合智能识别"]
            }

            Text { text: "导出画幅标准："; color: "#cbd5e1"; font.pixelSize: 12 }
            ComboBox {
                id: canvasCombo
                Layout.fillWidth: true
                model: ["3840×2160 16:9 横屏母带", "2160×3840 9:16 竖屏大片"]
            }

            ProgressBar {
                Layout.fillWidth: true
                value: aiModal.progress / 100.0
            }

            Text {
                text: aiModal.statusText
                color: "#94a3b8"; font.pixelSize: 11
                Layout.fillWidth: true; elide: Text.ElideMiddle
            }

            RowLayout {
                Layout.fillWidth: true
                Button {
                    text: "取消任务"
                    onClicked: bridge.cancelCurrentJob()
                    background: Rectangle { color: "#334155"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "开始 AI 自动初筛"
                    onClicked: {
                        var mode = aiModeCombo.currentIndex === 0 ? "1" : (aiModeCombo.currentIndex === 1 ? "2" : "3")
                        var c = canvasCombo.currentIndex === 0 ? "horizontal" : "vertical"
                        bridge.startAiScreening(root.mediaPool, root.getSelectedPathsList(), mode, 35.0, c, "10bit")
                    }
                    background: Rectangle { color: "#4f46e5"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
            }
        }
    }

    // 导出模态弹窗
    Dialog {
        id: exportModal
        anchors.centerIn: parent
        width: Math.min(520, root.width - 32)
        height: 320
        modal: true
        title: "🚀 导出 4K 60fps 最终母带"
        background: Rectangle { color: "#161822"; border.color: "#2a2f42"; radius: 10 }

        property real progress: 0.0
        property string statusText: "准备压制..."

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Text {
                text: "✨ 规范：HEVC Main 10 (p010le) · BT.2020 · ARIB STD-B67 (HLG) · 65Mbps\n100% 符合 YouTube / B站 原生 4K60 HDR 审核标准！"
                color: "#38bdf8"
                font.pixelSize: 12
                lineHeight: 1.3
            }

            ProgressBar {
                Layout.fillWidth: true
                value: exportModal.progress / 100.0
            }

            Text {
                text: exportModal.statusText
                color: "#f1f5f9"; font.pixelSize: 12
                Layout.fillWidth: true; elide: Text.ElideMiddle
            }

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
                    text: "开始硬件加速渲染"
                    onClicked: {
                        bridge.startExport(root.timelineClips, root.masterCanvas, root.targetColor)
                    }
                    background: Rectangle { color: "#2563eb"; radius: 4 }
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: "选择 DJI 视频素材目录"
        onAccepted: {
            var path = folderDialog.selectedFolder.toString()
            if (path.indexOf("file://") === 0) path = path.substring(7)
            bridge.scanDirectory(path)
        }
    }
}
