#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QThread>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QSet>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

class MediaEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString exportDirectory READ exportDirectory NOTIFY exportDirectoryChanged)

public:
    explicit MediaEngine(QObject *parent = nullptr);
    virtual ~MediaEngine();
    bool busy() const;

public slots:
    void importPaths(const QStringList &paths);
    void detectDji();
    void loadRecentDirs();
    void scanDirectory(const QString &dirPath);
    void startAiScreening(const QVariantList &selectedPaths, int targetDurationSec, const QString &mode);
    void startAiScreening(const QVariantList &mediaPool, const QStringList &selectedPaths, const QString &mode, double targetDurationSec, const QString &canvasMode, const QString &targetColor);
    void pauseAiScreening(const QString &reason = QString("用户主动暂停"));
    void resumeAiScreening();
    void cancelCurrentJob();
    void cancelAi();
    void cancelExport();
    void cancelTools();
    bool saveProject(const QVariantMap &project, const QString &path = QString());
    QVariantMap loadProject(const QString &path = QString());
    void snapToMusicAsync(const QVariantList &clips, const QString &path);
    void exportFcpxmlAsync(const QVariantList &clips, const QString &canvas, const QString &path);
    void exportJianyingAsync(const QVariantList &clips, const QString &canvas, const QString &path);
    void socialCopyAsync(const QVariantList &clips, const QString &platform);
    void transcribeTimeline(const QVariantList &clips);
    void cancelTranscription();
    void safeEjectDevice();
    QVariantMap exportReadiness(const QVariantList &clips) const;
    QUrl localMediaUrl(const QString &path) const { return QUrl::fromLocalFile(path); }
    bool openOutputFolder(const QString &path) const;
    QString exportDirectory() const;
    QUrl exportDirectoryUrl() const;
    bool setExportDirectory(const QUrl &folder);
    void startExport(const QVariantList &clips, const QString &masterCanvas, const QString &targetColor);
    void generateSrt(const QVariantList &clips, const QString &outputPath);
    void exportMetadata(const QVariantList &clips, const QString &formatType, const QString &outputPath);
    void exportFcpxml(const QVariantList &clips, const QString &masterCanvas, const QString &outputPath);
    void setExportLut(const QString &cubePath);
    void setBeatMusic(const QString &path);
    void copyText(const QString &text);
    void exportJianying(const QVariantList &clips, const QString &masterCanvas, const QString &outputDir);
    QString builtinBeatTrack();
    QVariantList snapToMusic(const QVariantList &clips, const QString &musicPath);
    QVariantList arrangeStoryline(const QVariantList &clips);
    QVariantList roughCutByWords(const QVariantList &clips, const QString &query);
    QString socialCopy(const QVariantList &clips, const QString &platform);
    QString beatMusic() const;

signals:
    void exportDirectoryChanged();
    void busyChanged();
    void aiCancelled();
    void exportCancelled();
    void toolsFinished();
    void musicSnapped(const QVariantList &clips);
    void socialCopyReady(const QString &text);
    void beatMusicChanged();
    void mediaLoaded(const QStringList &dates, const QVariantList &mediaList, const QString &sourceLabel);
    void importProgress(int current, int total, const QString &filename);
    void djiFound(const QString &path);
    void recentDirsLoaded(const QStringList &dirs);
    void statusMessage(const QString &message);
    void scanError(const QString &errorMessage);
    void aiProgress(double percent, const QString &stepText);
    void aiFinished(const QVariantList &tasks);
    void aiFailed(const QString &errorMessage);
    void aiPaused(int completed, int total, const QString &reason);
    void aiResumed();
    void speechSegmentsReady(const QString &videoPath, const QVariantList &segments);
    void transcriptionProgress(double percent, const QString &stepText);
    void transcriptionFinished(int segmentCount, const QString &warning);
    void transcriptionFailed(const QString &errorMessage);
    void transcriptionCancelled();
    void deviceDisconnected(const QString &deviceName);
    void deviceReconnected(const QString &deviceName, const QVariantMap &diff);
    void usbDeviceDetected(const QString &deviceName, const QString &mediaDir, int fileCount);
    void usbDeviceRemoved(const QString &deviceName);
    void exportProgress(double percent, const QString &stepText);
    void exportFinished(const QString &outputPath);
    void outputReady(const QString &outputPath);
    void exportFailed(const QString &errorMessage);

private:
    void checkDeviceHeartbeat();
    bool beginTool();
    void endTool();
    void exportProjectExtras(const QVariantList &clips, const QString &canvas, const QString &base);
    QString writeSrt(const QVariantList &clips, const QString &outputPath);
    QString exportMediaError(const QVariantList &clips) const;
    QVariantMap probeFile(const QString &filepath);
    QList<QPair<double, double>> detectVoiceSegments(const QString &audioOrVideoPath, double noiseDb = -30.0, double minSilenceDur = 0.6, double minSpeechDur = 0.3, QString *error = nullptr, double duration = 0, const std::function<void(double)> &onProgress = {});
    QList<QPair<double, double>> detectVisualSegments(const QString &path, double duration, QString &error, const std::function<void(double)> &onProgress = {});
    bool saveAiCheckpoint(const QStringList &selectedPaths, int currentIndex, const QVariantList &tasks, const QString &mode, double targetDurationSec, const QJsonObject &fingerprints);
    QPair<int, QVariantList> loadAiCheckpoint(const QStringList &selectedPaths, const QString &mode, double targetDurationSec);
    void clearAiCheckpoint();
    QVariantList transcriptSegmentsForClip(const QVariantMap &clip) const;
    QString transcriptTextForClip(const QVariantMap &clip) const;

    QString m_activeDeviceRoot;
    QStringList m_knownFiles;
    QHash<QString, QString> m_seenDevices;
    bool m_isDeviceOnline = true;
    QTimer *m_heartbeatTimer = nullptr;

    std::atomic<bool> m_importRunning{false};
    std::atomic<bool> m_toolCancelled{false};
    std::atomic<bool> m_toolRunning{false};
    std::atomic<bool> m_aiPaused{false};
    std::atomic<bool> m_aiQuiesced{false};
    std::atomic<bool> m_aiCheckpointOk{false};
    std::atomic<bool> m_aiRunning{false};
    std::atomic<bool> m_exportRunning{false};
    std::atomic<bool> m_exportCancelled{false};
    std::atomic<bool> m_transcriptionCancelled{false};
    std::atomic<bool> m_transcriptionRunning{false};
    mutable QMutex m_speechMutex;
    QHash<QString, QVariantList> m_speechAnnotations;
    QHash<QString, QString> m_speechFingerprints;
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_aiCancelled{false};
    QVariantList m_currentAiTasks;
    QProcess *m_exportProcess = nullptr;
    QString m_lutPath;
    QString m_beatMusic;
};
