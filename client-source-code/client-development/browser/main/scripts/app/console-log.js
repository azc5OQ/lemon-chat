// adds context, source location and time to console output
// use global.__LOG to adjust logging at runtime
(function ()
{
    var context_name = IS_CURRENT_THREAD_WORKER ? THREAD_NAME : "main";

    var COLORS = {
        "main":                   "#6ab0ff",
        "data_processing_worker": "#4caf50",
        "websocket_worker":       "#26c6da",
        "opus_encoder_worker":    "#ff9800",
        "opus_decoder_worker":    "#c586c0",
        "minimp3_worker":         "#d7ba7d"
    };
    var color = COLORS[context_name] || "#9aa0a6";

    var LEVELS = { debug: 10, info: 20, warn: 30, error: 40 };
    // keep warnings and errors by default. detailed logging builds costly stack traces
    var cfg = global.__LOG || (global.__LOG = { enabled: true, level: "warn", mute: [], time: true, raw: false });

    // broadcast logging preferences once. receivers do not broadcast them again
    var log_channel = null;
    try
    {
        log_channel = new BroadcastChannel("lemon_log_config");
    }
    catch (e)
    {
        console.warn("BroadcastChannel unavailable; cross-context log control disabled:", e.message);
        log_channel = null;
    }
    if (log_channel !== null)
    {
        log_channel.onmessage = function (ev)
        {
            if (ev && ev.data)
            {
                for (var k in ev.data)
                {
                    cfg[k] = ev.data[k];
                }
            }
        };
    }
    /**
     * @brief applies logging options locally and broadcasts them to other client contexts
     *
     * @param object patch -> logging options to replace
     *
     * @return void
     */
    cfg.set = function (patch)
    {
        for (var k in patch)
        {
            cfg[k] = patch[k];
        }
        if (log_channel !== null)
        {
            try
            {
                log_channel.postMessage(patch);
            }
            catch (ee)
            {
                console.warn("failed to broadcast log config:", ee.message);
            }
        }
    };
    /**
     * @brief disables wrapped console output across client contexts
     *
     * @return void
     */
    cfg.disable = function ()
    {
        cfg.set({ enabled: false });
    };
    /**
     * @brief enables wrapped console output across client contexts
     *
     * @return void
     */
    cfg.enable  = function ()
    {
        cfg.set({ enabled: true });
    };
    /**
     * @brief sets whether wrapped console lines include a timestamp
     *
     * @param boolean on -> false hides timestamps, any other value enables them
     *
     * @return void
     */
    cfg.setTime = function (on)
    {
        cfg.set({ time: on !== false });
    };

    /**
     * @brief pads a value with leading zeroes to the requested width
     *
     * @param number n -> value to format
     * @param number width -> minimum character count
     *
     * @return string -> padded value
     */
    function pad(n, width)
    {
        var s = "" + n;
        while (s.length < width)
        {
            s = "0" + s;
        }
        return s;
    }

    /**
     * @brief formats the local time used in a console prefix
     *
     * @return string -> hours, minutes, seconds and milliseconds
     */
    function stamp()
    {
        var d = new Date();
        return pad(d.getHours(), 2) + ":" + pad(d.getMinutes(), 2) + ":" + pad(d.getSeconds(), 2) + "." + pad(d.getMilliseconds(), 3);
    }

    // read the original caller from the stack instead of reporting the wrapper
    /**
     * @brief reads the console caller name and line from Chrome or Firefox stack frames
     *
     * @return string -> caller name and line with anon and ? as fallbacks
     */
    function origin()
    {
        var stack = "";
        try
        {
            throw new Error();
        }
        catch (e)
        {
            stack = e.stack || "";
        }

        var frames = [];
        var raw = stack.split("\n");
        for (var i = 0; i < raw.length; i++)
        {
            // remove Chrome's Error heading before reading frames
            if (raw[i].indexOf("Error") === 0 && raw[i].indexOf("@") === -1)
            {
                continue;
            }
            if (raw[i].length > 0)
            {
                frames.push(raw[i]);
            }
        }

        // frames[0] = origin(), frames[1] = the console wrapper, frames[2] = the real caller
        var caller = frames[2] || frames[frames.length - 1] || "";

        var fn = "anon";
        var m = caller.match(/at (?:async )?([^\s(]+)\s*\(/); // chrome: "    at fn (url:line:col)"
        if (m === null)
        {
            m = caller.match(/([^@\s]+)@/);
        }    // firefox: "fn@url:line:col"
        if (m !== null && m[1])
        {
            fn = m[1];
        }

        var line = "?";
        var lm = caller.match(/:(\d+):\d+\)?\s*$/);
        if (lm !== null)
        {
            line = lm[1];
        }

        return fn + ":" + line;
    }

    /**
     * @brief checks the minimum log level and muted client contexts
     *
     * @param string level -> debug, info, warn or error
     *
     * @return boolean -> true when this level and context are allowed
     */
    function passes(level)
    {
        if (LEVELS[level] < LEVELS[cfg.level || "debug"])
        {
            return false;
        }
        for (var i = 0; i < cfg.mute.length; i++)
        {
            if (cfg.mute[i] == context_name)
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief wraps a console method with context, caller and optional time prefixes
     *
     * @param string level -> severity used for filtering
     * @param function native_fn -> original console method
     *
     * @return function -> logger that applies the current logging options
     */
    function make(level, native_fn)
    {
        return function ()
        {
            if (cfg.enabled === false)
            {
                return;
            }                                 // global master switch, wins over everything
            if (cfg.raw)
            {
                return native_fn.apply(console, arguments);
            }
            if (passes(level) === false)
            {
                return;
            }

            // time is skipped (not even computed) when disabled
            var time_part = (cfg.time === false) ? "" : ("[" + stamp() + "]");
            var tag = "[" + context_name + "][" + origin() + "]" + time_part;
            var args = Array.prototype.slice.call(arguments);
            args.unshift("%c" + tag + "%c", "color:" + color + ";font-weight:bold", "color:inherit");
            return native_fn.apply(console, args);
        };
    }

    if (typeof console !== "undefined")
    {
        var native_log   = console.log   ? console.log.bind(console)   : function ()
        {
        };
        var native_info  = console.info  ? console.info.bind(console)  : native_log;
        var native_warn  = console.warn  ? console.warn.bind(console)  : native_log;
        var native_error = console.error ? console.error.bind(console) : native_log;

        console.log   = make("info",  native_log);
        console.debug = make("debug", native_log);
        console.info  = make("info",  native_info);
        console.warn  = make("warn",  native_warn);
        console.error = make("error", native_error);
    }
})();
