#!/usr/bin/env python3
import os
import sys
from PySide6.QtGui import QGuiApplication, QIcon
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuickControls2 import QQuickStyle
from backend.engine import EditorBridge

def main():
    # 强制启用深色风格与高 DPI 支持
    os.environ["QT_QUICK_CONTROLS_STYLE"] = "Basic"
    app = QGuiApplication(sys.argv)
    app.setApplicationName("DJI智能剪辑大师 Pro")
    app.setOrganizationName("DJI_Editor")

    engine = QQmlApplicationEngine()
    bridge = EditorBridge()
    app.aboutToQuit.connect(bridge.close)
    engine.rootContext().setContextProperty("bridge", bridge)

    qml_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "qml", "AppWindow.qml")
    engine.load(qml_path)

    if not engine.rootObjects():
        print("❌ QML 界面加载失败！")
        sys.exit(-1)

    print("====================================================================")
    print("★ DJI 智能剪辑大师 Pro · Qt Quick 6 原生导演台已启动！")
    print("★ 渲染引擎: Qt Quick (RHI / Metal 硬件加速)")
    print("★ 音视频核心: FFmpeg 65Mbps 10-Bit HDR (p010le / BT.2020 HLG)")
    print("====================================================================")

    sys.exit(app.exec())

if __name__ == "__main__":
    main()
