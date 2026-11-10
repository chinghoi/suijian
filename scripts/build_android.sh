#!/bin/sh
# Qt Android 打包。仓库已不再使用 PySide，因此不能走 pyside6-android-deploy。
set -eu
if [ "$(uname -s)" != "Linux" ] && [ "$(uname -s)" != "Darwin" ]; then
  echo "请在 Linux 或 macOS 上准备 Android SDK 与 Qt Android 套件"
  exit 2
fi
if [ -z "${ANDROID_SDK_ROOT:-}${ANDROID_HOME:-}" ] || [ -z "${QT_ANDROID_ROOT:-}" ]; then
  echo "未配置 ANDROID_SDK_ROOT 与 QT_ANDROID_ROOT，当前环境不能打出 APK"
  exit 2
fi
cmake -S . -B build-android -G Ninja \
  -DCMAKE_PREFIX_PATH="$QT_ANDROID_ROOT" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="${ANDROID_ABI:-arm64-v8a}" \
  -DANDROID_PLATFORM=android-28
cmake --build build-android
echo "Qt 目标已编译。APK 还需用该套件自带的 androiddeployqt 打包。"
