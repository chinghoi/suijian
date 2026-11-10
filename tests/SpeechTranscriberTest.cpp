#include "SpeechTranscriber.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDebug>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("SuiJianSpeechTest");
    QStandardPaths::setTestModeEnabled(true);
    const QString audio = qEnvironmentVariable("SUIJIAN_ASR_TEST_AUDIO");
    if (audio.isEmpty()) { qInfo() << "Set SUIJIAN_ASR_TEST_AUDIO to a speech fixture to run ASR smoke test"; return 77; }
    if (!QFileInfo::exists(audio)) return 1;
    QString error;
    const auto running = [] { return false; };
    if (!SpeechTranscriber::prepareModel(running, {}, &error)) { qCritical() << error; return 1; }
    SpeechTranscriber transcriber;
    QVariantList segments;
    if (!transcriber.transcribeFile(audio, 15, SpeechTranscriber::cachedModelPath(), running, {}, &segments, &error)
        || segments.isEmpty()) { qCritical() << "Speech produced no transcript:" << error; return 1; }
    for (const QVariant &item : segments) {
        const auto segment = item.toMap();
        if (segment.value("text").toString().trimmed().isEmpty()
            || segment.value("end").toDouble() <= segment.value("start").toDouble()) return 1;
    }
    if (transcriber.transcribeFile(audio, 15, SpeechTranscriber::cachedModelPath(), [] { return true; }, {}, &segments, &error)) return 1;
    qInfo() << "ASR transcription and cancellation checks passed";
}
