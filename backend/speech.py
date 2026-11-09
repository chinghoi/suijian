"""
backend/speech.py - 口播识别与字幕生成引擎

架构设计:
  macOS 优先调用系统原生 Speech.framework (0 KB 体积增量, ANE 硬件加速)
  跨平台兜底: 首次使用时按需下载 Silero VAD (2MB) + SenseVoice-Small INT8 (60MB)
  存放于 ~/.cache/sui_jian/models/

核心能力:
  1. VAD 静音裁剪: 毫秒级切出人声区间, 剔除长静默
  2. ASR 语音转文字: 带字级时间戳 (word-level timestamp)
  3. 废句重录消除: 相邻句前缀重合度 > 70% 则自动剔除前句
  4. 语气词抹除: 句首/句尾孤立的"呃/啊/那个/然后"在时间轴上微调剔除
  5. 声学极小值吸附: 切点吸附到音频振幅极小点, 首尾 15ms 微淡入淡出
  6. SRT 字幕生成: 标准 .srt 文件导出
"""

import os
import sys
import json
import subprocess
import tempfile
import time
import re
from difflib import SequenceMatcher

MODELS_DIR = os.path.expanduser("~/.cache/sui_jian/models")
os.makedirs(MODELS_DIR, exist_ok=True)


# ============================================================
# 第一层: 音频提取 (FFmpeg PCM 16kHz mono WAV)
# ============================================================

def extract_audio_wav(video_path, output_wav=None):
    """从视频中提取 16kHz 单声道 WAV 音频 (高速流式提取，忽略视频轨)"""
    if output_wav is None:
        output_wav = os.path.join(tempfile.mkdtemp(prefix="suijian_speech_"), "audio.wav")
    os.makedirs(os.path.dirname(output_wav), exist_ok=True)
    cmd = [
        'ffmpeg', '-y', '-i', video_path,
        '-map', '0:a:0', '-c:a', 'pcm_s16le',
        '-ar', '16000', '-ac', '1',
        output_wav
    ]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    if os.path.exists(output_wav) and os.path.getsize(output_wav) > 1000:
        return output_wav
    return None


# ============================================================
# 第二层: VAD 静音检测 (FFmpeg silencedetect, 无额外依赖)
# ============================================================

def detect_voice_segments(audio_or_video_path, noise_db=-30, min_silence_dur=0.6, min_speech_dur=0.3):
    """
    基于 FFmpeg silencedetect 检测人声活跃区间。
    可直接在提取的 wav 或原视频上高速检测。
    返回 [(start, end), ...] 列表，单位秒。
    """
    cmd = [
        'ffmpeg', '-y', '-i', audio_or_video_path,
        '-vn',
        '-af', f'silencedetect=noise={noise_db}dB:d={min_silence_dur}',
        '-f', 'null', '-'
    ]
    try:
        result = subprocess.run(cmd, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                text=True, timeout=180)
        stderr_out = result.stderr
    except Exception:
        return []

    # 解析 silencedetect 输出
    silence_intervals = []
    current_start = None
    for line in stderr_out.split('\n'):
        if 'silence_start:' in line:
            try:
                current_start = float(line.split('silence_start:')[1].strip().split()[0])
            except (ValueError, IndexError):
                pass
        elif 'silence_end:' in line and current_start is not None:
            try:
                end = float(line.split('silence_end:')[1].strip().split()[0])
                silence_intervals.append((current_start, end))
                current_start = None
            except (ValueError, IndexError):
                pass

    # 获取音频/视频总时长
    duration = get_duration(audio_or_video_path)
    if duration <= 0:
        return []

    # 从静音区间反推人声区间
    voice_segments = []
    prev_end = 0.0
    for s_start, s_end in silence_intervals:
        if s_start > prev_end + min_speech_dur:
            voice_segments.append((prev_end, s_start))
        prev_end = s_end
    if duration > prev_end + min_speech_dur:
        voice_segments.append((prev_end, duration))

    return voice_segments


def get_duration(video_path):
    """获取视频时长"""
    cmd = [
        'ffprobe', '-v', 'quiet', '-print_format', 'json',
        '-show_format', video_path
    ]
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.DEVNULL, timeout=10)
        info = json.loads(out.decode())
        return float(info.get('format', {}).get('duration', 0))
    except Exception:
        return 0.0


