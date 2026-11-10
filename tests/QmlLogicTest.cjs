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
const player = {stop() { this.stopped = true; }, source: '', position: 0};
const context = {player, bridge: {setBeatMusic() {}, setExportLut() {}, snapToMusicAsync() {}},
    timelineSync: {restart() {}}, recoveryTimer: {restart() {}},
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
assert.equal(player.position, 4000);
context.timelineClips = [b, a]; context.playingClip = a;
context.synchronizeTimeline();
assert.equal(context.activeClipIndex, 1);
assert.equal(player.source, 'file:///a.mov');
context.timelineClips = [{...a, clip_id:"first"}, {...a, clip_id:"duplicate"}];
context.playingClip = context.timelineClips[1]; context.activeClipIndex = 1;
context.synchronizeTimeline();
assert.equal(context.activeClipIndex, 1);
context.timelineClips = [];
context.synchronizeTimeline();
assert.equal(player.source, ''); assert.equal(context.activeClipIndex, -1);
context.restoreProject({clips:[b], canvas:'vertical', color:'8bit', beatMusic:'/music.wav', active:0});
context.synchronizeTimeline();
assert.equal(player.source, 'file:///b.mov');
assert.equal(context.canvasChoice, 'vertical'); assert.equal(context.beatMusic, '/music.wav');
context.aiModal.isRunning = true; context.aiModal.isStopping = true;
context.onAiCancelled();
assert.equal(context.aiModal.isRunning, false); assert.equal(context.aiModal.isStopping, false);
console.log('QML JavaScript syntax, timeline sync, project restore and cancellation checks passed');
