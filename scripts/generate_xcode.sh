#!/bin/sh
# 在 macOS 上生成可供 Xcode 打开的 iOS 工程。TestFlight 需要 Apple 开发者账号，此脚本不上传。
set -eu
if [ "$(uname -s)" != "Darwin" ]; then
  echo "Xcode 工程只能在 macOS 上生成"
  exit 2
fi
if [ -z "${QT_IOS_ROOT:-}" ]; then
  echo "未配置 QT_IOS_ROOT，不能生成可编译的 iOS 工程"
  exit 2
fi
cmake -S . -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_PREFIX_PATH="$QT_IOS_ROOT" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=iphoneos
echo "已生成 build-ios。用 Xcode 打开后才能归档并提交 TestFlight。"
