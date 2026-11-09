#include "MediaEngine.h"
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

MediaEngine::MediaEngine(QObject *parent) : QObject(parent) {
    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(2500);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &MediaEngine::checkDeviceHeartbeat);
    m_heartbeatTimer->start();
}

MediaEngine::~MediaEngine() {
    if (m_heartbeatTimer) m_heartbeatTimer->stop();
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
    if (!ffprobe.waitForFinished(4000)) {
        ffprobe.kill();
        return meta;
    }

    QJsonDocument doc = QJsonDocument::fromJson(ffprobe.readAllStandardOutput());
    QJsonObject rootObj = doc.object();
    QJsonArray streams = rootObj["streams"].toArray();

    QJsonObject vStream;
    for (const auto &s : streams) {
        if (s.toObject()["codec_type"].toString() == "video") {
            vStream = s.toObject();
            break;
        }
    }

    double dur = rootObj["format"].toObject()["duration"].toString().toDouble();
    if (dur <= 0) dur = vStream["duration"].toString().toDouble();
    if (dur <= 0) dur = 10.0;

    int w = vStream["width"].toInt(3840);
    int h = vStream["height"].toInt(2160);
    QString pixFmt = vStream["pix_fmt"].toString();
    bool is10bit = pixFmt.contains("10") || pixFmt.contains("p010");

    meta["path"] = filepath;
    meta["filename"] = fi.fileName();
    meta["duration"] = dur;
    int mins = static_cast<int>(dur) / 60;
    int secs = static_cast<int>(dur) % 60;
    meta["duration_str"] = QString("%1:%2").arg(mins, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'));
    meta["width"] = w;
    meta["height"] = h;
    meta["is_10bit"] = is10bit;
    meta["color_desc"] = is10bit ? "10-Bit HDR (D-Log M)" : "8-Bit 标准";
    meta["orient"] = (h > w) ? "vertical" : "horizontal";
    meta["date"] = fi.lastModified().toString("yyyy-MM-dd");
    meta["time"] = fi.lastModified().toString("hh:mm:ss");

    // 缩略图生成
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/sui_jian_thumbs";
    QDir().mkpath(cacheDir);
    QString thumbPath = cacheDir + "/" + fi.fileName() + ".jpg";

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
    std::thread([this, paths]() {
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

        m_activeDeviceRoot = dirForRecent;
        m_knownFiles = files;

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
    }).detach();
}

void MediaEngine::checkDeviceHeartbeat() {
#if defined(Q_OS_MAC)
    QDir volumesDir("/Volumes");
    for (const auto &entry : volumesDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString b = entry.fileName();
        if (b == "Macintosh HD" || b == "Recovery" || b == "Preboot" || b == "VM") continue;
        QString dcim = entry.absoluteFilePath() + "/DCIM";
        if (QDir(dcim).exists()) {
            QDir d(dcim);
            for (const auto &sub : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QStringList vids = QDir(sub.absoluteFilePath()).entryList(QStringList() << "*.MP4" << "*.mp4" << "*.MOV" << "*.mov", QDir::Files);
                if (!vids.isEmpty()) {
                    emit usbDeviceDetected(b, sub.absoluteFilePath(), vids.size());
                    break;
                }
            }
        }
    }
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
        diff["new_count"] = 0;
        emit deviceReconnected(QFileInfo(m_activeDeviceRoot).fileName(), diff);
    }
}

