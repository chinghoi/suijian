# 随剪 (SuiJian)

专为 **懒得剪辑的人** 设计的 DJI 视频快速初筛与出片工具。

拍了一堆航拍或 Vlog 视频堆在卡里懒得剪？「随剪」的核心就是帮你 **一键筛掉所有垃圾无效片段，只留精彩瞬间**。

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Windows%20%7C%20Linux%20%7C%20Android%20%7C%20iOS-green.svg)](#)

---

## 适用人群

* **拍完视频懒得剪的人**：卡里几十 GB 视频放了好几个月，一想到要打开臃肿复杂的剪辑软件就头疼；
* **只想快速挑出好看镜头的人**：不需要复杂的调色特效，只想快速把晃动废片扔掉、留下精华直接出片发朋友圈或社交平台；
* **随时拔卡补拍的随摄党**：初筛到一半随时拔掉相机去补录镜头，插上还能接着处理，不怕误拔丢失进度。

---

## 核心特性

### 1. 自动过滤无效废片
* 自动切掉镜头朝地、剧烈晃动、长段无声以及发呆无用的垃圾镜头；
* 本地 AI 自动挑出构图好、动作精彩的镜头，告别手动拖动进度条找亮点的痛苦。

### 2. 随时拔卡录像，插上断点续剪
* **随拔随拍**：处理中随时点击“安全拔卡”，拔走相机去补拍录像，断点自动存盘；
* **不怕误拔**：意外扯掉数据线绝不闪退报错，插回卡后自动接着断点继续；
* **卡内变动提醒**：如果在相机里删了原片，时间线会自动标红提醒失效；新拍的视频不会自动插队捣乱。

### 3. 原生 4K 10-Bit HDR 原画质直出
* 硬件加速流畅预览 DJI 4K 60fps 10-Bit (D-Log M / HLG) 大码率素材，不卡顿不掉帧；
* 一键导出横屏母带或 9:16 竖屏短视频。

### 4. 移动端与触控响应式双模架构
* **多端自适应**：界面基于 Qt Quick / QML 编写，同一套代码原生支持电脑端三栏专业工作台与移动端/平板触控流体布局；
* **触控大热区**：配备底部拇指操作栏与大尺寸触控卡片，针对触摸屏与便携设备全面优化；
* **手势快捷操作**：支持双指轻点 (Two-finger Tap) 快速撤销、双指捏合与滑动操作。

---

## 下载与运行

### 电脑端免环境安装包
GitHub Actions 云构建会自动随代码生成全平台独立运行包，可直接在 [Actions 构建产物](https://github.com/chinghoi/suijian/actions) 中下载：
* macOS: `SuiJian-macOS.dmg`
* Windows: `SuiJian-Windows.zip` (解压后直接双击运行 `随剪.exe`)
* Linux: `SuiJian-Linux.tar.gz`
* Linux (原生 C++ 极致轻量版 · 约 30MB): `SuiJian-Native-Linux-x64.tar.gz`

### 移动端与平板 (Android / iOS)
* 移动端流体布局与触控手势已完整实装，仓库内已包含完整的 Android 自适应图标与 iOS 资产；
* Android 独立 `.apk` 安装包流水线正在通过 `pyside6-android-deploy` 接入 GitHub Actions 矩阵，完成后可直接在 Releases 中一键下载安装。

### 源码运行
```bash
# 1. 安装依赖
pip install -r requirements.txt

# 2. 启动
python3 main.py
```

---

## 路线图 (Roadmap)

### 近期规划
- [ ] **独立外设适配模块**：自动检测 USB / SD 卡挂载，适配 DJI、GoPro、Sony、Canon 设备目录，自动过滤 `.LRF` / `.LRV` 等代理小文件。
- [ ] **通用 3D LUT 调色**：支持导入标准 `.cube` 调色预设，FFmpeg 硬件加速直接渲染。
- [ ] **免版权音乐与自动卡点**：内置专业 CC0 免费商用音乐库，基于音频重音节奏自动吸附剪辑点。
- [ ] **Android APK 自动化构建流水线**：接入 `pyside6-android-deploy` 与 Android SDK，实现 GitHub Actions 自动构建 `.apk` 安装包。

### 中期规划
- [ ] **多模态 AI 发圈文案**：成片后自动截取精彩关键帧，一键生成朋友圈与小红书文案。
- [ ] **剪辑工程直导**：支持导出 Final Cut Pro (`.fcpxml`) 及剪映草稿工程。
- [ ] **iOS / iPadOS 打包与 TestFlight**：导出 Xcode 工程并适配 iPadOS 触控与文件系统沙盒。

### 远期规划（探索性 · 远期中的远期）
- [ ] **多模态故事线编排**：按剧情起承转合与叙事意图自动排布成片节奏。
- [ ] **自然语言随心粗剪**：支持自然语言描述（如“只留猫咪特写和夕阳海边”）定向粗剪。

---

## 开源协议

本项目采用 [MIT License](LICENSE) 开源。
