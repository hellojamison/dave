// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/MacMenuBar.h"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>

// Objective-C interface must be at global scope (not inside a C++ namespace).
@interface DaveMenuItemTarget : NSObject
@end

@implementation DaveMenuItemTarget
- (void)action:(id)sender {
    NSMenuItem* item = sender;
    NSInteger tag = item.tag;
    using namespace dave::platform;
    switch (tag) {
        case 1: if (g_menuNew) g_menuNew(); break;
        case 2: if (g_menuOpen) g_menuOpen(); break;
        case 3: if (g_menuSave) g_menuSave(); break;
        case 4: if (g_menuSaveAs) g_menuSaveAs(); break;
        case 5: if (g_menuLoadWav) g_menuLoadWav(); break;
        case 6: if (g_menuLoadVideo) g_menuLoadVideo(); break;
        case 7: if (g_menuImportMarkers) g_menuImportMarkers(); break;
        case 8: if (g_menuExportMarkers) g_menuExportMarkers(); break;
        case 9: if (g_menuUndo) g_menuUndo(); break;
        case 10: if (g_menuRedo) g_menuRedo(); break;
        case 11: if (g_menuPlayStop) g_menuPlayStop(); break;
        case 12: if (g_menuReturnToStart) g_menuReturnToStart(); break;
        case 13: if (g_menuQuit) g_menuQuit(); break;
        case 14: if (g_menuToggleVideoWindow) g_menuToggleVideoWindow(); break;
        case 15: if (g_menuImportMidi) g_menuImportMidi(); break;
        case 16: if (g_menuToggleIoPanel) g_menuToggleIoPanel(); break;
        case 17: if (g_menuAddTrack) g_menuAddTrack(); break;
    }
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app {
    if (dave::platform::g_menuQuit) dave::platform::g_menuQuit();
    return NSTerminateCancel;
}
@end

namespace dave::platform {

// Global callback functions.
std::function<void()> g_menuNew;
std::function<void()> g_menuOpen;
std::function<void()> g_menuSave;
std::function<void()> g_menuSaveAs;
std::function<void()> g_menuLoadWav;
std::function<void()> g_menuLoadVideo;
std::function<void()> g_menuImportMarkers;
std::function<void()> g_menuExportMarkers;
std::function<void()> g_menuUndo;
std::function<void()> g_menuRedo;
std::function<void()> g_menuAddTrack;
std::function<void()> g_menuPlayStop;
std::function<void()> g_menuReturnToStart;
std::function<void()> g_menuToggleVideoWindow;
std::function<void()> g_menuToggleIoPanel;
std::function<void()> g_menuImportMidi;
std::function<void()> g_menuQuit;

static DaveMenuItemTarget* g_target = nil;

void setupMacMenuBar() {
    NSApplication* app = [NSApplication sharedApplication];

    // Create the target that receives all menu actions.
    g_target = [[DaveMenuItemTarget alloc] init];
    [app setDelegate:(id<NSApplicationDelegate>)g_target];

    SEL action = @selector(action:);
    auto makeItem = [&](NSString* title, NSInteger tag, NSString* key = nil) {
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                      action:action
                                               keyEquivalent:(key ?: @"")];
        item.target = g_target;
        item.tag = tag;
        return item;
    };

    // ─── App menu (Dave / About / Quit) ───────────────────────────────────
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@""];
    [appMenu addItemWithTitle:@"About Dave" action:nil keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItem:makeItem(@"Hide Dave", 0, @"h")];
    [appMenu addItem:makeItem(@"Quit Dave", 13, @"q")];
    [appMenuItem setSubmenu:appMenu];
    [[app mainMenu] addItem:appMenuItem];

    // ─── File menu ───────────────────────────────────────────────────────
    NSMenuItem* fileMenuItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [fileMenu addItem:makeItem(@"New", 1, @"n")];
    [fileMenu addItem:makeItem(@"Open…", 2, @"o")];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [fileMenu addItem:makeItem(@"Save", 3, @"s")];
    [fileMenu addItem:makeItem(@"Save As…", 4, @"S")];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [fileMenu addItem:makeItem(@"Load WAV…", 5)];
    [fileMenu addItem:makeItem(@"Import MIDI…", 15)];
    [fileMenu addItem:makeItem(@"Load Video…", 6)];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [fileMenu addItem:makeItem(@"Import Markers (CSV)…", 7)];
    [fileMenu addItem:makeItem(@"Export Markers (CSV)…", 8)];
    [fileMenuItem setSubmenu:fileMenu];
    [[app mainMenu] addItem:fileMenuItem];

    // ─── Edit menu ───────────────────────────────────────────────────────
    NSMenuItem* editMenuItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenu addItem:makeItem(@"Undo", 9, @"z")];
    [editMenu addItem:makeItem(@"Redo", 10, @"Z")];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItem:makeItem(@"Add Track", 17, @"N")];
    [editMenuItem setSubmenu:editMenu];
    [[app mainMenu] addItem:editMenuItem];

    // ─── Transport menu ──────────────────────────────────────────────────
    NSMenuItem* trMenuItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* trMenu = [[NSMenu alloc] initWithTitle:@"Transport"];
    [trMenu addItem:makeItem(@"Play/Stop", 11, @" ")];
    [trMenu addItem:makeItem(@"Return to Start", 12, @"\r")];
    [trMenuItem setSubmenu:trMenu];
    [[app mainMenu] addItem:trMenuItem];

    // ─── Window menu (standard macOS) ────────────────────────────────────
    NSMenuItem* winMenuItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* winMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [winMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [winMenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [winMenu addItem:[NSMenuItem separatorItem]];
    [winMenu addItem:makeItem(@"I/O Panel", 16)];
    // Toggles the picture between the sidebar and its own draggable window —
    // the escape hatch if the detached window ends up behind something.
    [winMenu addItem:makeItem(@"Video Window", 14, @"V")];
    [winMenuItem setSubmenu:winMenu];
    [[app mainMenu] addItem:winMenuItem];
}

} // namespace dave::platform

#endif // __APPLE__
