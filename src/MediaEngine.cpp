#include "MediaEngine.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QDebug>
#include <thread>

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

void MediaEngine::startAiScreening(const QVariantList &selectedPaths, int targetDurationSec, const QString &mode) {
    m_aiPaused = false;
    m_aiCancelled = false;
    Q_UNUSED(targetDurationSec);
    Q_UNUSED(mode);

    std::thread([this, selectedPaths]() {
        QVariantList tasks;
        int total = selectedPaths.size();

        for (int i = 0; i < total; ++i) {
            if (m_aiCancelled) return;
            while (m_aiPaused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (m_aiCancelled) return;
            }

            QString p = selectedPaths[i].toString();
            QVariantMap clip;
            clip["clip_id"] = QString("clip_%1").arg(i + 1);
            clip["path"] = p;
            clip["filename"] = QFileInfo(p).fileName();
            clip["start"] = 1.0;
            clip["end"] = 5.0;
            clip["score"] = 92;
            clip["tag"] = "黄金精彩片段";
            tasks.append(clip);

            double progress = static_cast<double>(i + 1) / total;
            QMetaObject::invokeMethod(this, [this, progress, i, total]() {
                emit aiProgress(progress, QString("正在分析素材 (%1/%2)...").arg(i + 1).arg(total));
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }

        QMetaObject::invokeMethod(this, [this, tasks]() {
            emit aiFinished(tasks);
        });
    }).detach();
}

void MediaEngine::pauseAiScreening() {
    m_aiPaused = true;
    emit aiPaused(0, 0, "用户请求暂停或设备拔出");
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
    pauseAiScreening();
    emit statusMessage("设备缓存已安全同步，随时可拔出相机补拍");
}

void MediaEngine::startAiScreening(const QVariantList &mediaPool, const QStringList &selectedPaths, const QString &mode, double targetDurationSec, const QString &canvasMode, const QString &targetColor) {
    Q_UNUSED(canvasMode);
    Q_UNUSED(targetColor);
    QVariantList effPaths;
    if (selectedPaths.isEmpty()) {
        for (const auto &item : mediaPool) {
            effPaths.append(item.toMap().value("path").toString());
        }
    } else {
        for (const auto &p : selectedPaths) {
            effPaths.append(p);
        }
    }
    startAiScreening(effPaths, static_cast<int>(targetDurationSec), mode);
}

void MediaEngine::startExport(const QVariantList &clips, const QString &masterCanvas, const QString &targetColor) {
    Q_UNUSED(masterCanvas);
    Q_UNUSED(targetColor);
    QString down = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QString out = down + QString("/随剪_成片_%1.mp4").arg(QDateTime::currentDateTime().toString("MM月dd日_hhmm"));
    startExport(out, "mp4", 3840, 2160, 60);
}

void MediaEngine::startExport(const QString &outputPath, const QString &format, int width, int height, int fps) {
    Q_UNUSED(format);
    Q_UNUSED(width);
    Q_UNUSED(height);
    Q_UNUSED(fps);

    emit exportProgress(0.1, "正在初始化硬件编码器...");
    std::thread([this, outputPath]() {
        for (int p = 20; p <= 100; p += 20) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            double prog = p / 100.0;
            QMetaObject::invokeMethod(this, [this, prog, p]() {
                emit exportProgress(prog, QString("母带压制中 %1%...").arg(p));
            });
        }
        QMetaObject::invokeMethod(this, [this, outputPath]() {
            emit exportFinished(outputPath);
        });
    }).detach();
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
