#include "MediaEngine.h"
#include "VisualScreening.h"
#include "ColorPipeline.h"
#include "SpeechTranscriber.h"
#include "EditingAlgorithms.h"
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
#include <QRegularExpression>
#include <thread>
#include <cmath>
#include <algorithm>
#include <QXmlStreamWriter>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QUuid>
#include <QGuiApplication>
#include <QClipboard>
#include <QMutexLocker>

static QString mediaFingerprint(const QString &path);

static QVariantList removeRepeatedSpeech(const QVariantList &segments) {
    QVariantList kept;
    const auto words = [](const QVariantMap &segment) {
        QString text = segment.value("text").toString().normalized(QString::NormalizationForm_C).toLower();
        text.remove(QRegularExpression("[^\\p{L}\\p{N}]", QRegularExpression::UseUnicodePropertiesOption));
        return text.toStdU16String();
    };
    for (const QVariant &item : segments) {
        const auto segment = item.toMap();
        if (!kept.isEmpty()) {
            const auto previous = kept.last().toMap();
            if (segment.value("start").toDouble() - previous.value("end").toDouble() <= 30.0
                && editing::sameRetake(words(previous), words(segment))) kept.removeLast();
        }
        kept.append(item);
    }
    return kept;
}


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
    m_transcriptionCancelled = true;
    m_toolCancelled = true;
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
    const int w = vStream["width"].toInt();
    const int h = vStream["height"].toInt();
    if (dur <= 0 || w <= 0 || h <= 0) return meta;
    int dispW = w;
    int dispH = h;
    int rotation = vStream["tags"].toObject()["rotate"].toString().toInt();
    for (const auto &side : vStream["side_data_list"].toArray()) {
        if (side.toObject().contains("rotation")) rotation = side.toObject()["rotation"].toInt();
    }
    if (std::abs(rotation) % 180 == 90) std::swap(dispW, dispH);
    double fps = 0;
    const QString rate = vStream["avg_frame_rate"].toString();
    const int slash = rate.indexOf('/');
    if (slash > 0) {
        const double n = rate.left(slash).toDouble();
        const double d = rate.mid(slash + 1).toDouble();
        if (d > 0) fps = n / d;
    } else {
        fps = rate.toDouble();
    }
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
    meta["width"] = dispW;
    meta["height"] = dispH;
    meta["is_10bit"] = is10bit;
    meta["color_desc"] = is10bit ? "10-Bit" : "8-Bit";
    meta["orient"] = (dispH > dispW) ? "vertical" : "horizontal";
    if (fps >= 1.0 && fps <= 1000.0) meta["fps"] = fps;
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
    if (busy()) { emit scanError("已有任务正在运行或停止，请稍后导入"); return; }
    m_importRunning = true;
    m_toolCancelled = false;
    emit busyChanged();
    m_workers.emplace_back([this, paths]() {
        struct RunningGuard { MediaEngine *engine; ~RunningGuard() {
            QMetaObject::invokeMethod(engine, [e = engine] { e->m_importRunning = false; emit e->busyChanged(); }, Qt::QueuedConnection);
        } } guard{this};
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

        const int totalFiles = files.size();
        for (int i = 0; i < files.size(); ++i) {
            if (m_toolCancelled) return;
            const QString filename = QFileInfo(files[i]).fileName();
            const int current = i + 1;
            QMetaObject::invokeMethod(this, [this, current, totalFiles, filename]() {
                emit importProgress(current, totalFiles, filename);
            });
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

QList<QPair<double, double>> MediaEngine::detectVoiceSegments(const QString &audioOrVideoPath, double noiseDb, double minSilenceDur, double minSpeechDur, QString *error, double duration, const std::function<void(double)> &onProgress) {
    QList<QPair<double, double>> voiceBlocks;
    if (m_aiPaused || m_aiCancelled) return {};
    QProcess ffmpeg;
    QStringList args = {
        "-y", "-i", audioOrVideoPath,
        "-map", "0:a:0", "-vn",
        "-af", QString("silencedetect=noise=%1dB:d=%2").arg(noiseDb).arg(minSilenceDur),
        "-f", "null", "-"
    };
    ffmpeg.start("ffmpeg", args);
    if (!ffmpeg.waitForStarted(5000)) {
        if (error && !m_aiPaused && !m_aiCancelled) *error = "无法启动 FFmpeg";
        return {};
    }
    QByteArray errBytes;
    QElapsedTimer clock;
    clock.start();
    double reported = -1;
    while (!ffmpeg.waitForFinished(100)) {
        errBytes += ffmpeg.readAllStandardError();
        if (onProgress && duration > 0 && (reported < 0 || clock.elapsed() >= 400)) {
            const int mark = errBytes.lastIndexOf("time=");
            if (mark >= 0) {
                const auto token = errBytes.mid(mark + 5).trimmed().split(' ').value(0).split(':');
                if (token.size() == 3) {
                    const double seconds = token.at(0).toDouble() * 3600 + token.at(1).toDouble() * 60 + token.at(2).toDouble();
                    const double fraction = std::min(0.999, std::max(0.0, seconds / duration));
                    if (fraction >= reported + 0.02) { onProgress(fraction); reported = fraction; }
                }
            }
            clock.restart();
        }
        if (m_aiCancelled || m_aiPaused) { ffmpeg.kill(); ffmpeg.waitForFinished(5000); return {}; }
        if (ffmpeg.state() == QProcess::NotRunning) break;
    }
    errBytes += ffmpeg.readAllStandardError();
    if (m_aiPaused || m_aiCancelled) return {};
    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
        if (error) *error = "音频分析失败: " + QString::fromUtf8(errBytes).right(1000);
        return {};
    }

    QString errOut = QString::fromUtf8(errBytes);
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

    double totalDur = duration;
    if (totalDur <= 0.0) {
        QProcess probe;
        probe.start("ffprobe", QStringList() << "-v" << "error" << "-show_entries" << "format=duration" << "-of" << "default=noprint_wrappers=1:nokey=1" << audioOrVideoPath);
        if (probe.waitForFinished(3000)) totalDur = QString::fromUtf8(probe.readAllStandardOutput()).trimmed().toDouble();
    }
    if (m_aiPaused || m_aiCancelled) return {};
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

QList<QPair<double, double>> MediaEngine::detectVisualSegments(const QString &path, double duration, QString &error, const std::function<void(double)> &onProgress) {
    constexpr int width = 160, height = 90, frameBytes = width * height;
    constexpr double step = 0.5;
    if (m_aiPaused || m_aiCancelled) return {};
    QProcess process;
    // One decode pass; only two small grayscale frames are retained in memory.
    process.start("ffmpeg", {"-v", "error", "-i", path, "-map", "0:v:0", "-an",
        "-vf", "fps=2,scale=160:90:flags=fast_bilinear,format=gray", "-f", "rawvideo", "pipe:1"});
    if (!process.waitForStarted(5000)) { if (!m_aiPaused && !m_aiCancelled) error = "无法启动 FFmpeg"; return {}; }
    QByteArray pending, previous, diagnostic;
    QElapsedTimer clock;
    clock.start();
    double reported = -1;
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
        if (onProgress && duration > 0 && (reported < 0 || clock.elapsed() >= 400)) {
            const double fraction = std::min(0.999, std::max(0.0, index * step / duration));
            if (fraction >= reported + 0.02) { onProgress(fraction); reported = fraction; clock.restart(); }
        }
    };
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(100);
        drain();
        if (m_aiCancelled || m_aiPaused) {
            process.kill(); process.waitForFinished(5000);
            return {};
        }
    }
    drain();
    if (m_aiPaused || m_aiCancelled) return {};
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || index == 0 || !pending.isEmpty()) {
        error = "画面分析失败: " + diagnostic;
        return {};
    }
    closeRun(index * step);
    return spans;
}

