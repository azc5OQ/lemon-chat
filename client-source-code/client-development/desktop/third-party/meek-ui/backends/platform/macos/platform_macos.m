//
// platforms/macos/platform_macos.m - macOS Cocoa + OpenGL backend.
//
// Desktop macOS host for the gui library. Drives:
//   - NSApplication + NSWindow + a custom NSView subclass for events
//   - NSOpenGLContext with a 4.1 Core Profile (last version Apple
//     supports; maps to GLSL 410 in the renderer's shaders)
//   - gles3_renderer.c for the actual draw calls (context-neutral,
//     same renderer the Android / DRM / X11 backends use -- Apple
//     branch in the renderer swaps the shader version preamble)
//
// Runs on both Intel and Apple Silicon. OpenGL is deprecated on
// macOS but functional; shipping-quality macOS support would move
// to Metal, which remains a separate future backend.
//
// FILE EXTENSION: .m (Objective-C) because Cocoa APIs are
// Objective-C. The C sources in gui/src compile as plain C; only
// this TU sees Objective-C syntax. Pairs with:
//   - platforms/macos/fs_macos.c     POSIX file I/O
//
// DEPENDENCIES (link order in build.sh):
//   -framework Cocoa -framework OpenGL -framework CoreVideo
//   -framework Foundation
//   -lm -ldl -lpthread
//
// APPROACH:
//
// The run loop is CFRunLoop underneath, but we don't use
// `[NSApp run]` (which would block and never return). Instead we
// poll events ourselves each tick via
// `[NSApp nextEventMatchingMask:... untilDate:[NSDate distantPast]]`
// with a distant-past timeout, then render one frame. This matches
// the Win32 platform's `PeekMessage / Translate / Dispatch` loop:
// the host's main() drives the cadence, not the OS.
//

#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>
#import <OpenGL/OpenGL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "animator.h"
#include "renderer.h"
#include "widget_registry.h"
#include "widgets/widget_image_cache.h"
#include "font.h"
#include "fs.h"
#include "clib/memory_manager.h"
#include "third_party/log.h"

#define _PLATFORM_INTERNAL
#include "platform.h"
#undef _PLATFORM_INTERNAL

//============================================================================
// custom NSView that owns the event callbacks
//============================================================================
//
// NSOpenGLView is legacy but still supported; subclassing it gets
// us acceptReplaceInputMethod and openGLContext handling for free.
// The key virtual methods we override are the mouse / keyboard
// callbacks plus reshape + drawRect.
//
// Coordinate convention: Cocoa gives event positions in a bottom-
// left-origin flipped coordinate system relative to the view. We
// convert to top-left-origin (what scene expects) by subtracting
// y from view.bounds.height before each dispatch.
//

@interface GuiMacView : NSOpenGLView
- (void)recomputeViewport;
- (int64)ptrX;
- (int64)ptrY;
@end

static GuiMacView*    g_view              = nil;
static NSWindow*      g_window            = nil;
// the window was created hidden (gui_app_config_t.hidden): never ordered front
static boole          g_hidden            = FALSE;
static boole          g_should_close      = FALSE;
static gui_color      g_clear_color       = (gui_color){ 0.0f, 0.0f, 0.0f, 1.0f };
static int64          g_viewport_w        = 0;
static int64          g_viewport_h        = 0;
static int64          g_ptr_x             = 0;
static int64          g_ptr_y             = 0;

//
// Translate a Cocoa keycode (kVK_* from HIToolbox/Events.h) into a
// Win32 virtual-key code. Same mapping scene.c expects from every
// platform, so the widget input code needs no per-OS branches.
//
static int64 _platform_macos_internal__keycode_to_vk(unsigned short keycode)
{
    switch (keycode)
    {
        case 0x33: return 0x08;   // delete (backspace)
        case 0x30: return 0x09;   // tab
        case 0x24: return 0x0D;   // return
        case 0x35: return 0x1B;   // escape
        case 0x31: return 0x20;   // space
        case 0x7B: return 0x25;   // left arrow
        case 0x7E: return 0x26;   // up arrow
        case 0x7C: return 0x27;   // right arrow
        case 0x7D: return 0x28;   // down arrow
        case 0x75: return 0x2E;   // forward delete
        case 0x73: return 0x24;   // home
        case 0x77: return 0x23;   // end
        case 0x74: return 0x21;   // page up
        case 0x79: return 0x22;   // page down
        default:   return 0;
    }
}

