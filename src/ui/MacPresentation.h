#pragma once
// macOS only. Native fullscreen puts the output on its own Space, as every other application does.
// The Space is only closed once macOS has finished the exit animation, so the window must not be hidden
// before that — hiding it mid-animation is what used to leave an empty Space behind.

#include <functional>

class QWindow;

// Allows this window to take a Space of its own, and keeps it out of the window cycling of the Dock.
void macPrepareFullScreen(QWindow *w);

// True while the window holds its own Space.
bool macIsFullScreen(QWindow *w);

// Enters native fullscreen on the screen the window is on.
void macEnterFullScreen(QWindow *w);

// Leaves fullscreen and calls `done` once the Space is gone. Calls `done` at once if there is none.
void macExitFullScreen(QWindow *w, std::function<void()> done);
