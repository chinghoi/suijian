# 随剪 SuiJian

随剪是基于 Qt 6 和 FFmpeg 的视频初筛与粗剪工具，项目以 C++ 为唯一核心实现。

## 当前能力

- 导入 MP4、MOV 等视频，按文件修改日期浏览、预览和手动调整时间线。
- 使用音量和静音检测生成候选片段，支持暂停及检查点恢复。它不是语音识别或视觉质量分析，筛选结果需要人工检查。
- 导出 MP4、FCPXML、JSON、CSV 和已有字幕的 SRT。导出成片时，画幅不匹配的素材采用清晰前景和低分辨率模糊背景；单个镜头失败会终止任务。
- 10-bit HLG 导出只接受已标记为 BT.2020 HLG 的 10-bit 素材；目前不提供 D-Log/SDR 转 HLG 或 HDR 转 SDR 的色彩转换。FCPXML 仍需在目标剪辑软件中验证。
- 暂停时须等待界面显示任务已暂停后再拔出设备。意外断开后请检查素材和恢复结果；此功能不等同于操作系统卸载磁盘。

## 本地构建

需要 Qt 6（Core、Gui、Quick、QuickControls2、Multimedia）、CMake 3.20、C++17，以及在运行时可调用的 `ffmpeg` 和 `ffprobe`。

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build --config Release
```

macOS 可运行 `build/随剪.app`；Linux 可运行 `build/DJIEditor`。应用会调用系统安装的 FFmpeg，并使用其可用的编码器。Linux 发布包目前还依赖系统 Qt 和 FFmpeg；macOS 构建会部署 Qt，仍需安装 FFmpeg。

## 待完善

- 原生语音转写、视觉质量与运动分析、跨平台模型管理。
- 经真实素材验证的色彩管理、HDR/SDR 转换与剪辑软件交换。
- 完整独立发布包和跨平台设备热插拔测试。

## 许可证

[MIT License](LICENSE)
