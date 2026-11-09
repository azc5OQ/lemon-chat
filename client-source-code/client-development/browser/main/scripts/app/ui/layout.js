// layout.js is embedded in template.html along with the other client files
// it is the desktop grid layout engine: column order and widths, the chat-input row and the edit mode,
// persisted in localStorage under lemon_layout; touch devices keep the legacy layout and skip it
// main.js wires it at load, ui.js calls it when panels are toggled; the state is g_layout in globals.js

var layout_edit_drag = null;
var layout_edit_animations = [];

/**
 * @brief restores g_layout.state from localStorage "lemon_layout" when the saved shape is valid
 *        snaps a collapsed info column (saved before the 90px minimum) back to its default width
 *
 * @return void
 */
function layout__layout_load_saved_state()
{
    try
    {
        let raw = utils__storage_get("lemon_layout");
        if (raw == null) { return; }
        let saved = JSON.parse(raw);
        if (saved != null && Array.isArray(saved.order) && saved.order.length == 3
            && saved.order.indexOf("channels") != -1 && saved.order.indexOf("chat") != -1
            && saved.order.indexOf("info") != -1 && ["channels", "chat", "info"].indexOf(saved.input_col) != -1
            && ["top", "bottom"].indexOf(saved.input_pos) != -1)
        {
            // a collapsed info column (saved before the 90px minimum existed) looks like
            // the panel was deleted - snap it back to the default width
            if (parseInt(saved.col_info, 10) < 90) { saved.col_info = "13%"; }
            if (!["full", "channels", "chat", "info"].includes(saved.toolbar_col))
            {
                saved.toolbar_col = "full";
            }
            if (!["top", "bottom"].includes(saved.toolbar_pos))
            {
                saved.toolbar_pos = "top";
            }
            g_layout.state = saved;
        }
    }
    catch (e) { console.warn("layout state restore failed:", e.message); }
}

/**
 * @brief persists g_layout.state to localStorage under "lemon_layout"
 *
 * @return void
 */
function layout__layout_save_state()
{
    utils__storage_set("lemon_layout", JSON.stringify(g_layout.state));
}

/**
 * @brief (re)builds the grid templates from g_layout.state and g_is_chat_hidden and assigns the panels to their areas
 *        does not touch the container's display; connect/disconnect owns that
 *
 * @return void
 */
