#include "MediaEngine.h"
#include "VisualScreening.h"
#include "ColorPipeline.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QDebug>
#include <QMap>
#include <QPair>
#include <QUrl>
#include <thread>
#include <cmath>
#include <algorithm>
#include <QXmlStreamWriter>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QTemporaryDir>

MediaEngine::MediaEngine(QObject *parent) : QObject(parent) {
    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(2500);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &MediaEngine::checkDeviceHeartbeat);
    m_heartbeatTimer->start();
}

MediaEngine::~MediaEngine() {
    if (m_heartbeatTimer) m_heartbeatTimer->stop();
    m_aiCancelled = true;
    m_aiPaused = false;
    m_exportCancelled = true;
    for (auto &worker : m_workers) if (worker.joinable()) worker.join();
    if (m_exportProcess) {
        m_exportProcess->kill();
        m_exportProcess->waitForFinished();
    }
}

void MediaEngine::detectDji() {
    QStringList candidates;
#if defined(Q_OS_MAC)
    QDir volumesDir("/Volumes");
    for (const auto &entry : volumesDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString dcim = entry.absoluteFilePath() + "/DCIM";
        if (QDir(dcim).exists()) {
            QDir d(dcim);
            for (const auto &sub : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                candidates.append(sub.absoluteFilePath());
            }
        }
    }
#elif defined(Q_OS_WIN)
    for (char drive = 'D'; drive <= 'Z'; ++drive) {
        QString root = QString("%1:/DCIM").arg(drive);
        if (QDir(root).exists()) {
            candidates.append(root);
        }
    }
#else
    QDir mediaDir("/media");
    if (mediaDir.exists()) {
        candidates.append("/media");
    }
#endif

    for (const auto &cand : candidates) {
        QDir d(cand);
        QStringList filters = {"*.MP4", "*.mp4", "*.MOV", "*.mov"};
        if (!d.entryList(filters, QDir::Files).isEmpty()) {
            emit djiFound(cand);
            return;
        }
    }
    emit djiFound("");
}

void MediaEngine::loadRecentDirs() {
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QFile file(cacheDir + "/dji_editor_recent.json");
    QStringList dirs;
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isArray()) {
            for (const auto &val : doc.array()) {
                dirs.append(val.toString());
            }
        }
    }
    emit recentDirsLoaded(dirs);
}

void MediaEngine::scanDirectory(const QString &dirPath) {
    importPaths(QStringList{dirPath});
}

