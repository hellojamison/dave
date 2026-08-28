// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/FileDrag.h"

// The macOS implementation lives in FileDragMac.mm. Elsewhere there is no
// drag-over preview yet; the drop itself still works through GLFW.
#ifndef __APPLE__
namespace dave::platform {
void installFileDragTracking(GLFWwindow*, std::function<void()>) {}
FileDragInfo fileDragInfo() { return {}; }
void clearFileDrag() {}
} // namespace dave::platform
#endif