function layout__layout_apply()
{
    if (layout_edit_drag != null && layout_edit_drag.applying == false)
    {
        _internal__layout_edit_finish(false, false);
    }
    if (g_layout.grid_active == false) { return; }

    let container = document.getElementById("communication-system-container");

    let head_menu = document.getElementById("communication-system-head-menu");
    let connected = getComputedStyle(container).display === "grid";
    let toolbar_column = g_layout.state.toolbar_col || "full";
    let toolbar_position = g_layout.state.toolbar_pos || "top";
    let rows = null;
    let heights = null;
    let toolbar_row = null;

    // Keep connection and local-settings buttons available while the chat is hidden after disconnecting.
    if (connected)
    {
        if (head_menu.parentElement !== container)
        {
            container.prepend(head_menu);
        }
        container.style.height = "100vh";
        head_menu.dataset.layoutToolbar = toolbar_position;
    }
    else
    {
        if (head_menu.parentElement === container)
        {
            container.parentElement.insertBefore(head_menu, container);
        }
        delete head_menu.dataset.layoutToolbar;
        container.style.height = "calc(100vh - " + head_menu.offsetHeight + "px)";
    }

    let order = g_layout.state.order.slice();
    if (g_is_chat_hidden == true)
    {
        order = order.filter(function(col) { return col != "chat"; });
    }

    // The message box shares the width of whichever column it is attached to.
    let row_a = [], row_b = [];
    for (let i = 0; i < order.length; i++)
    {
        let col = order[i];
        let holds_input = (g_is_chat_hidden == false && g_layout.state.input_col == col);
        row_a.push((holds_input && g_layout.state.input_pos == "top") ? "input" : col);
        row_b.push((holds_input && g_layout.state.input_pos == "bottom") ? "input" : col);
    }

    let widths = order.map(function(col) {
        if (col == "channels") { return "minmax(150px, " + g_layout.state.col_channels + ")"; }
        if (col == "info") { return "minmax(90px, " + g_layout.state.col_info + ")"; } // min 90: collapsing to 0 made the panel look deleted
        return "minmax(330px, 1fr)"; // chat soaks up the leftover space
    });
    if (g_is_chat_hidden == true && order.indexOf("channels") != -1)
    {
        widths[order.indexOf("channels")] = "minmax(150px, 1fr)"; // chat gone: channels takes the space
    }

    rows = [row_a, row_b];
    heights = g_layout.state.input_pos === "top" ? ["auto", "minmax(0, 1fr)"] : ["minmax(0, 1fr)", "auto"];
    if (connected)
    {
        // Extending the adjacent row keeps the other panels rectangular when the toolbar shares a column.
        toolbar_row = (toolbar_position === "top" ? row_a : row_b).slice();
        if (!order.includes(toolbar_column))
        {
            toolbar_row.fill("toolbar");
        }
        else
        {
            toolbar_row[order.indexOf(toolbar_column)] = "toolbar";
        }
        if (toolbar_position === "top")
        {
            rows.unshift(toolbar_row);
            heights.unshift("auto");
        }
        else
        {
            rows.push(toolbar_row);
            heights.push("auto");
        }
    }
    container.style.gridTemplateAreas = rows.map(row => '"' + row.join(" ") + '"').join(" ");
    container.style.gridTemplateColumns = widths.join(" ");
    container.style.gridTemplateRows = heights.join(" ");

    g_layout.panels.channels.style.gridArea = "channels";
    g_layout.panels.chat.style.gridArea = "chat";
    g_layout.panels.info.style.gridArea = "info";
    g_layout.panels.input.style.gridArea = "input";
    head_menu.style.gridArea = "toolbar";

    // neutralize the legacy inline-block sizing: the grid alone decides the geometry
    let names = ["channels", "chat", "info", "input"];
    for (let i = 0; i < names.length; i++)
    {
        let panel = g_layout.panels[names[i]];
        panel.style.width = "auto";
        panel.style.minWidth = "0";
        panel.style.height = "auto";
        panel.style.left = "0px";
    }

    // no left inset here: the input is in the chat column, so the corner mic button is
    // outside it and the composer starts flush with the chat panel's edge
    g_layout.panels.input.style.paddingLeft = "";

    document.getElementById("space-devider3").style.display = "none"; // legacy spacer row, obsolete in the grid

    g_layout.panels.chat.style.display = (g_is_chat_hidden == true) ? "none" : "block";
    g_layout.panels.input.style.display = (g_is_chat_hidden == true) ? "none" : "block";
}

// ---- column-width dragging (the 2px line at the chat panel's left edge, and the
// ---- matching handle at the info panel's left edge) ----

/**
 * @brief starts a column-width drag on a panel's left handle: the boundary to its left neighbour moves
 *        the elastic chat column (1fr) has no stored width, so adjusting only the other column moves the same boundary
 *
 * @param object e -> the mouse event
 * @param string panel_name -> "channels", "chat" or "info"
 *
 * @return void
 */
function layout__layout_column_drag_start(e, panel_name)
{
    if (g_layout.grid_active == false || g_layout.edit_active == true) { return; }

    let order = g_layout.state.order;
    let idx = order.indexOf(panel_name);
    if (idx == -1 || idx == 0) { return; } // leftmost: no boundary to its left

    let neighbour = order[idx - 1];
    let width_of = function(name) { return Math.round(g_layout.panels[name].getBoundingClientRect().width); };

    // dx limits so no column in the pair leaves its minimum (channels 150px, info 0px);
    // the chat minimum (330px) is guarded by the single-target max computed in the move handler
    let dx_min = -Infinity, dx_max = Infinity;
    let targets = [];
    if (neighbour != "chat")
    {
        let mn = (neighbour == "channels") ? 150 : 90;
        targets.push({ name: neighbour, sign: 1, start: width_of(neighbour) });
        dx_min = Math.max(dx_min, mn - width_of(neighbour));
    }
    if (panel_name != "chat")
    {
        let mn = (panel_name == "channels") ? 150 : 90;
        targets.push({ name: panel_name, sign: -1, start: width_of(panel_name) });
        dx_max = Math.min(dx_max, width_of(panel_name) - mn);
    }

    g_layout.drag = {
        targets: targets,
        pair_has_chat: (neighbour == "chat" || panel_name == "chat"),
        dx_min: dx_min,
        dx_max: dx_max,
        start_x: e.clientX
    };
    document.documentElement.addEventListener("mousemove", layout__layout_column_drag_move, false);
    document.documentElement.addEventListener("mouseup", layout__layout_column_drag_stop, false);
    e.preventDefault();
}

