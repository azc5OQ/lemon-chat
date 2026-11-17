// provides shared storage, logging, encoding and device helpers

// shared helpers

/**
 * @brief reads local storage or returns the fallback when storage is unavailable
 *
 * @param string key -> the key to read
 * @param string|null fallback -> handed back when the key is absent or the storage is unavailable
 *
 * @return string|null the stored value, else the fallback
 */
function utils__storage_get(key, fallback = null)
{
    try
    {
        if (typeof localStorage === "undefined")
        {
            return fallback;
        }

        let value = localStorage.getItem(key);
        return (value == null) ? fallback : value;
    }
    catch (e)
    {
        return fallback;
    }
}

// The interface size (local setting) draws the whole page with CSS zoom on the root element. Mouse events and
// getBoundingClientRect report coordinates in "coordinate units"; left and top of an element are CSS pixels of the
// zoomed page. Chrome 128 and newer and Firefox report screen pixels, so one CSS pixel is "zoom" coordinate units;
// older engines (Android WebView 116) report CSS pixels of the zoomed page, so the factor is 1. The factor is
// measured, not derived from the browser version.
var g_position_scale = 1;

// size of the window in coordinate units, measured together with g_position_scale
var g_viewport_size = { width: 0, height: 0 };

/**
 * @brief measures g_position_scale and g_viewport_size with an invisible element. Called when the interface
 * size changes and when the window is resized
 *
 * @return void
 */
function utils__measure_position_scale()
{
    if (typeof document === "undefined" || document.body == null)
    {
        return;
    }

    // place the probe
    let probe = document.createElement("div");
    probe.style.cssText = "position: fixed; left: 100px; top: 0; width: 1px; height: 1px; visibility: hidden; pointer-events: none;";
    let frame = document.createElement("div");
    frame.style.cssText = "position: fixed; inset: 0; visibility: hidden; pointer-events: none;";
    document.body.appendChild(probe);
    document.body.appendChild(frame);

    // read both boxes
    let left = probe.getBoundingClientRect().left;
    let frame_box = frame.getBoundingClientRect();
    g_position_scale = (left > 0) ? left / 100 : 1;
    g_viewport_size = { width: frame_box.width || window.innerWidth, height: frame_box.height || window.innerHeight };
    probe.remove();
    frame.remove();
}

/**
 * @brief converts a position in coordinate units (from a mouse event or getBoundingClientRect) into the CSS pixels
 * that left and top of a fixed or absolute element need
 *
 * @param number value -> position in coordinate units
 *
 * @return string the CSS length, for example "120px"
 */
function utils__position_px(value)
{
    return (value / g_position_scale) + "px";
}

/**
 * @brief width of the window in coordinate units; replaces window.innerWidth in position calculations
 *
 * @return number the width
 */
function utils__viewport_width()
{
    return g_viewport_size.width || window.innerWidth;
}

/**
 * @brief height of the window in coordinate units; replaces window.innerHeight in position calculations
 *
 * @return number the height
 */
function utils__viewport_height()
{
    return g_viewport_size.height || window.innerHeight;
}

/**
 * @brief width of the window in CSS pixels of the page; replaces window.innerWidth where it is combined with
 * offsetWidth, offsetLeft and other CSS pixel values
 *
 * @return number the width
 */
function utils__viewport_css_width()
{
    return utils__viewport_width() / g_position_scale;
}

/**
 * @brief height of the window in CSS pixels of the page; replaces window.innerHeight where it is combined with
 * offsetHeight, offsetTop and other CSS pixel values
 *
 * @return number the height
 */
function utils__viewport_css_height()
{
    return utils__viewport_height() / g_position_scale;
}

/**
 * @brief writes local storage and logs a warning if the write fails
 *
 * @param string key -> the key to write
 * @param string value -> the value to store
 *
 * @return void
 */
function utils__storage_set(key, value)
{
    try
    {
        localStorage.setItem(key, value);
    }
    catch (e)
    {
        console.warn("failed to persist " + key + ":", e.message);
    }
}

