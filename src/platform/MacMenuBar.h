// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>

namespace dave::platform {

// Sets up a native macOS screen-top menu bar via NSApplication.
void setupMacMenuBar();

// Menu action callbacks — set these before calling setupMacMenuBar().
// std::function so callers can assign lambdas with captures.
extern std::function<void()> g_menuNew;
extern std::function<void()> g_menuOpen;
extern std::function<void()> g_menuSave;
extern std::function<void()> g_menuSaveAs;
extern std::function<void()> g_menuLoadWav;
extern std::function<void()> g_menuLoadVideo;
extern std::function<void()> g_menuImportMarkers;
extern std::function<void()> g_menuExportMarkers;
extern std::function<void()> g_menuUndo;
extern std::function<void()> g_menuRedo;
extern std::function<void()> g_menuAddTrack;
extern std::function<void()> g_menuToggleTransientNavigation;
extern std::function<void()> g_menuNextLandmark;
extern std::function<void()> g_menuPreviousLandmark;
extern std::function<void()> g_menuExtendNextLandmark;
extern std::function<void()> g_menuExtendPreviousLandmark;
extern std::function<void()> g_menuPlayStop;
extern std::function<void()> g_menuReturnToStart;
extern std::function<void()> g_menuToggleVideoWindow;
extern std::function<void()> g_menuToggleIoPanel;
extern std::function<void()> g_menuImportMidi;
extern std::function<void()> g_menuOpenPreferences;
extern std::function<void()> g_menuQuit;

} // namespace dave::platform
