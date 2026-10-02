#include "MacPresentation.h"
#import <AppKit/AppKit.h>

void macSetMenuBarAndDockHidden(bool hidden)
{
    // macOS applies these options only while the application is active:
    // switching to another app brings the menu bar and the Dock back.
    [NSApp setPresentationOptions:hidden ? (NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar)
                                         : NSApplicationPresentationDefault];
}