/**
 * @brief waits for a delay without blocking execution
 *
 * @param number ms -> the delay in milliseconds
 *
 * @return promise resolves after the delay
 */
function utils__sleep(ms)
{
    return new Promise(resolve => window.setTimeout(resolve, ms));
}

// the log entries waiting to be added to the log: { text, kind, time }
var utils_log_pending = [];
var utils_log_timer = null;

// maximum log lines. remove older entries
var UTILS_LOG_MAX_LINES = 1000;

/**
 * @brief batches log updates every 100 ms to avoid delaying message handling
 *
 * @param string text -> text to log, split into entries at newline characters
 * @param string kind -> permission_denied displays a refused request in red, otherwise an ordinary entry
 *
 * @return void
 */
function utils__custom_log(text, kind)
{
    // queue the line
    utils_log_pending.push({ text: String(text), kind: (typeof kind === "string") ? kind : "", time: new Date().toLocaleTimeString() });

    // drop oldest lines
    if (utils_log_pending.length > UTILS_LOG_MAX_LINES)
    {
        utils_log_pending.splice(0, utils_log_pending.length - UTILS_LOG_MAX_LINES);
    }

    // timer already running
    if (utils_log_timer != null)
    {
        return;
    }

    // show lines later
    utils_log_timer = setTimeout(function()
    {
        let entries = utils_log_pending;
        utils_log_pending = [];
        utils_log_timer = null;
        if (g_textarea_log == null)
        {
            // a worker has no log element: the page adds the lines
            global.postMessage({ type: "log", value: entries.map(function(entry)
            {
                return entry.text;
            }).join("\n") });
            return;
        }
        utils__append_log_entries(entries);
    }, 100);
}

/**
 * @brief appends nonempty log lines and preserves the current scroll position
 *
 * @param array entries -> { text, kind, time } objects collected by utils__custom_log
 *
 * @return void
 */
function utils__append_log_entries(entries)
{
    let is_end_visible = true;
    let line = null;
    let texts = null;

    // Node's log element stores text without rendering
    if (G_HAS_DOM == false)
    {
        for (let entry of entries)
        {
            g_textarea_log.value = String(g_textarea_log.value || "").slice(-25000) + entry.time + " | " + entry.text + "\n";
        }
        return;
    }

    // remember scroll position
    is_end_visible = g_textarea_log.scrollTop + g_textarea_log.clientHeight >= g_textarea_log.scrollHeight - 20;

    // add the lines
    for (let entry of entries)
    {
        texts = entry.text.split("\n");
        for (let i = 0; i < texts.length; i++)
        {
            if (texts[i].trim().length == 0)
            {
                continue;
            }
            line = document.createElement("div");
            line.className = (entry.kind == "permission_denied") ? "log-line log-line-permission-denied" : "log-line";
            line.textContent = entry.time + " | " + texts[i];
            g_textarea_log.appendChild(line);
        }
    }

    // remove oldest lines
    while (g_textarea_log.childElementCount > UTILS_LOG_MAX_LINES)
    {
        g_textarea_log.firstElementChild.remove();
    }

    // keep end visible
    if (is_end_visible == true)
    {
        g_textarea_log.scrollTop = g_textarea_log.scrollHeight;
    }
}

/**
 * @brief shows the a-toast element with plain text, including untrusted names and error messages
 *
 * @param string message -> the text to show
 * @param string owner -> optional notice owner, replaced by a later ordinary alert
 *
 * @return void
 */
function utils__custom_alert(message = "", owner = "")
{
    const el = document.querySelector("a-toast");

    el.dataset.alertOwner = owner;
    el.style.visibility = 'visible';
    el.textContent = message;
}

/**
 * @brief hides the a-toast element and replaces its text
 *
 * @param string message -> the text left in the hidden toast
 * @param string|null owner -> when provided, hide only a notice belonging to this owner
 *
 * @return void
 */
