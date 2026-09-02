// platform-detection.js decides whether this device gets the touch interface (mobile themes,
// touch gestures, the long-press menus); main.js asks it once at page load

// touch detection in layers, most reliable signal first; any hit counts, because every
// layer covers the blind spots of the ones below it
function detect_touch_device()
{
    // the chromium client-hints mobile flag survives user agent freezing; safari and firefox lack it
    if (typeof navigator !== "undefined" && navigator.userAgentData != null && navigator.userAgentData.mobile === true)
    {
        return true;
    }

    let user_agent_string = (typeof navigator !== "undefined" && typeof navigator.userAgent === "string") ? navigator.userAgent : "";

    // android and ios cover practically every touch device; some of their browsers report
    // pointer/hover like a desktop, which is why the css probe alone was not enough
    if (/android|iphone|ipad|ipod/i.test(user_agent_string) == true)
    {
        return true;
    }

    // an ipad on ipados pretends to be a mac, so a "mac" with many touch points is an ipad
    if (/macintosh/i.test(user_agent_string) == true && typeof navigator !== "undefined" && navigator.maxTouchPoints > 1)
    {
        return true;
    }

    // the css probe stays as the last fallback for touch devices that are neither android nor ios
    if (typeof window !== "undefined" && window.matchMedia("(pointer: coarse) and (hover: none)").matches)
    {
        return true;
    }

    return false;
}