QVariantMap MediaEngine::probeFile(const QString &filepath) {
    QVariantMap meta;
    QFileInfo fi(filepath);
    if (!fi.exists()) return meta;

    QProcess ffprobe;
    QStringList args = {
        "-v", "quiet",
        "-print_format", "json",
        "-show_format",
        "-show_streams",
        filepath
    };
    ffprobe.start("ffprobe", args);
    if (!ffprobe.waitForFinished(4000) || ffprobe.exitCode() != 0) {
        ffprobe.kill();
        return meta;
    }

    QJsonDocument doc = QJsonDocument::fromJson(ffprobe.readAllStandardOutput());
    QJsonObject rootObj = doc.object();
    QJsonArray streams = rootObj["streams"].toArray();

    if (!doc.isObject()) return meta;
    QJsonObject vStream;
    for (const auto &s : streams) {
        if (s.toObject()["codec_type"].toString() == "video") {
            vStream = s.toObject();
            break;
        }
    }

    if (vStream.isEmpty()) return meta;
    double dur = rootObj["format"].toObject()["duration"].toString().toDouble();
    if (dur <= 0) dur = vStream["duration"].toString().toDouble();
    if (dur <= 0) dur = 10.0;

    int w = vStream["width"].toInt(3840);
    int h = vStream["height"].toInt(2160);
    int rotation = vStream["tags"].toObject()["rotate"].toString().toInt();
    for (const auto &side : vStream["side_data_list"].toArray()) {
        if (side.toObject().contains("rotation")) rotation = side.toObject()["rotation"].toInt();
    }
    if (std::abs(rotation) % 180 == 90) std::swap(w, h);
    QString pixFmt = vStream["pix_fmt"].toString();
    bool is10bit = pixFmt.contains("10") || pixFmt.contains("p010");

    bool hasAudio = false;
    for (const auto &stream : streams) hasAudio |= stream.toObject()["codec_type"].toString() == "audio";
    meta["has_audio"] = hasAudio;
    meta["path"] = filepath;
    meta["filename"] = fi.fileName();
    meta["duration"] = dur;
    int mins = static_cast<int>(dur) / 60;
    int secs = static_cast<int>(dur) % 60;
    meta["duration_str"] = QString("%1:%2").arg(mins, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'));
    meta["width"] = w;
    meta["height"] = h;
    meta["is_10bit"] = is10bit;
    meta["color_desc"] = is10bit ? "10-Bit" : "8-Bit";
    meta["orient"] = (h > w) ? "vertical" : "horizontal";
    meta["date"] = fi.lastModified().toString("yyyy-MM-dd");
    meta["time"] = fi.lastModified().toString("hh:mm:ss");

    // 缩略图生成
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/sui_jian_thumbs";
    QDir().mkpath(cacheDir);
    QByteArray thumbKey = (fi.absoluteFilePath() + QString::number(fi.lastModified().toMSecsSinceEpoch()) + QString::number(fi.size())).toUtf8();
    QString thumbPath = cacheDir + "/" + QCryptographicHash::hash(thumbKey, QCryptographicHash::Sha256).toHex() + ".jpg";

    if (!QFile::exists(thumbPath)) {
        QProcess ffmpeg;
        QStringList thumbArgs = {
            "-y", "-ss", QString::number(std::max(0.1, std::min(1.0, dur * 0.1))),
            "-i", filepath,
            "-vframes", "1",
            "-vf", "scale=480:-1",
            "-q:v", "5",
            thumbPath
        };
        ffmpeg.start("ffmpeg", thumbArgs);
        ffmpeg.waitForFinished(3000);
    }
    meta["thumb"] = QFile::exists(thumbPath) ? QUrl::fromLocalFile(thumbPath).toString() : "";
    return meta;
}

void MediaEngine::importPaths(const QStringList &paths) {
    m_workers.emplace_back([this, paths]() {
        QStringList validExts = {".mp4", ".mov", ".mkv", ".m4v"};
        QStringList files;
        QString dirForRecent;

        for (const QString &p : paths) {
            QString cleanPath = p;
            if (cleanPath.startsWith("file://")) cleanPath = QUrl(cleanPath).toLocalFile();
            QFileInfo fi(cleanPath);

            if (fi.isDir()) {
                if (dirForRecent.isEmpty()) dirForRecent = fi.absoluteFilePath();
                QDir dir(fi.absoluteFilePath());
                for (const auto &entry : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
                    if (validExts.contains("." + entry.suffix().toLower())) {
                        files.append(entry.absoluteFilePath());
                    }
                }
            } else if (fi.isFile()) {
                if (validExts.contains("." + fi.suffix().toLower())) {
                    files.append(fi.absoluteFilePath());
                    if (dirForRecent.isEmpty()) dirForRecent = fi.absolutePath();
                }
            }
        }

        files.removeDuplicates();
        if (files.isEmpty()) {
            QMetaObject::invokeMethod(this, [this]() {
                emit scanError("未找到有效视频文件（支持 .MP4 / .MOV）");
            });
            return;
        }

        QMetaObject::invokeMethod(this, [this, dirForRecent, files]() {
            m_activeDeviceRoot = dirForRecent;
            m_knownFiles = files;
            m_isDeviceOnline = true;
        });

        QVariantList mediaList;
        QSet<QString> dateSet;

        for (int i = 0; i < files.size(); ++i) {
            QVariantMap meta = probeFile(files[i]);
            if (!meta.isEmpty()) {
                mediaList.append(meta);
                dateSet.insert(meta["date"].toString());
            }
        }

        QStringList dates = dateSet.values();
        std::sort(dates.begin(), dates.end(), std::greater<QString>());
        QString sourceLabel = dirForRecent.isEmpty() ? QString("%1 个视频文件").arg(files.size()) : dirForRecent;

        QMetaObject::invokeMethod(this, [this, dates, mediaList, sourceLabel]() {
            emit mediaLoaded(dates, mediaList, sourceLabel);
        });
    });
}

void MediaEngine::checkDeviceHeartbeat() {
#if defined(Q_OS_MAC)
    QDir volumesDir("/Volumes");
    QSet<QString> detectedDevices;
    for (const auto &entry : volumesDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString b = entry.fileName();
        if (b == "Macintosh HD" || b == "Recovery" || b == "Preboot" || b == "VM") continue;
        QString dcim = entry.absoluteFilePath() + "/DCIM";
        if (QDir(dcim).exists()) {
            QDir d(dcim);
            for (const auto &sub : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QStringList vids = QDir(sub.absoluteFilePath()).entryList(QStringList() << "*.MP4" << "*.mp4" << "*.MOV" << "*.mov", QDir::Files);
                if (!vids.isEmpty()) {
                    detectedDevices.insert(b);
                    if (!m_seenDevices.contains(b)) emit usbDeviceDetected(b, sub.absoluteFilePath(), vids.size());
                    break;
                }
            }
        }
    }
    for (const auto &name : m_seenDevices - detectedDevices) emit usbDeviceRemoved(name);
    m_seenDevices = detectedDevices;
#endif

    if (m_activeDeviceRoot.isEmpty()) return;
    QDir d(m_activeDeviceRoot);
    bool online = d.exists();

    if (m_isDeviceOnline && !online) {
        m_isDeviceOnline = false;
        pauseAiScreening();
        emit deviceDisconnected(QFileInfo(m_activeDeviceRoot).fileName());
    } else if (!m_isDeviceOnline && online) {
        m_isDeviceOnline = true;
        QVariantMap diff;
        QVariantList missing;
        for (const auto &path : m_knownFiles) {
            if (!QFileInfo::exists(path)) missing.append(QVariantMap{{"path", path}});
        }
        int newCount = 0;
        for (const auto &entry : d.entryInfoList({"*.MP4", "*.mp4", "*.MOV", "*.mov", "*.mkv", "*.m4v"}, QDir::Files)) {
            if (!m_knownFiles.contains(entry.absoluteFilePath())) ++newCount;
        }
        diff["missing"] = missing;
        diff["new_count"] = newCount;
        emit deviceReconnected(QFileInfo(m_activeDeviceRoot).fileName(), diff);
    }
}

QList<QPair<double, double>> MediaEngine::detectVoiceSegments(const QString &audioOrVideoPath, double noiseDb, double minSilenceDur, double minSpeechDur, QString *error) {
    QList<QPair<double, double>> voiceBlocks;
    QProcess ffmpeg;
    QStringList args = {
        "-y", "-i", audioOrVideoPath,
        "-map", "0:a:0", "-vn",
        "-af", QString("silencedetect=noise=%1dB:d=%2").arg(noiseDb).arg(minSilenceDur),
        "-f", "null", "-"
    };
    ffmpeg.start("ffmpeg", args);
    if (!ffmpeg.waitForStarted(5000)) {
        if (error) *error = "无法启动 FFmpeg";
        return {};
    }
    while (!ffmpeg.waitForFinished(100)) {
        if (m_aiCancelled) { ffmpeg.kill(); ffmpeg.waitForFinished(5000); return {}; }
        if (ffmpeg.state() == QProcess::NotRunning) break;
    }
    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
        if (error) *error = "音频分析失败: " + QString::fromUtf8(ffmpeg.readAllStandardError()).right(1000);
        return {};
    }

    QString errOut = QString::fromUtf8(ffmpeg.readAllStandardError());
    QList<QPair<QString, double>> silences;
    for (const QString &line : errOut.split('\n')) {
        if (line.contains("silence_start:")) {
            QString sub = line.section("silence_start:", 1, 1).trimmed();
            double st = sub.section(' ', 0, 0).toDouble();
            silences.append(qMakePair(QString("start"), st));
        } else if (line.contains("silence_end:")) {
            QString sub = line.section("silence_end:", 1, 1).trimmed();
            double en = sub.section(' ', 0, 0).toDouble();
            silences.append(qMakePair(QString("end"), en));
        }
    }

    double totalDur = 0.0;
    QProcess probe;
    probe.start("ffprobe", QStringList() << "-v" << "error" << "-show_entries" << "format=duration" << "-of" << "default=noprint_wrappers=1:nokey=1" << audioOrVideoPath);
    if (probe.waitForFinished(3000)) {
        totalDur = probe.readAllStandardOutput().trimmed().toDouble();
    }
    if (totalDur <= 0.0) { if (error) *error = "无法读取音频时长"; return {}; }

    double curStart = 0.0;
    for (const auto &p : silences) {
        if (p.first == "start") {
            if (p.second > curStart + minSpeechDur) {
                voiceBlocks.append(qMakePair(curStart, p.second));
            }
        } else if (p.first == "end") {
            curStart = p.second;
        }
    }
    if (totalDur > curStart + minSpeechDur) {
        voiceBlocks.append(qMakePair(curStart, totalDur));
    }
    return voiceBlocks;
}

