#!/usr/bin/env python3
import os
import sys
import shutil
import subprocess
from PIL import Image

def generate_all_icons(master_image_path, output_dir):
    print(f"Loading master icon: {master_image_path}")
    base_img = Image.open(master_image_path).convert("RGBA")
    
    # 确保正方形 1024x1024
    if base_img.size != (1024, 1024):
        base_img = base_img.resize((1024, 1024), Image.Resampling.LANCZOS)

    os.makedirs(output_dir, exist_ok=True)
    
    # 根目录主力 PNG
    root_png = os.path.join(output_dir, "icon.png")
    base_img.save(root_png, "PNG")
    print(f"✓ Generated: {root_png}")

    # ==========================================
    # 1. macOS 适配 (.icns + AppIcon.png)
    # ==========================================
    macos_dir = os.path.join(output_dir, "macos")
    os.makedirs(macos_dir, exist_ok=True)
    base_img.save(os.path.join(macos_dir, "AppIcon-1024.png"), "PNG")
    
    iconset_dir = "/tmp/dji_app_icon.iconset"
    os.makedirs(iconset_dir, exist_ok=True)
    sizes = [
        ("icon_16x16.png", (16, 16)),
        ("icon_16x16@2x.png", (32, 32)),
        ("icon_32x32.png", (32, 32)),
        ("icon_32x32@2x.png", (64, 64)),
        ("icon_128x128.png", (128, 128)),
        ("icon_128x128@2x.png", (256, 256)),
        ("icon_256x256.png", (256, 256)),
        ("icon_256x256@2x.png", (512, 512)),
        ("icon_512x512.png", (512, 512)),
        ("icon_512x512@2x.png", (1024, 1024)),
    ]
    for name, sz in sizes:
        resized = base_img.resize(sz, Image.Resampling.LANCZOS)
        resized.save(os.path.join(iconset_dir, name), "PNG")
    
    icns_path = os.path.join(output_dir, "icon.icns")
    subprocess.run(["iconutil", "-c", "icns", iconset_dir, "-o", icns_path], check=True)
    shutil.copy2(icns_path, os.path.join(macos_dir, "icon.icns"))
    shutil.rmtree(iconset_dir, ignore_errors=True)
    print(f"✓ Generated macOS ICNS: {icns_path}")

    # ==========================================
    # 2. Windows 10/11 适配 (.ico + 多尺寸资产)
    # ==========================================
    win_dir = os.path.join(output_dir, "windows")
    os.makedirs(win_dir, exist_ok=True)
    ico_sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    ico_path = os.path.join(output_dir, "icon.ico")
    base_img.save(ico_path, format="ICO", sizes=ico_sizes)
    shutil.copy2(ico_path, os.path.join(win_dir, "icon.ico"))
    
    # Windows Fluent 磁贴图标
    for sz in [48, 64, 150, 256, 310]:
        t_img = base_img.resize((sz, sz), Image.Resampling.LANCZOS)
        t_img.save(os.path.join(win_dir, f"Square{sz}x{sz}Logo.png"), "PNG")
    print(f"✓ Generated Windows ICO: {ico_path}")

    # ==========================================
    # 3. iOS / iPadOS 适配 (1024x1024 纯色无圆角裁切)
    # ==========================================
    ios_dir = os.path.join(output_dir, "ios")
    os.makedirs(ios_dir, exist_ok=True)
    # iOS 系统会自动施加连续曲率圆角蒙版，因此原图必须是实底无透明裁切
    ios_master = Image.new("RGBA", (1024, 1024), (12, 13, 17, 255))
    ios_master.paste(base_img, (0, 0), base_img)
    ios_master.convert("RGB").save(os.path.join(ios_dir, "AppIcon-1024.png"), "PNG")
    print(f"✓ Generated iOS Master Icon: {os.path.join(ios_dir, 'AppIcon-1024.png')}")

    # ==========================================
    # 4. Android 自适应图标 (Adaptive Icons: 前景 + 背景)
    # ==========================================
    android_dir = os.path.join(output_dir, "android")
    os.makedirs(android_dir, exist_ok=True)
    
    # 4.1 背景层 (432x432)
    bg_img = Image.new("RGBA", (432, 432), (15, 17, 23, 255))
    bg_img.save(os.path.join(android_dir, "ic_launcher_background.png"), "PNG")
    
    # 4.2 前景层 (432x432，留出 25% 安全视界)
    fg_size = 280
    fg_resized = base_img.resize((fg_size, fg_size), Image.Resampling.LANCZOS)
    fg_canvas = Image.new("RGBA", (432, 432), (0, 0, 0, 0))
    offset = (432 - fg_size) // 2
    fg_canvas.paste(fg_resized, (offset, offset), fg_resized)
    fg_canvas.save(os.path.join(android_dir, "ic_launcher_foreground.png"), "PNG")
    
    # 4.3 传统各分辨率 mipmap
    density_sizes = {
        "mipmap-mdpi": (48, 48),
        "mipmap-hdpi": (72, 72),
        "mipmap-xhdpi": (96, 96),
        "mipmap-xxhdpi": (144, 144),
        "mipmap-xxxhdpi": (192, 192),
    }
    for folder, sz in density_sizes.items():
        sub_d = os.path.join(android_dir, folder)
        os.makedirs(sub_d, exist_ok=True)
        m_img = base_img.resize(sz, Image.Resampling.LANCZOS)
        m_img.save(os.path.join(sub_d, "ic_launcher.png"), "PNG")
    print(f"✓ Generated Android Adaptive Icons in: {android_dir}")

    # ==========================================
    # 5. Linux Freedesktop 标准规范
    # ==========================================
    linux_dir = os.path.join(output_dir, "linux")
    os.makedirs(linux_dir, exist_ok=True)
    for sz in [16, 24, 32, 48, 64, 128, 256, 512]:
        sz_d = os.path.join(linux_dir, f"{sz}x{sz}", "apps")
        os.makedirs(sz_d, exist_ok=True)
        l_img = base_img.resize((sz, sz), Image.Resampling.LANCZOS)
        l_img.save(os.path.join(sz_d, "dji-editor.png"), "PNG")
    print(f"✓ Generated Linux Icons in: {linux_dir}")

    # ==========================================
    # 6. Web / Favicon 资产
    # ==========================================
    web_dir = os.path.join(output_dir, "web")
    os.makedirs(web_dir, exist_ok=True)
    base_img.resize((32, 32), Image.Resampling.LANCZOS).save(os.path.join(web_dir, "favicon-32x32.png"), "PNG")
    base_img.resize((180, 180), Image.Resampling.LANCZOS).save(os.path.join(web_dir, "apple-touch-icon.png"), "PNG")
    base_img.resize((192, 192), Image.Resampling.LANCZOS).save(os.path.join(web_dir, "android-chrome-192x192.png"), "PNG")
    print(f"✓ Generated Web Assets in: {web_dir}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 generate_platform_icons.py <master_image> <output_dir>")
        sys.exit(1)
    generate_all_icons(sys.argv[1], sys.argv[2])
