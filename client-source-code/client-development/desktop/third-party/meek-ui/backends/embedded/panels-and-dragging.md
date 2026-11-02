# Panels, title bars and mouse dragging

Use one meek-ui scene for multiple panels drawn into the same host target. A 400-by-600 menu in the upper-left corner and a smaller panel elsewhere share the renderer, fonts, styles and input routing. A panel is a node subtree, so its buttons, labels and scrolling content move with it. It does not need a separate operating-system window or graphics device. UI means user interface.

**Current implementation:** positioned panels and their child hit testing work. The existing window and div widgets support scrollbar dragging; neither has a built-in movable title bar or a general `draggable` option. Applications currently implement panel movement in their host input logic. The intended title-bar extension is described below; it is not an existing creation flag.

## Put two panels into an embedded scene

The root window represents the full render target. Keep it transparent and put the visible panels inside it. This markup uses existing widgets:

```xml
<window id="overlay">
    <div id="menu">
        <div id="menu-title"><label text="Menu" /></div>
        <button text="Settings" />
    </div>
    <div id="small-panel">
        <div id="small-title"><label text="Tools" /></div>
        <button text="Action" />
    </div>
</window>
```

CSS (Cascading Style Sheets)-style declarations establish their geometry and title appearance:

```css
#overlay { padding: 0; background-color: transparent; }
#menu {
    position: fixed; left: 0; top: 0;
    width: 400px; height: 600px;
    background-color: #202630; overflow: hidden;
}
#small-panel {
    position: fixed; left: 520px; top: 120px;
    width: 220px; height: 160px;
    background-color: #303844; overflow: hidden;
}
#menu-title, #small-title {
    width: 100%; height: 36px; background-color: #40516a;
}
#menu-title label, #small-title label {
    width: 100%; height: 36px; line-height: 36px;
    text-align: center; color: white;
}
```

This example lays out panels and bars; it does not turn on dragging. Pass the **full target dimensions**, for example 1920 by 1080, to both `embedded_d3d11__begin_frame` and `scene__layout`. The 400-by-600 size belongs to the panel. Passing 400 by 600 to the adapter while drawing into a 1920-by-1080 target changes the rendering coordinate space rather than creating a small independent window. The same distinction applies to Direct3D 9 and the other adapters. Embedded adapters preserve the host image and draw the UI over it.

## Where a mouse event lands

`scene__layout` assigns every node a rectangle in target pixels, including the panel's offset. `scene__hit_test(scene__root(), x, y)` returns the matching node, testing children in paint order, including `z-index`. For ordinary rectangular nodes, the left/top edges are included and the right/bottom edges excluded. Inactive or hidden subtrees are skipped. The selected popup overlay and scrollbar strips have priority.

The Windows helper `embedded_win32__pointer_over_ui` treats a hit on the root itself as host space. A hit on a panel or its child belongs to the UI, including the panel's empty interior. Child transparency alone does not make it click-through. Hit regions are rectangular; transparent image pixels and rounded-corner cutouts are not automatically excluded. The current hit walk also rejects a point outside an ancestor's rectangle, even if visual overflow is allowed. Keep interactive children inside panel bounds or use the existing popup-overlay path.

The Windows window procedure, usually named `WndProc`, must forward input. In your own program, call the helper from your existing procedure; a hook is needed only when integrating with a host whose procedure you intercept. Pass unconsumed messages to the host's original procedure. Forward capture-change, cancel-mode and focus-loss notifications too. The helper now cancels interrupted presses on `WM_CAPTURECHANGED`, `WM_CANCELMODE` and `WM_KILLFOCUS`; cancellation must not generate a button click.

Left-button ownership stays with the side that received the press. A host press cannot be stolen by crossing a panel, and a UI press cannot leak its release to the host. The full renderer target stays unchanged. See [input routing](input-routing.md) for the tested Windows procedure chain and the Linux implementation boundary.