QList<QPair<double, double>> MediaEngine::detectVisualSegments(const QString &path, double duration, QString &error) {
    constexpr int width = 160, height = 90, frameBytes = width * height;
    constexpr double step = 0.5;
    QProcess process;
    // One decode pass; only two small grayscale frames are retained in memory.
    process.start("ffmpeg", {"-v", "error", "-i", path, "-map", "0:v:0", "-an",
        "-vf", "fps=2,scale=160:90:flags=fast_bilinear,format=gray", "-f", "rawvideo", "pipe:1"});
    if (!process.waitForStarted(5000)) { error = "无法启动 FFmpeg"; return {}; }
    QByteArray pending, previous, diagnostic;
    QList<QPair<double, double>> spans;
    qint64 index = 0;
    double runStart = -1;
    auto closeRun = [&](double end) {
        end = std::min(duration, end);
        if (runStart >= 0 && end - runStart >= 1.5) spans.append(qMakePair(runStart, end));
        runStart = -1;
    };
    auto drain = [&]() {
        pending += process.readAllStandardOutput();
        diagnostic = (diagnostic + process.readAllStandardError()).right(1500);
        qsizetype offset = 0;
        while (pending.size() - offset >= frameBytes) {
            const char *frame = pending.constData() + offset;
            const bool usable = screening::usableFrame(
                reinterpret_cast<const std::uint8_t *>(frame),
                previous.isEmpty() ? nullptr : reinterpret_cast<const std::uint8_t *>(previous.constData()), width, height);
            if (usable && runStart < 0) runStart = index * step;
            if (!usable) closeRun(index * step);
            previous = QByteArray(frame, frameBytes);
            ++index;
            offset += frameBytes;
        }
        pending.remove(0, offset);
    };
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(100);
        drain();
        if (m_aiCancelled) {
            process.kill(); process.waitForFinished(5000);
            return {};
        }
    }
    drain();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || index == 0 || !pending.isEmpty()) {
        error = "画面分析失败: " + diagnostic;
        return {};
    }
    closeRun(index * step);
    return spans;
}

bool MediaEngine::saveAiCheckpoint(const QStringList &selectedPaths, int currentIndex, const QVariantList &tasks, const QString &mode, double targetDurationSec) {
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheDir);
    QSaveFile f(cacheDir + "/suijian_ai_checkpoint.json");
    if (f.open(QIODevice::WriteOnly)) {
        QJsonObject obj;
        obj["screening_version"] = 2;
        obj["paths"] = QJsonArray::fromStringList(selectedPaths);
        obj["current_index"] = currentIndex;
        obj["mode"] = mode;
        obj["target_duration"] = targetDurationSec;
        QJsonArray arr;
        for (const auto &t : tasks) arr.append(QJsonObject::fromVariantMap(t.toMap()));
        obj["tasks"] = arr;
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        return f.commit();
    }
    return false;
}

QPair<int, QVariantList> MediaEngine::loadAiCheckpoint(const QStringList &selectedPaths, const QString &mode, double targetDurationSec) {
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QFile f(cacheDir + "/suijian_ai_checkpoint.json");
    if (f.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            QStringList savedPaths;
            for (const auto &p : obj["paths"].toArray()) savedPaths.append(p.toString());
            if (obj["screening_version"].toInt() == 2 && savedPaths == selectedPaths && obj["mode"].toString() == mode && obj["target_duration"].toDouble(-1) == targetDurationSec) {
                int idx = obj["current_index"].toInt(0);
                QVariantList list;
                for (const auto &t : obj["tasks"].toArray()) list.append(t.toObject().toVariantMap());
                if (idx >= 0 && idx <= selectedPaths.size()) return qMakePair(idx, list);
            }
        }
    }
    return qMakePair(0, QVariantList());
}

void MediaEngine::clearAiCheckpoint() {
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QFile::remove(cacheDir + "/suijian_ai_checkpoint.json");
}

void MediaEngine::startAiScreening(const QVariantList &selectedPaths, int targetDurationSec, const QString &mode) {
    QStringList paths;
    for (const auto &p : selectedPaths) paths.append(p.toString());
    startAiScreening(QVariantList(), paths, mode, static_cast<double>(targetDurationSec), "auto", "10bit");
}

