#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
#include <QPalette>
#include <QColor>
#include <QWindow>
#include "MediaEngine.h"

#ifdef Q_OS_MAC
bool styleMacWindow(QWindow &window);
#endif

int main(int argc, char *argv[]) {
#ifdef Q_OS_MAC
    // Finder applications do not inherit the shell's Homebrew search path.
    QByteArray path = qgetenv("PATH");
    path += ":/opt/homebrew/bin:/usr/local/bin";
    qputenv("PATH", path);
#endif
    QQuickStyle::setStyle("Basic");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

    QGuiApplication app(argc, argv);
    app.setApplicationName("随剪");
    app.setOrganizationName("SuiJian");

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, QColor("#14161c"));
    darkPalette.setColor(QPalette::WindowText, QColor("#ffffff"));
    darkPalette.setColor(QPalette::Base, QColor("#0c0d11"));
    darkPalette.setColor(QPalette::Text, QColor("#ffffff"));
    darkPalette.setColor(QPalette::Button, QColor("#1e2436"));
    darkPalette.setColor(QPalette::ButtonText, QColor("#ffffff"));
    app.setPalette(darkPalette);

    QIcon appIcon("assets/icon.png");
    if (!appIcon.isNull()) {
        app.setWindowIcon(appIcon);
    }

    QQmlApplicationEngine engine;
    MediaEngine bridge;
    engine.rootContext()->setContextProperty("bridge", &bridge);

#ifdef Q_OS_MAC
    engine.setInitialProperties({{QStringLiteral("visible"), false}});
#endif
    engine.load(QUrl(QStringLiteral("qrc:/DJI/Editor/qml/AppWindow.qml")));

    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

#ifdef Q_OS_MAC
    if (auto *window = qobject_cast<QWindow *>(engine.rootObjects().first())) {
        const bool styled = styleMacWindow(*window);
        window->show();
        if (!styled)
            styleMacWindow(*window);
    }
#endif
    return app.exec();
}
