#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <functional>

struct whisper_context;

class SpeechTranscriber {
public:
    using StopRequested = std::function<bool()>;
    using ProgressCallback = std::function<void(double, const QString &)>;

    SpeechTranscriber() = default;
    ~SpeechTranscriber();
    SpeechTranscriber(const SpeechTranscriber &) = delete;
    SpeechTranscriber &operator=(const SpeechTranscriber &) = delete;

    static QString cachedModelPath();
    static bool prepareModel(const StopRequested &cancelled,
                             const ProgressCallback &progress,
                             QString *error = nullptr);
    bool transcribeFile(const QString &videoPath,
                        double durationSeconds,
                        const QString &modelPath,
                        const StopRequested &cancelled,
                        const ProgressCallback &progress,
                        QVariantList *segments,
                        QString *error = nullptr);

private:
    whisper_context *m_context = nullptr;
};
