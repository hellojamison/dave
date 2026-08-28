// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>

struct GLFWwindow;

namespace dave::platform {

// Live file-drag-over state, for showing a drop preview before release. GLFW
// only surfaces the drop itself, so on macOS we add draggingUpdated/Exited to
// the native content view (see FileDragMac.mm). On other platforms this is a
// no-op and no preview shows — the drop still works through GLFW.
struct FileDragInfo {
    bool active = false;
    double x = 0.0;   // window content coords (points), matching cursorPos()
    double y = 0.0;
    std::string path; // the first dragged file, or empty
};

// Install drag-over tracking on `window`'s native view. `onUpdate` runs on the
// main thread during a drag (the app's own run loop is blocked in the OS drag
// loop meanwhile), so the caller uses it to redraw the preview each move.
void installFileDragTracking(GLFWwindow* window, std::function<void()> onUpdate);

// The current drag-over state (inactive when nothing is being dragged over).
FileDragInfo fileDragInfo();

// Clear the state — call once a drop has been handled so the preview vanishes.
void clearFileDrag();

} // namespace dave::platform
