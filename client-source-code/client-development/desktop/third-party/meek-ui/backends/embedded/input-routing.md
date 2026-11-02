# Filtering host input for an embedded UI

The embedded renderer draws into the host's full render target. A smaller meek-ui panel only occupies part of that coordinate space. Moving a panel changes its position and child hit rectangles; it does not resize the host target or create another native window. UI means user interface.

Input filtering is separate from drawing. The host asks whether meek-ui consumed an event, then delivers unconsumed events to its normal input path. An event must not activate both a meek-ui button and the application beneath it.

## Implemented Windows behavior

`embedded_win32__on_window_event(hwnd, msg, wparam, lparam)` translates the supported messages and returns `TRUE` when they should be consumed. Its return value is a message-routing decision; a button's C callback does not itself return that decision. Custom-drawn meek-ui buttons produce scene callbacks, not native button `WM_COMMAND` notifications.

The left-button owner is chosen on press and retained for the sequence:

| Event sequence | Result |
|---|---|
| Press outside all panels, move across a button, release over it | All three messages remain with the host; no meek-ui click. |
| Press a meek-ui button and release over it | Meek-ui receives the sequence and invokes its callback; the host receives neither press nor release. |
| Press a panel, move/release outside it | Meek-ui keeps ownership until release; no new click is delivered to the host. |
| UI capture is interrupted | Cancel the scene press without clicking. Consume its eventual release so the host does not receive an unmatched release. |
| Release arrives without an observed press | Forward it to the host, even if the pointer is now over a panel. |
| Left button was already held when filtering began | Movement stays with the host. |

The root represents full-target layout space and does not claim outside clicks. A visible panel's interior does claim them even where it has no button. Idle hover and vertical wheel use the current hit test. A second left-button double-click press follows the same ownership rule as a normal press. Right, middle and extra button messages currently pass through; there is no complete multi-button overlay policy yet. Keyboard/text routing retains the existing `scene__has_char_redirect()` gate and needs host-specific focus policy.

`WM_CAPTURECHANGED`, `WM_CANCELMODE` and `WM_KILLFOCUS` clear an interrupted scene press. They still return `FALSE` so the host also sees the notification. Capture is distinct from a hit test: it keeps a gesture's events routed after the pointer leaves the panel. Cancellation releases native capture only when the helper acquired it. The helper tracks one selected host window/scene; it is not a manager for several native windows.

## Connect it to a Windows window procedure

If you own the window procedure, insert the filter in that procedure and continue your usual processing when it returns `FALSE`. When integrating into an existing same-process window, subclass its procedure: `SetWindowLongPtrW` with `GWLP_WNDPROC` installs the replacement and returns the previous procedure. Preserve that value for this exact window. [Microsoft: SetWindowLongPtrW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowlongptrw)

This caller-owned fragment illustrates a single selected window. Installation, removal and scene calls must happen on the owning scene/window thread. `input_ready` becomes true only after initialization and layout are ready:

```c
#include "embedded_win32_input.h"

static WNDPROC previous_wndproc;
static boole input_ready;

static LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* Keep a local copy because forwarding can re-enter application code. */
    WNDPROC next = previous_wndproc;
    if (input_ready && embedded_win32__on_window_event(hwnd, msg, wp, lp))
    {
        return 0; /* A supported input message was consumed. */
    }
    return CallWindowProcW(next, hwnd, msg, wp, lp);
}

/* During installation, on the selected window's thread:
   hwnd is valid, belongs to this process, and is not already hooked by us. */
SetLastError(ERROR_SUCCESS);
LONG_PTR old_proc = SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                    (LONG_PTR)overlay_wndproc);
if (old_proc == 0 && GetLastError() != ERROR_SUCCESS)
{
    /* Installation failed: report the error and leave input_ready false. */
}
else
{
    previous_wndproc = (WNDPROC)old_proc;
    /* Set input_ready when the scene is initialized and laid out. */
}
```

Use `CallWindowProcW` to forward the original arguments and return its result. Calling `DefWindowProcW` here would skip the host's application logic. Directly calling the saved pointer is also not the Windows subclass contract. [Microsoft: CallWindowProcW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-callwindowprocw)

