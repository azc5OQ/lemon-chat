// This file lets themes move their lighting with the mouse. A theme enables it
// with --theme-pointer-light: 1 and uses --theme-pointer-x and --theme-pointer-y.
// --theme-pointer-surfaces lists the selectors that also receive local light positions.
// Those two values follow the mouse with a short delay, as a light would. A theme that
// draws something on the cursor itself uses --theme-pointer-raw-x and --theme-pointer-raw-y,
// the exact mouse position in pixels, written on the root element at the same time.

var theme_effects__refresh = (function()
{
    let enabled = false;
    let pointer_active = false;
    let frame = 0;
    let previous_time = 0;
    let current_x = 50;
    let current_y = 50;
    let target_x = 50;
    let target_y = 50;
    let pointer_x = 0;
    let pointer_y = 0;
    let reduced_motion = null;
    let mouse_available = null;
    let surface_selector = "";
    let separate_border_selector = "input, select, textarea, img, .client-audio-state";
    let surfaces = new Set();
    let visible_surfaces = new Set();
    let separate_lights = new Map();
    let surfaces_dirty = false;
    let surface_observer = null;
    let size_observer = null;
    let visibility_observer = null;
    let written_styles = new WeakMap();

    /**
     * @brief Writes a light value only when it changed.
     * @param Element element -> element receiving the value
     * @param string name -> CSS property name
     * @param string value -> new CSS value
     * @return void
     */
    function _internal__set_value(element, name, value)
    {
        if (element.style.getPropertyValue(name) != value)
        {
            element.style.setProperty(name, value);
            written_styles.set(element, element.style.cssText);
        }
    }

    /**
     * @brief Removes the light values from a panel that no longer uses them.
     * @param Element element -> panel to clear
     * @return void
     */
    function _internal__clear_surface(element)
    {
        let light = separate_lights.get(element);

        if (light != null)
        {
            light.remove();
            separate_lights.delete(element);
        }
        element.removeAttribute("data-theme-pointer-border");
        element.style.removeProperty("--theme-pointer-local-x");
        element.style.removeProperty("--theme-pointer-local-y");
        element.style.removeProperty("--theme-pointer-active");
        written_styles.set(element, element.style.cssText);
    }

    /**
     * @brief Registers a bordered element without reading its position yet.
     * @param Element element -> element selected by the theme
     * @return void
     */
    function _internal__add_surface(element)
    {
        if (surfaces.has(element) || element.classList.contains("theme-pointer-border-light"))
        {
            return;
        }
        surfaces.add(element);
        element.setAttribute("data-theme-pointer-border", element.matches(separate_border_selector) ? "overlay" : "box");
        _internal__set_value(element, "--theme-pointer-active", "0");
        if (visibility_observer != null)
        {
            visibility_observer.observe(element);
        }
        else
        {
            visible_surfaces.add(element);
        }
    }

    /**
     * @brief Stops watching an element that was removed or no longer has a themed border.
     * @param Element element -> element to forget
     * @return void
     */
    function _internal__remove_surface(element)
    {
        surfaces.delete(element);
        visible_surfaces.delete(element);
        if (visibility_observer != null)
        {
            visibility_observer.unobserve(element);
        }
        if (size_observer != null)
        {
            size_observer.unobserve(element);
        }
        _internal__clear_surface(element);
    }

    /**
     * @brief Keeps offscreen messages and closed dialogs out of the mouse calculations.
     * @param IntersectionObserverEntry[] entries -> elements whose visibility changed
     * @return void
     */
    function _internal__visibility_changed(entries)
    {
        let entry = null;

        for (entry of entries)
        {
            if (!surfaces.has(entry.target))
            {
                continue;
            }
            if (entry.isIntersecting)
            {
                visible_surfaces.add(entry.target);
                if (size_observer != null)
                {
                    size_observer.observe(entry.target);
                }
            }
            else
            {
                visible_surfaces.delete(entry.target);
                _internal__set_value(entry.target, "--theme-pointer-active", "0");
                if (separate_lights.has(entry.target))
                {
                    separate_lights.get(entry.target).remove();
                    separate_lights.delete(entry.target);
                }
                if (size_observer != null)
                {
                    size_observer.unobserve(entry.target);
                }
            }
        }
        _internal__schedule();
    }

    /**
     * @brief Schedules one update when the mouse light is in use.
     * @return void
     */
    function _internal__schedule()
    {
        if (enabled == true && pointer_active == true && frame == 0)
        {
            frame = window.requestAnimationFrame(_internal__draw);
        }
    }

    /**
     * @brief Refreshes a panel after its opening animation finishes.
     * @param Event event -> completed transition or animation
     * @return void
     */
    function _internal__layout_finished(event)
    {
        let element = null;

        for (element of visible_surfaces)
        {
            if (event.target == element || event.target.contains(element))
            {
                _internal__schedule();
                return;
            }
        }
    }

    /**
     * @brief Finds borders in new messages and controls without rescanning the chat history.
     * @param MutationRecord[] records -> changes to the page
     * @return void
     */
    function _internal__surface_changes(records)
    {
        let record = null;
        let changed = false;
        let node = null;
        let element = null;

        for (record of records)
        {
            if (record.type == "childList")
            {
                for (node of record.removedNodes)
                {
                    if (node.nodeType != 1 || node.isConnected || node.classList.contains("theme-pointer-border-light"))
                    {
                        continue;
                    }
                    if (surfaces.has(node))
                    {
                        _internal__remove_surface(node);
                    }
                    for (element of node.querySelectorAll("[data-theme-pointer-border]"))
                    {
                        _internal__remove_surface(element);
                    }
                    changed = true;
                }
                for (node of record.addedNodes)
                {
                    if (node.nodeType != 1 || !node.isConnected || node.classList.contains("theme-pointer-border-light"))
                    {
                        continue;
                    }
                    if (node.matches(surface_selector))
                    {
                        _internal__add_surface(node);
                    }
                    for (element of node.querySelectorAll(surface_selector))
                    {
                        _internal__add_surface(element);
                    }
                    changed = true;
                }
            }
            else if (!record.target.classList.contains("theme-pointer-border-light")
                && (record.attributeName != "style" || written_styles.get(record.target) != record.target.style.cssText))
            {
                if (record.target.matches(surface_selector))
                {
                    _internal__add_surface(record.target);
                }
                else if (surfaces.has(record.target))
                {
                    _internal__remove_surface(record.target);
                }
                changed = true;
            }
        }
        if (changed == true)
        {
            _internal__schedule();
        }
    }

    /**
     * @brief Starts watching the borders selected by the current theme.
     * @return void
     */
    function _internal__collect_surfaces()
    {
        let element = null;

        surfaces_dirty = false;
        if (surface_observer != null)
        {
            surface_observer.disconnect();
        }
        if (size_observer != null)
        {
            size_observer.disconnect();
        }
        if (visibility_observer != null)
        {
            visibility_observer.disconnect();
        }
        for (element of surfaces)
        {
            _internal__clear_surface(element);
        }
        surfaces.clear();
        visible_surfaces.clear();
        if (surface_selector == "")
        {
            return;
        }

        for (element of document.querySelectorAll(surface_selector))
        {
            _internal__add_surface(element);
        }
        if (surface_observer != null)
        {
            surface_observer.observe(document.documentElement,
            {
                attributes: true,
                attributeFilter: ["style", "class", "hidden", "open"],
                childList: true,
                subtree: true
            });
        }
    }

    /**
     * @brief Stops the lighting where it is when the pointer leaves the page.
     * @return void
     */
    function _internal__pause()
    {
        let element = null;

        if (frame != 0)
        {
            window.cancelAnimationFrame(frame);
            frame = 0;
        }
        previous_time = 0;
        pointer_active = false;
        if (enabled == true)
        {
            _internal__set_value(document.documentElement, "--theme-pointer-active", "0");
            for (element of visible_surfaces)
            {
                _internal__set_value(element, "--theme-pointer-active", "0");
            }
            for (element of separate_lights.values())
            {
                _internal__set_value(element, "--theme-pointer-active", "0");
            }
        }
    }

    /**
     * @brief Keeps a separate border light inside the same scrolling and popup layers as its control.
     * @param Element element -> field, picture or audio icon
     * @param DOMRect rectangle -> control position on the screen
     * @return object -> parent and dimensions for the light
     */
    function _internal__separate_light_layout(element, rectangle)
    {
        let host = element.parentElement;
        let host_rectangle = null;
        let scale_x = 1;
        let scale_y = 1;
        let computed = window.getComputedStyle(element);

        while (host.parentElement != null && window.getComputedStyle(host).position == "static")
        {
            host = host.parentElement;
        }
        host_rectangle = host.getBoundingClientRect();
        scale_x = host_rectangle.width > 0 ? host.offsetWidth / host_rectangle.width : 1;
        scale_y = host_rectangle.height > 0 ? host.offsetHeight / host_rectangle.height : 1;
        return {
            host: host,
            left: (rectangle.left - host_rectangle.left) * scale_x - host.clientLeft + host.scrollLeft,
            top: (rectangle.top - host_rectangle.top) * scale_y - host.clientTop + host.scrollTop,
            width: rectangle.width * scale_x,
            height: rectangle.height * scale_y,
            radius: computed.borderRadius,
            scale_x: scale_x,
            scale_y: scale_y
        };
    }

    /**
     * @brief Adds a light without covering a native control or replacing an audio icon's CSS drawing.
     * @param Element element -> control or picture to highlight
     * @param object layout -> previously measured position inside its parent
     * @return Element -> decorative light, which cannot receive clicks or focus
     */
    function _internal__separate_light(element, layout)
    {
        let light = separate_lights.get(element);

        if (light == null)
        {
            light = document.createElement("span");
            light.className = "theme-pointer-border-light";
            light.setAttribute("aria-hidden", "true");
            separate_lights.set(element, light);
        }
        if (light.parentElement !== layout.host)
        {
            layout.host.appendChild(light);
        }
        _internal__set_value(light, "left", layout.left.toFixed(3) + "px");
        _internal__set_value(light, "top", layout.top.toFixed(3) + "px");
        _internal__set_value(light, "width", layout.width.toFixed(3) + "px");
        _internal__set_value(light, "height", layout.height.toFixed(3) + "px");
        _internal__set_value(light, "border-radius", layout.radius);
        return light;
    }

    /**
     * @brief Moves the light toward the latest mouse position, then stops when it arrives.
     * @param number time -> time supplied by the browser for this frame
     * @return void
     */
    function _internal__draw(time)
    {
        let elapsed = previous_time == 0 ? 16 : Math.min(time - previous_time, 64);
        let amount = reduced_motion.matches ? 1 : 1 - Math.exp(-elapsed / 65);
        let moving = false;
        let root = document.documentElement;
        let width = window.innerWidth;
        let height = window.innerHeight;
        let light_x = 0;
        let light_y = 0;
        let rectangles = [];
        let elements = [];
        let layouts = [];
        let element = null;
        let target = null;
        let rectangle = null;
        let layout = null;
        let near = false;
        let index = 0;

        frame = 0;
        if (enabled == false || pointer_active == false || width <= 0 || height <= 0)
        {
            return;
        }

        target_x = Math.max(0, Math.min(100, pointer_x * 100 / width));
        target_y = Math.max(0, Math.min(100, pointer_y * 100 / height));
        current_x += (target_x - current_x) * amount;
        current_y += (target_y - current_y) * amount;
        moving = Math.abs(target_x - current_x) + Math.abs(target_y - current_y) > 0.02;
        if (moving == false)
        {
            current_x = target_x;
            current_y = target_y;
        }

        if (surfaces_dirty == true)
        {
            _internal__collect_surfaces();
        }
        light_x = current_x * width / 100;
        light_y = current_y * height / 100;
        for (element of visible_surfaces)
        {
            rectangle = element.getBoundingClientRect();
            near = rectangle.width > 0 && rectangle.height > 0
                && light_x > rectangle.left - 210 && light_x < rectangle.right + 210
                && light_y > rectangle.top - 210 && light_y < rectangle.bottom + 210;
            elements.push(element);
            rectangles.push(rectangle);
            layouts.push(near && element.matches(separate_border_selector)
                ? _internal__separate_light_layout(element, rectangle) : null);
        }

        // Read every panel first so our CSS writes do not force repeated layout work.
        _internal__set_value(root, "--theme-pointer-x", current_x.toFixed(3) + "%");
        _internal__set_value(root, "--theme-pointer-y", current_y.toFixed(3) + "%");
        _internal__set_value(root, "--theme-pointer-raw-x", pointer_x.toFixed(1) + "px");
        _internal__set_value(root, "--theme-pointer-raw-y", pointer_y.toFixed(1) + "px");
        _internal__set_value(root, "--theme-pointer-active", "1");
        for (index = 0; index < elements.length; index++)
        {
            element = elements[index];
            rectangle = rectangles[index];
            layout = layouts[index];
            target = element;
            if (element.matches(separate_border_selector))
            {
                if (layout == null)
                {
                    if (separate_lights.has(element))
                    {
                        _internal__set_value(separate_lights.get(element), "--theme-pointer-active", "0");
                    }
                    continue;
                }
                target = _internal__separate_light(element, layout);
            }
            _internal__set_value(target, "--theme-pointer-local-x", ((light_x - rectangle.left) * (layout == null ? 1 : layout.scale_x)).toFixed(3) + "px");
            _internal__set_value(target, "--theme-pointer-local-y", ((light_y - rectangle.top) * (layout == null ? 1 : layout.scale_y)).toFixed(3) + "px");
            _internal__set_value(target, "--theme-pointer-active", "1");
        }
        previous_time = moving ? time : 0;
        if (moving == true)
        {
            _internal__schedule();
        }
    }

    /**
     * @brief Keeps the newest mouse position without drawing for every mouse event.
     * @param PointerEvent event -> pointer movement inside the page
     * @return void
     */
    function _internal__pointer_move(event)
    {
        let width = window.innerWidth;
        let height = window.innerHeight;

        if (enabled == false || event.pointerType != "mouse" || width <= 0 || height <= 0)
        {
            return;
        }

        pointer_x = event.clientX;
        pointer_y = event.clientY;
        pointer_active = true;
        _internal__schedule();
    }

    /**
     * @brief Enables mouse lighting only when the selected theme and device allow it.
     * @return void
     */
    function _internal__refresh()
    {
        let should_enable = false;
        let root = null;
        let computed = null;
        let next_selector = "";
        let element = null;

        if (typeof window === "undefined" || typeof document === "undefined"
            || typeof window.matchMedia !== "function" || typeof window.requestAnimationFrame !== "function"
            || typeof window.getComputedStyle !== "function" || document.documentElement == null)
        {
            return;
        }

        root = document.documentElement;
        if (reduced_motion == null)
        {
            reduced_motion = window.matchMedia("(prefers-reduced-motion: reduce)");
            mouse_available = window.matchMedia("(any-hover: hover) and (any-pointer: fine)");
            reduced_motion.addEventListener("change", _internal__refresh);
            mouse_available.addEventListener("change", _internal__refresh);
            document.addEventListener("visibilitychange", _internal__refresh);
            if (typeof window.MutationObserver === "function")
            {
                surface_observer = new window.MutationObserver(_internal__surface_changes);
            }
            if (typeof window.ResizeObserver === "function")
            {
                size_observer = new window.ResizeObserver(_internal__schedule);
            }
            if (typeof window.IntersectionObserver === "function")
            {
                visibility_observer = new window.IntersectionObserver(_internal__visibility_changed);
            }
        }

        computed = window.getComputedStyle(root);
        should_enable = document.hidden == false && mouse_available.matches == true
            && computed.getPropertyValue("--theme-pointer-light").trim() === "1";
        next_selector = should_enable ? computed.getPropertyValue("--theme-pointer-surfaces").trim() : "";
        if (next_selector != surface_selector)
        {
            surface_selector = next_selector;
            surfaces_dirty = true;
        }
        if (should_enable == enabled)
        {
            if (enabled == true && surfaces_dirty == true)
            {
                _internal__collect_surfaces();
                _internal__schedule();
            }
            return;
        }
        enabled = should_enable;

        if (enabled == true)
        {
            window.addEventListener("pointermove", _internal__pointer_move, { passive: true });
            root.addEventListener("pointerleave", _internal__pause);
            window.addEventListener("blur", _internal__pause);
            window.addEventListener("resize", _internal__schedule, { passive: true });
            document.addEventListener("scroll", _internal__schedule, { capture: true, passive: true });
            document.addEventListener("transitionend", _internal__layout_finished);
            document.addEventListener("animationend", _internal__layout_finished);
            _internal__collect_surfaces();
        }
        else
        {
            _internal__pause();
            window.removeEventListener("pointermove", _internal__pointer_move);
            root.removeEventListener("pointerleave", _internal__pause);
            window.removeEventListener("blur", _internal__pause);
            window.removeEventListener("resize", _internal__schedule);
            document.removeEventListener("scroll", _internal__schedule, true);
            document.removeEventListener("transitionend", _internal__layout_finished);
            document.removeEventListener("animationend", _internal__layout_finished);
            if (surface_observer != null)
            {
                surface_observer.disconnect();
            }
            if (size_observer != null)
            {
                size_observer.disconnect();
            }
            if (visibility_observer != null)
            {
                visibility_observer.disconnect();
            }
            for (element of surfaces)
            {
                _internal__clear_surface(element);
            }
            surfaces.clear();
            visible_surfaces.clear();
            surfaces_dirty = false;
            root.style.removeProperty("--theme-pointer-x");
            root.style.removeProperty("--theme-pointer-y");
            root.style.removeProperty("--theme-pointer-raw-x");
            root.style.removeProperty("--theme-pointer-raw-y");
            root.style.removeProperty("--theme-pointer-active");
            written_styles = new WeakMap();
            current_x = target_x = 50;
            current_y = target_y = 50;
        }
    }

    return _internal__refresh;
})();
