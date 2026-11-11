#include <QWindow>
#import <AppKit/AppKit.h>

bool styleMacWindow(QWindow &window)
{
    NSView *view = reinterpret_cast<NSView *>(window.winId());
    NSWindow *nativeWindow = view.window;
    if (!nativeWindow)
        return false;
    nativeWindow.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    nativeWindow.titleVisibility = NSWindowTitleHidden;
    nativeWindow.titlebarAppearsTransparent = YES;
    nativeWindow.backgroundColor = [NSColor colorWithSRGBRed:20.0 / 255 green:22.0 / 255
                                                        blue:28.0 / 255 alpha:1.0];
    return true;
}
