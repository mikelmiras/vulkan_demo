// macOS platform layer: Cocoa window with a CAMetalLayer (Vulkan via MoltenVK / KosmicKrisp) + Dear ImGui's OSX backend.
// Rendering is done at one pixel per point: the editor and renderer share window coordinates, so the framebuffer must
// match ImGui's DisplaySize.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#define VK_USE_PLATFORM_METAL_EXT
#include <cstring>
#include <vulkan/vulkan.h>

#include "platform/platform.h"
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <crt_externs.h>   // (_NSGetEnviron: `environ` isn't there for a dynamic library)

#include <mach-o/dyld.h>

#include <algorithm>
#include <string>

#include "imgui.h"
#include "imgui_impl_osx.h"

// Gamepad feedback (gamepadRumble below)
static id g_hapticEngine = nil;   // CHHapticEngine
static id g_hapticPlayer = nil;   // id<CHHapticPatternPlayer>
static GCController* g_hapticPad = nil;

@interface VkwView : NSView
@end

@implementation VkwView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer*)makeBackingLayer {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.contentsScale = 1.0;
    return layer;
}
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
// ImGui's key responder (a subview) forwards keys it doesn't want to us; swallow them so AppKit doesn't beep
- (void)keyDown:(NSEvent*)event {}
- (void)keyUp:(NSEvent*)event {}
@end

@interface VkwWindowDelegate : NSObject <NSWindowDelegate>
@end

namespace platform {
namespace {
NSWindow* g_window = nil;
VkwView* g_view = nil;
VkwWindowDelegate* g_delegate = nil;
bool g_closeRequested = false;
bool g_captured = false;
float g_mouseDx = 0.f, g_mouseDy = 0.f;
}  // namespace
}  // namespace platform

@implementation VkwWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    platform::g_closeRequested = true;
    return NO;
}
@end

namespace platform {

bool createWindow(const char* title, uint32_t width, uint32_t height) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // Minimal menu so Cmd+Q works
        NSMenu* menuBar = [NSMenu new];
        NSMenuItem* appItem = [NSMenuItem new];
        [menuBar addItem:appItem];
        NSMenu* appMenu = [NSMenu new];
        [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        appItem.submenu = appMenu;
        NSApp.mainMenu = menuBar;
        [NSApp finishLaunching];

        // Keep within 90% of the screen
        NSRect visible = NSScreen.mainScreen.visibleFrame;
        CGFloat w = std::min<CGFloat>(width, visible.size.width * 0.9);
        CGFloat h = std::min<CGFloat>(height, visible.size.height * 0.9);

        g_window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, w, h)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        if (!g_window) return false;
        g_window.releasedWhenClosed = NO;
        g_window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        g_window.title = [NSString stringWithUTF8String:title];
        g_delegate = [VkwWindowDelegate new];
        g_window.delegate = g_delegate;

        g_view = [[VkwView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
        g_view.wantsLayer = YES;
        g_window.contentView = g_view;
        [g_window center];
        [g_window makeKeyAndOrderFront:nil];
        [g_window makeFirstResponder:g_view];
        [NSApp activateIgnoringOtherApps:YES];
        return true;
    }
}

void destroyWindow() {
    setCursorCaptured(false);
    if (g_hapticPlayer) gamepadRumble(0.f, 0.f, 0.f);
    g_hapticEngine = nil;
    g_hapticPad = nil;
    [g_window close];
    g_window = nil;
    g_view = nil;
    g_delegate = nil;
}

void pollEvents() {
    @autoreleasepool {
        while (NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                   untilDate:nil
                                                      inMode:NSDefaultRunLoopMode
                                                     dequeue:YES]) {
            if (g_captured) {
                switch (event.type) {
                    case NSEventTypeMouseMoved:
                    case NSEventTypeLeftMouseDragged:
                    case NSEventTypeRightMouseDragged:
                    case NSEventTypeOtherMouseDragged:
                        g_mouseDx += (float)event.deltaX;
                        g_mouseDy += (float)event.deltaY;
                        break;
                    default: break;
                }
            }
            [NSApp sendEvent:event];
        }
        if (g_captured && (!NSApp.isActive || !g_window.isKeyWindow)) setCursorCaptured(false);   // Cmd+Tab etc.
    }
}