@implementation GuiMacView

- (BOOL)acceptsFirstResponder         { return YES; }
- (BOOL)becomeFirstResponder          { return YES; }
- (BOOL)isFlipped                     { return YES; }  // use top-left-origin coords in drawRect etc.

- (void)recomputeViewport
{
    NSSize sz = [self convertSizeToBacking:[self bounds].size];
    g_viewport_w = (int64)sz.width;
    g_viewport_h = (int64)sz.height;
}

- (int64)ptrX { return g_ptr_x; }
- (int64)ptrY { return g_ptr_y; }

//
// Convert an NSEvent's locationInWindow into top-left-origin view
// coordinates in backing-store pixels. Retina displays have a 2x
// backing scale, so `convertPointToBacking` is essential -- we
// want to pass pixel coords to the scene, not points.
//
- (NSPoint)cocoaEventToScenePoint:(NSEvent*)event
{
    NSPoint inWin = [event locationInWindow];
    NSPoint inView = [self convertPoint:inWin fromView:nil];
    NSPoint inBacking = [self convertPointToBacking:inView];
    //
    // convertPointToBacking inverts y for non-flipped views. We
    // declared isFlipped=YES so y is already top-origin at the
    // point level; the backing conversion only scales the numbers
    // and doesn't re-flip. Backing pixels match the GL viewport
    // exactly (we set the viewport from convertSizeToBacking).
    //
    return inBacking;
}

- (void)updateTrackingAreas
{
    //
    // Remove any previously installed tracking area and re-add one
    // covering the current bounds. NSTrackingInVisibleRect would be
    // simpler but doesn't fire mouseEntered when the view is first
    // mapped, so we do it manually.
    //
    for (NSTrackingArea* area in [self trackingAreas])
    {
        [self removeTrackingArea:area];
    }
    NSTrackingAreaOptions opts = NSTrackingMouseMoved |
                                 NSTrackingActiveAlways |
                                 NSTrackingInVisibleRect;
    NSTrackingArea* area = [[NSTrackingArea alloc]
        initWithRect:[self bounds] options:opts owner:self userInfo:nil];
    [self addTrackingArea:area];
}

- (void)mouseMoved:(NSEvent*)event
{
    NSPoint p = [self cocoaEventToScenePoint:event];
    g_ptr_x = (int64)p.x;
    g_ptr_y = (int64)p.y;
    scene__on_mouse_move(g_ptr_x, g_ptr_y);
}

- (void)mouseDragged:(NSEvent*)event          { [self mouseMoved:event]; }
- (void)rightMouseDragged:(NSEvent*)event     { [self mouseMoved:event]; }
- (void)otherMouseDragged:(NSEvent*)event     { [self mouseMoved:event]; }

- (void)mouseDown:(NSEvent*)event
{
    NSPoint p = [self cocoaEventToScenePoint:event];
    g_ptr_x = (int64)p.x;
    g_ptr_y = (int64)p.y;
    scene__on_mouse_button(0, TRUE, g_ptr_x, g_ptr_y);
}
- (void)mouseUp:(NSEvent*)event
{
    NSPoint p = [self cocoaEventToScenePoint:event];
    g_ptr_x = (int64)p.x;
    g_ptr_y = (int64)p.y;
    scene__on_mouse_button(0, FALSE, g_ptr_x, g_ptr_y);
}
- (void)rightMouseDown:(NSEvent*)event
{
    NSPoint p = [self cocoaEventToScenePoint:event];
    scene__on_mouse_button(1, TRUE, (int64)p.x, (int64)p.y);
}
- (void)rightMouseUp:(NSEvent*)event
{
    NSPoint p = [self cocoaEventToScenePoint:event];
    scene__on_mouse_button(1, FALSE, (int64)p.x, (int64)p.y);
}
- (void)otherMouseDown:(NSEvent*)event
{
    NSPoint p = [self cocoaEventToScenePoint:event];
    scene__on_mouse_button(2, TRUE, (int64)p.x, (int64)p.y);
}
- (void)otherMouseUp:(NSEvent*)event
{
    NSPoint p = [self cocoaEventToScenePoint:event];
    scene__on_mouse_button(2, FALSE, (int64)p.x, (int64)p.y);
}

