#include "MediaEngine.h"
#include "VisualScreening.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QXmlStreamReader>
#include <stdexcept>

static void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static QVariantList screen(const QString &path, const QString &mode, bool expectError = false, double budget = 0) {
    MediaEngine engine;
    QEventLoop loop;
    bool completed = false, failed = false;
    QVariantList result;
    QObject::connect(&engine, &MediaEngine::aiFinished, &loop, [&](auto tasks) {completed = true; result = tasks; loop.quit();});
    QObject::connect(&engine, &MediaEngine::aiFailed, &loop, [&](auto error) {qInfo()<<error; failed = true; loop.quit();});
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {engine.cancelCurrentJob();loop.quit();});
    timeout.start(30000);
    engine.startAiScreening(QVariantList{}, QStringList{path}, mode, budget, "horizontal", "8bit");
    loop.exec();
    require(expectError ? failed && !completed : completed && !failed, "unexpected screening outcome");
    return result;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("SuiJianScreeningTest");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temp;
    require(temp.isValid(), "temporary directory failed");
    app.setOrganizationName("SuiJianTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    auto make = [&](QString name, QString video, bool audio) {
        QString path = temp.path()+"/"+name+".mp4";
        QStringList args{"-v","error","-y","-f","lavfi","-i",video};
        if (audio) args << "-f" << "lavfi" << "-i" << "anullsrc=r=16000:cl=mono";
        args << "-t" << "3" << "-c:v" << "mpeg4" << "-q:v" << "2" << path;
        QProcess process; process.start("ffmpeg",args);
        require(process.waitForFinished(30000) && process.exitCode()==0, "fixture generation failed");
        return path;
    };
    const auto detailed = make("detail", "testsrc2=size=160x90:rate=2,trim=end_frame=1,loop=loop=-1:size=1:start=0", false);
    const auto silent = make("silence", "color=black:size=160x90:rate=2", true);
    require(screen(detailed,"2").isEmpty(), "audio-free video retained by audio mode");
    require(screen(silent,"2").isEmpty(), "silence retained by audio mode");
    require(screen(silent,"1").isEmpty(), "black frames retained by visual mode");
    require(!screen(detailed,"1").isEmpty(), "detailed video rejected by visual mode");
    require(!screen(detailed,"3").isEmpty(), "combined mode discarded visual-only content");
    require(screen(silent,"3").isEmpty(), "combined mode retained silent black footage");
    {
        const auto original = temp.filePath("DJI_20261025110327_0099_D.MP4");
        const auto preview = temp.filePath("DJI_20261025110327_0099_D.LRF");
        const auto makeNano = [](const QString &path, const QString &video, const QString &codec, const QString &seconds) {
            QProcess process;
            process.start("ffmpeg", {"-v", "error", "-y", "-f", "lavfi", "-i", video,
                "-f", "lavfi", "-i", "anullsrc=r=16000:cl=mono", "-t", seconds, "-c:v", codec,
                "-c:a", "aac", "-metadata", "creation_time=2026-10-25T03:03:28Z",
                "-metadata", "fixture=DJI Osmo Nano", "-fflags", "+bitexact",
                "-movflags", "+faststart+use_metadata_tags", "-f", "mp4", path});
            require(process.waitForFinished(30000) && process.exitCode() == 0, "Nano fixture generation failed");
            // FFmpeg owns the encoder tag; substitute a same-length metadata key.
            QFile file(path);
            require(file.open(QIODevice::ReadWrite), "cannot tag Nano fixture");
            const auto key = file.readAll().indexOf("mdtafixture");
            require(key >= 0 && file.seek(key) && file.write("mdtaencoder") == 11, "cannot write Nano encoder tag");
        };
        // Contrasting images make the chosen input visible in the result.
        makeNano(original, "color=black:size=320x180:rate=2", "mpeg4", "3");
        const QString picture = "testsrc2=size=160x90:rate=2,trim=end_frame=1,loop=loop=-1:size=1:start=0";
        makeNano(preview, picture, "libx264", "3");
        const auto tasks = screen(original, "3");
        require(!tasks.isEmpty(), "matched Nano preview not screened");
        for (const auto &task : tasks) {
            const auto clip = task.toMap();
            require(clip.value("src").toString() == original && clip.value("end").toDouble() <= 3.0,
                "preview leaked into the timeline or exceeded original duration");
        }
        {
            QFile broken(preview);
            require(broken.open(QIODevice::ReadWrite) && broken.resize(broken.size() * 2 / 3),
                "cannot truncate Nano preview fixture");
        }
        require(screen(original, "3").isEmpty(), "truncated preview should fall back to the original");
        makeNano(preview, picture, "libx264", "2");
        require(screen(original, "3").isEmpty(), "mismatched preview should fall back to the original");
    }
    {
        // Quiet activity inside a good image should be protected as a unit.
        // Louder activity against black should not make that footage eligible.
        const QString mixed = temp.filePath("mixed.mp4");
        QProcess fixture;
        fixture.start("ffmpeg", QStringList{"-v", "error", "-y", "-f", "lavfi", "-i",
            "testsrc2=size=160x90:rate=2,trim=end_frame=1,loop=loop=-1:size=1:start=0,drawbox=color=black:t=fill:enable='gte(t,8)'",
            "-f", "lavfi", "-i", "aevalsrc=0.12*sin(2*PI*40*t)+(0.018*between(t\\,1\\,2)+0.2*between(t\\,9\\,10))*sin(2*PI*440*t):s=16000",
            "-t", "12", "-c:v", "mpeg4", "-q:v", "2", "-c:a", "aac", mixed});
        require(fixture.waitForFinished(30000) && fixture.exitCode() == 0, "mixed fixture generation failed");
        const auto checkCombined = [](const QString &path) {
            QVariantMap dialogue;
            double total = 0;
            for (const auto &task : screen(path, "3", false, 3.2)) {
                const auto clip = task.toMap();
                require(clip.value("end").toDouble() <= 8.0, "sound made black footage eligible");
                total += clip.value("dur").toDouble();
                if (clip.value("protected_dialogue").toBool()) dialogue = clip;
            }
            require(total <= 3.20001, "combined scan exceeded the duration budget");
            require(!dialogue.isEmpty() && dialogue.value("start").toDouble() < 0.4
                && dialogue.value("end").toDouble() > 2.8, "quiet audio or its padding was cut");
        };
        checkCombined(mixed);

        const QString rotated = temp.filePath("rotated.mp4");
        fixture.start("ffmpeg", QStringList{"-v", "error", "-y", "-i", mixed,
            "-c", "copy", "-metadata:s:v:0", "rotate=90", rotated});
        require(fixture.waitForFinished(30000) && fixture.exitCode() == 0, "rotated fixture generation failed");
        checkCombined(rotated);
#ifdef Q_OS_UNIX
        // Pace FFmpeg so pause/resume/cancel exercise an active dual-stream
        // process, rather than racing a fixture that finishes in milliseconds.
        const QByteArray originalPath = qgetenv("PATH");
        QString ffmpegPath = QStandardPaths::findExecutable("ffmpeg");
        ffmpegPath.replace("'", "'\\''");
        QDir().mkpath(temp.filePath("paced"));
        QFile paced(temp.filePath("paced/ffmpeg"));
        require(paced.open(QIODevice::WriteOnly), "cannot write paced FFmpeg wrapper");
        paced.write(("#!/bin/sh\nfor arg do\n case \"$arg\" in *lavfi.astats.Overall.RMS_level*)\n"
                     "  exec '" + ffmpegPath + "' -readrate 1 \"$@\";;\n esac\ndone\nexec '" +
                     ffmpegPath + "' \"$@\"\n").toUtf8());
        paced.close();
        require(paced.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
                "cannot make paced FFmpeg executable");
        qputenv("PATH", temp.filePath("paced").toUtf8() + ":" + originalPath);
        MediaEngine controlled;
        QEventLoop controlLoop;
        bool pauseRequested = false, paused = false, cancelled = false, completed = false;
        QObject::connect(&controlled, &MediaEngine::aiProgress, &controlLoop, [&](double, const QString &label) {
            if (!label.contains("检查画面与声音")) return;
            if (!pauseRequested) { pauseRequested = true; controlled.pauseAiScreening(); }
            else if (paused) controlled.cancelAi();
        });
        QObject::connect(&controlled, &MediaEngine::aiPaused, &controlLoop, [&](int, int, const QString &) {
            paused = true;
            controlled.resumeAiScreening();
        });
        QObject::connect(&controlled, &MediaEngine::aiCancelled, &controlLoop, [&] {
            cancelled = true; controlLoop.quit();
        });
        QObject::connect(&controlled, &MediaEngine::aiFinished, &controlLoop, [&] {
            completed = true; controlLoop.quit();
        });
        QObject::connect(&controlled, &MediaEngine::aiFailed, &controlLoop, &QEventLoop::quit);
        QTimer controlTimeout;
        controlTimeout.setSingleShot(true);
        QObject::connect(&controlTimeout, &QTimer::timeout, &controlLoop, [&] {
            controlled.cancelCurrentJob(); controlLoop.quit();
        });
        controlTimeout.start(15000);
        controlled.startAiScreening({}, {detailed, mixed}, "3", 0, "horizontal", "8bit");
        controlLoop.exec();
        qputenv("PATH", originalPath);
        require(paused && cancelled && !completed && !controlled.busy(),
                "combined scan did not pause, resume and cancel cleanly");
        {
            MediaEngine resumed;
            QEventLoop loop;
            bool resumedSecond = false, finished = false;
            QObject::connect(&resumed, &MediaEngine::aiProgress, &loop, [&](double, const QString &label) {
                if (label.contains("检查点") && label.contains("[2/2]")) resumedSecond = true;
            });
            QObject::connect(&resumed, &MediaEngine::aiFinished, &loop, [&] { finished = true; loop.quit(); });
            QObject::connect(&resumed, &MediaEngine::aiFailed, &loop, &QEventLoop::quit);
            QTimer::singleShot(15000, &loop, &QEventLoop::quit);
            resumed.startAiScreening({}, {detailed, mixed}, "3", 4.0, "horizontal", "8bit");
            loop.exec();
            require(resumedSecond && finished, "changing the budget discarded completed analysis");
        }
#endif
    }
    {
        MediaEngine budgetEngine;
        QEventLoop loop;
        QVariantList result;
        bool finished = false;
        QObject::connect(&budgetEngine, &MediaEngine::aiFinished, &loop, [&](const QVariantList &tasks) {
            result = tasks; finished = true; loop.quit();
        });
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        const QString other = temp.filePath("other.mp4");
        require(QFile::copy(detailed, other), "could not create second video");
        budgetEngine.startAiScreening({}, {detailed, other}, "1", 3.0, "horizontal", "8bit");
        loop.exec();
        require(finished && result.size() == 2, "duration budget excluded a later video");
        require(result[0].toMap().value("src").toString() == detailed &&
                result[1].toMap().value("src").toString() == other,
                "duration budget changed source order");
    }
    const auto broken = temp.path()+"/broken.mp4"; QFile file(broken); file.open(QIODevice::WriteOnly); file.write("invalid"); file.close();
    screen(broken,"2",true);

    MediaEngine engine;
    bool emptySelectionRejected = false;
    QObject::connect(&engine, &MediaEngine::aiFailed, &engine, [&](const QString &message) {
        if (message.contains("选择")) emptySelectionRejected = true;
    });
    engine.startAiScreening({QVariantMap{{"path", detailed}}}, {}, "1", 0, "horizontal", "8bit");
    require(emptySelectionRejected && !engine.busy(), "empty selection silently screened the whole pool");
    const QString cardDir = temp.filePath("CARD/DCIM/100MEDIA");
    require(QDir().mkpath(cardDir), "failed to create nested camera directory");
    require(QFile::copy(detailed, cardDir + "/nested.mp4"), "failed to create nested video");
    QFile proxy(cardDir + "/nested.LRV");
    require(proxy.open(QIODevice::WriteOnly), "failed to create proxy fixture");
    proxy.write("proxy");
    proxy.close();
    QVariantList imported;
    QStringList recent;
    QEventLoop importLoop;
    QObject::connect(&engine, &MediaEngine::mediaLoaded, &importLoop,
        [&](const QStringList &, const QVariantList &items, const QString &) { imported = items; importLoop.quit(); });
    QObject::connect(&engine, &MediaEngine::recentDirsLoaded, &importLoop,
        [&](const QStringList &paths) { recent = paths; });
    QTimer::singleShot(30000, &importLoop, &QEventLoop::quit);
    const QString dcim = temp.filePath("CARD/DCIM");
    engine.importPaths({dcim});
    importLoop.exec();
    require(imported.size() == 1 && imported.first().toMap().value("filename").toString() == "nested.mp4",
            "nested camera video missing or proxy imported");
    require(recent.contains(dcim), "successful import did not add recent directory");
    recent.clear();
    engine.loadRecentDirs();
    require(recent.contains(dcim), "recent directory was not persisted");
    const QString djiPath = temp.filePath("DJI_20261025110327_0001_D.MP4");
    require(QFile::copy(detailed, djiPath), "failed to create DJI filename fixture");
    QEventLoop dateLoop;
    QVariantList djiImported;
    QObject::connect(&engine, &MediaEngine::mediaLoaded, &dateLoop,
        [&](const QStringList &, const QVariantList &items, const QString &) { djiImported = items; dateLoop.quit(); });
    QTimer::singleShot(30000, &dateLoop, &QEventLoop::quit);
    engine.importPaths({djiPath});
    dateLoop.exec();
    require(djiImported.size() == 1 && djiImported[0].toMap().value("date").toString() == "2026-10-25"
        && djiImported[0].toMap().value("time").toString() == "11:03:27",
        "DJI capture date was replaced by file modification date");
    QVariantMap firstClip{
        {"src", detailed}, {"filename", "immich_clip.mp4"}, {"start", 1.25}, {"end", 2.25},
        {"dur", 1.0}, {"media_dur", 3.0}, {"text", ""}, {"subtitles", QVariantList{}}
    };
    QVariantMap secondClip{
        {"src", silent}, {"filename", "coast.mp4"}, {"start", 0.0}, {"end", 3.0},
        {"dur", 3.0}, {"media_dur", 3.0}, {"text", "海边日落"}, {"subtitles", QVariantList{}}
    };
    const QVariantList timeline{firstClip, secondClip};
    QString lastStatus;
    QObject::connect(&engine, &MediaEngine::statusMessage, [&](const QString &message) { lastStatus = message; });
    require(engine.roughCutByWords(timeline, "im").size() == timeline.size()
                && lastStatus.contains("没有镜头"),
            "short Latin substring falsely matched a filename");
    const QVariantList filenameMatch = engine.roughCutByWords(timeline, "immich");
    require(filenameMatch.size() == 1
                && filenameMatch.first().toMap().value("filename").toString() == QStringLiteral("immich_clip.mp4"),
            "whole filename term did not match");
    const QVariantList transcriptMatch = engine.roughCutByWords(timeline, "海边");
    require(transcriptMatch.size() == 1
                && transcriptMatch.first().toMap().value("filename").toString() == QStringLiteral("coast.mp4"),
            "Chinese transcript term did not match");

    const QString srtPath = temp.path()+"/captions.srt";
    QVariantMap captionedClip = firstClip;
    captionedClip["text"] = "A quote, and\nline";
    const QString destination = temp.filePath("Export results");
    require(QDir().mkpath(destination), "cannot create export fixture directory");
    require(engine.setExportDirectory(QUrl::fromLocalFile(destination)), "export directory selection failed");
    require(MediaEngine().exportDirectory() == destination, "export directory preference was not restored");
    require(!engine.setExportDirectory(QUrl::fromLocalFile(temp.filePath("missing directory"))),
            "missing directory accepted as export destination");
    const auto readiness = engine.exportReadiness({captionedClip});
    require(readiness.value("hasSubtitles").toBool() && readiness.value("mediaError").toString().isEmpty()
        && !readiness.value("directoryUnavailable").toBool(), "valid export incorrectly blocked");
    require(!engine.exportReadiness({firstClip}).value("hasSubtitles").toBool(), "missing subtitles not detected");
    QVariantMap offlineClip = captionedClip;
    offlineClip["src"] = temp.filePath("offline.mp4");
    const auto offline = engine.exportReadiness({offlineClip});
    require(!offline.value("mediaError").toString().isEmpty() && offline.value("hasSubtitles").toBool(),
            "offline source did not preserve existing subtitles");
    engine.exportMetadata({}, "csv", "");
    require(QDir(destination).entryList(QDir::Files).isEmpty(), "empty timeline wrote metadata");
    engine.exportMetadata({offlineClip}, "csv", "");
    engine.generateSrt({offlineClip}, "");
    require(QDir(destination).entryList(QStringList{"*.csv"}, QDir::Files).size() == 1
        && QDir(destination).entryList(QStringList{"*.srt"}, QDir::Files).size() == 1,
        "metadata or subtitle export ignored selected directory");
    engine.generateSrt({captionedClip}, srtPath);
    QFile srt(srtPath);
    require(srt.open(QIODevice::ReadOnly), "SRT with transcript was not created");
    const QString srtContent = QString::fromUtf8(srt.readAll());
    require(srtContent.contains("00:00:00,000 --> 00:00:01,000")
                && srtContent.contains("A quote, and\nline"),
            "SRT subtitle timing or text is wrong");
    const QString emptySrtPath = temp.path()+"/empty.srt";
    engine.generateSrt({firstClip}, emptySrtPath);
    require(!QFile::exists(emptySrtPath), "empty SRT file was created without transcript");
    QVariantMap blankAudioClip = firstClip;
    blankAudioClip["subtitles"] = QVariantList{QVariantMap{{"start", 0.0}, {"end", 1.0}, {"text", "[BLANK_AUDIO]"}}};
    require(!engine.exportReadiness({blankAudioClip}).value("hasSubtitles").toBool(), "blank-audio placeholder counted as a subtitle");
    engine.generateSrt({blankAudioClip}, emptySrtPath);
    require(!QFile::exists(emptySrtPath), "blank-audio placeholder exported as a subtitle");

    const QString jsonPath = temp.path()+"/metadata.json";
    engine.exportMetadata({captionedClip}, "json", jsonPath);
    QFile jsonFile(jsonPath);
    require(jsonFile.open(QIODevice::ReadOnly), "metadata JSON was not created");
    const auto json = QJsonDocument::fromJson(jsonFile.readAll());
    require(json.isObject()
                && json.object().value("clips").toArray().first().toObject()
                       .value("transcript_text").toString() == "A quote, and line",
            "metadata JSON omitted the transcript");

    const QString csvPath = temp.path()+"/metadata.csv";
    engine.exportMetadata({captionedClip}, "csv", csvPath);
    QFile csvFile(csvPath);
    require(csvFile.open(QIODevice::ReadOnly), "metadata CSV was not created");
    const QByteArray csv = csvFile.readAll();
    require(csv.contains("\"A quote, and line\"") && csv.contains("\"immich_clip.mp4\""),
            "metadata CSV did not quote transcript fields safely");
    const QString projectPath = temp.path()+"/roundtrip.suijian";
    engine.setBeatMusic(engine.builtinBeatTrack());
    captionedClip["transpose"] = 1;
    require(engine.saveProject({{"clips", QVariantList{captionedClip}}, {"beatMusic", engine.beatMusic()}, {"canvas", "vertical"}}, projectPath), "project save failed");
    const auto restored = engine.loadProject(projectPath);
    require(restored.value("clips").toList().first().toMap().value("transpose").toInt() == 1
        && restored.value("beatMusic").toString() == engine.beatMusic(), "project roundtrip lost editing state");
    const QString xmlPath = temp.path()+"/timeline.fcpxml";
    engine.exportFcpxml({captionedClip}, "vertical", xmlPath);
    QFile xmlFile(xmlPath); require(xmlFile.open(QIODevice::ReadOnly), "FCPXML missing");
    const QByteArray xml = xmlFile.readAll();
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) reader.readNext();
    require(!reader.hasError() && xml.contains("rotation=\"-90\"") && xml.contains("ref=\"music\""), "FCPXML lost rotation or music");
    require(QFile::exists(xmlPath+".suijian") && QFile::exists(xmlPath+".srt"), "editable project sidecars missing");
    const QString draftDir = temp.path()+"/draft";
    engine.exportJianying({captionedClip}, "vertical", draftDir);
    QFile draft(draftDir+"/draft_content.json"); require(draft.open(QIODevice::ReadOnly), "draft missing");
    const auto draftJson = QJsonDocument::fromJson(draft.readAll()).object();
    require(draftJson.value("materials").toObject().value("audios").toArray().size() == 1, "draft lost music");
    require(draftJson.value("tracks").toArray().first().toObject().value("segments").toArray().first().toObject().value("clip").toObject().value("rotation").toDouble() == 90, "draft lost rotation");

    QEventLoop cancellation;
    bool stopped = false;
    QObject::connect(&engine, &MediaEngine::aiCancelled, &cancellation, [&] { stopped = true; cancellation.quit(); });
    QTimer cancelTimeout; cancelTimeout.setSingleShot(true);
    QObject::connect(&cancelTimeout, &QTimer::timeout, &cancellation, &QEventLoop::quit);
    engine.startAiScreening({}, {detailed}, "1", 0, "horizontal", "8bit");
    require(engine.busy(), "job did not enter busy state");
    engine.cancelAi();
    cancelTimeout.start(10000); cancellation.exec();
    require(stopped && !engine.busy(), "AI cancellation did not reach stopped state");
    qInfo()<<"Screening integration checks passed";
}