void MediaEngine::pauseAiScreening(const QString &reason) {
    Q_UNUSED(reason);
    if (!m_aiRunning) {
        emit statusMessage("当前没有正在处理的筛选任务，请确认其他程序已停止读取设备");
        return;
    }
    m_aiPaused = true;
    emit statusMessage("正在完成当前素材并保存进度，请稍候再拔出设备");
}

void MediaEngine::resumeAiScreening() {
    m_aiPaused = false;
    emit aiResumed();
}

void MediaEngine::cancelCurrentJob() {
    m_aiCancelled = true;
    m_aiPaused = false;
    m_exportCancelled = true;
}

void MediaEngine::safeEjectDevice() {
    pauseAiScreening("准备拔出设备录像");
}

void MediaEngine::startAiScreening(const QVariantList &mediaPool, const QStringList &selectedPaths, const QString &mode, double targetDurationSec, const QString &canvasMode, const QString &targetColor) {
    Q_UNUSED(canvasMode);
    Q_UNUSED(targetColor);

    if (mode != "1" && mode != "2" && mode != "3" && mode != "4") {
        emit aiFailed("未知的筛选模式");
        return;
    }

    if (m_aiRunning.exchange(true)) { emit statusMessage("已有初筛任务正在停止，请稍后重试"); return; }
    m_aiPaused = false;
    m_aiCancelled = false;

    QMap<QString, QVariantMap> poolMap;
    for (const auto &item : mediaPool) {
        QVariantMap m = item.toMap();
        poolMap[m.value("path").toString()] = m;
    }

    QStringList effPaths = selectedPaths;
    if (effPaths.isEmpty()) {
        for (const auto &item : mediaPool) {
            effPaths.append(item.toMap().value("path").toString());
        }
    }

    m_workers.emplace_back([this, poolMap, effPaths, mode, targetDurationSec]() {
        struct RunningGuard { std::atomic<bool> &flag; ~RunningGuard() { flag = false; } } guard{m_aiRunning};
        int total = effPaths.size();
        if (total == 0) {
            QMetaObject::invokeMethod(this, [this]() { emit aiFinished(QVariantList()); });
            return;
        }

        auto cp = loadAiCheckpoint(effPaths, mode, targetDurationSec);
        int startIdx = cp.first;
        QVariantList allTasks = cp.second;

        if (startIdx > 0 && !allTasks.isEmpty()) {
            double pCp = std::max(1.0, (static_cast<double>(startIdx) / total) * 100.0);
            QMetaObject::invokeMethod(this, [this, pCp, startIdx, total]() {
                emit aiProgress(pCp, QString("发现已留存的初筛检查点，直接从第 [%1/%2] 个视频续接！").arg(startIdx + 1).arg(total));
            });
        }

        for (int idx = startIdx; idx < total; ++idx) {
            if (m_aiCancelled) {
                saveAiCheckpoint(effPaths, idx, allTasks, mode, targetDurationSec);
                return;
            }
            if (m_aiPaused) {
                bool saved = saveAiCheckpoint(effPaths, idx, allTasks, mode, targetDurationSec);
                QMetaObject::invokeMethod(this, [this, idx, total, saved]() { emit aiPaused(idx, total, saved ? "检查点已保存" : "检查点保存失败，请勿退出应用"); });
            }
            while (m_aiPaused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (m_aiCancelled) {
                    saveAiCheckpoint(effPaths, idx, allTasks, mode, targetDurationSec);
                    return;
                }
            }

            QString path = effPaths[idx];
            QVariantMap meta = poolMap.value(path);
            if (meta.isEmpty()) meta = probeFile(path);
            QString filename = meta.value("filename").toString();
            double dur = meta.value("duration", 10.0).toDouble();

            double pInit = std::max(1.0, (static_cast<double>(idx) / total) * 100.0);
            QMetaObject::invokeMethod(this, [this, pInit, idx, total, filename]() {
                emit aiProgress(pInit, QString("[%1/%2] 准备初筛分析: %3").arg(idx + 1).arg(total).arg(filename));
            });

            QVariantList currentFileTasks;
            QString analysisError;
            std::vector<screening::Span> candidates;
            if (mode != "1") {
                // No audio is a valid empty result; it is not a reason to retain
                // the first 25 seconds of silent footage.
                QVariantMap sourceMeta = meta.contains("has_audio") ? meta : probeFile(path);
                if (sourceMeta.isEmpty()) analysisError = "无法读取素材信息";
                else if (sourceMeta.value("has_audio").toBool()) {
                    const auto voices = detectVoiceSegments(path, -30.0,
                        mode == "4" ? 0.6 : 0.8, 0.3, &analysisError);
                    for (const auto &voice : voices) {
                        const double padding = mode == "4" ? 0.15 : 0.0;
                        candidates.emplace_back(voice.first - padding, voice.second + padding);
                    }
                }
            }
            if ((mode == "1" || mode == "3") && analysisError.isEmpty() && !m_aiCancelled) {
                QMetaObject::invokeMethod(this, [this, pInit, filename]() {
                    emit aiProgress(pInit, "检查画面曝光、细节与变化幅度: " + filename);
                });
                const auto visuals = detectVisualSegments(path, dur, analysisError);
                for (const auto &visual : visuals) candidates.emplace_back(visual.first, visual.second);
            }
            if (m_aiCancelled) {
                saveAiCheckpoint(effPaths, idx, allTasks, mode, targetDurationSec);
                return;
            }
            if (!analysisError.isEmpty() && QFileInfo::exists(path)) {
                saveAiCheckpoint(effPaths, idx, allTasks, mode, targetDurationSec);
                QMetaObject::invokeMethod(this, [this, filename, analysisError]() {
                    emit aiFailed(filename + ": " + analysisError);
                });
                return;
            }
            for (const auto &span : screening::mergeSpans(std::move(candidates), dur)) {
                const double clipDur = span.second - span.first;
                if (clipDur < 0.5) continue;
                QVariantMap clip;
                clip["src"] = path;
                clip["filename"] = filename;
                clip["start"] = span.first;
                clip["end"] = span.second;
                clip["dur"] = clipDur;
                clip["dur_str"] = QString("%1s").arg(clipDur, 0, 'f', 1);
                clip["orient"] = meta.value("orient", "horizontal");
                clip["is_10bit"] = meta.value("is_10bit", false);
                clip["thumb"] = meta.value("thumb", "");
                clip["transpose"] = meta.value("transpose", 0);
                currentFileTasks.append(clip);
            }

            if (!QFileInfo::exists(path)) {
                m_aiPaused = true;
                --idx;
                continue;
            }
            allTasks.append(currentFileTasks);
            saveAiCheckpoint(effPaths, idx + 1, allTasks, mode, targetDurationSec);
        }

        if (m_aiPaused && !m_aiCancelled) {
            bool saved = saveAiCheckpoint(effPaths, total, allTasks, mode, targetDurationSec);
            QMetaObject::invokeMethod(this, [this, total, saved]() { emit aiPaused(total, total, saved ? "检查点已保存" : "检查点保存失败，请勿退出应用"); });
            while (m_aiPaused && !m_aiCancelled) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (m_aiCancelled) return;

        // 目标成片节奏 / 时长预算控制 (targetDurationSec > 0 时生效)
        if (targetDurationSec > 0 && !allTasks.isEmpty()) {
            double currentTotal = 0.0;
            for (const auto &t : allTasks) currentTotal += t.toMap().value("dur").toDouble();
            if (currentTotal > targetDurationSec) {
                double budgetRatio = targetDurationSec / currentTotal;
                QVariantList budgeted;
                for (const auto &t : allTasks) {
                    QVariantMap c = t.toMap();
                    double oldDur = c.value("dur").toDouble();
                    double newDur = oldDur * budgetRatio;
                    double st = c.value("start").toDouble();
                    c["end"] = std::round((st + newDur) * 100.0) / 100.0;
                    c["dur"] = std::round(newDur * 100.0) / 100.0;
                    c["dur_str"] = QString("%1s").arg(newDur, 0, 'f', 1);
                    budgeted.append(c);
                }
                allTasks = budgeted;
            }
        }

        clearAiCheckpoint();

        QMetaObject::invokeMethod(this, [this, allTasks]() {
            emit aiProgress(100.0, QString("完成初筛，共生成 %1 段精选镜头").arg(allTasks.size()));
            emit aiFinished(allTasks);
        });
    });
}

void MediaEngine::startExport(const QVariantList &clips, const QString &masterCanvas, const QString &targetColor) {
    if (targetColor != "8bit" && targetColor != "10bit") {
        emit exportFailed("未知的导出色彩规格");
        return;
    }
    if (clips.isEmpty()) {
        emit exportFailed("时间线没有镜头可供导出");
        return;
    }

    if (m_exportRunning.exchange(true)) { emit exportFailed("上一个导出任务仍在停止，请稍后重试"); return; }
    QString down = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QString out = down + QString("/随剪_成片_%1.mp4").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz"));

    m_exportCancelled = false;
    m_workers.emplace_back([this, clips, masterCanvas, targetColor, out]() {
        struct RunningGuard { std::atomic<bool> &flag; ~RunningGuard() { flag = false; } } guard{m_exportRunning};
        QTemporaryDir tempDir(QDir::tempPath() + "/suijian_export_XXXXXX");
        if (!tempDir.isValid()) {
            QMetaObject::invokeMethod(this, [this]() { emit exportFailed("无法创建临时目录"); });
            return;
        }
        QString scratch = tempDir.path();

        int total = clips.size();
        QStringList renderedSlices;
        bool checkedFilters = false;
        bool hasToneMapping = false;

        for (int i = 0; i < total; ++i) {
            if (m_exportCancelled) { QDir(scratch).removeRecursively(); return; }
            QVariantMap t = clips[i].toMap();
            double pVal = (static_cast<double>(i) / total) * 92.0;
            QString fn = t.value("filename").toString();
            double dur = t.value("dur").toDouble();

            QMetaObject::invokeMethod(this, [this, pVal, i, total, fn, dur]() {
                emit exportProgress(pVal, QString("正在硬件压制镜头 [%1/%2]: %3 (%4s)").arg(i + 1).arg(total).arg(fn).arg(dur, 0, 'f', 1));
            });

            QProcess colorProbe;
            colorProbe.start("ffprobe", {"-v", "error", "-show_streams", "-of", "json", t.value("src").toString()});
            if (!colorProbe.waitForFinished(10000) || colorProbe.exitCode() != 0) {
                QMetaObject::invokeMethod(this, [this, fn]() { emit exportFailed(QString("无法读取 %1 的色彩信息").arg(fn)); });
                return;
            }
            auto streams = QJsonDocument::fromJson(colorProbe.readAllStandardOutput()).object().value("streams").toArray();
            QJsonObject stream;
            bool hasAudio = false;
            for (const auto &entry : streams) {
                auto item = entry.toObject();
                if (item.value("codec_type").toString() == "video" && stream.isEmpty()) stream = item;
                if (item.value("codec_type").toString() == "audio") hasAudio = true;
            }
            if (stream.isEmpty()) {
                QMetaObject::invokeMethod(this, [this, fn]() { emit exportFailed(QString("%1 没有可用的视频轨").arg(fn)); });
                return;
            }
            QString colorFilter;
            if (targetColor == "10bit") {
                if (stream.value("color_transfer").toString() != "arib-std-b67" || stream.value("color_primaries").toString() != "bt2020" || !(stream.value("pix_fmt").toString().contains("10") || stream.value("pix_fmt").toString().contains("p010"))) {
                    QMetaObject::invokeMethod(this, [this, fn]() { emit exportFailed(QString("%1 不是已标记的 10-bit BT.2020 HLG 素材；当前不支持转换为 HLG").arg(fn)); });
                    return;
                }
            } else if (stream.value("color_transfer").toString() == "arib-std-b67" || stream.value("color_transfer").toString() == "smpte2084") {
                if (!checkedFilters) {
                    QProcess filters;
                    filters.start("ffmpeg", {"-hide_banner", "-filters"});
                    if (filters.waitForFinished(10000) && filters.exitCode() == 0) {
                        const auto listing = filters.readAllStandardOutput();
                        bool zscale = false, tonemap = false;
                        for (const auto &line : listing.split('\n')) {
                            const auto fields = line.simplified().split(' ');
                            if (fields.size() > 1) {
                                zscale |= fields[1] == "zscale";
                                tonemap |= fields[1] == "tonemap";
                            }
                        }
                        hasToneMapping = zscale && tonemap;
                    }
                    checkedFilters = true;
                }
                const auto conversion = color::toSdr(stream, hasToneMapping);
                if (!conversion.error.isEmpty()) {
                    QMetaObject::invokeMethod(this, [this, fn, conversion]() { emit exportFailed(fn + ": " + conversion.error); });
                    return;
                }
                colorFilter = conversion.filter + ",";
                QMetaObject::invokeMethod(this, [this, pVal, fn]() { emit exportProgress(pVal, "HDR 转 Rec.709 SDR: " + fn); });
            }
            QString sName = QString("slice_%1.mp4").arg(i, 4, 10, QChar('0'));
            QString sPath = scratch + "/" + sName;

            QStringList args = {
                "-y",
                "-ss", QString::number(t.value("start").toDouble(), 'f', 3),
                "-t", QString::number(dur, 'f', 3),
                "-i", t.value("src").toString()
            };
            if (!hasAudio) args << "-f" << "lavfi" << "-t" << QString::number(dur, 'f', 3) << "-i" << "anullsrc=r=48000:cl=stereo";

            const int w = masterCanvas == "vertical" ? 2160 : 3840;
            const int h = masterCanvas == "vertical" ? 3840 : 2160;
            const int bw = masterCanvas == "vertical" ? 270 : 480;
            const int bh = masterCanvas == "vertical" ? 480 : 270;
            const int rotation = t.value("transpose").toInt();
            QString prefix = rotation == 1 || rotation == 2 ? QString("transpose=%1,").arg(rotation) : QString();
            prefix += colorFilter;
            QString filter = QString("[0:v:0]%1split=2[fgin][bgin];"
                "[bgin]scale=%2:%3:force_original_aspect_ratio=increase,crop=%2:%3,boxblur=8:2,"
                "scale=%4:%5:flags=fast_bilinear,eq=brightness=-0.08:saturation=1.15[bg];"
                "[fgin]scale=%4:%5:force_original_aspect_ratio=decrease:force_divisible_by=2,setsar=1[fg];"
                "[bg][fg]overlay=(W-w)/2:(H-h)/2:format=%6,setsar=1[outv]")
                .arg(prefix).arg(bw).arg(bh).arg(w).arg(h).arg(targetColor == "10bit" ? "yuv420p10" : "yuv420");
            args << "-filter_complex" << filter << "-map" << "[outv]" << "-map" << (hasAudio ? "0:a:0" : "1:a:0");

#if defined(Q_OS_MAC)
            if (targetColor == "8bit") {
                args << "-c:v" << "h264_videotoolbox" << "-b:v" << "35M" << "-pix_fmt" << "yuv420p";
            } else {
                args << "-c:v" << "hevc_videotoolbox" << "-b:v" << "65M" << "-tag:v" << "hvc1"
                     << "-pix_fmt" << "p010le" << "-profile:v" << "main10" << "-color_primaries" << "bt2020" << "-color_trc" << "arib-std-b67" << "-colorspace" << "bt2020nc";
            }
#else
            if (targetColor == "8bit") {
                args << "-c:v" << "libx264" << "-preset" << "veryfast" << "-b:v" << "35M" << "-pix_fmt" << "yuv420p";
            } else {
                args << "-c:v" << "libx265" << "-preset" << "veryfast" << "-b:v" << "65M" << "-tag:v" << "hvc1"
                     << "-pix_fmt" << "yuv420p10le" << "-profile:v" << "main10" << "-color_primaries" << "bt2020" << "-color_trc" << "arib-std-b67" << "-colorspace" << "bt2020nc";
            }
#endif
            if (!colorFilter.isEmpty()) {
                args << "-color_primaries" << "bt709" << "-color_trc" << "bt709"
                     << "-colorspace" << "bt709" << "-color_range" << "tv";
            }
            args << "-r" << "60000/1001" << "-c:a" << "aac" << "-b:a" << "320k" << "-ar" << "48000" << "-ac" << "2" << sPath;

            QProcess proc;
            proc.start("ffmpeg", args);
            while (!proc.waitForFinished(250)) {
                if (m_exportCancelled) { proc.kill(); proc.waitForFinished(); QDir(scratch).removeRecursively(); return; }
                if (proc.state() == QProcess::NotRunning) break;
            }
            if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0 || !QFile::exists(sPath) || QFileInfo(sPath).size() == 0) {
                QString error = QString::fromUtf8(proc.readAllStandardError()).right(1200);
                QDir(scratch).removeRecursively();
                QMetaObject::invokeMethod(this, [this, fn, error]() { emit exportFailed(QString("镜头 %1 导出失败：%2").arg(fn, error)); });
                return;
            }
            renderedSlices.append(sPath);
        }

        if (renderedSlices.isEmpty()) {
            QDir(scratch).removeRecursively();
            QMetaObject::invokeMethod(this, [this]() { emit exportFailed("压制失败，没有切片成功生成"); });
            return;
        }

        QMetaObject::invokeMethod(this, [this]() {
            emit exportProgress(94.0, "正在进行 4K 60fps 零损无缝拼接与流式优化...");
        });

        QString concatTxt = scratch + "/concat.txt";
        QFile cFile(concatTxt);
        if (cFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&cFile);
            for (const auto &p : renderedSlices) {
                ts << "file '" << p << "'\n";
            }
            cFile.close();
        }

        QStringList concatArgs = {
            "-y", "-f", "concat", "-safe", "0",
            "-i", concatTxt,
            "-c", "copy",
            "-movflags", "+faststart",
            out
        };

        QProcess concatProc;
        concatProc.start("ffmpeg", concatArgs);
        while (!concatProc.waitForFinished(250)) {
            if (m_exportCancelled) { concatProc.kill(); concatProc.waitForFinished(); QFile::remove(out); QDir(scratch).removeRecursively(); return; }
            if (concatProc.state() == QProcess::NotRunning) break;
        }
        bool concatOk = concatProc.exitStatus() == QProcess::NormalExit && concatProc.exitCode() == 0;

        QDir(scratch).removeRecursively();

        if (concatOk && QFile::exists(out) && QFileInfo(out).size() > 0) {
            QMetaObject::invokeMethod(this, [this, out]() {
                emit exportProgress(100.0, "压制成功落盘！");
                emit exportFinished(out);
            });
        } else {
            QFile::remove(out);
            QMetaObject::invokeMethod(this, [this]() {
                emit exportFailed("最终合并失败，请检查硬盘空间");
            });
        }
    });
}

