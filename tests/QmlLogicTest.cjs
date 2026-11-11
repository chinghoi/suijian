const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../qml/AppWindow.qml'), 'utf8');
function blockEnd(start) {
    let depth = 0, quote = '', comment = '';
    for (let i = start; i < source.length; i++) {
        const c = source[i], n = source[i+1];
        if (comment === '//') { if (c === '\n') comment = ''; continue; }
        if (comment === '/*') { if (c === '*' && n === '/') { comment = ''; i++; } continue; }
        if (quote) { if (c === '\\') i++; else if (c === quote) quote = ''; continue; }
        if (c === '/' && (n === '/' || n === '*')) { comment = c+n; i++; continue; }
        if (c === '"' || c === "'" || c === '`') { quote = c; continue; }
        if (c === '{') depth++;
        if (c === '}' && --depth === 0) return i;
    }
    throw Error('unclosed QML block');
}
for (const match of source.matchAll(/:\s*function\s*\(([^)]*)\)\s*\{/g)) {
    const open = match.index + match[0].lastIndexOf("{");
    new Function(match[1], source.slice(open + 1, blockEnd(open)));
}
const functions = [];
for (const match of source.matchAll(/function\s+(\w+)\s*\(([^)]*)\)\s*\{/g)) {
    const open = match.index + match[0].lastIndexOf('{');
    const body = source.slice(open + 1, blockEnd(open));
    new Function(match[2], body); // Parse every named JS function in the QML file.
    functions.push('function ' + match[1] + '(' + match[2] + '){' + body + '}');
}
for (const match of source.matchAll(/on[A-Z]\w*\s*:\s*\{/g)) {
    const open = match.index + match[0].lastIndexOf('{');
    new Function(source.slice(open + 1, blockEnd(open)));
}
const MediaPlayer = {LoadingMedia: 1, LoadedMedia: 2, BufferedMedia: 3, BufferingMedia: 4,
    EndOfMedia: 5, PlayingState: 6, PausedState: 7};
const player = {stop() { this.playbackState = 0; }, pause() { this.playbackState = MediaPlayer.PausedState; },
    play() { this.playbackState = MediaPlayer.PlayingState; }, source: '', position: 0,
    mediaStatus: MediaPlayer.LoadingMedia};
const context = {player, MediaPlayer, Qt: {callLater() {}, platform: {os: "linux"}},
    bridge: {setBeatMusic() {}, setExportLut() {}, snapToMusicAsync() {}, localMediaUrl(path) { return 'file://' + path; }},
    timelineSync: {restart() {}, stop() {}}, recoveryTimer: {restart() {}},
    toolModal: {open() {}}, statusBanner: {show() {}},
    aiModal: {}, exportModal: {}, Math, JSON};
context.clipIdCounter = 0;
context.root = context;
vm.createContext(context);
vm.runInContext(functions.join('\n'), context);
const a = {src:'/a.mov', start:0, end:2, dur:2};
const b = {src:'/b.mov', start:4, end:7, dur:3};
context.timelineClips = [b]; context.playingClip = a; context.activeClipIndex = 0;
context.synchronizeTimeline();
assert.equal(player.source, 'file:///b.mov');
context.applyMonitorLoad();
assert.equal(player.position, 0);
player.mediaStatus = MediaPlayer.LoadedMedia;
context.applyMonitorLoad();
assert.equal(player.position, 4000);
assert.equal(player.playbackState, MediaPlayer.PausedState);
context.timelineClips = [b, a]; context.playingClip = a;
context.synchronizeTimeline();
assert.equal(context.activeClipIndex, 1);
assert.equal(context.pendingMonitorLoad.position, 0);
context.applyMonitorLoad();
assert.equal(player.position, 0);
assert.equal(player.source, 'file:///a.mov');
context.timelineClips = [{...a, clip_id:"first"}, {...a, clip_id:"duplicate"}];
context.playingClip = context.timelineClips[1]; context.activeClipIndex = 1;
context.synchronizeTimeline();
assert.equal(context.activeClipIndex, 1);
context.timelineClips = [];
context.synchronizeTimeline();
assert.equal(player.source, ''); assert.equal(context.activeClipIndex, -1);
assert.equal(context.pendingMonitorLoad, null);
context.restoreProject({clips:[b], canvas:'vertical', color:'8bit', beatMusic:'/music.wav', active:0});
context.synchronizeTimeline();
assert.equal(player.source, 'file:///b.mov');
assert.equal(context.canvasChoice, 'vertical'); assert.equal(context.beatMusic, '/music.wav');
context.Qt.platform.os = "osx";
player.position = 0;
context.applyMonitorLoad();
assert.equal(context.monitorPriming, true);
assert.equal(player.playbackState, MediaPlayer.PlayingState);
player.position = 100;
context.applyMonitorLoad();
assert.equal(player.position, 4000);
assert.equal(context.monitorPriming, false);
assert.equal(player.playbackState, MediaPlayer.PausedState);
context.aiModal.isRunning = true; context.aiModal.isStopping = true;
context.onAiCancelled();
assert.equal(context.aiModal.isRunning, false); assert.equal(context.aiModal.isStopping, false);
const ready = {mediaError: "", hasSubtitles: true, directoryUnavailable: false, lutMissing: false};
assert.match(context.exportBlockedReason(0, 0, false, ready, "8bit"), /时间线/);
assert.match(context.exportBlockedReason(4, 1, true, ready, "8bit"), /任务/);
assert.match(context.exportBlockedReason(3, 1, false, {...ready, hasSubtitles: false}, "8bit"), /语音转写/);
assert.match(context.exportBlockedReason(4, 1, false, {...ready, directoryUnavailable: true}, "8bit"), /文件夹/);
const offline = {...ready, mediaError: "找不到素材"};
for (const type of [0, 1, 2]) assert.equal(context.exportBlockedReason(type, 1, false, offline, "8bit"), "找不到素材");
for (const type of [3, 4, 5]) assert.equal(context.exportBlockedReason(type, 1, false, offline, "8bit"), "");
assert.match(context.exportBlockedReason(0, 1, false, {...ready, lutMissing: true}, "8bit"), /调色/);
assert.equal(context.exportBlockedReason(0, 1, false, ready, "8bit"), "");
let shownMessage = null;
context.statusBanner.show = (...args) => { shownMessage = args; };
context.restoringProject = true;
context.onStatusMessage("已清除 LUT");
assert.equal(shownMessage, null);
context.restoringProject = false;
context.onStatusMessage("无法读取素材");
assert.equal(shownMessage[1], true);
context.followExport = true;
context.exportModal.isRunning = true;
context.onExportFinished("/downloads/final.mp4");
assert.equal(context.exportModal.completedPath, "/downloads/final.mp4");
assert.equal(context.exportModal.isRunning, false);
assert.equal(context.followExport, false);
console.log('QML JavaScript syntax, timeline sync, project restore and cancellation checks passed');
