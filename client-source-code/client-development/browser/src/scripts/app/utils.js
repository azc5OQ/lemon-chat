// utils.js is embedded in template.html along with the other client files, and in the node bundle
// it holds the small helpers every file uses: utils__storage_get / utils__storage_set around localStorage, sleep,
// utils__custom_log / utils__custom_alert (the on-page log textarea and the a-toast element), the byte helpers
// (base64 both ways, a null-terminator cut, a plain-javascript sha256) and the touch-device check

// ---- helpers ----

/**
 * @brief reads one key from localStorage
 *        the storage is missing in node and workers and throws in some private windows; both cases
 *        read as the fallback
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

/**
 * @brief writes one key to localStorage; a failed write only warns
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
 * @brief promise-based delay, for awaiting inside async loops
 *
 * @param number ms -> the delay in milliseconds
 *
 * @return promise resolves after the delay
 */
function utils__sleep(ms)
{
    return new Promise(resolve => window.setTimeout(resolve, ms));
}

/**
 * @brief appends a timestamped line to the on-page log textarea
 *        a worker has no textarea and posts the text to the main thread instead; a touch device
 *        skips logging entirely, and past ~50 KB only the newest half of the log is kept
 *
 * @param string text -> the line to log
 *
 * @return void
 */
function utils__custom_log(text)
{
    // g_textarea_log only exists in main thread
    // in case its null, use global log

    if (g_textarea_log == null)
    {
        global.postMessage({
            type: "log",
            value: text
        });
        return;
    }

    // keep the log bounded (phones keep the page alive for days, and every
    // append copies the whole string): past ~50 KB keep only the newest half
    if (g_textarea_log.value.length > 50000)
    {
        g_textarea_log.value = g_textarea_log.value.slice(-25000);
    }

    // append via .value, never textContent: textContent is only the textarea's
    // DEFAULT value - the first manual edit detaches the display from it forever
    let aa = new Date();
    let a = aa.toLocaleTimeString();
    g_textarea_log.value += a + " | " + text + "\n";
}

/**
 * @brief shows the a-toast element with the given message
 *
 * @param string message -> the text to show
 *
 * @return void
 */
function utils__custom_alert(message = "")
{
    const el = document.querySelector("a-toast");

    el.style.visibility = 'visible';
    el.innerHTML = message;
}

/**
 * @brief hides the a-toast element and replaces its text
 *
 * @param string message -> the text left in the hidden toast
 *
 * @return void
 */
function utils__hide_custom_alert(message = "")
{
    const el = document.querySelector("a-toast");

    el.style.visibility = 'hidden';
    el.innerHTML = message;
}

// ---- byte encoding ----

/**
 * @brief encodes bytes as a base64 string in plain javascript
 *
 * @param array arr -> the bytes, as an array or a typed array of numbers 0-255
 *
 * @return string the base64 text, padded with "="
 */
function utils__bytesToBase64String(arr)
{
    const abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; // base64 alphabet
    const bin = n => n.toString(2)
        .padStart(8, 0); // convert num to 8-bit binary string
    const l = arr.length;
    let result = '';

    for (let i = 0; i <= (l - 1) / 3; i++)
    {
        let c1 = i * 3 + 1 >= l; // case when "=" is on end
        let c2 = i * 3 + 2 >= l; // case when "=" is on end
        let chunk = bin(arr[3 * i]) + bin(c1 ? 0 : arr[3 * i + 1]) + bin(c2 ? 0 : arr[3 * i + 2]);
        let r = chunk.match(/.{1,6}/g)
            .map((x, j) => j == 3 && c2 ? '=' : (j == 2 && c1 ? '=' : abc[+('0b' + x)]));
        result += r.join('');
    }

    return result;
}

/**
 * @brief decodes a base64 string to bytes in plain javascript
 *
 * @param string str -> the base64 text
 *
 * @return array the bytes as numbers 0-255
 */