function utils__hide_custom_alert(message = "", owner = null)
{
    const el = document.querySelector("a-toast");

    if (owner !== null && el.dataset.alertOwner !== owner)
    {
        return;
    }
    delete el.dataset.alertOwner;
    el.style.visibility = 'hidden';
    el.textContent = message;
}

// byte encoding

/**
 * @brief encodes bytes as a base64 string in plain javascript
 *
 * @param array arr -> bytes as an array or typed array, with values from 0 to 255
 *
 * @return string the base64 text, padded with "="
 */
function utils__bytesToBase64String(arr)
{
    const abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; // base64 alphabet
    const bin = n => n.toString(2) .padStart(8, 0); // convert each byte to eight binary digits
    const l = arr.length;
    let result = '';

    for (let i = 0; i <= (l - 1) / 3; i++)
    {
        let c1 = i * 3 + 1 >= l; // case when "=" is on end
        let c2 = i * 3 + 2 >= l; // case when "=" is on end
        let chunk = bin(arr[3 * i]) + bin(c1 ? 0 : arr[3 * i + 1]) + bin(c2 ? 0 : arr[3 * i + 2]);
        let r = chunk.match(/.{1,6}/g) .map((x, j) => j == 3 && c2 ? '=' : (j == 2 && c1 ? '=' : abc[+('0b' + x)]));
        result += r.join('');
    }

    return result;
}

/**
 * @brief decodes a base64 string to bytes in plain javascript
 *
 * @param string str -> the base64 text
 *
 * @return array bytes with values from 0 to 255
 */
function utils__base64StringToBytes(str)
{
    const abc = [..."ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"]; // base64 alphabet
    let result = [];

    for (let i = 0; i < str.length / 4; i++)
    {
        let chunk = [...str.slice(4 * i, 4 * i + 4)];
        let bin = chunk.map(x => abc.indexOf(x) .toString(2) .padStart(6, 0)) .join('');
        let bytes = bin.match(/.{1,8}/g) .map(x => +('0b' + x));
        result.push(...bytes.slice(0, 3 - (str[4 * i + 2] == "=") - (str[4 * i + 3] == "=")));
    }
    return result;
}


/**
 * @brief cuts a string at its first null character, the way C strings end
 *
 * @param string str -> the text, possibly zero padded
 *
 * @return string the text before the first null, the whole string when there is none
 */
function utils__substringByNullTerminator(str)
{
    for (var x = 0; x < str.length; x++)
    {
        if (str.charCodeAt(x) == 0)
        {
            return str.substring(0, x);
        }
    }
    return str;
}

/**
 * @brief hashes a byte string with SHA-256
 *
 * @param string ascii -> text whose character codes are bytes from 0 to 255
 *
 * @return string|undefined -> hexadecimal digest or undefined for a character code above 255
 */