- (void)scrollWheel:(NSEvent*)event
{
    //
    // [event scrollingDeltaY] on modern macOS gives line counts for
    // discrete mice + pixel-precise trackpad values. Our scene's
    // wheel API takes a normalized "ticks" delta, which for
    // trackpads means ~fractional values -- the smooth-scroll
    // animator handles that fine.
    //
    NSPoint p = [self cocoaEventToScenePoint:event];
    CGFloat dy = [event scrollingDeltaY];
    if ([event hasPreciseScrollingDeltas])
    {
        //
        // Precise (trackpad) events arrive as pixel-scale numbers
        // like +40 / -40; normalize to the "one tick per 40px"
        // convention the rest of the codebase uses for trackpad
        // input.
        //
        dy /= 40.0;
    }
    scene__on_mouse_wheel((int64)p.x, (int64)p.y, (float)dy);
}

- (void)keyDown:(NSEvent*)event
{
    //
    // Two dispatches per key: VK code through scene__on_key for
    // nav keys, characters through scene__on_char for the
    // characters macOS's key-equivalence layer produced (includes
    // compose sequences like opt-e, e -> é).
    //
    int64 vk = _platform_macos_internal__keycode_to_vk([event keyCode]);
    if (vk != 0)
    {
        scene__on_key(vk, TRUE);
    }

    NSString* chars = [event charactersIgnoringModifiers];
    NSUInteger len  = [chars length];
    for (NSUInteger i = 0; i < len; i++)
    {
        unichar c = [chars characterAtIndex:i];
        //
        // Skip control chars (BACKSPACE=0x08, TAB=0x09, RETURN=0x0D,
        // ESC=0x1B, DELETE=0x7F) -- scene already saw these through
        // the VK path; they shouldn't also land as characters.
        //
        if (c < 0x20 || c == 0x7F) { continue; }
        //
        // Our scene on_char pipeline handles ASCII + Latin-1. Pass
        // the codepoint through as-is; widget_input filters it if
        // it can't render the glyph (same limit as the X11 + DRM
        // backends).
        //
        scene__on_char((uint)c);
    }
}

- (void)keyUp:(NSEvent*)event
{
    int64 vk = _platform_macos_internal__keycode_to_vk([event keyCode]);
    if (vk != 0)
    {
        scene__on_key(vk, FALSE);
    }
}

- (void)reshape
{
    [super reshape];
    [[self openGLContext] makeCurrentContext];
    [self recomputeViewport];
    scene__on_resize(g_viewport_w, g_viewport_h);
}

@end

//============================================================================
// NSWindowDelegate -- handles close + resize
//============================================================================

@interface GuiMacWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation GuiMacWindowDelegate

- (BOOL)windowShouldClose:(id)sender
{
    (void)sender;
    g_should_close = TRUE;
    //
    // Return NO so Cocoa doesn't actually dispose the window right
    // away -- platform__shutdown will tear it down explicitly when
    // the main loop exits. This matches how Windows' WM_CLOSE is
    // handled: set a flag, let the host-side loop end cleanly.
    //
    return NO;
}