/**
 * @brief the live column drag: clamps dx to the stored limits (plus the chat 330px minimum when chat is the elastic side), writes the new widths into g_layout.state and re-applies the grid
 *
 * @param object e -> the mouse event
 *
 * @return void
 */
function layout__layout_column_drag_move(e)
{
    if (g_layout.drag == null) { return; }

    let dx = e.clientX - g_layout.drag.start_x;

    if (g_layout.drag.pair_has_chat == true && g_layout.drag.targets.length == 1)
    {
        // chat is the elastic side of the pair: cap the single target so chat keeps >= 330px
        let container = document.getElementById("communication-system-container");
        let total = container.getBoundingClientRect().width;
        let t = g_layout.drag.targets[0];
        let other = (t.name == "channels") ? "info" : "channels";
        let other_width = Math.round(g_layout.panels[other].getBoundingClientRect().width);
        let max_width = total - other_width - ((g_is_chat_hidden == false) ? 330 : 0) - 8;
        if (t.sign == 1) { dx = Math.min(dx, max_width - t.start); }
        else { dx = Math.max(dx, t.start - max_width); }
    }

    if (dx < g_layout.drag.dx_min) { dx = g_layout.drag.dx_min; }
    if (dx > g_layout.drag.dx_max) { dx = g_layout.drag.dx_max; }

    for (let i = 0; i < g_layout.drag.targets.length; i++)
    {
        let t = g_layout.drag.targets[i];
        let new_width = t.start + t.sign * dx;
        if (t.name == "channels") { g_layout.state.col_channels = new_width + "px"; }
        else { g_layout.state.col_info = new_width + "px"; }
    }

    layout__layout_apply();
}

/**
 * @brief ends a column drag: persists the widths once and removes the document listeners
 *
 * @return void
 */
function layout__layout_column_drag_stop()
{
    if (g_layout.drag != null) { layout__layout_save_state(); }
    g_layout.drag = null;
    document.documentElement.removeEventListener("mousemove", layout__layout_column_drag_move, false);
    document.documentElement.removeEventListener("mouseup", layout__layout_column_drag_stop, false);
}

// ---- Drag panels to preview a layout, then release to keep it. ----

/**
 * @brief Turns panel rearranging on or off. Leaving it cancels an unfinished move.
 */
function layout__layout_edit_toggle()
{
    let button = document.getElementById("layout-edit-button");

    _internal__layout_edit_finish(false, false);
    g_layout.edit_active = !g_layout.edit_active;
    document.body.setAttribute("data-layout-edit", g_layout.edit_active ? "1" : "0");
    button.value = g_layout.edit_active ? "lock layout" : "layout";
    button.setAttribute("aria-pressed", g_layout.edit_active ? "true" : "false");
    button.title = g_layout.edit_active
        ? "drag panels or the toolbar; use the top or bottom edge for a full-width toolbar; Escape cancels; click here to lock"
        : "rearrange the panels";
}

/**
 * @brief Finds the panel under the mouse.
 * @param object e -> Mouse event.
 * @return string|null Panel name, or null outside the panels.
 */
function layout__layout_panel_name_from_event(e)
{
    let names = ["channels", "chat", "info", "input", "toolbar"];
    let i;

    for (i = 0; i < names.length; i++)
    {
        if (g_layout.panels[names[i]].contains(e.target))
        {
            return names[i];
        }
    }
    return null;
}

/**
 * @brief Removes the appearance used while moving a panel.
 */
function layout__layout_edit_clear_highlight()
{
    let names = ["channels", "chat", "info", "input", "toolbar"];
    let i;

    for (i = 0; i < names.length; i++)
    {
        g_layout.panels[names[i]].classList.remove("layout-dragging");
    }
    document.body.removeAttribute("data-layout-dragging");
}

/**
 * @brief Copies the layout without sharing its column order array.
 * @param object state -> Layout to copy.
 * @return object Independent layout state.
 */
function _internal__layout_edit_copy_state(state)
{
    return Object.assign({}, state, { order: state.order.slice() });
}

/**
 * @brief Reads all panel positions before any styles are changed.
 * @return object Panel rectangles indexed by name.
 */
