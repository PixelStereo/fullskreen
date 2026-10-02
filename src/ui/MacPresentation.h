#pragma once
// macOS only: hides the menu bar and the Dock while the application is active.
// Lets the output cover the main screen without macOS native fullscreen,
// which would open a separate Space.
void macSetMenuBarAndDockHidden(bool hidden);