bool MediaEngine::saveAiCheckpoint(const QStringList &selectedPaths, int currentIndex, const QVariantList &tasks, const QString &mode, double targetDurationSec, const QJsonObject &fingerprints) {
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheDir);
    QSaveFile f(cacheDir + "/suijian_ai_checkpoint.json");
    if (f.open(QIODevice::WriteOnly)) {
        QJsonObject obj;
        obj["screening_version"] = 3;
        obj["fingerprints"] = fingerprints;
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
            if (obj["screening_version"].toInt() == 3 && savedPaths == selectedPaths && obj["mode"].toString() == mode && obj["target_duration"].toDouble(-1) == targetDurationSec) {
                for (const QString &path : selectedPaths) {
                    if (mediaFingerprint(path).isEmpty() || obj["fingerprints"].toObject().value(path).toString() != mediaFingerprint(path)) return {0, {}};
                }
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
    emit statusMessage("正在暂停，停止读取存储卡后再拔出设备");
}

void MediaEngine::resumeAiScreening() {
    if (!m_aiRunning || !m_aiPaused) return;
    m_aiPaused = false;
    emit aiResumed();
}

void MediaEngine::cancelCurrentJob() {
    m_aiCancelled = true;
    m_aiPaused = false;
    m_exportCancelled = true;
    m_transcriptionCancelled = true;
}

void MediaEngine::cancelTranscription() {
    if (m_transcriptionRunning) m_transcriptionCancelled = true;
}

static QString mediaFingerprint(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists()) return {};
    return QString::number(info.size()) + QLatin1Char(':')
        + QString::number(info.lastModified().toMSecsSinceEpoch());
}

QVariantList MediaEngine::transcriptSegmentsForClip(const QVariantMap &clip) const {
    const double clipStart = clip.value("start", 0.0).toDouble();
    const double clipEnd = clip.value("end", clipStart + clip.value("dur", 0.0).toDouble()).toDouble();
    const auto intersecting = [clipStart, clipEnd](const QVariantList &source) {
        QVariantList result;
        for (const QVariant &item : source) {
            const QVariantMap segment = item.toMap();
            if (segment.value("end").toDouble() <= clipStart
                || segment.value("start").toDouble() >= clipEnd) continue;
            result.append(segment);
        }
        return result;
    };

    const QVariantList supplied = clip.value("subtitles").toList();
    if (!supplied.isEmpty()) return intersecting(supplied);

    const QString explicitText = clip.value("text").toString().trimmed();
    if (!explicitText.isEmpty()) {
        QVariantMap segment;
        segment["start"] = clipStart;
        segment["end"] = clipEnd;
        segment["text"] = explicitText;
        return {segment};
    }

    const QString path = clip.value("src").toString();
    const QString fingerprint = mediaFingerprint(path);
    if (fingerprint.isEmpty()) return {};

    QMutexLocker lock(&m_speechMutex);
    if (m_speechFingerprints.value(path) != fingerprint) return {};
    return intersecting(m_speechAnnotations.value(path));
}

QString MediaEngine::transcriptTextForClip(const QVariantMap &clip) const {
    QStringList text;
    for (const QVariant &item : transcriptSegmentsForClip(clip)) {
        const QString value = item.toMap().value("text").toString().simplified();
        if (!value.isEmpty() && (text.isEmpty() || text.last() != value)) text.append(value);
    }
    return text.join(QLatin1Char(' '));
}