Use one coordinate mapping for both drawing and input. Client coordinates that already match the render target can go directly to the helper. A scaled or offset presentation needs host-side conversion before calling the scene functions. DPI (dots per inch) scaling changes logical style lengths; mouse coordinates still arrive at the scene in target pixels. See [coordinate mapping and thread ownership](README.md#resolution-and-mouse-coordinates-at-the-hook-boundary). A presentation hook and `WndProc` can run on different threads: queue host input to the scene thread rather than touching the scene concurrently.

## One drag controller for all panels

The controller belongs to the application today. Store each panel's position in application state, plus one active-drag record: the selected panel/handle, pointer-to-panel offset, and allowed movement rectangle. A mouse can drag only one panel at a time. The second panel retains its own position while the first moves.

1. On left-button down, hit-test the scene and identify whether the topmost hit is an eligible drag handle or its decorative label. Do not treat a close button or another interactive child as title-background dragging. Select that handle's panel and remember `grab_x = mouse_x - panel_x`, `grab_y = mouse_y - panel_y`.
2. Capture the mouse with the host's input system (`SetCapture` on Windows). Call `scene__cancel_press()` when taking over an existing scene press, so the completed drag cannot also click a control. Give the drag controller ownership of this gesture; avoid running two independent capture/drag paths for the same press.
3. For each captured move, compute `panel_x = mouse_x - grab_x` and `panel_y = mouse_y - grab_y`. Keeping the original grab offset prevents the panel from jumping its corner under the cursor. Continue delivering moves and release outside the panel or host client area while captured.
4. Clamp the position to the chosen bounds, update the stored position and refresh layout before the next draw or hit test. The scene's child rectangles then move with the panel. Raising a panel on press is a separate explicit stacking policy; plain clicking does not automatically change its `z-index`.
5. On release, finish the drag and release only the capture this controller owns. On capture loss, focus loss, cancellation, panel deletion or hot reload, cancel and clear the drag target without clicking. Find nodes again after a tree replacement; old node pointers are invalid.

For a panel that fits entirely inside a target of size `(W,H)`, clamp its top-left position to `0 <= x <= W-panel_width` and `0 <= y <= H-panel_height`. For an allowed rectangle with origin `(L,T)`, add that origin to the corresponding limits. Re-clamp after a resize. When a panel is larger than the target, those upper limits would be negative: choose a policy such as reducing the panel's size, pinning its origin to zero with scrolling content, or keeping only its title bar reachable. The controller must make that choice explicitly.

With the simple fixed-position example above, the host can apply its stored position after `scene__resolve_styles()` and animation updates, before `scene__layout()`:

```c
/* x/y are the stored panel origin in target pixels. The scene scale is positive.
   This panel uses only left/top positioning and has no CSS transform. */
gui_style_t* style = &panel->resolved;
style->inset_l = x / scene__scale();
style->inset_t = y / scene__scale();
style->inset_l_set = TRUE;
style->inset_t_set = TRUE;
scene__layout(target_width, target_height);
```

Reapply those fields every frame: `resolved` is the computed style and will be replaced by subsequent style resolution. Apply every panel's stored position before that frame's layout. A future retained drag API (application programming interface) should handle that bookkeeping. Do not change only the drawn quad; that would leave input rectangles behind. Transformed or rotated drag handles additionally require converting pointer movement through the inverse transform; the simple example deliberately uses untransformed panels.

Drawing the smaller rectangle with raw renderer commands is also possible. Raw primitives have no scene node, hit region, event handler or automatic movement. The host would need to maintain all of those itself. For an interactive rectangle, a styled div keeps geometry, children and input in one scene.

## Intended optional title-bar support

The existing `<window>` widget can gain an optional title bar without introducing another widget type. Creation options should specify whether the bar exists, its height, the title text and its alignment or offset; styling should control the same properties, font, colors and padding. The bar should reserve content space so controls begin below it. The panel's width and height should describe its outer rectangle, including the bar.

A shared retained drag controller should let a window use its generated title bar as the handle, while a div can nominate a handle or allow its plain background to drag. Both need configurable movement bounds and an explicit bring-to-front policy. The controller must preserve ordinary button/scroll interactions, cancel on interrupted capture or tree replacement, and keep layout and hit testing aligned. These options and their public names still need implementation; the markup above does not claim them.

## Verification

`tests/visual/embedded_input_checks.c` runs real Windows mouse-capture notifications with shared scene layout and input. It checks offset and scaled child hit regions, panel boundaries, two panels and overlap order, host-applied movement and resize positions, outside releases, and interrupted capture. Before the fix it reproduced stale capture and an unwanted click after capture loss. Extended checks now verify a real procedure subclass and host/UI ownership across panel boundaries. All 35 assertions pass with clean tracked allocation teardown.

The test uses a hidden Windows window and links the embedded backend but does not initialize a graphics device or compare rendered pixels. It validates the input/layout foundation, not a finished draggable-title-bar feature or an injection into another application. Build and run it with:

```sh
cmake --build build/gcc-debug-d3d11 --target embedded_input_checks
build/gcc-debug-d3d11/embedded_input_checks.exe
```