QString MediaEngine::extractAudioWav(const QString &videoPath) {
    QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QString("/suijian_speech_%1").arg(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(tmpDir);
    QString outWav = tmpDir + "/audio.wav";

    QProcess ffmpeg;
    QStringList args = {
        "-y", "-i", videoPath,
        "-map", "0:a:0", "-c:a", "pcm_s16le",
        "-ar", "16000", "-ac", "1",
        outWav
    };
    ffmpeg.start("ffmpeg", args);
    if (!ffmpeg.waitForFinished(60000)) {
        ffmpeg.kill();
        return QString();
    }
    if (QFile::exists(outWav) && QFileInfo(outWav).size() > 500) {
        return outWav;
    }
    return QString();
}

QList<QPair<double, double>> MediaEngine::detectVoiceSegments(const QString &audioOrVideoPath, double noiseDb, double minSilenceDur, double minSpeechDur) {
    QList<QPair<double, double>> voiceBlocks;
    QProcess ffmpeg;
    QStringList args = {
        "-y", "-i", audioOrVideoPath,
        "-vn",
        "-af", QString("silencedetect=noise=%1dB:d=%2").arg(noiseDb).arg(minSilenceDur),
        "-f", "null", "-"
    };
    ffmpeg.start("ffmpeg", args);
    if (!ffmpeg.waitForFinished(60000)) {
        ffmpeg.kill();
        return voiceBlocks;
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
    if (totalDur <= 0.0) totalDur = 60.0;

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

void MediaEngine::saveAiCheckpoint(const QStringList &selectedPaths, int currentIndex, const QVariantList &tasks) {
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheDir);
    QFile f(cacheDir + "/suijian_ai_checkpoint.json");
    if (f.open(QIODevice::WriteOnly)) {
        QJsonObject obj;
        obj["paths"] = QJsonArray::fromStringList(selectedPaths);
        obj["current_index"] = currentIndex;
        QJsonArray arr;
        for (const auto &t : tasks) arr.append(QJsonObject::fromVariantMap(t.toMap()));
        obj["tasks"] = arr;
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }
}

QPair<int, QVariantList> MediaEngine::loadAiCheckpoint(const QStringList &selectedPaths) {
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QFile f(cacheDir + "/suijian_ai_checkpoint.json");
    if (f.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            QStringList savedPaths;
            for (const auto &p : obj["paths"].toArray()) savedPaths.append(p.toString());
            if (savedPaths == selectedPaths) {
                int idx = obj["current_index"].toInt(0);
                QVariantList list;
                for (const auto &t : obj["tasks"].toArray()) list.append(t.toObject().toVariantMap());
                return qMakePair(idx, list);
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
    m_aiPaused = true;
    emit aiPaused(0, 0, reason.isEmpty() ? "用户请求暂停" : reason);
}

void MediaEngine::resumeAiScreening() {
    m_aiPaused = false;
    emit aiResumed();
}

void MediaEngine::cancelCurrentJob() {
    m_aiCancelled = true;
    m_aiPaused = false;
}

void MediaEngine::safeEjectDevice() {
    pauseAiScreening("准备拔出设备录像");
    emit statusMessage("检查点已留存！现在可安全拔出相机去录像，插回后可随时续接。");
}

void MediaEngine::startAiScreening(const QVariantList &mediaPool, const QStringList &selectedPaths, const QString &mode, double targetDurationSec, const QString &canvasMode, const QString &targetColor) {
    Q_UNUSED(canvasMode);
    Q_UNUSED(targetColor);

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

    std::thread([this, poolMap, effPaths, mode, targetDurationSec]() {
        int total = effPaths.size();
        if (total == 0) {
            QMetaObject::invokeMethod(this, [this]() { emit aiFinished(QVariantList()); });
            return;
        }

        auto cp = loadAiCheckpoint(effPaths);
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
                saveAiCheckpoint(effPaths, idx, allTasks);
                return;
            }
            while (m_aiPaused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (m_aiCancelled) {
                    saveAiCheckpoint(effPaths, idx, allTasks);
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

            // 口播模式 (mode 4) 或 深度综合多模态混合模式 (mode 3)
            if ((mode == "3" || mode == "4") && dur > 2.0) {
                double pAud = std::max(1.0, std::min(99.0, ((idx + 0.15 * 0.6) / total) * 100.0));
                QMetaObject::invokeMethod(this, [this, pAud, idx, total, filename]() {
                    emit aiProgress(pAud, QString("[%1/%2] 提取音频流: %3").arg(idx + 1).arg(total).arg(filename));
                });

                QString wavPath = extractAudioWav(path);
                if (!wavPath.isEmpty()) {
                    double pVad = std::max(1.0, std::min(99.0, ((idx + 0.35 * 0.6) / total) * 100.0));
                    QMetaObject::invokeMethod(this, [this, pVad, idx, total, filename]() {
                        emit aiProgress(pVad, QString("[%1/%2] 分析语音停顿区间: %3").arg(idx + 1).arg(total).arg(filename));
                    });

                    auto voiceSegs = detectVoiceSegments(wavPath, -30.0, 0.6, 0.3);
                    QFile::remove(wavPath);
                    QDir().rmdir(QFileInfo(wavPath).absolutePath());

                    if (!voiceSegs.isEmpty()) {
                        for (int sIdx = 0; sIdx < voiceSegs.size(); ++sIdx) {
                            double st = std::max(0.0, voiceSegs[sIdx].first - 0.2);
                            double en = std::min(dur, voiceSegs[sIdx].second + 0.3);
                            double clipDur = en - st;
                            if (clipDur < 1.0) continue;

                            QVariantMap clip;
                            clip["src"] = path;
                            clip["filename"] = filename;
                            clip["start"] = std::round(st * 100.0) / 100.0;
                            clip["end"] = std::round(en * 100.0) / 100.0;
                            clip["dur"] = std::round(clipDur * 100.0) / 100.0;
                            clip["dur_str"] = QString("%1s").arg(clipDur, 0, 'f', 1);
                            clip["orient"] = meta.value("orient", "horizontal");
                            clip["is_10bit"] = meta.value("is_10bit", false);
                            clip["thumb"] = meta.value("thumb", "");
                            clip["transpose"] = meta.value("transpose", 0);
                            clip["text"] = QString("人声精彩原声片段 #%1").arg(sIdx + 1);
                            currentFileTasks.append(clip);
                        }
                    }
                }
            }

            // 模式 1 / 2 或模式 3 兜底
            if (currentFileTasks.isEmpty()) {
                double pSil = std::max(1.0, std::min(99.0, ((idx + 0.4) / total) * 100.0));
                QMetaObject::invokeMethod(this, [this, pSil, idx, total, filename]() {
                    emit aiProgress(pSil, QString("[%1/%2] 分析音频静音与人声区间: %3").arg(idx + 1).arg(total).arg(filename));
                });

                auto voiceSegs = detectVoiceSegments(path, -30.0, 0.8, 1.2);
                if (!voiceSegs.isEmpty()) {
                    for (int sIdx = 0; sIdx < voiceSegs.size(); ++sIdx) {
                        double st = voiceSegs[sIdx].first;
                        double en = voiceSegs[sIdx].second;
                        double clipDur = en - st;
                        if (clipDur < 1.0) continue;

                        QVariantMap clip;
                        clip["src"] = path;
                        clip["filename"] = filename;
                        clip["start"] = std::round(st * 100.0) / 100.0;
                        clip["end"] = std::round(en * 100.0) / 100.0;
                        clip["dur"] = std::round(clipDur * 100.0) / 100.0;
                        clip["dur_str"] = QString("%1s").arg(clipDur, 0, 'f', 1);
                        clip["orient"] = meta.value("orient", "horizontal");
                        clip["is_10bit"] = meta.value("is_10bit", false);
                        clip["thumb"] = meta.value("thumb", "");
                        clip["transpose"] = meta.value("transpose", 0);
                        currentFileTasks.append(clip);
                    }
                } else {
                    double clipDur = std::min(dur, 25.0);
                    QVariantMap clip;
                    clip["src"] = path;
                    clip["filename"] = filename;
                    clip["start"] = 0.0;
                    clip["end"] = std::round(clipDur * 100.0) / 100.0;
                    clip["dur"] = std::round(clipDur * 100.0) / 100.0;
                    clip["dur_str"] = QString("%1s").arg(clipDur, 0, 'f', 1);
                    clip["orient"] = meta.value("orient", "horizontal");
                    clip["is_10bit"] = meta.value("is_10bit", false);
                    clip["thumb"] = meta.value("thumb", "");
                    clip["transpose"] = meta.value("transpose", 0);
                    currentFileTasks.append(clip);
                }
            }

            allTasks.append(currentFileTasks);
            saveAiCheckpoint(effPaths, idx + 1, allTasks);
        }

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
                    double newDur = std::max(2.0, oldDur * budgetRatio);
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
    }).detach();
}

void MediaEngine::startExport(const QVariantList &clips, const QString &masterCanvas, const QString &targetColor) {
    if (clips.isEmpty()) {
        emit exportFailed("时间线没有镜头可供导出");
        return;
    }

    QString down = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QString out = down + QString("/随剪_成片_%1.mp4").arg(QDateTime::currentDateTime().toString("MM月dd日_hhmm"));

    std::thread([this, clips, masterCanvas, targetColor, out]() {
        QString scratch = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QString("/suijian_export_%1").arg(QDateTime::currentMSecsSinceEpoch());
        QDir().mkpath(scratch);

        int total = clips.size();
        QStringList renderedSlices;

        for (int i = 0; i < total; ++i) {
            QVariantMap t = clips[i].toMap();
            double pVal = (static_cast<double>(i) / total) * 92.0;
            QString fn = t.value("filename").toString();
            double dur = t.value("dur").toDouble();

            QMetaObject::invokeMethod(this, [this, pVal, i, total, fn, dur]() {
                emit exportProgress(pVal, QString("正在硬件压制镜头 [%1/%2]: %3 (%4s)").arg(i + 1).arg(total).arg(fn).arg(dur, 0, 'f', 1));
            });

            QString sName = QString("slice_%1.mp4").arg(i, 4, 10, QChar('0'));
            QString sPath = scratch + "/" + sName;

            QStringList args = {
                "-y",
                "-ss", QString::number(t.value("start").toDouble(), 'f', 3),
                "-t", QString::number(dur, 'f', 3),
                "-i", t.value("src").toString()
            };

            QString vf;
            if (masterCanvas == "vertical") {
                vf = "scale=2160:3840:force_original_aspect_ratio=decrease,pad=2160:3840:(ow-iw)/2:(oh-ih)/2,setsar=1";
            } else {
                vf = "scale=3840:2160:force_original_aspect_ratio=decrease,pad=3840:2160:(ow-iw)/2:(oh-ih)/2,setsar=1";
            }
            args << "-vf" << vf;

#if defined(Q_OS_MAC)
            if (targetColor == "8bit") {
                args << "-c:v" << "h264_videotoolbox" << "-b:v" << "35M" << "-pix_fmt" << "yuv420p";
            } else {
                args << "-c:v" << "hevc_videotoolbox" << "-b:v" << "65M" << "-tag:v" << "hvc1"
                     << "-color_primaries" << "bt2020" << "-color_trc" << "arib-std-b67" << "-colorspace" << "bt2020nc";
            }
#else
            if (targetColor == "8bit") {
                args << "-c:v" << "libx264" << "-preset" << "veryfast" << "-b:v" << "35M" << "-pix_fmt" << "yuv420p";
            } else {
                args << "-c:v" << "libx265" << "-preset" << "veryfast" << "-b:v" << "65M" << "-tag:v" << "hvc1"
                     << "-color_primaries" << "bt2020" << "-color_trc" << "arib-std-b67" << "-colorspace" << "bt2020nc";
            }
#endif
            args << "-c:a" << "aac" << "-b:a" << "320k" << "-ar" << "48000" << sPath;

            QProcess proc;
            proc.start("ffmpeg", args);
            proc.waitForFinished(120000);

            if (QFile::exists(sPath) && QFileInfo(sPath).size() > 1000) {
                renderedSlices.append(sPath);
            }
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
        concatProc.waitForFinished(180000);

        QDir(scratch).removeRecursively();

        if (QFile::exists(out) && QFileInfo(out).size() > 100000) {
            QMetaObject::invokeMethod(this, [this, out]() {
                emit exportProgress(100.0, "压制成功落盘！");
                emit exportFinished(out);
            });
        } else {
            QMetaObject::invokeMethod(this, [this]() {
                emit exportFailed("最终合并失败，请检查硬盘空间");
            });
        }
    }).detach();
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
            double dur = c.value("dur", 10.0).toDouble();
            ts << QString("    <asset id=\"r%1\" name=\"%2\" src=\"file://%3\" start=\"0s\" duration=\"%4s\" hasVideo=\"1\" hasAudio=\"1\" format=\"r1\"/>\n")
                  .arg(aid)
                  .arg(c.value("filename").toString())
                  .arg(src)
                  .arg(dur, 0, 'f', 3);
        }
    }
    ts << "  </resources>\n";
    ts << "  <library>\n";
    ts << "    <event name=\"随剪智能粗剪工程\">\n";
    ts << QString("      <project name=\"随剪_工程_%1\">\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm"));

    double totalSec = 0.0;
    for (const auto &item : clips) totalSec += item.toMap().value("dur", 0.0).toDouble();

    ts << QString("        <sequence format=\"r1\" duration=\"%1s\" tcStart=\"0s\" tcFormat=\"NDF\">\n").arg(totalSec, 0, 'f', 3);
    ts << "          <spine>\n";

    double offsetSec = 0.0;
    for (int i = 0; i < clips.size(); ++i) {
        QVariantMap c = clips[i].toMap();
        QString src = c.value("src").toString();
        int aid = knownAssets.indexOf(src) + 2;
        double startSec = c.value("start", 0.0).toDouble();
        double durSec = c.value("dur", 0.0).toDouble();

        ts << QString("            <asset-clip ref=\"r%1\" name=\"镜头_%2_%3\" offset=\"%4s\" start=\"%5s\" duration=\"%6s\" format=\"r1\"/>\n")
              .arg(aid)
              .arg(i + 1)
              .arg(c.value("filename").toString())
              .arg(offsetSec, 0, 'f', 3)
              .arg(startSec, 0, 'f', 3)
              .arg(durSec, 0, 'f', 3);
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