var sha256 = function sha256(ascii)
{
    /**
     * @brief rotates a 32-bit hash word to the right
     *
     * @param number value -> word to rotate
     * @param number amount -> number of bit positions
     *
     * @return number -> rotated 32-bit word
     */
    function rightRotate(value, amount)
    {
        return (value >>> amount) | (value << (32 - amount));
    };

    var mathPow = Math.pow;
    var maxWord = mathPow(2, 32);
    var lengthProperty = 'length';
    var i, j; // used as a counter across the whole file
    var result = '';

    var words = [];
    var asciiBitLength = ascii[lengthProperty] * 8;

    //* caching results is optional - remove/add slash from front of this line to toggle
    // initialize hash words from the fractional square roots of the first primes
    var hash = sha256.h = sha256.h || [];
    // round constants: first 32 bits of the fractional parts of the cube roots of the first 64 primes
    var k = sha256.k = sha256.k || [];
    var primeCounter = k[lengthProperty];
    /*/
    var hash = [], k = [];
    var primeCounter = 0;
    //*/

    var isComposite = {};
    for (var candidate = 2; primeCounter < 64; candidate++)
    {
        if (!isComposite[candidate])
        {
            for (i = 0; i < 313; i += candidate)
            {
                isComposite[i] = candidate;
            }
            hash[primeCounter] = (mathPow(candidate, .5) * maxWord) | 0;
            k[primeCounter++] = (mathPow(candidate, 1 / 3) * maxWord) | 0;
        }
    }

    ascii += '\x80'; // append the leading padding bit
    while (ascii[lengthProperty] % 64 - 56)
    {
        ascii += '\x00' // more zero padding
    }
    for (i = 0; i < ascii[lengthProperty]; i++)
    {
        j = ascii.charCodeAt(i);
        if (j >> 8)
        {
            return; // accept only character codes from 0 to 255
        }
        words[i >> 2] |= j << ((3 - i) % 4) * 8;
    }
    words[words[lengthProperty]] = ((asciiBitLength / maxWord) | 0);
    words[words[lengthProperty]] = (asciiBitLength);

    // process each chunk
    for (j = 0; j < words[lengthProperty];)
    {
        var w = words.slice(j, j += 16); // the message is expanded into 64 words as part of the iteration
        var oldHash = hash;
        // copy the eight working hash words for this block
        hash = hash.slice(0, 8);

        for (i = 0; i < 64; i++)
        {
            var i2 = i + j;
            // expand the message into 64 words
            // used below if
            var w15 = w[i - 15],
                w2 = w[i - 2];

            // iterate
            var a = hash[0],
                e = hash[4];
            var temp1 = hash[7] + (rightRotate(e, 6) ^ rightRotate(e, 11) ^ rightRotate(e, 25)) // S1
                +
                ((e & hash[5]) ^ ((~e) & hash[6])) // ch
                +
                k[i]
                // expand the message schedule if needed
                +
                (w[i] = (i < 16) ? w[i] : (
                    w[i - 16] + (rightRotate(w15, 7) ^ rightRotate(w15, 18) ^ (w15 >>> 3)) // s0
                    +
                    w[i - 7] +
                    (rightRotate(w2, 17) ^ rightRotate(w2, 19) ^ (w2 >>> 10)) // s1
                ) | 0);
            // compute S0 for this round
            var temp2 = (rightRotate(a, 2) ^ rightRotate(a, 13) ^ rightRotate(a, 22)) // S0
                +
                ((a & hash[1]) ^ (a & hash[2]) ^ (hash[1] & hash[2])); // maj

            hash = [(temp1 + temp2) | 0].concat(hash); // the next slice keeps only the eight hash words
            hash[4] = (hash[4] + temp1) | 0;
        }

        for (i = 0; i < 8; i++)
        {
            hash[i] = (hash[i] + oldHash[i]) | 0;
        }
    }

    for (i = 0; i < 8; i++)
    {
        for (j = 3; j + 1; j--)
        {
            var b = (hash[i] >> (j * 8)) & 255;
            result += ((b < 16) ? 0 : '') + b.toString(16);
        }
    }
    return result;
};

// platform detection

/**
 * @brief checks browser and device signals for touch input support
 *
 * @return boolean true for a touch device
 */
function utils__detect_touch_device()
{
    // prefer Chromium's mobile client hint when available
    if (typeof navigator !== "undefined" && navigator.userAgentData != null && navigator.userAgentData.mobile === true)
    {
        return true;
    }

    let user_agent_string = (typeof navigator !== "undefined" && typeof navigator.userAgent === "string") ? navigator.userAgent : "";

    // check Android and iOS when pointer signals resemble a desktop
    if (/android|iphone|ipad|ipod/i.test(user_agent_string) == true)
    {
        return true;
    }

    // identify iPadOS when it reports a Mac platform with multiple touch points
    if (/macintosh/i.test(user_agent_string) == true && typeof navigator !== "undefined" && navigator.maxTouchPoints > 1)
    {
        return true;
    }

    // use CSS input capabilities as the final fallback
    if (typeof window !== "undefined" && window.matchMedia("(pointer: coarse) and (hover: none)").matches)
    {
        return true;
    }

    return false;
}
