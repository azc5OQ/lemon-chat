// layout.js is embedded in template.html along with the other client files
// it is the desktop grid layout engine: column order and widths, the chat-input row and the edit mode,
// persisted in localStorage under lemon_layout; touch devices keep the legacy layout and skip it
// main.js wires it at load, ui.js calls it when panels are toggled; the state is g_layout in globals.js

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
            && saved.order.indexOf("info") != -1 && saved.input_col != null && saved.input_pos != null)
        {
            // a collapsed info column (saved before the 90px minimum existed) looks like
            // the panel was deleted - snap it back to the default width
            if (parseInt(saved.col_info, 10) < 90) { saved.col_info = "13%"; }
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
 * @brief (re)builds the grid templates from g_layout.state and g_is_chat_hidden and pins the panels to their areas
 *        does not touch the container's display; connect/disconnect owns that
 *
 * @return void
 */
function layout__layout_apply()
{
    if (g_layout.grid_active == false) { return; }

    let container = document.getElementById("communication-system-container");

    // fill everything under the head menu: the themes' height:80% left a dead band at
    // the bottom (the legacy layout covered it only by overflowing past the container)
    let head_menu = document.getElementById("communication-system-head-menu");
    container.style.height = "calc(100vh - " + ((head_menu != null) ? head_menu.offsetHeight : 30) + "px)";

    let order = g_layout.state.order.slice();
    if (g_is_chat_hidden == true)
    {
        order = order.filter(function(col) { return col != "chat"; });
    }

    // the input row lives inside the chat column, because the composer must align with the
    // chat panel above it - same left edge, same right edge, in every theme
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

    container.style.gridTemplateAreas = '"' + row_a.join(" ") + '" "' + row_b.join(" ") + '"';
    container.style.gridTemplateColumns = widths.join(" ");
    container.style.gridTemplateRows = (g_layout.state.input_pos == "top") ? "auto minmax(0, 1fr)" : "minmax(0, 1fr) auto";

    g_layout.panels.channels.style.gridArea = "channels";
    g_layout.panels.chat.style.gridArea = "chat";
    g_layout.panels.info.style.gridArea = "info";
    g_layout.panels.input.style.gridArea = "input";

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

    // no left inset here: the input sits in the chat column, so the corner mic button is
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

// ---- layout-edit mode: drag whole panels to re-arrange, then lock ----

/**
 * @brief toggles layout-edit mode (body attribute + button label); leaving it saves the layout and clears any in-progress panel drag
 *
 * @return void
 */
function layout__layout_edit_toggle()
{
    g_layout.edit_active = !g_layout.edit_active;
    document.body.setAttribute("data-layout-edit", g_layout.edit_active ? "1" : "0");
    document.getElementById("layout-edit-button").value = g_layout.edit_active ? "lock layout" : "layout";
    if (g_layout.edit_active == false)
    {
        layout__layout_save_state();
        layout__layout_edit_clear_highlight();
        g_layout.edit_dragged = null;
    }
}

/**
 * @brief which layout panel an event landed in
 *
 * @param object e -> the event
 *
 * @return string|null "channels", "chat", "info" or "input", null when outside all four
 */
function layout__layout_panel_name_from_event(e)
{
    let names = ["channels", "chat", "info", "input"];
    for (let i = 0; i < names.length; i++)
    {
        if (g_layout.panels[names[i]].contains(e.target)) { return names[i]; }
    }
    return null;
}

/**
 * @brief removes the dragging and drop-target highlight classes from all four panels
 *
 * @return void
 */
function layout__layout_edit_clear_highlight()
{
    let names = ["channels", "chat", "info", "input"];
    for (let i = 0; i < names.length; i++)
    {
        g_layout.panels[names[i]].classList.remove("layout-drop-target");
        g_layout.panels[names[i]].classList.remove("layout-dragging");
    }
}

/**
 * @brief edit mode: starts dragging the panel under the cursor and marks it visually
 *
 * @param object e -> the mouse event
 *
 * @return void
 */
function layout__layout_edit_mousedown(e)
{
    if (g_layout.edit_active == false || g_layout.grid_active == false) { return; }
    let name = layout__layout_panel_name_from_event(e);
    if (name == null) { return; }
    g_layout.edit_dragged = name;
    g_layout.panels[name].classList.add("layout-dragging");
    e.preventDefault();
    e.stopPropagation();
}

/**
 * @brief edit mode: highlights the panel currently hovered over as the drop target
 *
 * @param object e -> the mouse event
 *
 * @return void
 */
function layout__layout_edit_mousemove(e)
{
    if (g_layout.edit_dragged == null) { return; }
    let names = ["channels", "chat", "info", "input"];
    for (let i = 0; i < names.length; i++) { g_layout.panels[names[i]].classList.remove("layout-drop-target"); }
    let over = layout__layout_panel_name_from_event(e);
    if (over != null && over != g_layout.edit_dragged)
    {
        g_layout.panels[over].classList.add("layout-drop-target");
    }
}

/**
 * @brief edit mode drop: swaps two columns, or parks the input row in a column (dropping it on its own column flips top/bottom), then re-applies the grid
 *
 * @param object e -> the mouse event
 *
 * @return void
 */
function layout__layout_edit_mouseup(e)
{
    if (g_layout.edit_dragged == null) { return; }
    let source = g_layout.edit_dragged;
    let target = layout__layout_panel_name_from_event(e);
    g_layout.edit_dragged = null;
    layout__layout_edit_clear_highlight();
    if (target == null || target == source) { return; }

    if (source == "input" || target == "input")
    {
        // moving the input row: dropping it on a column parks it there; dropping it on
        // the column it already lives in flips it between top and bottom
        let col = (source == "input") ? target : source;
        if (g_layout.state.input_col == col)
        {
            g_layout.state.input_pos = (g_layout.state.input_pos == "top") ? "bottom" : "top";
        }
        else
        {
            g_layout.state.input_col = col;
        }
    }
    else
    {
        // two columns: swap their places
        let a = g_layout.state.order.indexOf(source);
        let b = g_layout.state.order.indexOf(target);
        g_layout.state.order[a] = target;
        g_layout.state.order[b] = source;
    }
    layout__layout_apply();
}
