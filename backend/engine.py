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

DEFAULT_CARD_DIR = "/Volumes/OsmoNano/DCIM/DJI_001"
THUMB_CACHE_DIR = os.path.expanduser("~/.cache/dji_editor_thumbs")
ANALYZER_BIN = os.path.expanduser("~/Desktop/.dji_vision_analyzer")
os.makedirs(THUMB_CACHE_DIR, exist_ok=True)


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


class ScanWorker(QThread):
    finished = Signal(list, list) # dates, media_list
    error = Signal(str)

    def __init__(self, target_dir):
        super().__init__()
        self.target_dir = target_dir

    def run(self):
        try:
            if not os.path.exists(self.target_dir):
                self.error.emit(f"目录不存在: {self.target_dir}")
                return
            files = sorted(glob.glob(os.path.join(self.target_dir, "*.MP4")) + glob.glob(os.path.join(self.target_dir, "*.mp4")))
            media_list = []
            dates_set = set()
            for f in files:
                meta = probe_file(f)
                if meta:
                    media_list.append(meta)
                    dates_set.add(meta["date"])
            dates = sorted(list(dates_set), reverse=True)
            self.finished.emit(dates, media_list)
        except Exception as e:
            self.error.emit(str(e))


class AiScreeningWorker(QThread):
    progress = Signal(float, str)
    finished = Signal(list)
    cancelled = Signal()

    def __init__(self, media_pool, selected_paths, ai_mode, max_dur, master_canvas, target_color):
        super().__init__()
        self.media_pool = {m["path"]: m for m in media_pool}
        self.selected_paths = selected_paths
        self.ai_mode = ai_mode
        self.max_dur = max_dur
        self.master_canvas = master_canvas
        self.target_color = target_color
        self._is_cancelled = False

    def cancel(self):
        self._is_cancelled = True

    def run(self):
        tasks = []
        total = len(self.selected_paths)
        if total == 0:
            self.finished.emit([])
            return

        for idx, path in enumerate(self.selected_paths):
            if self._is_cancelled:
                self.cancelled.emit()
                return
            meta = self.media_pool.get(path)
            if not meta:
                continue

            self.progress.emit((idx / total) * 100.0, f"AI 智能筛选镜头 [{idx+1}/{total}]: {meta['filename']}")
            dur = meta["duration"]

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
                    if self._is_cancelled:
                        self.cancelled.emit()
                        return
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
                tasks.append({
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

        self.progress.emit(100.0, f"完成筛选，共生成 {len(tasks)} 段精选镜头")
        self.finished.emit(tasks)


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
    mediaLoaded = Signal(list, list) # dates, mediaList
    statusMessage = Signal(str)
    aiProgress = Signal(float, str)
    aiFinished = Signal(list)
    exportProgress = Signal(float, str)
    exportFinished = Signal(str)
    exportFailed = Signal(str)
    scanError = Signal(str)

    def __init__(self):
        super().__init__()
        self._scan_worker = None
        self._ai_worker = None
        self._export_worker = None

    def close(self):
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
        self.statusMessage.emit(f"正在扫描: {target}")
        self._scan_worker = ScanWorker(target)
        self._scan_worker.finished.connect(self.mediaLoaded.emit)
        self._scan_worker.error.connect(self.scanError.emit)
        self._scan_worker.start()

    @Slot(list, list, str, float, str, str)
    def startAiScreening(self, media_pool, selected_paths, ai_mode, max_dur, master_canvas, target_color):
        if self._ai_worker and self._ai_worker.isRunning():
            self._ai_worker.cancel()
        self._ai_worker = AiScreeningWorker(media_pool, selected_paths, ai_mode, max_dur, master_canvas, target_color)
        self._ai_worker.progress.connect(self.aiProgress.emit)
        self._ai_worker.finished.connect(self.aiFinished.emit)
        self._ai_worker.start()

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