void MediaEngine::transcribeTimeline(const QVariantList &clips) {
    if (busy()) {
        emit transcriptionFailed("请等待当前筛选或导出任务结束后再转写");
        return;
    }
    if (m_transcriptionRunning.exchange(true)) {
        emit transcriptionFailed("已有转写任务正在运行");
        return;
    }

    QMap<QString, double> sources;
    for (const QVariant &item : clips) {
        const QVariantMap clip = item.toMap();
        const QString path = clip.value("src").toString();
        if (path.isEmpty()) continue;
        const double duration = std::max(clip.value("media_dur").toDouble(), clip.value("end").toDouble());
        sources[path] = std::max(sources.value(path), duration);
    }
    if (sources.isEmpty()) {
        m_transcriptionRunning = false;
        emit transcriptionFailed("时间线上没有可转写的素材");
        return;
    }

    m_transcriptionCancelled = false;
    emit busyChanged();
    m_workers.emplace_back([this, sources]() {
        struct RunningGuard { MediaEngine *engine; ~RunningGuard() {
            QMetaObject::invokeMethod(engine, [e = engine] { e->m_transcriptionRunning = false; emit e->busyChanged(); }, Qt::QueuedConnection);
        } } guard{this};
        auto report = [this](double percent, const QString &text) {
            const double safePercent = std::clamp(percent, 0.0, 99.0);
            QMetaObject::invokeMethod(this, [this, safePercent, text]() {
                emit transcriptionProgress(safePercent, text);
            }, Qt::QueuedConnection);
        };

        QString error;
        const bool modelReady = SpeechTranscriber::prepareModel([this] { return m_transcriptionCancelled.load(); },
            [&](double percent, const QString &text) { report(percent * 0.08, text); }, &error);
        if (!modelReady) {
            if (m_transcriptionCancelled) {
                QMetaObject::invokeMethod(this, [this]() { emit transcriptionCancelled(); }, Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(this, [this, error]() { emit transcriptionFailed(error); }, Qt::QueuedConnection);
            }
            return;
        }

        const QString modelPath = SpeechTranscriber::cachedModelPath();
        SpeechTranscriber transcriber;
        int successfulFiles = 0;
        int totalSegments = 0;
        QStringList errors;
        int fileIndex = 0;
        const int fileCount = sources.size();
        for (auto it = sources.cbegin(); it != sources.cend(); ++it, ++fileIndex) {
            if (m_transcriptionCancelled) break;
            const QString path = it.key();
            const double duration = it.value() > 0.0 ? it.value() : 0.0;
            const double base = 8.0 + 92.0 * fileIndex / fileCount;
            const double span = 92.0 / fileCount;
            QVariantList segments;
            QString fileError;
            const bool ok = transcriber.transcribeFile(path, duration, modelPath,
                [this] { return m_transcriptionCancelled.load(); },
                [&](double localPercent, const QString &text) {
                    report(base + span * localPercent / 100.0, text);
                }, &segments, &fileError);
            if (!ok) {
                if (!m_transcriptionCancelled) errors.append(QFileInfo(path).fileName() + ": " + fileError);
                continue;
            }

            const QString fingerprint = mediaFingerprint(path);
            {
                QMutexLocker lock(&m_speechMutex);
                m_speechAnnotations.insert(path, segments);
                m_speechFingerprints.insert(path, fingerprint);
            }
            successfulFiles++;
            totalSegments += segments.size();
            QMetaObject::invokeMethod(this, [this, path, segments]() {
                emit speechSegmentsReady(path, segments);
            }, Qt::QueuedConnection);
        }

        if (m_transcriptionCancelled) {
            QMetaObject::invokeMethod(this, [this]() { emit transcriptionCancelled(); }, Qt::QueuedConnection);
        } else if (successfulFiles == 0) {
            const QString summary = errors.isEmpty() ? QStringLiteral("没有素材完成转写") : errors.join("\n");
            QMetaObject::invokeMethod(this, [this, summary]() { emit transcriptionFailed(summary); }, Qt::QueuedConnection);
        } else {
            QString warning = errors.join("；");
            if (totalSegments == 0 && warning.isEmpty()) warning = QStringLiteral("未识别到语音；不会生成空字幕文件");
            QMetaObject::invokeMethod(this, [this, totalSegments, warning]() {
                emit transcriptionProgress(100.0, QString("转写完成，共识别 %1 段语音").arg(totalSegments));
                emit transcriptionFinished(totalSegments, warning);
            }, Qt::QueuedConnection);
        }
    });
}

void MediaEngine::safeEjectDevice() {
    if (!m_aiQuiesced) {
        if (m_aiRunning) pauseAiScreening("准备拔出设备录像");
        else emit statusMessage("当前没有正在处理的筛选任务，请确认其他程序已停止读取设备");
        return;
    }
    if (m_aiCheckpointOk) {
        emit statusMessage("已停止读取存储卡，检查点已留存。现在可以拔卡，插回后可从断点继续。");
    } else {
        emit statusMessage("已停止读取存储卡，现在可以拔卡。检查点没有写上，退出应用会丢掉未完成的进度。");
    }
}

void MediaEngine::startAiScreening(const QVariantList &mediaPool, const QStringList &selectedPaths, const QString &mode, double targetDurationSec, const QString &canvasMode, const QString &targetColor) {
    Q_UNUSED(canvasMode);
    Q_UNUSED(targetColor);

    if (mode != "1" && mode != "2" && mode != "3" && mode != "4") {
        emit aiFailed("未知的筛选模式");
        return;
    }

    if (busy()) { emit aiFailed("已有任务正在运行或停止，请稍后重试"); return; }
    m_aiRunning = true;
    emit busyChanged();
    m_aiPaused = false;
    m_aiQuiesced = false;
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
        struct RunningGuard { MediaEngine *engine; ~RunningGuard() {
            QMetaObject::invokeMethod(engine, [e = engine] { e->m_aiRunning = false; emit e->busyChanged(); if (e->m_aiCancelled) emit e->aiCancelled(); }, Qt::QueuedConnection);
        } } guard{this};
        SpeechTranscriber speechTranscriber;
        int total = effPaths.size();
        if (total == 0) {
            QMetaObject::invokeMethod(this, [this]() { emit aiFinished(QVariantList()); });
            return;
        }

        QJsonObject fingerprints;
        for (const QString &path : effPaths) fingerprints[path] = mediaFingerprint(path);
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
                saveAiCheckpoint(effPaths, idx, allTasks, mode, targetDurationSec, fingerprints);
                return;
            }
            if (m_aiPaused) {
                bool saved = saveAiCheckpoint(effPaths, idx, allTasks, mode, targetDurationSec, fingerprints);
                m_aiQuiesced = true;
                m_aiCheckpointOk = saved;
                QMetaObject::invokeMethod(this, [this, idx, total, saved]() { emit aiPaused(idx, total, saved ? "检查点已保存" : "检查点保存失败，请勿退出应用"); });
            }
            while (m_aiPaused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (m_aiCancelled) {
                    m_aiQuiesced = false;
                    saveAiCheckpoint(effPaths, idx, allTasks, mode, targetDurationSec, fingerprints);
                    return;
                }
            }
            m_aiQuiesced = false;

            QString path = effPaths[idx];
            QVariantMap meta = poolMap.value(path);
            if (meta.isEmpty()) meta = probeFile(path);
            QString filename = meta.value("filename").toString();
            if (!meta.contains("duration") || meta.value("duration").toDouble() <= 0) {
                QMetaObject::invokeMethod(this, [this, filename]() {
                    emit aiFailed((filename.isEmpty() ? QString("素材") : filename) + ": 无法读取时长，已停止");
                });
                return;
            }
            double dur = meta.value("duration").toDouble();

            double pInit = std::max(1.0, (static_cast<double>(idx) / total) * 100.0);
            QMetaObject::invokeMethod(this, [this, pInit, idx, total, filename]() {
                emit aiProgress(pInit, QString("[%1/%2] 准备初筛分析: %3").arg(idx + 1).arg(total).arg(filename));
            });

            QVariantList currentFileTasks;
            QString analysisError;
            std::vector<screening::Span> candidates;
            auto report = [&](double clipFraction, const QString &label) {
                const double fraction = std::clamp(clipFraction, 0.0, 0.999);
                const double percent = std::max(1.0, std::min(99.0, ((idx + fraction) / total) * 100.0));
                const QString text = QString("[%1/%2] %3: %4 · 本段 %5%")
                    .arg(idx + 1).arg(total).arg(label).arg(filename).arg(int(std::lround(fraction * 100)));
                QMetaObject::invokeMethod(this, [this, percent, text]() { emit aiProgress(percent, text); });
            };
            QVariantList speechSegments;
            if (mode == "4") {
                if (!meta.contains("has_audio")) meta = probeFile(path);
                if (meta.isEmpty()) analysisError = "无法读取素材信息";
                else if (meta.value("has_audio").toBool()) {
                    const auto stopped = [this] { return m_aiCancelled.load() || m_aiPaused.load(); };
                    if (SpeechTranscriber::prepareModel(stopped,
                        [&](double p, const QString &text) { report(p * 0.002, text); }, &analysisError)) {
                        speechTranscriber.transcribeFile(path, dur, SpeechTranscriber::cachedModelPath(), stopped,
                            [&](double p, const QString &text) { report(0.2 + p * 0.008, text); }, &speechSegments, &analysisError);
                    }
                    if (stopped()) { --idx; continue; }
                    speechSegments = removeRepeatedSpeech(speechSegments);
                    for (const QVariant &item : speechSegments) {
                        const auto segment = item.toMap();
                        candidates.emplace_back(std::max(0.0, segment.value("start").toDouble()),
                            std::min(dur, segment.value("end").toDouble()));
                    }
                    }
            } else if (mode != "1" && mode != "4") {
                // No audio is a valid empty result; it is not a reason to retain
                // the first 25 seconds of silent footage.
                QVariantMap sourceMeta = meta.contains("has_audio") ? meta : probeFile(path);
                if (m_aiPaused || m_aiCancelled) { --idx; continue; }
                if (sourceMeta.isEmpty()) analysisError = "无法读取素材信息";
                else if (sourceMeta.value("has_audio").toBool()) {
                    const auto voices = detectVoiceSegments(path, -30.0,
                        mode == "4" ? 0.6 : 0.8, 0.3, &analysisError, dur,
                        [&](double fraction) { report(mode == "3" ? fraction * 0.5 : fraction, "分析音频"); });
                    if (m_aiPaused || m_aiCancelled) { --idx; continue; }
                    for (const auto &voice : voices) {
                        const double padding = mode == "4" ? 0.15 : 0.0;
                        candidates.emplace_back(voice.first - padding, voice.second + padding);
                    }
                }
            }
            if ((mode == "1" || mode == "3") && analysisError.isEmpty() && !m_aiCancelled && !m_aiPaused) {
                const auto visuals = detectVisualSegments(path, dur, analysisError,
                    [&](double fraction) { report(mode == "3" ? 0.5 + fraction * 0.5 : fraction, "检查画面"); });
                if (m_aiPaused || m_aiCancelled) { --idx; continue; }
                for (const auto &visual : visuals) candidates.emplace_back(visual.first, visual.second);
            }
            if (m_aiCancelled) {
                saveAiCheckpoint(effPaths, idx, allTasks, mode, targetDurationSec, fingerprints);
                return;
            }
            if (m_aiPaused) { --idx; continue; }
            if (!analysisError.isEmpty() && QFileInfo::exists(path)) {
                saveAiCheckpoint(effPaths, idx, allTasks, mode, targetDurationSec, fingerprints);
                QMetaObject::invokeMethod(this, [this, filename, analysisError]() {
                    emit aiFailed(filename + ": " + analysisError);
                });
                return;
            }
            const auto spans = mode == "4" ? candidates : screening::mergeSpans(std::move(candidates), dur);
            for (const auto &span : spans) {
                const double clipDur = span.second - span.first;
                if (clipDur < 0.5) continue;
                QVariantMap clip;
                clip["clip_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
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
                clip["media_dur"] = dur;
                clip["has_audio"] = meta.value("has_audio").toBool();
                clip["width"] = meta.value("width");
                clip["height"] = meta.value("height");
                if (meta.contains("fps")) clip["fps"] = meta.value("fps");
                if (mode == "4") {
                    QVariantList subtitles;
                    for (const QVariant &item : speechSegments) {
                        const auto sub = item.toMap();
                        if (sub.value("end").toDouble() > span.first && sub.value("start").toDouble() < span.second) subtitles.append(sub);
                    }
                    clip["subtitles"] = subtitles;
                }
                currentFileTasks.append(clip);
            }

            if (!QFileInfo::exists(path)) {
                m_aiPaused = true;
                --idx;
                continue;
            }
            if (mediaFingerprint(path) != fingerprints.value(path).toString()) {
                saveAiCheckpoint(effPaths, idx, allTasks, mode, targetDurationSec, fingerprints);
                QMetaObject::invokeMethod(this, [this] { emit aiFailed("素材在分析期间发生变化，请重新导入后筛选"); });
                return;
            }
            allTasks.append(currentFileTasks);
            saveAiCheckpoint(effPaths, idx + 1, allTasks, mode, targetDurationSec, fingerprints);
        }

        if (m_aiPaused && !m_aiCancelled) {
            bool saved = saveAiCheckpoint(effPaths, total, allTasks, mode, targetDurationSec, fingerprints);
            m_aiQuiesced = true;
            m_aiCheckpointOk = saved;
            QMetaObject::invokeMethod(this, [this, total, saved]() { emit aiPaused(total, total, saved ? "检查点已保存" : "检查点保存失败，请勿退出应用"); });
            while (m_aiPaused && !m_aiCancelled) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            m_aiQuiesced = false;
        }
        if (m_aiCancelled) return;

        // 目标成片节奏 / 时长预算控制 (targetDurationSec > 0 时生效)
        if (targetDurationSec > 0 && !allTasks.isEmpty()) {
            QVariantList selected;
            double remaining = targetDurationSec;
            for (const QVariant &item : allTasks) {
                QVariantMap clip = item.toMap();
                const double fps = clip.value("fps", 60000.0 / 1001.0).toDouble();
                const double rate = fps > 0 ? fps : 60000.0 / 1001.0;
                const auto span = editing::budgetSpan(clip.value("start").toDouble(), clip.value("end").toDouble(), remaining, rate, mode == "4");
                if (span.second <= span.first) continue;
                clip["start"] = span.first;
                clip["end"] = span.second;
                clip["dur"] = span.second - span.first;
                clip["dur_str"] = QString("%1s").arg(span.second - span.first, 0, 'f', 1);
                selected.append(clip);
                remaining -= span.second - span.first;
            }
            allTasks = selected;
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

    for (const QVariant &item : clips) {
        const auto clip = item.toMap();
        const double start = clip.value("start").toDouble(), end = clip.value("end").toDouble(), duration = clip.value("dur").toDouble();
        if (clip.value("src").toString().isEmpty() || !std::isfinite(start) || !std::isfinite(end) || !std::isfinite(duration)
            || start < 0 || duration <= 0 || end <= start || std::abs(end - start - duration) > 0.02) {
            emit exportFailed("时间线包含无效出入点或零时长镜头，请重新筛选或调整后导出"); return;
        }
    }
    if (busy()) { emit exportFailed("已有任务正在运行或停止，请稍后重试"); return; }
    m_exportRunning = true;
    emit busyChanged();
    QString down = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(down);
    QString out = down + QString("/随剪_成片_%1.mp4").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz"));

    m_exportCancelled = false;
    const QString lutPath = m_lutPath;
    const QString beatMusic = m_beatMusic;
    double timelineSeconds = 0;
    for (const auto &clip : clips) timelineSeconds += clip.toMap().value("dur").toDouble();
    m_workers.emplace_back([this, clips, masterCanvas, targetColor, out, lutPath, beatMusic, timelineSeconds]() {
        struct RunningGuard { MediaEngine *engine; ~RunningGuard() {
            QMetaObject::invokeMethod(engine, [e = engine] { e->m_exportRunning = false; emit e->busyChanged(); if (e->m_exportCancelled) emit e->exportCancelled(); }, Qt::QueuedConnection);
        } } guard{this};
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
            const int transpose = t.value("transpose").toInt();
            int streamRotation = stream.value("tags").toObject().value("rotate").toString().toInt();
            for (const auto &side : stream.value("side_data_list").toArray()) {
                if (side.toObject().contains("rotation")) streamRotation = side.toObject().value("rotation").toInt();
            }
            const QString sar = stream.value("sample_aspect_ratio").toString();
            const int colon = sar.indexOf(':');
            double par = 1;
            if (colon > 0) {
                bool okN = false, okD = false;
                const double n = sar.left(colon).toDouble(&okN), d = sar.mid(colon + 1).toDouble(&okD);
                if (okN && okD && n > 0 && d > 0) par = n / d;
            }
            double clipAspect = stream.value("width").toInt() > 0 && stream.value("height").toInt() > 0
                ? stream.value("width").toInt() * par / stream.value("height").toInt() : 0;
            const int turns = ((streamRotation % 360) + 360) % 360;
            if (clipAspect > 0 && (turns == 90 || turns == 270)) clipAspect = 1 / clipAspect;
            if (clipAspect > 0 && (transpose == 1 || transpose == 2)) clipAspect = 1 / clipAspect;
            const double outputAspect = double(w) / h;
            const bool sameAspect = clipAspect > 0 && std::abs(clipAspect - outputAspect) / std::max(clipAspect, outputAspect) <= 0.02;
            const QString overlayPix = targetColor == "10bit" ? "yuv420p10" : "yuv420";
            const QString framePix = targetColor == "10bit" ? "yuv420p10le" : "yuv420p";
            QString prefix = transpose == 1 || transpose == 2 ? QString("transpose=%1,").arg(transpose) : QString();
            prefix += colorFilter;
            if (!lutPath.isEmpty() && targetColor == "8bit") {
                QFile lutFile(lutPath);
                const QString header = lutFile.open(QIODevice::ReadOnly) ? QString::fromUtf8(lutFile.read(4096)) : QString();
                if (!header.contains("LUT_3D_SIZE")) {
                    QMetaObject::invokeMethod(this, [this]() { emit exportFailed("LUT 文件不是可读取的 .cube 3D LUT"); });
                    return;
                }
                QString escaped = QFileInfo(lutPath).absoluteFilePath();
                escaped.replace("\\", "/");
                escaped.replace(":", "\\:");
                escaped.replace("'", "\\'");
                prefix += QString("lut3d=file='%1',").arg(escaped);
            } else if (!lutPath.isEmpty()) {
                QMetaObject::invokeMethod(this, [this]() { emit statusMessage("LUT 只在 8-bit SDR 导出时套用，本次 HLG 母带未改色彩路径"); });
            }
            QString filter = sameAspect
                ? QString("[0:v:0]%1scale=%2:%3,format=%4,setsar=1[outv]").arg(prefix).arg(w).arg(h).arg(framePix)
                : QString("[0:v:0]%1split=2[fgin][bgin];"
                    "[bgin]scale=%2:%3:force_original_aspect_ratio=increase,crop=%2:%3,boxblur=8:2,"
                    "scale=%4:%5:flags=fast_bilinear,eq=brightness=-0.08:saturation=1.15[bg];"
                    "[fgin]scale=%4:%5:force_original_aspect_ratio=decrease:force_divisible_by=2,setsar=1[fg];"
                    "[bg][fg]overlay=(W-w)/2:(H-h)/2:format=%6,setsar=1[outv]")
                    .arg(prefix).arg(bw).arg(bh).arg(w).arg(h).arg(overlayPix);
            args << "-filter_complex" << filter << "-map" << "[outv]" << "-map" << (hasAudio ? "0:a:0?" : "1:a:0?");

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
            if (m_exportCancelled) { QDir(scratch).removeRecursively(); return; }
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
        if (m_exportCancelled) { QFile::remove(out); QDir(scratch).removeRecursively(); return; }
        bool concatOk = concatProc.exitStatus() == QProcess::NormalExit && concatProc.exitCode() == 0;

        if (!(concatOk && QFile::exists(out) && QFileInfo(out).size() > 0)) {
            QFile::remove(out);
            QDir(scratch).removeRecursively();
            QMetaObject::invokeMethod(this, [this]() {
                emit exportFailed("最终合并失败，请检查硬盘空间");
            });
            return;
        }
        if (!beatMusic.isEmpty()) {
            if (!QFileInfo::exists(beatMusic)) {
                QFile::remove(out);
                QDir(scratch).removeRecursively();
                QMetaObject::invokeMethod(this, [this]() {
                    emit exportFailed("卡点音乐已不在原路径，成片未保留");
                });
                return;
            }
            const QString mixed = scratch + "/with-music.mp4";
            const QString graph = QString("[1:a:0]aresample=48000,atrim=0:%1,asetpts=PTS-STARTPTS,apad,atrim=0:%1[a]")
                .arg(std::max(0.1, timelineSeconds), 0, 'f', 3);
            QProcess mix;
            mix.start("ffmpeg", {"-y", "-i", out, "-i", beatMusic, "-filter_complex", graph,
                "-map", "0:v:0", "-map", "[a]", "-c:v", "copy", "-c:a", "aac", "-b:a", "320k",
                "-ar", "48000", "-ac", "2", "-movflags", "+faststart", mixed});
            while (!mix.waitForFinished(250)) {
                if (m_exportCancelled) { mix.kill(); mix.waitForFinished(); QFile::remove(out); QDir(scratch).removeRecursively(); return; }
                if (mix.state() == QProcess::NotRunning) break;
            }
            if (m_exportCancelled) { QFile::remove(out); QDir(scratch).removeRecursively(); return; }
            if (mix.exitStatus() != QProcess::NormalExit || mix.exitCode() != 0 || !QFile::exists(mixed) || QFileInfo(mixed).size() == 0) {
                const QString error = QString::fromUtf8(mix.readAllStandardError()).right(800);
                QFile::remove(out);
                QDir(scratch).removeRecursively();
                QMetaObject::invokeMethod(this, [this, error]() {
                    emit exportFailed(QString("卡点音乐没有写进成片：%1").arg(error));
                });
                return;
            }
            QFile::remove(out);
            if (!QFile::rename(mixed, out)) {
                QDir(scratch).removeRecursively();
                QMetaObject::invokeMethod(this, [this]() { emit exportFailed("无法替换成片音轨"); });
                return;
            }
        }
        QDir(scratch).removeRecursively();
        QMetaObject::invokeMethod(this, [this, out]() {
            emit exportProgress(100.0, "压制成功落盘！");
            emit exportFinished(out);
        });
    });
}

void MediaEngine::exportMetadata(const QVariantList &clips, const QString &formatType, const QString &outputPath) {
    QString ext = (formatType.toLower() == "csv") ? "csv" : "json";
    QString out = outputPath;
    if (out.isEmpty()) {
        QString down = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        out = down + QString("/随剪_元数据清单_%1.%2").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz")).arg(ext);
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
        const auto csvField = [](QString value) {
            value.replace("\"", "\"\"");
            return QStringLiteral("\"") + value + QStringLiteral("\"");
        };
        for (int i = 0; i < clips.size(); ++i) {
            QVariantMap c = clips[i].toMap();
            QString orientStr = (c.value("orient").toString() == "vertical") ? "竖屏 9:16" : "横屏 16:9";
            QString colorStr = c.value("is_10bit").toBool() ? "10-Bit" : "8-Bit";
            const QStringList fields = {
                QString::number(i + 1),
                csvField(c.value("filename").toString()),
                csvField(c.value("src").toString()),
                QString::number(c.value("start", 0.0).toDouble(), 'f', 2),
                QString::number(c.value("end", 0.0).toDouble(), 'f', 2),
                QString::number(c.value("dur", 0.0).toDouble(), 'f', 2),
                csvField(c.value("dur_str").toString()),
                csvField(orientStr),
                csvField(colorStr),
                csvField(transcriptTextForClip(c))
            };
            ts << fields.join(QLatin1Char(',')) << QLatin1Char('\n');
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
            cObj["transcript_text"] = transcriptTextForClip(c);
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
        out = down + QString("/随剪_剪辑工程_%1.fcpxml").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz"));
    }

    QSaveFile file(out);
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
        if (m_toolCancelled) return;
        QVariantMap c = clips[i].toMap();
        QString src = c.value("src").toString();
        if (!knownAssets.contains(src)) {
            knownAssets.append(src);
            int aid = knownAssets.size() + 1;
            const auto metadata = probeFile(src);
            if (metadata.isEmpty()) { emit statusMessage("素材不可读，FCPXML 未导出：" + src); return; }
            double dur = metadata.value("duration", 0.0).toDouble();
            const double fps = metadata.value("fps", 60000.0 / 1001.0).toDouble();
            const int denominator = std::abs(fps - 30000.0 / 1001.0) < 0.001 || std::abs(fps - 60000.0 / 1001.0) < 0.001
                || std::abs(fps - 24000.0 / 1001.0) < 0.001 ? 1001 : 1000;
            const qint64 numerator = qRound64(std::max(1.0, fps) * denominator);
            ts << QString("    <format id=\"f%1\" frameDuration=\"%2/%3s\" width=\"%4\" height=\"%5\"/>\n")
                .arg(aid).arg(denominator).arg(numerator).arg(metadata.value("width", w).toInt()).arg(metadata.value("height", h).toInt());
            for (const auto &clip : clips) {
                auto other = clip.toMap();
                if (other.value("src").toString() == src) dur = std::max(dur, other.value("end").toDouble());
            }
            ts << QString("    <asset id=\"r%1\" name=\"%2\" start=\"0s\" duration=\"%4/1000s\" hasVideo=\"1\" hasAudio=\"%5\" format=\"f%1\"><media-rep kind=\"original-media\" src=\"%3\"/></asset>\n")
                  .arg(aid)
                  .arg(c.value("filename").toString().toHtmlEscaped())
                  .arg(QString::fromUtf8(QUrl::fromLocalFile(src).toEncoded()).toHtmlEscaped())
                  .arg(qRound64(dur * 1000))
                  .arg(metadata.value("has_audio").toBool() ? 1 : 0);
        }
    }
    double musicDuration = 0;
    if (!m_beatMusic.isEmpty()) {
        QProcess probe;
        probe.start("ffprobe", {"-v", "error", "-show_entries", "format=duration", "-of", "default=noprint_wrappers=1:nokey=1", m_beatMusic});
        if (probe.waitForFinished(4000) && probe.exitCode() == 0) musicDuration = QString::fromUtf8(probe.readAllStandardOutput()).trimmed().toDouble();
        if (musicDuration <= 0) { emit statusMessage("无法读取卡点音乐，工程未完整导出"); return; }
        ts << QString("    <asset id=\"music\" name=\"卡点音乐\" start=\"0s\" duration=\"%2/1000s\" hasVideo=\"0\" hasAudio=\"1\"><media-rep kind=\"original-media\" src=\"%1\"/></asset>\n")
            .arg(QString::fromUtf8(QUrl::fromLocalFile(m_beatMusic).toEncoded()).toHtmlEscaped()).arg(qRound64(musicDuration * 1000));
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

        ts << QString("            <asset-clip ref=\"r%1\" name=\"镜头_%2_%3\" offset=\"%4/1000s\" start=\"%5/1000s\" duration=\"%6/1000s\" format=\"f%1\">\n")
              .arg(aid)
              .arg(i + 1)
              .arg(c.value("filename").toString().toHtmlEscaped())
              .arg(qRound64(offsetSec * 1000))
              .arg(qRound64(startSec * 1000))
              .arg(qRound64(durSec * 1000));
        const int turn = c.value("transpose").toInt();
        if (turn == 1 || turn == 2) ts << QString("              <adjust-transform rotation=\"%1\"/>\n").arg(turn == 1 ? -90 : 90);
        if (musicDuration > 0) ts << "              <adjust-volume amount=\"-96dB\"/>\n";
        if (i == 0 && musicDuration > 0) {
            ts << QString("              <asset-clip ref=\"music\" name=\"卡点音乐\" lane=\"-1\" offset=\"%1/1000s\" start=\"0s\" duration=\"%2/1000s\"/>\n")
                .arg(qRound64(startSec * 1000)).arg(qRound64(std::min(totalSec, musicDuration) * 1000));
        }
        ts << "            </asset-clip>\n";
        offsetSec += durSec;
    }

    ts << "          </spine>\n";
    ts << "        </sequence>\n";
    ts << "      </project>\n";
    ts << "    </event>\n";
    ts << "  </library>\n";
    ts << "</fcpxml>\n";

    ts.flush();
    if (m_toolCancelled) { file.cancelWriting(); return; }
    if (!file.commit()) { emit statusMessage("FCPXML 写入失败"); return; }
    exportProjectExtras(clips, masterCanvas, out);
    emit statusMessage(QString("FCPXML 已导出：%1。已保留出入点、旋转及音乐；字幕请导入同名 SRT，LUT 和毛玻璃背景需在目标软件重建；完整设置见同名 .suijian 工程").arg(out));
}

void MediaEngine::generateSrt(const QVariantList &clips, const QString &outputPath) {
    QString out = outputPath;
    if (out.isEmpty()) {
        QString down = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        out = down + QString("/随剪_字幕_%1.srt").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz"));
    }

    QSaveFile file(out);
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
        const double clipEnd = clip.value("end", clipStart + clipDur).toDouble();
        const QVariantList subs = transcriptSegmentsForClip(clip);

        for (const auto &sItem : subs) {
            QVariantMap sub = sItem.toMap();
            QString text = sub.value("text").toString().trimmed();
            if (text.isEmpty()) continue;

            const double sourceStart = sub.value("start").toDouble();
            const double sourceEnd = sub.value("end").toDouble();
            if (sourceEnd <= clipStart || sourceStart >= clipEnd) continue;
            const double subStart = sourceStart - clipStart;
            const double subEnd = sourceEnd - clipStart;
            double absStart = accumulated + std::max(0.0, subStart);
            double absEnd = accumulated + std::min(clipDur, subEnd);
            if (absEnd <= absStart) continue;

            auto formatTime = [](double s) {
                const qint64 totalMs = std::max<qint64>(0, qRound64(s * 1000.0));
                const qint64 h = totalMs / 3600000;
                const qint64 m = (totalMs / 60000) % 60;
                const qint64 sec = (totalMs / 1000) % 60;
                const qint64 ms = totalMs % 1000;
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
    if (idx == 1) {
        ts.flush();
        file.cancelWriting();
        emit statusMessage("没有可导出的字幕。请先运行「语音转写」，或为镜头添加字幕文本");
        return;
    }
    ts.flush();
    if (!file.commit()) {
        emit statusMessage("字幕文件写入失败");
        return;
    }
    emit statusMessage(QString("字幕已导出: %1").arg(out));
}

void MediaEngine::setBeatMusic(const QString &path) {
    QString local = path;
    if (local.startsWith("file://")) local = QUrl(local).toLocalFile();
    if (local == m_beatMusic) return;
    m_beatMusic = local;
    emit beatMusicChanged();
}

void MediaEngine::copyText(const QString &text) {
    if (auto *app = QGuiApplication::instance()) {
        if (auto *clipboard = QGuiApplication::clipboard()) clipboard->setText(text);
    }
}

QString MediaEngine::beatMusic() const { return m_beatMusic; }

void MediaEngine::setExportLut(const QString &cubePath) {
    QString path = cubePath;
    if (path.startsWith("file://")) path = QUrl(path).toLocalFile();
    m_lutPath = path;
    emit statusMessage(path.isEmpty() ? "已清除 LUT" : QString("导出将套用 LUT: %1").arg(QFileInfo(path).fileName()));
}

static QList<double> beatTimesFrom(const QString &mediaPath, const std::atomic<bool> &cancelled) {
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", {"-v", "error", "-i", mediaPath, "-ac", "1", "-ar", "22050", "-f", "s16le", "pipe:1"});
    QByteArray pcm;
    QElapsedTimer timeout;
    timeout.start();
    while (!ffmpeg.waitForFinished(100)) {
        pcm += ffmpeg.readAllStandardOutput();
        ffmpeg.readAllStandardError();
        if (cancelled || timeout.elapsed() > 120000 || pcm.size() > 128 * 1024 * 1024) {
            ffmpeg.kill(); ffmpeg.waitForFinished(); return {};
        }
    }
    pcm += ffmpeg.readAllStandardOutput();
    if (cancelled || ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) return {};
    const auto *samples = reinterpret_cast<const qint16 *>(pcm.constData());
    const int count = pcm.size() / int(sizeof(qint16));
    const int rate = 22050;
    const int window = rate / 40;
    if (count < window * 4) return {};
    QList<double> energy;
    for (int i = 0; i + window <= count; i += window) {
        if (cancelled) return {};
        double sum = 0;
        for (int k = 0; k < window; ++k) sum += std::abs(samples[i + k]);
        energy.append(sum / window);
    }
    double mean = 0;
    for (double value : energy) mean += value;
    mean /= energy.size();
    QList<double> beats;
    for (int i = 1; i + 1 < energy.size(); ++i) {
        if (energy[i] > energy[i - 1] && energy[i] >= energy[i + 1] && energy[i] > mean * 1.6) {
            const double time = i * double(window) / rate;
            if (beats.isEmpty() || time - beats.last() >= 0.25) beats.append(time);
        }
    }
    return beats;
}

QString MediaEngine::builtinBeatTrack() {
    const QString path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/suijian_demo_beat.wav";
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (QFileInfo::exists(path) && QFileInfo(path).size() > 44) return path;
    const int rate = 22050;
    const int frames = rate * 4;
    QByteArray pcm(frames * 2, 0);
    auto *samples = reinterpret_cast<qint16 *>(pcm.data());
    for (int beat = 0; beat < 8; ++beat) {
        const int start = beat * rate / 2;
        for (int i = 0; i < rate / 40 && start + i < frames; ++i) {
            const double envelope = std::exp(-i / (rate / 200.0));
            samples[start + i] = qint16(std::sin(6.28318530718 * 90 * i / rate) * envelope * 28000);
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    QByteArray header(44, 0);
    header.replace(0, 4, "RIFF");
    const quint32 chunk = 36 + pcm.size();
    header[4] = chunk & 255; header[5] = (chunk >> 8) & 255; header[6] = (chunk >> 16) & 255; header[7] = (chunk >> 24) & 255;
    header.replace(8, 4, "WAVE");
    header.replace(12, 4, "fmt ");
    header[16] = 16;
    header[20] = 1;
    header[22] = 1;
    header[24] = rate & 255; header[25] = (rate >> 8) & 255;
    const quint32 byteRate = rate * 2;
    header[28] = byteRate & 255; header[29] = (byteRate >> 8) & 255; header[30] = (byteRate >> 16) & 255; header[31] = (byteRate >> 24) & 255;
    header[32] = 2; header[34] = 16;
    header.replace(36, 4, "data");
    const quint32 dataSize = pcm.size();
    header[40] = dataSize & 255; header[41] = (dataSize >> 8) & 255; header[42] = (dataSize >> 16) & 255; header[43] = (dataSize >> 24) & 255;
    file.write(header);
    file.write(pcm);
    return path;
}

QVariantList MediaEngine::snapToMusic(const QVariantList &clips, const QString &musicPath) {
    QString path = musicPath;
    if (path.startsWith("file://")) path = QUrl(path).toLocalFile();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        emit statusMessage("没有可用的音乐文件，时间线未改");
        return clips;
    }
    const QList<double> beats = beatTimesFrom(path, m_toolCancelled);
    if (beats.isEmpty()) {
        emit statusMessage("没有从音乐里找到重音，时间线未改");
        return clips;
    }
    QVariantList snapped;
    double cursor = 0;
    bool changed = false;
    for (const auto &item : clips) {
        QVariantMap clip = item.toMap();
        const double start = std::max(0.0, clip.value("start").toDouble());
        const double duration = std::max(0.0, clip.value("dur").toDouble());
        const double media = clip.value("media_dur").toDouble();
        const double room = media > 0 ? media - start : duration;
        if (room < 0.1) {
            snapped.append(clip);
            cursor += duration;
            continue;
        }
        double cut = cursor + std::min(duration, room);
        double nearest = cut;
        double gap = 1e9;
        for (double beat : beats) {
            if (beat < cursor + 0.1 || beat > cursor + room + 0.001) continue;
            const double distance = std::abs(beat - cut);
            if (distance < gap) { gap = distance; nearest = beat; }
        }
        const double newDur = std::max(0.1, nearest - cursor);
        if (std::abs(newDur - duration) > 0.01) changed = true;
        clip["start"] = start;
        clip["end"] = start + newDur;
        clip["dur"] = newDur;
        clip["dur_str"] = QString("%1s").arg(newDur, 0, 'f', 1);
        snapped.append(clip);
        cursor += newDur;
    }
    if (!changed) {
        emit statusMessage("这些重音落不到现有素材长度里，时间线未改");
        return clips;
    }
    m_beatMusic = path;
    emit beatMusicChanged();
    emit statusMessage(QString("剪切点已按成片时间落到 %1 个重音上。导出时成片音轨换成这首音乐，镜头原声不进入成片").arg(beats.size()));
    return snapped;
}

QVariantList MediaEngine::arrangeStoryline(const QVariantList &clips) {
    if (clips.size() < 3) {
        emit statusMessage("至少 3 个镜头才按起承转合重排");
        return clips;
    }
    QList<QPair<double, int>> ranked;
    for (int i = 0; i < clips.size(); ++i) ranked.append({clips[i].toMap().value("dur").toDouble(), i});
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    const int opening = ranked.first().second;
    const int ending = ranked[1].second;
    const int climax = ranked.last().second;
    QVariantList result;
    result.append(clips[opening]);
    for (int i = 0; i < clips.size(); ++i) {
        if (i != opening && i != ending && i != climax) result.append(clips[i]);
    }
    result.append(clips[climax]);
    result.append(clips[ending]);
    emit statusMessage("已按起、承、转、合重排：短镜头开场，最长镜头作高潮，次短镜头收尾");
    return result;
}

static QStringList queryTokens(const QString &query) {
    QStringList parts;
    QString current;
    auto flush = [&] {
        QString token = current.trimmed().normalized(QString::NormalizationForm_C);
        if (token.startsWith("只留")) token = token.mid(2);
        if (token.startsWith("留下")) token = token.mid(2);
        static const QRegularExpression cjkPattern("[\\p{Han}\\p{Hiragana}\\p{Katakana}\\p{Hangul}]");
        if (!token.isEmpty() && (token.size() >= 2 || cjkPattern.match(token).hasMatch())) parts.append(token);
        current.clear();
    };
    for (const QChar &ch : query) {
        if (ch.isSpace() || QString("，。,.、和与或").contains(ch)) flush();
        else current.append(ch);
    }
    flush();
    parts.removeDuplicates();
    return parts;
}

static bool queryTermMatches(const QString &text, const QString &term) {
    static const QRegularExpression cjkPattern("[\\p{Han}\\p{Hiragana}\\p{Katakana}\\p{Hangul}]");
    if (cjkPattern.match(term).hasMatch()) return text.contains(term, Qt::CaseInsensitive);

    const QString pattern = QString("(?<![\\p{L}\\p{N}])%1(?![\\p{L}\\p{N}])")
        .arg(QRegularExpression::escape(term));
    const QRegularExpression expression(pattern,
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    return expression.match(text).hasMatch();
}

QVariantList MediaEngine::roughCutByWords(const QVariantList &clips, const QString &query) {
    const QStringList tokens = queryTokens(query);
    if (tokens.isEmpty()) {
        emit statusMessage("请输入要保留的词，时间线未改");
        return clips;
    }
    QVariantList kept;
    for (const auto &item : clips) {
        const QVariantMap clip = item.toMap();
        QString filename = clip.value("filename").toString();
        if (filename.isEmpty()) filename = QFileInfo(clip.value("src").toString()).fileName();
        filename = QFileInfo(filename).completeBaseName();
        const QStringList searchable = {
            filename.normalized(QString::NormalizationForm_C),
            clip.value("text").toString().normalized(QString::NormalizationForm_C),
            transcriptTextForClip(clip).normalized(QString::NormalizationForm_C),
            clip.value("orient").toString() == "vertical" ? QStringLiteral("竖屏") : QStringLiteral("横屏")
        };
        for (const QString &token : tokens) {
            if (std::any_of(searchable.cbegin(), searchable.cend(), [&](const QString &text) {
                    return queryTermMatches(text, token);
                })) {
                kept.append(clip);
                break;
            }
        }
    }
    if (kept.isEmpty()) {
        emit statusMessage("没有镜头的文件名或已有字幕对上这些词，时间线未改");
        return clips;
    }
    emit statusMessage(QString("按词语留下 %1 个镜头。只匹配文件名和已有文字，不识别画面内容").arg(kept.size()));
    return kept;
}

QString MediaEngine::socialCopy(const QVariantList &clips, const QString &platform) {
    if (clips.isEmpty()) return "时间线上没有镜头";
    double seconds = 0;
    for (const auto &item : clips) seconds += item.toMap().value("dur").toDouble();
    const QVariantMap first = clips.first().toMap();
    const QString frame = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/suijian_social.jpg";
    QDir().mkpath(QFileInfo(frame).absolutePath());
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", {"-y", "-ss", QString::number(first.value("start").toDouble()),
        "-i", first.value("src").toString(), "-frames:v", "1", "-vf", "scale=720:-1", frame});
    QElapsedTimer timeout; timeout.start();
    while (!ffmpeg.waitForFinished(100)) {
        if (m_toolCancelled || timeout.elapsed() > 8000) { ffmpeg.kill(); ffmpeg.waitForFinished(); break; }
    }
    const QString shot = QString("%1 个镜头，约 %2 秒").arg(clips.size()).arg(int(std::lround(seconds)));
    const QString still = QFileInfo::exists(frame) ? QString("关键帧: %1").arg(frame) : "关键帧未生成";
    if (platform == "xhs") {
        return QString("标题：%1 个瞬间\n正文：%2。按文件名整理，不是画面识别。\n%3\n#随剪 #航拍").arg(clips.size()).arg(shot).arg(still);
    }
    return QString("【随剪】%1。%2").arg(shot, still);
}

void MediaEngine::exportJianying(const QVariantList &clips, const QString &masterCanvas, const QString &outputDir) {
    if (clips.isEmpty()) { emit statusMessage("时间线上没有镜头"); return; }
    QString dirPath = outputDir;
    if (dirPath.startsWith("file://")) dirPath = QUrl(dirPath).toLocalFile();
    if (dirPath.isEmpty()) {
        dirPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
            + QString("/随剪_剪映_%1").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz"));
    }
    QDir().mkpath(dirPath);
    const bool vertical = masterCanvas == "vertical";
    const int canvasW = vertical ? 1080 : 1920;
    const int canvasH = vertical ? 1920 : 1080;
    QJsonArray videos, segments, audios, audioSegments;
    qint64 cursor = 0;
    double projectFps = 0;
    for (int i = 0; i < clips.size(); ++i) {
        if (m_toolCancelled) return;
        const QVariantMap clip = clips[i].toMap();
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qint64 sourceStart = qint64(std::max(0.0, clip.value("start").toDouble()) * 1000000);
        const qint64 duration = qint64(clip.value("dur").toDouble() * 1000000);
        if (duration <= 0) continue;
        QVariantMap probed;
        int vw = clip.value("width").toInt();
        int vh = clip.value("height").toInt();
        double fps = clip.value("fps").toDouble();
        if (vw <= 0 || vh <= 0 || fps < 1.0) probed = probeFile(clip.value("src").toString());
        if (vw <= 0) vw = probed.value("width").toInt();
        if (vh <= 0) vh = probed.value("height").toInt();
        if (fps < 1.0) fps = probed.value("fps").toDouble();
        if (vw <= 0 || vh <= 0) {
            emit statusMessage(QString("无法读取 %1 的画面尺寸，剪映草稿未写出").arg(clip.value("filename").toString()));
            return;
        }
        if (projectFps < 1.0 && fps >= 1.0 && fps <= 1000.0) projectFps = fps;
        const qint64 fullDuration = clip.contains("media_dur")
            ? qint64(clip.value("media_dur").toDouble() * 1000000)
            : sourceStart + duration;
        videos.append(QJsonObject{{"id", id}, {"path", clip.value("src").toString()}, {"material_name", clip.value("filename").toString()},
            {"duration", fullDuration}, {"has_audio", clip.value("has_audio").toBool()}, {"width", vw}, {"height", vh}, {"type", "video"}});
        segments.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"material_id", id},
            {"extra_material_refs", QJsonArray()}, {"speed", 1.0}, {"volume", m_beatMusic.isEmpty() ? 1.0 : 0.0}, {"visible", true},
            {"clip", QJsonObject{{"rotation", clip.value("transpose").toInt() == 1 ? 90.0 : (clip.value("transpose").toInt() == 2 ? -90.0 : 0.0)}, {"scale", QJsonObject{{"x", 1.0}, {"y", 1.0}}}, {"transform", QJsonObject{{"x", 0.0}, {"y", 0.0}}}}},
            {"source_timerange", QJsonObject{{"start", sourceStart}, {"duration", duration}}},
            {"target_timerange", QJsonObject{{"start", cursor}, {"duration", duration}}}});
        cursor += duration;
    }
    if (videos.isEmpty()) { emit statusMessage("没有可写入的镜头"); return; }
    if (projectFps < 1.0) projectFps = 60000.0 / 1001.0;
    if (!m_beatMusic.isEmpty()) {
        QProcess probe;
        probe.start("ffprobe", {"-v", "error", "-show_entries", "format=duration", "-of", "default=noprint_wrappers=1:nokey=1", m_beatMusic});
        if (!probe.waitForFinished(4000) || probe.exitCode() != 0) { emit statusMessage("音乐读取失败，剪映草稿未导出"); return; }
        const qint64 musicLength = qRound64(QString::fromUtf8(probe.readAllStandardOutput()).trimmed().toDouble() * 1000000);
        if (musicLength <= 0) { emit statusMessage("音乐时长无效，剪映草稿未导出"); return; }
        const qint64 musicUsed = std::min(cursor, musicLength);
        const QString musicId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        audios.append(QJsonObject{{"id", musicId}, {"path", m_beatMusic}, {"name", QFileInfo(m_beatMusic).fileName()}, {"type", "music"}, {"duration", musicLength}});
        audioSegments.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"material_id", musicId}, {"speed", 1.0}, {"volume", 1.0},
            {"source_timerange", QJsonObject{{"start", 0}, {"duration", musicUsed}}}, {"target_timerange", QJsonObject{{"start", 0}, {"duration", musicUsed}}}});
    }
    QJsonObject content{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"fps", projectFps}, {"duration", cursor},
        {"canvas_config", QJsonObject{{"width", canvasW}, {"height", canvasH}, {"ratio", vertical ? "9:16" : "16:9"}}},
        {"materials", QJsonObject{{"videos", videos}, {"audios", audios}, {"texts", QJsonArray()}}},
        {"tracks", QJsonArray{QJsonObject{{"id", "video"}, {"type", "video"}, {"segments", segments}}, QJsonObject{{"id", "audio"}, {"type", "audio"}, {"segments", audioSegments}}}}};
    QSaveFile draft(dirPath + "/draft_content.json");
    if (!draft.open(QIODevice::WriteOnly)) { emit statusMessage("无法写入剪映草稿"); return; }
    const auto draftData = QJsonDocument(content).toJson(QJsonDocument::Indented);
    if (draft.write(draftData) != draftData.size() || m_toolCancelled || !draft.commit()) { emit statusMessage("剪映草稿写入失败或已取消"); return; }
    QSaveFile meta(dirPath + "/draft_meta_info.json");
    if (meta.open(QIODevice::WriteOnly)) {
        meta.write(QJsonDocument(QJsonObject{{"draft_name", "随剪"}, {"draft_id", content.value("id").toString()}, {"tm_duration", cursor}}).toJson());
        if (!meta.commit()) { emit statusMessage("剪映草稿目录信息写入失败"); return; }
    }
    exportProjectExtras(clips, masterCanvas, dirPath + "/timeline");
    emit statusMessage(QString("剪映草稿已写出：%1。已写入旋转和音乐轨；字幕需导入 timeline.srt，LUT 和模糊背景需重建。完整设置在 timeline.suijian；草稿仍需在剪映客户端验证").arg(dirPath));
}

