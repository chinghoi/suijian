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
        if not rot:
            rot = int(info.get('format', {}).get('tags', {}).get('rotate', 0))
            
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

def scan_camera_volumes():
    r"""
    跨平台扫描已挂载的相机存储外设与可移动磁盘
    macOS: /Volumes/*
    Windows: D:\ ~ Z:\
    Linux: /media/*, /run/media/*, /mnt/*
    """
    found = []
    candidates = []
    if sys.platform == 'darwin':
        for v in glob.glob('/Volumes/*'):
            b = os.path.basename(v)
            if b in ('Macintosh HD', 'Recovery', 'Preboot', 'VM', 'Update'):
                continue
            candidates.append((b, v))
    elif sys.platform == 'win32':
        import string
        for l in string.ascii_uppercase[2:]:
            root = f"{l}:\\"
            if os.path.exists(root):
                candidates.append((f"Disk ({l}:)", root))
    else:
        for p in glob.glob('/media/*/*') + glob.glob('/run/media/*/*'):
            if os.path.isdir(p):
                candidates.append((os.path.basename(p), p))

    valid_exts = ('.mp4', '.mov', '.mkv', '.m4v')
    for name, v_root in candidates:
        if not os.path.exists(v_root):
            continue
        media_dir = ''
        files = []
        dcim = os.path.join(v_root, 'DCIM')
        if os.path.exists(dcim):
            for sub in sorted(glob.glob(os.path.join(dcim, '*'))):
                if os.path.isdir(sub):
                    try:
                        vids = [f for f in os.listdir(sub) if f.lower().endswith(valid_exts) and not f.startswith('.')]
                        if vids:
                            media_dir = sub
                            files.extend(vids)
                    except Exception:
                        pass
            if not media_dir:
                try:
                    vids = [f for f in os.listdir(dcim) if f.lower().endswith(valid_exts) and not f.startswith('.')]
                    if vids:
                        media_dir = dcim
                        files.extend(vids)
                except Exception:
                    pass
        if not media_dir:
            sony_dir = os.path.join(v_root, 'PRIVATE', 'M4ROOT', 'CLIP')
            if os.path.exists(sony_dir):
                try:
                    vids = [f for f in os.listdir(sony_dir) if f.lower().endswith(valid_exts) and not f.startswith('.')]
                    if vids:
                        media_dir = sony_dir
                        files.extend(vids)
                except Exception:
                    pass
        if not media_dir:
            try:
                vids = [f for f in os.listdir(v_root) if f.lower().endswith(valid_exts) and not f.startswith('.')]
                if vids:
                    media_dir = v_root
                    files.extend(vids)
            except Exception:
                pass
        if media_dir and files:
            found.append({
                'dev_name': name,
                'root': v_root,
                'media_dir': media_dir,
                'count': len(files)
            })
    return found