function _internal__layout_edit_boxes()
{
    let boxes = {};
    let name;

    for (name of ["channels", "chat", "info", "input", "toolbar"])
    {
        boxes[name] = g_layout.panels[name].getBoundingClientRect();
    }
    return boxes;
}

/**
 * @brief Releases a completed panel animation.
 * @param object e -> Animation completion event.
 */
function _internal__layout_edit_animation_finished(e)
{
    let index = layout_edit_animations.indexOf(e.target);

    if (index != -1)
    {
        layout_edit_animations.splice(index, 1);
    }
}

/**
 * @brief Stops earlier panel movement before measuring the new layout.
 */
function _internal__layout_edit_stop_animations()
{
    let animations = layout_edit_animations;
    let animation;

    layout_edit_animations = [];
    for (animation of animations)
    {
        animation.cancel();
    }
}

/**
 * @brief Slides panels from their old screen positions into the new grid.
 * @param object before -> Positions before changing the grid.
 * @param object after -> New grid positions.
 * @param string|null dragged -> Panel following the mouse, if any.
 */
function _internal__layout_edit_animate(before, after, dragged)
{
    let name;
    let panel;
    let dx;
    let dy;
    let animation;

    if (window.matchMedia("(prefers-reduced-motion: reduce)").matches)
    {
        return;
    }
    for (name of ["channels", "chat", "info", "input", "toolbar"])
    {
        panel = g_layout.panels[name];
        if (name == dragged || before[name].width == 0 || after[name].width == 0 || typeof panel.animate != "function")
        {
            continue;
        }
        dx = before[name].left - after[name].left;
        dy = before[name].top - after[name].top;
        if (Math.abs(dx) < 1 && Math.abs(dy) < 1)
        {
            continue;
        }
        animation = panel.animate(
        [
            { translate: dx + "px " + dy + "px" },
            { translate: "0px 0px" }
        ], { duration: 180, easing: "ease-out" });
        animation.onfinish = _internal__layout_edit_animation_finished;
        layout_edit_animations.push(animation);
    }
}

/**
 * @brief Removes the mouse offset so the panel's grid position can be measured.
 * @param object drag -> Current panel move.
 */
function _internal__layout_edit_restore_translate(drag)
{
    let style = g_layout.panels[drag.source].style;

    if (drag.translate == "")
    {
        style.removeProperty("translate");
    }
    else
    {
        style.setProperty("translate", drag.translate, drag.translate_priority);
    }
}

/**
 * @brief Keeps the grabbed point under the mouse and marks the intended position.
 * @param object drag -> Current panel move and mouse position.
 */
function _internal__layout_edit_position(drag)
{
    let box = drag.boxes[drag.source];
    let container = document.getElementById("communication-system-container");
    let dx = drag.x - box.left - drag.grab_x * box.width;
    let dy = drag.y - box.top - drag.grab_y * box.height;

    g_layout.panels[drag.source].style.setProperty("translate", dx + "px " + dy + "px", "important");
    drag.placeholder.hidden = !drag.valid;
    drag.placeholder.style.left = (box.left - drag.container_rect.left - container.clientLeft + container.scrollLeft) + "px";
    drag.placeholder.style.top = (box.top - drag.container_rect.top - container.clientTop + container.scrollTop) + "px";
    drag.placeholder.style.width = box.width + "px";
    drag.placeholder.style.height = box.height + "px";
}

/**
 * @brief Chooses a column position, or a top or bottom position for the message box and toolbar.
 * @param object drag -> Current panel move with unanimated grid positions.
 * @return object Layout preview for the current mouse position.
 */