void MediaEngine::startExport(const QString &outputPath, const QString &format, int width, int height, int fps) {
    Q_UNUSED(format);
    Q_UNUSED(width);
    Q_UNUSED(height);
    Q_UNUSED(fps);
    QVariantList dummy;
    startExport(dummy, "horizontal", "10bit");
}

void MediaEngine::exportMetadata(const QVariantList &clips, const QString &formatType, const QString &outputPath) {
    QString ext = (formatType.toLower() == "csv") ? "csv" : "json";
    QString out = outputPath;
    if (out.isEmpty()) {
        QString down = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        out = down + QString("/随剪_元数据清单_%1.%2").arg(QDateTime::currentDateTime().toString("MM月dd日_hhmm")).arg(ext);
    }

    QFile file(out);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit statusMessage(QString("元数据导出失败，无法创建文件: %1").arg(out));
        return;
    }

    if (ext == "csv") {
        // UTF-8 BOM 以兼容 Excel / Numbers
        file.write("\xEF\xBB\xBF");
        QTextStream ts(&file);
        ts << "镜头编号,源文件名,源文件路径,入点起点(秒),出点终点(秒),截取时长(秒),时长文本,真实拍摄朝向,10-Bit色彩,转录文本\n";
        for (int i = 0; i < clips.size(); ++i) {
            QVariantMap c = clips[i].toMap();
            QString orientStr = (c.value("orient").toString() == "vertical") ? "竖屏 9:16" : "横屏 16:9";
            QString colorStr = c.value("is_10bit").toBool() ? "10-Bit HDR (D-Log M)" : "8-Bit SDR";
            QString txt = c.value("text").toString().replace("\"", "\"\"");
            ts << QString("%1,\"%2\",\"%3\",%4,%5,%6,\"%7\",\"%8\",\"%9\",\"%10\"\n")
                  .arg(i + 1)
                  .arg(c.value("filename").toString())
                  .arg(c.value("src").toString())
                  .arg(c.value("start", 0.0).toDouble(), 0, 'f', 2)
                  .arg(c.value("end", 0.0).toDouble(), 0, 'f', 2)
                  .arg(c.value("dur", 0.0).toDouble(), 0, 'f', 2)
                  .arg(c.value("dur_str").toString())
                  .arg(orientStr)
                  .arg(colorStr)
                  .arg(txt);
        }
    } else {
        QJsonObject rootObj;
        rootObj["project"] = "随剪 SuiJian";
        rootObj["export_time"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        rootObj["clip_count"] = clips.size();

        double totalDur = 0.0;
        QJsonArray clipsArr;
        for (int i = 0; i < clips.size(); ++i) {
            QVariantMap c = clips[i].toMap();
            double d = c.value("dur", 0.0).toDouble();
            totalDur += d;

            QJsonObject cObj;
            cObj["index"] = i + 1;
            cObj["filename"] = c.value("filename").toString();
            cObj["src_path"] = c.value("src").toString();
            cObj["in_point_sec"] = c.value("start", 0.0).toDouble();
            cObj["out_point_sec"] = c.value("end", 0.0).toDouble();
            cObj["cut_duration_sec"] = d;
            cObj["duration_formatted"] = c.value("dur_str").toString();
            cObj["orientation"] = c.value("orient").toString();
            cObj["is_10bit_hdr"] = c.value("is_10bit").toBool();
            cObj["transcript_text"] = c.value("text").toString();
            clipsArr.append(cObj);
        }
        rootObj["total_duration_sec"] = totalDur;
        rootObj["clips"] = clipsArr;

        QJsonDocument doc(rootObj);
        file.write(doc.toJson(QJsonDocument::Indented));
    }
    file.close();
    emit statusMessage(QString("元数据清单已成功导出: %1").arg(out));
}

