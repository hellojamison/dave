// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/FileDrag.h"

#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <mutex>

namespace dave::platform {
namespace {

std::mutex g_mutex;
FileDragInfo g_info;
std::function<void()> g_onUpdate;

// Pull the first dragged file path off the pasteboard, or "".
std::string firstDraggedPath(id<NSDraggingInfo> sender) {
    NSPasteboard* pb = [sender draggingPasteboard];
    NSArray* urls = [pb readObjectsForClasses:@[[NSURL class]]
                                      options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    if (urls.count == 0) return {};
    NSURL* url = urls.firstObject;
    if (![url isFileURL]) return {};
    return std::string(url.path.UTF8String ? url.path.UTF8String : "");
}

// Drag location -> window content coordinates with a top-left origin, matching
// glfwGetCursorPos and therefore ImGui's screen space for the main viewport.
void locationInContent(NSView* view, id<NSDraggingInfo> sender,
                       double& x, double& y) {
    const NSPoint inWindow = [sender draggingLocation];
    const NSPoint inView = [view convertPoint:inWindow fromView:nil];
    x = inView.x;
    y = view.bounds.size.height - inView.y;   // flip to top-left origin
}

NSDragOperation daveDraggingUpdated(id self, SEL, id<NSDraggingInfo> sender) {
    NSView* view = (NSView*)self;
    double x = 0.0, y = 0.0;
    locationInContent(view, sender, x, y);
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_info.active = true;
        g_info.x = x;
        g_info.y = y;
        if (g_info.path.empty()) g_info.path = firstDraggedPath(sender);
        notify = g_onUpdate;
    }
    // Render a frame now: during a drag the app's own loop is parked in the OS
    // modal loop, so nothing else will draw the moving preview.
    if (notify) notify();
    return NSDragOperationGeneric;
}

void daveDraggingExited(id self, SEL, id<NSDraggingInfo>) {
    (void)self;
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_info = FileDragInfo{};
        notify = g_onUpdate;
    }
    if (notify) notify();
}

} // namespace

void installFileDragTracking(GLFWwindow* window, std::function<void()> onUpdate) {
    if (window == nullptr) return;
    NSWindow* nsWindow = glfwGetCocoaWindow(window);
    NSView* view = nsWindow.contentView;
    if (view == nil) return;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_onUpdate = std::move(onUpdate);
    }

    // GLFW's content view already answers draggingEntered:/performDragOperation:
    // (that is how the drop callback works) but not draggingUpdated:/Exited:.
    // Add them to its class so the drag position reaches us mid-drag. If a
    // future GLFW adds its own, class_addMethod leaves the existing one alone.
    Class cls = object_getClass(view);
    class_addMethod(cls, @selector(draggingUpdated:),
                    (IMP)daveDraggingUpdated, "Q@:@");
    class_addMethod(cls, @selector(draggingExited:),
                    (IMP)daveDraggingExited, "v@:@");
}

FileDragInfo fileDragInfo() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_info;
}

void clearFileDrag() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_info = FileDragInfo{};
}

} // namespace dave::platform