function _internal__layout_edit_next_state(drag)
{
    let next = _internal__layout_edit_copy_state(g_layout.state);
    let columns = [];
    let remaining;
    let name;
    let box;
    let center = drag.x + (0.5 - drag.grab_x) * drag.source_width;
    let index = 0;
    let distance;
    let nearest = Infinity;
    let top = Infinity;
    let bottom = -Infinity;

    for (name of next.order)
    {
        box = drag.boxes[name];
        if (box.width > 0 && box.height > 0)
        {
            columns.push(name);
            top = Math.min(top, box.top);
            bottom = Math.max(bottom, box.bottom);
        }
    }
    if (drag.source == "toolbar")
    {
        next.toolbar_col = "full";
        next.toolbar_pos = drag.y < (drag.container_rect.top + drag.container_rect.bottom) / 2 ? "top" : "bottom";
        if (drag.y > drag.container_rect.top + 45 && drag.y < drag.container_rect.bottom - 45)
        {
            for (name of columns)
            {
                box = drag.boxes[name];
                distance = Math.max(box.left - drag.x, 0, drag.x - box.right);
                if (distance < nearest)
                {
                    nearest = distance;
                    next.toolbar_col = name;
                }
            }
        }
    }
    else if (drag.source == "input")
    {
        top = Math.min(top, drag.boxes.input.top);
        bottom = Math.max(bottom, drag.boxes.input.bottom);
        for (name of columns)
        {
            box = drag.boxes[name];
            distance = Math.max(box.left - drag.x, 0, drag.x - box.right);
            if (distance < nearest)
            {
                nearest = distance;
                next.input_col = name;
            }
        }
        next.input_pos = drag.y < (top + bottom) / 2 ? "top" : "bottom";
    }
    else
    {
        remaining = columns.filter(function(column)
        {
            return column != drag.source;
        });
        for (name of remaining)
        {
            box = drag.boxes[name];
            if (center > box.left + box.width / 2)
            {
                index++;
            }
        }
        remaining.splice(index, 0, drag.source);
        // Keep a hidden chat column in its saved position.
        index = 0;
        next.order = next.order.map(function(column)
        {
            if (columns.indexOf(column) != -1)
            {
                return remaining[index++];
            }
            return column;
        });
    }
    return next;
}

/**
 * @brief Updates the live layout at most once per screen refresh.
 */
function _internal__layout_edit_frame()
{
    let drag = layout_edit_drag;
    let container = document.getElementById("communication-system-container");
    let rect;
    let next;
    let before;

    if (drag == null)
    {
        return;
    }
    drag.frame = 0;
    rect = container.getBoundingClientRect();
    if (g_is_chat_hidden != drag.chat_hidden || getComputedStyle(container).display != "grid"
        || Math.abs(rect.left - drag.container_rect.left) > 1 || Math.abs(rect.top - drag.container_rect.top) > 1
        || Math.abs(rect.width - drag.container_rect.width) > 1 || Math.abs(rect.height - drag.container_rect.height) > 1)
    {
        _internal__layout_edit_finish(false, false);
        return;
    }
    if (!drag.started)
    {
        if (Math.hypot(drag.x - drag.start_x, drag.y - drag.start_y) < 6)
        {
            return;
        }
        drag.started = true;
        g_layout.panels[drag.source].classList.add("layout-dragging");
        document.body.setAttribute("data-layout-dragging", "1");
        drag.placeholder = document.createElement("div");
        drag.placeholder.className = "layout-drop-placeholder";
        drag.placeholder.setAttribute("data-label", "Drop here");
        drag.placeholder.setAttribute("aria-hidden", "true");
        container.appendChild(drag.placeholder);
    }
    drag.valid = drag.x >= rect.left && drag.x <= rect.right && drag.y >= rect.top && drag.y <= rect.bottom;
    if (drag.valid)
    {
        next = _internal__layout_edit_next_state(drag);
        if (next.order.join() != g_layout.state.order.join()
            || next.input_col != g_layout.state.input_col || next.input_pos != g_layout.state.input_pos
            || next.toolbar_col != g_layout.state.toolbar_col || next.toolbar_pos != g_layout.state.toolbar_pos)
        {
            before = _internal__layout_edit_boxes();
            _internal__layout_edit_stop_animations();
            _internal__layout_edit_restore_translate(drag);
            g_layout.state = next;
            drag.applying = true;
            layout__layout_apply();
            drag.applying = false;
            drag.boxes = _internal__layout_edit_boxes();
            _internal__layout_edit_animate(before, drag.boxes, drag.source);
        }
    }
    _internal__layout_edit_position(drag);
}

/**
 * @brief Saves a completed move or restores the layout from before the drag.
 * @param boolean commit -> Keep the preview when released inside the layout.
 * @param boolean animate -> Slide panels into their final positions.
 */