The fragment is a same-thread integration recipe, not an automatic hook installer. On removal, cancel any active UI press while the scene is still alive, disable filtering, restore the saved procedure, then tear down the overlay. Check that `GetWindowLongPtrW(hwnd, GWLP_WNDPROC)` still names your procedure before replacing it: another component may have installed a subclass above yours. Coordinate removal of that chain instead of overwriting it. Do not unload a DLL (dynamic-link library) while a procedure in the chain still points into it. Forward destruction notifications, including `WM_NCDESTROY`; do graphics teardown at the graphics lifecycle boundary rather than assuming the window callback has a valid graphics context.

If the window procedure and presentation hook run on different threads, this direct synchronous recipe does not apply. Queue events through the host's existing mechanism and process scene input on the drawing thread. The window thread still needs an immediate consumption policy based on a safely published interaction snapshot or ownership state. The current helper provides no cross-thread queue or snapshot; do not call the mutable scene simultaneously from both threads. Input coordinates must be mapped to the same full-target pixels used by layout; see [coordinate conversion](README.md#resolution-and-mouse-coordinates-at-the-hook-boundary).

Raw input and polling are separate paths. Blocking `WM_LBUTTONDOWN` does not suppress an application's raw-device events, direct key/button polling or game input library. A host using those needs equivalent filtering where it dispatches that input. This helper handles ordinary window messages.

## Linux: dispatch events before the host handles them

Linux does not have one universal `WndProc` equivalent. X11 (X Window System, version 11), Wayland and host toolkits expose different event mechanisms. The graphics choice, such as Vulkan or OpenGL (Open Graphics Library), does not determine which input system the application uses.

An X11 host commonly reads an event through Xlib's `XNextEvent`, then dispatches it to application code. Put a consumption decision at that dispatch boundary. Preserve the same press owner through movement and release. Pointer grabs keep the release associated with the pressed client; respect the host's existing grab instead of installing a competing event reader or unconditionally replacing its input selection. [X.Org: Xlib event handling](https://www.x.org/releases/X11R7.5/doc/libX11/libX11.html), [X.Org: pointer grabs](https://www.x.org/Development/Documentation/GrabProcessing/)

Conceptually, the host's existing loop should do this (the names below are caller-defined pseudocode, not meek-ui functions):

```text
event = host.next_event()
consumed = overlay.translate_and_filter(event)
if not consumed:
    host.handle_event(event)
```

Map motion and button events to `scene__on_mouse_move` and `scene__on_mouse_button`. Core X11 wheel events commonly use buttons 4 and 5; map them to `scene__on_mouse_wheel`. Cancel an interrupted press with `scene__cancel_press`. Use the host's input method for composed text. Keep coordinates relative to the selected host target, including any scaling or presentation offset.

The current [X11 example translator](../../examples/embedded_common/embedded_x11_input.h) is demonstration code: it returns `void` and forwards input unconditionally. It is **not** the consumed/unconsumed overlay filter described above and has not gained parity with the Windows helper in this change. Production X11 filtering, retained ownership and cancellation need an adapter around the host's dispatcher. Do not copy that example unchanged and claim that underlying application clicks are protected.

Wayland sends pointer events through listeners associated with the host connection and surface. Insert the same consumption decision into the host's pointer dispatch, respecting surface coordinates and the compositor's implicit button grab. Keep events in their defined order. There is no Windows-style procedure pointer to replace. Host toolkit dispatch may be the appropriate integration point. [Wayland pointer protocol](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_pointer)

No Wayland embedded input-filter adapter is supplied here. These Linux sections describe the integration boundaries and the remaining implementation work; the Windows test results below do not validate Linux input filtering.

## Checks run

`tests/visual/embedded_input_checks.c` now has 35 passing assertions. It installs a real `GWLP_WNDPROC` subclass on a hidden test window, verifies that host messages and their return values are preserved, checks that UI button clicks never reach the original procedure, then restores that procedure. It also covers ownership across panel boundaries, cancellation, double-click pairing and the existing two-panel geometry checks. Tracked teardown is clean.

These are input/layout checks using a real Windows message chain, without a graphics device. General draggable title bars remain separate work; see [panels and dragging](panels-and-dragging.md).
