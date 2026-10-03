#include "MacPresentation.h"

#import <AppKit/AppKit.h>

#include <QWindow>

static NSWindow *nsWindowOf(QWindow *w)
{
    if (!w || !w->handle()) return nil;
    NSView *view = reinterpret_cast<NSView *>(w->winId());
    return view ? [view window] : nil;
}

void macPrepareFullScreen(QWindow *w)
{
    NSWindow *win = nsWindowOf(w);
    if (!win) return;
    NSWindowCollectionBehavior b = [win collectionBehavior];
    b |= NSWindowCollectionBehaviorFullScreenPrimary; // its own Space, as any other application
    b &= ~NSWindowCollectionBehaviorFullScreenNone;
    [win setCollectionBehavior:b];
}

bool macIsFullScreen(QWindow *w)
{
    NSWindow *win = nsWindowOf(w);
    return win && ([win styleMask] & NSWindowStyleMaskFullScreen) != 0;
}

void macEnterFullScreen(QWindow *w)
{
    NSWindow *win = nsWindowOf(w);
    if (!win || ([win styleMask] & NSWindowStyleMaskFullScreen)) return;
    macPrepareFullScreen(w);
    [win toggleFullScreen:nil];
}

void macExitFullScreen(QWindow *w, std::function<void()> done)
{
    NSWindow *win = nsWindowOf(w);
    if (!win || !([win styleMask] & NSWindowStyleMaskFullScreen)) {
        if (done) done();
        return;
    }
    // The Space is only gone once macOS says the window left fullscreen: everything waits for that.
    __block id token = nil;
    __block std::function<void()> fn = std::move(done);
    token = [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidExitFullScreenNotification
                                                              object:win
                                                               queue:[NSOperationQueue mainQueue]
                                                          usingBlock:^(NSNotification *) {
        [[NSNotificationCenter defaultCenter] removeObserver:token];
        if (fn) fn();
        fn = nullptr;
    }];
    [win toggleFullScreen:nil];
}