function _internal__layout_edit_finish(commit, animate)
{
    let drag = layout_edit_drag;
    let before;

    if (drag == null)
    {
        return;
    }
    before = _internal__layout_edit_boxes();
    cancelAnimationFrame(drag.frame);
    _internal__layout_edit_stop_animations();
    _internal__layout_edit_restore_translate(drag);
    if (drag.placeholder != null)
    {
        drag.placeholder.remove();
    }
    layout_edit_drag = null;
    g_layout.edit_dragged = null;
    layout__layout_edit_clear_highlight();
    document.removeEventListener("keydown", _internal__layout_edit_cancel);
    document.removeEventListener("visibilitychange", _internal__layout_edit_cancel);
    window.removeEventListener("blur", _internal__layout_edit_cancel);
    window.removeEventListener("resize", _internal__layout_edit_cancel);
    if (!commit)
    {
        g_layout.state = drag.original;
    }
    layout__layout_apply();
    if (drag.started && animate)
    {
        _internal__layout_edit_animate(before, _internal__layout_edit_boxes(), null);
    }
    if (drag.started && commit)
    {
        layout__layout_save_state();
    }
}

/**
 * @brief Cancels when Escape is pressed or the browser view changes.
 * @param object e -> Keyboard, visibility, resize or focus event.
 */
function _internal__layout_edit_cancel(e)
{
    if (e.type == "keydown" && e.key != "Escape")
    {
        return;
    }
    if (e.type == "visibilitychange" && !document.hidden)
    {
        return;
    }
    _internal__layout_edit_finish(false, e.type == "keydown");
}

/**
 * @brief Remembers the grabbed panel and its starting layout.
 * @param object e -> Mouse press inside the layout.
 */
function layout__layout_edit_mousedown(e)
{
    let name;
    let container = document.getElementById("communication-system-container");
    let boxes;
    let box;
    let style;

    if (e.button != 0 || !g_layout.edit_active || !g_layout.grid_active || getComputedStyle(container).display != "grid")
    {
        return;
    }
    if (e.target.closest("#layout-edit-button, #local-settings-container") != null)
    {
        return;
    }
    name = layout__layout_panel_name_from_event(e);
    if (name == null)
    {
        return;
    }
    _internal__layout_edit_finish(false, false);
    _internal__layout_edit_stop_animations();
    boxes = _internal__layout_edit_boxes();
    box = boxes[name];
    if (box.width == 0 || box.height == 0)
    {
        return;
    }
    style = g_layout.panels[name].style;
    layout_edit_drag =
    {
        source: name,
        original: _internal__layout_edit_copy_state(g_layout.state),
        boxes: boxes,
        container_rect: container.getBoundingClientRect(),
        chat_hidden: g_is_chat_hidden,
        x: e.clientX,
        y: e.clientY,
        start_x: e.clientX,
        start_y: e.clientY,
        grab_x: (e.clientX - box.left) / box.width,
        grab_y: (e.clientY - box.top) / box.height,
        source_width: box.width,
        translate: style.getPropertyValue("translate"),
        translate_priority: style.getPropertyPriority("translate"),
        frame: 0,
        started: false,
        applying: false,
        valid: false,
        placeholder: null
    };
    g_layout.edit_dragged = name;
    document.addEventListener("keydown", _internal__layout_edit_cancel);
    document.addEventListener("visibilitychange", _internal__layout_edit_cancel);
    window.addEventListener("blur", _internal__layout_edit_cancel);
    window.addEventListener("resize", _internal__layout_edit_cancel);
    e.preventDefault();
    e.stopPropagation();
}

/**
 * @brief Queues the next live preview while the left mouse button is held.
 * @param object e -> Mouse movement.
 */
function layout__layout_edit_mousemove(e)
{
    if (layout_edit_drag == null)
    {
        return;
    }
    if ((e.buttons & 1) == 0)
    {
        _internal__layout_edit_finish(false, false);
        return;
    }
    layout_edit_drag.x = e.clientX;
    layout_edit_drag.y = e.clientY;
    if (layout_edit_drag.frame == 0)
    {
        layout_edit_drag.frame = requestAnimationFrame(_internal__layout_edit_frame);
    }
    e.preventDefault();
}

/**
 * @brief Finishes the preview at the release position and saves a valid drop.
 * @param object e -> Mouse release.
 */
function layout__layout_edit_mouseup(e)
{
    if (layout_edit_drag == null || e.button != 0)
    {
        return;
    }
    cancelAnimationFrame(layout_edit_drag.frame);
    layout_edit_drag.x = e.clientX;
    layout_edit_drag.y = e.clientY;
    _internal__layout_edit_frame();
    if (layout_edit_drag != null)
    {
        _internal__layout_edit_finish(layout_edit_drag.valid, true);
    }
}