void MediaEngine::exportFcpxml(const QVariantList &clips, const QString &masterCanvas, const QString &outputPath) {
    QString out = outputPath;
    if (out.isEmpty()) {
        QString down = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        out = down + QString("/随剪_剪辑工程_%1.fcpxml").arg(QDateTime::currentDateTime().toString("MM月dd日_hhmm"));
    }

    QFile file(out);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit statusMessage(QString("FCPXML 导出失败: %1").arg(out));
        return;
    }

    bool isVert = (masterCanvas == "vertical");
    QString w = isVert ? "2160" : "3840";
    QString h = isVert ? "3840" : "2160";

    QTextStream ts(&file);
    ts << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    ts << "<!DOCTYPE fcpxml>\n";
    ts << "<fcpxml version=\"1.9\">\n";
    ts << "  <resources>\n";
    ts << QString("    <format id=\"r1\" name=\"FFVideoFormat%1x%2p5994\" frameDuration=\"1001/60000s\" width=\"%1\" height=\"%2\"/>\n").arg(w).arg(h);

    QStringList knownAssets;
    for (int i = 0; i < clips.size(); ++i) {
        QVariantMap c = clips[i].toMap();
        QString src = c.value("src").toString();
        if (!knownAssets.contains(src)) {
            knownAssets.append(src);
            int aid = knownAssets.size() + 1;
            double dur = probeFile(src).value("duration", 0.0).toDouble();
            for (const auto &clip : clips) {
                auto other = clip.toMap();
                if (other.value("src").toString() == src) dur = std::max(dur, other.value("end").toDouble());
            }
            ts << QString("    <asset id=\"r%1\" name=\"%2\" src=\"%3\" start=\"0s\" duration=\"%4/1000s\" hasVideo=\"1\" hasAudio=\"1\" format=\"r1\"/>\n")
                  .arg(aid)
                  .arg(c.value("filename").toString().toHtmlEscaped())
                  .arg(QString::fromUtf8(QUrl::fromLocalFile(src).toEncoded()).toHtmlEscaped())
                  .arg(qRound64(dur * 1000));
        }
    }
    ts << "  </resources>\n";
    ts << "  <library>\n";
    ts << "    <event name=\"随剪智能粗剪工程\">\n";
    ts << QString("      <project name=\"随剪_工程_%1\">\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm"));

    double totalSec = 0.0;
    for (const auto &item : clips) totalSec += item.toMap().value("dur", 0.0).toDouble();

    ts << QString("        <sequence format=\"r1\" duration=\"%1/1000s\" tcStart=\"0s\" tcFormat=\"NDF\">\n").arg(qRound64(totalSec * 1000));
    ts << "          <spine>\n";

    double offsetSec = 0.0;
    for (int i = 0; i < clips.size(); ++i) {
        QVariantMap c = clips[i].toMap();
        QString src = c.value("src").toString();
        int aid = knownAssets.indexOf(src) + 2;
        double startSec = c.value("start", 0.0).toDouble();
        double durSec = c.value("dur", 0.0).toDouble();

        ts << QString("            <asset-clip ref=\"r%1\" name=\"镜头_%2_%3\" offset=\"%4/1000s\" start=\"%5/1000s\" duration=\"%6/1000s\" format=\"r1\"/>\n")
              .arg(aid)
              .arg(i + 1)
              .arg(c.value("filename").toString().toHtmlEscaped())
              .arg(qRound64(offsetSec * 1000))
              .arg(qRound64(startSec * 1000))
              .arg(qRound64(durSec * 1000));
        offsetSec += durSec;
    }

    ts << "          </spine>\n";
    ts << "        </sequence>\n";
    ts << "      </project>\n";
    ts << "    </event>\n";
    ts << "  </library>\n";
    ts << "</fcpxml>\n";

    file.close();
    emit statusMessage(QString("FCPXML 工程已导出: %1 (可直接在达芬奇或FCPX中导入)").arg(out));
}

