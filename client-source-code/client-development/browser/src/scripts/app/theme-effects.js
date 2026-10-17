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
    let surfaces = [];
    let removed_surfaces = [];
    let surfaces_dirty = false;
    let surface_observer = null;
    let size_observer = null;
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
        element.style.removeProperty("--theme-pointer-local-x");
        element.style.removeProperty("--theme-pointer-local-y");
        element.style.removeProperty("--theme-pointer-active");
        written_styles.set(element, element.style.cssText);
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

        for (element of surfaces)
        {
            if (event.target == element || event.target.contains(element))
            {
                _internal__schedule();
                return;
            }
        }
    }

    /**
     * @brief Updates light positions after a panel moves or a context menu changes.
     * @param MutationRecord[] records -> changes to the watched panels and their parents
     * @return void
     */
    function _internal__surface_changes(records)
    {
        let record = null;
        let changed = false;

        for (record of records)
        {
            if (record.type == "childList")
            {
                surfaces_dirty = true;
                changed = true;
            }
            else if (record.attributeName != "style" || written_styles.get(record.target) != record.target.style.cssText)
            {
                changed = true;
            }
        }
        if (changed == true)
        {
            _internal__schedule();
        }
    }

    /**
     * @brief Finds the selected panels and watches only their layout and menu changes.
     * @return void
     */
    function _internal__collect_surfaces()
    {
        let next_surfaces = [];
        let element = null;
        let ancestor = null;
        let watched = new Set();
        let menu_container = document.getElementById("contextmenus-container");

        if (surface_selector != "")
        {
            try
            {
                next_surfaces = Array.from(document.querySelectorAll(surface_selector));
            }
            catch (error)
            {
                next_surfaces = [];
            }
        }
        for (element of surfaces)
        {
            if (next_surfaces.includes(element) == false)
            {
                removed_surfaces.push(element);
            }
        }
        surfaces = next_surfaces;
        surfaces_dirty = false;
        if (surface_observer != null)
        {
            surface_observer.disconnect();
        }
        if (size_observer != null)
        {
            size_observer.disconnect();
        }
        if (surface_selector == "")
        {
            return;
        }

        for (element of surfaces)
        {
            ancestor = element;
            while (ancestor != null)
            {
                watched.add(ancestor);
                ancestor = ancestor.parentElement;
            }
            if (size_observer != null)
            {
                size_observer.observe(element);
            }
        }
        if (menu_container != null)
        {
            watched.add(menu_container);
        }
        if (surface_observer != null)
        {
            for (element of watched)
            {
                surface_observer.observe(element,
                {
                    attributes: true,
                    attributeFilter: ["style", "class", "hidden", "open"],
                    childList: element == menu_container
                });
            }
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
            for (element of surfaces)
            {
                _internal__set_value(element, "--theme-pointer-active", "0");
            }
        }
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
        let element = null;
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
        for (element of surfaces)
        {
            rectangles.push(element.getBoundingClientRect());
        }

        // Read every panel first so our CSS writes do not force repeated layout work.
        _internal__set_value(root, "--theme-pointer-x", current_x.toFixed(3) + "%");
        _internal__set_value(root, "--theme-pointer-y", current_y.toFixed(3) + "%");
        _internal__set_value(root, "--theme-pointer-raw-x", pointer_x.toFixed(1) + "px");
        _internal__set_value(root, "--theme-pointer-raw-y", pointer_y.toFixed(1) + "px");
        _internal__set_value(root, "--theme-pointer-active", "1");
        for (index = 0; index < surfaces.length; index++)
        {
            _internal__set_value(surfaces[index], "--theme-pointer-local-x", (light_x - rectangles[index].left).toFixed(3) + "px");
            _internal__set_value(surfaces[index], "--theme-pointer-local-y", (light_y - rectangles[index].top).toFixed(3) + "px");
            _internal__set_value(surfaces[index], "--theme-pointer-active", "1");
        }
        for (element of removed_surfaces)
        {
            _internal__clear_surface(element);
        }
        removed_surfaces = [];
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
                for (element of removed_surfaces)
                {
                    _internal__clear_surface(element);
                }
                removed_surfaces = [];
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
            for (element of surfaces.concat(removed_surfaces))
            {
                _internal__clear_surface(element);
            }
            surfaces = [];
            removed_surfaces = [];
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
