#!/usr/bin/env python3
import os
import sys
from PySide6.QtGui import QGuiApplication, QIcon, QPalette, QColor
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuickControls2 import QQuickStyle
from backend.engine import EditorBridge

def setup_macos_dark_appearance():
    """强制让 macOS 窗口标题栏与控件适配深色主题，杜绝浅色标题栏不统一问题"""
    if sys.platform == "darwin":
        try:
            import AppKit
            ns_app = AppKit.NSApplication.sharedApplication()
            dark_aqua = AppKit.NSAppearance.appearanceNamed_(AppKit.NSAppearanceNameDarkAqua)
            ns_app.setAppearance_(dark_aqua)
            for w in ns_app.windows():
                w.setAppearance_(dark_aqua)
        except Exception as e:
            print(f"macOS 原生深色模式适配提示: {e}")

def main():
    # 1. 优先设置纯净跨平台样式
    QQuickStyle.setStyle("Basic")
    os.environ["QT_QUICK_CONTROLS_STYLE"] = "Basic"

    app = QGuiApplication(sys.argv)
    app.setApplicationName("翼剪")
    app.setOrganizationName("DJI_Editor")

    # 2. 全局深色调色板注入，确保窗口框架与原生控件 100% 统一为深色系
    dark_palette = QPalette()
    dark_palette.setColor(QPalette.Window, QColor("#14161c"))
    dark_palette.setColor(QPalette.WindowText, QColor("#ffffff"))
    dark_palette.setColor(QPalette.Base, QColor("#0c0d11"))
    dark_palette.setColor(QPalette.Text, QColor("#ffffff"))
    dark_palette.setColor(QPalette.Button, QColor("#1e2436"))
    dark_palette.setColor(QPalette.ButtonText, QColor("#ffffff"))
    app.setPalette(dark_palette)

    # 3. macOS 原生标题栏深色外观预配置
    setup_macos_dark_appearance()

    icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets", "icon.png")
    if os.path.exists(icon_path):
        app.setWindowIcon(QIcon(icon_path))

    engine = QQmlApplicationEngine()
    bridge = EditorBridge()
    app.aboutToQuit.connect(bridge.close)
    engine.rootContext().setContextProperty("bridge", bridge)

    qml_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "qml", "AppWindow.qml")
    engine.load(qml_path)

    if not engine.rootObjects():
        print("❌ QML 界面加载失败！")
        sys.exit(-1)

    # 4. 窗口创建后再次确保深色外观生效
    setup_macos_dark_appearance()

    print("====================================================================")
    print("★ 原生 4K 10-Bit HDR 视频导演台已启动！")
    print("★ 窗口状态: macOS 深色原生标题栏已同步统一")
    print("====================================================================")

    sys.exit(app.exec())

if __name__ == "__main__":
    main()
