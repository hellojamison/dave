#include "engine/plugins/PluginEditor.h"
#include "engine/plugins/PluginInstance.h"

#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/base/funknownimpl.h>
#include <public.sdk/source/vst/hosting/hostclasses.h>

#include <AppKit/AppKit.h>
#include <algorithm>
#include <cstdio>

namespace dave::engine {

using namespace Steinberg;
using namespace Steinberg::Vst;

// PluginEditor::Impl holds the Cocoa + SDK objects that must be released on
// close. We keep them here so PluginEditor.h stays Objective-C-free.
struct PluginEditor::Impl {
    IPtr<IPlugView> plugView;
    NSWindow* nsWindow = nil;       // strong (we release on close)
    NSView* plugViewNs = nil;       // the plugin's view, retained by the window
};

PluginEditor::PluginEditor() : p_(std::make_unique<Impl>()) {}
PluginEditor::~PluginEditor() { close(); }

bool PluginEditor::open(PluginInstance& instance, const std::string& title) {
    close();

    // 1) Get the controller + ask it for a plug view.
    FUnknown* ctrlUnknown = static_cast<FUnknown*>(instance.editControllerAsUnknown());
    if (!ctrlUnknown) {
        return false;
    }
    IEditController* controller = FUnknownPtr<IEditController>(ctrlUnknown);
    if (!controller) {
        return false;
    }
    p_->plugView = owned(controller->createView("editor"));
    if (!p_->plugView) {
        std::fprintf(stderr, "Dave: plugin '%s' has no editor view\n", title.c_str());
        return false;
    }

    // 2) Create a native NSWindow sized to the plugin's preferred content size.
    ViewRect r;
    if (p_->plugView->getSize(&r) != kResultOk) {
        // Sensible default if the plugin doesn't report a size.
        r.left = r.top = 0; r.right = 600; r.bottom = 400;
    }
    const int w = std::max(100, r.right - r.left);
    const int h = std::max(100, r.bottom - r.top);

    NSRect contentRect = NSMakeRect(0, 0, w, h);
    NSUInteger styleMask = NSTitledWindowMask | NSClosableWindowMask |
                           NSMiniaturizableWindowMask | NSResizableWindowMask;
    p_->nsWindow = [[NSWindow alloc] initWithContentRect:contentRect
                                              styleMask:styleMask
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    if (!p_->nsWindow) {
        std::fprintf(stderr, "Dave[plugin-editor]: NSWindow alloc failed\n");
        p_->plugView = nullptr;
        return false;
    }
    [p_->nsWindow setTitle:[NSString stringWithUTF8String:title.c_str()]];
    [p_->nsWindow makeKeyAndOrderFront:nil];

    // 3) Attach the plug view to the window's contentView. On macOS the plugin
    //    expects kPlatformTypeNSView; we hand it the contentView as the parent.
    void* parent = (__bridge void*)[p_->nsWindow contentView];
    if (p_->plugView->attached(parent, kPlatformTypeNSView) != kResultOk) {
        std::fprintf(stderr, "Dave[plugin-editor]: plugView attached() failed\n");
        [p_->nsWindow close];
        p_->nsWindow = nil;
        p_->plugView = nullptr;
        return false;
    }

    // The plug view is now parented; remember it for close.
    p_->plugViewNs = (__bridge NSView*)parent;
    std::fprintf(stderr, "Dave[plugin-editor]: opened '%s' (%dx%d)\n",
                 title.c_str(), w, h);
    return true;
}

void PluginEditor::close() {
    if (p_->plugView) {
        p_->plugView->removed();
        p_->plugView = nullptr;
    }
    if (p_->nsWindow) {
        [p_->nsWindow close];
        p_->nsWindow = nil;
    }
    p_->plugViewNs = nil;
}

bool PluginEditor::isOpen() const {
    return p_->nsWindow != nil;
}

} // namespace dave::engine
