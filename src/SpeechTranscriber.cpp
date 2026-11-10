#include "SpeechTranscriber.h"

#include <whisper.h>

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {
constexpr qint64 kModelSize = 77691713;
constexpr char kModelSha256[] = "be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21";
const QUrl kModelUrl(QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin?download=true"));
constexpr int kSampleRate = 16000;
constexpr int kChunkSeconds = 30;
constexpr int kChunkSamples = kSampleRate * kChunkSeconds;

QString sha256File(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

struct WhisperProgress {
    const SpeechTranscriber::StopRequested *cancelled = nullptr;
    SpeechTranscriber::ProgressCallback progress;
    int chunkIndex = 0;
    int chunkCount = 1;
    QString filename;
};

bool whisperAbort(void *userData) {
    const auto *state = static_cast<const WhisperProgress *>(userData);
    return (*state->cancelled)();
}

void whisperProgress(whisper_context *, whisper_state *, int percent, void *userData) {
    auto *state = static_cast<WhisperProgress *>(userData);
    const double currentChunk = std::clamp(percent / 100.0, 0.0, 1.0);
    const double overall = 50.0 + 50.0 * (state->chunkIndex + currentChunk) / state->chunkCount;
    if (state->progress) {
        state->progress(overall, QString("识别语音：%1 · 第 %2/%3 段")
            .arg(state->filename).arg(state->chunkIndex + 1).arg(state->chunkCount));
    }
}

QString ffmpegTimestampMicros(const QByteArray &line) {
    const auto separator = line.indexOf('=');
    if (separator < 0) return {};
    return QString::fromLatin1(line.mid(separator + 1)).trimmed();
}
}

QString SpeechTranscriber::cachedModelPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/models/ggml-tiny.bin");
}

SpeechTranscriber::~SpeechTranscriber() {
    if (m_context) whisper_free(m_context);
}

bool SpeechTranscriber::prepareModel(const StopRequested &cancelled,
                                     const ProgressCallback &progress,
                                     QString *error) {
    if (cancelled()) { if (error) *error = QStringLiteral("转写已取消"); return false; }
    const QString path = cachedModelPath();
    const QFileInfo existing(path);
    if (existing.exists() && existing.size() == kModelSize
        && sha256File(path) == QString::fromLatin1(kModelSha256)) {
        return true;
    }
    if (cancelled()) {
        if (error) *error = QStringLiteral("转写已取消");
        return false;
    }

    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("无法创建语音模型缓存目录");
        return false;
    }

    QSaveFile modelFile(path);
    if (!modelFile.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法创建语音模型缓存文件");
        return false;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(kModelUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = manager.get(request);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QString writeError;
    bool timedOut = false;
    QTimer timeoutWatch;
    timeoutWatch.setSingleShot(true);
    QObject::connect(&timeoutWatch, &QTimer::timeout, reply, [&]() {
        timedOut = true;
        if (reply->isRunning()) reply->abort();
    });

    QObject::connect(reply, &QIODevice::readyRead, reply, [&]() {
        const QByteArray block = reply->readAll();
        if (block.isEmpty()) return;
        timeoutWatch.start(120000);
        if (modelFile.write(block) != block.size()) {
            writeError = QStringLiteral("语音模型写入失败");
            reply->abort();
            return;
        }
        hash.addData(block);
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
        [&](qint64 received, qint64 total) {
            if (progress) {
                const double percent = total > 0
                    ? std::clamp(100.0 * received / total, 0.0, 99.0) : 0.0;
                const QString amount = total > 0
                    ? QString("%1 / %2 MB").arg(received / (1024 * 1024)).arg(total / (1024 * 1024))
                    : QString("%1 MB 已下载").arg(received / (1024 * 1024));
                progress(percent, QString("下载语音模型：%1").arg(amount));
            }
        });

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeoutWatch.start(120000);
    QTimer cancelWatch;
    cancelWatch.setInterval(100);
    QObject::connect(&cancelWatch, &QTimer::timeout, reply, [&]() {
        if (cancelled() && reply->isRunning()) reply->abort();
    });
    cancelWatch.start();
    if (!reply->isFinished()) loop.exec();
    cancelWatch.stop();
    timeoutWatch.stop();

    const QByteArray trailing = reply->readAll();
    if (!trailing.isEmpty()) {
        if (modelFile.write(trailing) != trailing.size()) writeError = QStringLiteral("语音模型写入失败");
        hash.addData(trailing);
    }

    const auto networkError = reply->error();
    const QString networkMessage = reply->errorString();
    delete reply;

    if (cancelled()) {
        modelFile.cancelWriting();
        if (error) *error = QStringLiteral("转写已取消");
        return false;
    }
    if (!writeError.isEmpty()) {
        modelFile.cancelWriting();
        if (error) *error = writeError;
        return false;
    }
    if (timedOut) {
        modelFile.cancelWriting();
        if (error) *error = QStringLiteral("语音模型下载超时，请检查网络后重试");
        return false;
    }
    if (networkError != QNetworkReply::NoError) {
        modelFile.cancelWriting();
        if (error) *error = QStringLiteral("语音模型下载失败：%1").arg(networkMessage);
        return false;
    }
    if (hash.result().toHex() != QByteArray(kModelSha256)) {
        modelFile.cancelWriting();
        if (error) *error = QStringLiteral("语音模型校验失败，未启用该文件");
        return false;
    }
    if (!modelFile.commit()) {
        if (error) *error = QStringLiteral("无法保存语音模型缓存");
        return false;
    }
    if (progress) progress(100.0, QStringLiteral("语音模型已准备好"));
    return true;
}

bool SpeechTranscriber::transcribeFile(const QString &videoPath,
                                       double durationSeconds,
                                       const QString &modelPath,
                                       const StopRequested &cancelled,
                                       const ProgressCallback &progress,
                                       QVariantList *segments,
                                       QString *error) {
    if (cancelled()) {
        if (error) *error = QStringLiteral("转写已取消");
        return false;
    }

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        if (error) *error = QStringLiteral("无法创建音频临时目录");
        return false;
    }
    const QString pcmPath = tempDir.filePath(QStringLiteral("speech.f32le"));
    QProcess ffmpeg;
    ffmpeg.setProcessChannelMode(QProcess::SeparateChannels);
    ffmpeg.start(QStringLiteral("ffmpeg"), {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-nostats"), QStringLiteral("-y"), QStringLiteral("-i"), videoPath,
        QStringLiteral("-map"), QStringLiteral("0:a:0"), QStringLiteral("-vn"),
        QStringLiteral("-ac"), QStringLiteral("1"), QStringLiteral("-ar"), QString::number(kSampleRate),
        QStringLiteral("-f"), QStringLiteral("f32le"), QStringLiteral("-progress"), QStringLiteral("pipe:1"),
        pcmPath
    });
    if (!ffmpeg.waitForStarted()) {
        if (error) *error = QStringLiteral("无法启动 FFmpeg 音频解码器");
        return false;
    }

    QByteArray progressBuffer;
    QByteArray errorBuffer;
    while (ffmpeg.state() != QProcess::NotRunning) {
        if (cancelled()) {
            ffmpeg.kill();
            ffmpeg.waitForFinished();
            if (error) *error = QStringLiteral("转写已取消");
            return false;
        }
        ffmpeg.waitForReadyRead(150);
        progressBuffer += ffmpeg.readAllStandardOutput();
        errorBuffer += ffmpeg.readAllStandardError();
        if (errorBuffer.size() > 4096) errorBuffer.remove(0, errorBuffer.size() - 4096);
        int newline = -1;
        while ((newline = progressBuffer.indexOf('\n')) >= 0) {
            const QByteArray line = progressBuffer.left(newline).trimmed();
            progressBuffer.remove(0, newline + 1);
            if (line.startsWith("out_time_ms=") && durationSeconds > 0 && progress) {
                bool ok = false;
                const double micros = ffmpegTimestampMicros(line).toDouble(&ok);
                if (ok) {
                    const double percent = std::clamp(50.0 * micros / (durationSeconds * 1000000.0), 0.0, 49.0);
                    progress(percent, QStringLiteral("正在提取音轨：%1").arg(QFileInfo(videoPath).fileName()));
                }
            }
        }
    }
    ffmpeg.waitForFinished();
    errorBuffer += ffmpeg.readAllStandardError();
    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
        const QString details = QString::fromUtf8(errorBuffer).trimmed().right(500);
        if (error) *error = details.isEmpty()
            ? QStringLiteral("素材没有可读取的音轨")
            : QStringLiteral("音轨读取失败：%1").arg(details);
        return false;
    }

    QFile pcmFile(pcmPath);
    if (!pcmFile.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取解码后的音轨");
        return false;
    }
    const qint64 totalSamples = pcmFile.size() / static_cast<qint64>(sizeof(float));
    if (totalSamples < kSampleRate / 2) {
        if (error) *error = QStringLiteral("音轨太短，无法识别语音");
        return false;
    }

    if (!m_context) {
        if (progress) progress(50.0, QStringLiteral("正在载入语音识别模型"));
        whisper_context_params contextParams = whisper_context_default_params();
        m_context = whisper_init_from_file_with_params(modelPath.toUtf8().constData(), contextParams);
        if (!m_context) {
            if (error) *error = QStringLiteral("语音识别模型无法载入");
            return false;
        }
    }

    const int chunkCount = static_cast<int>((totalSamples + kChunkSamples - 1) / kChunkSamples);
    const QString filename = QFileInfo(videoPath).fileName();
    for (int chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        if (cancelled()) {
            if (error) *error = QStringLiteral("转写已取消");
            return false;
        }
        const qint64 sampleCount = std::min<qint64>(kChunkSamples, totalSamples - static_cast<qint64>(chunkIndex) * kChunkSamples);
        const QByteArray raw = pcmFile.read(sampleCount * static_cast<qint64>(sizeof(float)));
        if (raw.size() != sampleCount * static_cast<qint64>(sizeof(float))) {
            if (error) *error = QStringLiteral("解码后的音轨数据不完整");
            return false;
        }
        std::vector<float> samples(static_cast<size_t>(sampleCount));
        std::memcpy(samples.data(), raw.constData(), static_cast<size_t>(raw.size()));

        WhisperProgress callbackState{&cancelled, progress, chunkIndex, chunkCount, filename};
        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.n_threads = std::clamp(QThread::idealThreadCount(), 1, 8);
        params.translate = false;
        params.no_context = true;
        params.print_realtime = false;
        params.print_progress = false;
        params.print_timestamps = false;
        params.print_special = false;
        params.language = "auto";
        params.detect_language = false;
        params.progress_callback = whisperProgress;
        params.progress_callback_user_data = &callbackState;
        params.abort_callback = whisperAbort;
        params.abort_callback_user_data = &callbackState;

        if (whisper_full(m_context, params, samples.data(), samples.size()) != 0) {
            if (error) *error = cancelled() ? QStringLiteral("转写已取消") : QStringLiteral("语音识别未能完成");
            return false;
        }

        const double chunkOffset = static_cast<double>(chunkIndex * kChunkSeconds);
        for (int i = 0; i < whisper_full_n_segments(m_context); ++i) {
            const char *rawText = whisper_full_get_segment_text(m_context, i);
            const QString text = QString::fromUtf8(rawText ? rawText : "").simplified();
            if (text.isEmpty()) continue;
            const double start = chunkOffset + whisper_full_get_segment_t0(m_context, i) * 0.01;
            const double end = chunkOffset + whisper_full_get_segment_t1(m_context, i) * 0.01;
            if (end <= start) continue;
            QVariantMap segment;
            segment["start"] = start;
            segment["end"] = end;
            segment["text"] = text;
            segments->append(segment);
        }
    }
    return true;
}