void MediaEngine::generateSrt(const QVariantList &clips, const QString &outputPath) {
    QString out = outputPath;
    if (out.isEmpty()) {
        QString down = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        out = down + QString("/随剪_字幕_%1.srt").arg(QDateTime::currentDateTime().toString("MM月dd日_hhmm"));
    }

    QFile file(out);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit statusMessage("无法创建字幕文件");
        return;
    }

    QTextStream ts(&file);
    int idx = 1;
    double accumulated = 0.0;

    for (const auto &item : clips) {
        QVariantMap clip = item.toMap();
        double clipStart = clip.value("start", 0.0).toDouble();
        double clipDur = clip.value("dur", 0.0).toDouble();
        QVariantList subs = clip.value("subtitles").toList();

        if (subs.isEmpty() && clip.contains("text")) {
            QVariantMap s;
            s["start"] = clipStart;
            s["end"] = clipStart + clipDur;
            s["text"] = clip.value("text");
            subs.append(s);
        }

        for (const auto &sItem : subs) {
            QVariantMap sub = sItem.toMap();
            QString text = sub.value("text").toString().trimmed();
            if (text.isEmpty()) continue;

            double subStart = sub.value("start").toDouble() - clipStart;
            double subEnd = sub.value("end").toDouble() - clipStart;
            double absStart = accumulated + std::max(0.0, subStart);
            double absEnd = accumulated + std::min(clipDur, subEnd);

            auto formatTime = [](double s) {
                int h = static_cast<int>(s) / 3600;
                int m = (static_cast<int>(s) % 3600) / 60;
                int sec = static_cast<int>(s) % 60;
                int ms = static_cast<int>((s - static_cast<int>(s)) * 1000);
                return QString("%1:%2:%3,%4")
                    .arg(h, 2, 10, QChar('0'))
                    .arg(m, 2, 10, QChar('0'))
                    .arg(sec, 2, 10, QChar('0'))
                    .arg(ms, 3, 10, QChar('0'));
            };

            ts << idx++ << "\n";
            ts << formatTime(absStart) << " --> " << formatTime(absEnd) << "\n";
            ts << text << "\n\n";
        }
        accumulated += clipDur;
    }
    file.close();
    emit statusMessage(QString("字幕已导出: %1").arg(out));
}