void setCursorCaptured(bool captured) {
    if (captured == g_captured || !g_window) return;
    g_captured = captured;
    g_mouseDx = g_mouseDy = 0.f;
    if (captured) {
        // Park the cursor in the middle of the window so clicks keep landing on it, then freeze it there
        NSRect frame = [g_window convertRectToScreen:g_view.frame];
        CGFloat screenH = NSScreen.screens[0].frame.size.height;
        CGWarpMouseCursorPosition(CGPointMake(NSMidX(frame), screenH - NSMidY(frame)));
        CGAssociateMouseAndMouseCursorPosition(false);
        [NSCursor hide];
    } else {
        CGAssociateMouseAndMouseCursorPosition(true);
        [NSCursor unhide];
    }
}

bool cursorCaptured() { return g_captured; }

void takeMouseDelta(float& dx, float& dy) {
    dx = g_mouseDx;
    dy = g_mouseDy;
    g_mouseDx = g_mouseDy = 0.f;
}

void setFullscreen(bool fullscreen) {
    if (g_window && fullscreen != isFullscreen()) [g_window toggleFullScreen:nil];
}

bool isFullscreen() { return g_window && (g_window.styleMask & NSWindowStyleMaskFullScreen) != 0; }

// Gamepad feedback through the GameController framework (macOS 11+): the pad's haptics engine plays one continuous
// event (rumble), its light sets the light bar

static GCController* firstPad() {
    if (@available(macOS 11.0, *))
        if (GCController.current) return GCController.current;
    return GCController.controllers.firstObject;
}

bool gamepadRumble(float low, float high, float seconds) {
    if (@available(macOS 11.0, *)) {
        GCController* pad = firstPad();
        if (!pad || !pad.haptics) return false;
        if (pad != g_hapticPad || !g_hapticEngine) {
            g_hapticPlayer = nil;
            g_hapticPad = pad;
            CHHapticEngine* engine = [pad.haptics createEngineWithLocality:GCHapticsLocalityDefault];
            NSError* error = nil;
            if (!engine || ![engine startAndReturnError:&error]) {
                g_hapticEngine = nil;
                return false;
            }
            g_hapticEngine = engine;
        }
        if (g_hapticPlayer) {
            [(id<CHHapticPatternPlayer>)g_hapticPlayer stopAtTime:CHHapticTimeImmediate error:nil];
            g_hapticPlayer = nil;
        }
        if (seconds <= 0.f || (low <= 0.f && high <= 0.f)) return true;
        float intensity = std::clamp(std::max(low, high), 0.f, 1.f);
        float sharpness = high / (low + high);   // (heavy motor: dull; light one: sharp)
        CHHapticEventParameter* i = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity
                                                                                  value:intensity];
        CHHapticEventParameter* s = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness
                                                                                  value:sharpness];
        CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                             parameters:@[i, s]
                                                           relativeTime:0
                                                               duration:seconds];
        NSError* error = nil;
        CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
        if (!pattern) return false;
        id<CHHapticPatternPlayer> player = [(CHHapticEngine*)g_hapticEngine createPlayerWithPattern:pattern error:&error];
        if (!player || ![player startAtTime:CHHapticTimeImmediate error:&error]) return false;
        g_hapticPlayer = player;
        return true;
    }
    return false;
}

bool setGamepadLight(float r, float g, float b) {
    if (@available(macOS 11.0, *)) {
        GCController* pad = firstPad();
        if (!pad || !pad.light) return false;
        pad.light.color = [[GCColor alloc] initWithRed:std::clamp(r, 0.f, 1.f)
                                                 green:std::clamp(g, 0.f, 1.f)
                                                  blue:std::clamp(b, 0.f, 1.f)];
        return true;
    }
    return false;
}

bool setExecutableIcon(const std::filesystem::path&, const std::string&, std::string& error) {
    error = "macOS apps get their icon from the bundle (Contents/Resources)";
    return false;
}