bool MediaEngine::busy() const {
    return m_aiRunning || m_exportRunning || m_transcriptionRunning || m_toolRunning || m_importRunning;
}
void MediaEngine::cancelAi() { m_aiCancelled = true; m_aiPaused = false; }
void MediaEngine::cancelExport() { m_exportCancelled = true; }
void MediaEngine::cancelTools() { m_toolCancelled = true; }
bool MediaEngine::beginTool() {
    if (busy()) { emit statusMessage("已有任务正在运行或停止，请稍后重试"); return false; }
    m_toolCancelled = false;
    m_toolRunning = true;
    emit busyChanged();
    return true;
}
void MediaEngine::endTool() {
    QMetaObject::invokeMethod(this, [this] {
        m_toolRunning = false;
        emit busyChanged();
        emit toolsFinished();
    }, Qt::QueuedConnection);
}
void MediaEngine::snapToMusicAsync(const QVariantList &clips, const QString &path) {
    if (!beginTool()) { emit toolsFinished(); return; }
    m_workers.emplace_back([this, clips, path] {
        const auto result = snapToMusic(clips, path);
        if (!m_toolCancelled) emit musicSnapped(result);
        endTool();
    });
}
void MediaEngine::exportFcpxmlAsync(const QVariantList &clips, const QString &canvas, const QString &path) {
    if (!beginTool()) { emit toolsFinished(); return; }
    m_workers.emplace_back([this, clips, canvas, path] { exportFcpxml(clips, canvas, path); endTool(); });
}
void MediaEngine::socialCopyAsync(const QVariantList &clips, const QString &platform) {
    if (!beginTool()) { emit toolsFinished(); return; }
    m_workers.emplace_back([this, clips, platform] {
        const auto result = socialCopy(clips, platform);
        if (!m_toolCancelled) emit socialCopyReady(result);
        endTool();
    });
}

