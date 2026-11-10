#include "MediaEngine.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QDebug>
#include <QFile>
#include <stdexcept>

static void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static QVariantList screen(const QString &path, const QString &mode, bool expectError = false) {
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
    qInfo()<<"Screening integration checks passed";
}