// The Dock icon (macOS windows have no icon of their own)
void setWindowIcon(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || !width || !height) return;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                    pixelsWide:(NSInteger)width
                                                                    pixelsHigh:(NSInteger)height
                                                                 bitsPerSample:8
                                                               samplesPerPixel:4
                                                                      hasAlpha:YES
                                                                      isPlanar:NO
                                                                colorSpaceName:NSDeviceRGBColorSpace
                                                                   bytesPerRow:(NSInteger)width * 4
                                                                  bitsPerPixel:32];
    if (!rep) return;
    std::memcpy([rep bitmapData], rgba, (size_t)width * height * 4);
    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [image addRepresentation:rep];
    [NSApp setApplicationIconImage:image];
}

bool closeRequested() { return g_closeRequested; }

void windowSize(uint32_t& width, uint32_t& height) {
    if (!g_window || g_window.miniaturized) {
        width = height = 0;
        return;
    }
    NSSize size = g_view.bounds.size;
    width = (uint32_t)size.width;
    height = (uint32_t)size.height;
    ((CAMetalLayer*)g_view.layer).drawableSize = CGSizeMake(width, height);
}

float dpiScale() { return 1.f; }

const char* vulkanSurfaceExtension() { return VK_EXT_METAL_SURFACE_EXTENSION_NAME; }

VkSurfaceKHR createVulkanSurface(VkInstance instance) {
    VkMetalSurfaceCreateInfoEXT ci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    ci.pLayer = (CAMetalLayer*)g_view.layer;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateMetalSurfaceEXT(instance, &ci, nullptr, &surface) != VK_SUCCESS) return VK_NULL_HANDLE;
    return surface;
}

void imguiInit() { ImGui_ImplOSX_Init(g_view); }

void imguiNewFrame(float) {
    ImGui_ImplOSX_NewFrame(g_view);
    ImGui::GetIO().DisplayFramebufferScale = ImVec2(1.f, 1.f);   // the swapchain is not Retina-scaled (see top)
}

void imguiShutdown() { ImGui_ImplOSX_Shutdown(); }

std::filesystem::path openFileDialog(const char* title, const char* extension) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.message = [NSString stringWithUTF8String:title];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        // Filter by extension (types macOS doesn't know about are skipped: then any file can be picked)
        NSMutableArray<UTType*>* types = [NSMutableArray array];
        for (NSString* ext in [[NSString stringWithUTF8String:extension] componentsSeparatedByString:@","])
            if (UTType* type = [UTType typeWithFilenameExtension:ext]) [types addObject:type];
        if (types.count > 0) panel.allowedContentTypes = types;
        NSModalResponse r = [panel runModal];
        [g_window makeKeyAndOrderFront:nil];
        if (r != NSModalResponseOK || !panel.URL) return {};
        return std::filesystem::u8path(panel.URL.fileSystemRepresentation);
    }
}

std::filesystem::path openFolderDialog(const char* title) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.message = [NSString stringWithUTF8String:title];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.canCreateDirectories = YES;
        panel.allowsMultipleSelection = NO;
        NSModalResponse r = [panel runModal];
        [g_window makeKeyAndOrderFront:nil];
        if (r != NSModalResponseOK || !panel.URL) return {};
        return std::filesystem::u8path(panel.URL.fileSystemRepresentation);
    }
}

static const char* const kOpener = "open";

bool openWithSystem(const std::filesystem::path& file) {
    std::string path = file.u8string();
    char* argv[] = {const_cast<char*>(kOpener), const_cast<char*>(path.c_str()), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, kOpener, nullptr, nullptr, argv, *_NSGetEnviron()) != 0) return false;
    std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();   // (reaped when it's done)
    return true;
}

std::filesystem::path executablePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    std::filesystem::path p = std::filesystem::canonical(buf.c_str(), ec);
    return ec ? std::filesystem::path(buf.c_str()) : p;
}

std::filesystem::path executableDir() {
    std::filesystem::path exe = executablePath();
    return exe.empty() ? std::filesystem::current_path() : exe.parent_path();
}

std::vector<std::filesystem::path> uiFontCandidates() {
    return {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/Library/Fonts/Arial Unicode.ttf"};
}

}  // namespace platform
