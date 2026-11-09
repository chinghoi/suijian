#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QThread>
#include <QProcess>
#include <QTimer>
#include <atomic>

class MediaEngine : public QObject {
    Q_OBJECT

public:
    explicit MediaEngine(QObject *parent = nullptr);
    virtual ~MediaEngine();

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
    void safeEjectDevice();
    void startExport(const QString &outputPath, const QString &format, int width, int height, int fps);
    void startExport(const QVariantList &clips, const QString &masterCanvas, const QString &targetColor);
    void generateSrt(const QVariantList &clips, const QString &outputPath);
    void exportMetadata(const QVariantList &clips, const QString &formatType, const QString &outputPath);
    void exportFcpxml(const QVariantList &clips, const QString &masterCanvas, const QString &outputPath);

signals:
    void mediaLoaded(const QStringList &dates, const QVariantList &mediaList, const QString &sourceLabel);
    void importProgress(int current, int total, const QString &filename);
    void djiFound(const QString &path);
    void recentDirsLoaded(const QStringList &dirs);
    void statusMessage(const QString &message);
    void scanError(const QString &errorMessage);
    void aiProgress(double percent, const QString &stepText);
    void aiFinished(const QVariantList &tasks);
    void aiPaused(int completed, int total, const QString &reason);
    void aiResumed();
    void speechSegmentsReady(const QString &videoPath, const QVariantList &segments);
    void deviceDisconnected(const QString &deviceName);
    void deviceReconnected(const QString &deviceName, const QVariantMap &diff);
    void usbDeviceDetected(const QString &deviceName, const QString &mediaDir, int fileCount);
    void usbDeviceRemoved(const QString &deviceName);
    void exportProgress(double percent, const QString &stepText);
    void exportFinished(const QString &outputPath);
    void exportFailed(const QString &errorMessage);

private:
    void checkDeviceHeartbeat();
    QVariantMap probeFile(const QString &filepath);
    QString extractAudioWav(const QString &videoPath);
    QList<QPair<double, double>> detectVoiceSegments(const QString &audioOrVideoPath, double noiseDb = -30.0, double minSilenceDur = 0.6, double minSpeechDur = 0.3);
    void saveAiCheckpoint(const QStringList &selectedPaths, int currentIndex, const QVariantList &tasks);
    QPair<int, QVariantList> loadAiCheckpoint(const QStringList &selectedPaths);
    void clearAiCheckpoint();

    QString m_activeDeviceRoot;
    QStringList m_knownFiles;
    bool m_isDeviceOnline = true;
    QTimer *m_heartbeatTimer = nullptr;

    std::atomic<bool> m_aiPaused{false};
    std::atomic<bool> m_aiCancelled{false};
    QVariantList m_currentAiTasks;
    QProcess *m_exportProcess = nullptr;
};