- (void)windowDidResize:(NSNotification*)notification
{
    (void)notification;
    if (g_view != nil)
    {
        [g_view recomputeViewport];
        scene__on_resize(g_viewport_w, g_viewport_h);
    }
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification
{
    //
    // Fired by Cocoa when the window's backingScaleFactor changes
    // -- typically because the user dragged it from a 1x display
    // to a 2x Retina (or vice versa). Re-pick scale so widget
    // sizes + font rasterizations match the new pixel density.
    // The font cache will lazily rasterize new atlases at the
    // first font__at call after this; the LRU evicts old-size
    // atlases naturally as the cache fills.
    //
    (void)notification;
    if (g_window != nil)
    {
        CGFloat s = [g_window backingScaleFactor];
        if (s < 1.0) { s = 1.0; }
        if (s > 4.0) { s = 4.0; }
        scene__set_scale((float)s);
        if (g_view != nil)
        {
            [g_view recomputeViewport];
            scene__on_resize(g_viewport_w, g_viewport_h);
        }
        log_info("ui_scale: backing factor changed -> %.2f", (double)s);
    }
}

@end

static GuiMacWindowDelegate* g_window_delegate = nil;

//============================================================================
// UI scale from backing-store factor
//============================================================================

static float _platform_macos_internal__pick_ui_scale(void)
{
    if (g_window == nil) { return 1.0f; }

    //
    // NSWindow.backingScaleFactor is exactly the thing we want: 1.0
    // on non-Retina displays, 2.0 on standard Retina, 3.0 on Pro
    // Display XDR. Matches what the Windows port derives from DPI
    // and what the Android port derives from density buckets --
    // one logical pt = `scale` physical pixels.
    //
    CGFloat s = [g_window backingScaleFactor];
    if (s < 1.0) { s = 1.0; }
    if (s > 4.0) { s = 4.0; }
    log_info("ui_scale: backingScaleFactor=%.2f", (double)s);
    return (float)s;
}

//============================================================================
// host symbol resolver (dlsym fallback)
//============================================================================

static gui_handler_fn _platform_macos_internal__resolve_host_symbol(char* name)
{
    if (name == NULL || name[0] == 0) { return NULL; }
    void* sym = dlsym(RTLD_DEFAULT, name);
    return (gui_handler_fn)sym;
}

//============================================================================
// public API
//============================================================================

boole platform__init(const gui_app_config* cfg)
{
    memory_manager__init();

    if (cfg == NULL)
    {
        log_error("platform__init: cfg is NULL");
        return FALSE;
    }

    @autoreleasepool
    {
        g_clear_color = cfg->clear_color;
        g_hidden      = cfg->hidden;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        int w = (int)(cfg->width  > 0 ? cfg->width  : 1280);
        int h = (int)(cfg->height > 0 ? cfg->height : 800);

        NSRect frame = NSMakeRect(0, 0, w, h);
        NSWindowStyleMask style = NSWindowStyleMaskTitled |
                                  NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskResizable |
                                  NSWindowStyleMaskMiniaturizable;

        g_window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:style
                        backing:NSBackingStoreBuffered
                          defer:NO];
        if (g_window == nil)
        {
            log_error("NSWindow allocation failed");
            //
            // No window resources to release here -- the alloc failed
            // before we touched delegate / view / context.
            //
            memory_manager__shutdown();
            return FALSE;
        }

        //
        // Title. Cocoa's NSString is UTF-8; our cfg->title is
        // wchar_t* because the Windows platform wants it -- convert
        // via a char[] round-trip (naive ASCII truncation, same as
        // the X11 path). Proper UTF-8 handling would use wcstombs.
        //
        if (cfg->title != NULL)
        {
            char ascii[256];
            size_t i = 0;
            for (; i < sizeof(ascii) - 1 && cfg->title[i] != 0; i++)
            {
                wchar_t c = cfg->title[i];
                ascii[i] = (c < 0x80) ? (char)c : '?';
            }
            ascii[i] = 0;
            NSString* nstitle = [NSString stringWithUTF8String:ascii];
            if (nstitle != nil) { [g_window setTitle:nstitle]; }
        }

        g_window_delegate = [[GuiMacWindowDelegate alloc] init];
        [g_window setDelegate:g_window_delegate];

        //
        // Pixel format: 4.1 Core Profile (the last version macOS
        // supports). Maps to GLSL 410 in the renderer. RGBA8,
        // double-buffered, no depth/stencil (we draw 2D only).
        //
        NSOpenGLPixelFormatAttribute attrs[] = {
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
            NSOpenGLPFAColorSize, 32,
            NSOpenGLPFAAlphaSize, 8,
            NSOpenGLPFADoubleBuffer,
            NSOpenGLPFAAccelerated,
            0
        };
        NSOpenGLPixelFormat* pf = [[NSOpenGLPixelFormat alloc]
            initWithAttributes:attrs];
        if (pf == nil)
        {
            log_error("NSOpenGLPixelFormat: no 4.1 Core Profile available");
            //
            // Window + delegate exist; release them via ARC before
            // bailing so the failed launch doesn't leak Cocoa state.
            //
            [g_window setDelegate:nil];
            [g_window close];
            g_window          = nil;
            g_window_delegate = nil;
            memory_manager__shutdown();
            return FALSE;
        }

        g_view = [[GuiMacView alloc]
            initWithFrame:frame pixelFormat:pf];
        [g_window setContentView:g_view];
        [g_window makeFirstResponder:g_view];
        [g_window center];

        // a hidden window is never ordered front (gui_app_config_t.hidden);
        // the GL view still has its drawable, so frames render and read
        // back for the test runner without anything appearing on screen
        if (g_hidden == FALSE)
        {
            [g_window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }

        //
        // Make the GL context current, then pin swap interval to 1
        // so present is vblank-aligned. CGLSetParameter with
        // kCGLCPSwapInterval is the macOS equivalent of
        // eglSwapInterval / wglSwapIntervalEXT.
        //
        [[g_view openGLContext] makeCurrentContext];
        GLint sync = 1;
        [[g_view openGLContext] setValues:&sync
                             forParameter:NSOpenGLContextParameterSwapInterval];

        [g_view recomputeViewport];
    }

    if (!renderer__init(NULL))
    {
        log_error("renderer__init failed");
        //
        // Drop window + view + delegate too. ARC handles the actual
        // dealloc; nilling the globals releases the strong refs.
        //
        [g_window setDelegate:nil];
        [g_window close];
        g_view            = nil;
        g_window          = nil;
        g_window_delegate = nil;
        renderer__shutdown();
        memory_manager__shutdown();
        return FALSE;
    }

    widget_registry__bootstrap_builtins();
    if (!font__init())
    {
        log_error("font__init failed");
        renderer__shutdown();
        [g_window setDelegate:nil];
        [g_window close];
        g_view            = nil;
        g_window          = nil;
        g_window_delegate = nil;
        memory_manager__shutdown();
        return FALSE;
    }

    scene__set_symbol_resolver(_platform_macos_internal__resolve_host_symbol);

    float scale = _platform_macos_internal__pick_ui_scale();
    if (scale > 0.0f)
    {
        scene__set_scale(scale);
    }

    log_info("platform_macos: up (%lld x %lld)",
             (long long)g_viewport_w, (long long)g_viewport_h);
    return TRUE;
}

boole platform__tick(void)
{
    if (g_should_close) { return FALSE; }

    @autoreleasepool
    {
        //
        // Drain the event queue with a zero timeout. [NSDate
        // distantPast] is the canonical "don't block" value. Every
        // event that's ready gets dispatched via sendEvent -- which
        // funnels mouse / keyboard events to our view, window
        // events (resize, close) to the delegate.
        //
        for (;;)
        {
            NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:[NSDate distantPast]
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES];
            if (ev == nil) { break; }
            [NSApp sendEvent:ev];
            [NSApp updateWindows];
        }

        if (g_should_close) { return FALSE; }

        //
        // Frame timestamp. clock_gettime(CLOCK_MONOTONIC) is the
        // portable POSIX monotonic; macOS implements it in terms of
        // mach_absolute_time underneath.
        //
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int64 now_ms = (int64)ts.tv_sec * 1000 + (int64)(ts.tv_nsec / 1000000);
            scene__begin_frame_time(now_ms);
        }

        [[g_view openGLContext] makeCurrentContext];

        scene__resolve_styles();
        animator__tick();
        scene__layout(g_viewport_w, g_viewport_h);

        renderer__begin_frame(g_viewport_w, g_viewport_h, g_clear_color);
        scene__emit_draws();
        renderer__end_frame();

        //
        // Present. flushBuffer is macOS's equivalent of SwapBuffers
        // / eglSwapBuffers.
        //
        [[g_view openGLContext] flushBuffer];
    }

    return !g_should_close;
}