static QString projectPath(QString path) {
    if (path.startsWith("file:")) path = QUrl(path).toLocalFile();
    if (path.isEmpty()) path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/recovery.suijian";
    return path;
}
bool MediaEngine::saveProject(const QVariantMap &project, const QString &path) {
    const QString out = projectPath(path);
    if (!QDir().mkpath(QFileInfo(out).absolutePath())) { emit statusMessage("无法创建工程保存目录"); return false; }
    QJsonObject document = QJsonObject::fromVariantMap(project);
    document["schema_version"] = 1;
    QSaveFile file(out);
    const QByteArray data = QJsonDocument(document).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        emit statusMessage("工程保存失败，请检查磁盘空间和写入权限");
        return false;
    }
    return true;
}
QVariantMap MediaEngine::loadProject(const QString &path) {
    QFile file(projectPath(path));
    if (!file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly)) { emit statusMessage("无法打开工程"); return {}; }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto object = document.object();
    if (error.error != QJsonParseError::NoError || object.value("schema_version").toInt() != 1
        || !object.value("clips").isArray()) {
        emit statusMessage("工程格式无效或版本不受支持，当前时间线已保留"); return {};
    }
    for (const auto &item : object.value("clips").toArray()) {
        const auto c = item.toObject();
        const double start = c.value("start").toDouble(-1), end = c.value("end").toDouble(-1);
        if (c.value("src").toString().isEmpty() || !std::isfinite(start) || !std::isfinite(end)
            || start < 0 || end <= start || std::abs(c.value("dur").toDouble() - (end - start)) > 0.02) {
            emit statusMessage("工程包含无效镜头，当前时间线已保留"); return {};
        }
    }
    auto project = object.toVariantMap();
    QVariantMap missing;
    for (const auto &item : object.value("clips").toArray()) {
        const auto path = item.toObject().value("src").toString();
        if (!QFileInfo::exists(path)) missing[path] = true;
    }
    project["missing_paths"] = missing;
    return project;
}

void MediaEngine::exportProjectExtras(const QVariantList &clips, const QString &canvas, const QString &base) {
    QVariantList annotated;
    for (const QVariant &item : clips) {
        auto clip = item.toMap();
        clip["subtitles"] = transcriptSegmentsForClip(clip);
        annotated.append(clip);
    }
    saveProject({{"clips", annotated}, {"canvas", canvas}, {"beatMusic", m_beatMusic}, {"lut", m_lutPath}}, base + ".suijian");
    generateSrt(annotated, base + ".srt");
}

void MediaEngine::exportJianyingAsync(const QVariantList &clips, const QString &canvas, const QString &path) {
    if (!beginTool()) { emit toolsFinished(); return; }
    m_workers.emplace_back([this, clips, canvas, path] {
        if (!m_toolCancelled) exportJianying(clips, canvas, path);
        endTool();
    });
}
