import os
import sys
import json
import glob
import time
import shutil
import datetime
import subprocess
import threading
from PySide6.QtCore import QObject, Signal, Slot, Property, QThread

from backend.speech import run_speech_pipeline, generate_srt

DEFAULT_CARD_DIR = "/Volumes/OsmoNano/DCIM/DJI_001"
THUMB_CACHE_DIR = os.path.expanduser("~/.cache/sui_jian_thumbs")
CACHE_DIR = os.path.expanduser("~/.cache/sui_jian")
CHECKPOINT_FILE = os.path.join(CACHE_DIR, "ai_screening_checkpoint.json")
ANALYZER_BIN = os.path.expanduser("~/Desktop/.dji_vision_analyzer")
os.makedirs(THUMB_CACHE_DIR, exist_ok=True)
os.makedirs(CACHE_DIR, exist_ok=True)


def probe_file(filepath):
    """提取视频基础元数据"""
    try:
        cmd = [
            'ffprobe', '-v', 'quiet', '-print_format', 'json',
            '-show_format', '-show_streams', filepath
        ]
        res = subprocess.check_output(cmd, stderr=subprocess.DEVNULL)
        info = json.loads(res.decode())
        v_stream = next((s for s in info.get('streams', []) if s.get('codec_type') == 'video'), {})
        
        dur = float(info.get('format', {}).get('duration', 0.0))
        if dur <= 0:
            dur = float(v_stream.get('duration', 0.0))
            
        w = int(v_stream.get('width', 3840))
        h = int(v_stream.get('height', 2160))
        pix_fmt = v_stream.get('pix_fmt', 'yuv420p')
        is_10bit = ('10' in pix_fmt or 'p010' in pix_fmt or v_stream.get('bits_per_raw_sample') in ('10', 10))
        
        # 旋转与朝向检测
        rot = 0
        side_data = v_stream.get('side_data_list', [])
        for sd in side_data:
            if 'rotation' in sd:
                rot = int(sd['rotation'])
        if not rot:
            rot = int(v_stream.get('tags', {}).get('rotate', 0))
            
        transpose = None
        if rot == 90:
            transpose = 1
            w, h = h, w
        elif rot == 270 or rot == -90:
            transpose = 2
            w, h = h, w
            
        orient = 'vertical' if h > w else 'horizontal'
        
        # 拍摄日期解析
        mtime = os.path.getmtime(filepath)
        dt = datetime.datetime.fromtimestamp(mtime)
        creation_time = v_stream.get('tags', {}).get('creation_time') or info.get('format', {}).get('tags', {}).get('creation_time')
        if creation_time:
            try:
                dt = datetime.datetime.fromisoformat(creation_time.replace('Z', '+00:00'))
            except Exception:
                pass
        date_str = dt.strftime('%Y-%m-%d')
        time_str = dt.strftime('%H:%M:%S')

        # 缩略图生成与缓存
        base = os.path.basename(filepath)
        thumb_name = f"{base}.jpg"
        thumb_path = os.path.join(THUMB_CACHE_DIR, thumb_name)
        if not os.path.exists(thumb_path) or os.path.getsize(thumb_path) == 0:
            sample_time = min(1.0, max(0.1, dur * 0.1))
            cmd_thumb = [
                'ffmpeg', '-y', '-ss', str(sample_time), '-i', filepath,
                '-vframes', '1', '-vf', 'scale=480:-1', '-q:v', '5', thumb_path
            ]
            subprocess.run(cmd_thumb, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            
        return {
            "path": filepath,
            "filename": base,
            "duration": dur,
            "duration_str": f"{int(dur//60):02d}:{int(dur%60):02d}",
            "width": w,
            "height": h,
            "is_10bit": is_10bit,
            "color_desc": "10-Bit HDR (D-Log M)" if is_10bit else "8-Bit 标准",
            "orient": orient,
            "transpose": transpose,
            "date": date_str,
            "time": time_str,
            "thumb": f"file://{thumb_path}" if os.path.exists(thumb_path) else ""
        }
    except Exception as e:
        print(f"Probe error for {filepath}: {e}")
        return None


RECENT_CONFIG = os.path.expanduser("~/.cache/dji_editor_recent.json")

def load_recent_dirs():
    try:
        if os.path.exists(RECENT_CONFIG):
            with open(RECENT_CONFIG, "r", encoding="utf-8") as f:
                return json.load(f)
    except Exception:
        pass
    return []

def save_recent_dir(d):
    try:
        cur = load_recent_dirs()
        if d in cur:
            cur.remove(d)
        cur.insert(0, d)
        cur = cur[:5]
        with open(RECENT_CONFIG, "w", encoding="utf-8") as f:
            json.dump(cur, f, ensure_ascii=False)
    except Exception:
        pass

def find_dji_paths():
    candidates = [DEFAULT_CARD_DIR]
    for v in glob.glob('/Volumes/*'):
        dcim = os.path.join(v, 'DCIM')
        if os.path.exists(dcim):
            for sub in glob.glob(os.path.join(dcim, '*')):
                if os.path.isdir(sub):
                    candidates.append(sub)
    for c in candidates:
        if os.path.exists(c) and (glob.glob(os.path.join(c, "*.MP4")) or glob.glob(os.path.join(c, "*.mp4"))):
            return c
    return ""


class MediaImportWorker(QThread):
    finished = Signal(list, list, str) # dates, media_list, source_label
    error = Signal(str)

    def __init__(self, targets):
        super().__init__()
        self.targets = targets if isinstance(targets, list) else [targets]

    def run(self):
        try:
            valid_exts = {'.mp4', '.mov', '.mkv', '.m4v'}
            collected_files = []
            dir_for_recent = None

            for t in self.targets:
                if not t:
                    continue
                if t.startswith('file://'):
                    t = t[7:]
                t = os.path.abspath(os.path.expanduser(t))

                if os.path.isdir(t):
                    if not dir_for_recent:
                        dir_for_recent = t
                    for root, _, files in os.walk(t):
                        for f in files:
                            ext = os.path.splitext(f)[1].lower()
                            if ext in valid_exts and not f.startswith('.'):
                                collected_files.append(os.path.join(root, f))
                        if root != t and not any(k in root for k in ('DCIM', '100MEDIA', 'DJI')):
                            break
                elif os.path.isfile(t):
                    ext = os.path.splitext(t)[1].lower()
                    if ext in valid_exts and not os.path.basename(t).startswith('.'):
                        collected_files.append(t)
                        if not dir_for_recent:
                            dir_for_recent = os.path.dirname(t)

            collected_files = sorted(list(set(collected_files)))
            if not collected_files:
                self.error.emit("未找到可导入的有效视频文件（支持 .MP4 / .MOV）")
                return

            if dir_for_recent:
                save_recent_dir(dir_for_recent)

            media_list = []
            dates_set = set()
            for f in collected_files:
                meta = probe_file(f)
                if meta:
                    media_list.append(meta)
                    dates_set.add(meta["date"])

            dates = sorted(list(dates_set), reverse=True)
            source_label = dir_for_recent if dir_for_recent else f"{len(media_list)} 个视频文件"
            self.finished.emit(dates, media_list, source_label)
        except Exception as e:
            self.error.emit(str(e))


def load_ai_checkpoint(selected_paths):
    if os.path.exists(CHECKPOINT_FILE):
        try:
            with open(CHECKPOINT_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
                if data.get("selected_paths") == selected_paths:
                    return data.get("completed_idx", 0), data.get("tasks", [])
        except Exception:
            pass
    return 0, []

def save_ai_checkpoint(selected_paths, completed_idx, tasks):
    try:
        with open(CHECKPOINT_FILE, "w", encoding="utf-8") as f:
            json.dump({
                "selected_paths": selected_paths,
                "completed_idx": completed_idx,
                "tasks": tasks,
                "time": time.time()
            }, f, ensure_ascii=False)
    except Exception:
        pass

def clear_ai_checkpoint():
    try:
        if os.path.exists(CHECKPOINT_FILE):
            os.remove(CHECKPOINT_FILE)
    except Exception:
        pass


class DeviceHealthManager(QThread):
    deviceDisconnected = Signal(str)
    deviceReconnected = Signal(str, dict)

    def __init__(self, bridge):
        super().__init__()
        self.bridge = bridge
        self.active_root = ""
        self.known_fingerprints = {}
        self.is_online = True
        self._running = True

    def set_monitored_root(self, root_path, media_list):
        self.active_root = root_path
        self.known_fingerprints = {}
        for m in media_list:
            p = m.get("path")
            if p and os.path.exists(p):
                try:
                    self.known_fingerprints[p] = {
                        "size": os.path.getsize(p),
                        "mtime": os.path.getmtime(p),
                        "filename": m.get("filename", os.path.basename(p))
                    }
                except Exception:
                    pass
        self.is_online = os.path.exists(root_path) if root_path else True

    def stop(self):
        self._running = False

    def run(self):
        while self._running:
            try:
                if self.active_root:
                    now_online = os.path.exists(self.active_root)
                    if self.is_online and not now_online:
                        # 设备意外断开 / 误拔
                        self.is_online = False
                        dev_name = os.path.basename(self.active_root) or self.active_root
                        self.deviceDisconnected.emit(dev_name)
                        # 保护性自动暂停初筛并存盘
                        if self.bridge._ai_worker and self.bridge._ai_worker.isRunning():
                            self.bridge.pauseAiScreening("存储卡或设备意外拔出")
                    elif not self.is_online and now_online:
                        # 设备重新连接
                        self.is_online = True
                        dev_name = os.path.basename(self.active_root) or self.active_root
                        diff = self.reconcile_assets()
                        self.deviceReconnected.emit(dev_name, diff)
            except Exception as e:
                pass
            time.sleep(1.5)

    def reconcile_assets(self):
        missing = []
        modified = []
        valid_exts = {'.mp4', '.mov', '.mkv', '.m4v'}

        for p, fp in list(self.known_fingerprints.items()):
            if not os.path.exists(p):
                # 删 (丢失)
                missing.append({
                    "path": p,
                    "filename": fp["filename"],
                    "reason": "源视频已在设备中被移除"
                })
            else:
                try:
                    cur_sz = os.path.getsize(p)
                    cur_mt = os.path.getmtime(p)
                    # 改 (等同于删)
                    if cur_sz != fp["size"] or abs(cur_mt - fp["mtime"]) > 1.5:
                        modified.append({
                            "path": p,
                            "filename": fp["filename"],
                            "reason": "源视频已被重新录制覆盖（等同于删除）"
                        })
                        missing.append({
                            "path": p,
                            "filename": fp["filename"],
                            "reason": "源视频已被重新录制覆盖（原剪辑失效）"
                        })
                except Exception:
                    pass

        # 扫描是否有新拍摄的文件 (不自动加入现有工程)
        new_files = []
        try:
            for root, _, files in os.walk(self.active_root):
                for f in files:
                    ext = os.path.splitext(f)[1].lower()
                    if ext in valid_exts and not f.startswith('.'):
                        full_p = os.path.join(root, f)
                        if full_p not in self.known_fingerprints:
                            new_files.append(full_p)
                if root != self.active_root and not any(k in root for k in ('DCIM', '100MEDIA', 'DJI')):
                    break
        except Exception:
            pass

        return {
            "device": os.path.basename(self.active_root) or self.active_root,
            "missing": missing,
            "modified": modified,
            "new_count": len(new_files),
            "new_files": new_files[:5]
        }


class AiScreeningWorker(QThread):
    progress = Signal(float, str)
    finished = Signal(list)
    cancelled = Signal()
    paused = Signal(int, int, str) # completed, total, reason
    resumed = Signal()
    speechSegmentsReady = Signal(str, list)  # video_path, annotated segments (含 filler/retake 标记)

    def __init__(self, media_pool, selected_paths, ai_mode, max_dur, master_canvas, target_color):
        super().__init__()
        self.media_pool = {m["path"]: m for m in media_pool}
        self.selected_paths = selected_paths
        self.ai_mode = ai_mode
        self.max_dur = max_dur
        self.master_canvas = master_canvas
        self.target_color = target_color
        self._is_cancelled = False
        self._is_paused = False
        self._pause_reason = ""
        self._current_idx = 0
        self._tasks = []

    def cancel(self):
        self._is_cancelled = True

    def pause(self, reason="挂起暂停"):
        self._is_paused = True
        self._pause_reason = reason
        save_ai_checkpoint(self.selected_paths, self._current_idx, self._tasks)
        self.paused.emit(self._current_idx, len(self.selected_paths), reason)

    def resume(self):
        self._is_paused = False
        self.resumed.emit()

    def run(self):
        total = len(self.selected_paths)
        if total == 0:
            self.finished.emit([])
            return

        # 检查是否具备历史断点检查点
        start_idx, restored_tasks = load_ai_checkpoint(self.selected_paths)
        if start_idx > 0 and len(restored_tasks) > 0:
            self._tasks = restored_tasks
            self.progress.emit((start_idx / total) * 100.0, f"发现已留存的初筛检查点，直接从第 [{start_idx+1}/{total}] 个视频续接！")
        else:
            self._tasks = []
            start_idx = 0

        for idx in range(start_idx, total):
            self._current_idx = idx

            # 暂停与中断检测
            while self._is_paused and not self._is_cancelled:
                time.sleep(0.3)

            if self._is_cancelled:
                save_ai_checkpoint(self.selected_paths, idx, self._tasks)
                self.cancelled.emit()
                return

            path = self.selected_paths[idx]
            meta = self.media_pool.get(path)
            if not meta:
                continue

            self.progress.emit((idx / total) * 100.0, f"AI 智能筛选镜头 [{idx+1}/{total}]: {meta['filename']}")
            dur = meta["duration"]

            # ===== 口播连贯识别模式 (mode "4") =====
            if self.ai_mode == "4" and dur > 2.0:
                def _speech_progress(pct, msg):
                    overall = ((idx + pct) / total) * 100.0
                    self.progress.emit(overall, msg)

                speech_clips, annotated_segs = run_speech_pipeline(
                    path, meta, progress_callback=_speech_progress
                )
                if speech_clips:
                    self._tasks.extend(speech_clips)
                    if annotated_segs:
                        self.speechSegmentsReady.emit(path, annotated_segs)
                    save_ai_checkpoint(self.selected_paths, idx + 1, self._tasks)
                    continue
                # 口播识别无结果则 fallback 到混合模式

            # 静止/抖动与语音切分逻辑
            voice_blocks = []
            scenic_blocks = []

            # 语音检测
            if self.ai_mode in ("2", "3") and dur > 3.0:
                try:
                    cmd_s = ['ffmpeg', '-i', path, '-af', 'silencedetect=noise=-30dB:d=0.8', '-f', 'null', '-']
                    out_s = subprocess.run(cmd_s, stderr=subprocess.PIPE, text=True).stderr
                    silences = []
                    for line in out_s.split('\n'):
                        if 'silence_start:' in line:
                            st = float(line.split('silence_start:')[1].strip().split()[0])
                            silences.append(('start', st))
                        elif 'silence_end:' in line:
                            en = float(line.split('silence_end:')[1].strip().split()[0])
                            silences.append(('end', en))
                    cur_start = 0.0
                    for tag, val in silences:
                        if tag == 'start':
                            if val > cur_start + 1.2:
                                voice_blocks.append([cur_start, val])
                        elif tag == 'end':
                            cur_start = val
                    if dur > cur_start + 1.2:
                        voice_blocks.append([cur_start, dur])
                except Exception:
                    pass

            # CoreML / 视觉审美分析
            if self.ai_mode in ("1", "3") and os.path.exists(ANALYZER_BIN):
                sample_step = max(8.0, dur / 6.0)
                sp_times = [t for t in range(2, int(max(3, dur - 2)), int(sample_step))]
                for sp in sp_times:
                    if self._is_cancelled or self._is_paused:
                        break
                    tmp_j = f"/tmp/dji_plan_samp_{os.getpid()}_{sp}.jpg"
                    cmd_j = ['ffmpeg', '-y', '-ss', str(sp), '-i', path, '-vframes', '1', '-vf', 'scale=480:-1', '-q:v', '5', tmp_j]
                    subprocess.run(cmd_j, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    if os.path.exists(tmp_j):
                        try:
                            res_j = subprocess.check_output([ANALYZER_BIN, tmp_j], stderr=subprocess.DEVNULL).decode()
                            data_j = json.loads(res_j)
                            if data_j.get('is_scenic') or data_j.get('aesthetics', 0) > 0.45:
                                scenic_blocks.append([max(0.0, sp - 2.0), min(dur, sp + 6.0)])
                        except Exception:
                            pass
                        finally:
                            try: os.remove(tmp_j)
                            except Exception: pass

            raw_cands = voice_blocks + scenic_blocks
            if not raw_cands:
                raw_cands = [[0.0, min(dur, 25.0)]]
            raw_cands.sort(key=lambda x: x[0])
            merged = []
            for s_t, e_t in raw_cands:
                if not merged:
                    merged.append([s_t, e_t])
                else:
                    if s_t <= merged[-1][1] + 2.0:
                        merged[-1][1] = max(merged[-1][1], e_t)
                    else:
                        merged.append([s_t, e_t])

            for s_t, e_t in merged:
                clip_dur = e_t - s_t
                if clip_dur < 1.0:
                    continue
                self._tasks.append({
                    "src": path,
                    "filename": meta["filename"],
                    "start": round(s_t, 2),
                    "end": round(e_t, 2),
                    "dur": round(clip_dur, 2),
                    "dur_str": f"{clip_dur:.1f}s",
                    "orient": meta["orient"],
                    "is_10bit": meta["is_10bit"],
                    "thumb": meta["thumb"],
                    "transpose": meta.get("transpose")
                })

            # 每处理完一个视频，立即存盘检查点，确保误拔或断开时零损失
            save_ai_checkpoint(self.selected_paths, idx + 1, self._tasks)

        clear_ai_checkpoint()
        self.progress.emit(100.0, f"完成筛选，共生成 {len(self._tasks)} 段精选镜头")
        self.finished.emit(self._tasks)


class ExportWorker(QThread):
    progress = Signal(float, str)
    finished = Signal(str)
    failed = Signal(str)
    cancelled = Signal()

    def __init__(self, clips, master_canvas, target_color):
        super().__init__()
        self.clips = clips
        self.master_canvas = master_canvas
        self.target_color = target_color
        self._is_cancelled = False
        self._proc = None

    def cancel(self):
        self._is_cancelled = True
        if self._proc:
            try:
                self._proc.terminate()
            except Exception:
                pass

    def run(self):
        scratch = f"/tmp/dji_qt_render_{os.getpid()}"
        os.makedirs(scratch, exist_ok=True)
        rendered_slices = []

        try:
            total = len(self.clips)
            if total == 0:
                self.failed.emit("没有可导出的镜头")
                return

            # 画幅自适应 Filter
            filter_h2v = (
                '[0:v:0]split=2[fg_in][bg_in];'
                '[bg_in]scale=854:480,crop=270:480,boxblur=8:2,scale=2160:3840:flags=fast_bilinear,eq=brightness=-0.08:saturation=1.15[bg];'
                '[fg_in]scale=2160:1216[fg];'
                '[bg][fg]overlay=(W-w)/2:(H-h)/2[outv]'
            )
            filter_v2h = (
                '[0:v:0]split=2[fg_in][bg_in];'
                '[bg_in]scale=480:854,crop=480:270,boxblur=8:2,scale=3840:2160:flags=fast_bilinear,eq=brightness=-0.08:saturation=1.15[bg];'
                '[fg_in]scale=1216:2160[fg];'
                '[bg][fg]overlay=(W-w)/2:(H-h)/2[outv]'
            )

            # 10-Bit HDR 与 8-Bit 硬件编码配置
            if self.target_color == "10bit":
                enc_opts = [
                    '-c:v', 'hevc_videotoolbox',
                    '-profile:v', 'main10',
                    '-pix_fmt', 'p010le',
                    '-color_primaries', 'bt2020',
                    '-color_trc', 'arib-std-b67',
                    '-colorspace', 'bt2020nc',
                    '-color_range', 'tv',
                    '-b:v', '65M'
                ]
            else:
                enc_opts = [
                    '-c:v', 'hevc_videotoolbox',
                    '-pix_fmt', 'yuv420p',
                    '-b:v', '45M'
                ]

            for i, t in enumerate(self.clips):
                if self._is_cancelled:
                    self.cancelled.emit()
                    return

                p_val = (i / total) * 92.0
                self.progress.emit(p_val, f"正在硬件压制镜头 [{i+1}/{total}]: {t['filename']} ({t['dur']}s)")
                s_name = f"slice_{i:03d}_{t['filename']}_{int(t['start'])}_{int(t['end'])}.mp4"
                s_path = os.path.join(scratch, s_name)

                cmd = ['ffmpeg', '-y', '-ss', f"{t['start']:.3f}", '-t', f"{t['dur']:.3f}", '-i', t['src']]
                if self.master_canvas == 'vertical':
                    if t['orient'] == 'horizontal':
                        cmd += ['-filter_complex', filter_h2v, '-map', '[outv]', '-map', '0:a:0?']
                    elif t.get('transpose'):
                        cmd += ['-vf', f'transpose={t["transpose"]},scale=2160:3840', '-map', '0:v:0', '-map', '0:a:0?']
                    else:
                        cmd += ['-vf', 'scale=2160:3840', '-map', '0:v:0', '-map', '0:a:0?']
                else:
                    if t['orient'] == 'vertical':
                        cmd += ['-filter_complex', filter_v2h, '-map', '[outv]', '-map', '0:a:0?']
                    else:
                        cmd += ['-vf', 'scale=3840:2160', '-map', '0:v:0', '-map', '0:a:0?']

                cmd += enc_opts + ['-r', '59.94', '-tag:v', 'hvc1', '-c:a', 'aac', '-b:a', '320k', '-ar', '48000', s_path]

                self._proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self._proc.wait()
                if self._is_cancelled:
                    self.cancelled.emit()
                    return

                if os.path.exists(s_path) and os.path.getsize(s_path) > 10000:
                    rendered_slices.append(s_path)

            if len(rendered_slices) == 0:
                self.failed.emit("压制失败，没有切片成功生成")
                return

            # 无缝混流拼接
            self.progress.emit(94.0, "正在进行 4K 60fps 零损无缝拼接与流式优化...")
            concat_txt = os.path.join(scratch, "concat.txt")
            with open(concat_txt, "w") as f:
                for p in rendered_slices:
                    f.write(f"file '{p}'\n")

            now_str = datetime.datetime.now().strftime("%m月%d日_%H%M")
            out_name = f"DJI_精选大片_{'竖屏' if self.master_canvas=='vertical' else '横屏'}_{'10Bit_HDR母带' if self.target_color=='10bit' else '8Bit标准'}_{now_str}.mp4"
            out_file = os.path.join(os.path.expanduser("~/Downloads"), out_name)

            cmd_concat = [
                'ffmpeg', '-y', '-f', 'concat', '-safe', '0', '-i', concat_txt,
                '-c', 'copy', '-movflags', '+faststart', out_file
            ]
            self._proc = subprocess.Popen(cmd_concat, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self._proc.wait()

            if os.path.exists(out_file) and os.path.getsize(out_file) > 100000:
                self.progress.emit(100.0, "压制成功落盘！")
                self.finished.emit(out_file)
            else:
                self.failed.emit("最终合并失败，请检查硬盘空间")

        except Exception as e:
            self.failed.emit(str(e))
        finally:
            shutil.rmtree(scratch, ignore_errors=True)


class EditorBridge(QObject):
    # 状态与数据信号
    mediaLoaded = Signal(list, list, str) # dates, mediaList, sourceLabel
    djiFound = Signal(str)
    recentDirsLoaded = Signal(list)
    statusMessage = Signal(str)
    aiProgress = Signal(float, str)
    aiFinished = Signal(list)
    aiPaused = Signal(int, int, str)
    aiResumed = Signal()
    speechSegmentsReady = Signal(str, list)  # video_path, annotated word segments
    deviceDisconnected = Signal(str)
    deviceReconnected = Signal(str, dict)
    checkpointDetected = Signal(int, int)
    exportProgress = Signal(float, str)
    exportFinished = Signal(str)
    exportFailed = Signal(str)
    scanError = Signal(str)

    def __init__(self):
        super().__init__()
        self._scan_worker = None
        self._ai_worker = None
        self._export_worker = None
        self._health_manager = DeviceHealthManager(self)
        self._health_manager.deviceDisconnected.connect(self._on_device_disconnected)
        self._health_manager.deviceReconnected.connect(self._on_device_reconnected)
        self._health_manager.start()

    def _on_device_disconnected(self, dev_name):
        self.deviceDisconnected.emit(dev_name)
        self.statusMessage.emit(f"⚠️ 设备 [{dev_name}] 已断开（拔出/录像中）· 任务已安全挂起")

    def _on_device_reconnected(self, dev_name, diff):
        self.deviceReconnected.emit(dev_name, diff)
        self.statusMessage.emit(f"✅ 设备 [{dev_name}] 已重新连接！")

    def close(self):
        if hasattr(self, "_health_manager") and self._health_manager:
            self._health_manager.stop()
            self._health_manager.wait()
        if self._scan_worker and self._scan_worker.isRunning():
            self._scan_worker.wait()
        if self._ai_worker and self._ai_worker.isRunning():
            self._ai_worker.cancel()
            self._ai_worker.wait()
        if self._export_worker and self._export_worker.isRunning():
            self._export_worker.cancel()
            self._export_worker.wait()

    @Slot(str)
    def scanDirectory(self, path=""):
        target = path.strip() if path.strip() else DEFAULT_CARD_DIR
        self.importPaths([target])

    @Slot(list)
    def importPaths(self, paths):
        if not paths:
            return
        clean_paths = []
        for p in paths:
            if not p:
                continue
            s = str(p)
            if s.startswith('file://'):
                s = s[7:]
            clean_paths.append(s)
        self.statusMessage.emit(f"正在导入 {len(clean_paths)} 项素材...")
        if self._scan_worker and self._scan_worker.isRunning():
            self._scan_worker.wait()
        self._scan_worker = MediaImportWorker(clean_paths)
        self._scan_worker.finished.connect(self._on_import_finished)
        self._scan_worker.error.connect(self.scanError.emit)
        self._scan_worker.start()

    def _on_import_finished(self, dates, media_list, source_label):
        self.mediaLoaded.emit(dates, media_list, source_label)
        self.loadRecentDirs()
        root_dir = source_label if (source_label and os.path.exists(source_label)) else ""
        if not root_dir and media_list:
            first_p = media_list[0].get("path", "")
            if first_p:
                root_dir = os.path.dirname(first_p)
        self._health_manager.set_monitored_root(root_dir, media_list)
        self.statusMessage.emit(f"已成功加载 {len(media_list)} 个素材 ({source_label})")

    @Slot()
    def detectDji(self):
        found = find_dji_paths()
        if found:
            self.djiFound.emit(found)
            self.statusMessage.emit(f"检测到已连接的 DJI 相机存储卡: {found}")
        else:
            self.djiFound.emit("")
            self.statusMessage.emit("未检测到连接的 DJI 相机，可手动选择文件夹或视频")

    @Slot()
    def loadRecentDirs(self):
        self.recentDirsLoaded.emit(load_recent_dirs())

    @Slot(list, list, str, float, str, str)
    def startAiScreening(self, media_pool, selected_paths, ai_mode, max_dur, master_canvas, target_color):
        if self._ai_worker and self._ai_worker.isRunning():
            self._ai_worker.cancel()
            self._ai_worker.wait()
        self._ai_worker = AiScreeningWorker(media_pool, selected_paths, ai_mode, max_dur, master_canvas, target_color)
        self._ai_worker.progress.connect(self.aiProgress.emit)
        self._ai_worker.finished.connect(self.aiFinished.emit)
        self._ai_worker.paused.connect(self.aiPaused.emit)
        self._ai_worker.resumed.connect(self.aiResumed.emit)
        self._ai_worker.speechSegmentsReady.connect(self.speechSegmentsReady.emit)
        self._ai_worker.start()

    @Slot()
    def pauseAiScreening(self, reason="用户主动暂停"):
        if self._ai_worker and self._ai_worker.isRunning():
            self._ai_worker.pause(reason)
            self.statusMessage.emit(f"⏸️ AI 任务已暂停 ({reason}) · 断点已安全存盘")

    @Slot()
    def resumeAiScreening(self):
        if self._ai_worker and self._ai_worker.isRunning():
            self._ai_worker.resume()
            self.statusMessage.emit("▶️ AI 任务已从断点恢复执行")

    @Slot()
    def safeEjectDevice(self):
        """用户主动请求安全拔出设备去录像"""
        if self._ai_worker and self._ai_worker.isRunning():
            self._ai_worker.pause("准备拔出设备录像")
        self.statusMessage.emit("✅ 检查点已留存！现在可安全拔出相机去录像，插回后可随时续接。")

    @Slot(result=bool)
    def hasCheckpoint(self):
        return os.path.exists(CHECKPOINT_FILE)

    @Slot()
    def clearCheckpoint(self):
        clear_ai_checkpoint()
        self.statusMessage.emit("已清除历史初筛检查点")

    @Slot(list, str, str)
    def startExport(self, clips, master_canvas, target_color):
        if self._export_worker and self._export_worker.isRunning():
            self._export_worker.cancel()
        self._export_worker = ExportWorker(clips, master_canvas, target_color)
        self._export_worker.progress.connect(self.exportProgress.emit)
        self._export_worker.finished.connect(self.exportFinished.emit)
        self._export_worker.failed.connect(self.exportFailed.emit)
        self._export_worker.start()

    @Slot()
    def cancelCurrentJob(self):
        if self._ai_worker and self._ai_worker.isRunning():
            self._ai_worker.cancel()
        if self._export_worker and self._export_worker.isRunning():
            self._export_worker.cancel()
        self.statusMessage.emit("任务已由用户主动取消")

    @Slot(list, str)
    def generateSrt(self, clips, output_path):
        """从剪辑片段生成 SRT 字幕文件"""
        try:
            if not output_path:
                output_path = os.path.join(
                    os.path.expanduser("~/Downloads"),
                    f"随剪_字幕_{datetime.datetime.now().strftime('%m月%d日_%H%M')}.srt"
                )
            generate_srt(clips, output_path)
            self.statusMessage.emit(f"字幕已导出: {output_path}")
        except Exception as e:
            self.statusMessage.emit(f"字幕导出失败: {e}")
