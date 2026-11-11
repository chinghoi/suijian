#include <QElapsedTimer>
#include <QDebug>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QThread>
#include <QUrl>
#include <memory>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    if (argc != 2) return 1;
    const QString path = QString::fromLocal8Bit(argv[1]);
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) return 1;
    QByteArray qml = source.readAll();
    const QByteArray marker = "    id: root";
    if (!qml.contains(marker)) return 1;
    qml.replace(marker, R"QML(    id: root
    QtObject {
        id: bridge
        property bool busy: false
        property string exportDirectory: "/tmp"
        function loadProject(path) { return {clips: [], active: -1} }
        function saveProject(project, path) { return true }
        function setBeatMusic(path) {}
        function setExportLut(path) {}
        function detectDji() {}
        function cancelCurrentJob() {}
        function loadRecentDirs() {}
        function exportReadiness(clips) { return {} }
    }
    property int layoutTestResult: 0
    property int layoutTestPhase: 0
    Timer {
        running: true; repeat: true; interval: 50
        onTriggered: {
            if (layoutTestPhase === 0 && root.projectReady) {
                layoutTestPhase = 1
                root.width = 1100; root.height = 800
                timelineTools.open()
            } else if (layoutTestPhase === 1 && timelineTools.opened) {
                if (toolsScroll.availableHeight + 1 < toolsBody.implicitHeight) {
                    root.layoutTestResult = -1; stop(); return
                }
                layoutTestPhase = 2
                timelineTools.height = 200
            } else if (layoutTestPhase === 2) {
                root.layoutTestResult = toolsScroll.contentHeight > toolsScroll.availableHeight ? 1 : -2
                stop()
            }
        }
    }
)QML");
    qml.replace("        target: bridge", "        target: bridge\n        ignoreUnknownSignals: true");
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(qml, QUrl::fromLocalFile(path));
    std::unique_ptr<QObject> root(component.create());
    if (!root) { qCritical().noquote() << component.errorString(); return 1; }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 8000 && root->property("layoutTestResult").toInt() == 0) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    const int result = root->property("layoutTestResult").toInt();
    if (result != 1) qCritical() << "Editing tools dialog layout failed:" << result;
    return result == 1 ? 0 : 1;
}