void platform__shutdown(void)
{
    //
    // Image cache + font subsystem release renderer-owned textures,
    // so they have to run while the renderer is still alive.
    //
    widget_image__cache_shutdown();
    font__shutdown();
    renderer__shutdown();

    @autoreleasepool
    {
        if (g_window != nil)
        {
            [g_window setDelegate:nil];
            [g_window orderOut:nil];
            [g_window close];
            g_window = nil;
        }
        g_view = nil;
        g_window_delegate = nil;
    }

    memory_manager__shutdown();
}

//
// Capture API: stubbed on macOS. Visual-test runner is Windows-only
// for now.
//
boole platform__capture_bmp(const char* path)
{
    (void)path;
    return FALSE;
}

void platform__set_topmost(void) { /* macOS: would use setLevel: NSFloatingWindowLevel (and nothing for a hidden window). Skipped for now. */ }

//============================================================================
// entry-point trampoline
//============================================================================
//
// Same pattern as Linux DRM + X11: platform.h renamed the host's
// int main() to app_main(); we provide the real main() here
// (guarded from the rename by _PLATFORM_INTERNAL on include) and
// forward. macOS single-binary build, no library/host split.
//

#include "_main_trampoline.h"
GUI_DEFINE_MAIN_TRAMPOLINE()
