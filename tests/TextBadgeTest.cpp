#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThread>

#include <functional>
#include <memory>

static bool waitFor(const std::function<bool()> &condition) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        if (condition()) return true;
        QThread::msleep(2);
    } while (timer.elapsed() < 2000);
    return false;
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (argc != 2) return 1;

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
import QtQuick
import QtQuick.Layouts
Item {
    width: 400
    height: 42
    RowLayout {
        anchors.fill: parent
        TextBadge {
            objectName: "badge"
            horizontalPadding: 12
            showDot: true
            text: "已暂停"
            Layout.fillWidth: true
            Layout.maximumWidth: implicitWidth
        }
        Item { Layout.fillWidth: true }
        Text { text: "100%" }
    }
}
)", QUrl::fromLocalFile(QDir(QString::fromLocal8Bit(argv[1])).filePath("BadgeHarness.qml")));
    QQuickWindow window;
    std::unique_ptr<QObject> holder(component.create());
    auto *root = qobject_cast<QQuickItem *>(holder.get());
    if (!root) { qCritical().noquote() << component.errorString(); return 1; }
    auto *badge = root->findChild<QQuickItem *>("badge");
    if (!badge) return 1;

    window.resize(420, 80);
    root->setParentItem(window.contentItem());
    window.show();

    QQuickItem *caption = nullptr;
    for (auto *child : badge->childItems()) {
        if (child->property("text").isValid()) caption = child;
    }
    if (!caption) return 1;
    const auto fits = [&] {
        return caption->width() > 0 && caption->x() + caption->width() <= badge->width() + 0.01;
    };
    if (!waitFor([&] { return fits() && badge->width() >= badge->implicitWidth() - 1; })) {
        qCritical() << "Short caption did not fit";
        return 1;
    }
    const qreal shortWidth = badge->width();
    badge->setProperty("text", QStringLiteral("初筛全速进行中"));
    if (!waitFor([&] { return fits() && badge->width() > shortWidth && !caption->property("truncated").toBool(); })) {
        qCritical() << "Badge failed to expand for longer status";
        return 1;
    }
    root->setWidth(95);
    if (!waitFor([&] { return fits() && caption->property("truncated").toBool(); })) {
        qCritical() << "Narrow layout let the text escape the badge";
        return 1;
    }
    qInfo() << "Badge expands with text and elides it within a narrow layout";
}