function utils__base64StringToBytes(str)
{
    const abc = [..."ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"]; // base64 alphabet
    let result = [];

    for (let i = 0; i < str.length / 4; i++)
    {
        let chunk = [...str.slice(4 * i, 4 * i + 4)];
        let bin = chunk.map(x => abc.indexOf(x)
            .toString(2)
            .padStart(6, 0))
            .join('');
        let bytes = bin.match(/.{1,8}/g)
            .map(x => +('0b' + x));
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

var sha256 = function sha256(ascii)
{
    function rightRotate(value, amount)
    {
        return (value >>> amount) | (value << (32 - amount));
    };

    var mathPow = Math.pow;
    var maxWord = mathPow(2, 32);
    var lengthProperty = 'length';
    var i, j; // Used as a counter across the whole file
    var result = '';

    var words = [];
    var asciiBitLength = ascii[lengthProperty] * 8;

    //* caching results is optional - remove/add slash from front of this line to toggle
    // Initial hash value: first 32 bits of the fractional parts of the square roots of the first 8 primes
    // (we actually calculate the first 64, but extra values are just ignored)
    var hash = sha256.h = sha256.h || [];
    // Round constants: first 32 bits of the fractional parts of the cube roots of the first 64 primes
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

    ascii += '\x80'; // Append Æ‡' bit (plus zero padding)
    while (ascii[lengthProperty] % 64 - 56) ascii += '\x00' // More zero padding
    for (i = 0; i < ascii[lengthProperty]; i++)
    {
        j = ascii.charCodeAt(i);
        if (j >> 8) return; // ASCII check: only accept characters in range 0-255
        words[i >> 2] |= j << ((3 - i) % 4) * 8;
    }
    words[words[lengthProperty]] = ((asciiBitLength / maxWord) | 0);
    words[words[lengthProperty]] = (asciiBitLength);

    // process each chunk
    for (j = 0; j < words[lengthProperty];)
    {
        var w = words.slice(j, j += 16); // The message is expanded into 64 words as part of the iteration
        var oldHash = hash;
        // This is now the undefinedworking hash", often labelled as variables a...g
        // (we have to truncate as well, otherwise extra entries at the end accumulate
        hash = hash.slice(0, 8);

        for (i = 0; i < 64; i++)
        {
            var i2 = i + j;
            // Expand the message into 64 words
            // Used below if
            var w15 = w[i - 15],
                w2 = w[i - 2];

            // Iterate
            var a = hash[0],
                e = hash[4];
            var temp1 = hash[7] +
                (rightRotate(e, 6) ^ rightRotate(e, 11) ^ rightRotate(e, 25)) // S1
                +
                ((e & hash[5]) ^ ((~e) & hash[6])) // ch
                +
                k[i]
                // Expand the message schedule if needed
                +
                (w[i] = (i < 16) ? w[i] : (
                    w[i - 16] +
                    (rightRotate(w15, 7) ^ rightRotate(w15, 18) ^ (w15 >>> 3)) // s0
                    +
                    w[i - 7] +
                    (rightRotate(w2, 17) ^ rightRotate(w2, 19) ^ (w2 >>> 10)) // s1
                ) | 0);
            // This is only used once, so *could* be moved below, but it only saves 4 bytes and makes things unreadble
            var temp2 = (rightRotate(a, 2) ^ rightRotate(a, 13) ^ rightRotate(a, 22)) // S0
                +
                ((a & hash[1]) ^ (a & hash[2]) ^ (hash[1] & hash[2])); // maj

            hash = [(temp1 + temp2) | 0].concat(hash); // We don't bother trimming off the extra ones, they're harmless as long as we're truncating when we do the slice()
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

// ---- platform detection ----

/**
 * @brief decides whether this device gets the touch interface
 *        detection in layers, most reliable signal first; any hit counts, because every layer
 *        covers the blind spots of the ones below it
 *
 * @return boolean true for a touch device
 */
function utils__detect_touch_device()
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