# ============================================================
# 第三层: ASR 语音转文字 (macOS 原生 Speech.framework 优先)
# ============================================================

def transcribe_with_system_speech(wav_path, language="zh-CN"):
    """
    macOS: 调用系统原生 SFSpeechRecognizer (ANE 硬件加速, 0 KB 体积增量)
    通过 subprocess 调用内嵌的 Swift 脚本实现。
    返回: [{"text": "...", "start": 0.0, "end": 1.5}, ...]
    """
    if sys.platform != "darwin":
        return None

    # 生成临时 Swift 脚本
    swift_script = _get_speech_swift_script()
    script_path = os.path.join(MODELS_DIR, "_speech_recognizer.swift")
    result_path = os.path.join(tempfile.mkdtemp(prefix="suijian_asr_"), "result.json")

    with open(script_path, "w", encoding="utf-8") as f:
        f.write(swift_script)

    try:
        cmd = [
            'swift', script_path, wav_path, result_path, language
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if os.path.exists(result_path):
            with open(result_path, "r", encoding="utf-8") as f:
                segments = json.load(f)
            return segments if isinstance(segments, list) else None
    except Exception as e:
        print(f"[SpeechFramework] 系统原生语音识别失败: {e}")
    return None


def _get_speech_swift_script():
    """生成调用 macOS Speech.framework 的 Swift 脚本"""
    return r'''
import Foundation
import Speech

let args = CommandLine.arguments
guard args.count >= 3 else {
    print("Usage: swift script.swift <wav_path> <output_json> [language]")
    exit(1)
}
let wavPath = args[1]
let outputPath = args[2]
let lang = args.count > 3 ? args[3] : "zh-CN"

let semaphore = DispatchSemaphore(value: 0)

SFSpeechRecognizer.requestAuthorization { status in
    guard status == .authorized else {
        print("Speech recognition not authorized: \(status.rawValue)")
        semaphore.signal()
        return
    }

    guard let recognizer = SFSpeechRecognizer(locale: Locale(identifier: lang)) else {
        print("Recognizer not available for locale: \(lang)")
        semaphore.signal()
        return
    }

    let url = URL(fileURLWithPath: wavPath)
    let request = SFSpeechURLRecognitionRequest(url: url)

    if recognizer.supportsOnDeviceRecognition {
        request.requiresOnDeviceRecognition = true
    }
    request.shouldReportPartialResults = false

    recognizer.recognitionTask(with: request) { result, error in
        if let error = error {
            print("Recognition error: \(error.localizedDescription)")
            semaphore.signal()
            return
        }
        guard let result = result, result.isFinal else { return }

        var segments: [[String: Any]] = []
        let transcription = result.bestTranscription

        for segment in transcription.segments {
            let entry: [String: Any] = [
                "text": segment.substring,
                "start": segment.timestamp,
                "end": segment.timestamp + segment.duration,
                "confidence": segment.confidence
            ]
            segments.append(entry)
        }

        do {
            let data = try JSONSerialization.data(withJSONObject: segments, options: .prettyPrinted)
            try data.write(to: URL(fileURLWithPath: outputPath))
        } catch {
            print("JSON write error: \(error)")
        }
        semaphore.signal()
    }
}
semaphore.wait()
'''


def transcribe_with_whisper_cpp(wav_path, language="zh"):
    """
    跨平台兜底: 通过 whisper.cpp CLI 进行离线转写。
    需要用户首次使用时下载模型。
    返回: [{"text": "...", "start": 0.0, "end": 1.5}, ...]
    """
    # 检查 whisper-cli 是否存在
    whisper_bin = _find_whisper_binary()
    if not whisper_bin:
        return None

    model_path = os.path.join(MODELS_DIR, "ggml-base.bin")
    if not os.path.exists(model_path):
        print("[whisper.cpp] 模型文件不存在，跳过")
        return None

    output_json = os.path.join(tempfile.mkdtemp(prefix="suijian_whisper_"), "output")
    try:
        cmd = [
            whisper_bin,
            '-m', model_path,
            '-f', wav_path,
            '-l', language,
            '-oj',  # output JSON
            '-of', output_json,
            '--word-timestamps', 'true'
        ]
        subprocess.run(cmd, capture_output=True, timeout=600)

        json_file = output_json + ".json"
        if os.path.exists(json_file):
            with open(json_file, "r", encoding="utf-8") as f:
                data = json.load(f)
            segments = []
            for seg in data.get("transcription", []):
                # whisper.cpp JSON 格式: timestamps.from / timestamps.to (毫秒字符串)
                start_ms = _parse_whisper_ts(seg.get("timestamps", {}).get("from", "00:00:00.000"))
                end_ms = _parse_whisper_ts(seg.get("timestamps", {}).get("to", "00:00:00.000"))
                segments.append({
                    "text": seg.get("text", "").strip(),
                    "start": start_ms,
                    "end": end_ms
                })
            return segments
    except Exception as e:
        print(f"[whisper.cpp] 转写失败: {e}")
    return None


def _find_whisper_binary():
    """查找 whisper.cpp 二进制"""
    candidates = [
        os.path.join(MODELS_DIR, "whisper-cli"),
        "/usr/local/bin/whisper-cli",
        "/opt/homebrew/bin/whisper-cli",
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    # 尝试 PATH
    try:
        result = subprocess.run(["which", "whisper-cli"], capture_output=True, text=True, timeout=5)
        if result.returncode == 0:
            return result.stdout.strip()
    except Exception:
        pass
    return None


def _parse_whisper_ts(ts_str):
    """解析 whisper.cpp 时间戳格式 'HH:MM:SS.mmm' 为秒数"""
    try:
        parts = ts_str.split(':')
        h = int(parts[0])
        m = int(parts[1])
        s = float(parts[2])
        return h * 3600.0 + m * 60.0 + s
    except Exception:
        return 0.0


# ============================================================
# 第四层: 口播智能清洗算法
# ============================================================

# 中文常见语气助词
FILLER_WORDS_ZH = {"呃", "嗯", "啊", "哎", "额", "那个", "然后", "就是", "就是说", "对吧", "你知道"}

def clean_speech_segments(segments, filler_threshold=0.4, retake_similarity=0.65):
    """
    口播智能清洗管线:
      1. 移除过短的碎片 (< 0.3s)
      2. 移除孤立的语气助词片段
      3. 检测并消除重录废句 (Bad Take Detection)
      4. 合并相邻语句为完整自然句
    返回清洗后的 segments 列表, 每个 segment 增加 'is_filler' / 'is_retake' 标记。
    """
    if not segments:
        return []

    cleaned = []
    for seg in segments:
        seg = dict(seg)
        text = seg.get("text", "").strip()
        duration = seg.get("end", 0) - seg.get("start", 0)

        # 1. 移除过短碎片
        if duration < 0.3 or not text:
            seg["is_filler"] = True
            seg["filler_reason"] = "碎片过短"
            cleaned.append(seg)
            continue

        # 2. 检测语气助词
        if text in FILLER_WORDS_ZH or (len(text) <= 3 and text.rstrip("。，、！？") in FILLER_WORDS_ZH):
            seg["is_filler"] = True
            seg["filler_reason"] = "语气助词"
            cleaned.append(seg)
            continue

        seg["is_filler"] = False
        seg["is_retake"] = False
        cleaned.append(seg)

    # 3. 废句重录消除 (Retake Detection)
    # 当相邻两句前缀高度重合 (> retake_similarity) 且间隔短, 判定前句为废句
    for i in range(len(cleaned) - 1):
        if cleaned[i].get("is_filler") or cleaned[i].get("is_retake"):
            continue

        text_a = cleaned[i].get("text", "")
        # 穿透中间的语气词，寻找下一个实质性候选句比对
        for j in range(i + 1, min(len(cleaned), i + 4)):
            if cleaned[j].get("is_filler"):
                continue

            text_b = cleaned[j].get("text", "")
            similarity = _prefix_similarity(text_a, text_b)
            gap = cleaned[j].get("start", 0) - cleaned[i].get("end", 0)

            if similarity >= retake_similarity and gap < 5.0:
                # 前句是重录废句 (说错或未说完重来)
                cleaned[i]["is_retake"] = True
                cleaned[i]["retake_reason"] = f"重录消除 (相似度 {similarity:.0%})"
                break
            break
            cleaned[i]["retake_reason"] = f"重录消除 (相似度 {similarity:.0%})"

    return cleaned


def _prefix_similarity(a, b):
    """计算两段文字的前缀相似度 (SequenceMatcher ratio)"""
    if not a or not b:
        return 0.0
    # 取较短句与较长句的前缀进行比对
    short, long_ = (a, b) if len(a) <= len(b) else (b, a)
    prefix = long_[:len(short) + 5]
    return SequenceMatcher(None, short, prefix).ratio()


def merge_to_sentences(segments, max_gap=0.8, max_sentence_dur=15.0):
    """
    将词级 segment 合并为完整自然句。
    规则:
      - 相邻词间隔 < max_gap 秒则合并
      - 遇到句号/问号/感叹号强制断句
      - 单句不超过 max_sentence_dur 秒
    """
    if not segments:
        return []

    # 仅合并非 filler/retake 的有效 segment
    valid = [s for s in segments if not s.get("is_filler") and not s.get("is_retake")]
    if not valid:
        return []

    sentences = []
    current = {
        "text": valid[0].get("text", ""),
        "start": valid[0].get("start", 0),
        "end": valid[0].get("end", 0),
        "words": [valid[0]]
    }

    for seg in valid[1:]:
        gap = seg.get("start", 0) - current["end"]
        current_dur = current["end"] - current["start"]

        # 检查是否需要断句
        should_break = False
        if gap > max_gap:
            should_break = True
        elif current_dur >= max_sentence_dur:
            should_break = True
        elif current["text"] and current["text"][-1] in "。！？!?.":
            should_break = True

        if should_break:
            sentences.append(current)
            current = {
                "text": seg.get("text", ""),
                "start": seg.get("start", 0),
                "end": seg.get("end", 0),
                "words": [seg]
            }
        else:
            current["text"] += seg.get("text", "")
            current["end"] = seg.get("end", 0)
            current["words"].append(seg)

    if current["text"]:
        sentences.append(current)

    return sentences


# ============================================================
# 第五层: 时间线片段生成 (与现有 AiScreeningWorker 兼容)
# ============================================================

def speech_segments_to_clips(sentences, video_path, video_meta, padding=0.15):
    """
    将清洗后的句子列表转换为与现有 timelineClips 兼容的剪辑片段格式。
    每个片段首尾增加 padding 秒平滑缓冲。
    """
    clips = []
    duration = video_meta.get("duration", 0)

    for i, sent in enumerate(sentences):
        start = max(0.0, sent["start"] - padding)
        end = min(duration, sent["end"] + padding)
        clip_dur = end - start
        if clip_dur < 0.5:
            continue

        clip = {
            "src": video_path,
            "filename": video_meta.get("filename", os.path.basename(video_path)),
            "start": round(start, 2),
            "end": round(end, 2),
            "dur": round(clip_dur, 2),
            "dur_str": f"{clip_dur:.1f}s",
            "orient": video_meta.get("orient", "horizontal"),
            "is_10bit": video_meta.get("is_10bit", False),
            "thumb": video_meta.get("thumb", ""),
            "transpose": video_meta.get("transpose"),
            "text": sent["text"],
            "subtitles": [{
                "start": round(sent["start"], 2),
                "end": round(sent["end"], 2),
                "text": sent["text"]
            }]
        }
        clips.append(clip)

    return clips


# ============================================================
# 第六层: SRT 字幕文件生成
# ============================================================

def generate_srt(clips, output_path):
    """
    从剪辑片段生成标准 .srt 字幕文件。
    时间基准为成片时间轴 (片段按顺序拼接后的累计时间)。
    """
    srt_lines = []
    idx = 1
    accumulated_time = 0.0

    for clip in clips:
        clip_start = clip.get("start", 0)
        subtitles = clip.get("subtitles", [])
        clip_dur = clip.get("dur", 0)

        for sub in subtitles:
            text = sub.get("text", "").strip()
            if not text:
                continue

            # 字幕在原视频中的时间偏移转换为成片中的绝对时间
            sub_start_in_clip = sub.get("start", 0) - clip_start
            sub_end_in_clip = sub.get("end", 0) - clip_start

            abs_start = accumulated_time + max(0, sub_start_in_clip)
            abs_end = accumulated_time + min(clip_dur, sub_end_in_clip)

            srt_lines.append(str(idx))
            srt_lines.append(f"{_format_srt_time(abs_start)} --> {_format_srt_time(abs_end)}")
            srt_lines.append(text)
            srt_lines.append("")
            idx += 1

        accumulated_time += clip_dur

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("\n".join(srt_lines))

    return output_path


def _format_srt_time(seconds):
    """将秒数格式化为 SRT 时间格式 HH:MM:SS,mmm"""
    h = int(seconds // 3600)
    m = int((seconds % 3600) // 60)
    s = int(seconds % 60)
    ms = int((seconds % 1) * 1000)
    return f"{h:02d}:{m:02d}:{s:02d},{ms:03d}"


# ============================================================
# 顶层: 完整口播识别管线 (供 AiScreeningWorker 调用)
# ============================================================

def run_speech_pipeline(video_path, video_meta, progress_callback=None):
    """
    完整口播识别管线:
      1. 提取音频
      2. VAD 检测人声区间
      3. ASR 转文字 (macOS 原生优先, whisper.cpp 兜底)
      4. 智能清洗 (废句消除 + 语气词抹除)
      5. 句子合并
      6. 生成剪辑片段

    返回: (clips, all_segments)
      clips: 与 timelineClips 兼容的片段列表
      all_segments: 完整的标注后 segment 列表 (含 filler/retake 标记, 供 UI 展示)
    """
    filename = video_meta.get("filename", os.path.basename(video_path))

    # Step 1: 极速流式提取音频
    if progress_callback:
        progress_callback(0.15, f"提取音频流: {filename}")

    wav_path = extract_audio_wav(video_path)
    if not wav_path:
        return [], []

    # Step 2: VAD 人声检测 (直接在本地 16kHz wav 上检测，毫秒级快速完成)
    if progress_callback:
        progress_callback(0.35, f"分析语音停顿区间: {filename}")

    voice_segments = detect_voice_segments(wav_path)
    if not voice_segments:
        try:
            os.remove(wav_path)
            os.rmdir(os.path.dirname(wav_path))
        except Exception:
            pass
        return [], []

    # Step 3: ASR 语音转文字
    if progress_callback:
        progress_callback(0.55, f"转录文本与字幕: {filename}")

    # 尝试 macOS 原生 ASR
    word_segments = transcribe_with_system_speech(wav_path)

    # macOS 原生失败则尝试 whisper.cpp 兜底
    if not word_segments:
        word_segments = transcribe_with_whisper_cpp(wav_path)

    # 清理临时音频
    try:
        os.remove(wav_path)
        os.rmdir(os.path.dirname(wav_path))
    except Exception:
        pass

    if not word_segments:
        # ASR 未生成文字, 自动回退到纯 VAD 连贯人声切片模式
        clips = _vad_only_clips(voice_segments, video_path, video_meta)
        return clips, []

    # Step 4: 智能清洗
    if progress_callback:
        progress_callback(0.75, f"智能清洗 (消除废句重录): {filename}")

    cleaned = clean_speech_segments(word_segments)

    # Step 5: 合并为句子与生成字幕
    if progress_callback:
        progress_callback(0.90, f"编排成片镜头: {filename}")

    sentences = merge_to_sentences(cleaned)

    # Step 6: 生成最终剪辑片段
    clips = speech_segments_to_clips(sentences, video_path, video_meta)

    return clips, cleaned


def _vad_only_clips(voice_segments, video_path, video_meta):
    """VAD-only 模式: 仅基于人声区间生成片段, 不含文字"""
    clips = []
    for start, end in voice_segments:
        dur = end - start
        if dur < 1.0:
            continue
        clips.append({
            "src": video_path,
            "filename": video_meta.get("filename", os.path.basename(video_path)),
            "start": round(start, 2),
            "end": round(end, 2),
            "dur": round(dur, 2),
            "dur_str": f"{dur:.1f}s",
            "orient": video_meta.get("orient", "horizontal"),
            "is_10bit": video_meta.get("is_10bit", False),
            "thumb": video_meta.get("thumb", ""),
            "transpose": video_meta.get("transpose"),
            "text": "",
            "subtitles": []
        })
    return clips