def find_dji_paths():
    scanned = scan_camera_volumes()
    if scanned:
        return scanned[0]["media_dir"]
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
    progress = Signal(int, int, str)   # current, total, filename
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

            total_files = len(collected_files)
            completed_count = 0
            media_list = []
            dates_set = set()

            from concurrent.futures import ThreadPoolExecutor, as_completed

            # 最高 8 线程并发极速提取视频元数据与缩略图，避免单线程 USB 阻塞
            max_workers = min(8, max(2, os.cpu_count() or 4))
            with ThreadPoolExecutor(max_workers=max_workers) as executor:
                future_to_file = {executor.submit(probe_file, f): f for f in collected_files}
                for future in as_completed(future_to_file):
                    f = future_to_file[future]
                    completed_count += 1
                    base_name = os.path.basename(f)
                    self.progress.emit(completed_count, total_files, base_name)
                    try:
                        meta = future.result()
                        if meta:
                            media_list.append(meta)
                            dates_set.add(meta["date"])
                    except Exception:
                        pass

            media_list.sort(key=lambda x: x["path"])
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
    usbDeviceDetected = Signal(str, str, int)  # dev_name, media_dir, file_count
    usbDeviceRemoved = Signal(str)            # dev_name

    def __init__(self, bridge):
        super().__init__()
        self.bridge = bridge
        self.active_root = ""
        self.known_fingerprints = {}
        self.is_online = True
        self._running = True
        self._known_volumes = {}

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
        try:
            initial = scan_camera_volumes()
            for d in initial:
                self._known_volumes[d["root"]] = d
        except Exception:
            pass

        while self._running:
            try:
                # 1. 跨平台实时扫描相机及移动外设插拔
                current_devices = scan_camera_volumes()
                cur_roots = {d["root"]: d for d in current_devices}

                # 插入识别
                for r, info in cur_roots.items():
                    if r not in self._known_volumes:
                        self._known_volumes[r] = info
                        self.usbDeviceDetected.emit(info["dev_name"], info["media_dir"], info["count"])

                # 拔出识别
                for r in list(self._known_volumes.keys()):
                    if r not in cur_roots:
                        old_info = self._known_volumes.pop(r)
                        self.usbDeviceRemoved.emit(old_info["dev_name"])

                # 2. 检查当前被编辑素材所在根目录的存活状态
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
            except Exception:
                pass
            time.sleep(1.2)

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
            self.progress.emit(max(1.0, (start_idx / total) * 100.0), f"发现已留存的初筛检查点，直接从第 [{start_idx+1}/{total}] 个视频续接！")
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

            p_init = max(1.0, (idx / total) * 100.0)
            self.progress.emit(p_init, f"[{idx+1}/{total}] 准备初筛分析: {meta['filename']}")
            dur = meta["duration"]

            # ===== 1. 口播模式 (mode 4) 或 深度综合多模态混合模式 (mode 3) =====
            speech_clips = []
            annotated_segs = []
            if self.ai_mode in ("3", "4") and dur > 2.0:
                def _speech_progress(pct, msg):
                    overall = max(1.0, min(99.0, ((idx + pct * 0.6) / total) * 100.0))
                    self.progress.emit(overall, f"[{idx+1}/{total}] {msg}")

                speech_clips, annotated_segs = run_speech_pipeline(
                    path, meta, progress_callback=_speech_progress
                )
                if annotated_segs:
                    self.speechSegmentsReady.emit(path, annotated_segs)

            # 模式 4：纯口播智能粗剪
            if self.ai_mode == "4":
                if speech_clips:
                    self._tasks.extend(speech_clips)
                    save_ai_checkpoint(self.selected_paths, idx + 1, self._tasks)
                    continue

            # 模式 3：深度综合多模态混合 (口播主体 + 间歇优质空镜抽选 + 静音死寂剔除)
            if self.ai_mode == "3" and speech_clips:
                current_file_tasks = list(speech_clips)

                # 计算人声区间之外的间歇段落 (大于 3.5 秒的停顿或空镜)
                covered_spans = [(c["start"], c["end"]) for c in speech_clips]
                covered_spans.sort(key=lambda x: x[0])

                gaps = []
                cur_pos = 0.0
                for s_st, s_en in covered_spans:
                    if s_st - cur_pos >= 3.5:
                        gaps.append((cur_pos, s_st))
                    cur_pos = max(cur_pos, s_en)
                if dur - cur_pos >= 3.5:
                    gaps.append((cur_pos, dur))

                # 对空镜间隙执行 Apple 神经网络视觉审美抽样
                if os.path.exists(ANALYZER_BIN):
                    for g_st, g_en in gaps:
                        if self._is_cancelled or self._is_paused:
                            break
                        gap_mid = (g_st + g_en) / 2.0
                        tmp_j = f"/tmp/dji_plan_samp_{os.getpid()}_{int(gap_mid)}.jpg"
                        cmd_j = ['ffmpeg', '-y', '-ss', str(gap_mid), '-i', path, '-vframes', '1', '-vf', 'scale=480:-1', '-q:v', '5', tmp_j]
                        subprocess.run(cmd_j, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                        if os.path.exists(tmp_j):
                            try:
                                res_j = subprocess.check_output([ANALYZER_BIN, tmp_j], stderr=subprocess.DEVNULL).decode()
                                data_j = json.loads(res_j)
                                if data_j.get('is_scenic') or data_j.get('aesthetics', 0) > 0.45:
                                    broll_dur = min(g_en - g_st, 6.0)
                                    broll_start = round(g_st + 0.5, 2)
                                    broll_end = round(broll_start + broll_dur, 2)
                                    current_file_tasks.append({
                                        "src": path,
                                        "filename": meta["filename"],
                                        "start": broll_start,
                                        "end": broll_end,
                                        "dur": round(broll_end - broll_start, 2),
                                        "dur_str": f"{broll_end - broll_start:.1f}s",
                                        "orient": meta["orient"],
                                        "is_10bit": meta["is_10bit"],
                                        "thumb": meta["thumb"],
                                        "transpose": meta.get("transpose"),
                                        "text": "精选空镜 B-Roll",
                                        "subtitles": []
                                    })
                            except Exception:
                                pass
                            finally:
                                try: os.remove(tmp_j)
                                except Exception: pass

                current_file_tasks.sort(key=lambda x: x["start"])
                self._tasks.extend(current_file_tasks)
                save_ai_checkpoint(self.selected_paths, idx + 1, self._tasks)
                continue

            # 纯语音静音检测 (模式 2，或模式 3 无人声时的兜底)
            voice_blocks = []
            scenic_blocks = []

            if self.ai_mode in ("2", "3") and dur > 3.0:
                p_sil = max(1.0, min(99.0, ((idx + 0.35) / total) * 100.0))
                self.progress.emit(p_sil, f"[{idx+1}/{total}] 分析音频静音与人声区间: {meta['filename']}")
                try:
                    cmd_s = ['ffmpeg', '-y', '-i', path, '-map', '0:a:0', '-af', 'silencedetect=noise=-30dB:d=0.8', '-f', 'null', '-']
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

            # CoreML / 视觉审美分析 (模式 1，或模式 3 无人声时的兜底)
            if self.ai_mode in ("1", "3") and os.path.exists(ANALYZER_BIN):
                p_vis = max(1.0, min(99.0, ((idx + 0.65) / total) * 100.0))
                self.progress.emit(p_vis, f"[{idx+1}/{total}] 评估视觉画面与稳定构图: {meta['filename']}")
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

        # 目标成片节奏 / 时长预算控制 (max_dur > 0 时生效)
        if self.max_dur > 0 and self._tasks:
            total_current_dur = sum(c.get("dur", 0.0) for c in self._tasks)
            if total_current_dur > self.max_dur:
                # 优先保留有口播叙事的片段，其次保留高美学空镜
                speech_tasks = [t for t in self._tasks if t.get("text") and t.get("text") != "精选空镜 B-Roll"]
                scenic_tasks = [t for t in self._tasks if not (t.get("text") and t.get("text") != "精选空镜 B-Roll")]

                budget_left = self.max_dur
                selected_speech = []
                for st in speech_tasks:
                    if budget_left - st.get("dur", 0.0) >= -5.0:
                        selected_speech.append(st)
                        budget_left -= st.get("dur", 0.0)
                    else:
                        break

                selected_scenic = []
                for sc in scenic_tasks:
                    if budget_left - sc.get("dur", 0.0) >= 0:
                        selected_scenic.append(sc)
                        budget_left -= sc.get("dur", 0.0)
                    elif budget_left > 2.0:
                        trimmed_dur = round(budget_left, 2)
                        sc_copy = dict(sc)
                        sc_copy["end"] = round(sc_copy["start"] + trimmed_dur, 2)
                        sc_copy["dur"] = trimmed_dur
                        sc_copy["dur_str"] = f"{trimmed_dur:.1f}s"
                        selected_scenic.append(sc_copy)
                        budget_left = 0
                        break

                final_tasks = selected_speech + selected_scenic
                final_tasks.sort(key=lambda x: (x.get("src", ""), x.get("start", 0.0)))
                self._tasks = final_tasks

        clear_ai_checkpoint()
        self.progress.emit(100.0, f"完成初筛，共生成 {len(self._tasks)} 段精选镜头")
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
                        if t.get('transpose'):
                            v2h_tr = (
                                f'[0:v:0]transpose={t["transpose"]},split=2[fg_in][bg_in];'
                                '[bg_in]scale=480:854,crop=480:270,boxblur=8:2,scale=3840:2160:flags=fast_bilinear,eq=brightness=-0.08:saturation=1.15[bg];'
                                '[fg_in]scale=1216:2160[fg];'
                                '[bg][fg]overlay=(W-w)/2:(H-h)/2[outv]'
                            )
                            cmd += ['-filter_complex', v2h_tr, '-map', '[outv]', '-map', '0:a:0?']
                        else:
                            cmd += ['-filter_complex', filter_v2h, '-map', '[outv]', '-map', '0:a:0?']
                    elif t.get('transpose'):
                        cmd += ['-vf', f'transpose={t["transpose"]},scale=3840:2160', '-map', '0:v:0', '-map', '0:a:0?']
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


def generate_metadata_list(clips, out_path, format_type="json"):
    import csv
    if format_type.lower() == "csv":
        with open(out_path, "w", encoding="utf-8-sig", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["镜头编号", "源文件名", "源文件路径", "入点起点(秒)", "出点终点(秒)", "裁切保留时长(秒)", "时长文本", "真实拍摄朝向", "10-Bit色彩", "转录文本/字幕"])
            for i, c in enumerate(clips):
                writer.writerow([
                    i + 1,
                    c.get("filename", ""),
                    c.get("src", ""),
                    f"{float(c.get('start', 0.0)):.2f}",
                    f"{float(c.get('end', 0.0)):.2f}",
                    f"{float(c.get('dur', 0.0)):.2f}",
                    c.get("dur_str", ""),
                    "竖屏 9:16" if c.get("orient") == "vertical" else "横屏 16:9",
                    "10-Bit HDR (D-Log M)" if c.get("is_10bit") else "8-Bit SDR",
                    c.get("text", "")
                ])
    else:
        meta_data = {
            "project": "随剪 SuiJian",
            "export_time": datetime.datetime.now().isoformat(),
            "clip_count": len(clips),
            "total_duration_sec": round(sum(float(c.get('dur', 0.0)) for c in clips), 2),
            "clips": []
        }
        for i, c in enumerate(clips):
            meta_data["clips"].append({
                "index": i + 1,
                "filename": c.get("filename", ""),
                "src_path": c.get("src", ""),
                "in_point_sec": float(c.get("start", 0.0)),
                "out_point_sec": float(c.get("end", 0.0)),
                "cut_duration_sec": float(c.get("dur", 0.0)),
                "duration_formatted": c.get("dur_str", ""),
                "orientation": c.get("orient", "horizontal"),
                "is_10bit_hdr": bool(c.get("is_10bit")),
                "subtitles": c.get("subtitles", []),
                "transcript_text": c.get("text", "")
            })
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(meta_data, f, ensure_ascii=False, indent=2)


def generate_fcpxml(clips, master_canvas, out_path):
    import xml.etree.ElementTree as ET
    from xml.dom import minidom
    
    is_vert = (master_canvas == 'vertical')
    w = "2160" if is_vert else "3840"
    h = "3840" if is_vert else "2160"
    
    root = ET.Element('fcpxml', version='1.9')
    resources = ET.SubElement(root, 'resources')
    
    fmt_id = "r1"
    ET.SubElement(resources, 'format', {
        'id': fmt_id,
        'name': f'FFVideoFormat{w}x{h}p5994',
        'frameDuration': '1001/60000s',
        'width': w,
        'height': h
    })
    
    asset_map = {}
    for i, c in enumerate(clips):
        src_path = c.get('src', '')
        if src_path not in asset_map:
            aid = f"r{len(asset_map) + 2}"
            asset_map[src_path] = aid
            dur_sec = float(c.get('dur', 10.0))
            ET.SubElement(resources, 'asset', {
                'id': aid,
                'name': c.get('filename', os.path.basename(src_path)),
                'src': f"file://{src_path}",
                'start': '0s',
                'duration': f"{dur_sec:.3f}s",
                'hasVideo': '1',
                'hasAudio': '1',
                'format': fmt_id
            })
            
    library = ET.SubElement(root, 'library')
    event = ET.SubElement(library, 'event', name="随剪智能粗剪工程")
    now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    project = ET.SubElement(event, 'project', name=f"随剪_剪辑工程_{now_str}")
    
    total_sec = sum(float(c.get('dur', 0.0)) for c in clips)
    sequence = ET.SubElement(project, 'sequence', {
        'format': fmt_id,
        'duration': f"{total_sec:.3f}s",
        'tcStart': '0s',
        'tcFormat': 'NDF'
    })
    spine = ET.SubElement(sequence, 'spine')
    
    offset_sec = 0.0
    for i, c in enumerate(clips):
        src_path = c.get('src', '')
        aid = asset_map.get(src_path, fmt_id)
        start_sec = float(c.get('start', 0.0))
        dur_sec = float(c.get('dur', 0.0))
        ET.SubElement(spine, 'asset-clip', {
            'ref': aid,
            'name': f"镜头_{i+1}_{c.get('filename', '')}",
            'offset': f"{offset_sec:.3f}s",
            'start': f"{start_sec:.3f}s",
            'duration': f"{dur_sec:.3f}s",
            'format': fmt_id
        })
        offset_sec += dur_sec
        
    xml_raw = ET.tostring(root, encoding='utf-8')
    pretty_xml = minidom.parseString(xml_raw).toprettyxml(indent="  ", encoding="utf-8")
    with open(out_path, "wb") as f:
        f.write(pretty_xml)


class EditorBridge(QObject):
    # 状态与数据信号
    mediaLoaded = Signal(list, list, str) # dates, mediaList, sourceLabel
    importProgress = Signal(int, int, str) # current, total, filename
    djiFound = Signal(str)
    recentDirsLoaded = Signal(list)
    statusMessage = Signal(str)
    aiProgress = Signal(float, str)
    aiFinished = Signal(list)
    aiPaused = Signal(int, int, str)
    aiResumed = Signal()
    speechSegmentsReady = Signal(str, list)  # video_path, annotated word segments
    usbDeviceDetected = Signal(str, str, int)  # dev_name, media_dir, file_count
    usbDeviceRemoved = Signal(str)            # dev_name
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
        self.current_media_pool = []
        self._health_manager = DeviceHealthManager(self)
        self._health_manager.deviceDisconnected.connect(self._on_device_disconnected)
        self._health_manager.deviceReconnected.connect(self._on_device_reconnected)
        self._health_manager.usbDeviceDetected.connect(self._on_usb_device_detected)
        self._health_manager.usbDeviceRemoved.connect(self._on_usb_device_removed)
        self._health_manager.start()

    def _on_usb_device_detected(self, dev_name, media_dir, file_count):
        self.usbDeviceDetected.emit(dev_name, media_dir, file_count)
        if not self.current_media_pool:
            self.statusMessage.emit(f"自动识别到已连接相机 [{dev_name}] ({file_count} 个素材)，已自动载入")
            self.importPaths([media_dir])
        else:
            self.statusMessage.emit(f"检测到新接入相机外设 [{dev_name}] ({file_count} 个素材)")

    def _on_usb_device_removed(self, dev_name):
        self.usbDeviceRemoved.emit(dev_name)
        self.statusMessage.emit(f"存储设备 [{dev_name}] 已从系统拔出移除")

    def _on_device_disconnected(self, dev_name):
        self.deviceDisconnected.emit(dev_name)
        self.statusMessage.emit(f"设备 [{dev_name}] 已断开（拔出/录像中）· 任务已安全挂起")

    def _on_device_reconnected(self, dev_name, diff):
        self.deviceReconnected.emit(dev_name, diff)
        self.statusMessage.emit(f"设备 [{dev_name}] 已重新连接！")

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

        if not clean_paths:
            return

        self.statusMessage.emit(f"正在多线程并发解析 {len(clean_paths)} 项素材...")

        # 安全处理正在运行的旧导入任务，避免在主线程 wait() 造成 UI 假死卡顿
        if self._scan_worker and self._scan_worker.isRunning():
            try:
                self._scan_worker.finished.disconnect()
                self._scan_worker.progress.disconnect()
                self._scan_worker.error.disconnect()
            except Exception:
                pass

        self._scan_worker = MediaImportWorker(clean_paths)
        self._scan_worker.finished.connect(self._on_import_finished)
        self._scan_worker.progress.connect(self.importProgress.emit)
        self._scan_worker.error.connect(self.scanError.emit)
        self._scan_worker.start()

    def _on_import_finished(self, dates, media_list, source_label):
        self.current_media_pool = media_list
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
        devices = scan_camera_volumes()
        if devices:
            dev = devices[0]
            self.djiFound.emit(dev["media_dir"])
            self.statusMessage.emit(f"检测到已连接的相机外设: {dev['dev_name']} ({dev['count']} 个视频)")
            if not self.current_media_pool:
                self.importPaths([dev["media_dir"]])
        else:
            found = find_dji_paths()
            if found:
                self.djiFound.emit(found)
                self.statusMessage.emit(f"检测到已连接的相机存储卡: {found}")
                if not self.current_media_pool:
                    self.importPaths([found])
            else:
                self.djiFound.emit("")
                self.statusMessage.emit("未检测到连接的相机外设，可手动选择文件夹或视频")

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
            self.statusMessage.emit(f"AI 任务已暂停 ({reason}) · 断点已安全存盘")

    @Slot()
    def resumeAiScreening(self):
        if self._ai_worker and self._ai_worker.isRunning():
            self._ai_worker.resume()
            self.statusMessage.emit("AI 任务已从断点恢复执行")

    @Slot()
    def safeEjectDevice(self):
        """用户主动请求安全拔出设备去录像"""
        if self._ai_worker and self._ai_worker.isRunning():
            self._ai_worker.pause("准备拔出设备录像")
        self.statusMessage.emit("检查点已留存！现在可安全拔出相机去录像，插回后可随时续接。")

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

    @Slot(list, str, str)
    def exportMetadata(self, clips, format_type, output_path):
        """导出剪辑片段的结构化元数据清单 (JSON 或 CSV)"""
        try:
            ext = "csv" if format_type.lower() == "csv" else "json"
            if not output_path:
                output_path = os.path.join(
                    os.path.expanduser("~/Downloads"),
                    f"随剪_元数据清单_{datetime.datetime.now().strftime('%m月%d日_%H%M')}.{ext}"
                )
            generate_metadata_list(clips, output_path, ext)
            self.statusMessage.emit(f"元数据清单已成功导出: {output_path}")
        except Exception as e:
            self.statusMessage.emit(f"元数据清单导出失败: {e}")

    @Slot(list, str, str)
    def exportFcpxml(self, clips, master_canvas, output_path):
        """导出标准 Final Cut Pro XML (FCPXML) 专业剪辑工程"""
        try:
            if not output_path:
                output_path = os.path.join(
                    os.path.expanduser("~/Downloads"),
                    f"随剪_剪辑工程_{datetime.datetime.now().strftime('%m月%d日_%H%M')}.fcpxml"
                )
            generate_fcpxml(clips, master_canvas, output_path)
            self.statusMessage.emit(f"FCPXML 工程已导出: {output_path} (可直接在 FCPX 或达芬奇中导入打开)")
        except Exception as e:
            self.statusMessage.emit(f"FCPXML 导出失败: {e}")
