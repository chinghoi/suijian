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
    void pauseAiScreening();
    void resumeAiScreening();
    void cancelCurrentJob();
    void safeEjectDevice();
    void startExport(const QString &outputPath, const QString &format, int width, int height, int fps);

signals:
    void mediaLoaded(const QStringList &dates, const QVariantList &mediaList, const QString &sourceLabel);
    void djiFound(const QString &path);
    void recentDirsLoaded(const QStringList &dirs);
    void statusMessage(const QString &message);
    void scanError(const QString &errorMessage);
    void aiProgress(double percent, const QString &stepText);
    void aiFinished(const QVariantList &tasks);
    void aiPaused(int completed, int total, const QString &reason);
    void aiResumed();
    void deviceDisconnected(const QString &deviceName);
    void deviceReconnected(const QString &deviceName, const QVariantMap &diff);
    void exportProgress(double percent, const QString &stepText);
    void exportFinished(const QString &outputPath);

private:
    void checkDeviceHeartbeat();
    QVariantMap probeFile(const QString &filepath);

    QString m_activeDeviceRoot;
    QStringList m_knownFiles;
    bool m_isDeviceOnline = true;
    QTimer *m_heartbeatTimer = nullptr;

    std::atomic<bool> m_aiPaused{false};
    std::atomic<bool> m_aiCancelled{false};
    QVariantList m_currentAiTasks;
    QProcess *m_exportProcess = nullptr;
};
