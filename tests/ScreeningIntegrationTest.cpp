#include "MediaEngine.h"
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
static QVariantList screen(const QString &path, const QString &mode, bool expectError = false) {
    MediaEngine engine;
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
    QEventLoop loop;
    bool completed = false, failed = false;
    QVariantList result;
    QObject::connect(&engine, &MediaEngine::aiFinished, &loop, [&](auto tasks) {completed = true; result = tasks; loop.quit();});
    QObject::connect(&engine, &MediaEngine::aiFailed, &loop, [&](auto error) {qInfo()<<error; failed = true; loop.quit();});
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {engine.cancelCurrentJob();loop.quit();});
    timeout.start(30000);
    engine.startAiScreening(QVariantList{}, QStringList{path}, mode, 0, "horizontal", "8bit");
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
    const auto broken = temp.path()+"/broken.mp4"; QFile file(broken); file.open(QIODevice::WriteOnly); file.write("invalid"); file.close();
    screen(broken,"2",true);

    MediaEngine engine;
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
