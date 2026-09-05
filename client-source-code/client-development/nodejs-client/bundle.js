// Headless bundle template. Mirrors ../browser/src/template.html's second <script> block,
// minus the rendering and audio halves. Expanded by build-node.py into bundle.js.
//
// ORDER IS LOAD-BEARING, for three separate reasons:
//
//  1. dom-shim.js must run FIRST and OUTSIDE moduleFactory. aes-js.js line 17 does
//     `var global = (self || window || global || {})`, so `window` has to already exist by the time
//     the factory body runs, or `global` comes out as `{}` and every `global.*` read in the tree
//     misses.
//  2. utils.js sits before aes-js.js in the browser too, i.e. OUTSIDE the factory. Keep it there:
//     the workers never get it, and matching that keeps one mental model of what is in scope.
//  3. aes-js.js OPENS `function moduleFactory()` and main.js CLOSES it with `}));`. Everything
//     between is one function body. Nothing may be inserted after main.js that expects to see the
//     factory's locals - there is no scope out there.

// Headless stand-ins for the browser globals the client touches, so messages.js and main.js run
// unchanged under nodejs-mobile. Bails in a browser (document already exists); absorbing every ui
// write here is correct, not a workaround - in node there is no ui.
//
// Side effect: window becomes globalThis and document/navigator exist, so an npm package that
// sniffs `typeof document` takes its browser path here. Audit future deps for that.

if (typeof document === "undefined")
{
    // reports length 0 (paint loops correctly do nothing) but answers any index with a dead
    // element, because the client indexes collections blind: c[0], c[count - 1]
    function make_dead_collection()
    {
        return new Proxy([], {
            get: function(target, key)
            {
                if (key === "length") { return 0; }
                if (typeof key === "string" && /^-?\d+$/.test(key)) { return make_dead_element(); }
                return target[key];
            }
        });
    }

    // swallows any property set and answers any method call with another of itself, so chains
    // like getElementById(x).style.display = "none" cannot throw
    function make_dead_element()
    {
        let element = {
            style: {
                // channel_tree__apply_avatar_to_ui calls these unconditionally; a plain {} would throw
                setProperty: function() {},
                removeProperty: function() {},
                getPropertyValue: function() { return ""; }
            },
            dataset: {},
            classList: {
                add: function() {},
                remove: function() {},
                toggle: function() {},
                // false means "not in that visual state", which skips the render branch guarding it
                contains: function() { return false; }
            },
            children: make_dead_collection(),
            childNodes: make_dead_collection(),
            innerHTML: "",
            textContent: "",
            innerText: "",
            value: "",
            checked: false,
            id: "",
            className: "",
            appendChild: function(child) { return child; },
            removeChild: function(child) { return child; },
            insertAdjacentHTML: function() {},
            insertBefore: function(child) { return child; },
            setAttribute: function() {},
            getAttribute: function() { return null; },
            removeAttribute: function() {},
            addEventListener: function() {},
            removeEventListener: function() {},
            remove: function() {},
            focus: function() {},
            blur: function() {},
            click: function() {},
            scrollIntoView: function() {},
            getBoundingClientRect: function() { return { top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 }; },
            querySelector: function() { return make_dead_element(); },
            querySelectorAll: function() { return make_dead_collection(); },
            getElementsByClassName: function() { return make_dead_collection(); },
            getElementsByTagName: function() { return make_dead_collection(); },

            // getters, not plain properties - a plain property would recurse during construction.
            // messages.js dereferences lastElementChild directly, so these two return elements
            get firstElementChild() { return make_dead_element(); },
            get lastElementChild() { return make_dead_element(); },

            // null, matching the empty children - and `while (x.firstChild != null)` drain loops
            // must terminate, which a dead element would prevent
            get firstChild() { return null; },
            get lastChild() { return null; }
        };

        return element;
    }

    // shared by document.location and window.location
    let dead_location = {
        href: "",
        protocol: "http:",
        hostname: "",
        host: "",
        port: "",
        reload: function() {}
    };

    global.document = {
        // never null: call sites do getElementById(x).something = y directly
        getElementById: function() { return make_dead_element(); },
        querySelector: function() { return make_dead_element(); },
        querySelectorAll: function() { return make_dead_collection(); },
        getElementsByClassName: function() { return make_dead_collection(); },
        getElementsByTagName: function() { return make_dead_collection(); },
        createElement: function() { return make_dead_element(); },
        createTextNode: function() { return make_dead_element(); },
        createDocumentFragment: function() { return make_dead_element(); },
        addEventListener: function() {},
        removeEventListener: function() {},
        body: make_dead_element(),
        documentElement: make_dead_element(),
        location: dead_location
    };

    // window IS the global object, as in a browser - the vendor umds do `root.aesjs = ...` with
    // root = window, and the app uses the bare name. a separate object breaks that silently
    global.window = global;

    // only what node does not already provide. no timer wrappers: node has the real ones, and
    // wrapping them on the global would make them call themselves
    let window_members = {
        addEventListener: function() {},
        removeEventListener: function() {},
        getComputedStyle: function() { return {}; },
        innerWidth: 0,
        innerHeight: 0,
        location: dead_location,
        scrollTo: function() {},
        requestAnimationFrame: function(callback) { return setTimeout(callback, 16); },
        cancelAnimationFrame: function(id) { return clearTimeout(id); }
    };

    for (let member_name in window_members)
    {
        if (typeof global[member_name] === "undefined")
        {
            global[member_name] = window_members[member_name];
        }
    }

    global.navigator = { userAgent: "nodejs-mobile" };

    // every ui entry point becomes a no-op; a proxy so newly added UI functions cannot crash node
    global.UI = new Proxy({}, {
        get: function() { return function() {}; }
    });

    // used as g_sound_effects.x.play()
    global.g_sound_effects = new Proxy({}, {
        get: function() {
            return { play: function() {}, pause: function() {}, currentTime: 0, volume: 0 };
        }
    });

    // ---- node platform flags ----

    // aes-js picks its `global` from self first; point it at the shim window
    if (typeof globalThis.self === "undefined") { globalThis.self = globalThis.window; }

    // normally declared by worker-entry.js, which is not in the bundle
    globalThis.THREAD_NAME = "node-service";
    globalThis.DBG_WORKER_BOOT_LOG = false;

    // js-sha256 sniffs node and eval-requires crypto, which vm.runInThisContext lacks
    globalThis.JS_SHA256_NO_NODE_JS = true;
}

// ---------------------------------------------------------------------------------------------
// LOAD-BEARING - do not "improve":
//   1. collections report length 0: a non-zero length turns messages.js's `while (lights.length)`
//      into an infinite loop, because classList.remove is a no-op here
//   2. getElementById / querySelector never return null: call sites dereference directly
//   3. sibling getters (nextSibling etc.) stay ABSENT: main.js walks siblings in
//      `while (node != null)` loops that terminate only because undefined != null is false
//   4. ui.js and sounds.js stay OUT of the node bundle: their `var UI` / `var g_sound_effects`
//      shadow the proxies above, and sounds.js throws on load under this shim
// how each rule was earned: lemonchat-study/nodejs-mobile-headless-audit.md
// ---------------------------------------------------------------------------------------------


        (function (root, factory)
        {
            if (typeof define === 'function' && define.amd)
            {
                define([], factory);
            }
            else if (typeof module === 'object' && typeof exports !== 'undefined')
            {
                module.exports = factory();
            }
            else
            {
                root.what = factory();
            }
        }(this, function moduleFactory()
        {
            var global = (function ()
            {
                // alternative method, similar to `Function('return this')()`
                // but without using `eval` (which is disabled when
                // using Content Security Policy).

                if (typeof self !== 'undefined') { return self; }
                if (typeof window !== 'undefined') { return window; }
                if (typeof global !== 'undefined') { return global; }
                // When running tests none of the above have been defined
                return {};
            })();

            (function (root)
            {

                function checkInt(value)
                {
                    return (parseInt(value) === value);
                }
                ``

                function checkInts(arrayish)
                {
                    if (!checkInt(arrayish.length))
                    {
                        return false;
                    }

                    for (var i = 0; i < arrayish.length; i++)
                    {
                        if (!checkInt(arrayish[i]) || arrayish[i] < 0 || arrayish[i] > 255)
                        {
                            return false;
                        }
                    }

                    return true;
                }

                function coerceArray(arg, copy)
                {

                    // ArrayBuffer view
                    if (arg.buffer && ArrayBuffer.isView(arg) && arg.name === 'Uint8Array')
                    {

                        if (copy)
                        {
                            if (arg.slice)
                            {
                                arg = arg.slice();
                            }
                            else
                            {
                                arg = Array.prototype.slice.call(arg);
                            }
                        }

                        return arg;
                    }

                    // It's an array; check it is a valid representation of a byte
                    if (Array.isArray(arg))
                    {
                        if (!checkInts(arg))
                        {
                            throw new Error('Array contains invalid value: ' + arg);
                        }

                        return new Uint8Array(arg);
                    }

                    // Something else, but behaves like an array (maybe a Buffer? Arguments?)
                    if (checkInt(arg.length) && checkInts(arg))
                    {
                        return new Uint8Array(arg);
                    }

                    throw new Error('unsupported array-like object');
                }

                function createArray(length)
                {
                    return new Uint8Array(length);
                }

                function copyArray(sourceArray, targetArray, targetStart, sourceStart, sourceEnd)
                {
                    if (sourceStart != null || sourceEnd != null)
                    {
                        if (sourceArray.slice)
                        {
                            sourceArray = sourceArray.slice(sourceStart, sourceEnd);
                        }
                        else
                        {
                            sourceArray = Array.prototype.slice.call(sourceArray, sourceStart, sourceEnd);
                        }
                    }
                    targetArray.set(sourceArray, targetStart);
                }


                var convertUtf8 = (function ()
                {
                    function toBytes(text)
                    {
                        var result = [],
                            i = 0;
                        text = encodeURI(text);
                        while (i < text.length)
                        {
                            var c = text.charCodeAt(i++);

                            // if it is a % sign, encode the following 2 bytes as a hex value
                            if (c === 37)
                            {
                                result.push(parseInt(text.substr(i, 2), 16))
                                i += 2;

                                // otherwise, just the actual byte
                            }
                            else
                            {
                                result.push(c)
                            }
                        }

                        return coerceArray(result);
                    }

                    function fromBytes(bytes)
                    {
                        var result = [],
                            i = 0;

                        while (i < bytes.length)
                        {
                            var c = bytes[i];

                            if (c < 128)
                            {
                                result.push(String.fromCharCode(c));
                                i++;
                            }
                            else if (c > 191 && c < 224)
                            {
                                result.push(String.fromCharCode(((c & 0x1f) << 6) | (bytes[i + 1] & 0x3f)));
                                i += 2;
                            }
                            else
                            {
                                result.push(String.fromCharCode(((c & 0x0f) << 12) | ((bytes[i + 1] & 0x3f) << 6) | (bytes[i + 2] & 0x3f)));
                                i += 3;
                            }
                        }

                        return result.join('');
                    }

                    return {
                        toBytes: toBytes,
                        fromBytes: fromBytes,
                    }
                })();

                var convertHex = (function ()
                {
                    function toBytes(text)
                    {
                        var result = [];
                        for (var i = 0; i < text.length; i += 2)
                        {
                            result.push(parseInt(text.substr(i, 2), 16));
                        }

                        return result;
                    }

                    // http://ixti.net/development/javascript/2011/11/11/base64-encodedecode-of-utf8-in-browser-with-js.html
                    var Hex = '0123456789abcdef';

                    function fromBytes(bytes)
                    {
                        var result = [];
                        for (var i = 0; i < bytes.length; i++)
                        {
                            var v = bytes[i];
                            result.push(Hex[(v & 0xf0) >> 4] + Hex[v & 0x0f]);
                        }
                        return result.join('');
                    }

                    return {
                        toBytes: toBytes,
                        fromBytes: fromBytes,
                    }
                })();


                // Number of rounds by keysize
                var numberOfRounds = {
                    16: 10,
                    24: 12,
                    32: 14
                }

                // Round constant words
                var rcon = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36, 0x6c, 0xd8, 0xab, 0x4d, 0x9a, 0x2f, 0x5e, 0xbc, 0x63, 0xc6, 0x97, 0x35, 0x6a, 0xd4, 0xb3, 0x7d, 0xfa, 0xef, 0xc5, 0x91];

                // S-box and Inverse S-box (S is for Substitution)
                var S = [0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16];
                var Si = [0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb, 0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb, 0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e, 0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84, 0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06, 0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73, 0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b, 0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4, 0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f, 0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef, 0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61, 0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d];

                // Transformations for encryption
                var T1 = [0xc66363a5, 0xf87c7c84, 0xee777799, 0xf67b7b8d, 0xfff2f20d, 0xd66b6bbd, 0xde6f6fb1, 0x91c5c554, 0x60303050, 0x02010103, 0xce6767a9, 0x562b2b7d, 0xe7fefe19, 0xb5d7d762, 0x4dababe6, 0xec76769a, 0x8fcaca45, 0x1f82829d, 0x89c9c940, 0xfa7d7d87, 0xeffafa15, 0xb25959eb, 0x8e4747c9, 0xfbf0f00b, 0x41adadec, 0xb3d4d467, 0x5fa2a2fd, 0x45afafea, 0x239c9cbf, 0x53a4a4f7, 0xe4727296, 0x9bc0c05b, 0x75b7b7c2, 0xe1fdfd1c, 0x3d9393ae, 0x4c26266a, 0x6c36365a, 0x7e3f3f41, 0xf5f7f702, 0x83cccc4f, 0x6834345c, 0x51a5a5f4, 0xd1e5e534, 0xf9f1f108, 0xe2717193, 0xabd8d873, 0x62313153, 0x2a15153f, 0x0804040c, 0x95c7c752, 0x46232365, 0x9dc3c35e, 0x30181828, 0x379696a1, 0x0a05050f, 0x2f9a9ab5, 0x0e070709, 0x24121236, 0x1b80809b, 0xdfe2e23d, 0xcdebeb26, 0x4e272769, 0x7fb2b2cd, 0xea75759f, 0x1209091b, 0x1d83839e, 0x582c2c74, 0x341a1a2e, 0x361b1b2d, 0xdc6e6eb2, 0xb45a5aee, 0x5ba0a0fb, 0xa45252f6, 0x763b3b4d, 0xb7d6d661, 0x7db3b3ce, 0x5229297b, 0xdde3e33e, 0x5e2f2f71, 0x13848497, 0xa65353f5, 0xb9d1d168, 0x00000000, 0xc1eded2c, 0x40202060, 0xe3fcfc1f, 0x79b1b1c8, 0xb65b5bed, 0xd46a6abe, 0x8dcbcb46, 0x67bebed9, 0x7239394b, 0x944a4ade, 0x984c4cd4, 0xb05858e8, 0x85cfcf4a, 0xbbd0d06b, 0xc5efef2a, 0x4faaaae5, 0xedfbfb16, 0x864343c5, 0x9a4d4dd7, 0x66333355, 0x11858594, 0x8a4545cf, 0xe9f9f910, 0x04020206, 0xfe7f7f81, 0xa05050f0, 0x783c3c44, 0x259f9fba, 0x4ba8a8e3, 0xa25151f3, 0x5da3a3fe, 0x804040c0, 0x058f8f8a, 0x3f9292ad, 0x219d9dbc, 0x70383848, 0xf1f5f504, 0x63bcbcdf, 0x77b6b6c1, 0xafdada75, 0x42212163, 0x20101030, 0xe5ffff1a, 0xfdf3f30e, 0xbfd2d26d, 0x81cdcd4c, 0x180c0c14, 0x26131335, 0xc3ecec2f, 0xbe5f5fe1, 0x359797a2, 0x884444cc, 0x2e171739, 0x93c4c457, 0x55a7a7f2, 0xfc7e7e82, 0x7a3d3d47, 0xc86464ac, 0xba5d5de7, 0x3219192b, 0xe6737395, 0xc06060a0, 0x19818198, 0x9e4f4fd1, 0xa3dcdc7f, 0x44222266, 0x542a2a7e, 0x3b9090ab, 0x0b888883, 0x8c4646ca, 0xc7eeee29, 0x6bb8b8d3, 0x2814143c, 0xa7dede79, 0xbc5e5ee2, 0x160b0b1d, 0xaddbdb76, 0xdbe0e03b, 0x64323256, 0x743a3a4e, 0x140a0a1e, 0x924949db, 0x0c06060a, 0x4824246c, 0xb85c5ce4, 0x9fc2c25d, 0xbdd3d36e, 0x43acacef, 0xc46262a6, 0x399191a8, 0x319595a4, 0xd3e4e437, 0xf279798b, 0xd5e7e732, 0x8bc8c843, 0x6e373759, 0xda6d6db7, 0x018d8d8c, 0xb1d5d564, 0x9c4e4ed2, 0x49a9a9e0, 0xd86c6cb4, 0xac5656fa, 0xf3f4f407, 0xcfeaea25, 0xca6565af, 0xf47a7a8e, 0x47aeaee9, 0x10080818, 0x6fbabad5, 0xf0787888, 0x4a25256f, 0x5c2e2e72, 0x381c1c24, 0x57a6a6f1, 0x73b4b4c7, 0x97c6c651, 0xcbe8e823, 0xa1dddd7c, 0xe874749c, 0x3e1f1f21, 0x964b4bdd, 0x61bdbddc, 0x0d8b8b86, 0x0f8a8a85, 0xe0707090, 0x7c3e3e42, 0x71b5b5c4, 0xcc6666aa, 0x904848d8, 0x06030305, 0xf7f6f601, 0x1c0e0e12, 0xc26161a3, 0x6a35355f, 0xae5757f9, 0x69b9b9d0, 0x17868691, 0x99c1c158, 0x3a1d1d27, 0x279e9eb9, 0xd9e1e138, 0xebf8f813, 0x2b9898b3, 0x22111133, 0xd26969bb, 0xa9d9d970, 0x078e8e89, 0x339494a7, 0x2d9b9bb6, 0x3c1e1e22, 0x15878792, 0xc9e9e920, 0x87cece49, 0xaa5555ff, 0x50282878, 0xa5dfdf7a, 0x038c8c8f, 0x59a1a1f8, 0x09898980, 0x1a0d0d17, 0x65bfbfda, 0xd7e6e631, 0x844242c6, 0xd06868b8, 0x824141c3, 0x299999b0, 0x5a2d2d77, 0x1e0f0f11, 0x7bb0b0cb, 0xa85454fc, 0x6dbbbbd6, 0x2c16163a];
                var T2 = [0xa5c66363, 0x84f87c7c, 0x99ee7777, 0x8df67b7b, 0x0dfff2f2, 0xbdd66b6b, 0xb1de6f6f, 0x5491c5c5, 0x50603030, 0x03020101, 0xa9ce6767, 0x7d562b2b, 0x19e7fefe, 0x62b5d7d7, 0xe64dabab, 0x9aec7676, 0x458fcaca, 0x9d1f8282, 0x4089c9c9, 0x87fa7d7d, 0x15effafa, 0xebb25959, 0xc98e4747, 0x0bfbf0f0, 0xec41adad, 0x67b3d4d4, 0xfd5fa2a2, 0xea45afaf, 0xbf239c9c, 0xf753a4a4, 0x96e47272, 0x5b9bc0c0, 0xc275b7b7, 0x1ce1fdfd, 0xae3d9393, 0x6a4c2626, 0x5a6c3636, 0x417e3f3f, 0x02f5f7f7, 0x4f83cccc, 0x5c683434, 0xf451a5a5, 0x34d1e5e5, 0x08f9f1f1, 0x93e27171, 0x73abd8d8, 0x53623131, 0x3f2a1515, 0x0c080404, 0x5295c7c7, 0x65462323, 0x5e9dc3c3, 0x28301818, 0xa1379696, 0x0f0a0505, 0xb52f9a9a, 0x090e0707, 0x36241212, 0x9b1b8080, 0x3ddfe2e2, 0x26cdebeb, 0x694e2727, 0xcd7fb2b2, 0x9fea7575, 0x1b120909, 0x9e1d8383, 0x74582c2c, 0x2e341a1a, 0x2d361b1b, 0xb2dc6e6e, 0xeeb45a5a, 0xfb5ba0a0, 0xf6a45252, 0x4d763b3b, 0x61b7d6d6, 0xce7db3b3, 0x7b522929, 0x3edde3e3, 0x715e2f2f, 0x97138484, 0xf5a65353, 0x68b9d1d1, 0x00000000, 0x2cc1eded, 0x60402020, 0x1fe3fcfc, 0xc879b1b1, 0xedb65b5b, 0xbed46a6a, 0x468dcbcb, 0xd967bebe, 0x4b723939, 0xde944a4a, 0xd4984c4c, 0xe8b05858, 0x4a85cfcf, 0x6bbbd0d0, 0x2ac5efef, 0xe54faaaa, 0x16edfbfb, 0xc5864343, 0xd79a4d4d, 0x55663333, 0x94118585, 0xcf8a4545, 0x10e9f9f9, 0x06040202, 0x81fe7f7f, 0xf0a05050, 0x44783c3c, 0xba259f9f, 0xe34ba8a8, 0xf3a25151, 0xfe5da3a3, 0xc0804040, 0x8a058f8f, 0xad3f9292, 0xbc219d9d, 0x48703838, 0x04f1f5f5, 0xdf63bcbc, 0xc177b6b6, 0x75afdada, 0x63422121, 0x30201010, 0x1ae5ffff, 0x0efdf3f3, 0x6dbfd2d2, 0x4c81cdcd, 0x14180c0c, 0x35261313, 0x2fc3ecec, 0xe1be5f5f, 0xa2359797, 0xcc884444, 0x392e1717, 0x5793c4c4, 0xf255a7a7, 0x82fc7e7e, 0x477a3d3d, 0xacc86464, 0xe7ba5d5d, 0x2b321919, 0x95e67373, 0xa0c06060, 0x98198181, 0xd19e4f4f, 0x7fa3dcdc, 0x66442222, 0x7e542a2a, 0xab3b9090, 0x830b8888, 0xca8c4646, 0x29c7eeee, 0xd36bb8b8, 0x3c281414, 0x79a7dede, 0xe2bc5e5e, 0x1d160b0b, 0x76addbdb, 0x3bdbe0e0, 0x56643232, 0x4e743a3a, 0x1e140a0a, 0xdb924949, 0x0a0c0606, 0x6c482424, 0xe4b85c5c, 0x5d9fc2c2, 0x6ebdd3d3, 0xef43acac, 0xa6c46262, 0xa8399191, 0xa4319595, 0x37d3e4e4, 0x8bf27979, 0x32d5e7e7, 0x438bc8c8, 0x596e3737, 0xb7da6d6d, 0x8c018d8d, 0x64b1d5d5, 0xd29c4e4e, 0xe049a9a9, 0xb4d86c6c, 0xfaac5656, 0x07f3f4f4, 0x25cfeaea, 0xafca6565, 0x8ef47a7a, 0xe947aeae, 0x18100808, 0xd56fbaba, 0x88f07878, 0x6f4a2525, 0x725c2e2e, 0x24381c1c, 0xf157a6a6, 0xc773b4b4, 0x5197c6c6, 0x23cbe8e8, 0x7ca1dddd, 0x9ce87474, 0x213e1f1f, 0xdd964b4b, 0xdc61bdbd, 0x860d8b8b, 0x850f8a8a, 0x90e07070, 0x427c3e3e, 0xc471b5b5, 0xaacc6666, 0xd8904848, 0x05060303, 0x01f7f6f6, 0x121c0e0e, 0xa3c26161, 0x5f6a3535, 0xf9ae5757, 0xd069b9b9, 0x91178686, 0x5899c1c1, 0x273a1d1d, 0xb9279e9e, 0x38d9e1e1, 0x13ebf8f8, 0xb32b9898, 0x33221111, 0xbbd26969, 0x70a9d9d9, 0x89078e8e, 0xa7339494, 0xb62d9b9b, 0x223c1e1e, 0x92158787, 0x20c9e9e9, 0x4987cece, 0xffaa5555, 0x78502828, 0x7aa5dfdf, 0x8f038c8c, 0xf859a1a1, 0x80098989, 0x171a0d0d, 0xda65bfbf, 0x31d7e6e6, 0xc6844242, 0xb8d06868, 0xc3824141, 0xb0299999, 0x775a2d2d, 0x111e0f0f, 0xcb7bb0b0, 0xfca85454, 0xd66dbbbb, 0x3a2c1616];
                var T3 = [0x63a5c663, 0x7c84f87c, 0x7799ee77, 0x7b8df67b, 0xf20dfff2, 0x6bbdd66b, 0x6fb1de6f, 0xc55491c5, 0x30506030, 0x01030201, 0x67a9ce67, 0x2b7d562b, 0xfe19e7fe, 0xd762b5d7, 0xabe64dab, 0x769aec76, 0xca458fca, 0x829d1f82, 0xc94089c9, 0x7d87fa7d, 0xfa15effa, 0x59ebb259, 0x47c98e47, 0xf00bfbf0, 0xadec41ad, 0xd467b3d4, 0xa2fd5fa2, 0xafea45af, 0x9cbf239c, 0xa4f753a4, 0x7296e472, 0xc05b9bc0, 0xb7c275b7, 0xfd1ce1fd, 0x93ae3d93, 0x266a4c26, 0x365a6c36, 0x3f417e3f, 0xf702f5f7, 0xcc4f83cc, 0x345c6834, 0xa5f451a5, 0xe534d1e5, 0xf108f9f1, 0x7193e271, 0xd873abd8, 0x31536231, 0x153f2a15, 0x040c0804, 0xc75295c7, 0x23654623, 0xc35e9dc3, 0x18283018, 0x96a13796, 0x050f0a05, 0x9ab52f9a, 0x07090e07, 0x12362412, 0x809b1b80, 0xe23ddfe2, 0xeb26cdeb, 0x27694e27, 0xb2cd7fb2, 0x759fea75, 0x091b1209, 0x839e1d83, 0x2c74582c, 0x1a2e341a, 0x1b2d361b, 0x6eb2dc6e, 0x5aeeb45a, 0xa0fb5ba0, 0x52f6a452, 0x3b4d763b, 0xd661b7d6, 0xb3ce7db3, 0x297b5229, 0xe33edde3, 0x2f715e2f, 0x84971384, 0x53f5a653, 0xd168b9d1, 0x00000000, 0xed2cc1ed, 0x20604020, 0xfc1fe3fc, 0xb1c879b1, 0x5bedb65b, 0x6abed46a, 0xcb468dcb, 0xbed967be, 0x394b7239, 0x4ade944a, 0x4cd4984c, 0x58e8b058, 0xcf4a85cf, 0xd06bbbd0, 0xef2ac5ef, 0xaae54faa, 0xfb16edfb, 0x43c58643, 0x4dd79a4d, 0x33556633, 0x85941185, 0x45cf8a45, 0xf910e9f9, 0x02060402, 0x7f81fe7f, 0x50f0a050, 0x3c44783c, 0x9fba259f, 0xa8e34ba8, 0x51f3a251, 0xa3fe5da3, 0x40c08040, 0x8f8a058f, 0x92ad3f92, 0x9dbc219d, 0x38487038, 0xf504f1f5, 0xbcdf63bc, 0xb6c177b6, 0xda75afda, 0x21634221, 0x10302010, 0xff1ae5ff, 0xf30efdf3, 0xd26dbfd2, 0xcd4c81cd, 0x0c14180c, 0x13352613, 0xec2fc3ec, 0x5fe1be5f, 0x97a23597, 0x44cc8844, 0x17392e17, 0xc45793c4, 0xa7f255a7, 0x7e82fc7e, 0x3d477a3d, 0x64acc864, 0x5de7ba5d, 0x192b3219, 0x7395e673, 0x60a0c060, 0x81981981, 0x4fd19e4f, 0xdc7fa3dc, 0x22664422, 0x2a7e542a, 0x90ab3b90, 0x88830b88, 0x46ca8c46, 0xee29c7ee, 0xb8d36bb8, 0x143c2814, 0xde79a7de, 0x5ee2bc5e, 0x0b1d160b, 0xdb76addb, 0xe03bdbe0, 0x32566432, 0x3a4e743a, 0x0a1e140a, 0x49db9249, 0x060a0c06, 0x246c4824, 0x5ce4b85c, 0xc25d9fc2, 0xd36ebdd3, 0xacef43ac, 0x62a6c462, 0x91a83991, 0x95a43195, 0xe437d3e4, 0x798bf279, 0xe732d5e7, 0xc8438bc8, 0x37596e37, 0x6db7da6d, 0x8d8c018d, 0xd564b1d5, 0x4ed29c4e, 0xa9e049a9, 0x6cb4d86c, 0x56faac56, 0xf407f3f4, 0xea25cfea, 0x65afca65, 0x7a8ef47a, 0xaee947ae, 0x08181008, 0xbad56fba, 0x7888f078, 0x256f4a25, 0x2e725c2e, 0x1c24381c, 0xa6f157a6, 0xb4c773b4, 0xc65197c6, 0xe823cbe8, 0xdd7ca1dd, 0x749ce874, 0x1f213e1f, 0x4bdd964b, 0xbddc61bd, 0x8b860d8b, 0x8a850f8a, 0x7090e070, 0x3e427c3e, 0xb5c471b5, 0x66aacc66, 0x48d89048, 0x03050603, 0xf601f7f6, 0x0e121c0e, 0x61a3c261, 0x355f6a35, 0x57f9ae57, 0xb9d069b9, 0x86911786, 0xc15899c1, 0x1d273a1d, 0x9eb9279e, 0xe138d9e1, 0xf813ebf8, 0x98b32b98, 0x11332211, 0x69bbd269, 0xd970a9d9, 0x8e89078e, 0x94a73394, 0x9bb62d9b, 0x1e223c1e, 0x87921587, 0xe920c9e9, 0xce4987ce, 0x55ffaa55, 0x28785028, 0xdf7aa5df, 0x8c8f038c, 0xa1f859a1, 0x89800989, 0x0d171a0d, 0xbfda65bf, 0xe631d7e6, 0x42c68442, 0x68b8d068, 0x41c38241, 0x99b02999, 0x2d775a2d, 0x0f111e0f, 0xb0cb7bb0, 0x54fca854, 0xbbd66dbb, 0x163a2c16];
                var T4 = [0x6363a5c6, 0x7c7c84f8, 0x777799ee, 0x7b7b8df6, 0xf2f20dff, 0x6b6bbdd6, 0x6f6fb1de, 0xc5c55491, 0x30305060, 0x01010302, 0x6767a9ce, 0x2b2b7d56, 0xfefe19e7, 0xd7d762b5, 0xababe64d, 0x76769aec, 0xcaca458f, 0x82829d1f, 0xc9c94089, 0x7d7d87fa, 0xfafa15ef, 0x5959ebb2, 0x4747c98e, 0xf0f00bfb, 0xadadec41, 0xd4d467b3, 0xa2a2fd5f, 0xafafea45, 0x9c9cbf23, 0xa4a4f753, 0x727296e4, 0xc0c05b9b, 0xb7b7c275, 0xfdfd1ce1, 0x9393ae3d, 0x26266a4c, 0x36365a6c, 0x3f3f417e, 0xf7f702f5, 0xcccc4f83, 0x34345c68, 0xa5a5f451, 0xe5e534d1, 0xf1f108f9, 0x717193e2, 0xd8d873ab, 0x31315362, 0x15153f2a, 0x04040c08, 0xc7c75295, 0x23236546, 0xc3c35e9d, 0x18182830, 0x9696a137, 0x05050f0a, 0x9a9ab52f, 0x0707090e, 0x12123624, 0x80809b1b, 0xe2e23ddf, 0xebeb26cd, 0x2727694e, 0xb2b2cd7f, 0x75759fea, 0x09091b12, 0x83839e1d, 0x2c2c7458, 0x1a1a2e34, 0x1b1b2d36, 0x6e6eb2dc, 0x5a5aeeb4, 0xa0a0fb5b, 0x5252f6a4, 0x3b3b4d76, 0xd6d661b7, 0xb3b3ce7d, 0x29297b52, 0xe3e33edd, 0x2f2f715e, 0x84849713, 0x5353f5a6, 0xd1d168b9, 0x00000000, 0xeded2cc1, 0x20206040, 0xfcfc1fe3, 0xb1b1c879, 0x5b5bedb6, 0x6a6abed4, 0xcbcb468d, 0xbebed967, 0x39394b72, 0x4a4ade94, 0x4c4cd498, 0x5858e8b0, 0xcfcf4a85, 0xd0d06bbb, 0xefef2ac5, 0xaaaae54f, 0xfbfb16ed, 0x4343c586, 0x4d4dd79a, 0x33335566, 0x85859411, 0x4545cf8a, 0xf9f910e9, 0x02020604, 0x7f7f81fe, 0x5050f0a0, 0x3c3c4478, 0x9f9fba25, 0xa8a8e34b, 0x5151f3a2, 0xa3a3fe5d, 0x4040c080, 0x8f8f8a05, 0x9292ad3f, 0x9d9dbc21, 0x38384870, 0xf5f504f1, 0xbcbcdf63, 0xb6b6c177, 0xdada75af, 0x21216342, 0x10103020, 0xffff1ae5, 0xf3f30efd, 0xd2d26dbf, 0xcdcd4c81, 0x0c0c1418, 0x13133526, 0xecec2fc3, 0x5f5fe1be, 0x9797a235, 0x4444cc88, 0x1717392e, 0xc4c45793, 0xa7a7f255, 0x7e7e82fc, 0x3d3d477a, 0x6464acc8, 0x5d5de7ba, 0x19192b32, 0x737395e6, 0x6060a0c0, 0x81819819, 0x4f4fd19e, 0xdcdc7fa3, 0x22226644, 0x2a2a7e54, 0x9090ab3b, 0x8888830b, 0x4646ca8c, 0xeeee29c7, 0xb8b8d36b, 0x14143c28, 0xdede79a7, 0x5e5ee2bc, 0x0b0b1d16, 0xdbdb76ad, 0xe0e03bdb, 0x32325664, 0x3a3a4e74, 0x0a0a1e14, 0x4949db92, 0x06060a0c, 0x24246c48, 0x5c5ce4b8, 0xc2c25d9f, 0xd3d36ebd, 0xacacef43, 0x6262a6c4, 0x9191a839, 0x9595a431, 0xe4e437d3, 0x79798bf2, 0xe7e732d5, 0xc8c8438b, 0x3737596e, 0x6d6db7da, 0x8d8d8c01, 0xd5d564b1, 0x4e4ed29c, 0xa9a9e049, 0x6c6cb4d8, 0x5656faac, 0xf4f407f3, 0xeaea25cf, 0x6565afca, 0x7a7a8ef4, 0xaeaee947, 0x08081810, 0xbabad56f, 0x787888f0, 0x25256f4a, 0x2e2e725c, 0x1c1c2438, 0xa6a6f157, 0xb4b4c773, 0xc6c65197, 0xe8e823cb, 0xdddd7ca1, 0x74749ce8, 0x1f1f213e, 0x4b4bdd96, 0xbdbddc61, 0x8b8b860d, 0x8a8a850f, 0x707090e0, 0x3e3e427c, 0xb5b5c471, 0x6666aacc, 0x4848d890, 0x03030506, 0xf6f601f7, 0x0e0e121c, 0x6161a3c2, 0x35355f6a, 0x5757f9ae, 0xb9b9d069, 0x86869117, 0xc1c15899, 0x1d1d273a, 0x9e9eb927, 0xe1e138d9, 0xf8f813eb, 0x9898b32b, 0x11113322, 0x6969bbd2, 0xd9d970a9, 0x8e8e8907, 0x9494a733, 0x9b9bb62d, 0x1e1e223c, 0x87879215, 0xe9e920c9, 0xcece4987, 0x5555ffaa, 0x28287850, 0xdfdf7aa5, 0x8c8c8f03, 0xa1a1f859, 0x89898009, 0x0d0d171a, 0xbfbfda65, 0xe6e631d7, 0x4242c684, 0x6868b8d0, 0x4141c382, 0x9999b029, 0x2d2d775a, 0x0f0f111e, 0xb0b0cb7b, 0x5454fca8, 0xbbbbd66d, 0x16163a2c];

                // Transformations for decryption
                var T5 = [0x51f4a750, 0x7e416553, 0x1a17a4c3, 0x3a275e96, 0x3bab6bcb, 0x1f9d45f1, 0xacfa58ab, 0x4be30393, 0x2030fa55, 0xad766df6, 0x88cc7691, 0xf5024c25, 0x4fe5d7fc, 0xc52acbd7, 0x26354480, 0xb562a38f, 0xdeb15a49, 0x25ba1b67, 0x45ea0e98, 0x5dfec0e1, 0xc32f7502, 0x814cf012, 0x8d4697a3, 0x6bd3f9c6, 0x038f5fe7, 0x15929c95, 0xbf6d7aeb, 0x955259da, 0xd4be832d, 0x587421d3, 0x49e06929, 0x8ec9c844, 0x75c2896a, 0xf48e7978, 0x99583e6b, 0x27b971dd, 0xbee14fb6, 0xf088ad17, 0xc920ac66, 0x7dce3ab4, 0x63df4a18, 0xe51a3182, 0x97513360, 0x62537f45, 0xb16477e0, 0xbb6bae84, 0xfe81a01c, 0xf9082b94, 0x70486858, 0x8f45fd19, 0x94de6c87, 0x527bf8b7, 0xab73d323, 0x724b02e2, 0xe31f8f57, 0x6655ab2a, 0xb2eb2807, 0x2fb5c203, 0x86c57b9a, 0xd33708a5, 0x302887f2, 0x23bfa5b2, 0x02036aba, 0xed16825c, 0x8acf1c2b, 0xa779b492, 0xf307f2f0, 0x4e69e2a1, 0x65daf4cd, 0x0605bed5, 0xd134621f, 0xc4a6fe8a, 0x342e539d, 0xa2f355a0, 0x058ae132, 0xa4f6eb75, 0x0b83ec39, 0x4060efaa, 0x5e719f06, 0xbd6e1051, 0x3e218af9, 0x96dd063d, 0xdd3e05ae, 0x4de6bd46, 0x91548db5, 0x71c45d05, 0x0406d46f, 0x605015ff, 0x1998fb24, 0xd6bde997, 0x894043cc, 0x67d99e77, 0xb0e842bd, 0x07898b88, 0xe7195b38, 0x79c8eedb, 0xa17c0a47, 0x7c420fe9, 0xf8841ec9, 0x00000000, 0x09808683, 0x322bed48, 0x1e1170ac, 0x6c5a724e, 0xfd0efffb, 0x0f853856, 0x3daed51e, 0x362d3927, 0x0a0fd964, 0x685ca621, 0x9b5b54d1, 0x24362e3a, 0x0c0a67b1, 0x9357e70f, 0xb4ee96d2, 0x1b9b919e, 0x80c0c54f, 0x61dc20a2, 0x5a774b69, 0x1c121a16, 0xe293ba0a, 0xc0a02ae5, 0x3c22e043, 0x121b171d, 0x0e090d0b, 0xf28bc7ad, 0x2db6a8b9, 0x141ea9c8, 0x57f11985, 0xaf75074c, 0xee99ddbb, 0xa37f60fd, 0xf701269f, 0x5c72f5bc, 0x44663bc5, 0x5bfb7e34, 0x8b432976, 0xcb23c6dc, 0xb6edfc68, 0xb8e4f163, 0xd731dcca, 0x42638510, 0x13972240, 0x84c61120, 0x854a247d, 0xd2bb3df8, 0xaef93211, 0xc729a16d, 0x1d9e2f4b, 0xdcb230f3, 0x0d8652ec, 0x77c1e3d0, 0x2bb3166c, 0xa970b999, 0x119448fa, 0x47e96422, 0xa8fc8cc4, 0xa0f03f1a, 0x567d2cd8, 0x223390ef, 0x87494ec7, 0xd938d1c1, 0x8ccaa2fe, 0x98d40b36, 0xa6f581cf, 0xa57ade28, 0xdab78e26, 0x3fadbfa4, 0x2c3a9de4, 0x5078920d, 0x6a5fcc9b, 0x547e4662, 0xf68d13c2, 0x90d8b8e8, 0x2e39f75e, 0x82c3aff5, 0x9f5d80be, 0x69d0937c, 0x6fd52da9, 0xcf2512b3, 0xc8ac993b, 0x10187da7, 0xe89c636e, 0xdb3bbb7b, 0xcd267809, 0x6e5918f4, 0xec9ab701, 0x834f9aa8, 0xe6956e65, 0xaaffe67e, 0x21bccf08, 0xef15e8e6, 0xbae79bd9, 0x4a6f36ce, 0xea9f09d4, 0x29b07cd6, 0x31a4b2af, 0x2a3f2331, 0xc6a59430, 0x35a266c0, 0x744ebc37, 0xfc82caa6, 0xe090d0b0, 0x33a7d815, 0xf104984a, 0x41ecdaf7, 0x7fcd500e, 0x1791f62f, 0x764dd68d, 0x43efb04d, 0xccaa4d54, 0xe49604df, 0x9ed1b5e3, 0x4c6a881b, 0xc12c1fb8, 0x4665517f, 0x9d5eea04, 0x018c355d, 0xfa877473, 0xfb0b412e, 0xb3671d5a, 0x92dbd252, 0xe9105633, 0x6dd64713, 0x9ad7618c, 0x37a10c7a, 0x59f8148e, 0xeb133c89, 0xcea927ee, 0xb761c935, 0xe11ce5ed, 0x7a47b13c, 0x9cd2df59, 0x55f2733f, 0x1814ce79, 0x73c737bf, 0x53f7cdea, 0x5ffdaa5b, 0xdf3d6f14, 0x7844db86, 0xcaaff381, 0xb968c43e, 0x3824342c, 0xc2a3405f, 0x161dc372, 0xbce2250c, 0x283c498b, 0xff0d9541, 0x39a80171, 0x080cb3de, 0xd8b4e49c, 0x6456c190, 0x7bcb8461, 0xd532b670, 0x486c5c74, 0xd0b85742];
                var T6 = [0x5051f4a7, 0x537e4165, 0xc31a17a4, 0x963a275e, 0xcb3bab6b, 0xf11f9d45, 0xabacfa58, 0x934be303, 0x552030fa, 0xf6ad766d, 0x9188cc76, 0x25f5024c, 0xfc4fe5d7, 0xd7c52acb, 0x80263544, 0x8fb562a3, 0x49deb15a, 0x6725ba1b, 0x9845ea0e, 0xe15dfec0, 0x02c32f75, 0x12814cf0, 0xa38d4697, 0xc66bd3f9, 0xe7038f5f, 0x9515929c, 0xebbf6d7a, 0xda955259, 0x2dd4be83, 0xd3587421, 0x2949e069, 0x448ec9c8, 0x6a75c289, 0x78f48e79, 0x6b99583e, 0xdd27b971, 0xb6bee14f, 0x17f088ad, 0x66c920ac, 0xb47dce3a, 0x1863df4a, 0x82e51a31, 0x60975133, 0x4562537f, 0xe0b16477, 0x84bb6bae, 0x1cfe81a0, 0x94f9082b, 0x58704868, 0x198f45fd, 0x8794de6c, 0xb7527bf8, 0x23ab73d3, 0xe2724b02, 0x57e31f8f, 0x2a6655ab, 0x07b2eb28, 0x032fb5c2, 0x9a86c57b, 0xa5d33708, 0xf2302887, 0xb223bfa5, 0xba02036a, 0x5ced1682, 0x2b8acf1c, 0x92a779b4, 0xf0f307f2, 0xa14e69e2, 0xcd65daf4, 0xd50605be, 0x1fd13462, 0x8ac4a6fe, 0x9d342e53, 0xa0a2f355, 0x32058ae1, 0x75a4f6eb, 0x390b83ec, 0xaa4060ef, 0x065e719f, 0x51bd6e10, 0xf93e218a, 0x3d96dd06, 0xaedd3e05, 0x464de6bd, 0xb591548d, 0x0571c45d, 0x6f0406d4, 0xff605015, 0x241998fb, 0x97d6bde9, 0xcc894043, 0x7767d99e, 0xbdb0e842, 0x8807898b, 0x38e7195b, 0xdb79c8ee, 0x47a17c0a, 0xe97c420f, 0xc9f8841e, 0x00000000, 0x83098086, 0x48322bed, 0xac1e1170, 0x4e6c5a72, 0xfbfd0eff, 0x560f8538, 0x1e3daed5, 0x27362d39, 0x640a0fd9, 0x21685ca6, 0xd19b5b54, 0x3a24362e, 0xb10c0a67, 0x0f9357e7, 0xd2b4ee96, 0x9e1b9b91, 0x4f80c0c5, 0xa261dc20, 0x695a774b, 0x161c121a, 0x0ae293ba, 0xe5c0a02a, 0x433c22e0, 0x1d121b17, 0x0b0e090d, 0xadf28bc7, 0xb92db6a8, 0xc8141ea9, 0x8557f119, 0x4caf7507, 0xbbee99dd, 0xfda37f60, 0x9ff70126, 0xbc5c72f5, 0xc544663b, 0x345bfb7e, 0x768b4329, 0xdccb23c6, 0x68b6edfc, 0x63b8e4f1, 0xcad731dc, 0x10426385, 0x40139722, 0x2084c611, 0x7d854a24, 0xf8d2bb3d, 0x11aef932, 0x6dc729a1, 0x4b1d9e2f, 0xf3dcb230, 0xec0d8652, 0xd077c1e3, 0x6c2bb316, 0x99a970b9, 0xfa119448, 0x2247e964, 0xc4a8fc8c, 0x1aa0f03f, 0xd8567d2c, 0xef223390, 0xc787494e, 0xc1d938d1, 0xfe8ccaa2, 0x3698d40b, 0xcfa6f581, 0x28a57ade, 0x26dab78e, 0xa43fadbf, 0xe42c3a9d, 0x0d507892, 0x9b6a5fcc, 0x62547e46, 0xc2f68d13, 0xe890d8b8, 0x5e2e39f7, 0xf582c3af, 0xbe9f5d80, 0x7c69d093, 0xa96fd52d, 0xb3cf2512, 0x3bc8ac99, 0xa710187d, 0x6ee89c63, 0x7bdb3bbb, 0x09cd2678, 0xf46e5918, 0x01ec9ab7, 0xa8834f9a, 0x65e6956e, 0x7eaaffe6, 0x0821bccf, 0xe6ef15e8, 0xd9bae79b, 0xce4a6f36, 0xd4ea9f09, 0xd629b07c, 0xaf31a4b2, 0x312a3f23, 0x30c6a594, 0xc035a266, 0x37744ebc, 0xa6fc82ca, 0xb0e090d0, 0x1533a7d8, 0x4af10498, 0xf741ecda, 0x0e7fcd50, 0x2f1791f6, 0x8d764dd6, 0x4d43efb0, 0x54ccaa4d, 0xdfe49604, 0xe39ed1b5, 0x1b4c6a88, 0xb8c12c1f, 0x7f466551, 0x049d5eea, 0x5d018c35, 0x73fa8774, 0x2efb0b41, 0x5ab3671d, 0x5292dbd2, 0x33e91056, 0x136dd647, 0x8c9ad761, 0x7a37a10c, 0x8e59f814, 0x89eb133c, 0xeecea927, 0x35b761c9, 0xede11ce5, 0x3c7a47b1, 0x599cd2df, 0x3f55f273, 0x791814ce, 0xbf73c737, 0xea53f7cd, 0x5b5ffdaa, 0x14df3d6f, 0x867844db, 0x81caaff3, 0x3eb968c4, 0x2c382434, 0x5fc2a340, 0x72161dc3, 0x0cbce225, 0x8b283c49, 0x41ff0d95, 0x7139a801, 0xde080cb3, 0x9cd8b4e4, 0x906456c1, 0x617bcb84, 0x70d532b6, 0x74486c5c, 0x42d0b857];
                var T7 = [0xa75051f4, 0x65537e41, 0xa4c31a17, 0x5e963a27, 0x6bcb3bab, 0x45f11f9d, 0x58abacfa, 0x03934be3, 0xfa552030, 0x6df6ad76, 0x769188cc, 0x4c25f502, 0xd7fc4fe5, 0xcbd7c52a, 0x44802635, 0xa38fb562, 0x5a49deb1, 0x1b6725ba, 0x0e9845ea, 0xc0e15dfe, 0x7502c32f, 0xf012814c, 0x97a38d46, 0xf9c66bd3, 0x5fe7038f, 0x9c951592, 0x7aebbf6d, 0x59da9552, 0x832dd4be, 0x21d35874, 0x692949e0, 0xc8448ec9, 0x896a75c2, 0x7978f48e, 0x3e6b9958, 0x71dd27b9, 0x4fb6bee1, 0xad17f088, 0xac66c920, 0x3ab47dce, 0x4a1863df, 0x3182e51a, 0x33609751, 0x7f456253, 0x77e0b164, 0xae84bb6b, 0xa01cfe81, 0x2b94f908, 0x68587048, 0xfd198f45, 0x6c8794de, 0xf8b7527b, 0xd323ab73, 0x02e2724b, 0x8f57e31f, 0xab2a6655, 0x2807b2eb, 0xc2032fb5, 0x7b9a86c5, 0x08a5d337, 0x87f23028, 0xa5b223bf, 0x6aba0203, 0x825ced16, 0x1c2b8acf, 0xb492a779, 0xf2f0f307, 0xe2a14e69, 0xf4cd65da, 0xbed50605, 0x621fd134, 0xfe8ac4a6, 0x539d342e, 0x55a0a2f3, 0xe132058a, 0xeb75a4f6, 0xec390b83, 0xefaa4060, 0x9f065e71, 0x1051bd6e, 0x8af93e21, 0x063d96dd, 0x05aedd3e, 0xbd464de6, 0x8db59154, 0x5d0571c4, 0xd46f0406, 0x15ff6050, 0xfb241998, 0xe997d6bd, 0x43cc8940, 0x9e7767d9, 0x42bdb0e8, 0x8b880789, 0x5b38e719, 0xeedb79c8, 0x0a47a17c, 0x0fe97c42, 0x1ec9f884, 0x00000000, 0x86830980, 0xed48322b, 0x70ac1e11, 0x724e6c5a, 0xfffbfd0e, 0x38560f85, 0xd51e3dae, 0x3927362d, 0xd9640a0f, 0xa621685c, 0x54d19b5b, 0x2e3a2436, 0x67b10c0a, 0xe70f9357, 0x96d2b4ee, 0x919e1b9b, 0xc54f80c0, 0x20a261dc, 0x4b695a77, 0x1a161c12, 0xba0ae293, 0x2ae5c0a0, 0xe0433c22, 0x171d121b, 0x0d0b0e09, 0xc7adf28b, 0xa8b92db6, 0xa9c8141e, 0x198557f1, 0x074caf75, 0xddbbee99, 0x60fda37f, 0x269ff701, 0xf5bc5c72, 0x3bc54466, 0x7e345bfb, 0x29768b43, 0xc6dccb23, 0xfc68b6ed, 0xf163b8e4, 0xdccad731, 0x85104263, 0x22401397, 0x112084c6, 0x247d854a, 0x3df8d2bb, 0x3211aef9, 0xa16dc729, 0x2f4b1d9e, 0x30f3dcb2, 0x52ec0d86, 0xe3d077c1, 0x166c2bb3, 0xb999a970, 0x48fa1194, 0x642247e9, 0x8cc4a8fc, 0x3f1aa0f0, 0x2cd8567d, 0x90ef2233, 0x4ec78749, 0xd1c1d938, 0xa2fe8cca, 0x0b3698d4, 0x81cfa6f5, 0xde28a57a, 0x8e26dab7, 0xbfa43fad, 0x9de42c3a, 0x920d5078, 0xcc9b6a5f, 0x4662547e, 0x13c2f68d, 0xb8e890d8, 0xf75e2e39, 0xaff582c3, 0x80be9f5d, 0x937c69d0, 0x2da96fd5, 0x12b3cf25, 0x993bc8ac, 0x7da71018, 0x636ee89c, 0xbb7bdb3b, 0x7809cd26, 0x18f46e59, 0xb701ec9a, 0x9aa8834f, 0x6e65e695, 0xe67eaaff, 0xcf0821bc, 0xe8e6ef15, 0x9bd9bae7, 0x36ce4a6f, 0x09d4ea9f, 0x7cd629b0, 0xb2af31a4, 0x23312a3f, 0x9430c6a5, 0x66c035a2, 0xbc37744e, 0xcaa6fc82, 0xd0b0e090, 0xd81533a7, 0x984af104, 0xdaf741ec, 0x500e7fcd, 0xf62f1791, 0xd68d764d, 0xb04d43ef, 0x4d54ccaa, 0x04dfe496, 0xb5e39ed1, 0x881b4c6a, 0x1fb8c12c, 0x517f4665, 0xea049d5e, 0x355d018c, 0x7473fa87, 0x412efb0b, 0x1d5ab367, 0xd25292db, 0x5633e910, 0x47136dd6, 0x618c9ad7, 0x0c7a37a1, 0x148e59f8, 0x3c89eb13, 0x27eecea9, 0xc935b761, 0xe5ede11c, 0xb13c7a47, 0xdf599cd2, 0x733f55f2, 0xce791814, 0x37bf73c7, 0xcdea53f7, 0xaa5b5ffd, 0x6f14df3d, 0xdb867844, 0xf381caaf, 0xc43eb968, 0x342c3824, 0x405fc2a3, 0xc372161d, 0x250cbce2, 0x498b283c, 0x9541ff0d, 0x017139a8, 0xb3de080c, 0xe49cd8b4, 0xc1906456, 0x84617bcb, 0xb670d532, 0x5c74486c, 0x5742d0b8];
                var T8 = [0xf4a75051, 0x4165537e, 0x17a4c31a, 0x275e963a, 0xab6bcb3b, 0x9d45f11f, 0xfa58abac, 0xe303934b, 0x30fa5520, 0x766df6ad, 0xcc769188, 0x024c25f5, 0xe5d7fc4f, 0x2acbd7c5, 0x35448026, 0x62a38fb5, 0xb15a49de, 0xba1b6725, 0xea0e9845, 0xfec0e15d, 0x2f7502c3, 0x4cf01281, 0x4697a38d, 0xd3f9c66b, 0x8f5fe703, 0x929c9515, 0x6d7aebbf, 0x5259da95, 0xbe832dd4, 0x7421d358, 0xe0692949, 0xc9c8448e, 0xc2896a75, 0x8e7978f4, 0x583e6b99, 0xb971dd27, 0xe14fb6be, 0x88ad17f0, 0x20ac66c9, 0xce3ab47d, 0xdf4a1863, 0x1a3182e5, 0x51336097, 0x537f4562, 0x6477e0b1, 0x6bae84bb, 0x81a01cfe, 0x082b94f9, 0x48685870, 0x45fd198f, 0xde6c8794, 0x7bf8b752, 0x73d323ab, 0x4b02e272, 0x1f8f57e3, 0x55ab2a66, 0xeb2807b2, 0xb5c2032f, 0xc57b9a86, 0x3708a5d3, 0x2887f230, 0xbfa5b223, 0x036aba02, 0x16825ced, 0xcf1c2b8a, 0x79b492a7, 0x07f2f0f3, 0x69e2a14e, 0xdaf4cd65, 0x05bed506, 0x34621fd1, 0xa6fe8ac4, 0x2e539d34, 0xf355a0a2, 0x8ae13205, 0xf6eb75a4, 0x83ec390b, 0x60efaa40, 0x719f065e, 0x6e1051bd, 0x218af93e, 0xdd063d96, 0x3e05aedd, 0xe6bd464d, 0x548db591, 0xc45d0571, 0x06d46f04, 0x5015ff60, 0x98fb2419, 0xbde997d6, 0x4043cc89, 0xd99e7767, 0xe842bdb0, 0x898b8807, 0x195b38e7, 0xc8eedb79, 0x7c0a47a1, 0x420fe97c, 0x841ec9f8, 0x00000000, 0x80868309, 0x2bed4832, 0x1170ac1e, 0x5a724e6c, 0x0efffbfd, 0x8538560f, 0xaed51e3d, 0x2d392736, 0x0fd9640a, 0x5ca62168, 0x5b54d19b, 0x362e3a24, 0x0a67b10c, 0x57e70f93, 0xee96d2b4, 0x9b919e1b, 0xc0c54f80, 0xdc20a261, 0x774b695a, 0x121a161c, 0x93ba0ae2, 0xa02ae5c0, 0x22e0433c, 0x1b171d12, 0x090d0b0e, 0x8bc7adf2, 0xb6a8b92d, 0x1ea9c814, 0xf1198557, 0x75074caf, 0x99ddbbee, 0x7f60fda3, 0x01269ff7, 0x72f5bc5c, 0x663bc544, 0xfb7e345b, 0x4329768b, 0x23c6dccb, 0xedfc68b6, 0xe4f163b8, 0x31dccad7, 0x63851042, 0x97224013, 0xc6112084, 0x4a247d85, 0xbb3df8d2, 0xf93211ae, 0x29a16dc7, 0x9e2f4b1d, 0xb230f3dc, 0x8652ec0d, 0xc1e3d077, 0xb3166c2b, 0x70b999a9, 0x9448fa11, 0xe9642247, 0xfc8cc4a8, 0xf03f1aa0, 0x7d2cd856, 0x3390ef22, 0x494ec787, 0x38d1c1d9, 0xcaa2fe8c, 0xd40b3698, 0xf581cfa6, 0x7ade28a5, 0xb78e26da, 0xadbfa43f, 0x3a9de42c, 0x78920d50, 0x5fcc9b6a, 0x7e466254, 0x8d13c2f6, 0xd8b8e890, 0x39f75e2e, 0xc3aff582, 0x5d80be9f, 0xd0937c69, 0xd52da96f, 0x2512b3cf, 0xac993bc8, 0x187da710, 0x9c636ee8, 0x3bbb7bdb, 0x267809cd, 0x5918f46e, 0x9ab701ec, 0x4f9aa883, 0x956e65e6, 0xffe67eaa, 0xbccf0821, 0x15e8e6ef, 0xe79bd9ba, 0x6f36ce4a, 0x9f09d4ea, 0xb07cd629, 0xa4b2af31, 0x3f23312a, 0xa59430c6, 0xa266c035, 0x4ebc3774, 0x82caa6fc, 0x90d0b0e0, 0xa7d81533, 0x04984af1, 0xecdaf741, 0xcd500e7f, 0x91f62f17, 0x4dd68d76, 0xefb04d43, 0xaa4d54cc, 0x9604dfe4, 0xd1b5e39e, 0x6a881b4c, 0x2c1fb8c1, 0x65517f46, 0x5eea049d, 0x8c355d01, 0x877473fa, 0x0b412efb, 0x671d5ab3, 0xdbd25292, 0x105633e9, 0xd647136d, 0xd7618c9a, 0xa10c7a37, 0xf8148e59, 0x133c89eb, 0xa927eece, 0x61c935b7, 0x1ce5ede1, 0x47b13c7a, 0xd2df599c, 0xf2733f55, 0x14ce7918, 0xc737bf73, 0xf7cdea53, 0xfdaa5b5f, 0x3d6f14df, 0x44db8678, 0xaff381ca, 0x68c43eb9, 0x24342c38, 0xa3405fc2, 0x1dc37216, 0xe2250cbc, 0x3c498b28, 0x0d9541ff, 0xa8017139, 0x0cb3de08, 0xb4e49cd8, 0x56c19064, 0xcb84617b, 0x32b670d5, 0x6c5c7448, 0xb85742d0];

                // Transformations for decryption key expansion
                var U1 = [0x00000000, 0x0e090d0b, 0x1c121a16, 0x121b171d, 0x3824342c, 0x362d3927, 0x24362e3a, 0x2a3f2331, 0x70486858, 0x7e416553, 0x6c5a724e, 0x62537f45, 0x486c5c74, 0x4665517f, 0x547e4662, 0x5a774b69, 0xe090d0b0, 0xee99ddbb, 0xfc82caa6, 0xf28bc7ad, 0xd8b4e49c, 0xd6bde997, 0xc4a6fe8a, 0xcaaff381, 0x90d8b8e8, 0x9ed1b5e3, 0x8ccaa2fe, 0x82c3aff5, 0xa8fc8cc4, 0xa6f581cf, 0xb4ee96d2, 0xbae79bd9, 0xdb3bbb7b, 0xd532b670, 0xc729a16d, 0xc920ac66, 0xe31f8f57, 0xed16825c, 0xff0d9541, 0xf104984a, 0xab73d323, 0xa57ade28, 0xb761c935, 0xb968c43e, 0x9357e70f, 0x9d5eea04, 0x8f45fd19, 0x814cf012, 0x3bab6bcb, 0x35a266c0, 0x27b971dd, 0x29b07cd6, 0x038f5fe7, 0x0d8652ec, 0x1f9d45f1, 0x119448fa, 0x4be30393, 0x45ea0e98, 0x57f11985, 0x59f8148e, 0x73c737bf, 0x7dce3ab4, 0x6fd52da9, 0x61dc20a2, 0xad766df6, 0xa37f60fd, 0xb16477e0, 0xbf6d7aeb, 0x955259da, 0x9b5b54d1, 0x894043cc, 0x87494ec7, 0xdd3e05ae, 0xd33708a5, 0xc12c1fb8, 0xcf2512b3, 0xe51a3182, 0xeb133c89, 0xf9082b94, 0xf701269f, 0x4de6bd46, 0x43efb04d, 0x51f4a750, 0x5ffdaa5b, 0x75c2896a, 0x7bcb8461, 0x69d0937c, 0x67d99e77, 0x3daed51e, 0x33a7d815, 0x21bccf08, 0x2fb5c203, 0x058ae132, 0x0b83ec39, 0x1998fb24, 0x1791f62f, 0x764dd68d, 0x7844db86, 0x6a5fcc9b, 0x6456c190, 0x4e69e2a1, 0x4060efaa, 0x527bf8b7, 0x5c72f5bc, 0x0605bed5, 0x080cb3de, 0x1a17a4c3, 0x141ea9c8, 0x3e218af9, 0x302887f2, 0x223390ef, 0x2c3a9de4, 0x96dd063d, 0x98d40b36, 0x8acf1c2b, 0x84c61120, 0xaef93211, 0xa0f03f1a, 0xb2eb2807, 0xbce2250c, 0xe6956e65, 0xe89c636e, 0xfa877473, 0xf48e7978, 0xdeb15a49, 0xd0b85742, 0xc2a3405f, 0xccaa4d54, 0x41ecdaf7, 0x4fe5d7fc, 0x5dfec0e1, 0x53f7cdea, 0x79c8eedb, 0x77c1e3d0, 0x65daf4cd, 0x6bd3f9c6, 0x31a4b2af, 0x3fadbfa4, 0x2db6a8b9, 0x23bfa5b2, 0x09808683, 0x07898b88, 0x15929c95, 0x1b9b919e, 0xa17c0a47, 0xaf75074c, 0xbd6e1051, 0xb3671d5a, 0x99583e6b, 0x97513360, 0x854a247d, 0x8b432976, 0xd134621f, 0xdf3d6f14, 0xcd267809, 0xc32f7502, 0xe9105633, 0xe7195b38, 0xf5024c25, 0xfb0b412e, 0x9ad7618c, 0x94de6c87, 0x86c57b9a, 0x88cc7691, 0xa2f355a0, 0xacfa58ab, 0xbee14fb6, 0xb0e842bd, 0xea9f09d4, 0xe49604df, 0xf68d13c2, 0xf8841ec9, 0xd2bb3df8, 0xdcb230f3, 0xcea927ee, 0xc0a02ae5, 0x7a47b13c, 0x744ebc37, 0x6655ab2a, 0x685ca621, 0x42638510, 0x4c6a881b, 0x5e719f06, 0x5078920d, 0x0a0fd964, 0x0406d46f, 0x161dc372, 0x1814ce79, 0x322bed48, 0x3c22e043, 0x2e39f75e, 0x2030fa55, 0xec9ab701, 0xe293ba0a, 0xf088ad17, 0xfe81a01c, 0xd4be832d, 0xdab78e26, 0xc8ac993b, 0xc6a59430, 0x9cd2df59, 0x92dbd252, 0x80c0c54f, 0x8ec9c844, 0xa4f6eb75, 0xaaffe67e, 0xb8e4f163, 0xb6edfc68, 0x0c0a67b1, 0x02036aba, 0x10187da7, 0x1e1170ac, 0x342e539d, 0x3a275e96, 0x283c498b, 0x26354480, 0x7c420fe9, 0x724b02e2, 0x605015ff, 0x6e5918f4, 0x44663bc5, 0x4a6f36ce, 0x587421d3, 0x567d2cd8, 0x37a10c7a, 0x39a80171, 0x2bb3166c, 0x25ba1b67, 0x0f853856, 0x018c355d, 0x13972240, 0x1d9e2f4b, 0x47e96422, 0x49e06929, 0x5bfb7e34, 0x55f2733f, 0x7fcd500e, 0x71c45d05, 0x63df4a18, 0x6dd64713, 0xd731dcca, 0xd938d1c1, 0xcb23c6dc, 0xc52acbd7, 0xef15e8e6, 0xe11ce5ed, 0xf307f2f0, 0xfd0efffb, 0xa779b492, 0xa970b999, 0xbb6bae84, 0xb562a38f, 0x9f5d80be, 0x91548db5, 0x834f9aa8, 0x8d4697a3];
                var U2 = [0x00000000, 0x0b0e090d, 0x161c121a, 0x1d121b17, 0x2c382434, 0x27362d39, 0x3a24362e, 0x312a3f23, 0x58704868, 0x537e4165, 0x4e6c5a72, 0x4562537f, 0x74486c5c, 0x7f466551, 0x62547e46, 0x695a774b, 0xb0e090d0, 0xbbee99dd, 0xa6fc82ca, 0xadf28bc7, 0x9cd8b4e4, 0x97d6bde9, 0x8ac4a6fe, 0x81caaff3, 0xe890d8b8, 0xe39ed1b5, 0xfe8ccaa2, 0xf582c3af, 0xc4a8fc8c, 0xcfa6f581, 0xd2b4ee96, 0xd9bae79b, 0x7bdb3bbb, 0x70d532b6, 0x6dc729a1, 0x66c920ac, 0x57e31f8f, 0x5ced1682, 0x41ff0d95, 0x4af10498, 0x23ab73d3, 0x28a57ade, 0x35b761c9, 0x3eb968c4, 0x0f9357e7, 0x049d5eea, 0x198f45fd, 0x12814cf0, 0xcb3bab6b, 0xc035a266, 0xdd27b971, 0xd629b07c, 0xe7038f5f, 0xec0d8652, 0xf11f9d45, 0xfa119448, 0x934be303, 0x9845ea0e, 0x8557f119, 0x8e59f814, 0xbf73c737, 0xb47dce3a, 0xa96fd52d, 0xa261dc20, 0xf6ad766d, 0xfda37f60, 0xe0b16477, 0xebbf6d7a, 0xda955259, 0xd19b5b54, 0xcc894043, 0xc787494e, 0xaedd3e05, 0xa5d33708, 0xb8c12c1f, 0xb3cf2512, 0x82e51a31, 0x89eb133c, 0x94f9082b, 0x9ff70126, 0x464de6bd, 0x4d43efb0, 0x5051f4a7, 0x5b5ffdaa, 0x6a75c289, 0x617bcb84, 0x7c69d093, 0x7767d99e, 0x1e3daed5, 0x1533a7d8, 0x0821bccf, 0x032fb5c2, 0x32058ae1, 0x390b83ec, 0x241998fb, 0x2f1791f6, 0x8d764dd6, 0x867844db, 0x9b6a5fcc, 0x906456c1, 0xa14e69e2, 0xaa4060ef, 0xb7527bf8, 0xbc5c72f5, 0xd50605be, 0xde080cb3, 0xc31a17a4, 0xc8141ea9, 0xf93e218a, 0xf2302887, 0xef223390, 0xe42c3a9d, 0x3d96dd06, 0x3698d40b, 0x2b8acf1c, 0x2084c611, 0x11aef932, 0x1aa0f03f, 0x07b2eb28, 0x0cbce225, 0x65e6956e, 0x6ee89c63, 0x73fa8774, 0x78f48e79, 0x49deb15a, 0x42d0b857, 0x5fc2a340, 0x54ccaa4d, 0xf741ecda, 0xfc4fe5d7, 0xe15dfec0, 0xea53f7cd, 0xdb79c8ee, 0xd077c1e3, 0xcd65daf4, 0xc66bd3f9, 0xaf31a4b2, 0xa43fadbf, 0xb92db6a8, 0xb223bfa5, 0x83098086, 0x8807898b, 0x9515929c, 0x9e1b9b91, 0x47a17c0a, 0x4caf7507, 0x51bd6e10, 0x5ab3671d, 0x6b99583e, 0x60975133, 0x7d854a24, 0x768b4329, 0x1fd13462, 0x14df3d6f, 0x09cd2678, 0x02c32f75, 0x33e91056, 0x38e7195b, 0x25f5024c, 0x2efb0b41, 0x8c9ad761, 0x8794de6c, 0x9a86c57b, 0x9188cc76, 0xa0a2f355, 0xabacfa58, 0xb6bee14f, 0xbdb0e842, 0xd4ea9f09, 0xdfe49604, 0xc2f68d13, 0xc9f8841e, 0xf8d2bb3d, 0xf3dcb230, 0xeecea927, 0xe5c0a02a, 0x3c7a47b1, 0x37744ebc, 0x2a6655ab, 0x21685ca6, 0x10426385, 0x1b4c6a88, 0x065e719f, 0x0d507892, 0x640a0fd9, 0x6f0406d4, 0x72161dc3, 0x791814ce, 0x48322bed, 0x433c22e0, 0x5e2e39f7, 0x552030fa, 0x01ec9ab7, 0x0ae293ba, 0x17f088ad, 0x1cfe81a0, 0x2dd4be83, 0x26dab78e, 0x3bc8ac99, 0x30c6a594, 0x599cd2df, 0x5292dbd2, 0x4f80c0c5, 0x448ec9c8, 0x75a4f6eb, 0x7eaaffe6, 0x63b8e4f1, 0x68b6edfc, 0xb10c0a67, 0xba02036a, 0xa710187d, 0xac1e1170, 0x9d342e53, 0x963a275e, 0x8b283c49, 0x80263544, 0xe97c420f, 0xe2724b02, 0xff605015, 0xf46e5918, 0xc544663b, 0xce4a6f36, 0xd3587421, 0xd8567d2c, 0x7a37a10c, 0x7139a801, 0x6c2bb316, 0x6725ba1b, 0x560f8538, 0x5d018c35, 0x40139722, 0x4b1d9e2f, 0x2247e964, 0x2949e069, 0x345bfb7e, 0x3f55f273, 0x0e7fcd50, 0x0571c45d, 0x1863df4a, 0x136dd647, 0xcad731dc, 0xc1d938d1, 0xdccb23c6, 0xd7c52acb, 0xe6ef15e8, 0xede11ce5, 0xf0f307f2, 0xfbfd0eff, 0x92a779b4, 0x99a970b9, 0x84bb6bae, 0x8fb562a3, 0xbe9f5d80, 0xb591548d, 0xa8834f9a, 0xa38d4697];
                var U3 = [0x00000000, 0x0d0b0e09, 0x1a161c12, 0x171d121b, 0x342c3824, 0x3927362d, 0x2e3a2436, 0x23312a3f, 0x68587048, 0x65537e41, 0x724e6c5a, 0x7f456253, 0x5c74486c, 0x517f4665, 0x4662547e, 0x4b695a77, 0xd0b0e090, 0xddbbee99, 0xcaa6fc82, 0xc7adf28b, 0xe49cd8b4, 0xe997d6bd, 0xfe8ac4a6, 0xf381caaf, 0xb8e890d8, 0xb5e39ed1, 0xa2fe8cca, 0xaff582c3, 0x8cc4a8fc, 0x81cfa6f5, 0x96d2b4ee, 0x9bd9bae7, 0xbb7bdb3b, 0xb670d532, 0xa16dc729, 0xac66c920, 0x8f57e31f, 0x825ced16, 0x9541ff0d, 0x984af104, 0xd323ab73, 0xde28a57a, 0xc935b761, 0xc43eb968, 0xe70f9357, 0xea049d5e, 0xfd198f45, 0xf012814c, 0x6bcb3bab, 0x66c035a2, 0x71dd27b9, 0x7cd629b0, 0x5fe7038f, 0x52ec0d86, 0x45f11f9d, 0x48fa1194, 0x03934be3, 0x0e9845ea, 0x198557f1, 0x148e59f8, 0x37bf73c7, 0x3ab47dce, 0x2da96fd5, 0x20a261dc, 0x6df6ad76, 0x60fda37f, 0x77e0b164, 0x7aebbf6d, 0x59da9552, 0x54d19b5b, 0x43cc8940, 0x4ec78749, 0x05aedd3e, 0x08a5d337, 0x1fb8c12c, 0x12b3cf25, 0x3182e51a, 0x3c89eb13, 0x2b94f908, 0x269ff701, 0xbd464de6, 0xb04d43ef, 0xa75051f4, 0xaa5b5ffd, 0x896a75c2, 0x84617bcb, 0x937c69d0, 0x9e7767d9, 0xd51e3dae, 0xd81533a7, 0xcf0821bc, 0xc2032fb5, 0xe132058a, 0xec390b83, 0xfb241998, 0xf62f1791, 0xd68d764d, 0xdb867844, 0xcc9b6a5f, 0xc1906456, 0xe2a14e69, 0xefaa4060, 0xf8b7527b, 0xf5bc5c72, 0xbed50605, 0xb3de080c, 0xa4c31a17, 0xa9c8141e, 0x8af93e21, 0x87f23028, 0x90ef2233, 0x9de42c3a, 0x063d96dd, 0x0b3698d4, 0x1c2b8acf, 0x112084c6, 0x3211aef9, 0x3f1aa0f0, 0x2807b2eb, 0x250cbce2, 0x6e65e695, 0x636ee89c, 0x7473fa87, 0x7978f48e, 0x5a49deb1, 0x5742d0b8, 0x405fc2a3, 0x4d54ccaa, 0xdaf741ec, 0xd7fc4fe5, 0xc0e15dfe, 0xcdea53f7, 0xeedb79c8, 0xe3d077c1, 0xf4cd65da, 0xf9c66bd3, 0xb2af31a4, 0xbfa43fad, 0xa8b92db6, 0xa5b223bf, 0x86830980, 0x8b880789, 0x9c951592, 0x919e1b9b, 0x0a47a17c, 0x074caf75, 0x1051bd6e, 0x1d5ab367, 0x3e6b9958, 0x33609751, 0x247d854a, 0x29768b43, 0x621fd134, 0x6f14df3d, 0x7809cd26, 0x7502c32f, 0x5633e910, 0x5b38e719, 0x4c25f502, 0x412efb0b, 0x618c9ad7, 0x6c8794de, 0x7b9a86c5, 0x769188cc, 0x55a0a2f3, 0x58abacfa, 0x4fb6bee1, 0x42bdb0e8, 0x09d4ea9f, 0x04dfe496, 0x13c2f68d, 0x1ec9f884, 0x3df8d2bb, 0x30f3dcb2, 0x27eecea9, 0x2ae5c0a0, 0xb13c7a47, 0xbc37744e, 0xab2a6655, 0xa621685c, 0x85104263, 0x881b4c6a, 0x9f065e71, 0x920d5078, 0xd9640a0f, 0xd46f0406, 0xc372161d, 0xce791814, 0xed48322b, 0xe0433c22, 0xf75e2e39, 0xfa552030, 0xb701ec9a, 0xba0ae293, 0xad17f088, 0xa01cfe81, 0x832dd4be, 0x8e26dab7, 0x993bc8ac, 0x9430c6a5, 0xdf599cd2, 0xd25292db, 0xc54f80c0, 0xc8448ec9, 0xeb75a4f6, 0xe67eaaff, 0xf163b8e4, 0xfc68b6ed, 0x67b10c0a, 0x6aba0203, 0x7da71018, 0x70ac1e11, 0x539d342e, 0x5e963a27, 0x498b283c, 0x44802635, 0x0fe97c42, 0x02e2724b, 0x15ff6050, 0x18f46e59, 0x3bc54466, 0x36ce4a6f, 0x21d35874, 0x2cd8567d, 0x0c7a37a1, 0x017139a8, 0x166c2bb3, 0x1b6725ba, 0x38560f85, 0x355d018c, 0x22401397, 0x2f4b1d9e, 0x642247e9, 0x692949e0, 0x7e345bfb, 0x733f55f2, 0x500e7fcd, 0x5d0571c4, 0x4a1863df, 0x47136dd6, 0xdccad731, 0xd1c1d938, 0xc6dccb23, 0xcbd7c52a, 0xe8e6ef15, 0xe5ede11c, 0xf2f0f307, 0xfffbfd0e, 0xb492a779, 0xb999a970, 0xae84bb6b, 0xa38fb562, 0x80be9f5d, 0x8db59154, 0x9aa8834f, 0x97a38d46];
                var U4 = [0x00000000, 0x090d0b0e, 0x121a161c, 0x1b171d12, 0x24342c38, 0x2d392736, 0x362e3a24, 0x3f23312a, 0x48685870, 0x4165537e, 0x5a724e6c, 0x537f4562, 0x6c5c7448, 0x65517f46, 0x7e466254, 0x774b695a, 0x90d0b0e0, 0x99ddbbee, 0x82caa6fc, 0x8bc7adf2, 0xb4e49cd8, 0xbde997d6, 0xa6fe8ac4, 0xaff381ca, 0xd8b8e890, 0xd1b5e39e, 0xcaa2fe8c, 0xc3aff582, 0xfc8cc4a8, 0xf581cfa6, 0xee96d2b4, 0xe79bd9ba, 0x3bbb7bdb, 0x32b670d5, 0x29a16dc7, 0x20ac66c9, 0x1f8f57e3, 0x16825ced, 0x0d9541ff, 0x04984af1, 0x73d323ab, 0x7ade28a5, 0x61c935b7, 0x68c43eb9, 0x57e70f93, 0x5eea049d, 0x45fd198f, 0x4cf01281, 0xab6bcb3b, 0xa266c035, 0xb971dd27, 0xb07cd629, 0x8f5fe703, 0x8652ec0d, 0x9d45f11f, 0x9448fa11, 0xe303934b, 0xea0e9845, 0xf1198557, 0xf8148e59, 0xc737bf73, 0xce3ab47d, 0xd52da96f, 0xdc20a261, 0x766df6ad, 0x7f60fda3, 0x6477e0b1, 0x6d7aebbf, 0x5259da95, 0x5b54d19b, 0x4043cc89, 0x494ec787, 0x3e05aedd, 0x3708a5d3, 0x2c1fb8c1, 0x2512b3cf, 0x1a3182e5, 0x133c89eb, 0x082b94f9, 0x01269ff7, 0xe6bd464d, 0xefb04d43, 0xf4a75051, 0xfdaa5b5f, 0xc2896a75, 0xcb84617b, 0xd0937c69, 0xd99e7767, 0xaed51e3d, 0xa7d81533, 0xbccf0821, 0xb5c2032f, 0x8ae13205, 0x83ec390b, 0x98fb2419, 0x91f62f17, 0x4dd68d76, 0x44db8678, 0x5fcc9b6a, 0x56c19064, 0x69e2a14e, 0x60efaa40, 0x7bf8b752, 0x72f5bc5c, 0x05bed506, 0x0cb3de08, 0x17a4c31a, 0x1ea9c814, 0x218af93e, 0x2887f230, 0x3390ef22, 0x3a9de42c, 0xdd063d96, 0xd40b3698, 0xcf1c2b8a, 0xc6112084, 0xf93211ae, 0xf03f1aa0, 0xeb2807b2, 0xe2250cbc, 0x956e65e6, 0x9c636ee8, 0x877473fa, 0x8e7978f4, 0xb15a49de, 0xb85742d0, 0xa3405fc2, 0xaa4d54cc, 0xecdaf741, 0xe5d7fc4f, 0xfec0e15d, 0xf7cdea53, 0xc8eedb79, 0xc1e3d077, 0xdaf4cd65, 0xd3f9c66b, 0xa4b2af31, 0xadbfa43f, 0xb6a8b92d, 0xbfa5b223, 0x80868309, 0x898b8807, 0x929c9515, 0x9b919e1b, 0x7c0a47a1, 0x75074caf, 0x6e1051bd, 0x671d5ab3, 0x583e6b99, 0x51336097, 0x4a247d85, 0x4329768b, 0x34621fd1, 0x3d6f14df, 0x267809cd, 0x2f7502c3, 0x105633e9, 0x195b38e7, 0x024c25f5, 0x0b412efb, 0xd7618c9a, 0xde6c8794, 0xc57b9a86, 0xcc769188, 0xf355a0a2, 0xfa58abac, 0xe14fb6be, 0xe842bdb0, 0x9f09d4ea, 0x9604dfe4, 0x8d13c2f6, 0x841ec9f8, 0xbb3df8d2, 0xb230f3dc, 0xa927eece, 0xa02ae5c0, 0x47b13c7a, 0x4ebc3774, 0x55ab2a66, 0x5ca62168, 0x63851042, 0x6a881b4c, 0x719f065e, 0x78920d50, 0x0fd9640a, 0x06d46f04, 0x1dc37216, 0x14ce7918, 0x2bed4832, 0x22e0433c, 0x39f75e2e, 0x30fa5520, 0x9ab701ec, 0x93ba0ae2, 0x88ad17f0, 0x81a01cfe, 0xbe832dd4, 0xb78e26da, 0xac993bc8, 0xa59430c6, 0xd2df599c, 0xdbd25292, 0xc0c54f80, 0xc9c8448e, 0xf6eb75a4, 0xffe67eaa, 0xe4f163b8, 0xedfc68b6, 0x0a67b10c, 0x036aba02, 0x187da710, 0x1170ac1e, 0x2e539d34, 0x275e963a, 0x3c498b28, 0x35448026, 0x420fe97c, 0x4b02e272, 0x5015ff60, 0x5918f46e, 0x663bc544, 0x6f36ce4a, 0x7421d358, 0x7d2cd856, 0xa10c7a37, 0xa8017139, 0xb3166c2b, 0xba1b6725, 0x8538560f, 0x8c355d01, 0x97224013, 0x9e2f4b1d, 0xe9642247, 0xe0692949, 0xfb7e345b, 0xf2733f55, 0xcd500e7f, 0xc45d0571, 0xdf4a1863, 0xd647136d, 0x31dccad7, 0x38d1c1d9, 0x23c6dccb, 0x2acbd7c5, 0x15e8e6ef, 0x1ce5ede1, 0x07f2f0f3, 0x0efffbfd, 0x79b492a7, 0x70b999a9, 0x6bae84bb, 0x62a38fb5, 0x5d80be9f, 0x548db591, 0x4f9aa883, 0x4697a38d];

                function convertToInt32(bytes)
                {
                    var result = [];
                    for (var i = 0; i < bytes.length; i += 4)
                    {
                        result.push(
                            (bytes[i] << 24) |
                            (bytes[i + 1] << 16) |
                            (bytes[i + 2] << 8) |
                            bytes[i + 3]
                        );
                    }
                    return result;
                }

                var AES = function (key)
                {
                    if (!(this instanceof AES))
                    {
                        throw Error('AES must be instanitated with `new`');
                    }

                    Object.defineProperty(this, 'key',
                        {
                            value: coerceArray(key, true)
                        });

                    this._prepare();
                }

                AES.prototype._prepare = function ()
                {

                    var rounds = numberOfRounds[this.key.length];
                    if (rounds == null)
                    {
                        throw new Error('invalid key size (must be 16, 24 or 32 bytes)');
                    }

                    // encryption round keys
                    this._Ke = [];

                    // decryption round keys
                    this._Kd = [];

                    for (var i = 0; i <= rounds; i++)
                    {
                        this._Ke.push([0, 0, 0, 0]);
                        this._Kd.push([0, 0, 0, 0]);
                    }

                    var roundKeyCount = (rounds + 1) * 4;
                    var KC = this.key.length / 4;

                    // convert the key into ints
                    var tk = convertToInt32(this.key);

                    // copy values into round key arrays
                    var index;
                    for (var i = 0; i < KC; i++)
                    {
                        index = i >> 2;
                        this._Ke[index][i % 4] = tk[i];
                        this._Kd[rounds - index][i % 4] = tk[i];
                    }

                    // key expansion (fips-197 section 5.2)
                    var rconpointer = 0;
                    var t = KC,
                        tt;
                    while (t < roundKeyCount)
                    {
                        tt = tk[KC - 1];
                        tk[0] ^= ((S[(tt >> 16) & 0xFF] << 24) ^
                            (S[(tt >> 8) & 0xFF] << 16) ^
                            (S[tt & 0xFF] << 8) ^
                            S[(tt >> 24) & 0xFF] ^
                            (rcon[rconpointer] << 24));
                        rconpointer += 1;

                        // key expansion (for non-256 bit)
                        if (KC != 8)
                        {
                            for (var i = 1; i < KC; i++)
                            {
                                tk[i] ^= tk[i - 1];
                            }

                            // key expansion for 256-bit keys is "slightly different" (fips-197)
                        }
                        else
                        {
                            for (var i = 1; i < (KC / 2); i++)
                            {
                                tk[i] ^= tk[i - 1];
                            }
                            tt = tk[(KC / 2) - 1];

                            tk[KC / 2] ^= (S[tt & 0xFF] ^
                                (S[(tt >> 8) & 0xFF] << 8) ^
                                (S[(tt >> 16) & 0xFF] << 16) ^
                                (S[(tt >> 24) & 0xFF] << 24));

                            for (var i = (KC / 2) + 1; i < KC; i++)
                            {
                                tk[i] ^= tk[i - 1];
                            }
                        }

                        // copy values into round key arrays
                        var i = 0,
                            r, c;
                        while (i < KC && t < roundKeyCount)
                        {
                            r = t >> 2;
                            c = t % 4;
                            this._Ke[r][c] = tk[i];
                            this._Kd[rounds - r][c] = tk[i++];
                            t++;
                        }
                    }

                    // inverse-cipher-ify the decryption round key (fips-197 section 5.3)
                    for (var r = 1; r < rounds; r++)
                    {
                        for (var c = 0; c < 4; c++)
                        {
                            tt = this._Kd[r][c];
                            this._Kd[r][c] = (U1[(tt >> 24) & 0xFF] ^
                                U2[(tt >> 16) & 0xFF] ^
                                U3[(tt >> 8) & 0xFF] ^
                                U4[tt & 0xFF]);
                        }
                    }
                }

                AES.prototype.encrypt = function (plaintext)
                {
                    if (plaintext.length != 16)
                    {
                        throw new Error('invalid plaintext size (must be 16 bytes)');
                    }

                    var rounds = this._Ke.length - 1;
                    var a = [0, 0, 0, 0];

                    // convert plaintext to (ints ^ key)
                    var t = convertToInt32(plaintext);
                    for (var i = 0; i < 4; i++)
                    {
                        t[i] ^= this._Ke[0][i];
                    }

                    // apply round transforms
                    for (var r = 1; r < rounds; r++)
                    {
                        for (var i = 0; i < 4; i++)
                        {
                            a[i] = (T1[(t[i] >> 24) & 0xff] ^
                                T2[(t[(i + 1) % 4] >> 16) & 0xff] ^
                                T3[(t[(i + 2) % 4] >> 8) & 0xff] ^
                                T4[t[(i + 3) % 4] & 0xff] ^
                                this._Ke[r][i]);
                        }
                        t = a.slice();
                    }

                    // the last round is special
                    var result = createArray(16),
                        tt;
                    for (var i = 0; i < 4; i++)
                    {
                        tt = this._Ke[rounds][i];
                        result[4 * i] = (S[(t[i] >> 24) & 0xff] ^ (tt >> 24)) & 0xff;
                        result[4 * i + 1] = (S[(t[(i + 1) % 4] >> 16) & 0xff] ^ (tt >> 16)) & 0xff;
                        result[4 * i + 2] = (S[(t[(i + 2) % 4] >> 8) & 0xff] ^ (tt >> 8)) & 0xff;
                        result[4 * i + 3] = (S[t[(i + 3) % 4] & 0xff] ^ tt) & 0xff;
                    }

                    return result;
                }


                AES.prototype.decrypt = function (ciphertext)
                {
                    if (ciphertext.length != 16)
                    {
                        throw new Error('invalid ciphertext size (must be 16 bytes)');
                    }

                    var rounds = this._Kd.length - 1;
                    var a = [0, 0, 0, 0];

                    // convert plaintext to (ints ^ key)
                    var t = convertToInt32(ciphertext);
                    for (var i = 0; i < 4; i++)
                    {
                        t[i] ^= this._Kd[0][i];
                    }

                    // apply round transforms
                    for (var r = 1; r < rounds; r++)
                    {
                        for (var i = 0; i < 4; i++)
                        {
                            a[i] = (T5[(t[i] >> 24) & 0xff] ^
                                T6[(t[(i + 3) % 4] >> 16) & 0xff] ^
                                T7[(t[(i + 2) % 4] >> 8) & 0xff] ^
                                T8[t[(i + 1) % 4] & 0xff] ^
                                this._Kd[r][i]);
                        }
                        t = a.slice();
                    }

                    // the last round is special
                    var result = createArray(16),
                        tt;
                    for (var i = 0; i < 4; i++)
                    {
                        tt = this._Kd[rounds][i];
                        result[4 * i] = (Si[(t[i] >> 24) & 0xff] ^ (tt >> 24)) & 0xff;
                        result[4 * i + 1] = (Si[(t[(i + 3) % 4] >> 16) & 0xff] ^ (tt >> 16)) & 0xff;
                        result[4 * i + 2] = (Si[(t[(i + 2) % 4] >> 8) & 0xff] ^ (tt >> 8)) & 0xff;
                        result[4 * i + 3] = (Si[t[(i + 1) % 4] & 0xff] ^ tt) & 0xff;
                    }
                    return result;
                }


                /**
                 *  Mode Of Operation - Electonic Codebook (ECB)
                 */
                var ModeOfOperationECB = function (key)
                {
                    if (!(this instanceof ModeOfOperationECB))
                    {
                        throw Error('AES must be instanitated with `new`');
                    }

                    this.description = "Electronic Code Block";
                    this.name = "ecb";

                    this._aes = new AES(key);
                }

                ModeOfOperationECB.prototype.encrypt = function (plaintext)
                {
                    plaintext = coerceArray(plaintext);

                    if ((plaintext.length % 16) !== 0)
                    {
                        throw new Error('invalid plaintext size (must be multiple of 16 bytes)');
                    }

                    var ciphertext = createArray(plaintext.length);
                    var block = createArray(16);

                    for (var i = 0; i < plaintext.length; i += 16)
                    {
                        copyArray(plaintext, block, 0, i, i + 16);
                        block = this._aes.encrypt(block);
                        copyArray(block, ciphertext, i);
                    }

                    return ciphertext;
                }

                ModeOfOperationECB.prototype.decrypt = function (ciphertext)
                {
                    ciphertext = coerceArray(ciphertext);

                    if ((ciphertext.length % 16) !== 0)
                    {
                        throw new Error('invalid ciphertext size (must be multiple of 16 bytes)');
                    }

                    var plaintext = createArray(ciphertext.length);
                    var block = createArray(16);

                    for (var i = 0; i < ciphertext.length; i += 16)
                    {
                        copyArray(ciphertext, block, 0, i, i + 16);
                        block = this._aes.decrypt(block);
                        copyArray(block, plaintext, i);
                    }

                    return plaintext;
                }


                /**
                 *  Mode Of Operation - Cipher Block Chaining (CBC)
                 */
                var ModeOfOperationCBC = function (key, iv)
                {
                    if (!(this instanceof ModeOfOperationCBC))
                    {
                        throw Error('AES must be instanitated with `new`');
                    }

                    this.description = "Cipher Block Chaining";
                    this.name = "cbc";

                    if (!iv)
                    {
                        iv = createArray(16);

                    }
                    else if (iv.length != 16)
                    {
                        throw new Error('invalid initialation vector size (must be 16 bytes)');
                    }

                    this._lastCipherblock = coerceArray(iv, true);

                    this._aes = new AES(key);
                }

                ModeOfOperationCBC.prototype.encrypt = function (plaintext)
                {
                    plaintext = coerceArray(plaintext);

                    if ((plaintext.length % 16) !== 0)
                    {
                        throw new Error('invalid plaintext size (must be multiple of 16 bytes)');
                    }

                    var ciphertext = createArray(plaintext.length);
                    var block = createArray(16);

                    for (var i = 0; i < plaintext.length; i += 16)
                    {
                        copyArray(plaintext, block, 0, i, i + 16);

                        for (var j = 0; j < 16; j++)
                        {
                            block[j] ^= this._lastCipherblock[j];
                        }

                        this._lastCipherblock = this._aes.encrypt(block);
                        copyArray(this._lastCipherblock, ciphertext, i);
                    }

                    return ciphertext;
                }

                ModeOfOperationCBC.prototype.decrypt = function (ciphertext)
                {
                    ciphertext = coerceArray(ciphertext);

                    if ((ciphertext.length % 16) !== 0)
                    {
                        throw new Error('invalid ciphertext size (must be multiple of 16 bytes)');
                    }

                    var plaintext = createArray(ciphertext.length);
                    var block = createArray(16);

                    for (var i = 0; i < ciphertext.length; i += 16)
                    {
                        copyArray(ciphertext, block, 0, i, i + 16);
                        block = this._aes.decrypt(block);

                        for (var j = 0; j < 16; j++)
                        {
                            plaintext[i + j] = block[j] ^ this._lastCipherblock[j];
                        }

                        copyArray(ciphertext, this._lastCipherblock, 0, i, i + 16);
                    }

                    return plaintext;
                }


                /**
                 *  Mode Of Operation - Cipher Feedback (CFB)
                 */
                var ModeOfOperationCFB = function (key, iv, segmentSize)
                {
                    if (!(this instanceof ModeOfOperationCFB))
                    {
                        throw Error('AES must be instanitated with `new`');
                    }

                    this.description = "Cipher Feedback";
                    this.name = "cfb";

                    if (!iv)
                    {
                        iv = createArray(16);

                    }
                    else if (iv.length != 16)
                    {
                        throw new Error('invalid initialation vector size (must be 16 size)');
                    }

                    if (!segmentSize)
                    {
                        segmentSize = 1;
                    }

                    this.segmentSize = segmentSize;

                    this._shiftRegister = coerceArray(iv, true);

                    this._aes = new AES(key);
                }

                ModeOfOperationCFB.prototype.encrypt = function (plaintext)
                {
                    if ((plaintext.length % this.segmentSize) != 0)
                    {
                        throw new Error('invalid plaintext size (must be segmentSize bytes)');
                    }

                    var encrypted = coerceArray(plaintext, true);

                    var xorSegment;
                    for (var i = 0; i < encrypted.length; i += this.segmentSize)
                    {
                        xorSegment = this._aes.encrypt(this._shiftRegister);
                        for (var j = 0; j < this.segmentSize; j++)
                        {
                            encrypted[i + j] ^= xorSegment[j];
                        }

                        // Shift the register
                        copyArray(this._shiftRegister, this._shiftRegister, 0, this.segmentSize);
                        copyArray(encrypted, this._shiftRegister, 16 - this.segmentSize, i, i + this.segmentSize);
                    }

                    return encrypted;
                }

                ModeOfOperationCFB.prototype.decrypt = function (ciphertext)
                {
                    if ((ciphertext.length % this.segmentSize) != 0)
                    {
                        throw new Error('invalid ciphertext size (must be segmentSize bytes)');
                    }

                    var plaintext = coerceArray(ciphertext, true);

                    var xorSegment;
                    for (var i = 0; i < plaintext.length; i += this.segmentSize)
                    {
                        xorSegment = this._aes.encrypt(this._shiftRegister);

                        for (var j = 0; j < this.segmentSize; j++)
                        {
                            plaintext[i + j] ^= xorSegment[j];
                        }

                        // Shift the register
                        copyArray(this._shiftRegister, this._shiftRegister, 0, this.segmentSize);
                        copyArray(ciphertext, this._shiftRegister, 16 - this.segmentSize, i, i + this.segmentSize);
                    }

                    return plaintext;
                }

                /**
                 *  Mode Of Operation - Output Feedback (OFB)
                 */
                var ModeOfOperationOFB = function (key, iv)
                {
                    if (!(this instanceof ModeOfOperationOFB))
                    {
                        throw Error('AES must be instanitated with `new`');
                    }

                    this.description = "Output Feedback";
                    this.name = "ofb";

                    if (!iv)
                    {
                        iv = createArray(16);

                    }
                    else if (iv.length != 16)
                    {
                        throw new Error('invalid initialation vector size (must be 16 bytes)');
                    }

                    this._lastPrecipher = coerceArray(iv, true);
                    this._lastPrecipherIndex = 16;

                    this._aes = new AES(key);
                }

                ModeOfOperationOFB.prototype.encrypt = function (plaintext)
                {
                    var encrypted = coerceArray(plaintext, true);

                    for (var i = 0; i < encrypted.length; i++)
                    {
                        if (this._lastPrecipherIndex === 16)
                        {
                            this._lastPrecipher = this._aes.encrypt(this._lastPrecipher);
                            this._lastPrecipherIndex = 0;
                        }
                        encrypted[i] ^= this._lastPrecipher[this._lastPrecipherIndex++];
                    }

                    return encrypted;
                }

                // Decryption is symetric
                ModeOfOperationOFB.prototype.decrypt = ModeOfOperationOFB.prototype.encrypt;


                /**
                 *  Counter object for CTR common mode of operation
                 */
                var Counter = function (initialValue)
                {
                    if (!(this instanceof Counter))
                    {
                        throw Error('Counter must be instanitated with `new`');
                    }

                    // We allow 0, but anything false-ish uses the default 1
                    if (initialValue !== 0 && !initialValue)
                    {
                        initialValue = 1;
                    }

                    if (typeof (initialValue) === 'number')
                    {
                        this._counter = createArray(16);
                        this.setValue(initialValue);

                    }
                    else
                    {
                        this.setBytes(initialValue);
                    }
                }

                Counter.prototype.setValue = function (value)
                {
                    if (typeof (value) !== 'number' || parseInt(value) != value)
                    {
                        throw new Error('invalid counter value (must be an integer)');
                    }

                    for (var index = 15; index >= 0; --index)
                    {
                        this._counter[index] = value % 256;
                        value = value >> 8;
                    }
                }

                Counter.prototype.setBytes = function (bytes)
                {
                    bytes = coerceArray(bytes, true);

                    if (bytes.length != 16)
                    {
                        throw new Error('invalid counter bytes size (must be 16 bytes)');
                    }

                    this._counter = bytes;
                };

                Counter.prototype.increment = function ()
                {
                    for (var i = 15; i >= 0; i--)
                    {
                        if (this._counter[i] === 255)
                        {
                            this._counter[i] = 0;
                        }
                        else
                        {
                            this._counter[i]++;
                            break;
                        }
                    }
                }


                /**
                 *  Mode Of Operation - Counter (CTR)
                 */
                var ModeOfOperationCTR = function (key, counter)
                {
                    if (!(this instanceof ModeOfOperationCTR))
                    {
                        throw Error('AES must be instanitated with `new`');
                    }

                    this.description = "Counter";
                    this.name = "ctr";

                    if (!(counter instanceof Counter))
                    {
                        counter = new Counter(counter)
                    }

                    this._counter = counter;

                    this._remainingCounter = null;
                    this._remainingCounterIndex = 16;

                    this._aes = new AES(key);
                }

                ModeOfOperationCTR.prototype.encrypt = function (plaintext)
                {
                    var encrypted = coerceArray(plaintext, true);

                    for (var i = 0; i < encrypted.length; i++)
                    {
                        if (this._remainingCounterIndex === 16)
                        {
                            this._remainingCounter = this._aes.encrypt(this._counter._counter);
                            this._remainingCounterIndex = 0;
                            this._counter.increment();
                        }
                        encrypted[i] ^= this._remainingCounter[this._remainingCounterIndex++];
                    }

                    return encrypted;
                }

                // Decryption is symetric
                ModeOfOperationCTR.prototype.decrypt = ModeOfOperationCTR.prototype.encrypt;


                ///////////////////////
                // Padding

                // See:https://tools.ietf.org/html/rfc2315
                function pkcs7pad(data)
                {
                    data = coerceArray(data, true);
                    var padder = 16 - (data.length % 16);
                    var result = createArray(data.length + padder);
                    copyArray(data, result);
                    for (var i = data.length; i < result.length; i++)
                    {
                        result[i] = padder;
                    }
                    return result;
                }

                function pkcs7strip(data)
                {
                    data = coerceArray(data, true);
                    if (data.length < 16)
                    {
                        throw new Error('PKCS#7 invalid length');
                    }

                    var padder = data[data.length - 1];
                    if (padder > 16)
                    {
                        throw new Error('PKCS#7 padding byte out of range');
                    }

                    var length = data.length - padder;
                    for (var i = 0; i < padder; i++)
                    {
                        if (data[length + i] !== padder)
                        {
                            throw new Error('PKCS#7 invalid padding byte');
                        }
                    }

                    var result = createArray(length);
                    copyArray(data, result, 0, 0, length);
                    return result;
                }

                ///////////////////////
                // Exporting


                // The block cipher
                var aesjs = {
                    AES: AES,
                    Counter: Counter,

                    ModeOfOperation:
                    {
                        ecb: ModeOfOperationECB,
                        cbc: ModeOfOperationCBC,
                        cfb: ModeOfOperationCFB,
                        ofb: ModeOfOperationOFB,
                        ctr: ModeOfOperationCTR
                    },

                    utils:
                    {
                        hex: convertHex,
                        utf8: convertUtf8
                    },

                    padding:
                    {
                        pkcs7:
                        {
                            pad: pkcs7pad,
                            strip: pkcs7strip
                        }
                    },

                    _arrayTest:
                    {
                        coerceArray: coerceArray,
                        createArray: createArray,
                        copyArray: copyArray,
                    }
                };


                // node.js
                if (typeof exports !== 'undefined')
                {
                    module.exports = aesjs

                    // RequireJS/AMD
                    // http://www.requirejs.org/docs/api.html
                    // https://github.com/amdjs/amdjs-api/wiki/AMD
                }
                else if (typeof (define) === 'function' && define.amd)
                {
                    define(aesjs);

                    // Web Browsers
                }
                else
                {

                    // If there was an existing library at "aesjs" make sure it's still available
                    if (root.aesjs)
                    {
                        aesjs._aesjs = root.aesjs;
                    }

                    root.aesjs = aesjs;
                }


            })(this);

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

// globals.js is embedded in template.html (and the node bundle) along with the other client files
// it holds every piece of state that more than one file uses, one group per topic
// every top-level var and function between aes-js.js and the end of main.js lives in one closure, so
// all of them are visible everywhere; the prefix is the convention: g_ = app-wide state, G_ = app-wide
// constant, no prefix = private to the file that declares it

// ---- connection and session ----

var g_are_server_details_predefined = false;

// autoconnect without a click: browsers only start audio after a user gesture, so a page opened
// straight from disk may refuse the AudioContext; set this to false if that bites (a click on
// connect is enough). not an issue in the android webview or on a site the user already clicked

var g_is_autoconnect_without_user_action_active = false;

// whether the last connect attempt actually failed (so the login shows
// "connection failed, retrying in Xs" - not before)
var g_last_connect_attempt_failed = false;

// the server details used when g_are_server_details_predefined is true (no prompt for host, port
// and keys); with autoconnect on top the page needs no click at all, at the cost of the sound rule above

var g_autoconnect_details = {
    host: "192.168.1.106",
    port: 1111,
    keys: [
        "test",
        // "test2"
    ]
};

// the username this client asks for while connecting; the server uses it instead of the
// assigned user0/user1/... name, provided nobody connected is using it. leave "" for the
// assigned name. a registered identity's admin-set name still wins over this
var g_chosen_username = "";

var g_host = "";

var g_is_websocket_connected = false;

var g_is_authenticated = false;

var g_should_connection_check_be_running = false;

// the heartbeat: one every interval_ms, the link counts as lost after lost_threshold_ms without
// a reply (three missed 10 s heartbeats plus slack, so a dead network shows within a minute)
var g_connection_check = {
    interval_ms: 10 * 1000,
    lost_threshold_ms: 35 * 1000,
    sleep_resolve: null,        // wakes the check loop early
    last_response_timestamp: 0  // set on every heartbeat reply, first when the client connects
};

// session statistics for the strip themes' session-info card. bytes are counted at
// the two choke points every message passes through (encrypted base64 lengths, so
// "wire-ish" numbers); ping is the heartbeat round trip.
var g_session = {
    bytes_sent: 0,
    bytes_received: 0,
    connected_at: 0,
    ping_sent_at: 0,
    last_ping_ms: -1
};

// fast reconnect (server setting): a lost socket keeps the page exactly as it is and re-dials with
// the same identity; the server adopts the still-open session. only a failed attempt shows
// "connection lost" and wipes the page
var g_fast_reconnect = {
    in_progress: false,   // an attempt is running: no wipe, no toast, one attempt only
    resumed: false,       // the server said fast_reconnect_ok: the lists that follow are a refresh
    deadline_timer: null, // the classic "connection lost" path when the attempt stalls
    pending_lists: null   // the refreshed lists, applied together so the page repaints once
};

// the local keypair as { public_key_string, identity_string }, awaited by the driver and filled by
// dispatch.js when the worker made it; connection.js builds the slot at load. the private key stays in the worker
var g_identity_slot = null;

// the live status for the login page. the local driver writes it; in the webview,
// node's reports arriving over the loopback overwrite it, because node owns the real connection
var g_connection_status = {
    state: "idle",          // idle | connecting | waiting_retry | connected
    reason: "",             // why the last attempt failed, human readable
    next_retry_at: 0,       // ms timestamp of the next automatic attempt, 0 = none
    last_connected_at: 0    // ms timestamp the server connection last existed, 0 = never
};

// set through the export seam. every listener receives every status change; under node
// two exist, the loopback ui and the java bridge
var g_connection_status_listeners = [];

// filled by the close and error handlers, consumed by the driver for the retry status
var g_last_disconnect_reason = "";

// android reports the device's real network state over the bridge; null means never told
// a browser has navigator.onLine, but the android webview lies about it, so this is the
// only trustworthy answer to whether the phone has any network
var g_device_has_network = null;

// ---- identity and crypto ----

var g_metadata_keys = [];

var g_dh_generator = null;

var g_dh_modulus = null;

var g_dh_secret_exponent = null;

if (typeof window != 'undefined')
{
    g_dh_generator = 2n;

    var dh_modulus_bits = 8192; // 2048, 4096, or 8192 - MUST match the server's DH_MODULUS_BITS in dh_primes.h

    // safe prime; the size is chosen by dh_modulus_bits above
    var dh_modulus_string = "32317006071311007300338913926423828248817941241140239112842009751400741706634354222619689417363569347117901737909704191754605873209195028853758986185622153212175412514901774520270235796078236248884246189477587641105928646099411723245426622522193230540919037680524235519125679715870117001058055877651038861847280257976054903569732561526167081339361799541336476559160368317896729073178384589680639671900977202194168647225871031411336429319536193471636533209717077448227988588565369208645296636077250268955505928362751121174096972998068410554359584866583291642136218231078990999448652468262416972035911852507045361090559";
    if (dh_modulus_bits == 4096) { dh_modulus_string = "769693417275193209984647063932271739387855846059952565355802298991172654607712104048642837327086393649061117273977479029847880929901816490502575106445708811728815104699538212859676255621694933541780065300216380119365448477045659714142752962409351060465337847941705392356465059912091910379610354725312649190019796723866880686790102505810145302961022375682955537024852712153016097337874982984026217981644194741246064934907045623310252540105014478602030042625050790892256552738501094150544017503366521405695110938208299693781667383898463231081051406284286841973557391837014717399840317430691522097547152517168661381236591027075489125945391080953462725086945640553263655450552529331021143283920078415126554651532264427032676910742519456229972244566183184460134372157613705601578581124341629241972089511142281008551119184446873409929566545851290281361166571221433352162292794386037934251491816926283501321267998170847037246436312385384549128374516008246102779333275418845691810684079267893733515705735729967088709311436111220883412133997678612136656076019025878523079903588858263406471350679442414909683284164773663260965983542100471510190345089294782440003918912451456992077790689703598681468439291465843547007915368517110174489101683183"; }
    if (dh_modulus_bits == 8192) { dh_modulus_string = "977457999394373613160803436413990067824664325329752783398860665650891758153546599425963979290921371512554424155404999662026528515231003619059250227581073193191817667145872254566150016152267378041043707227533090523130854162674719502912605093407840683480360272745764870153876137077404098306192221010043540729677343845507367074906936225679715947791388837030145881441948294883090889231339114529926218527174080089614380520453541140942641189135120655392817995558672348259240314664441211284094553472715483266674338226096876439738570920830431990068192898823624154982561509367679529622217472548774982858792731946532808614927934739139866206407985368007801168974233995065956999264060271473667589912911635282735847769997114239537087675283088650912126339354407150753898672557625721020468123121985217987055904104587919799098757089865856148151372159009557757252554523938389147793317088678845894492183206966490686288450161789777284440531583751707235355985011244929317700461604659491747569397015417605015844114189684757052804077400345793511780894375616367276781058142309055525279814951138171200246013005920763485637476273362810215876618783686490089700542042236756450450875748150296041125307287408385472961923570457489504445415040799421139916299426300456654408986639074321942096027448333024943556858265321864649169731360525833923693176347415312865652669889035367188555225154697353012485541899891405325448203641838378457181625265662866012641101851261909549715296062722220871699753970653198277244417978954211900339094323007747108915608099803996978341421956304178871205193962176652206358214977516703892527582137534788148271002406853630936928238671372686645287319965234542895792366832783193938302132219463877916744337606350077802640502896551808414614146119887726456058723288826440787605655349724793408478985959688481296784606909713086104258354166909655924759369179237403392373490546567686597579050582406536565868808879640217167277372359442106490085619712603267716148318470568791898988932303644832977117895522999512850187807810874398403568491329765776005361335497620431318887438033026603280081068652656086250926318691324234636743583950635209943952384403217947022081052893713058850302983931039183796265186758214153198152532323300955155280353467780525511888234737731346004632762960546857790075686663681335897303755575277977768637069626833995439307976899886428471454602498498262648098952772543021280017770216979020055000596511525631600903871686087096720903178326987700681798435674674029499323"; }
    g_dh_modulus = BigInt(dh_modulus_string);
}

var g_my_rsa_key_object = null;

var g_rsa_public_key_string = "";

var g_identity_string = "";

var g_is_rsa_key_generated = false;

var g_keys_init_status = false;

var g_is_identity_switch_in_progress = false; // deliberate disconnect->new-keypair->reconnect cycle (top bar "identity" button)

// the size this server last asked for, so the offer is not repeated on every redial. the
// connection driver retries forever, and the rejection repeats on every attempt
var g_rsa_key_too_weak_prompted_for_bits = 0;

// ---- server policy ----

// server-side policy this client only obeys, announced at login (authentication_status) and on every
// admin save (server_policy); absent fields on an older server keep these defaults. the keys are the
// wire names, so server_settings_tab__apply_server_policy_fields copies them generically (a number only when positive)
var g_server_policy = {
    is_fast_reconnect_allowed: false,          // a lost socket may resume its session instead of starting over
    show_music_bot_marquee_to_everyone: false, // a streaming bot's marquee also for people outside its channel
    is_alias_registration_allowed: false,      // admins may register aliases (display names) on identities
    allow_typing_indicator: false,             // "x is typing ..." is sent and shown
    allow_client_renames: true,                // users may rename themselves (admins always may); on unless the server says otherwise
    allow_avatars: false,                      // image avatars on identities
    avatar_max_size: 51200,                    // largest raw avatar image in bytes
    allow_file_uploads: false,                 // any file may be sent in chat; off until the server says so
    file_upload_max_size: 10 * 1024 * 1024,    // largest raw chat file in bytes
    allow_chat_pictures: true,                 // inline pictures in chat; on unless the server says otherwise
    chat_picture_max_size: 4 * 1024 * 1024,    // largest raw inline picture in bytes; bigger images still travel as files
    icon_max_size: 5000                        // largest raw tag/channel icon in bytes (the server settings tab upload)
};

// ---- who is here ----

var g_local_username = "";

var g_local_client_id = 0;

// true once the server granted this session the admin tag
var g_is_local_client_admin = false;

var g_client_list = [];

var g_channel_list = [];

var g_icons = [];

var g_tags = [];

var g_map_client_id_to_array_index = new Map();

var g_offline_client_list = []; // might be supported by the server, might not

var g_is_client_list_retrieved = false;

var g_is_channel_list_retrieved = false;

var g_selected_channel_id;

var g_selected_client_id;

var g_current_channel_id = 0;

// internal sentinel meaning "the root level" while walking the channel tree.
// root channels are identified by their is_root_channel flag, never by this value
// (server ids are uint64 and can no longer carry -1).
const g_ROOT_LEVEL_PARENT_SENTINEL = -1;

var g_chat_message_author_public_keys = {}; // server chat message id -> author's public key; used to decide whether to honour an incoming delete/edit

// ---- channel keys ----

var g_current_channel_keys = null;

// the previous channel keys: keys change when somebody joins, so a message encrypted with the old ones
// can still arrive; the data-processing worker tries these too, and drops them on a channel switch

var g_historic_keys_of_current_channel = [];

// ---- chat context and composer ----

var g_chat_context_array = [
    {
        type: "channel",
        chat_context_id: "chat-context-channel-0",
        last_known_message_sender_username: ""
    }
];

var g_current_chat_context_id = "chat-context-channel-0";

var g_chat_message_receiver_type = "channel";

var g_chat_message_receiver_id = "main";

// which offline person the chat input is currently addressing (set when their circle is
// tapped). empty whenever the open conversation is a channel or a connected client.
var g_offline_chat_recipient_alias = "";

var g_selected_server_chat_message_id = null;

var g_local_message_id = 0;

var g_selected_font = "custom-font-usage-default";

var g_selected_font_color = "#ffffff";

var g_selected_font_size = 12;

var g_base64_picture_string_to_send = "" ;

var g_file_send_intent = "";

var g_file_send_intent_extra_data = {};

var g_is_file_being_uploaded = false;

// ---- platform ----

var g_is_client_running_under_touch_device = false;  // for touch devices

var g_is_running_in_android_webview = false;

// AVATAR PREFETCH (option). on by default: after joining, quietly ask for every connected
// person's avatar one at a time so faces are there before anybody is clicked.
var g_android_app_mode = "";

// first push is the app handing over saved settings, later ones mean a switch moved while running
var g_have_received_android_settings = false;

var g_is_microphone_enabled_on_touch_device = false;  // for touch devices

// true on the page, false inside a web worker and in the node runtime, where document does not exist
var G_HAS_DOM = (typeof window !== "undefined" && typeof document !== "undefined");

// ---- workers ----

var g_opus_encoder_worker = null;

var g_opus_decoder_worker = null;

var g_data_processing_worker = null;

var g_minimp3_worker = null;

var g_websocket_worker = null;

// ---- ui flags ----

var g_textarea_log = null;

var g_is_chat_hidden = false;

var g_show_hide_toggle = false;

// optional per-theme extras: flat channel list + a live right-pane member list
var g_is_channel_list_flattened = false;

var g_alert_push_to_talk_key_shown_once = false;

var g_alert_streaming_music_shown_once = false;

var g_are_sound_effects_enabled = true;

var g_stop_song_stream_message_received = false;

var g_selected_song_name = null;

// ---- connect page hold ----

// launching the webview onto a node that is already logged in used to show the connect
// page for a moment before the burst arrived. the page STARTS held back (the spinner is
// the first page); it is revealed only when connecting turns out to fail or stall
var g_is_holding_back_connect_page = true;

// the spinner's reveal deadline; node's "still connecting" reports push it out
var g_connect_holdback_deadline = 0;

var g_connect_holdback_started = 0;

// ---- idle ----

// deep idle state (android background mode): only the websocket + slow heartbeat stay alive
var g_is_deep_idle = false;

var g_is_deep_idle_pending = false;

// ---- typing, unread, avatars, layout ----

// typing indicator (only alive when the server policy allows it). g_typing.state maps a chat
// context id to { client_id: expiry_timestamp_ms } - an entry that is not refreshed just
// expires, so a sender that disappears mid-sentence never leaves "x is typing" hanging
var g_typing = {
    state: {},          // chat context id -> { client_id: expiry_timestamp_ms }
    last_sent_at: 0,
    render_timer: null
};

// unread message count per channel, keyed by channel id; the channel being looked at is always zero.
// kept as state, not only in the badge markup, so node can raise a notification without a dom
var g_channel_unread_counts = {};

// avatars (server opt-in, g_server_policy.allow_avatars). cache maps client_id -> base64
// data-url; the queue drives chunked lazy loading; g_profile_avatar_client_id is the client whose
// avatar belongs in the big right-pane #current-client-avatar.
var g_profile_avatar_client_id = -1;

// the avatar state; grid_visible gates painting faces into the small circles (in other themes that
// circle is the mic-state icon), the big right-pane #current-client-avatar works in every theme
var g_avatar = {
    grid_visible: false,   // the avatar grid member list (strip themes) is shown
    load_queue: [],        // client ids whose avatar is wanted, loaded in chunks
    load_scheduled: false,
    prefetch_queue: [],    // everyone connected, asked one at a time after joining
    prefetch_timer: null
};

// the desktop grid layout: the three columns (channels, chat, info) and the chat-input row in one css
// grid; order, the input row's place and column widths are editable and persist under "lemon_layout".
// touch devices keep the legacy layout, so grid_active stays false there
var g_layout = {
    grid_active: false,
    edit_active: false,
    panels: null,        // name -> panel element, filled at init
    drag: null,          // active column-width drag
    edit_dragged: null,  // panel name being moved in edit mode
    state: {             // what persists in localStorage under "lemon_layout"
        order: ["channels", "chat", "info"], // left-to-right column order
        input_col: "chat",                   // which column holds the chat-input row
        input_pos: "bottom",                 // "top" or "bottom" inside that column
        col_channels: "15%",                 // channels column width (becomes px after a drag)
        col_info: "13%"                      // info column width
    }
};

// ---- chat files ----

// transfers that are currently being received, stored by file id. encrypted_size is the total
// the progress ring divides by
var g_chat_file_transfers = {};

// decrypted files stored by card key, which is the server message id, or "local-N" for our own echo
// they live here because the download button should not carry megabytes in a dom attribute
var g_chat_files_by_message_id = {};

// the file the user attached but has not sent yet, as { name, size, mime, base64 }
var g_pending_chat_file = null;

// ---- server settings tab ----

var g_channel_properties_edit_channel_id = null; // the channel currently open in the edit form (null while creating); the form's icon box targets it

var g_icon_upload_queue = [];             // base64 icons waiting to be uploaded one at a time

var g_icon_upload_in_flight_base64 = null; // the icon whose server reply we are currently waiting for

// the admin's country block list as edited in the settings tab; the selectable countries come from
// the flag stylesheet the client ships, the display names from the browser's Intl

// the admin's unsaved block list, replaced whenever server_settings_values arrives
var g_blocked_countries_draft = [];

// ---- local settings (read from localStorage by node-runtime.js at load) ----

// size of the rsa identity keypair this device creates. it is part of the identity function:
// the same passphrase at a different size is a DIFFERENT keypair, so changing this makes the
// server see a new person. only the sizes the wasm and the server both accept are allowed
var G_ALLOWED_RSA_KEY_BITS = [2048, 3072, 4096, 6144, 8192];

var g_rsa_key_bits = 2048;

// local-only preference for the avatar circles next to chat messages
var g_show_message_avatars = false;

// read receipts have two halves that are set separately: whether we draw the eye others send
// us, and whether we send one back. both are on unless turned off
var g_show_seen_indicator = true;

var g_send_seen_receipts = true;

// a received message scrolls the chat to the end; off keeps the place of somebody reading older messages
var g_auto_scroll_chat_to_end = true;

// the user can hide the mic button outright; it stays visible by default
var g_hide_microphone_button = false;

// local-only preference for which microphone to capture from; "" means the browser's default
var g_selected_microphone_device_id = "";

// local-only preference that makes the mic button toggle transmission instead of push-to-talk
var g_is_continuous_mic_mode = false;

// true while continuous-mode transmission is running: a tap started it and no tap has stopped it yet
var g_is_continuous_transmission_active = false;

// which key pushes to talk. 81 is Q, the long-standing default
var g_push_to_talk_key_code = 81;

var g_push_to_talk_key_label = "Q";

// just a copy so the checkbox can show the right state, because the real flag lives in java
var g_is_file_logging_enabled = false;

// ---- node runtime (the headless android service) ----

// loopback mode means connecting to the node runtime on-device, plaintext and token-gated
// the port stays zero on the desktop and website, which never receive these settings fields
var g_loopback_port = 0;

var g_loopback_token = "";

// the server's auth frame, kept from the moment it arrives, because the ui replay leads with it
var g_node_cached_auth_frame = null;

// false parks the connection: the socket is closed and the reconnect ticker idles. it is
// always true in the browser; only the node host flips it, for the webview handover
var g_node_connection_wanted = true;

// set through the export seam. it receives every decrypted server frame raw, for the ui replay
var g_node_frame_listener = null;

// node only: true while a ui is attached to the loopback. node runs the same client
// code as the page, so without this it would assume somebody is reading its "current"
// channel and never count those messages as unread
var g_node_has_attached_ui = false;

// the bridge host's hook for incoming calls. it stays null everywhere but android
var g_node_incoming_call_listener = null;

// set through the export seam. every listener is called after every dispatched message
var g_node_message_listeners = [];

// set through the export seam. it receives the total unread count for the launcher icon badge
var g_node_unread_listener = null;

// ---- sounds ----

// the 14 ui sound clips; sounds.js fills it at load, main.js, ui.js and dispatch.js play them
var g_sound_effects = null;

// ---- audio and voice ----

const G_AUDIO_STATE = {
    PUSH_TO_TALK_ACTIVE: 1,
    PUSH_TO_TALK_ENABLED: 2,
    PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS: 3,
    AUDIO_COMPLETELY_DISABLED: 4
};

// for voice chat

var g_peer_connection_with_server = null;

var g_iceconfig = null;

var g_datachannel = null;

var g_is_webrtc_datachannel_connected = false;

var g_is_webrtc_datachannel_check_running = false;

// server-side datachannel cooldown: after 10 attempts that never connected the server refuses to
// build peers for a while and says for how long; the retry loop sleeps that long instead of 10 s
var g_webrtc_datachannel_cooldown_until_ms = 0;

var g_datachannel_retry_sleep_resolve = null; // a login resolves it early, the server then starts counting afresh

var g_is_voice_chat_allowed_by_server = false;   // audio subsystem active (datachannel kept up); true when client voice OR music-bot audio is on

var g_is_client_microphone_allowed_by_server = false;   // may this client transmit its own mic; true only when client voice is on

var g_local_audio_stream = null;

var g_is_microphone_enabled = false;

var g_is_microphone_active = false;

// whether audio can actually be sent right now. false means the datachannel is gone or
// the server disabled audio for us
var g_is_microphone_available = false;

var g_is_microphone_always_on = false;

var g_last_sent_value_microphone_usage = false;

var g_audio_config = {
    codec: {
        bufferSize: 16384 / 2
    }
};

var g_opus_decoding_sampler = null;

var g_silence = null;

var g_audio_context = null;

var g_microphone_recorder = null;

var g_audio_player_gain_node = null;

var g_audio_recorder_gain_node = null;

var g_audio_input = null;

var g_client_volume_by_id = {};  // local per-client playback volume (client_id -> gain, 1.0 = default); worklet mode only, never sent to the server

// per-sender frame sequence for outgoing voice/song audio; 16 bits, wraps at 65536
var g_voice_send_sequence_number = 0;

            /**
             *
             *  Secure Hash Algorithm (SHA256)
             *  http://www.webtoolkit.info/
             *
             *  Original code by Angel Marin, Paul Johnston.
             *
             **/

            function SHA256(s)
            {

                var chrsz = 8;
                var hexcase = 0;

                function safe_add(x, y)
                {
                    var lsw = (x & 0xFFFF) + (y & 0xFFFF);
                    var msw = (x >> 16) + (y >> 16) + (lsw >> 16);
                    return (msw << 16) | (lsw & 0xFFFF);
                }

                function S(X, n)
                {
                    return (X >>> n) | (X << (32 - n));
                }

                function R(X, n)
                {
                    return (X >>> n);
                }

                function Ch(x, y, z)
                {
                    return ((x & y) ^ ((~x) & z));
                }

                function Maj(x, y, z)
                {
                    return ((x & y) ^ (x & z) ^ (y & z));
                }

                function Sigma0256(x)
                {
                    return (S(x, 2) ^ S(x, 13) ^ S(x, 22));
                }

                function Sigma1256(x)
                {
                    return (S(x, 6) ^ S(x, 11) ^ S(x, 25));
                }

                function Gamma0256(x)
                {
                    return (S(x, 7) ^ S(x, 18) ^ R(x, 3));
                }

                function Gamma1256(x)
                {
                    return (S(x, 17) ^ S(x, 19) ^ R(x, 10));
                }

                function core_sha256(m, l)
                {
                    var K = new Array(0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5, 0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174, 0xE49B69C1, 0xEFBE4786, 0xFC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA, 0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x6CA6351, 0x14292967, 0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85, 0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070, 0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3, 0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2);
                    var HASH = new Array(0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19);
                    var W = new Array(64);
                    var a, b, c, d, e, f, g, h, i, j;
                    var T1, T2;

                    m[l >> 5] |= 0x80 << (24 - l % 32);
                    m[((l + 64 >> 9) << 4) + 15] = l;

                    for (var i = 0; i < m.length; i += 16)
                    {
                        a = HASH[0];
                        b = HASH[1];
                        c = HASH[2];
                        d = HASH[3];
                        e = HASH[4];
                        f = HASH[5];
                        g = HASH[6];
                        h = HASH[7];

                        for (var j = 0; j < 64; j++)
                        {
                            if (j < 16) W[j] = m[j + i];
                            else W[j] = safe_add(safe_add(safe_add(Gamma1256(W[j - 2]), W[j - 7]), Gamma0256(W[j - 15])), W[j - 16]);

                            T1 = safe_add(safe_add(safe_add(safe_add(h, Sigma1256(e)), Ch(e, f, g)), K[j]), W[j]);
                            T2 = safe_add(Sigma0256(a), Maj(a, b, c));

                            h = g;
                            g = f;
                            f = e;
                            e = safe_add(d, T1);
                            d = c;
                            c = b;
                            b = a;
                            a = safe_add(T1, T2);
                        }

                        HASH[0] = safe_add(a, HASH[0]);
                        HASH[1] = safe_add(b, HASH[1]);
                        HASH[2] = safe_add(c, HASH[2]);
                        HASH[3] = safe_add(d, HASH[3]);
                        HASH[4] = safe_add(e, HASH[4]);
                        HASH[5] = safe_add(f, HASH[5]);
                        HASH[6] = safe_add(g, HASH[6]);
                        HASH[7] = safe_add(h, HASH[7]);
                    }
                    return HASH;
                }

                function str2binb(str)
                {
                    var bin = Array();
                    var mask = (1 << chrsz) - 1;
                    for (var i = 0; i < str.length * chrsz; i += chrsz)
                    {
                        bin[i >> 5] |= (str.charCodeAt(i / chrsz) & mask) << (24 - i % 32);
                    }
                    return bin;
                }

                function Utf8Encode(string)
                {
                    string = string.replace(/\r\n/g, "\n");
                    var utftext = "";

                    for (var n = 0; n < string.length; n++)
                    {

                        var c = string.charCodeAt(n);

                        if (c < 128)
                        {
                            utftext += String.fromCharCode(c);
                        }
                        else if ((c > 127) && (c < 2048))
                        {
                            utftext += String.fromCharCode((c >> 6) | 192);
                            utftext += String.fromCharCode((c & 63) | 128);
                        }
                        else
                        {
                            utftext += String.fromCharCode((c >> 12) | 224);
                            utftext += String.fromCharCode(((c >> 6) & 63) | 128);
                            utftext += String.fromCharCode((c & 63) | 128);
                        }

                    }

                    return utftext;
                }

                function binb2hex(binarray)
                {
                    var hex_tab = hexcase ? "0123456789ABCDEF" : "0123456789abcdef";
                    var str = "";
                    for (var i = 0; i < binarray.length * 4; i++)
                    {
                        str += hex_tab.charAt((binarray[i >> 2] >> ((3 - i % 4) * 8 + 4)) & 0xF) +
                            hex_tab.charAt((binarray[i >> 2] >> ((3 - i % 4) * 8)) & 0xF);
                    }
                    return str;
                }

                s = Utf8Encode(s);
                return binb2hex(core_sha256(str2binb(s), s.length * chrsz));
            }

            var sha256 = {};
            sha256.hex = function (s)
            {
                return SHA256(s);
            };

            //NEW JAVASCRIPT FILE
            /**
         * [js-sha256]{@link https://github.com/emn178/js-sha256}
         *
         * @version 0.9.0
         * @author Chen, Yi-Cyuan [emn178@gmail.com]
         * @copyright Chen, Yi-Cyuan 2014-2017
         * @license MIT
         */
            /*jslint bitwise: true */



            (function ()
            {
                //'use strict';

                var ERROR = 'input is invalid type';
                var WINDOW = typeof window === 'object';
                var root = WINDOW ? window : {};
                if (root.JS_SHA256_NO_WINDOW)
                {
                    WINDOW = false;
                }
                var WEB_WORKER = !WINDOW && typeof self === 'object';
                var NODE_JS = !root.JS_SHA256_NO_NODE_JS && typeof process === 'object' && process.versions && process.versions.node;
                if (NODE_JS)
                {
                    root = global;
                } else if (WEB_WORKER)
                {
                    root = self;
                }
                var COMMON_JS = !root.JS_SHA256_NO_COMMON_JS && typeof module === 'object' && module.exports;
                var AMD = typeof define === 'function' && define.amd;
                var ARRAY_BUFFER = !root.JS_SHA256_NO_ARRAY_BUFFER && typeof ArrayBuffer !== 'undefined';
                var HEX_CHARS = '0123456789abcdef'.split('');
                var EXTRA = [-2147483648, 8388608, 32768, 128];
                var SHIFT = [24, 16, 8, 0];
                var K = [
                    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
                    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
                    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
                    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
                    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
                ];
                var OUTPUT_TYPES = ['hex', 'array', 'digest', 'arrayBuffer'];

                var blocks = [];

                if (root.JS_SHA256_NO_NODE_JS || !Array.isArray)
                {
                    Array.isArray = function (obj)
                    {
                        return Object.prototype.toString.call(obj) === '[object Array]';
                    };
                }

                if (ARRAY_BUFFER && (root.JS_SHA256_NO_ARRAY_BUFFER_IS_VIEW || !ArrayBuffer.isView))
                {
                    ArrayBuffer.isView = function (obj)
                    {
                        return typeof obj === 'object' && obj.buffer && obj.buffer.constructor === ArrayBuffer;
                    };
                }

                var createOutputMethod = function (outputType, is224)
                {
                    return function (message)
                    {
                        return new _Sha256(is224, true).update(message)[outputType]();
                    };
                };

                var createMethod = function (is224)
                {
                    var method = createOutputMethod('hex', is224);
                    if (NODE_JS)
                    {
                        method = nodeWrap(method, is224);
                    }
                    method.create = function ()
                    {
                        return new _Sha256(is224);
                    };
                    method.update = function (message)
                    {
                        return method.create().update(message);
                    };
                    for (var i = 0; i < OUTPUT_TYPES.length; ++i)
                    {
                        var type = OUTPUT_TYPES[i];
                        method[type] = createOutputMethod(type, is224);
                    }
                    return method;
                };

                var nodeWrap = function (method, is224)
                {
                    var crypto = eval("require('crypto')");
                    var Buffer = eval("require('buffer').Buffer");
                    var algorithm = is224 ? 'sha224' : 'sha256';
                    var nodeMethod = function (message)
                    {
                        if (typeof message === 'string')
                        {
                            return crypto.createHash(algorithm).update(message, 'utf8').digest('hex');
                        } else
                        {
                            if (message === null || message === undefined)
                            {
                                throw new Error(ERROR);
                            } else if (message.constructor === ArrayBuffer)
                            {
                                message = new Uint8Array(message);
                            }
                        }
                        if (Array.isArray(message) || ArrayBuffer.isView(message) ||
                            message.constructor === Buffer)
                        {
                            return crypto.createHash(algorithm).update(new Buffer(message)).digest('hex');
                        } else
                        {
                            return method(message);
                        }
                    };
                    return nodeMethod;
                };

                var createHmacOutputMethod = function (outputType, is224)
                {
                    return function (key, message)
                    {
                        return new HmacSha256(key, is224, true).update(message)[outputType]();
                    };
                };

                var createHmacMethod = function (is224)
                {
                    var method = createHmacOutputMethod('hex', is224);
                    method.create = function (key)
                    {
                        return new HmacSha256(key, is224);
                    };
                    method.update = function (key, message)
                    {
                        return method.create(key).update(message);
                    };
                    for (var i = 0; i < OUTPUT_TYPES.length; ++i)
                    {
                        var type = OUTPUT_TYPES[i];
                        method[type] = createHmacOutputMethod(type, is224);
                    }
                    return method;
                };

                function _Sha256(is224, sharedMemory)
                {
                    if (sharedMemory)
                    {
                        blocks[0] = blocks[16] = blocks[1] = blocks[2] = blocks[3] =
                            blocks[4] = blocks[5] = blocks[6] = blocks[7] =
                            blocks[8] = blocks[9] = blocks[10] = blocks[11] =
                            blocks[12] = blocks[13] = blocks[14] = blocks[15] = 0;
                        this.blocks = blocks;
                    } else
                    {
                        this.blocks = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
                    }

                    if (is224)
                    {
                        this.h0 = 0xc1059ed8;
                        this.h1 = 0x367cd507;
                        this.h2 = 0x3070dd17;
                        this.h3 = 0xf70e5939;
                        this.h4 = 0xffc00b31;
                        this.h5 = 0x68581511;
                        this.h6 = 0x64f98fa7;
                        this.h7 = 0xbefa4fa4;
                    } else
                    { // 256
                        this.h0 = 0x6a09e667;
                        this.h1 = 0xbb67ae85;
                        this.h2 = 0x3c6ef372;
                        this.h3 = 0xa54ff53a;
                        this.h4 = 0x510e527f;
                        this.h5 = 0x9b05688c;
                        this.h6 = 0x1f83d9ab;
                        this.h7 = 0x5be0cd19;
                    }

                    this.block = this.start = this.bytes = this.hBytes = 0;
                    this.finalized = this.hashed = false;
                    this.first = true;
                    this.is224 = is224;
                }

                _Sha256.prototype.update = function (message)
                {
                    if (this.finalized)
                    {
                        return;
                    }
                    var notString, type = typeof message;
                    if (type !== 'string')
                    {
                        if (type === 'object')
                        {
                            if (message === null)
                            {
                                throw new Error(ERROR);
                            } else if (ARRAY_BUFFER && message.constructor === ArrayBuffer)
                            {
                                message = new Uint8Array(message);
                            } else if (!Array.isArray(message))
                            {
                                if (!ARRAY_BUFFER || !ArrayBuffer.isView(message))
                                {
                                    throw new Error(ERROR);
                                }
                            }
                        } else
                        {
                            throw new Error(ERROR);
                        }
                        notString = true;
                    }
                    var code, index = 0, i, length = message.length, blocks = this.blocks;

                    while (index < length)
                    {
                        if (this.hashed)
                        {
                            this.hashed = false;
                            blocks[0] = this.block;
                            blocks[16] = blocks[1] = blocks[2] = blocks[3] =
                                blocks[4] = blocks[5] = blocks[6] = blocks[7] =
                                blocks[8] = blocks[9] = blocks[10] = blocks[11] =
                                blocks[12] = blocks[13] = blocks[14] = blocks[15] = 0;
                        }

                        if (notString)
                        {
                            for (i = this.start; index < length && i < 64; ++index)
                            {
                                blocks[i >> 2] |= message[index] << SHIFT[i++ & 3];
                            }
                        } else
                        {
                            for (i = this.start; index < length && i < 64; ++index)
                            {
                                code = message.charCodeAt(index);
                                if (code < 0x80)
                                {
                                    blocks[i >> 2] |= code << SHIFT[i++ & 3];
                                } else if (code < 0x800)
                                {
                                    blocks[i >> 2] |= (0xc0 | (code >> 6)) << SHIFT[i++ & 3];
                                    blocks[i >> 2] |= (0x80 | (code & 0x3f)) << SHIFT[i++ & 3];
                                } else if (code < 0xd800 || code >= 0xe000)
                                {
                                    blocks[i >> 2] |= (0xe0 | (code >> 12)) << SHIFT[i++ & 3];
                                    blocks[i >> 2] |= (0x80 | ((code >> 6) & 0x3f)) << SHIFT[i++ & 3];
                                    blocks[i >> 2] |= (0x80 | (code & 0x3f)) << SHIFT[i++ & 3];
                                } else
                                {
                                    code = 0x10000 + (((code & 0x3ff) << 10) | (message.charCodeAt(++index) & 0x3ff));
                                    blocks[i >> 2] |= (0xf0 | (code >> 18)) << SHIFT[i++ & 3];
                                    blocks[i >> 2] |= (0x80 | ((code >> 12) & 0x3f)) << SHIFT[i++ & 3];
                                    blocks[i >> 2] |= (0x80 | ((code >> 6) & 0x3f)) << SHIFT[i++ & 3];
                                    blocks[i >> 2] |= (0x80 | (code & 0x3f)) << SHIFT[i++ & 3];
                                }
                            }
                        }

                        this.lastByteIndex = i;
                        this.bytes += i - this.start;
                        if (i >= 64)
                        {
                            this.block = blocks[16];
                            this.start = i - 64;
                            this.hash();
                            this.hashed = true;
                        } else
                        {
                            this.start = i;
                        }
                    }
                    if (this.bytes > 4294967295)
                    {
                        this.hBytes += this.bytes / 4294967296 << 0;
                        this.bytes = this.bytes % 4294967296;
                    }
                    return this;
                };

                _Sha256.prototype.finalize = function ()
                {
                    if (this.finalized)
                    {
                        return;
                    }
                    this.finalized = true;
                    var blocks = this.blocks, i = this.lastByteIndex;
                    blocks[16] = this.block;
                    blocks[i >> 2] |= EXTRA[i & 3];
                    this.block = blocks[16];
                    if (i >= 56)
                    {
                        if (!this.hashed)
                        {
                            this.hash();
                        }
                        blocks[0] = this.block;
                        blocks[16] = blocks[1] = blocks[2] = blocks[3] =
                            blocks[4] = blocks[5] = blocks[6] = blocks[7] =
                            blocks[8] = blocks[9] = blocks[10] = blocks[11] =
                            blocks[12] = blocks[13] = blocks[14] = blocks[15] = 0;
                    }
                    blocks[14] = this.hBytes << 3 | this.bytes >>> 29;
                    blocks[15] = this.bytes << 3;
                    this.hash();
                };

                _Sha256.prototype.hash = function ()
                {
                    var a = this.h0, b = this.h1, c = this.h2, d = this.h3, e = this.h4, f = this.h5, g = this.h6,
                        h = this.h7, blocks = this.blocks, j, s0, s1, maj, t1, t2, ch, ab, da, cd, bc;

                    for (j = 16; j < 64; ++j)
                    {
                        // rightrotate
                        t1 = blocks[j - 15];
                        s0 = ((t1 >>> 7) | (t1 << 25)) ^ ((t1 >>> 18) | (t1 << 14)) ^ (t1 >>> 3);
                        t1 = blocks[j - 2];
                        s1 = ((t1 >>> 17) | (t1 << 15)) ^ ((t1 >>> 19) | (t1 << 13)) ^ (t1 >>> 10);
                        blocks[j] = blocks[j - 16] + s0 + blocks[j - 7] + s1 << 0;
                    }

                    bc = b & c;
                    for (j = 0; j < 64; j += 4)
                    {
                        if (this.first)
                        {
                            if (this.is224)
                            {
                                ab = 300032;
                                t1 = blocks[0] - 1413257819;
                                h = t1 - 150054599 << 0;
                                d = t1 + 24177077 << 0;
                            } else
                            {
                                ab = 704751109;
                                t1 = blocks[0] - 210244248;
                                h = t1 - 1521486534 << 0;
                                d = t1 + 143694565 << 0;
                            }
                            this.first = false;
                        } else
                        {
                            s0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
                            s1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
                            ab = a & b;
                            maj = ab ^ (a & c) ^ bc;
                            ch = (e & f) ^ (~e & g);
                            t1 = h + s1 + ch + K[j] + blocks[j];
                            t2 = s0 + maj;
                            h = d + t1 << 0;
                            d = t1 + t2 << 0;
                        }
                        s0 = ((d >>> 2) | (d << 30)) ^ ((d >>> 13) | (d << 19)) ^ ((d >>> 22) | (d << 10));
                        s1 = ((h >>> 6) | (h << 26)) ^ ((h >>> 11) | (h << 21)) ^ ((h >>> 25) | (h << 7));
                        da = d & a;
                        maj = da ^ (d & b) ^ ab;
                        ch = (h & e) ^ (~h & f);
                        t1 = g + s1 + ch + K[j + 1] + blocks[j + 1];
                        t2 = s0 + maj;
                        g = c + t1 << 0;
                        c = t1 + t2 << 0;
                        s0 = ((c >>> 2) | (c << 30)) ^ ((c >>> 13) | (c << 19)) ^ ((c >>> 22) | (c << 10));
                        s1 = ((g >>> 6) | (g << 26)) ^ ((g >>> 11) | (g << 21)) ^ ((g >>> 25) | (g << 7));
                        cd = c & d;
                        maj = cd ^ (c & a) ^ da;
                        ch = (g & h) ^ (~g & e);
                        t1 = f + s1 + ch + K[j + 2] + blocks[j + 2];
                        t2 = s0 + maj;
                        f = b + t1 << 0;
                        b = t1 + t2 << 0;
                        s0 = ((b >>> 2) | (b << 30)) ^ ((b >>> 13) | (b << 19)) ^ ((b >>> 22) | (b << 10));
                        s1 = ((f >>> 6) | (f << 26)) ^ ((f >>> 11) | (f << 21)) ^ ((f >>> 25) | (f << 7));
                        bc = b & c;
                        maj = bc ^ (b & d) ^ cd;
                        ch = (f & g) ^ (~f & h);
                        t1 = e + s1 + ch + K[j + 3] + blocks[j + 3];
                        t2 = s0 + maj;
                        e = a + t1 << 0;
                        a = t1 + t2 << 0;
                    }

                    this.h0 = this.h0 + a << 0;
                    this.h1 = this.h1 + b << 0;
                    this.h2 = this.h2 + c << 0;
                    this.h3 = this.h3 + d << 0;
                    this.h4 = this.h4 + e << 0;
                    this.h5 = this.h5 + f << 0;
                    this.h6 = this.h6 + g << 0;
                    this.h7 = this.h7 + h << 0;
                };

                _Sha256.prototype.hex = function ()
                {
                    this.finalize();

                    var h0 = this.h0, h1 = this.h1, h2 = this.h2, h3 = this.h3, h4 = this.h4, h5 = this.h5,
                        h6 = this.h6, h7 = this.h7;

                    var hex = HEX_CHARS[(h0 >> 28) & 0x0F] + HEX_CHARS[(h0 >> 24) & 0x0F] +
                        HEX_CHARS[(h0 >> 20) & 0x0F] + HEX_CHARS[(h0 >> 16) & 0x0F] +
                        HEX_CHARS[(h0 >> 12) & 0x0F] + HEX_CHARS[(h0 >> 8) & 0x0F] +
                        HEX_CHARS[(h0 >> 4) & 0x0F] + HEX_CHARS[h0 & 0x0F] +
                        HEX_CHARS[(h1 >> 28) & 0x0F] + HEX_CHARS[(h1 >> 24) & 0x0F] +
                        HEX_CHARS[(h1 >> 20) & 0x0F] + HEX_CHARS[(h1 >> 16) & 0x0F] +
                        HEX_CHARS[(h1 >> 12) & 0x0F] + HEX_CHARS[(h1 >> 8) & 0x0F] +
                        HEX_CHARS[(h1 >> 4) & 0x0F] + HEX_CHARS[h1 & 0x0F] +
                        HEX_CHARS[(h2 >> 28) & 0x0F] + HEX_CHARS[(h2 >> 24) & 0x0F] +
                        HEX_CHARS[(h2 >> 20) & 0x0F] + HEX_CHARS[(h2 >> 16) & 0x0F] +
                        HEX_CHARS[(h2 >> 12) & 0x0F] + HEX_CHARS[(h2 >> 8) & 0x0F] +
                        HEX_CHARS[(h2 >> 4) & 0x0F] + HEX_CHARS[h2 & 0x0F] +
                        HEX_CHARS[(h3 >> 28) & 0x0F] + HEX_CHARS[(h3 >> 24) & 0x0F] +
                        HEX_CHARS[(h3 >> 20) & 0x0F] + HEX_CHARS[(h3 >> 16) & 0x0F] +
                        HEX_CHARS[(h3 >> 12) & 0x0F] + HEX_CHARS[(h3 >> 8) & 0x0F] +
                        HEX_CHARS[(h3 >> 4) & 0x0F] + HEX_CHARS[h3 & 0x0F] +
                        HEX_CHARS[(h4 >> 28) & 0x0F] + HEX_CHARS[(h4 >> 24) & 0x0F] +
                        HEX_CHARS[(h4 >> 20) & 0x0F] + HEX_CHARS[(h4 >> 16) & 0x0F] +
                        HEX_CHARS[(h4 >> 12) & 0x0F] + HEX_CHARS[(h4 >> 8) & 0x0F] +
                        HEX_CHARS[(h4 >> 4) & 0x0F] + HEX_CHARS[h4 & 0x0F] +
                        HEX_CHARS[(h5 >> 28) & 0x0F] + HEX_CHARS[(h5 >> 24) & 0x0F] +
                        HEX_CHARS[(h5 >> 20) & 0x0F] + HEX_CHARS[(h5 >> 16) & 0x0F] +
                        HEX_CHARS[(h5 >> 12) & 0x0F] + HEX_CHARS[(h5 >> 8) & 0x0F] +
                        HEX_CHARS[(h5 >> 4) & 0x0F] + HEX_CHARS[h5 & 0x0F] +
                        HEX_CHARS[(h6 >> 28) & 0x0F] + HEX_CHARS[(h6 >> 24) & 0x0F] +
                        HEX_CHARS[(h6 >> 20) & 0x0F] + HEX_CHARS[(h6 >> 16) & 0x0F] +
                        HEX_CHARS[(h6 >> 12) & 0x0F] + HEX_CHARS[(h6 >> 8) & 0x0F] +
                        HEX_CHARS[(h6 >> 4) & 0x0F] + HEX_CHARS[h6 & 0x0F];
                    if (!this.is224)
                    {
                        hex += HEX_CHARS[(h7 >> 28) & 0x0F] + HEX_CHARS[(h7 >> 24) & 0x0F] +
                            HEX_CHARS[(h7 >> 20) & 0x0F] + HEX_CHARS[(h7 >> 16) & 0x0F] +
                            HEX_CHARS[(h7 >> 12) & 0x0F] + HEX_CHARS[(h7 >> 8) & 0x0F] +
                            HEX_CHARS[(h7 >> 4) & 0x0F] + HEX_CHARS[h7 & 0x0F];
                    }
                    return hex;
                };

                _Sha256.prototype.toString = _Sha256.prototype.hex;

                _Sha256.prototype.digest = function ()
                {
                    this.finalize();

                    var h0 = this.h0, h1 = this.h1, h2 = this.h2, h3 = this.h3, h4 = this.h4, h5 = this.h5,
                        h6 = this.h6, h7 = this.h7;

                    var arr = [
                        (h0 >> 24) & 0xFF, (h0 >> 16) & 0xFF, (h0 >> 8) & 0xFF, h0 & 0xFF,
                        (h1 >> 24) & 0xFF, (h1 >> 16) & 0xFF, (h1 >> 8) & 0xFF, h1 & 0xFF,
                        (h2 >> 24) & 0xFF, (h2 >> 16) & 0xFF, (h2 >> 8) & 0xFF, h2 & 0xFF,
                        (h3 >> 24) & 0xFF, (h3 >> 16) & 0xFF, (h3 >> 8) & 0xFF, h3 & 0xFF,
                        (h4 >> 24) & 0xFF, (h4 >> 16) & 0xFF, (h4 >> 8) & 0xFF, h4 & 0xFF,
                        (h5 >> 24) & 0xFF, (h5 >> 16) & 0xFF, (h5 >> 8) & 0xFF, h5 & 0xFF,
                        (h6 >> 24) & 0xFF, (h6 >> 16) & 0xFF, (h6 >> 8) & 0xFF, h6 & 0xFF
                    ];
                    if (!this.is224)
                    {
                        arr.push((h7 >> 24) & 0xFF, (h7 >> 16) & 0xFF, (h7 >> 8) & 0xFF, h7 & 0xFF);
                    }
                    return arr;
                };

                _Sha256.prototype.array = _Sha256.prototype.digest;

                _Sha256.prototype.arrayBuffer = function ()
                {
                    this.finalize();

                    var buffer = new ArrayBuffer(this.is224 ? 28 : 32);
                    var dataView = new DataView(buffer);
                    dataView.setUint32(0, this.h0);
                    dataView.setUint32(4, this.h1);
                    dataView.setUint32(8, this.h2);
                    dataView.setUint32(12, this.h3);
                    dataView.setUint32(16, this.h4);
                    dataView.setUint32(20, this.h5);
                    dataView.setUint32(24, this.h6);
                    if (!this.is224)
                    {
                        dataView.setUint32(28, this.h7);
                    }
                    return buffer;
                };

                function HmacSha256(key, is224, sharedMemory)
                {
                    var i, type = typeof key;
                    if (type === 'string')
                    {
                        var bytes = [], length = key.length, index = 0, code;
                        for (i = 0; i < length; ++i)
                        {
                            code = key.charCodeAt(i);
                            if (code < 0x80)
                            {
                                bytes[index++] = code;
                            } else if (code < 0x800)
                            {
                                bytes[index++] = (0xc0 | (code >> 6));
                                bytes[index++] = (0x80 | (code & 0x3f));
                            } else if (code < 0xd800 || code >= 0xe000)
                            {
                                bytes[index++] = (0xe0 | (code >> 12));
                                bytes[index++] = (0x80 | ((code >> 6) & 0x3f));
                                bytes[index++] = (0x80 | (code & 0x3f));
                            } else
                            {
                                code = 0x10000 + (((code & 0x3ff) << 10) | (key.charCodeAt(++i) & 0x3ff));
                                bytes[index++] = (0xf0 | (code >> 18));
                                bytes[index++] = (0x80 | ((code >> 12) & 0x3f));
                                bytes[index++] = (0x80 | ((code >> 6) & 0x3f));
                                bytes[index++] = (0x80 | (code & 0x3f));
                            }
                        }
                        key = bytes;
                    } else
                    {
                        if (type === 'object')
                        {
                            if (key === null)
                            {
                                throw new Error(ERROR);
                            } else if (ARRAY_BUFFER && key.constructor === ArrayBuffer)
                            {
                                key = new Uint8Array(key);
                            } else if (!Array.isArray(key))
                            {
                                if (!ARRAY_BUFFER || !ArrayBuffer.isView(key))
                                {
                                    throw new Error(ERROR);
                                }
                            }
                        } else
                        {
                            throw new Error(ERROR);
                        }
                    }

                    if (key.length > 64)
                    {
                        key = (new _Sha256(is224, true)).update(key).array();
                    }

                    var oKeyPad = [], iKeyPad = [];
                    for (i = 0; i < 64; ++i)
                    {
                        var b = key[i] || 0;
                        oKeyPad[i] = 0x5c ^ b;
                        iKeyPad[i] = 0x36 ^ b;
                    }

                    _Sha256.call(this, is224, sharedMemory);

                    this.update(iKeyPad);
                    this.oKeyPad = oKeyPad;
                    this.inner = true;
                    this.sharedMemory = sharedMemory;
                }
                HmacSha256.prototype = new _Sha256();

                HmacSha256.prototype.finalize = function ()
                {
                    _Sha256.prototype.finalize.call(this);
                    if (this.inner)
                    {
                        this.inner = false;
                        var innerHash = this.array();
                        Sha256.call(this, this.is224, this.sharedMemory);
                        this.update(this.oKeyPad);
                        this.update(innerHash);
                        _Sha256.prototype.finalize.call(this);
                    }
                };

                var exports = createMethod();
                exports._sha256 = exports;
                exports.sha224 = createMethod(true);
                exports._sha256.hmac = createHmacMethod();
                exports.sha224.hmac = createHmacMethod(true);

                if (COMMON_JS)
                {
                    module.exports = exports;
                } else
                {
                    root._sha256 = exports._sha256;
                    root.sha224 = exports.sha224;
                    if (AMD)
                    {
                        define(function ()
                        {
                            return exports;
                        });
                    }
                }
            })();


// the rsa keypair, encrypt, decrypt, signing and the dh modpow all run in rsa_keygen.wasm;
// this one file replaced the whole old jsbn/rsa/cryptico vendor stack (dh itself uses the
// native BigInt, which node has too)
// rsa-crypto.js is embedded in template.html along with the other client files, and in the node bundle
// it is the client's rsa layer (lemon_crypto): keypair from a passphrase, encrypt, decrypt and the pkcs#1
// challenge, with the modular exponentiation in rsa_keygen.wasm and everything else native BigInt
// every wire format is byte-identical to the old cryptico stack, so identities and messages stay compatible
// identity-crypto.js, worker-data-processing.js and chat-files.js call it

var rsa_keygen_wasm_exports = null;

/**
 * @brief the rsa keygen wasm, compiled on first use
 *        the module imports nothing and is ~23 KB. synchronous compilation is fine because every
 *        rsa caller lives in the data-processing worker; the main thread only ever uses the
 *        diffie-hellman helper, which needs no wasm
 *
 * @return object the wasm exports
 */
function rsa_crypto__get_rsa_keygen_wasm()
{
    if (rsa_keygen_wasm_exports == null)
    {
        var rsa_keygen_webassembly_base64 = 'AGFzbQEAAAABIwdgAAF/YAF/AX9gAn9/AX9gAn9/AGADf39/AGAAAXxgAX8AAw8OAAECAwIEAwUBAAAGBAQEBQFwAQEBBQMBAAQGCAF/AUGAgAQLB78BCAZtZW1vcnkCAA9fX3N0YWNrX3BvaW50ZXIDABtyc2Ffa2V5Z2VuX19nZXRfc2VlZF9idWZmZXIAABZyc2Ffa2V5Z2VuX19nZXRfcmVzdWx0AAEUcnNhX2tleWdlbl9fZ2VuZXJhdGUAAh1yc2Ffa2V5Z2VuX19nZXRfbW9kcG93X2J1ZmZlcgAIHXJzYV9rZXlnZW5fX2dldF9tb2Rwb3dfcmVzdWx0AAkScnNhX2tleWdlbl9fbW9kcG93AAoK5cwBDggAQcCFhIAACxcAQQAgACAAQQdLG0G0EGxBwIeEgABqC6ZZBAh/An4HfwJ+I4CAgIAAQYAQayICJICAgIAAQQAhAwJAIABB/79/akH/QUkNACABQf99akGAfkkNACAAQQF1IQRBACEFIAJBAEGACPwLACACQYAIakEAQYAI/AsAIAJBgAhqIQYgAiEDQQAhBwNAAkAgAygCAEEBRw0AIAYoAgBBE2wgBXMhBQsgA0EBNgIAIAYgB0HAhYSAAGotAAAgBWpB/wFxNgIAIANBBGohAyAGQQRqIQYgASAHQQFqIgdHDQALQQchAwNAIANBgNGHgABqIAM6AAAgA0H50IeAAGogA0F5aiIGOgAAIANB/9CHgABqIAZBBmo6AAAgA0H+0IeAAGogBkEFajoAACADQf3Qh4AAaiAGQQRqOgAAIANB/NCHgABqIAZBA2o6AAAgA0H70IeAAGogBkECajoAACADQfrQh4AAaiAGQQFqOgAAIANB/wFHIQYgA0EIaiEDIAYNAAtBACEHQQAhA0EAIQYDQCAGQYDRh4AAaiIFIAcgBS0AACIFaiACQYAIaiADQQJ0aigCAGpB/wFxIgdBgNGHgABqIggtAAA6AAAgCCAFOgAAQQAgA0EBaiIDIAMgAUYbIQMgBkEBaiIGQYACRw0AC0EAIQZBgAIhB0EAIQMDQCADQQFqQf8BcSIBIAEtAIDRh4AAIgEgBmoiBkH/AXEiBS0AgNGHgAA6AIDRh4AAIAUgAToAgNGHgAAgA0ECakH/AXEiAyADLQCA0YeAACIFIAZqIgZB/wFxIgEtAIDRh4AAOgCA0YeAACABIAU6AIDRh4AAIAdBfmoiBw0AC0EAIAE2AoTTh4AAQQAgAzYCgNOHgAAgACAEayEJAkADQEHgioWAACAJEIOAgIAAQQAoAuCKhYAAIgdBAnQhAAJAAkACQAJAIAdBAUgNAAJAIABFDQBBrKuFgABB5IqFgAAgAPwKAAALIAdBiARJDQFBACAHNgKoq4WAAEEAQQA1AqyrhYAAQn98Igo+AqyrhYAADAILAkBBoBAgAGsiA0UNACAAQayrhYAAakEAIAP8CwALQQAgBzYCqKuFgAAMAwsCQEGgECAAayIDRQ0AIABBrKuFgABqQQAgA/wLAAtBACAHNgKoq4WAAEEAQQA1AqyrhYAAQn98Igo+AqyrhYAAIAdBAUYNAQsgB0F/aiIBQQNxIQZBASEDAkAgB0F+akEDSQ0AIAFBfHEhCEEAIQFBvKuFgAAhAwNAIANBdGoiBSAKQj+HIAU1AgB8Igo+AgAgA0F4aiIFIApCP4cgBTUCAHwiCj4CACADQXxqIgUgCkI/hyAFNQIAfCIKPgIAIAMgCkI/hyADNQIAfCIKPgIAIANBEGohAyAIIAFBBGoiAUcNAAsgBkUNASABQQFqIQMLIANBAnRBrKuFgABqIQMDQCADIApCP4cgAzUCAHwiCj4CACADQQRqIQMgBkF/aiIGDQALCyAAQairhYAAaiEDIAdBAWoiCCEGAkADQAJAIAMoAgBFDQBCACEKA0AgCkIghiADNQIAhEIDgiEKIANBfGohAyAGQX9qIgZBAUsNAAsgClANA0EALQDkioWAAEEBcUUNAyAAQeCKhYAAaiEBQQEhBwNAIAdBAnQ1AoCAhIAAIQtCACEKIAEhAyAIIQYDQCAKQiCGIAM1AgCEIAuCIQogA0F8aiEDIAZBf2oiBkEBSw0ACyAKUA0EIAdBAWoiB0GoAUcNAAtB4IqFgABBChCEgICAAEUNAwNAQYSbhYAAIAQQg4CAgABBACgChJuFgAAiB0ECdCEAAkACQAJAAkAgB0EBSA0AAkAgAEUNAEHQu4WAAEGIm4WAACAA/AoAAAsgB0GIBEkNAUEAIAc2Asy7hYAAQQBBADUC0LuFgABCf3wiCj4C0LuFgAAMAgsCQEGgECAAayIDRQ0AIABB0LuFgABqQQAgA/wLAAtBACAHNgLMu4WAAAwDCwJAQaAQIABrIgNFDQAgAEHQu4WAAGpBACAD/AsAC0EAIAc2Asy7hYAAQQBBADUC0LuFgABCf3wiCj4C0LuFgAAgB0EBRg0BCyAHQX9qIgFBA3EhBkEBIQMCQCAHQX5qQQNJDQAgAUF8cSEIQQAhAUHgu4WAACEDA0AgA0F0aiIFIApCP4cgBTUCAHwiCj4CACADQXhqIgUgCkI/hyAFNQIAfCIKPgIAIANBfGoiBSAKQj+HIAU1AgB8Igo+AgAgAyAKQj+HIAM1AgB8Igo+AgAgA0EQaiEDIAggAUEEaiIBRw0ACyAGRQ0BIAFBAWohAwsgA0ECdEHQu4WAAGohAwNAIAMgCkI/hyADNQIAfCIKPgIAIANBBGohAyAGQX9qIgYNAAsLIABBzLuFgABqIQMgB0EBaiIIIQYDQAJAIAMoAgBFDQBCACEKA0AgCkIghiADNQIAhEIDgiEKIANBfGohAyAGQX9qIgZBAUsNAAsgClANAkEALQCIm4WAAEEBcUUNAiAAQYSbhYAAaiEBQQEhBwNAIAdBAnQ1AoCAhIAAIQtCACEKIAEhAyAIIQYDQCAKQiCGIAM1AgCEIAuCIQogA0F8aiEDIAZBf2oiBkEBSw0ACyAKUA0DIAdBAWoiB0GoAUcNAAtBhJuFgABBChCEgICAAEUNAgJAAkACQEEAKALgioWAACIFQQAoAoSbhYAAIgxHDQAgBUEBaiEGIAVBAnQhAwNAIAZBf2oiBkEBSA0CIANBhJuFgABqIQcgA0HgioWAAGohASADQXxqIQMgASgCACIBIAcoAgAiB0YNAAsgASAHTQ0BIAUhDSAFIQwMAgsgBSAMTA0AIAwhDSAFIQwMAQsgBUECdCEDAkACQCAFQQFIIgcNAAJAIANFDQBBzK2GgABB5IqFgAAgA/wKAAALIAVBhwRLDQELQaAQIANrIgZFDQAgA0HMrYaAAGpBACAG/AsAC0EAIAU2AsithoAAIAxBAnQhBgJAAkAgDEEBSA0AAkAgBkUNAEHkioWAAEGIm4WAACAG/AoAAAsgDEGHBEsNAQtBoBAgBmsiAUUNACAGQeSKhYAAakEAIAH8CwALQQAgDDYC4IqFgAACQAJAIAcNAAJAIANFDQBBiJuFgABBzK2GgAAgA/wKAAALIAVBhwRLDQELQaAQIANrIgZFDQAgA0GIm4WAAGpBACAG/AsAC0EAIAU2AoSbhYAAIAUhDQsgDEECdCEIAkACQAJAAkACQCAMQQFIDQACQCAIRQ0AQayrhYAAQeSKhYAAIAj8CgAACyAMQYgESQ0BQQAgDDYCqKuFgABBAEEANQKsq4WAAEJ/fCIKPgKsq4WAAAwCCwJAQaAQIAhrIgNFDQAgCEGsq4WAAGpBACAD/AsAC0EAIAw2AqirhYAAIAwhDgwDCwJAQaAQIAhrIgNFDQAgCEGsq4WAAGpBACAD/AsAC0EAIAw2AqirhYAAQQBBADUCrKuFgABCf3wiCj4CrKuFgAAgDEEBRg0BCyAMQX9qIgdBA3EhBkEBIQMCQCAMQX5qQQNJDQAgB0F8cSEFQQAhB0G8q4WAACEDA0AgA0F0aiIBIApCP4cgATUCAHwiCj4CACADQXhqIgEgCkI/hyABNQIAfCIKPgIAIANBfGoiASAKQj+HIAE1AgB8Igo+AgAgAyAKQj+HIAM1AgB8Igo+AgAgA0EQaiEDIAUgB0EEaiIHRw0ACyAGRQ0BIAdBAWohAwsgA0ECdEGsq4WAAGohAwNAIAMgCkI/hyADNQIAfCIKPgIAIANBBGohAyAGQX9qIgYNAAsLIAhBqKuFgABqIQMgDCEGA0ACQCADKAIARQ0AIAYhDgwCC0EAIAZBf2oiDjYCqKuFgAAgA0F8aiEDIAZBAUohByAOIQYgBw0ACwsgDUECdCEIAkACQAJAAkACQCANQQFIDQACQCAIRQ0AQdC7hYAAQYibhYAAIAj8CgAACyANQYgESQ0BQQAgDTYCzLuFgABBAEEANQLQu4WAAEJ/fCIKPgLQu4WAAAwCCwJAQaAQIAhrIgNFDQAgCEHQu4WAAGpBACAD/AsAC0EAIA02Asy7hYAAIA0hAAwDCwJAQaAQIAhrIgNFDQAgCEHQu4WAAGpBACAD/AsAC0EAIA02Asy7hYAAQQBBADUC0LuFgABCf3wiCj4C0LuFgAAgDUEBRg0BCyANQX9qIgdBA3EhBkEBIQMCQCANQX5qQQNJDQAgB0F8cSEFQQAhB0Hgu4WAACEDA0AgA0F0aiIBIApCP4cgATUCAHwiCj4CACADQXhqIgEgCkI/hyABNQIAfCIKPgIAIANBfGoiASAKQj+HIAE1AgB8Igo+AgAgAyAKQj+HIAM1AgB8Igo+AgAgA0EQaiEDIAUgB0EEaiIHRw0ACyAGRQ0BIAdBAWohAwsgA0ECdEHQu4WAAGohAwNAIAMgCkI/hyADNQIAfCIKPgIAIANBBGohAyAGQX9qIgYNAAsLIAhBzLuFgABqIQMgDSEGA0ACQCADKAIARQ0AIAYhAAwCC0EAIAZBf2oiADYCzLuFgAAgA0F8aiEDIAZBAUohByAAIQYgBw0ACwtB8MuFgABBAEGkEPwLAAJAIA5BAUgNACAAQf7///8HcSEIIABBAXEhDyAAQQJ0QfTLhYAAaiEQQQAhEUHwy4WAACEFA0ACQCAAQQFIDQAgEUECdCISNQKsq4WAACELAkACQAJAIABBAUcNAEEAIQZCACEKDAELQgAhCkEAIQNBACEGA0AgBSADaiIHQQRqIgEgCiABNQIAfCADQdC7hYAAajUCACALfnwiCj4CACAHQQhqIgcgCkIgiCAHNQIAfCADQdS7hYAAajUCACALfnwiCj4CACAKQiCIIQogA0EIaiEDIAggBkECaiIGRw0ACyAPRQ0BCyASQfTLhYAAaiAGQQJ0IgNqIgYgCiAGNQIAfCADNQLQu4WAACALfnwiCj4CACAKQiCIIQoLIBAhAyAKUA0AA0AgAyAKIAM1AgB8Igo+AgAgA0EEaiEDIApCIIgiCkIAUg0ACwsgEEEEaiEQIAVBBGohBSARQQFqIhEgDkcNAAsLQQAgACAOaiIGNgLwy4WAACAGQQFIDQYgBkECdCIHQfDLhYAAaiEDIAZBAWohBgJAA0AgB0Hwy4WAAGooAgANAUEAIAZBfmo2AvDLhYAAIANBfGohAyAHQXxqIQcgBkF/aiIGQQFMDQgMAAsLQgAhCiAGIQEDQCAKQiCGIAM1AgCEQgOCIQogA0F8aiEDIAFBf2oiAUEBSw0ACyAKUA0GQZTchYAAIQBBlNyFgABBAEGkEPwLAAJAIAxBAUgNACANQQEgDUEBShsiA0H+////B3EhBCADQQFxIQkgA0ECdEGY3IWAAGohEEEAIREDQAJAIA1BAUgNACARQQJ0Ig41AuSKhYAAIQsCQAJAAkAgDUEBRw0AQQAhAUIAIQoMAQtCACEKQQAhA0EAIQEDQCAAIANqIgVBBGoiCCAKIAg1AgB8IANBiJuFgABqNQIAIAt+fCIKPgIAIAVBCGoiBSAKQiCIIAU1AgB8IANBjJuFgABqNQIAIAt+fCIKPgIAIApCIIghCiADQQhqIQMgBCABQQJqIgFHDQALIAlFDQELIA5BmNyFgABqIAFBAnQiA2oiASAKIAE1AgB8IAM1AoibhYAAIAt+fCIKPgIAIApCIIghCgsgECEDIApQDQADQCADIAogAzUCAHwiCj4CACADQQRqIQMgCkIgiCIKQgBSDQALCyAQQQRqIRAgAEEEaiEAIBFBAWoiESAMRw0ACwtBACANIAxqIgE2ApTchYAAAkAgAUEBSA0AIAFBAnRBlNyFgABqIQMgAUEBaiEBA0AgAygCAA0BQQAgAUF+ajYClNyFgAAgA0F8aiEDIAFBf2oiAUEBSg0ACwsgBkF/aiEBIAdB8MuFgABqIQNCACEKIAYhBQNAIApCIIYgAzUCAIRCA4IhCiADQXxqIQMgBUF/aiIFQQFLDQALAkAgAUEBIAFBAUobQQJ0IgNFDQBBvOyFgABB9MuFgAAgA/wKAAALAkACQAJAIAFBhwRKDQACQEGgECAHayIDRQ0AIAdBvOyFgABqQQAgA/wLAAtBACABNgK47IWAACAKQgFRDQEMAgtBACABNgK47IWAACAKQgFSDQELQgAhE0EAIQNCACEUAkACQCAGQQJGDQAgAUEBcSEEIAFBfnEhAEIAIRRBACEFQQAhAwNAQgAhCkIAIQsCQCAFIgggAU4NACADQfTLhYAAajUCACELIANBvOyFgABqNQIAIQoLIANBvOyFgABqIAogFHwgC3wiCj4CACAKQiCIIRRCACEKQgAhCwJAIAhBAWoiBSABTg0AIANB+MuFgABqNQIAIQsgA0HA7IWAAGo1AgAhCgsgA0HA7IWAAGogCiAUfCALfCIKPgIAIANBCGohAyAKQiCIIRQgBUEBaiIFIABHDQALIARFDQEgCEECaiEDC0IAIQoCQCADIAFODQAgA0ECdCIFNQL0y4WAACEKIAU1ArzshYAAIRMLIANBAnQgEyAUfCAKfCIKPgK87IWAACAKQiCIIRQLAkAgFFANACAHQbzshYAAakEBNgIAIAYhAQtBACABNgK47IWAAAtCASEKQX8hBkG87IWAACEDA0AgAyAKIAM1AgB8Igo+AgAgA0EEaiEDIAZBAWohBiAKQiCIIgpCAFINAAsCQCABIAZKDQBBACAGQQFqIgE2ArjshYAACyABQQJ0QbTshYAAaiEDAkACQANAIANBBGooAgANAUEAIAFBf2oiBjYCuOyFgAAgA0F8aiEDIAFBAUohByAGIQEgBw0ADAILCyABQQFIDQBCACEKAkACQCABQQFGDQAgAUEBcSEIIAFB/v///wdxIQVBACEGQgAhCgNAIANBBGoiByAKQiCGIAc1AgCEIgpCA4AiCz4CACADIAogC0IDfn1CIIYgAzUCAIQiCkIDgCILPgIAIAogC0IDfn0hCiADQXhqIQMgBSAGQQJqIgZHDQALIAhFDQEgASAGayEBCyABQQJ0QbjshYAAaiIDIApCIIYgAzUCAIRCA4A+AgALQQAoArjshYAAIgNBAUgNACADQQFqIQYgA0ECdEG47IWAAGohAwNAIAMoAgANAUEAIAZBfmo2ArjshYAAIANBfGohAyAGQX9qIgZBAUoNAAsLQdz8hYAAQbjshYAAQairhYAAEIWAgIAAQYCNhoAAQbjshYAAQcy7hYAAEIWAgIAAQciIiYAAQYSbhYAAQeCKhYAAEIWAgIAAQQAoAuCKhYAAIhFBAnQhAwJAAkAgEUEBSA0AAkAgA0UNAEHwmImAAEHkioWAACAD/AoAAAsgEUGHBEsNAQtBoBAgA2siBkUNACADQfCYiYAAakEAIAb8CwALQQAgETYC7JiJgABBmKmJgABBAEGcEPwLAEEAQoGAgIAQNwKQqYmAAEG0uYmAACEGQbS5iYAAQQBBpBD8CwBBACgCzIiJgAAhAwJAQQAoAsiIiYAAIgFBAUcNACADQQFGDQYLQQAoAvCYiYAAIQwCQCARQQFHDQAgDEEBRg0IC0EBIQRBACEQIBEhBwNAIAFBAEohEgJAIAFBAUgNACAEIQUgA0EBcQ0AA0BBACEJQQAhBgJAAkAgAUEBRg0AIAFBAXEhEiABQX5xIQ5BACEGQdCIiYAAIQMDQEEAIQgCQCAGQQFqIAFODQAgAygCAEEfdCEICyADQXxqIgAgACgCAEEBdiAIcjYCAEEAIQgCQCAGQQJqIgYgAU4NACADQQRqKAIAQR90IQgLIAMgAygCAEEBdiAIcjYCACADQQhqIQMgDiAGRw0ACyASRQ0BCwJAIAZBAWoiAyABTg0AIANBAnQoAsyIiYAAQR90IQkLIAZBAnQiAyADKALMiImAAEEBdiAJcjYCzIiJgAALAkAgAUECdCIDQciIiYAAaigCAA0AIANBxIiJgABqIQYDQEEAIAEiA0F/aiIBNgLIiImAACADQQFMDQEgA0F/aiEBIAYoAgAhCCAGQXxqIQYgCEUNAAsgA0F/aiEBCwJAAkBBAC0AlKmJgABBAXENACAFQQFIDQFBACEJQQAhBgJAAkAgBUEBRg0AIAVBAXEhEiAFQf7///8HcSEOQQAhBkGYqYmAACEDA0BBACEIAkAgBkEBaiAFTg0AIAMoAgBBH3QhCAsgA0F8aiIAIAAoAgBBAXYgCHI2AgBBACEIAkAgBkECaiIGIAVODQAgA0EEaigCAEEfdCEICyADIAMoAgBBAXYgCHI2AgAgA0EIaiEDIA4gBkcNAAsgEkUNAQsCQCAGQQFqIgMgBU4NACADQQJ0KAKUqYmAAEEfdCEJCyAGQQJ0IgMgAygClKmJgABBAXYgCXI2ApSpiYAACyAFQQJ0IgNBkKmJgABqKAIADQEgA0GMqYmAAGohAwNAQQAhBEEAIAVBf2oiBjYCkKmJgAAgBUEBSiEIQQAhBSAIRQ0CIAMoAgAhCCADQXxqIQMgBiEFIAYhBCAIDQIMAAsLAkAgBSARIAUgEUobIgRBAUgNAEIAIRRBACEDQQQhBgNAQgAhCgJAIAMgBU4NACAGQZCpiYAAajUCACEKC0IAIQsCQCADIBFODQAgBkHgioWAAGo1AgAhCwsgBkGQqYmAAGogCiAUfCALfCIKPgIAIAZBBGohBiAKQiCIIRQgBCADQQFqIgNHDQALAkACQCAUUEUNACAEIQUMAQsgBEECdEEBNgKUqYmAACAEQQFqIQULQQAhDkEAIAU2ApCpiYAAQQAhBgJAAkAgBUEBRg0AIAVBAXEhCSAFQX5xIQRBACEGQZipiYAAIQMDQEEAIQgCQCAGQQFqIAVODQAgAygCAEEfdCEICyADQXxqIgAgACgCAEEBdiAIcjYCAEEAIQgCQCAGQQJqIgYgBU4NACADQQRqKAIAQR90IQgLIAMgAygCAEEBdiAIcjYCACADQQhqIQMgBCAGRw0ACyAJRQ0BCwJAIAZBAWoiAyAFTg0AIANBAnQoApSpiYAAQR90IQ4LIAZBAnQiAyADKAKUqYmAAEEBdiAOcjYClKmJgAALIAVBAnRBkKmJgABqIQMDQAJAIAMoAgBFDQAgBSEEDAMLQQAgBUF/aiIENgKQqYmAACADQXxqIQMgBUEBSiEGIAQhBSAGDQAMAgsLQQAgBDYCkKmJgAAgBCEFCyABQQBKIRIgAUEBSA0BQQAoAsyIiYAAQQFxRQ0ACwsgB0EASiEGAkAgB0EBSA0AIBAhBSAMQQFxDQADQEEAIQlBACEGAkACQCAHQQFGDQAgB0EBcSEMIAdBfnEhDkEAIQZB9JiJgAAhAwNAQQAhCAJAIAZBAWogB04NACADKAIAQR90IQgLIANBfGoiACAAKAIAQQF2IAhyNgIAQQAhCAJAIAZBAmoiBiAHTg0AIANBBGooAgBBH3QhCAsgAyADKAIAQQF2IAhyNgIAIANBCGohAyAOIAZHDQALIAxFDQELAkAgBkEBaiIDIAdODQAgA0ECdCgC8JiJgABBH3QhCQsgBkECdCIDIAMoAvCYiYAAQQF2IAlyNgLwmImAAAsCQCAHQQJ0IgNB7JiJgABqKAIADQAgA0HomImAAGohBgNAQQAgByIDQX9qIgc2AuyYiYAAIANBAUwNASADQX9qIQcgBigCACEIIAZBfGohBiAIRQ0ACyADQX9qIQcLAkACQEEALQC4uYmAAEEBcQ0AIAVBAUgNAUEAIQlBACEGAkACQCAFQQFGDQAgBUEBcSEMIAVB/v///wdxIQ5BACEGQby5iYAAIQMDQEEAIQgCQCAGQQFqIAVODQAgAygCAEEfdCEICyADQXxqIgAgACgCAEEBdiAIcjYCAEEAIQgCQCAGQQJqIgYgBU4NACADQQRqKAIAQR90IQgLIAMgAygCAEEBdiAIcjYCACADQQhqIQMgDiAGRw0ACyAMRQ0BCwJAIAZBAWoiAyAFTg0AIANBAnQoAri5iYAAQR90IQkLIAZBAnQiAyADKAK4uYmAAEEBdiAJcjYCuLmJgAALIAVBAnQiA0G0uYmAAGooAgANASADQbC5iYAAaiEDA0BBACEQQQAgBUF/aiIGNgK0uYmAACAFQQFKIQhBACEFIAhFDQIgAygCACEIIANBfGohAyAGIQUgBiEQIAgNAgwACwsCQCAFIBEgBSARShsiEEEBSA0AQgAhFEEAIQNBBCEGA0BCACEKAkAgAyAFTg0AIAZBtLmJgABqNQIAIQoLQgAhCwJAIAMgEU4NACAGQeCKhYAAajUCACELCyAGQbS5iYAAaiAKIBR8IAt8Igo+AgAgBkEEaiEGIApCIIghFCAQIANBAWoiA0cNAAsCQAJAIBRQRQ0AIBAhBQwBCyAQQQJ0QQE2Ari5iYAAIBBBAWohBQtBACEOQQAgBTYCtLmJgABBACEGAkACQCAFQQFGDQAgBUEBcSEJIAVBfnEhEEEAIQZBvLmJgAAhAwNAQQAhCAJAIAZBAWogBU4NACADKAIAQR90IQgLIANBfGoiACAAKAIAQQF2IAhyNgIAQQAhCAJAIAZBAmoiBiAFTg0AIANBBGooAgBBH3QhCAsgAyADKAIAQQF2IAhyNgIAIANBCGohAyAQIAZHDQALIAlFDQELAkAgBkEBaiIDIAVODQAgA0ECdCgCuLmJgABBH3QhDgsgBkECdCIDIAMoAri5iYAAQQF2IA5yNgK4uYmAAAsgBUECdEG0uYmAAGohAwNAAkAgAygCAEUNACAFIRAMAwtBACAFQX9qIhA2ArS5iYAAIANBfGohAyAFQQFKIQYgECEFIAYNAAwCCwtBACAQNgK0uYmAACAQIQULIAdBAEohBiAHQQFIDQFBACgC8JiJgABBAXFFDQALCwJAAkACQAJAIAEgB0cNACABQQFqIQUgAUECdCEDA0AgBUF/aiIFQQFIDQMgA0HsmImAAGohCCADQciIiYAAaiEAIANBfGohAyAAKAIAIgAgCCgCACIIRg0ACyAAIAhNDQEMAgsgASAHSg0BCwJAIAZFDQBCACETQQAhA0IAIQsCQAJAIAdBAUYNACAHQQFxIQ4gB0F+cSEAQgAhC0EAIQZBACEDA0AgA0HwmImAAGoiCDUCACEUQgAhCgJAIAYiBSABTg0AIANBzIiJgABqNQIAIQoLIAggFCAKfSALfCIKPgIAIApCP4chCyADQfSYiYAAaiIGNQIAIRRCACEKAkAgBUEBaiIIIAFODQAgA0HQiImAAGo1AgAhCgsgBiAUIAp9IAt8Igo+AgAgA0EIaiEDIApCP4chCyAIQQFqIgYgAEcNAAsgDkUNASAFQQJqIQMLIANBAnQiBkHwmImAAGohBSAGNQLwmImAACEKAkAgAyABTg0AIAY1AsyIiYAAIRMLIAUgCiATfSALfD4CAAsgB0ECdCIDQeyYiYAAaigCAA0AIANB6JiJgABqIQYDQEEAIAciA0F/aiIHNgLsmImAACADQQFMDQEgA0F/aiEHIAYoAgAhBSAGQXxqIQYgBUUNAAsgA0F/aiEHCwJAAkACQCAQIARHDQAgBEEBaiEGIARBAnQhAwNAIAZBf2oiBkEBSA0DIANBkKmJgABqIQUgA0G0uYmAAGohCCADQXxqIQMgCCgCACIIIAUoAgAiBUYNAAsgCCAFTQ0BDAILIBAgBEoNAQsCQCAQIBEgECARShsiBUEBSA0AQgAhFEEAIQNBBCEGA0BCACEKAkAgAyAQTg0AIAZBtLmJgABqNQIAIQoLQgAhCwJAIAMgEU4NACAGQeCKhYAAajUCACELCyAGQbS5iYAAaiAKIBR8IAt8Igo+AgAgBkEEaiEGIApCIIghFCAFIANBAWoiA0cNAAsCQCAUUA0AIAVBAnRBATYCuLmJgAAgBUEBaiEFC0EAIQNBACAFNgK0uYmAAEIAIRNCACELAkACQCAFQQFGDQAgBUEBcSEOIAVBfnEhEEIAIQtBACEGQQAhAwNAIANBuLmJgABqIgA1AgAhFEIAIQoCQCAGIgggBE4NACADQZSpiYAAajUCACEKCyAAIBQgCn0gC3wiCj4CACAKQj+HIQsgA0G8uYmAAGoiBjUCACEUQgAhCgJAIAhBAWoiACAETg0AIANBmKmJgABqNQIAIQoLIAYgFCAKfSALfCIKPgIAIANBCGohAyAKQj+HIQsgAEEBaiIGIBBHDQALIA5FDQEgCEECaiEDCyADQQJ0IgZBuLmJgABqIQggBjUCuLmJgAAhCgJAIAMgBE4NACAGNQKUqYmAACETCyAIIAogE30gC3w+AgALIAVBAnRBtLmJgABqIQMDQAJAIAMoAgBFDQAgBSEQDAULQQAgBUF/aiIQNgK0uYmAACADQXxqIQMgBUEBSiEGIBAhBSAGDQAMBAsLQQAgBTYCtLmJgAAgBSEQDAILIBBBAUgNAUIAIRNBACEDQgAhCwJAAkAgEEEBRg0AIBBBAXEhDiAQQf7///8HcSEAQgAhC0EAIQZBACEDA0AgA0G4uYmAAGoiCDUCACEUQgAhCgJAIAYiBSAETg0AIANBlKmJgABqNQIAIQoLIAggFCAKfSALfCIKPgIAIApCP4chCyADQby5iYAAaiIGNQIAIRRCACEKAkAgBUEBaiIIIARODQAgA0GYqYmAAGo1AgAhCgsgBiAUIAp9IAt8Igo+AgAgA0EIaiEDIApCP4chCyAIQQFqIgYgAEcNAAsgDkUNASAFQQJqIQMLIANBAnQiBkG4uYmAAGohBSAGNQK4uYmAACEKAkAgAyAETg0AIAY1ApSpiYAAIRMLIAUgCiATfSALfD4CAAsgEEECdEG0uYmAAGohAwNAIAMoAgANAkEAIBBBf2oiBjYCtLmJgAAgA0F8aiEDIBBBAUohBSAGIRAgBQ0AC0EAIRAMAQsCQCASRQ0AQgAhE0EAIQNCACELAkACQCABQQFGDQAgAUEBcSEOIAFBfnEhAEIAIQtBACEGQQAhAwNAIANBzIiJgABqIgg1AgAhFEIAIQoCQCAGIgUgB04NACADQfCYiYAAajUCACEKCyAIIBQgCn0gC3wiCj4CACAKQj+HIQsgA0HQiImAAGoiBjUCACEUQgAhCgJAIAVBAWoiCCAHTg0AIANB9JiJgABqNQIAIQoLIAYgFCAKfSALfCIKPgIAIANBCGohAyAKQj+HIQsgCEEBaiIGIABHDQALIA5FDQEgBUECaiEDCyADQQJ0IgZBzIiJgABqIQUgBjUCzIiJgAAhCgJAIAMgB04NACAGNQLwmImAACETCyAFIAogE30gC3w+AgALIAFBAnQiA0HIiImAAGooAgANACADQcSIiYAAaiEGA0BBACABIgNBf2oiATYCyIiJgAAgA0EBTA0BIANBf2ohASAGKAIAIQUgBkF8aiEGIAVFDQALIANBf2ohAQsCQAJAAkAgBCAQRw0AIARBAWohBiAEQQJ0IQMDQCAGQX9qIgZBAUgNAyADQbS5iYAAaiEFIANBkKmJgABqIQggA0F8aiEDIAgoAgAiCCAFKAIAIgVGDQALIAggBU0NAQwCCyAEIBBKDQELAkAgBCARIAQgEUobIgVBAUgNAEIAIRRBACEDQQQhBgNAQgAhCgJAIAMgBE4NACAGQZCpiYAAajUCACEKC0IAIQsCQCADIBFODQAgBkHgioWAAGo1AgAhCwsgBkGQqYmAAGogCiAUfCALfCIKPgIAIAZBBGohBiAKQiCIIRQgBSADQQFqIgNHDQALAkAgFFANACAFQQJ0QQE2ApSpiYAAIAVBAWohBQtBACEDQQAgBTYCkKmJgABCACETQgAhCwJAAkAgBUEBRg0AIAVBAXEhDiAFQX5xIQRCACELQQAhBkEAIQMDQCADQZSpiYAAaiIANQIAIRRCACEKAkAgBiIIIBBODQAgA0G4uYmAAGo1AgAhCgsgACAUIAp9IAt8Igo+AgAgCkI/hyELIANBmKmJgABqIgY1AgAhFEIAIQoCQCAIQQFqIgAgEE4NACADQby5iYAAajUCACEKCyAGIBQgCn0gC3wiCj4CACADQQhqIQMgCkI/hyELIABBAWoiBiAERw0ACyAORQ0BIAhBAmohAwsgA0ECdCIGQZSpiYAAaiEIIAY1ApSpiYAAIQoCQCADIBBODQAgBjUCuLmJgAAhEwsgCCAKIBN9IAt8PgIACyAFQQJ0QZCpiYAAaiEDA0ACQCADKAIARQ0AIAUhBAwEC0EAIAVBf2oiBDYCkKmJgAAgA0F8aiEDIAVBAUohBiAEIQUgBg0ADAMLC0EAIAU2ApCpiYAAIAUhBAwBCyAEQQFIDQBCACETQQAhA0IAIQsCQAJAIARBAUYNACAEQQFxIQ4gBEH+////B3EhAEIAIQtBACEGQQAhAwNAIANBlKmJgABqIgg1AgAhFEIAIQoCQCAGIgUgEE4NACADQbi5iYAAajUCACEKCyAIIBQgCn0gC3wiCj4CACAKQj+HIQsgA0GYqYmAAGoiBjUCACEUQgAhCgJAIAVBAWoiCCAQTg0AIANBvLmJgABqNQIAIQoLIAYgFCAKfSALfCIKPgIAIANBCGohAyAKQj+HIQsgCEEBaiIGIABHDQALIA5FDQEgBUECaiEDCyADQQJ0IgZBlKmJgABqIQUgBjUClKmJgAAhCgJAIAMgEE4NACAGNQK4uYmAACETCyAFIAogE30gC3w+AgALIARBAnRBkKmJgABqIQMDQCADKAIADQFBACAEQX9qIgY2ApCpiYAAIANBfGohAyAEQQFKIQUgBiEEIAUNAAtBACEEC0EAKALMiImAACEDAkAgAUEBRw0AIANBAUYNBwtBACgC8JiJgAAhDCAHQQFHDQAgDEEBRw0AC0G0uYmAACEGDAcLQQAgB0F/aiIBNgLMu4WAACAGQX9qIQYgA0F8aiEDIAdBAUohBSABIQcgBUUNAQwACwsLQQAgB0F/aiIBNgKoq4WAACAGQX9qIQYgA0F8aiEDIAdBAUohBSABIQcgBUUNAgwACwsLQZCpiYAAIQYLQaSdhoAAIAZB4IqFgAAQhYCAgABBlNyFgABBwIeEgAAQhoCAgABBAEEzOwH0l4SAAEG47IWAAEGoqISAABCGgICAAEHgioWAAEHcuISAABCGgICAAEGEm4WAAEGQyYSAABCGgICAAEHc/IWAAEHE2YSAABCGgICAAEGAjYaAAEH46YSAABCGgICAAEGknYaAAEGs+oSAABCGgICAAEEBIQMLIAJBgBBqJICAgIAAIAML/RAECH8Bfgd/A34gAUEHcSECAkACQCABQQN1IgNBAEgNAEF/IQQDQCAEQfHth4AAahCHgICAAEQAAAAAAABwQKL8AzoAACADIARBAWoiBEcNAAtBACEEQQBBAC0A8O2HgABBfyACdEF/c3FBACACGzoA8O2HgAAgAEEAQaQQ/AsAIABBBGohBQJAIANFDQAgA0EBaiIEQQFxIQYgA0EDdCECIARB/v///wdxIQdBACEEIAMhCANAIAUgCEF8cWoiCSAEQfDth4AAai0AACACQRhxdCAJKAIAcjYCACAFIAhBf2pBfHFqIgkgBEHx7YeAAGotAAAgAkF4akEYcXQgCSgCAHI2AgAgAkFwaiECIAhBfmohCCAHIARBAmoiBEcNAAsgBkUNAgsgBSADIARrIgJBfHFqIgggBC0A8O2HgAAgAkEDdHQgCCgCAHI2AgAMAQtBAEEALQDw7YeAAEF/IAJ0QX9zcUEAIAIbOgDw7YeAACAAQQRqQQBBoBD8CwALIAAgA0EEakECdSIENgIAAkACQCAEQQFODQAgBCEIDAELIAAgBEECdGohAgNAAkAgAigCAEUNACAEIQgMAgsgACAEQX9qIgg2AgAgAkF8aiECIARBAUohBSAIIQQgBQ0ACwtBASABQX9qIgl0IQQgACAJQQV1IgVBAnRqQQRqKAIAIQICQAJAIAUgCE4NACACIARxDQELIAAgBUECdGpBBGogAiAEcjYCACAAKAIAIgggBUoNACAAIAVBAWoiCDYCAAsCQAJAIAhFDQAgAC0ABEEBcQ0BCyAAQQRqIQRCASEKQX8hAgNAIAQgCiAENQIAfCIKPgIAIARBBGohBCACQQFqIQIgCkIgiCIKQgBSDQALAkACQCAIIAJKDQAgACACQQFqIgg2AgAMAQsgCEEBSA0BCyAIQQFqIQIgACAIQQJ0aiEEA0AgBCgCAA0BIAAgAkF+ajYCACAEQXxqIQQgAkF/aiICQQFKDQALC0GI04eAAEEAQaQQ/AsAIAVBAnRBjNOHgABqIgQgBCgCAEEBIAl0cjYCAAJAQQAoAojTh4AAIgMgBUoNAEEAIAVBAWoiAzYCiNOHgAALIANB/v///wdxIQsgA0EBcSEMIANBAnRBhNOHgABqIQ0gACgCACIHQf7///8HcSEOIAdBAXEhDyAHQQJ0IABqQXxqIRBBASEFA0AgBUECdCIJKAKAgISAACERQQAhBkEAIQQCQCAHQQFIDQAgEa0hEkIAIQogByECAkACQCAHQQFGDQBCACEKIA4hCCAQIQQgByECA0AgCkIghiAEQQRqNQIAhCASgkIghiAENQIAhCASgiEKIARBeGohBCACQX5qIQIgCEF+aiIIDQALIA9FDQELIApCIIYgACACQQJ0ajUCAIQgEoIhCgsgCqchBAsgCSAENgKw44eAAAJAIANBAUgNACARrSESQgAhCiADIQICQAJAIANBAUYNAEIAIQogCyEIIA0hBCADIQIDQCAKQiCGIARBBGo1AgCEIBKCQiCGIAQ1AgCEIBKCIQogBEF4aiEEIAJBfmohAiAIQX5qIggNAAsgDEUNAQsgCkIghiACQQJ0QYjTh4AAajUCAIQgEoIhCgsgCqchBgsgCSAGNgLQ6IeAACAFQQFqIgVBqAFHDQALIABBBGohCyABQQBIIQxBASEEA0ACQAJAAkACQCAEQQJ0KAKw44eAAEUNACAEQQFqIgRBqAFHDQQgAEEBEISAgIAADQELQgIhCkF/IQIgCyEEA0AgBCAKIAQ1AgB8Igo+AgAgBEEEaiEEIAJBAWohAiAKQiCIIgpCAFINAAsCQAJAAkAgACgCACIJIAJKDQAgACACQQFqIgk2AgAMAQsgCUEBSA0BCyAAIAlBAnRqIQQgCSECA0ACQCAEKAIARQ0AIAIhCQwCCyAAIAJBf2oiCTYCACAEQXxqIQQgAkEBSiEIIAkhAiAIDQALC0HoeiEEA0AgBEHM6IeAAGoiAiACKAIAQQJqIgg2AgACQCAIIARBnIWEgABqKAIAIgVJDQAgAiAIIAVrNgIACwJAIARFDQAgBEHQ6IeAAGoiAiACKAIAQQJqIgg2AgACQCAIIARBoIWEgABqKAIAIgVJDQAgAiAIIAVrNgIACyAEQQhqIQQMAQsLIAlFDQFBASEEQQAgACAJQQJ0aiICKAIAIghna0FgIAgbIAlBBXRqIAFMDQMgCUEBSA0CQgAhE0EAIQRCACESAkACQCAJQQFGDQAgCUEBcSENIAlB/v///wdxIRFBACEIQQAoAojTh4AAIQVCACESQQAhBANAIAAgBGoiB0EEaiIGNQIAIRRCACEKAkAgCCIDIAVODQAgBEGM04eAAGo1AgAhCgsgBiAUIAp9IBJ8Igo+AgAgCkI/hyESIAdBCGoiCDUCACEUQgAhCgJAIANBAWoiByAFTg0AIARBkNOHgABqNQIAIQoLIAggFCAKfSASfCIKPgIAIARBCGohBCAKQj+HIRIgB0EBaiIIIBFHDQALIA1FDQEgA0ECaiEECyALIARBAnQiBWoiCDUCACEKAkAgBEEAKAKI04eAAE4NACAFNQKM04eAACETCyAIIAogE30gEnw+AgALIAlBAWohBANAIAIoAgANAyAAIARBfmo2AgAgAkF8aiECIARBf2oiBEEBSg0ADAMLCw8LQQEhBCAMRQ0BC0HoeiEEA0AgBEHM6IeAAGoiCCAEQZyFhIAAaigCACICIAgoAgBqIARB7O2HgABqKAIAayIIQQAgAiAIIAJJG2s2AgACQCAEDQBBASEEDAILIARB0OiHgABqIgggBEGghYSAAGooAgAiAiAIKAIAaiAEQfDth4AAaigCAGsiCEEAIAIgCCACSRtrNgIAIARBCGohBAwACwsL8wsEA38BfgV/AXwCQAJAAkACQAJAIAAoAgAiAkEBSA0AIAAoAgAhAkEAIQNBBCEEA0AgBEH49YeAAGogACAEaigCADYCACAEQQRqIQQgA0EBaiIDIAJIDQALIAJBiARIDQBBACACNgL49YeAAEEAQQA1Avz1h4AAQn98IgU+Avz1h4AADAELAkBBoBAgAkECdCIEayICRQ0AIARB/PWHgABqQQAgAvwLAAtBACAAKAIAIgI2Avj1h4AAQQEhBCACQQFIDQJBAEEANQL89YeAAEJ/fCIFPgL89YeAACACQQFGDQELIAJBf2oiBkEDcSEDQQEhBAJAAkAgAkF+akEDSQ0AIAZBfHEhB0EAIQZBjPaHgAAhBANAIARBdGoiCCAFQj+HIAg1AgB8IgU+AgAgBEF4aiIIIAVCP4cgCDUCAHwiBT4CACAEQXxqIgggBUI/hyAINQIAfCIFPgIAIAQgBUI/hyAENQIAfCIFPgIAIARBEGohBCAHIAZBBGoiBkcNAAsgA0UNASAGQQFqIQQLIARBAnRB/PWHgABqIQQDQCAEIAVCP4cgBDUCAHwiBT4CACAEQQRqIQQgA0F/aiIDDQALCyACIQQLIARBAnQhAwJAA0AgA0H49YeAAGooAgANAUEAIARBf2oiAjYC+PWHgAAgA0F8aiEDIARBAUohBiACIQQgBg0ADAILCwJAIANFDQBBoIaIgABB/PWHgAAgA/wKAAALIAQhAiAEQYcESw0BCwJAQaAQIAJBAnQiBGsiA0UNACAEQaCGiIAAakEAIAP8CwALIAIhBAtBACECQQAgBDYCnIaIgAACQCAEQQFIDQBBACgCoIaIgABBAXENAEEAIQkDQEEAIQMCQAJAIARBAUYNACAEQQFxIQogBEF+cSEHQQAhA0GkhoiAACECA0BBACEGAkAgA0EBaiAETg0AIAIoAgBBH3QhBgsgAkF8aiIIIAgoAgBBAXYgBnI2AgBBACEGAkAgA0ECaiIDIARODQAgAkEEaigCAEEfdCEGCyACIAIoAgBBAXYgBnI2AgAgAkEIaiECIAcgA0cNAAsgCkUNAQtBACECAkAgA0EBaiIGIARODQAgBkECdCgCoIaIgABBH3QhAgsgA0ECdCIDIAMoAqCGiIAAQQF2IAJyNgKghoiAAAsgBEECdEGchoiAAGohAgJAAkADQCACKAIADQFBACAEQX9qIgM2ApyGiIAAIAJBfGohAiAEQQFKIQYgAyEEIAYNAAsgCUEBaiEJDAELIAlBAWohCSAEQQFIDQBBACgCoIaIgABBAXFFDQELCyAAEIuAgIAAIAFBAWpBAXYhAUEAIQcDQBCHgICAACELQeymiIAAQQBBnBD8CwBBAEEBNgLkpoiAAEEAIAtEAAAAAAAAZUCi/ANBAnQoAoCAhIAANgLopoiAAEHAloiAAEHkpoiAAEGchoiAABCMgICAAAJAAkBBACgCwJaIgAAiCEEBRw0AQQAoAsSWiIAAQQFGDQELAkAgCEEAKAL49YeAAEcNACAIQQFqIQIgCEECdCEEA0AgAkF/aiICQQFIDQIgBEH49YeAAGohAyAEQcCWiIAAaiEGIARBfGohBCAGKAIAIAMoAgBGDQALC0EBIQACQAJAA0BBACgC+PWHgAAhBCAAIAlGDQECQCAIIARHDQAgCEEBaiECIAhBAnQhBANAIAJBf2oiAkEBSA0EIARB+PWHgABqIQMgBEHAloiAAGohBiAEQXxqIQQgBigCACADKAIARg0ACwsgAEEBaiEAQYi3iIAAQcCWiIAAQazHiIAAEI2AgIAAQcCWiIAAQYi3iIAAQcCWiIAAEI2AgIAAQQAhAkEAKALAloiAACIIQQFHDQBBACgCxJaIgABBAUcNAAwFCwsgCCAERg0AQQAPCyAIQQFqIQIgCEECdCEEA0AgAkF/aiICQQFIDQEgBEH49YeAAGohAyAEQcCWiIAAaiEGIARBfGohBCAGKAIAIAMoAgBGDQALQQAPC0EBIQIgB0EBaiIHIAFHDQALCyACC5gNBAh/An4BfwJ+AkACQCABKAIAIgNBAUgNAEEAIQRBBCEFA0AgACAFaiABIAVqKAIANgIAIAVBBGohBSAEQQFqIgQgASgCACIDSA0ACyADQYcESg0BCwJAQaAQIANBAnQiBWsiBEUNACAAIAVqQQRqQQAgBPwLAAsgASgCACEDCyAAIAM2AgACQAJAAkAgAyACKAIAIgZHDQAgA0EBaiEEIAIgA0ECdCIBaiEFIAAgAWohAQNAIARBf2oiBEEBSA0CIAUoAgAhByABKAIAIQggBUF8aiEFIAFBfGohASAIIAdGDQALIAggB00NAgwBCyADIAZMDQELQQAhCUEAIQECQCADRQ0AIANBBXQhBQJAIAAgA0ECdGooAgAiAQ0AIAVBYGohAQwBCyAFIAFnayEBCwJAAkAgBkUNACABIAZBBXRrIAIgBkECdGooAgBnaiEBAkAgBkEBTg0AIAYhCQwBCyACKAIAIQlBACEEQQQhBQNAIAVBpPiIgABqIAIgBWooAgA2AgAgBUEEaiEFIARBAWoiBCAJSA0ACyAJQYcESg0BCwJAQaAQIAlBAnQiBWsiBEUNACAFQaj4iIAAakEAIAT8CwALIAIoAgAhCQtBACAJNgKk+IiAAAJAIAFBAUgNAANAIAEhCgJAIAlBAUgNACAJQQNxIQNBACEBQQAhBwJAAkAgCUEESQ0AIAlB/P///wdxIQZBACEBQbT4iIAAIQVBACEHA0AgBUF0aiIEIAQoAgAiBEEBdCABcjYCACAFQXhqIgEgASgCACIBQQF0IARBH3ZyNgIAIAVBfGoiBCAEKAIAIghBAXQgAUEfdnI2AgAgBSAFKAIAIgRBAXQgCEEfdnI2AgAgBUEQaiEFIARBH3YhASAGIAdBBGoiB0cNAAsgA0UNAQsgB0ECdEGo+IiAAGohBQNAIAUgBSgCACIEQQF0IAFyNgIAIAVBBGohBSAEQR92IQEgA0F/aiIDDQALCyAEQX9KDQBBACAJQQFqIgU2AqT4iIAAIAlBAnRBATYCqPiIgAAgBSEJCyAKQX9qIQEgCkEBSg0ACwsgAEEEaiEKQQAoAqT4iIAAIQMDQAJAAkACQAJAIAAoAgAiBiAJRw0AIAlBAWohASAJQQJ0IQUDQCABQX9qIgFBAUgNAiAFQaT4iIAAaiEEIAAgBWohByAFQXxqIQUgBygCACIHIAQoAgAiBEYNAAsgCUEBSA0DIAcgBEsNAgwDCyAGIAlMDQILIAZBAUgNAQtCACELQQAhBUIAIQwCQAJAIAZBAUYNACAGQQFxIQ0gBkF+cSEJQgAhDEEAIQFBACEFA0AgACAFaiIHQQRqIgg1AgAhDkIAIQ8CQCABIgQgA04NACAFQaj4iIAAajUCACEPCyAIIA4gD30gDHwiDz4CACAPQj+HIQwgB0EIaiIBNQIAIQ5CACEPAkAgBEEBaiIHIANODQAgBUGs+IiAAGo1AgAhDwsgASAOIA99IAx8Ig8+AgAgBUEIaiEFIA9CP4chDCAHQQFqIgEgCUcNAAsgDUUNASAEQQJqIQULIAogBUECdCIEaiIBNQIAIQ8CQCAFIANODQAgBDUCqPiIgAAhCwsgASAPIAt9IAx8PgIACyAGQQFqIQEgACAGQQJ0aiEFAkADQCAFKAIADQEgACABQX5qNgIAIAVBfGohBSABQX9qIgFBAUoNAAsLIAMhCQsCQCAJIAIoAgBHDQAgCUEBaiEBIAlBAnQhBQNAIAFBf2oiAUEBSA0DIAIgBWohBCAFQaT4iIAAaiEHIAVBfGohBSAHKAIAIAQoAgBGDQALCyAJQQFIDQBBACEGQQAhAQJAAkAgCUEBRg0AIAlBAXEhDSAJQf7///8HcSEIQQAhAUGs+IiAACEFA0BBACEEAkAgAUEBaiAJTg0AIAUoAgBBH3QhBAsgBUF8aiIHIAcoAgBBAXYgBHI2AgBBACEEAkAgAUECaiIBIAlODQAgBUEEaigCAEEfdCEECyAFIAUoAgBBAXYgBHI2AgAgBUEIaiEFIAggAUcNAAsgDUUNAQsCQCABQQFqIgUgCU4NACAFQQJ0KAKo+IiAAEEfdCEGCyABQQJ0IgUgBSgCqPiIgABBAXYgBnI2Aqj4iIAACyAJQQJ0IgVBpPiIgABqKAIADQAgBUGg+IiAAGohBQJAA0BBACAJQX9qIgM2AqT4iIAAIAlBAUwNASAJQX9qIQkgBSgCACEBIAVBfGohBSABRQ0ACwsgAyEJDAALCwu/BAEFfwJAAkACQCAAKAIAIgJFDQAgAkEBTg0BQQAhAgwCCyABQTA6AABBASECDAELIAJBAWohAyAAIAJBAnRqIQRBACECQQAhBQNAAkACQAJAIAUgBCgCACIAQRx2IgZyRQ0AIAEgAmogBi0AoIWEgAA6AAAgAkEBaiECIAQtAANBD3EhBQwBCyAAQRh2IgUNAEEBIQYMAQsgASACaiAFLQCghYSAADoAACACQQFqIQIgBCgCACEAQQAhBgsgAEEUdkEPcSEFAkACQAJAAkAgBkUNACAFRQ0BCyABIAJqIAUtAKCFhIAAOgAAIAJBAWohAiAELwECQQ9xIQUMAQsgAEEQdkEPcSIFDQBBASEGDAELIAEgAmogBS0AoIWEgAA6AAAgAkEBaiECIAQoAgAhAEEAIQYLIABBDHZBD3EhBQJAAkACQAJAIAZFDQAgBUUNAQsgASACaiAFLQCghYSAADoAACACQQFqIQIgBCgCAEEIdkEPcSEFDAELIABBCHZBD3EiBQ0AQQEhBgwBCyABIAJqIAUtAKCFhIAAOgAAIAJBAWohAiAEKAIAIQBBACEGCyAAQQR2QQ9xIQUCQAJAAkACQCAGRQ0AIAVFDQELIAEgAmogBS0AoIWEgAA6AAAgAkEBaiECIAQoAgBBD3EhAAwBCyAAQQ9xIgANAEEAIQUMAQsgASACaiAALQCghYSAADoAAEEBIQUgAkEBaiECCyAEQXxqIQQgA0F/aiIDQQFKDQALCyABIAJqQQA6AAAL3QYCC38CfEEAIQBBACgCgNOHgAAiAUEBakH/AXEiAiACLQCA0YeAACICQQAtAITTh4AAaiIDQf8BcSIELQCA0YeAACIFOgCA0YeAACAEIAI6AIDRh4AAIAUgAmpB/wFxLQCA0YeAACEFIAFBAmpB/wFxIgIgAyACLQCA0YeAACICaiIDQf8BcSIELQCA0YeAACIGOgCA0YeAACAEIAI6AIDRh4AAIAYgAmpB/wFxLQCA0YeAACEGIAFBA2pB/wFxIgIgAyACLQCA0YeAACICaiIDQf8BcSIELQCA0YeAACIHOgCA0YeAACAEIAI6AIDRh4AAIAcgAmpB/wFxLQCA0YeAACEHIAFBBGpB/wFxIgIgAyACLQCA0YeAACICaiIDQf8BcSIELQCA0YeAACIIOgCA0YeAACAEIAI6AIDRh4AAIAggAmpB/wFxLQCA0YeAACEIIAFBBWpB/wFxIgIgAyACLQCA0YeAACICaiIEQf8BcSIDLQCA0YeAACIJOgCA0YeAACADIAI6AIDRh4AAIAkgAmpB/wFxLQCA0YeAACEJIAFBBmpB/wFxIgEgBCABLQCA0YeAACIDaiICQf8BcSIELQCA0YeAACIKOgCA0YeAACAEIAM6AIDRh4AAIAogA2pB/wFxLQCA0YeAACEDQQAgATYCgNOHgABBACAENgKE04eAAEQAAAAAAADwQiELAkAgBbhEAAAAAAAAcECiIAa4oEQAAAAAAABwQKIgB7igRAAAAAAAAHBAoiAIuKBEAAAAAAAAcECiIAm4oEQAAAAAAABwQKIgA7igIgxEAAAAAAAAMENjRQ0ARAAAAAAAAPBCIQtBACEAA0AgACEEIAFBAWpB/wFxIgEgAiABLQCA0YeAACIAaiICQf8BcSIDLQCA0YeAACIFOgCA0YeAACADIAA6AIDRh4AAIAUgAGpB/wFxLQCA0YeAACEAIAtEAAAAAAAAcECiIQsgDCAEuKBEAAAAAAAAcECiIgxEAAAAAAAAMENjDQALQQAgAzYChNOHgABBACABNgKA04eAAAsCQCAMRAAAAAAAAEBDZkUNAANAIABBAXYhACALRAAAAAAAAOA/oiELIAxEAAAAAAAA4D+iIgxEAAAAAAAAQENmDQALCyAMIAC4oCALowsXAEEAIAAgAEECSxtBtBBsQfC9hoAAagsIAEGQ74aAAAvlCQEJf0EAIQACQANAIABBAWohASAAQfC9hoAAai0AAEUNASAAQbQQSSECIAEhACACDQALCwJAIAFBy29qQc1vTw0AQQAPCyABQQZqIQMgAUF9aiEEIAFBfmohAEEAIQJBxP+GgABBAEGkEPwLAEEAIQUDQEFQIQYCQCAAQfC9hoAAai0AACIHQVBqQf8BcUEKSQ0AAkAgB0Gff2pB/wFxQQZPDQBBqX8hBgwBCwJAIAdBv39qQf8BcUEFTQ0AQQAPC0FJIQYLIAVBAXZB/P///wdxIgggBiAHaiACQRxxdCAIKALI/4aAAHI2Asj/hoAAIABBf2ohACACQQRqIQIgBUEBaiEFIARBf2oiBEF+Rw0AC0EAIAFBBmpBA3Y2AsT/hoAAIANBA3YiAEEBaiEBIABBAnRBxP+GgABqIQACQANAIAAoAgANAUEAIAFBfmo2AsT/hoAAIABBfGohACABQX9qIgFBAUoNAAsLQQAhAAJAA0AgAEEBaiEBIABBpM6GgABqLQAARQ0BIABBtBBJIQIgASEAIAINAAsLAkAgAUHLb2pBzW9PDQBBAA8LIAFBBmohAyABQbEQaiEEIAFBshBqIQBBACECQeiPh4AAQQBBpBD8CwBBACEFA0BBUCEGAkAgAEHwvYaAAGotAAAiB0FQakH/AXFBCkkNAAJAIAdBn39qQf8BcUEGTw0AQal/IQYMAQsCQCAHQb9/akH/AXFBBU0NAEEADwtBSSEGCyAFQQF2Qfz///8HcSIIIAYgB2ogAkEccXQgCCgC7I+HgAByNgLsj4eAACAAQX9qIQAgAkEEaiECIAVBAWohBSAEQX9qIgRBshBHDQALQQAgAUEGakEDdjYC6I+HgAAgA0EDdiIAQQFqIQEgAEECdEHoj4eAAGohAAJAA0AgACgCAA0BQQAgAUF+ajYC6I+HgAAgAEF8aiEAIAFBf2oiAUEBSg0ACwtBACEAAkADQCAAQQFqIQEgAEHY3oaAAGotAABFDQEgAEG0EEkhAiABIQAgAg0ACwsCQCABQctvakHNb08NAEEADwsgAUEGaiEDIAFB5SBqIQQgAUHmIGohAEEAIQJBjKCHgABBAEGkEPwLAEEAIQUDQEFQIQYCQCAAQfC9hoAAai0AACIHQVBqQf8BcUEKSQ0AAkAgB0Gff2pB/wFxQQZPDQBBqX8hBgwBCwJAIAdBv39qQf8BcUEFTQ0AQQAPC0FJIQYLIAVBAXZB/P///wdxIgggBiAHaiACQRxxdCAIKAKQoIeAAHI2ApCgh4AAIABBf2ohACACQQRqIQIgBUEBaiEFIARBf2oiBEHmIEcNAAtBACABQQZqQQN2IgA2Aoygh4AAIANBAXZB/P///wdxQYygh4AAaiEBAkADQAJAIAEoAgBFDQAgACECDAILQQAgAEF/aiICNgKMoIeAACABQXxqIQEgAEEBSiEFIAIhACAFDQALIAINAEEADwtBACEAAkAgAkGEAkoNAEEAKAKQoIeAACIBQQFxRQ0AQQEhAAJAIAJBAUcNACABQQFHDQBBAEEwOwGQ74aAAEEBDwtBsLCHgABBxP+GgABBjKCHgAAQhYCAgABBjKCHgAAQi4CAgABB1MCHgABBsLCHgABB6I+HgAAQjICAgABB1MCHgABBkO+GgAAQhoCAgAALIAALryMFDH8Efhh/AX4CfwJAAkAgACgCACIBQQFIDQAgACgCACEBQQAhAkEEIQMDQCADQYDoiIAAaiAAIANqKAIANgIAIANBBGohAyACQQFqIgIgAUgNAAsgAUGHBEoNAQsCQEGgECABQQJ0IgNrIgJFDQAgA0GE6IiAAGpBACAC/AsACyAAKAIAIQELQQAgATYCgOiIgABBACAAKAIAIgQ2AvjniIAAQQBBAkECQQIgACgCBCIDIANsayADbCICIANsayACbCICIANsayACbCICIANsQX5qIAJsIgU2AvzniIAAQdjJiYAAQQBBpBD8CwAgBEEFdCIGQX9qQQV1IgNBAnRB3MmJgABqIgIgAigCAEGAgICAeHI2AgACQEEAKALYyYmAACADSg0AQQAgA0EBajYC2MmJgAALQdjJiYAAQdjJiYAAIAAQhYCAgAACQAJAQQAoAtjJiYAAIgdBAU4NACAHIQIMAQsgB0EDcSEIQQAhAkEAIQkCQAJAIAdBBEkNACAHQfz///8HcSEKQQAhAkHoyYmAACEDQQAhCQNAIANBdGoiASABKAIAIgFBAXQgAnI2AgAgA0F4aiICIAIoAgAiAkEBdCABQR92cjYCACADQXxqIgEgASgCACILQQF0IAJBH3ZyNgIAIAMgAygCACIBQQF0IAtBH3ZyNgIAIANBEGohAyABQR92IQIgCiAJQQRqIglHDQALIAhFDQELIAlBAnRB3MmJgABqIQMDQCADIAMoAgAiAUEBdCACcjYCACADQQRqIQMgAUEfdiECIAhBf2oiCA0ACwsCQCABQX9MDQAgByECDAELQQAgB0EBaiICNgLYyYmAACAHQQJ0QQE2AtzJiYAACyAAQQRqIQwCQAJAAkACQAJAAkACQAJAIAIgACgCACIDRw0AIAJBAWohASACQQJ0IQMDQCABQX9qIgFBAUgNAiAAIANqIQggA0HYyYmAAGohCSADQXxqIQMgCSgCACIJIAgoAgAiCEYNAAsgAkEBSA0DIAkgCEsNAgwDCyACQQFIDQIgAiADTA0CDAELIAJBAU4NACACIQEMAgtCACENQQAhA0IAIQ4CQAJAIAJBAUYNACACQQFxIQogAkF+cSELQgAhDkEAIQFBACEDA0AgA0HcyYmAAGoiCTUCACEPQgAhEAJAIAEiCCAAKAIATg0AIAAgA2pBBGo1AgAhEAsgCSAPIBB9IA58IhA+AgAgEEI/hyEOIANB4MmJgABqIgE1AgAhD0IAIRACQCAIQQFqIgkgACgCAE4NACAAIANqQQhqNQIAIRALIAEgDyAQfSAOfCIQPgIAIANBCGohAyAQQj+HIQ4gCUEBaiIBIAtHDQALIApFDQEgCEECaiEDCyADQQJ0IgFB3MmJgABqIQggATUC3MmJgAAhEAJAIAMgACgCAE4NACAMIAFqNQIAIQ0LIAggECANfSAOfD4CAAsgAkECdCEDA0AgA0HYyYmAAGooAgANAUEAIAJBf2oiATYC2MmJgAAgA0F8aiEDIAJBAUohCCABIQIgCA0ACwJAQaAQIANrIgJFDQAgA0Gwx4iAAGpBACAC/AsAC0EAIAE2AqzHiIAADAMLIAIiAUEBSA0AAkAgAUECdCIDRQ0AQbDHiIAAQdzJiYAAIAP8CgAACyABQYgESQ0BQQAgATYCrMeIgAAgAUECdCIDRQ0DQdzJiYAAQbDHiIAAIAP8CgAADAMLAkBBoBAgAUECdCIDayICRQ0AIANBsMeIgABqQQAgAvwLAAtBACABNgKsx4iAAAwBCwJAQaAQIANrIgJFDQAgA0Gwx4iAAGpBACAC/AsAC0EAIAE2AqzHiIAAIAFBAnQiCEUNAEHcyYmAAEGwx4iAACAI/AoAAAsgAkUNACADQdzJiYAAakEAIAL8CwALQQAgATYC2MmJgAAgBEEBdCIRQQAgEUEAShsiA0H8////B3EhEiADQQJxQQFyIRMgEUEBciIDQQAgA0EAShtBAnRBBGohFCAEQQFqIRUgBEF+cSEWIARBAXEhFyAEQf7///8HcSEJIARBfmohGCAEQX9qIRkgBEECdCIaQfzZiYAAaiEbIBpBoOqJgABqIRwgBEEDdEGg6omAAGohHUGgECAaayEeIBpBgNqJgABqIR9BACgCgOiIgAAhICAEQYcESiEhQR8hAwNAAkAgBiADIiJ1IiNFDQBBACEKAkACQCAEQQBIDQACQCAURQ0AQaDqiYAAQQAgFPwLAAsCQCAERQ0AQQAhCEHYyYmAACEkQaDqiYAAISUgHCEmIBkhJwNAAkAgCCABTg0AIAhBAnQiKDUC3MmJgAAiDVANACAIQQFqIgMgBE4NAEIAISlCACEQAkACQCAYIAhGDQAgJ0F+cSEqIBkgCGtBAXEhK0IAIRBBACEDQQAhAgNAICUgA2oiC0EEaiIKNQIAIQ9CACEOAkAgCCACakEBaiIHIAFODQAgJCADakEIajUCACANfiEOCyAKIBAgD3wgDnwiED4CACAQQiCIIQ4gC0EIaiILNQIAIQ9CACEQAkAgB0EBaiABTg0AICQgA2pBDGo1AgAgDX4hEAsgCyAOIA98IBB8IhA+AgAgA0EIaiEDIBBCIIghECAqIAJBAmoiAkcNAAsgK0UNASAIIAJqQQFqIQMLIChBoOqJgABqIANBAnQiC2oiAjUCACEOAkAgAyABTg0AIAs1AtzJiYAAIA1+ISkLIAIgECAOfCApfCIQPgIAIBBCIIghEAsgEFANACAmIQMDQCADIBAgAzUCAHwiED4CACADQQRqIQMgEEIgiCIQQgBSDQALCyAmQQRqISYgJ0F/aiEnICVBCGohJSAkQQRqISQgCEEBaiIIIARHDQALC0EAIQhBACECAkAgEUEDSA0AQQAhCEGg6omAACEDQQAhAgNAIAMgAygCACILQQF0IAJyNgIAIANBBGoiAiACKAIAIgJBAXQgC0EfdnI2AgAgA0EIaiILIAsoAgAiC0EBdCACQR92cjYCACADQQxqIgIgAigCACICQQF0IAtBH3ZyNgIAIANBEGohAyACQR92IQIgEiAIQQRqIghHDQALCyAIQQJ0QaDqiYAAaiEDIBMhCANAIAMgAygCACILQQF0IAJyNgIAIANBBGohAyALQR92IQIgCEF/aiIIDQALAkAgBA0AQQAhCgwBC0EAIQhBqOqJgAAhCwNAQgAhEAJAIAggAU4NACAIQQJ0NQLcyYmAACIQIBB+IRALIAhBA3QiAyAQQv////8PgyADNQKg6omAAHwiDj4CoOqJgAAgAyAQQiCIIAM1AqTqiYAAfCAOQiCIfCIQPgKk6omAAAJAIBBCgICAgBBUDQAgCyEDA0AgAyADKAIAQQFqIgI2AgAgA0EEaiEDIAJFDQALCyALQQhqIQsgCEEBaiIIIARHDQALQQAhC0Gg6omAACEIIBwhCgNAIAtBAnQiBygCoOqJgAAgBWytIQ5CACEQAkACQAJAIBkNAEEAIQEMAQtBACEDQQAhAQNAIAggA2oiAiAQIAI1AgB8IANBhOiIgABqNQIAIA5+fCIQPgIAIAJBBGoiAiAQQiCIIAI1AgB8IANBiOiIgABqNQIAIA5+fCIQPgIAIBBCIIghECADQQhqIQMgCSABQQJqIgFHDQALIBdFDQELIAdBoOqJgABqIAFBAnQiA2oiAiAQIAI1AgB8IAM1AoToiIAAIA5+fCIQPgIAIBBCIIghEAsCQCAQUA0AIAohAwNAIAMgECADNQIAfCIQPgIAIANBBGohAyAQQiCIIhBCAFINAAsLIApBBGohCiAIQQRqIQggC0EBaiILIARHDQALAkAgGkUNAEGA2omAACAcIBr8CgAAC0EBIQogIQ0BCyAeRQ0AIB9BACAe/AsAC0EAIAQ2AvzZiYAAAkACQAJAAkACQAJAAkAgHSgCAA0AIBUhAiAaIQMCQCAEICBGDQAgICECIAQhASAKIAQgIEpxDQIMAwsDQCACQX9qIgJBAUgNASADQYDoiIAAaiEBIANB/NmJgABqIQggA0F8aiEDIAgoAgAiCCABKAIAIgtGDQALIAQhAiAEIQEgCiAIIAtLcQ0BDAILICAhAiAEIQEgCkUNAQtCACENAkACQAJAIBkNAEEAIQNCACEODAELQgAhDkEAIQFBACEDA0AgA0GA2omAAGoiCzUCACEPQgAhEAJAIAEiCCACTg0AIANBhOiIgABqNQIAIRALIAsgDyAQfSAOfCIQPgIAIBBCP4chDiADQYTaiYAAaiIBNQIAIQ9CACEQAkAgCEEBaiILIAJODQAgA0GI6IiAAGo1AgAhEAsgASAPIBB9IA58IhA+AgAgA0EIaiEDIBBCP4chDiALQQFqIgEgFkcNAAsgF0UNASAIQQJqIQMLIANBAnQiAUGA2omAAGohCCABNQKA2omAACEQAkAgAyACTg0AIAE1AoToiIAAIQ0LIAggECANfSAOfD4CAAsgGyEDIAQhAgNAAkAgAygCAEUNACACIQEMAgtBACEIQQAgAkF/aiIBNgL82YmAACADQXxqIQMgAkEBSiELIAEhAiALDQAMAgsLAkAgAUEBTg0AQQAhCAwBCyABQQJ0IQMCQANAIANB/NmJgABqKAIADQFBACEIQQAgAUF/aiICNgL82YmAACADQXxqIQMgAUEBSiELIAIhASALDQALQQAhAQwBCwJAIANFDQBB3MmJgABBgNqJgAAgA/wKAAALIAFBhwRLDQFBASEICwJAQaAQIAFBAnQiA2siAkUNACADQdzJiYAAakEAIAL8CwALQQAgATYC2MmJgAAgI0EBcUUNAyAIRQ0CDAELQQAgATYC2MmJgAAgI0EBcUUNAgsgAUEDcSELQQAhAkEAIQoCQAJAIAFBf2pBA0kNACABQXxxISRBACECQejJiYAAIQNBACEKA0AgA0F0aiIIIAgoAgAiCEEBdCACcjYCACADQXhqIgIgAigCACICQQF0IAhBH3ZyNgIAIANBfGoiCCAIKAIAIgdBAXQgAkEfdnI2AgAgAyADKAIAIghBAXQgB0EfdnI2AgAgA0EQaiEDIAhBH3YhAiAkIApBBGoiCkcNAAsgC0UNAQsgCkECdEHcyYmAAGohAwNAIAMgAygCACIIQQF0IAJyNgIAIANBBGohAyAIQR92IQIgC0F/aiILDQALCyAIQX9KDQBBACABQQFqIgM2AtjJiYAAIAFBAnRBATYC3MmJgAAgAyEBCwJAAkACQCABIAAoAgAiA0cNACABQQFqIQIgAUECdCEDA0AgAkF/aiICQQFIDQIgACADaiEIIANB2MmJgABqIQsgA0F8aiEDIAsoAgAiCyAIKAIAIghGDQALIAFBAUgNAyALIAhLDQIMAwsgAUEBSA0CIAEgA0oNAQwCCyABQQFIDQELQgAhDUEAIQNCACEOAkACQCABQQFGDQAgAUEBcSEHIAFBfnEhCkIAIQ5BACECQQAhAwNAIANB3MmJgABqIgs1AgAhD0IAIRACQCACIgggACgCAE4NACAAIANqQQRqNQIAIRALIAsgDyAQfSAOfCIQPgIAIBBCP4chDiADQeDJiYAAaiICNQIAIQ9CACEQAkAgCEEBaiILIAAoAgBODQAgACADakEIajUCACEQCyACIA8gEH0gDnwiED4CACADQQhqIQMgEEI/hyEOIAtBAWoiAiAKRw0ACyAHRQ0BIAhBAmohAwsgA0ECdCICQdzJiYAAaiEIIAI1AtzJiYAAIRACQCADIAAoAgBODQAgDCACajUCACENCyAIIBAgDX0gDnw+AgALIAFBAnRB2MmJgABqIQMgASECA0ACQCADKAIARQ0AIAIhAQwCC0EAIAJBf2oiATYC2MmJgAAgA0F8aiEDIAJBAUohCCABIQIgCA0ACwsgIkF/aiEDICINAAsgAUECdCEDAkACQCABQQFIDQACQCADRQ0AQbDHiIAAQdzJiYAAIAP8CgAACyABQYcESw0BC0GgECADayICRQ0AIANBsMeIgABqQQAgAvwLAAtBACABNgKsx4iAAAvlBAEJfwJAAkAgAigCACIDDQBBACEEDAELIANBBXQhBQJAIAIgA0ECdGooAgAiAw0AIAVBYGohBAwBCyAFIANnayEEC0GUm4qAACABQazHiIAAEI2AgIAAQQIhA0G4q4qAACEBA0ACQAJAIANBAXENACABIANBAXZBpBBsQfCKioAAaiIFIAUQjYCAgAAMAQsgASABQdxvakGUm4qAABCNgICAAAsgAUGkEGohASADQQFqIgNBEEcNAAtBgLCMgABBAEGcEPwLAEEAQoGAgIAQNwL4r4yAAEGwj4yAACEDQbCPjIAAQfivjIAAQazHiIAAEI2AgIAAAkAgBEEDakECdSIGQQFIDQAgAkEEaiEHIAZBAnRBfGohCEHUn4yAACEBQbCPjIAAIQNBACEFA0ACQCAFRQ0AIAEgAyADEI2AgIAAIAMgASABEI2AgIAAIAEgAyADEI2AgIAAIAMgASABEI2AgIAACyAIIAVqIQlBACEKAkAgBkF/aiIGQQN2IgsgAigCAE4iBA0AIAcgC0ECdGooAgAgCUEccUEDcnZBAXRBAnEhCgsCQCAEDQAgByALQQJ0aigCACAJQRxxQQJydkEBcSAKciEKCyAKQQF0IQoCQCAEDQAgByALQQJ0aigCACAJQRxxQQFydkEBcSAKciEKCyAKQQF0IQoCQCAEDQAgByALQQJ0aigCACAJQRxxdkEBcSAKciEKCwJAIApFDQAgASADIApBpBBsQfCKioAAahCNgICAACADIQQgASEDIAQhAQsgBUF8aiEFIAZBAEoNAAsLIAAgA0H4r4yAABCNgICAAAvLCwcMfwF+A38CfgF/An4Bf0EAIQMCQAJAQQAoAvjniIAAIgRBf0gNAEEAIQMCQCAEQQJ0QQhqIgVFDQBB0NeIgABBACAF/AsACyAEQQFIDQAgBEH+////B3EhBiAEQQFxIQcgAkEEaiEIIAFBBGohCSAEQX9qIgpBfnEhCyAKQQFxIQwgBEECdCIFQczXiIAAaiENIAVB0NeIgABqIQ4gAigCACEFQQA1AoToiIAAIQ9BACgC/OeIgAAhECABKAIAIRFBACESA0BCACETQgAhFAJAIBIgEU4NACAJIBJBAnRqNQIAIRQLAkACQAJAIAoNAEEAIQEMAQtCACETQQAhFUEAIQEDQCABQdDXiIAAaiIDNQIAIRZCACEXAkAgFSIYIAVODQAgFCACIAFqQQRqNQIAfiEXCyADIBMgFnwgF3wiFz4CACAXQiCIIRMgAUHU14iAAGoiFTUCACEWQgAhFwJAIBhBAWoiAyAFTg0AIBQgAiABakEIajUCAH4hFwsgFSATIBZ8IBd8Ihc+AgAgAUEIaiEBIBdCIIghEyADQQFqIhUgBkcNAAsgB0UNASAYQQJqIQELIAFBAnQiFUHQ14iAAGohGCAVNQLQ14iAACEWQgAhFwJAIAEgBU4NACAUIAggFWo1AgB+IRcLIBggEyAWfCAXfCIXPgIAIBdCIIghEwsgDiATIA41AgB8IhY3AgBBASEYIBBBACgC0NeIgAAiAWytIhMgD34gAa18QiCIIRcgFqchASAWQiCIpyEVAkACQAJAAkAgCg4CAwEAC0EAIQFBACEVA0AgAUHQ14iAAGogFyABQdTXiIAAaiIYNQIAfCABQYjoiIAAajUCACATfnwiFz4CACAYIBdCIIggAUHY14iAAGo1AgB8IAFBjOiIgABqNQIAIBN+fCIXPgIAIBdCIIghFyABQQhqIQEgCyAVQQJqIhVHDQALIAxFDQEgFUEBaiEYCyAYQQJ0IgFBzNeIgABqIBcgATUC0NeIgAB8IAE1AoToiIAAIBN+fCIXPgIAIBdCIIghFwsgDkEEaigCACEVIA4oAgAhAQsgDSAXIAGtfCIXPgIAIA4gFSAXQiCIp2o2AgAgEkEBaiISIARHDQALAkAgBEECdCIBRQ0AIABBBGpB0NeIgAAgAfwKAAALQQEhAyAEQYgETg0BC0GgECAEQQJ0IgFrIgVFDQAgACABakEEakEAIAX8CwALIAAgBDYCAAJAAkACQAJAIARBAnRB0NeIgABqKAIADQACQCAEQQAoAoDoiIAAIgFHDQAgBEEBaiEFIARBAnQhAQNAIAVBf2oiBUEBSA0CIAFBgOiIgABqIRUgACABaiEYIAFBfGohASAYKAIAIhggFSgCACIVRg0ACyADIBggFUtxDQIMAwsgAyAEIAFKcUUNAgwBCyADRQ0BC0IAIRRBACEBQgAhEwJAAkAgBEEBRg0AIARBAXEhCyAEQX5xIQZBACEFQQAoAoDoiIAAIRVCACETQQAhAQNAIAAgAWoiA0EEaiICNQIAIRZCACEXAkAgBSIYIBVODQAgAUGE6IiAAGo1AgAhFwsgAiAWIBd9IBN8Ihc+AgAgF0I/hyETIANBCGoiBTUCACEWQgAhFwJAIBhBAWoiAyAVTg0AIAFBiOiIgABqNQIAIRcLIAUgFiAXfSATfCIXPgIAIAFBCGohASAXQj+HIRMgA0EBaiIFIAZHDQALIAtFDQEgGEECaiEBCyAAQQRqIAFBAnQiFWoiBTUCACEXAkAgAUEAKAKA6IiAAE4NACAVNQKE6IiAACEUCyAFIBcgFH0gE3w+AgALIAAgBEECdGohAQNAIAEoAgANASAAIARBf2oiBTYCACABQXxqIQEgBEEBSiEVIAUhBCAVDQAMAgsLIARBAUgNACAEQQFqIQUgACAEQQJ0aiEBA0AgASgCAA0BIAAgBUF+ajYCACABQXxqIQEgBUF/aiIFQQFKDQALCwsLugUBAEGAgAQLsQUCAAAAAwAAAAUAAAAHAAAACwAAAA0AAAARAAAAEwAAABcAAAAdAAAAHwAAACUAAAApAAAAKwAAAC8AAAA1AAAAOwAAAD0AAABDAAAARwAAAEkAAABPAAAAUwAAAFkAAABhAAAAZQAAAGcAAABrAAAAbQAAAHEAAAB/AAAAgwAAAIkAAACLAAAAlQAAAJcAAACdAAAAowAAAKcAAACtAAAAswAAALUAAAC/AAAAwQAAAMUAAADHAAAA0wAAAN8AAADjAAAA5QAAAOkAAADvAAAA8QAAAPsAAAABAQAABwEAAA0BAAAPAQAAFQEAABkBAAAbAQAAJQEAADMBAAA3AQAAOQEAAD0BAABLAQAAUQEAAFsBAABdAQAAYQEAAGcBAABvAQAAdQEAAHsBAAB/AQAAhQEAAI0BAACRAQAAmQEAAKMBAAClAQAArwEAALEBAAC3AQAAuwEAAMEBAADJAQAAzQEAAM8BAADTAQAA3wEAAOcBAADrAQAA8wEAAPcBAAD9AQAACQIAAAsCAAAdAgAAIwIAAC0CAAAzAgAAOQIAADsCAABBAgAASwIAAFECAABXAgAAWQIAAF8CAABlAgAAaQIAAGsCAAB3AgAAgQIAAIMCAACHAgAAjQIAAJMCAACVAgAAoQIAAKUCAACrAgAAswIAAL0CAADFAgAAzwIAANcCAADdAgAA4wIAAOcCAADvAgAA9QIAAPkCAAABAwAABQMAABMDAAAdAwAAKQMAACsDAAA1AwAANwMAADsDAAA9AwAARwMAAFUDAABZAwAAWwMAAF8DAABtAwAAcQMAAHMDAAB3AwAAiwMAAI8DAACXAwAAoQMAAKkDAACtAwAAswMAALkDAADHAwAAywMAANEDAADXAwAA3wMAAOUDAAAwMTIzNDU2Nzg5YWJjZGVmAACnAwRuYW1lABAPcnNhX2tleWdlbi53YXNtAe0CDgAbcnNhX2tleWdlbl9fZ2V0X3NlZWRfYnVmZmVyARZyc2Ffa2V5Z2VuX19nZXRfcmVzdWx0AhRyc2Ffa2V5Z2VuX19nZW5lcmF0ZQMbX3JzYV9pbnRlcm5hbF9fcmFuZG9tX3ByaW1lBBtfcnNhX2ludGVybmFsX19taWxsZXJfcmFiaW4FFV9yc2FfaW50ZXJuYWxfX2JuX21vZAYYX3JzYV9pbnRlcm5hbF9fYm5fdG9faGV4BxVfcnNhX2ludGVybmFsX19yYW5kb20IHXJzYV9rZXlnZW5fX2dldF9tb2Rwb3dfYnVmZmVyCR1yc2Ffa2V5Z2VuX19nZXRfbW9kcG93X3Jlc3VsdAoScnNhX2tleWdlbl9fbW9kcG93CxlfcnNhX2ludGVybmFsX19tb250X3NldHVwDBdfcnNhX2ludGVybmFsX19tb250X3Bvdw0XX3JzYV9pbnRlcm5hbF9fbW9udF9tdWwHEgEAD19fc3RhY2tfcG9pbnRlcgkKAQAHLnJvZGF0YQB2CXByb2R1Y2VycwEMcHJvY2Vzc2VkLWJ5AQVjbGFuZ1YyMi4xLjQgKGh0dHBzOi8vZ2l0aHViLmNvbS9sbHZtL2xsdm0tcHJvamVjdCAzNTk5MDUwNDUwN2Q3OWUwYjlkZWI4MDljOGVlNWUxYjM0Y2VlZjIwKQCUAQ90YXJnZXRfZmVhdHVyZXMIKwtidWxrLW1lbW9yeSsPYnVsay1tZW1vcnktb3B0KxZjYWxsLWluZGlyZWN0LW92ZXJsb25nKwptdWx0aXZhbHVlKw9tdXRhYmxlLWdsb2JhbHMrE25vbnRyYXBwaW5nLWZwdG9pbnQrD3JlZmVyZW5jZS10eXBlcysIc2lnbi1leHQ=';
        var binary_string = atob(rsa_keygen_webassembly_base64);
        var module_bytes = new Uint8Array(binary_string.length);

        for (var i = 0; i < binary_string.length; i++)
        {
            module_bytes[i] = binary_string.charCodeAt(i);
        }

        var wasm_module = new WebAssembly.Module(module_bytes.buffer);
        rsa_keygen_wasm_exports = new WebAssembly.Instance(wasm_module, {}).exports;
    }
    return rsa_keygen_wasm_exports;
}

var lemon_crypto = (function ()
{
    var my = {};

    var base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    var hex_chars = "0123456789abcdef";

    // marks a signed payload inside the envelope. lemon-chat never signs, but the
    // separator is still split on so a signed message from any other cryptico client
    // yields the same plaintext it always did
    var signature_separator = "::52cee64bb3a38f6403386519a39ac91c::";

    // conversions, ported unchanged from cryptico: the envelope is built out of these, so a single
    // differing character would break older clients

    /**
     * @brief binary string (one character per byte) -> base64
     *
     * @param string t -> the binary string
     *
     * @return string the base64 text
     */
    my.b256to64 = function (t)
    {
        var a, c, n;
        var r = "", s = 0;
        var tl = t.length;

        for (n = 0; n < tl; n++)
        {
            c = t.charCodeAt(n);
            if (s == 0)
            {
                r += base64_chars.charAt((c >> 2) & 63);
                a = (c & 3) << 4;
            }
            else if (s == 1)
            {
                r += base64_chars.charAt((a | (c >> 4) & 15));
                a = (c & 15) << 2;
            }
            else if (s == 2)
            {
                r += base64_chars.charAt(a | ((c >> 6) & 3));
                r += base64_chars.charAt(c & 63);
            }
            s += 1;
            if (s == 3) { s = 0; }
        }

        if (s > 0)
        {
            r += base64_chars.charAt(a);
            r += "=";
        }
        if (s == 1)
        {
            r += "=";
        }
        return r;
    };

    /**
     * @brief base64 -> binary string (one character per byte)
     *
     * @param string t -> the base64 text
     *
     * @return string the binary string
     */
    my.b64to256 = function (t)
    {
        var c, n;
        var r = "", s = 0, a = 0;
        var tl = t.length;

        for (n = 0; n < tl; n++)
        {
            c = base64_chars.indexOf(t.charAt(n));
            if (c >= 0)
            {
                if (s) { r += String.fromCharCode(a | (c >> (6 - s)) & 255); }
                s = (s + 2) & 7;
                a = (c << s) & 255;
            }
        }
        return r;
    };

    /**
     * @brief hex -> base64
     *
     * @param string h -> the hex text
     *
     * @return string the base64 text
     */
    my.b16to64 = function (h)
    {
        var i, c;
        var ret = "";

        if (h.length % 2 == 1) { h = "0" + h; }

        for (i = 0; i + 3 <= h.length; i += 3)
        {
            c = parseInt(h.substring(i, i + 3), 16);
            ret += base64_chars.charAt(c >> 6) + base64_chars.charAt(c & 63);
        }
        if (i + 1 == h.length)
        {
            c = parseInt(h.substring(i, i + 1), 16);
            ret += base64_chars.charAt(c << 2);
        }
        else if (i + 2 == h.length)
        {
            c = parseInt(h.substring(i, i + 2), 16);
            ret += base64_chars.charAt(c >> 2) + base64_chars.charAt((c & 3) << 4);
        }
        while ((ret.length & 3) > 0) { ret += "="; }
        return ret;
    };

    /**
     * @brief base64 -> hex
     *
     * @param string s -> the base64 text
     *
     * @return string the hex text
     */
    my.b64to16 = function (s)
    {
        var ret = "";
        var i, v;
        var k = 0;
        var slop = 0;

        for (i = 0; i < s.length; ++i)
        {
            if (s.charAt(i) == "=") { break; }
            v = base64_chars.indexOf(s.charAt(i));
            if (v < 0) { continue; }

            if (k == 0)
            {
                ret += hex_chars.charAt(v >> 2);
                slop = v & 3;
                k = 1;
            }
            else if (k == 1)
            {
                ret += hex_chars.charAt((slop << 2) | (v >> 4));
                slop = v & 0xf;
                k = 2;
            }
            else if (k == 2)
            {
                ret += hex_chars.charAt(slop);
                ret += hex_chars.charAt(v >> 2);
                slop = v & 3;
                k = 3;
            }
            else
            {
                ret += hex_chars.charAt((slop << 2) | (v >> 4));
                ret += hex_chars.charAt(v & 0xf);
                k = 0;
            }
        }
        if (k == 1) { ret += hex_chars.charAt(slop << 2); }
        return ret;
    };

    /**
     * @brief a string's character codes as a byte array
     *
     * @param string string -> the text
     *
     * @return array the codes
     */
    my.string2bytes = function (string)
    {
        var bytes = new Array();
        for (var i = 0; i < string.length; i++)
        {
            bytes.push(string.charCodeAt(i));
        }
        return bytes;
    };

    /**
     * @brief a byte array as a string, one character per byte
     *
     * @param array bytes -> the bytes
     *
     * @return string the text
     */
    my.bytes2string = function (bytes)
    {
        var string = "";
        for (var i = 0; i < bytes.length; i++)
        {
            string += String.fromCharCode(bytes[i]);
        }
        return string;
    };

    /**
     * @brief zero padding to a whole number of aes blocks
     *        not pkcs#7: this is the padding cryptico shipped, and the envelope format is fixed by old clients
     *
     * @param array bytes -> the plaintext bytes
     *
     * @return array a padded copy
     */
    my.pad16 = function (bytes)
    {
        var new_bytes = bytes.slice(0);
        var padding = (16 - (bytes.length % 16)) % 16;

        for (var i = 0; i < padding; i++) { new_bytes.push(0); }
        return new_bytes;
    };

    /**
     * @brief the inverse of pad16: strips the trailing zero bytes
     *
     * @param array bytes -> the padded bytes
     *
     * @return array the bytes without the padding
     */
    my.depad = function (bytes)
    {
        var end = bytes.length;
        while (end > 0 && bytes[end - 1] == 0) { end--; }
        return bytes.slice(0, end);
    };

    // random material from getRandomValues, the csprng the rest of the client uses (the old stack
    // drew it from an arcfour pool seeded with Math.random)

    /**
     * @brief random bytes from the cryptographically secure RNG
     *
     * @param number count -> how many
     *
     * @return array the bytes
     */
    function random_bytes(count)
    {
        var buffer = new Uint8Array(count);
        var bytes = new Array(count);

        crypto.getRandomValues(buffer);
        for (var i = 0; i < count; i++) { bytes[i] = buffer[i]; }
        return bytes;
    }

    /**
     * @brief a fresh random 32-byte aes key
     *
     * @return array the key bytes
     */
    my.generateAESKey = function () { return random_bytes(32); };
    /**
     * @brief a fresh random 16-byte aes iv
     *
     * @return array the iv bytes
     */
    my.blockIV = function () { return random_bytes(16); };

    // -----------------------------------------------------------------------
    // big number helpers
    // -----------------------------------------------------------------------

    /**
     * @brief a BigInt from a hex string; throws when the text is not hex
     *
     * @param string hex -> the hex text
     *
     * @return bigint the value
     */
    function bigint_from_hex(hex)
    {
        if (typeof hex !== "string" || hex.length == 0 || /[^0-9a-fA-F]/.test(hex))
        {
            throw new Error("not a hex string");
        }
        return BigInt("0x" + hex);
    }

    /**
     * @brief minimal big-endian bytes of a positive value, the shape jsbn's toByteArray produced once its leading zero byte is skipped
     *
     * @param bigint value -> the value
     *
     * @return array the bytes
     */
    function bigint_to_bytes(value)
    {
        var hex = value.toString(16);
        var bytes = [];

        if (hex.length % 2 == 1) { hex = "0" + hex; }
        for (var i = 0; i < hex.length; i += 2)
        {
            bytes.push(parseInt(hex.substring(i, i + 2), 16));
        }
        return bytes;
    }

    /**
     * @brief how many bits a value needs
     *
     * @param bigint value -> the value
     *
     * @return number the bit length, 0 for anything not positive
     */
    function bit_length(value)
    {
        if (value <= 0n) { return 0; }
        return value.toString(2).length;
    }

    /**
     * @brief base^exponent mod modulus on the wasm, in hex; the modulus must be odd, which every rsa n, p and q is
     *
     * @param string base_hex -> the base
     * @param string exponent_hex -> the exponent
     * @param string modulus_hex -> the modulus
     *
     * @return string the result in hex
     */
    function wasm_modpow_hex(base_hex, exponent_hex, modulus_hex)
    {
        var wasm = rsa_crypto__get_rsa_keygen_wasm();
        var memory_bytes = new Uint8Array(wasm.memory.buffer);
        var operands = [base_hex, exponent_hex, modulus_hex];
        var i, j;

        for (i = 0; i < 3; i++)
        {
            var input_ptr = wasm.rsa_keygen__get_modpow_buffer(i);

            if (operands[i].length > 2080) { throw new Error("modpow operand too large"); }
            for (j = 0; j < operands[i].length; j++)
            {
                memory_bytes[input_ptr + j] = operands[i].charCodeAt(j);
            }
            memory_bytes[input_ptr + operands[i].length] = 0;
        }

        if (wasm.rsa_keygen__modpow() != 1) { throw new Error("rsa_keygen__modpow rejected input"); }

        memory_bytes = new Uint8Array(wasm.memory.buffer);
        var result_ptr = wasm.rsa_keygen__get_modpow_result();
        var hex = "";
        for (j = result_ptr; memory_bytes[j] != 0; j++)
        {
            hex += String.fromCharCode(memory_bytes[j]);
        }
        return hex;
    }

    /**
     * @brief base^exponent mod modulus on the wasm, for BigInts
     *
     * @param bigint base -> the base
     * @param bigint exponent -> the exponent
     * @param bigint modulus -> the modulus, odd
     *
     * @return bigint the result
     */
    function modpow_wasm(base, exponent, modulus)
    {
        return bigint_from_hex(wasm_modpow_hex(base.toString(16), exponent.toString(16), modulus.toString(16)));
    }

    /**
     * @brief base^exponent mod modulus by square and multiply over native BigInt
     *        the diffie-hellman step runs on the main thread, which never instantiates the wasm
     *        module, so it uses this instead
     *
     * @param bigint base -> the base
     * @param bigint exponent -> the exponent
     * @param bigint modulus -> the modulus
     *
     * @return bigint the result
     */
    my.modpow = function (base, exponent, modulus)
    {
        var result = 1n;

        base = base % modulus;
        while (exponent > 0n)
        {
            if ((exponent & 1n) == 1n) { result = (result * base) % modulus; }
            base = (base * base) % modulus;
            exponent >>= 1n;
        }
        return result;
    };

    // -----------------------------------------------------------------------
    // pkcs#1 v1.5, ported from rsa.js including its utf-8 expansion of the payload
    // -----------------------------------------------------------------------

    /**
     * @brief pkcs#1 type 2 padding of a text into an n-byte block: utf8 encoding, random non-zero padding, the 0x02 marker
     *        throws when the text does not fit
     *
     * @param string s -> the text
     * @param number n -> the block length in bytes
     *
     * @return bigint the padded block as a number
     */
    function pkcs1_pad(s, n)
    {
        var ba = new Array(n);
        var i = s.length - 1;
        var pad;

        if (n < s.length + 11) { throw new Error("Message too long for RSA (n=" + n + ", l=" + s.length + ")"); }

        while (i >= 0 && n > 0)
        {
            var c = s.charCodeAt(i--);

            if (c < 128)
            {
                ba[--n] = c;
            }
            else if ((c > 127) && (c < 2048))
            {
                ba[--n] = (c & 63) | 128;
                ba[--n] = (c >> 6) | 192;
            }
            else
            {
                ba[--n] = (c & 63) | 128;
                ba[--n] = ((c >> 6) & 63) | 128;
                ba[--n] = (c >> 12) | 224;
            }
        }
        ba[--n] = 0;

        // the pad bytes must all be non-zero, because the first zero after the header
        // is what marks where the payload starts. drawn as one block, with any zeros
        // redrawn individually so each byte stays uniform over 1..255
        var pad_block = random_bytes(n > 2 ? n - 2 : 0);
        var pad_index = 0;
        while (n > 2)
        {
            pad = pad_block[pad_index++];
            while (pad == 0) { pad = random_bytes(1)[0]; }
            ba[--n] = pad;
        }
        ba[--n] = 2;
        ba[--n] = 0;

        var hex = "";
        for (i = 0; i < ba.length; i++)
        {
            hex += hex_chars.charAt((ba[i] >> 4) & 15) + hex_chars.charAt(ba[i] & 15);
        }
        return BigInt("0x" + hex);
    }

    /**
     * @brief the inverse of pkcs1_pad: checks the marker and the padding and decodes the utf8 text
     *
     * @param bigint value -> the decrypted block
     * @param number n -> the block length in bytes
     *
     * @return string|null the text, null when the padding does not check out
     */
    function pkcs1_unpad(value, n)
    {
        var b = bigint_to_bytes(value);
        var i = 0;
        var ret = "";

        while (i < b.length && b[i] == 0) { ++i; }
        if (b.length - i != n - 1 || b[i] != 2) { return null; }

        ++i;
        while (b[i] != 0)
        {
            if (++i >= b.length) { return null; }
        }

        while (++i < b.length)
        {
            var c = b[i] & 255;

            if (c < 128)
            {
                ret += String.fromCharCode(c);
            }
            else if ((c > 191) && (c < 224))
            {
                ret += String.fromCharCode(((c & 31) << 6) | (b[i + 1] & 63));
                ++i;
            }
            else
            {
                ret += String.fromCharCode(((c & 15) << 12) | ((b[i + 1] & 63) << 6) | (b[i + 2] & 63));
                i += 2;
            }
        }
        return ret;
    }

    // the key object: native BigInt fields under the names the old RSAKey used, so anything
    // holding a key keeps working

    /**
     * @brief an rsa key: n and e, and for a private key d, p, q, dmp1, dmq1 and coeff, all BigInts
     *
     * @return void a constructor, used with new
     */
    function RSAKey()
    {
        this.n = null;
        this.e = 0;
        this.d = null;
        this.p = null;
        this.q = null;
        this.dmp1 = null;
        this.dmq1 = null;
        this.coeff = null;
    }

    /**
     * @brief loads the public half from hex; throws when either part is missing
     *
     * @param string N -> the modulus in hex
     * @param string E -> the exponent in hex
     *
     * @return void
     */
    RSAKey.prototype.setPublic = function (N, E)
    {
        if (N == null || E == null || N.length == 0 || E.length == 0) { throw new Error("Invalid RSA public key"); }
        this.n = bigint_from_hex(N);
        this.e = BigInt(parseInt(E, 16));
    };

    /**
     * @brief loads a whole private key from hex; throws when the public part is missing
     *
     * @param string N -> the modulus
     * @param string E -> the public exponent
     * @param string D -> the private exponent
     * @param string P -> the first prime
     * @param string Q -> the second prime
     * @param string DP -> d mod (p-1)
     * @param string DQ -> d mod (q-1)
     * @param string C -> the crt coefficient
     *
     * @return void
     */
    RSAKey.prototype.setPrivateEx = function (N, E, D, P, Q, DP, DQ, C)
    {
        if (N == null || E == null || N.length == 0 || E.length == 0) { throw new Error("Invalid RSA private key"); }
        this.n = bigint_from_hex(N);
        this.e = BigInt(parseInt(E, 16));
        this.d = bigint_from_hex(D);
        this.p = bigint_from_hex(P);
        this.q = bigint_from_hex(Q);
        this.dmp1 = bigint_from_hex(DP);
        this.dmq1 = bigint_from_hex(DQ);
        this.coeff = bigint_from_hex(C);
    };

    /**
     * @brief x^e mod n, on the wasm
     *
     * @param bigint x -> the block
     *
     * @return bigint the result
     */
    RSAKey.prototype.doPublic = function (x)
    {
        return modpow_wasm(x, this.e, this.n);
    };

    /**
     * @brief x^d mod n, by the crt combination rsa.js did when p and q are known, both exponentiations on the wasm
     *
     * @param bigint x -> the block
     *
     * @return bigint the result
     */
    RSAKey.prototype.doPrivate = function (x)
    {
        if (this.p == null || this.q == null)
        {
            return modpow_wasm(x, this.d, this.n);
        }

        // the same crt combine rsa.js did, with both exponentiations on the wasm
        var xp = modpow_wasm(x % this.p, this.dmp1, this.p);
        var xq = modpow_wasm(x % this.q, this.dmq1, this.q);

        while (xp < xq) { xp = xp + this.p; }
        return (((xp - xq) * this.coeff) % this.p) * this.q + xq;
    };

    /**
     * @brief pkcs#1 encryption of a text with the public half
     *
     * @param string text -> the plaintext
     *
     * @return string the ciphertext as an even-length hex string
     */
    RSAKey.prototype.encrypt = function (text)
    {
        var m = pkcs1_pad(text, (bit_length(this.n) + 7) >> 3);
        var c = this.doPublic(m);
        var h = c.toString(16);

        if ((h.length & 1) == 0) { return h; }
        return "0" + h;
    };

    /**
     * @brief undoes RSAKey.prototype.encrypt with the private half
     *
     * @param string ctext -> the ciphertext in hex
     *
     * @return string|null the plaintext, null when the padding does not check out
     */
    RSAKey.prototype.decrypt = function (ctext)
    {
        var c;

        try { c = bigint_from_hex(ctext); }
        catch (err) { return null; }

        var m = this.doPrivate(c);
        return pkcs1_unpad(m, (bit_length(this.n) + 7) >> 3);
    };

    my.RSAKey = RSAKey;

    // -----------------------------------------------------------------------
    // aes-cbc over aes-js, in cryptico's layout: the iv is block 0 of the output
    // -----------------------------------------------------------------------

    /**
     * @brief aes-cbc under a random iv, the iv prepended, as base64
     *
     * @param string plaintext -> the text
     * @param array key -> the aes key bytes
     *
     * @return string the base64 ciphertext
     */
    my.encryptAESCBC = function (plaintext, key)
    {
        var blocks = my.pad16(my.string2bytes(plaintext));
        var iv = my.blockIV();
        var cbc = new aesjs.ModeOfOperation.cbc(key, iv);
        var encrypted = Array.prototype.slice.call(cbc.encrypt(blocks));

        return my.b256to64(my.bytes2string(iv.concat(encrypted)));
    };

    /**
     * @brief the inverse of encryptAESCBC; a partial trailing block is cut, since aes-js rejects it
     *
     * @param string encrypted_text -> the base64 ciphertext, iv first
     * @param array key -> the aes key bytes
     *
     * @return string the plaintext, "" when the input is too short
     */
    my.decryptAESCBC = function (encrypted_text, key)
    {
        var bytes = my.string2bytes(my.b64to256(encrypted_text));
        var iv = bytes.slice(0, 16);
        var body = bytes.slice(16);

        // aes-js rejects a partial trailing block; the old code fed it garbage instead
        body = body.slice(0, body.length - (body.length % 16));
        if (iv.length < 16 || body.length == 0) { return ""; }

        var cbc = new aesjs.ModeOfOperation.cbc(key, iv);
        var decrypted = Array.prototype.slice.call(cbc.decrypt(body));

        return my.bytes2string(my.depad(decrypted));
    };

    // -----------------------------------------------------------------------
    // keys and the hybrid envelope
    // -----------------------------------------------------------------------

    /**
     * @brief the identity keypair: the seeded prime walk in the wasm, which reproduces the old jsbn walk bit for bit (see client-webassemblies/rsa-keygen)
     *
     * @param string passphrase -> the seed, hashed with sha256
     * @param number bitlength -> the modulus size
     *
     * @return RSAKey the keypair
     */
    my.generateRSAKey = function (passphrase, bitlength)
    {
        var wasm = rsa_crypto__get_rsa_keygen_wasm();
        var seed_hex = sha256.hex(passphrase);
        var memory_bytes = new Uint8Array(wasm.memory.buffer);
        var seed_ptr = wasm.rsa_keygen__get_seed_buffer();
        var i, f, b;

        for (i = 0; i < seed_hex.length; i++)
        {
            memory_bytes[seed_ptr + i] = seed_hex.charCodeAt(i);
        }

        if (wasm.rsa_keygen__generate(bitlength, seed_hex.length) != 1)
        {
            throw new Error("rsa_keygen__generate rejected bits=" + bitlength);
        }

        // results are minimal hex in the order n, e, d, p, q, dmp1, dmq1, coeff
        memory_bytes = new Uint8Array(wasm.memory.buffer);
        var fields = [];
        for (f = 0; f < 8; f++)
        {
            var result_ptr = wasm.rsa_keygen__get_result(f);
            var hex = "";
            for (b = result_ptr; memory_bytes[b] != 0; b++)
            {
                hex += String.fromCharCode(memory_bytes[b]);
            }
            fields.push(hex);
        }

        var key = new RSAKey();
        key.setPrivateEx(fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6], fields[7]);
        return key;
    };

    /**
     * @brief the string form of a public key: the modulus as base64
     *
     * @param RSAKey rsakey -> the key
     *
     * @return string the public key string
     */
    my.publicKeyString = function (rsakey)
    {
        return my.b16to64(rsakey.n.toString(16));
    };

    /**
     * @brief a public key from its string form (the modulus in base64, anything after a "|" ignored), exponent 3
     *
     * @param string string -> the public key string
     *
     * @return RSAKey the key
     */
    my.publicKeyFromString = function (string)
    {
        var N = my.b64to16(string.split("|")[0]);
        var rsa = new RSAKey();

        rsa.setPublic(N, "03");
        return rsa;
    };

    /**
     * @brief hybrid encryption for a receiver: a fresh aes key rsa-encrypted with their public key, "?", then the aes-cbc ciphertext
     *
     * @param string plaintext -> the text
     * @param string publickeystring -> the receiver's public key string
     *
     * @return object { status: "success", cipher } or { status: "Invalid public key" }
     */
    my.encrypt = function (plaintext, publickeystring)
    {
        var cipherblock = "";
        var aeskey = my.generateAESKey();

        try
        {
            var publickey = my.publicKeyFromString(publickeystring);
            cipherblock += my.b16to64(publickey.encrypt(my.bytes2string(aeskey))) + "?";
        }
        catch (err)
        {
            return { status: "Invalid public key" };
        }

        cipherblock += my.encryptAESCBC(plaintext, aeskey);
        return { status: "success", cipher: cipherblock };
    };

    /**
     * @brief the inverse of lemon_crypto.encrypt with our private key
     *        lemon-chat never signs; a signed payload from another cryptico client still yields its
     *        plaintext, it just is not verified here
     *
     * @param string ciphertext -> the hybrid ciphertext
     * @param RSAKey key -> our private key
     *
     * @return object { status: "success", plaintext, signature } or { status: "failure" } when the aes key does not decrypt
     */
    my.decrypt = function (ciphertext, key)
    {
        var cipherblock = ciphertext.split("?");
        var aeskey = key.decrypt(my.b64to16(cipherblock[0]));

        if (aeskey == null) { return { status: "failure" }; }

        aeskey = my.string2bytes(aeskey);

        // lemon-chat never signs; a signed payload from another cryptico client still
        // yields its plaintext, it just is not verified here
        var plaintext = my.decryptAESCBC(cipherblock[1], aeskey).split(signature_separator);

        return {
            status: "success",
            plaintext: plaintext[0],
            signature: (plaintext.length == 3) ? "unverified" : "unsigned"
        };
    };

    /**
     * @brief the server's rsa challenge: pkcs#1 ciphertext, base64, no aes layer
     *
     * @param string ciphertext -> the challenge in base64
     * @param RSAKey key -> our private key
     *
     * @return string|null the plaintext, null when the padding does not check out
     */
    my.challenge_decrypt = function (ciphertext, key)
    {
        return key.decrypt(my.b64to16(ciphertext));
    };

    return my;

}());

// in for ONE function: custom_typeof (audio-opus-glue.js:77), which messages.js calls when clamping
// a chat message's font size. it is a general helper that happens to live in the audio file. the
// opus classes around it are only constructed from the worker entry points, which never run here
// audio-opus-glue.js is embedded in template.html along with the other client files
// it is the opus codec side of audio: it turns microphone sound into network packets and packets back
// into sound, and runs inside the opus encoder and decoder workers (worker-entry.js picks the handler)
// audio.js and dispatch.js post to these workers, voice.js sends what they encode
// in this file a "frame" is one opus packet; in audio.js it is one moment of sound

// state private to this file
var encoder = null;


// OPUS AUDIO TUNABLES: each value below is safe to change on its own; numbers elsewhere in this file are load-bearing
// (decoder channel count 2 with stereo interleaving assumed downstream, sample rate 48000, OPUS_CTL_RESET_STATE 4028).
// the jitter buffer knobs under "AUDIO WORKLET TUNABLES" in audio.js matter more for felt delay than anything here

// how many people can be heard talking at once: one opus decoder per concurrent speaker (opus is stateful,
// streams cannot share one). all are allocated at worker start and never freed; a released decoder goes back
// to the pool. this is speakers-at-once, not channel size; a handful of simultaneous voices is already a mush
var OPUS_DECODER_POOL_SIZE = 16;

// microphone encode frame length. opus allows 2.5 / 5 / 10 / 20 / 40 / 60 ms.
// larger = fewer packets and better compression, but more latency and more
// audio lost per dropped packet. MUST be <= OPUS_DECODER_FRAME_CAPACITY_MS
var OPUS_ENCODER_FRAME_DURATION_MS = 40;

// opus encoder mode: 2048 = OPUS_APPLICATION_VOIP (tuned for speech),
// 2049 = OPUS_APPLICATION_AUDIO (music), 2051 = RESTRICTED_LOWDELAY
var OPUS_ENCODER_APPLICATION = 2048;

// decode output buffer size in ms of audio, per decoder: must hold the largest frame any sender might send
// (voice frames above, plus the server music bot's). 60 ms is the opus maximum, so it always fits; only lower
// this if you are sure no larger frames ever arrive
var OPUS_DECODER_FRAME_CAPACITY_MS = 60;

// --- packet loss concealment (PLC): when frames go missing, libopus can fabricate plausible fill audio
// from the decoder's state instead of leaving a gap or click. active only on the low-latency worklet path
// (the fallback mixer below never conceals)

// max fill frames invented per gap. concealment decays fast: 1-2 is
// transparent, 3+ starts to smear / sound robotic. set 0 to disable PLC
var OPUS_PLC_MAX_CONCEAL_FRAMES = 2;

// a gap wider than this many frames is treated as a dropout/reconnect rather
// than packet loss: the stream restarts clean instead of being prefixed with
// stale invented audio
var OPUS_PLC_MAX_GAP_TO_CONCEAL = 25;

// every talk-spurt start (press, song start) jumps the outgoing sequence by this much, and receivers scrub that
// sender's decoder at half this delta. must be well above any real in-mapping loss run (~160 frames before idle
// release) and under 32768 for the 16-bit wrap arithmetic; the sender's encoder is reset at the same boundary
var G_OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP = 1000;

// --- fallback mixer (only when AudioWorklet is unavailable) ---------------
// used on insecure contexts / old browsers. the worklet path ignores all of
// this and mixes on the audio clock instead.

// how often the fallback mixer wakes to decode+mix one round. raise toward
// 40-60 ms to save cpu on weak devices at the cost of latency; going much
// past ~60 ms makes playback choppy
var OPUS_TICK_INTERVAL_MS = 20;

// a sender silent this long has its decoder returned to the pool for reuse
var OPUS_DECODER_IDLE_RELEASE_SECONDS = 5;

// consecutive out-of-order "late" frames before we decide the sender
// restarted its sequence counter (a reconnect) and resync to it
var OPUS_STALE_FRAMES_BEFORE_RESYNC = 25;

// ---- derived from the knobs above; do not edit these directly ----
var OPUS_DECODER_IDLE_TICKS_BEFORE_RELEASE = Math.round(OPUS_DECODER_IDLE_RELEASE_SECONDS * 1000 / OPUS_TICK_INTERVAL_MS);
var OPUS_HOUSEKEEPING_TICKS_PER_SECOND = Math.round(1000 / OPUS_TICK_INTERVAL_MS);

// ---- fixed opus ABI constant, NOT a tunable ----
var OPUS_CTL_RESET_STATE = 4028; // OPUS_RESET_STATE from opus_defines.h


var custom_typeof = (function (global)
{
    var cache = {};
    return function (obj)
    {
        var key;
        return obj === null ? 'null' // null
            : obj === global ? 'global' // window in browser or global in nodejs
                : (key = typeof obj) !== 'object' ? key // basic: string, boolean, number, undefined, function
                    : obj.nodeType ? 'object' // DOM element
                        : cache[key = ({}).toString.call(obj)] // cached. date, regexp, error, object, array, math
                        || (cache[key] = key.slice(8, -1).toLowerCase()); // get XXXX from [object XXXX], and cache it
    };
}(this));



/**
 * @brief an opus encoder on the wasm: the encoder state, a resampler when the microphone rate is not the opus rate, and the heap views the encode paths write into
 *
 * @param number application -> the opus application mode (2048 voip, 2049 audio, 2051 restricted lowdelay)
 * @param number frameDuration -> the frame length in ms
 * @param number sampleRate -> the opus rate, 48000
 * @param number originalRate -> the microphone's rate; a resampler is built when it differs
 * @param number channels -> channels of the input
 * @param object params -> unused
 *
 * @return void a constructor, used with new
 */
function OpusEncoder(application, frameDuration, sampleRate, originalRate, channels, params)
{
    var err;
    var bufSize;
    var outSize;

    this.originalRate = originalRate;
    this.resampler = null;
    this.resampler_44100kHz_to_48000kHz = null;
    
    this.sampleRate = sampleRate;
    this.bufPos = 0;
    err = Module._malloc(4);

    this.frameSize = sampleRate * frameDuration / 1000;
    this.channels = channels;

    // sample rate must be one of 8000, 12000, 16000, 24000, or 48000.

    this.handle = _opus_encoder_create(sampleRate, channels, application, err);

    if (this.handle == 0)
    {
        throw new Error('_opus_encoder_create fail ');
        return;
    }
    
    if (sampleRate != originalRate)
    {
        try
        {
            console.log("originalRate" + originalRate);
            console.log("sampleRate" + sampleRate);

            this.resampler = new SpeexResampler(channels, originalRate, sampleRate);
        }
        catch (e)
        {
            console.log("encoder new SpeexResampler(channels, originalRate, sampleRate) error");
            return;
        }
    }

    this.resampler_44100kHz_to_48000kHz = new SpeexResampler(channels, 44100, 48000);

    Module._free(err);
    bufSize = 4 * this.frameSize * this.channels;
    this.bufPtr = Module._malloc(bufSize);
    this.buf = Module.HEAPF32.subarray(this.bufPtr / 4, (this.bufPtr + bufSize) / 4);
    outSize = 1275 * 3 + 7;
    this.outPtr = Module._malloc(outSize);
    this.out = Module.HEAPU8.subarray(this.outPtr, this.outPtr + outSize);
}

/**
 * @brief re-derives the cached heap views when the wasm heap grew
 *        SpeexResampler.process reallocates its io buffers whenever a bigger chunk than any before
 *        arrives, which can trigger exactly that mid-run. called at the top of both encode paths;
 *        a no-op while the cached buffer still matches the live heap
 *
 * @return void
 */
OpusEncoder.prototype.refresh_heap_views_if_detached = function ()
{
    if (this.buf.buffer === Module.HEAPF32.buffer)
    {
        return;
    }

    this.buf = Module.HEAPF32.subarray(this.bufPtr / 4, (this.bufPtr + 4 * this.frameSize * this.channels) / 4);
    this.out = Module.HEAPU8.subarray(this.outPtr, this.outPtr + (1275 * 3 + 7));
}

/**
 * @brief invalidates the audio still in the buffer and rebuilds the resampler
 *        added because leftover data remained after a push-to-talk; experimental, did not help
 *
 * @return void
 */
OpusEncoder.prototype.reset = function ()
{
    
    if (this.buf && this.buf.length > 0)
    {
        this.buf.fill(0.0);
    }
    
    this.bufPos = 0;

    console.log("opus encoder reset");


    if (this.resampler)
    {
        let old_resampler = this.resampler;
        this.resampler = new SpeexResampler(this.channels, this.originalRate, this.sampleRate);
        old_resampler.destroy();
        console.log("old resampler destroyed");
    }

    if (this.resampler_44100kHz_to_48000kHz)
    {
        let old_resampler = this.resampler_44100kHz_to_48000kHz;
        this.resampler_44100kHz_to_48000kHz = new SpeexResampler(this.channels, 44100, 48000);
        old_resampler.destroy();
        console.log("old resampler destroyed");
    }

}

/**
 * @brief encodes microphone samples: resamples when needed, fills whole frames and encodes each completed one
 *        whether resampling is needed is known at construction, from the microphone rate; done
 *        wrong, a voice comes out too high or too low
 *
 * @param Float32Array samples -> mono pcm at the microphone rate
 *
 * @return array|undefined the opus packets completed by these samples (Uint8Array each), undefined when the resampler failed
 */
OpusEncoder.prototype.encode = function (samples)
{
    // if resample is needed or not, is known right at the beginning, based on clients microphone
    // so only one resampler is used here, compared to encode_mp3_chunk function

    // if resampling is not done right, persons voice will be either high pitched, or too low

    var size;
    var ret;
    var result;
    var packets = [];

    if (this.resampler)
    {
        try
        {
            samples = this.resampler.process(samples);
        } catch (e)
        {
            console.log(e);
            return;
        }
    }

    this.refresh_heap_views_if_detached();

    while (samples && samples.length > 0)
    {
        size = Math.min(samples.length, this.buf.length - this.bufPos);
        this.buf.set(samples.subarray(0, size), this.bufPos);
        this.bufPos += size;
        samples = samples.subarray(size);
        if (this.bufPos == this.buf.length)
        {
            this.bufPos = 0;

            ret = _opus_encode_float(this.handle, this.bufPtr, this.frameSize, this.outPtr, this.out.byteLength);
            if (ret < 0)
            {
                console.log("encoder error");
                return;
            }
            result = (new Uint8Array(this.out.subarray(0, ret))).buffer;
            packets.push(result);
        }
    }
    if (packets.length > 0)
    {
        return packets;
    }
}

/**
 * @brief encodes decoded mp3 samples, resampling 44100 Hz material to 48 kHz first, since every mp3 has its own rate
 *
 * @param Float32Array samples -> pcm from the mp3 decoder
 * @param number input_pcm_sample_rate -> the mp3's rate
 *
 * @return array|undefined the opus packets completed by these samples, undefined when the resampler failed
 */
OpusEncoder.prototype.encode_mp3_chunk = function (samples, input_pcm_sample_rate)
{
    // goal of this function is to make sure PCM is converted to 48kHz if it needs to be
    // problem is, each mp3 file has different sample rate, some are 44.1kHz, some are 48

    var size;
    var ret;
    var result;
    var packets = [];

    if (input_pcm_sample_rate == 44100)
    {
        try
        {
            samples = this.resampler_44100kHz_to_48000kHz.process(samples);
        } catch (e)
        {
            console.log(e);
            return;
        }
    }

    this.refresh_heap_views_if_detached();

    while (samples && samples.length > 0)
    {
        size = Math.min(samples.length, this.buf.length - this.bufPos);
        this.buf.set(samples.subarray(0, size), this.bufPos);
        this.bufPos += size;
        samples = samples.subarray(size);
        if (this.bufPos == this.buf.length)
        {
            this.bufPos = 0;

            ret = _opus_encode_float(this.handle, this.bufPtr, this.frameSize, this.outPtr, this.out.byteLength);
            if (ret < 0)
            {
                console.log("encoder error");
                return;
            }
            result = (new Uint8Array(this.out.subarray(0, ret))).buffer;
            packets.push(result);
        }
    }
    if (packets.length > 0)
    {
        return packets;
    }
}

/**
 * @brief frees the wasm encoder state and the resamplers
 *
 * @return void
 */
OpusEncoder.prototype.destroy = function ()
{
    _opus_encoder_destroy(this.handle);
    if (this.resampler)
    {
        this.resampler.destroy();
    }

    if (this.resampler_44100kHz_to_48000kHz)
    {
        this.resampler_44100kHz_to_48000kHz.destroy();
    }

    this.handle = null;
    this.buf = null;
}

/**
 * @brief an opus decoder on the wasm: the decoder state and the heap views one packet and one decoded frame live in
 *
 * @param number sampleRate -> the opus rate, 48000
 * @param number channels -> output channels, 2 for the interleaved stereo everything downstream expects
 *
 * @return void a constructor, used with new
 */
function OpusDecoder(sampleRate, channels)
{
    this.channels = channels;
    var err = Module._malloc(4);
    this.handle = _opus_decoder_create(sampleRate, this.channels, err);
    var errNum = Module.getValue(err, "i32");
    Module._free(err);
    if (errNum != 0)
    {
        console.error("opus decoder creation failed, error code " + errNum);
        return;
    }

    this.frameSize = sampleRate * OPUS_DECODER_FRAME_CAPACITY_MS / 1000;
    var bufSize = 1275 * 3 + 7;
    var pcmSamples = this.frameSize * this.channels;

    this.bufSize = bufSize;
    this.bufPtr = Module._malloc(bufSize);
    this.buf = Module.HEAPU8.subarray(this.bufPtr, this.bufPtr + bufSize);


    this.pcmBufferSize = 4 * pcmSamples;

    // Module.HEAPF32.subarray creates a view for allocated buffer. Used when creating Float32Array later at some point
    this.pcmPtr = Module._malloc(this.pcmBufferSize);
    this.pcm = Module.HEAPF32.subarray(this.pcmPtr / 4, this.pcmPtr / 4 + pcmSamples);

    // samples per channel of the last successfully decoded real frame. packet loss
    // concealment must synthesize exactly one frame of the stream's real duration,
    // which is not known until the first frame of that stream has been decoded
    this.last_decoded_frame_size = 0;
}

/**
 * @brief re-derives the wasm-heap views from the current heap buffer
 *        the views cached at construction time detach if the emscripten heap grows during a later
 *        instance's allocation, so after building the whole decoder pool every member's views are
 *        refreshed once, when no further mallocs follow
 *
 * @return void
 */
OpusDecoder.prototype.refresh_heap_views = function()
{
    this.buf = Module.HEAPU8.subarray(this.bufPtr, this.bufPtr + this.bufSize);
    this.pcm = Module.HEAPF32.subarray(this.pcmPtr / 4, this.pcmPtr / 4 + this.frameSize * this.channels);
}

/**
 * @brief decodes one opus packet into this.pcm
 *        the payload is the audio bytes decrypted with the maintainer's channel key on this end;
 *        an oversize packet (beyond a valid opus packet's 1275*3+7 bytes) is refused, because it
 *        would throw a RangeError and take the whole decoder worker down
 *
 * @param ArrayBuffer payload -> the opus packet
 *
 * @return number samples per channel produced, negative on an opus error, -1 for an oversize packet
 */
OpusDecoder.prototype.decode = function (payload)
{
    // payload = audio bytes decrypted with the maintainer's channel key on this end. stock libopus signature
    // opus_decode_float(state, data, len, pcm_out, frame_size, decode_fec): frame_size is the capacity of pcm_out
    // in samples per channel, the return value how many were produced; decoded pcm is read from this.pcm afterwards

    // a valid opus packet never exceeds this.bufSize (1275*3+7); anything bigger would
    // make the set() below throw a RangeError and take the whole decoder worker down
    if (payload.byteLength > this.bufSize)
    {
        return -1;
    }

    this.buf.set(new Uint8Array(payload));
    var ret = _opus_decode_float(this.handle, this.bufPtr, payload.byteLength, this.pcmPtr, this.frameSize, 0);

    if (ret > 0)
    {
        this.last_decoded_frame_size = ret;
    }

    return ret;
}

/**
 * @brief packet loss concealment: opus_decode_float with data=NULL and len=0 makes libopus synthesize one plausible frame from the predictor state instead of going silent
 *        possible once a real frame told us the stream's frame duration
 *
 * @return number samples per channel written into this.pcm, 0 when unable
 */
OpusDecoder.prototype.conceal_lost_frame = function ()
{
    if (this.last_decoded_frame_size <= 0)
    {
        return 0;
    }

    var ret = _opus_decode_float(this.handle, 0, 0, this.pcmPtr, this.last_decoded_frame_size, 0);

    if (ret < 0)
    {
        return 0;
    }

    return ret;
}

/**
 * @brief frees the wasm decoder state
 *
 * @return void
 */
OpusDecoder.prototype.destroy = function ()
{
    _opus_decoder_destroy(this.handle);
    this.handle = null;
    this.buf = null;
    this.pcm = null;
}

var IS_WORKER = !global.document && !!global.postMessage;
var IS_CURRENT_THREAD_WORKER = IS_WORKER && /blob:/i.test((global.location || {}).protocol);


/**
 * @brief creates one of the workers from this very page: the moduleFactory source is patched with the worker's name (THREAD_NAME) and loaded as a blob url
 *
 * @param string worker_name -> the name the worker runs under, "opus_decoder_worker" and the like
 *
 * @return Worker the new worker
 */
function audio_opus_glue__create_new_webworker_in_same_file(worker_name)
{
    console.log("trying to create webworker " + worker_name);
    let URL = global.URL || global.webkitURL || null;
    let code = moduleFactory.toString();

    // when creating new webworker within same file, get the string that represents code
    // alter the string, add variable to it with name of the worker

    let string_to_find = "var THREAD_NAME = "; // first offurence is this variable
    let first_occurence_index = code.indexOf(string_to_find);
    let replace_start_index = code.indexOf(string_to_find, first_occurence_index + 1);
    replace_start_index = replace_start_index + string_to_find.length;

    code = code.substring(0, replace_start_index) + "" + "\"" + worker_name + "" + code.substring(replace_start_index + worker_name.length, code.length);

    if (DBG_WORKER_BOOT_LOG) { console.log("[patcher] " + worker_name + " patched at " + replace_start_index); }


    let worker_url = URL.createObjectURL(new Blob(['(', code, ')();'], { type: 'text/javascript' }));
    let newly_created_worker = new global.Worker(worker_url);
    console.log("webworker created: " + worker_name);
    newly_created_worker.onmessage = dispatch__mainthread_onmessage;
    newly_created_worker.worker_name = worker_name;
    return newly_created_worker;
}


// dispatch__mainthread_onmessage (the main-thread worker-message dispatcher) lives in main.js


/**
 * @brief the opus encoder worker's message handler: "encode" and "encode_mp3_chunk" post the packets back, the rest manages the encoder (create, clear, destroy)
 *
 * @param object e -> the worker message event; e.data.value of "encode" must be a Float32Array
 *
 * @return void
 */
function audio_opus_glue__opus_encoder_worker_onmessage(e)
{
    if (e.data.type == "encode")
    {
        let opus_data_chunks = encoder.encode(e.data.value);

        // no full frame completed: nothing to send, and posting undefined
        // made the main thread throw on .length
        if (opus_data_chunks == null)
        {
            return;
        }

        global.postMessage({
            type: "opus_encoder_worker__encode_result",
            value: opus_data_chunks
        });
    }
    else if (e.data.type == "clear_opus_encoder_buffer")
    {
        // drops the half-filled frame (it used to survive across push-to-talk sessions
        // and replay old speech on the next press)
        if (encoder != null)
        {
            encoder.bufPos = 0;

            // the mic-path resampler keeps a few ms of filter history; rebuild it so no
            // old samples bleed into the next talk (reset_mem is not exported from wasm)
            if (encoder.resampler != null)
            {
                let old_resampler = encoder.resampler;
                encoder.resampler = new SpeexResampler(encoder.channels, encoder.originalRate, encoder.sampleRate);
                old_resampler.destroy();
            }

            // codec predictor reset - one half of a pair with the receivers' spurt-start scrub
            // (triggered by the sequence jump); unpaired, spurt heads decode as a loud squelch
            try { _opus_encoder_ctl(encoder.handle, OPUS_CTL_RESET_STATE); } catch (reset_err) { console.log("encoder reset_state failed: " + reset_err); }
        }
    }
    else if (e.data.type == "encode_mp3_chunk")
    {
        let opus_data_chunks = encoder.encode_mp3_chunk(e.data.value, e.data.mp3_sample_rate);

        // same guard as "encode": an incomplete frame returns nothing
        if (opus_data_chunks == null)
        {
            return;
        }

        global.postMessage({
            type: "opus_encoder_worker__encode_result",
            value: opus_data_chunks
        });
    }
    else if (e.data.type == "init")
    {
        let encoder_application_use = OPUS_ENCODER_APPLICATION; // VOIP use (see TUNABLES)
        let encoder_frame_duration = OPUS_ENCODER_FRAME_DURATION_MS; // Opus allows 2.5, 5, 10, 20, 40, or 60 ms
        let encoder_channels = 1;
        let encoder_output_samplerate = 48000; // if unsupported sample rate is used, Encoder wont construct
        let encoder_original_samplerate = e.data.sampleRate;
        encoder = new OpusEncoder(encoder_application_use, encoder_frame_duration, encoder_output_samplerate, encoder_original_samplerate, encoder_channels, 0);
    }
    else if (e.data.type == "destruct")
    {
        encoder.destroy();
    }
}


var opus_decoder_worker_interval;
var opus_decoder_worker_clients_opus_data = [];
var opus_decoder_worker_clients_opus_data_backbuffer = [];
var opus_decoder_worker_clients_opus_data_count = 0; // im keeping count in separate int varaible for simplier access
var opus_decoder_worker_current_channel_opus_client_ids = [];
var opus_decoder_worker_current_channel_opus_client_ids_map = new Map();

// the per-sender decoder pool: opus is stateful, so each concurrently audible sender needs its own decoder
// (interleaved streams through one corrupt each other's predictor). allocated once at init (a mid-run malloc can
// grow the wasm heap and detach the cached views), never freed: a release returns the slot, OPUS_RESET_STATE scrubs it
var opus_decoder_pool = [];
var opus_decoder_pool_free_indices = [];
var opus_decoder_sender_map = new Map(); // sender client_id -> { pool_index, last_used_tick }
var opus_decoder_tick_counter = 0;
var opus_decoder_mix_scratch = null; // Float32Array(frameSize), created at init after the pool

// receive-side sequence telemetry (frames lost on the unordered/unreliable datachannel)
var opus_decoder_worker_lost_frame_count = 0;
var opus_decoder_worker_late_frame_count = 0;
var opus_decoder_worker_last_logged_lost_frame_count = 0;
var opus_decoder_worker_concealed_frame_count = 0;

// PLC knobs (OPUS_PLC_MAX_CONCEAL_FRAMES / OPUS_PLC_MAX_GAP_TO_CONCEAL) live in the TUNABLES block up top

// direct mode: one end of a MessageChannel whose other end sits inside the player worklet.
// while set, chunks are decoded on arrival and their pcm goes straight to the worklet
// (per sender, no mixing here) - no 20 ms tick, no main-thread hop. null = tick/fallback mode
var opus_decoder_direct_worklet_port = null;

/**
 * @brief the sender's decoder, claiming a pool slot on first use
 *        on pool exhaustion the longest-idle sender's slot is stolen (that one stream restarts
 *        cleanly on its next frame, everyone else stays untouched)
 *
 * @param number sender_client_id -> the sender
 *
 * @return OpusDecoder|null the decoder, null when the pool is empty
 */
function audio_opus_glue__opus_decoder_worker_get_decoder_for_sender(sender_client_id)
{
    let mapping = opus_decoder_sender_map.get(sender_client_id);

    if (mapping != null)
    {
        mapping.last_used_tick = opus_decoder_tick_counter;
        return opus_decoder_pool[mapping.pool_index];
    }

    let pool_index = -1;

    if (opus_decoder_pool_free_indices.length > 0)
    {
        pool_index = opus_decoder_pool_free_indices.pop();
    }
    else
    {
        let oldest_tick = Infinity;
        let oldest_sender = null;

        for (const [sender, m] of opus_decoder_sender_map)
        {
            if (m.last_used_tick < oldest_tick)
            {
                oldest_tick = m.last_used_tick;
                oldest_sender = sender;
            }
        }

        if (oldest_sender == null)
        {
            console.warn("opus decoder pool exhausted: no free/idle decoder for new sender");
            return null;
        }

        pool_index = opus_decoder_sender_map.get(oldest_sender).pool_index;
        opus_decoder_sender_map.delete(oldest_sender);
    }

    // scrub the previous occupant's predictor state in place - no malloc/free involved.
    // the frame-size hint dies with the state: concealment from a stranger's predictor
    // would synthesize the previous occupant's voice into the new sender's stream
    _opus_decoder_ctl(opus_decoder_pool[pool_index].handle, OPUS_CTL_RESET_STATE);
    opus_decoder_pool[pool_index].last_decoded_frame_size = 0;

    opus_decoder_sender_map.set(sender_client_id, {
        pool_index: pool_index,
        last_used_tick: opus_decoder_tick_counter,
        last_sequence_number: null,
        consecutive_stale_count: 0
    });
    return opus_decoder_pool[pool_index];
}

/**
 * @brief returns the slots of senders that have been silent for a while to the free list
 *        no state is touched here; the reset happens when the slot is reassigned
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_release_idle_decoders()
{
    for (const [sender, m] of opus_decoder_sender_map)
    {
        if (opus_decoder_tick_counter - m.last_used_tick > OPUS_DECODER_IDLE_TICKS_BEFORE_RELEASE)
        {
            opus_decoder_pool_free_indices.push(m.pool_index);
            opus_decoder_sender_map.delete(sender);
        }
    }
}


/**
 * @brief decodes one chunk with that sender's pooled decoder under the per-sender sequence rules: duplicate and late drop, loss concealment, resync after a counter restart
 *        both the direct pipe and the 20 ms tick mixer use it
 *
 * @param number sender_client_id -> the sender
 * @param object chunk_entry -> the opus bytes and their sequence number
 * @param boolean allow_plc -> true to patch concealed frames over a detected loss
 *
 * @return array|null fresh interleaved-stereo Float32Arrays (concealed frames before the real one), null when the chunk was dropped
 */
function audio_opus_glue__opus_decoder_worker_decode_sender_chunk(sender_client_id, chunk_entry, allow_plc)
{
    let sender_decoder = audio_opus_glue__opus_decoder_worker_get_decoder_for_sender(sender_client_id);
    let frames_to_conceal = 0;

    if (sender_decoder == null)
    {
        return null;
    }

    let sender_mapping = opus_decoder_sender_map.get(sender_client_id);

    // sequence handling: drop duplicates and late frames, conceal losses. a long run of
    // "late" frames means the sender restarted its counter (reconnect) - resync to it
    if (chunk_entry.sequence_number != null && sender_mapping.last_sequence_number != null)
    {
        let sequence_delta = (chunk_entry.sequence_number - sender_mapping.last_sequence_number) & 0xffff;

        if (sequence_delta == 0)
        {
            return null; // duplicate frame
        }

        if (sequence_delta >= 0x8000)
        {
            opus_decoder_worker_late_frame_count = opus_decoder_worker_late_frame_count + 1;
            sender_mapping.consecutive_stale_count = sender_mapping.consecutive_stale_count + 1;

            // a run of stale frames (~0.5 s of audio): not reordering, the sender's
            // counter restarted - fall through and resync to it
            if (sender_mapping.consecutive_stale_count <= OPUS_STALE_FRAMES_BEFORE_RESYNC)
            {
                return null;
            }

            // a restarted counter means the sender's codec restarted too: scrub, or its
            // fresh stream decodes through the dead session's predictor state
            _opus_decoder_ctl(sender_decoder.handle, OPUS_CTL_RESET_STATE);
            sender_decoder.last_decoded_frame_size = 0;
        }
        else if (sequence_delta - 1 >= (G_OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP >> 1))
        {
            // the sender's deliberate spurt-start jump (its encoder was reset at the same
            // boundary): scrub so both predictors begin from zero. not loss, so not counted
            _opus_decoder_ctl(sender_decoder.handle, OPUS_CTL_RESET_STATE);
            sender_decoder.last_decoded_frame_size = 0;
        }
        else if (sequence_delta > 1)
        {
            let lost_now = sequence_delta - 1;

            if (lost_now > 250)
            {
                lost_now = 250; // keep the stat sane on an extreme burst
            }

            opus_decoder_worker_lost_frame_count = opus_decoder_worker_lost_frame_count + lost_now;

            // short gaps get concealed below, right before the real frame is decoded
            if (allow_plc && lost_now <= OPUS_PLC_MAX_GAP_TO_CONCEAL)
            {
                frames_to_conceal = Math.min(lost_now, OPUS_PLC_MAX_CONCEAL_FRAMES);
            }

            if (opus_decoder_worker_lost_frame_count - opus_decoder_worker_last_logged_lost_frame_count >= 100)
            {
                console.log("audio receive: " + opus_decoder_worker_lost_frame_count + " frames lost (" + opus_decoder_worker_concealed_frame_count + " concealed), " + opus_decoder_worker_late_frame_count + " late/duplicate so far");
                opus_decoder_worker_last_logged_lost_frame_count = opus_decoder_worker_lost_frame_count;
            }
        }
    }

    if (chunk_entry.sequence_number != null)
    {
        sender_mapping.last_sequence_number = chunk_entry.sequence_number;
        sender_mapping.consecutive_stale_count = 0;
    }

    let decoded_frames = [];

    // conceal BEFORE decoding the real frame: libopus extrapolates each synthetic frame
    // from the state as of the last good frame, and the real frame must be decoded after
    // them so the predictor continues in stream order
    for (let conceal_index = 0; conceal_index < frames_to_conceal; conceal_index++)
    {
        let concealed_sample_count = sender_decoder.conceal_lost_frame();

        if (concealed_sample_count <= 0)
        {
            break;
        }

        decoded_frames.push(new Float32Array(sender_decoder.pcm.subarray(0, concealed_sample_count * sender_decoder.channels)));
        opus_decoder_worker_concealed_frame_count = opus_decoder_worker_concealed_frame_count + 1;
    }

    let decoded_sample_count = sender_decoder.decode(chunk_entry.opus_chunk);

    if (decoded_sample_count <= 0)
    {
        if (decoded_frames.length == 0)
        {
            return null;
        }

        return decoded_frames;
    }

    // decode returns samples PER CHANNEL; the frame buffer holds interleaved stereo
    decoded_frames.push(new Float32Array(sender_decoder.pcm.subarray(0, decoded_sample_count * sender_decoder.channels)));
    return decoded_frames;
}

/**
 * @brief direct mode: decodes one sender's chunk immediately (no 20 ms tick latency) and transfers the pcm straight to the player worklet, which jitter-buffers per sender and mixes on the audio clock
 *        concealed frames ride the same pipe ahead of the real frame; to the worklet they are
 *        received audio filling the time the loss covered
 *
 * @param number sender_client_id -> the sender
 * @param object chunk_entry -> the opus bytes and their sequence number
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_decode_and_pipe(sender_client_id, chunk_entry)
{
    let decoded_frames = audio_opus_glue__opus_decoder_worker_decode_sender_chunk(sender_client_id, chunk_entry, true);

    if (decoded_frames == null)
    {
        return;
    }

    for (let frame_index = 0; frame_index < decoded_frames.length; frame_index++)
    {
        let decoded_pcm = decoded_frames[frame_index];

        opus_decoder_direct_worklet_port.postMessage({
            id: sender_client_id,
            pcm: decoded_pcm
        }, [decoded_pcm.buffer]);
    }
}

/**
 * @brief fallback (tick) mode: registers the sender for this 20 ms round and queues the chunk for the mixer
 *
 * @param number sender_client_id -> the sender
 * @param object chunk_entry -> the opus bytes and their sequence number
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_queue_chunk_for_tick(sender_client_id, chunk_entry)
{
    if (!opus_decoder_worker_current_channel_opus_client_ids.includes(sender_client_id))
    {
        opus_decoder_worker_current_channel_opus_client_ids.push(sender_client_id);
        opus_decoder_worker_current_channel_opus_client_ids.forEach((clientId, index) => {
            opus_decoder_worker_current_channel_opus_client_ids_map.set(clientId, index);
        });

        let new_client_index = opus_decoder_worker_current_channel_opus_client_ids_map.get(sender_client_id);
        opus_decoder_worker_clients_opus_data[new_client_index] = [];
    }

    let client_index = opus_decoder_worker_current_channel_opus_client_ids_map.get(sender_client_id);
    opus_decoder_worker_clients_opus_data[client_index].push(chunk_entry);
    opus_decoder_worker_clients_opus_data_count = opus_decoder_worker_clients_opus_data_count + 1;
}

/**
 * @brief direct-pipe mode housekeeping: the mixing tick is gone, but idle decoder slots still need returning to the pool
 *        the tick counter advances by one tick-interval's worth per second so the "1 tick =
 *        OPUS_TICK_INTERVAL_MS" units of last_used_tick / IDLE_TICKS_BEFORE_RELEASE stay valid
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_housekeeping_function()
{
    opus_decoder_tick_counter = opus_decoder_tick_counter + OPUS_HOUSEKEEPING_TICKS_PER_SECOND;
    audio_opus_glue__opus_decoder_worker_release_idle_decoders();
}

/**
 * @brief the fallback mixer's 20 ms tick: orders each sender's queued chunks by sequence number (wrap-aware), decodes them with that sender's decoder, sums and clamps the frames and posts one mixed block per time step
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_interval_function()
{
    opus_decoder_tick_counter = opus_decoder_tick_counter + 1;

    // cheap periodic sweep: hand slots of long-silent senders back to the pool
    if ((opus_decoder_tick_counter & 63) == 0)
    {
        audio_opus_glue__opus_decoder_worker_release_idle_decoders();
    }

    if (opus_decoder_worker_clients_opus_data_count == 0)
    {
        return;
    }

    // order each sender's chunks by sequence number (wrap-aware around 65536) so frames the
    // unordered datachannel delivered scrambled are decoded in capture order. legacy chunks
    // without a sequence number keep arrival order
    for (let sender_index = 0; sender_index < opus_decoder_worker_current_channel_opus_client_ids.length; sender_index++)
    {
        let sender_chunks = opus_decoder_worker_clients_opus_data[sender_index];

        if (sender_chunks.length > 1 && sender_chunks[0].sequence_number != null)
        {
            let anchor = sender_chunks[0].sequence_number;

            sender_chunks.sort(function(a, b)
            {
                return ((a.sequence_number - anchor) & 0xffff) - ((b.sequence_number - anchor) & 0xffff);
            });
        }
    }

    // finf longest Array of ArrayBuffers. Who has it? the longest array length will be n times
    // loop n times, for each client, check if he has said index too, if he has it, merge it,


    let longestLength = 0;

    // find longest array that holds most ArrayBuffers (opus chunks)
    for (let client_index = 0; client_index < opus_decoder_worker_current_channel_opus_client_ids.length; client_index++)
    {
        let current_length = opus_decoder_worker_clients_opus_data[client_index].length;
        if (longestLength < current_length)
        {
            longestLength = current_length;
        }
    }

    // per time step: decode each sender's chunk with that sender's decoder (predictor state stays per-stream),
    // sum the frames in js, clamp to [-1,1], post one mixed block, the message the main thread always consumed.
    // decode is called with clear=1, so the shared frame buffer holds exactly this call's frame before accumulating
    for (let ArrayBuffer_index = 0; ArrayBuffer_index < longestLength; ArrayBuffer_index++)
    {
        let mixed_sample_count = 0;

        opus_decoder_mix_scratch.fill(0);

        for (let client_index = 0; client_index < opus_decoder_worker_current_channel_opus_client_ids.length; client_index++)
        {
            // skip single_opus_chunk if client doesnt have element at that index
            if (ArrayBuffer_index >= opus_decoder_worker_clients_opus_data[client_index].length)
            {
                continue;
            }

            let sender_client_id = opus_decoder_worker_current_channel_opus_client_ids[client_index];
            let chunk_entry = opus_decoder_worker_clients_opus_data[client_index][ArrayBuffer_index];

            // sequence rules and per-sender decode live in the shared helper (the direct pipe's too); null means
            // the chunk was dropped. plc is off here: the tick mixer aligns frames by queue position, so extra
            // concealed frames would shift this sender against the others mid-round
            let decoded_frames = audio_opus_glue__opus_decoder_worker_decode_sender_chunk(sender_client_id, chunk_entry, false);

            if (decoded_frames == null)
            {
                continue;
            }

            let decoded_pcm = decoded_frames[0];

            for (let k = 0; k < decoded_pcm.length; k++)
            {
                opus_decoder_mix_scratch[k] = opus_decoder_mix_scratch[k] + decoded_pcm[k];
            }

            if (decoded_pcm.length > mixed_sample_count)
            {
                mixed_sample_count = decoded_pcm.length;
            }
        }

        if (mixed_sample_count == 0)
        {
            continue;
        }

        // clamp-copy: summed streams can overshoot [-1,1]; the copy is freshly allocated so its
        // buffer is transferred instead of structure-cloned on the worker -> main hop
        let pulse_code_modulation_bytes_for_webaudio = new Float32Array(mixed_sample_count);

        for (let k = 0; k < mixed_sample_count; k++)
        {
            let sample = opus_decoder_mix_scratch[k];
            pulse_code_modulation_bytes_for_webaudio[k] = sample > 1.0 ? 1.0 : (sample < -1.0 ? -1.0 : sample);
        }

        global.postMessage({
            type: "opus_decoder_worker__decode_result",
            value: pulse_code_modulation_bytes_for_webaudio
        }, [pulse_code_modulation_bytes_for_webaudio.buffer]);
    }

    opus_decoder_worker_clients_opus_data.length = 0;
    opus_decoder_worker_clients_opus_data_count = 0;
    opus_decoder_worker_current_channel_opus_client_ids.length = 0;
    opus_decoder_worker_current_channel_opus_client_ids_map.clear();
}

/**
 * @brief the opus decoder worker's message handler
 *        a voice frame ([4B sender id][encrypted 2B sequence + opus]) is decrypted with the channel
 *        keys and decoded, through the direct pipe or the tick queue; the other types manage the
 *        pool, the pipe port and the keys
 *
 * @param object event -> the worker message event; event.data.type picks the branch
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_onmessage(event)
{
    // voice frame: [4B sender client id][encrypted(2B sequence + opus)]
    if (event.data.type == "mainthread__add_data_to_opus_decoder")
    {
        let dataView = new DataView(event.data.value);

        // clientid is always first 4 bytes of received chunk of bytes and is in every chunk of bytes
        let extracted_client_id = dataView.getInt32(0, true);

        let opus_ArrayBuffer_encrypted = event.data.value.slice(4);
        let decrypted_bytes = keys__decrypt_data_with_aes_keys(g_current_channel_keys, opus_ArrayBuffer_encrypted);

        // after decryption: [2B sequence little endian][opus]. too-short frames are dropped
        if (decrypted_bytes == null || decrypted_bytes.length < 3)
        {
            return;
        }

        let chunk_entry = {
            sequence_number: decrypted_bytes[0] | (decrypted_bytes[1] << 8),
            opus_chunk: decrypted_bytes.subarray(2)
        };

        // direct mode decodes right now and pipes to the worklet; tick mode queues for the mixer
        if (opus_decoder_direct_worklet_port != null)
        {
            audio_opus_glue__opus_decoder_worker_decode_and_pipe(extracted_client_id, chunk_entry);
            return;
        }

        audio_opus_glue__opus_decoder_worker_queue_chunk_for_tick(extracted_client_id, chunk_entry);
    }
    else if (event.data.type == "mainthread__add_data_to_opus_decoder_musicbot")
    {
        let dataView = new DataView(event.data.value);
        let extracted_client_id = dataView.getInt32(0, true);
        let chunk_entry = null;

        if (extracted_client_id == -2)
        {
            // legacy format from an old server binary: [4B -2][opus], no sequence number
            chunk_entry = {
                sequence_number: null,
                opus_chunk: event.data.value.slice(4)
            };
        }
        else
        {
            // new format: [4B bot client id][2B sequence little endian][opus]
            if (event.data.value.byteLength < 7)
            {
                return;
            }

            chunk_entry = {
                sequence_number: dataView.getUint16(4, true),
                opus_chunk: event.data.value.slice(6)
            };
        }

        if (opus_decoder_direct_worklet_port != null)
        {
            audio_opus_glue__opus_decoder_worker_decode_and_pipe(extracted_client_id, chunk_entry);
            return;
        }

        audio_opus_glue__opus_decoder_worker_queue_chunk_for_tick(extracted_client_id, chunk_entry);
    }
    else if (event.data.type == "mainthread__channel_keys_for_opus_decoder")
    {
        g_current_channel_keys = event.data.value;
        // console.log("mainthread__channel_keys_for_opus_decoder got channel keys", current_channel_keys);
    }
    else if (event.data.type == "use_direct_worklet_pipe")
    {
        // the main thread transferred one end of a MessageChannel whose other end sits inside the
        // player worklet: from here on, decode on arrival and pipe per-sender pcm directly - the
        // mixing tick and the main-thread hop are gone. the interval becomes slow housekeeping
        opus_decoder_direct_worklet_port = event.data.port;

        clearInterval(opus_decoder_worker_interval);
        opus_decoder_worker_interval = setInterval(audio_opus_glue__opus_decoder_worker_housekeeping_function, 1000);

        // drop anything the tick queue still holds; the pipe owns playback from here
        opus_decoder_worker_clients_opus_data.length = 0;
        opus_decoder_worker_clients_opus_data_count = 0;
        opus_decoder_worker_current_channel_opus_client_ids.length = 0;
        opus_decoder_worker_current_channel_opus_client_ids_map.clear();

        console.log("opus decoder worker: direct worklet pipe active");
    }
    else if (event.data.type == "deep_idle_stop")
    {
        // android background idle: nothing plays, stop the periodic tick entirely
        clearInterval(opus_decoder_worker_interval);
        opus_decoder_worker_interval = null;
    }
    else if (event.data.type == "deep_idle_resume")
    {
        // restore whichever cadence matches the active pipeline
        // (worklet pipe -> slow housekeeping, fallback mixer -> fast tick)
        if (opus_decoder_worker_interval == null)
        {
            if (opus_decoder_direct_worklet_port != null)
            {
                opus_decoder_worker_interval = setInterval(audio_opus_glue__opus_decoder_worker_housekeeping_function, 1000);
            }
            else
            {
                opus_decoder_worker_interval = setInterval(audio_opus_glue__opus_decoder_worker_interval_function, OPUS_TICK_INTERVAL_MS);
            }
        }
    }
    else if (event.data.type == "init")
    {
        // fallback mixer tick (replaced by housekeeping once the worklet pipe activates)
        opus_decoder_worker_interval = setInterval(audio_opus_glue__opus_decoder_worker_interval_function, OPUS_TICK_INTERVAL_MS);

        // the whole per-sender decoder pool is allocated up front: every wasm malloc happens here, before any
        // audio flows, so the heap never grows mid-run under the cached views. decoders are stereo (a decoder's
        // channel count is independent of the packet's): stereo bot packets stay stereo, mono voice is duplicated by libopus
        for (let pool_index = 0; pool_index < OPUS_DECODER_POOL_SIZE; pool_index++)
        {
            opus_decoder_pool.push(new OpusDecoder(48000, 2));
            opus_decoder_pool_free_indices.push(pool_index);
        }

        // allocating a later pool member may have grown the heap and detached the views the
        // earlier members cached at construction - re-derive them all now that mallocs are done
        for (let pool_index = 0; pool_index < OPUS_DECODER_POOL_SIZE; pool_index++)
        {
            opus_decoder_pool[pool_index].refresh_heap_views();
        }

        opus_decoder_mix_scratch = new Float32Array(opus_decoder_pool[0].frameSize * opus_decoder_pool[0].channels);
    }
}


// messages.js is embedded in template.html along with the other client files, and in the node bundle
// it is the protocol layer: server_msg holds a handler for every server->client message, client_msg the
// builders for client->server requests; wire format details live here, not in the ui
// dispatch.js calls the handlers, ui.js and the feature files call the builders

// state private to this file
var received_files = []; // in chunks

var is_pressing = null;  // for touch devices

var is_long_press = null;

var server_msg = {
    /**
     * @brief the tag list: fills g_tags from the server snapshot, renders the settings tag table, and paints tag chips onto every visible client row (clients in hidden channels are skipped)
     *        the complete list arrives at every login, so the previous session's rows go first
     *
     * @param object msg -> the server message, msg.message.tags holds the tags
     *
     * @return void
     */
    process_tag_list_from_server: function(msg)
    {
        UI.wire_settings_delete_delegation();

        // the complete list arrives at every login, and the reconnect reset empties only g_tags,
        // so the rows of the previous session have to go before the new ones are appended
        document.getElementById("server-settings-tab-tags-container").innerHTML = "";

        for (let i = 0; i < msg.message.tags.length; i++)
        {
            let tag = msg.message.tags[i];
            g_tags.push(tag);

            let icon = tag.has_icon ? channel_tree__get_icon_by_icon_id(tag.tag_linked_icon_id) : null;

            let base64_icon = "";

            if (icon != null)
            {
                base64_icon = icon.base64_icon;
            }

            // the admin tag cannot be deleted, but its row still needs the column or the
            // cells above it shift out of line with the header
            let tag_delete_button_html = (tag.tag_id != 0)
                ? "<button class=\"settings-entry-delete-button\" title=\"delete tag\">✕</button>"
                : "<span class=\"settings-entry-delete-spacer\"></span>";
            let html_to_append = "<div class=\"server-settings-tag-entry\" data-tag-id=\""+tag.tag_id+"\">\n\
                                    <p class=\"tag-settings-entry-p\">"+tag.tag_id+"</p>\n\
                                    <p class=\"tag-settings-entry-p\">"+tag.tag_name+"</p>\n\
                                    <p class=\"tag-settings-entry-p\">"+tag.tag_linked_icon_id+"</p>\n\
                                    <div class=\"tag-settings-entry-img\" style=\"background-image: url("+base64_icon+");\"></div>\n\
                                    "+tag_delete_button_html+"\n\
                                </div>";

            document.getElementById("server-settings-tab-tags-container").insertAdjacentHTML("beforeend", html_to_append);
        }

        // add tags for clients in UI

        for (let i = 0; i < g_client_list.length; i++)
        {
            console.log("process_tag_list_from_server | processing client: " + g_client_list[i].username);
            for (let y = 0; y < g_client_list[i].tag_ids.length; y++)
            {
                console.log("process_tag_list_from_server | processing tag id : " + g_client_list[i].tag_ids[y]);

                let tag = channel_tree__get_tag_by_tag_id(g_client_list[i].tag_ids[y]);

                if (tag == null)
                {
                    console.log("tag is null");
                    continue;
                }

                let target_element = document.getElementById("client-tags-" + g_client_list[i].client_id);

                // clients in a hidden channel have no row, and throwing here aborted the loop
                // so every client after them lost their tags too
                if (target_element == null)
                {
                    continue;
                }

                const node = document.createElement("div");
                node.className = "single-tag";
                node.setAttribute("tag-id", tag.tag_id);

                let icon = tag.has_icon ? channel_tree__get_icon_by_icon_id(tag.tag_linked_icon_id) : null;

                if (icon != null)
                {
                    node.style.backgroundImage = "url("+icon.base64_icon+")";
                }

                target_element.appendChild(node);
            }
        }
    },
    /**
     * @brief the admin identity-management list: one row per stored identity
     *        abbreviated hash (full hash in title and on the controls' data-hash), last-seen username,
     *        online dot, its tags as chips each with a remove cross, an "add tag" dropdown, and a delete-identity cross
     *
     * @param object msg -> the server message, msg.message.identities holds the identities
     *
     * @return void
     */
    process_identity_list_from_server: function(msg)
    {
        let list = document.getElementById("server-settings-identities-list");
        if (list == null) { return; }

        let identities = (msg && msg.message && msg.message.identities) ? msg.message.identities : [];

        let tag_display_name = function(tag_id)
        {
            if (tag_id == 0) { return "admin"; }
            let tag = channel_tree__get_tag_by_tag_id(tag_id);
            return (tag != null && tag.tag_name) ? tag.tag_name : ("#" + tag_id);
        };

        let html = "";
        for (let i = 0; i < identities.length; i++)
        {
            let identity = identities[i];
            let hash = identity.public_key_hash != null ? identity.public_key_hash : "";
            let hash_attr = chat__sanitize_string(hash);
            let short_hash = hash.length > 18 ? (hash.substring(0, 18) + "…") : hash;
            let username = (identity.username != null && identity.username.length > 0) ? identity.username : "—";
            let tag_ids = identity.tag_ids != null ? identity.tag_ids : [];

            // tags as removable chips
            let chips = "";
            for (let t = 0; t < tag_ids.length; t++)
            {
                chips += "<span class=\"identity-tag-chip\">" + chat__sanitize_string(tag_display_name(tag_ids[t]))
                        + "<span class=\"identity-tag-remove\" data-identity-hash=\"" + hash_attr + "\" data-tag-id=\"" + tag_ids[t] + "\" title=\"remove this tag from the identity\">✕</span></span>";
            }

            // "add tag" dropdown: server tags this identity does not already hold (admin tag excluded)
            let add_options = "<option value=\"\">+ tag</option>";
            for (let t = 0; t < g_tags.length; t++)
            {
                if (g_tags[t].tag_id == 0) { continue; }
                if (tag_ids.indexOf(g_tags[t].tag_id) != -1) { continue; }
                add_options += "<option value=\"" + g_tags[t].tag_id + "\">" + chat__sanitize_string(g_tags[t].tag_name) + "</option>";
            }
            let add_select = "<select class=\"identity-tag-add\" data-identity-hash=\"" + hash_attr + "\" title=\"give this identity a tag\">" + add_options + "</select>";

            let online_dot = identity.is_online == true
                ? "<span title='online' style='color:#4caf50;'>●</span>"
                : "<span title='offline' style='color:#888;'>○</span>";

            // an identity is REGISTERED exactly when an admin gave it an alias - that is what
            // unlocks the offline people list and offline messages, so it is worth stating
            // plainly rather than leaving the admin to infer it from an empty cell
            let alias = (identity.alias != null) ? identity.alias : "";
            let is_registered = (alias.length > 0);

            let registered_cell = is_registered
                ? "<span title='has an alias: may list offline people and receive offline messages' style='color:#4caf50;'>yes</span>"
                : "<span title='no alias: not registered on this server' style='opacity:0.6;'>no</span>";

            // edit in place: type a name and press enter (or the ✓) to register, ✕ clears it
            let alias_cell = "<input class=\"identity-alias-input\" data-identity-hash=\"" + hash_attr + "\" value=\"" + chat__sanitize_string(alias) + "\" placeholder=\"no alias\" title=\"admin-registered display name; empty means not registered\">"
                            + "<button class=\"identity-alias-set-button\" data-identity-hash=\"" + hash_attr + "\" title=\"save this alias\">✓</button>"
                            + (is_registered ? "<button class=\"identity-alias-clear-button\" data-identity-hash=\"" + hash_attr + "\" title=\"clear the alias (unregisters this identity)\">✕</button>" : "");

            html += "<div class=\"identity-entry\">"
                    + "<p class=\"identity-entry-p\" title=\"" + hash_attr + "\">" + chat__sanitize_string(short_hash) + "</p>"
                    + "<p class=\"identity-entry-p\" title=\"" + chat__sanitize_string(username) + "\">" + chat__sanitize_string(username) + "</p>"
                    + "<p class=\"identity-entry-p identity-entry-p-online\">" + online_dot + "</p>"
                    + "<p class=\"identity-entry-p identity-entry-p-registered\">" + registered_cell + "</p>"
                    + "<div class=\"identity-entry-alias\">" + alias_cell + "</div>"
                    + "<div class=\"identity-entry-tags\">" + chips + add_select + "</div>"
                    + "<button class=\"identity-delete-button\" data-identity-hash=\"" + hash_attr + "\" title=\"delete this identity and strip all its tags\">✕</button>"
                    + "</div>";
        }

        if (identities.length == 0)
        {
            html = "<p style=\"font-size:11px; opacity:0.6;\">no stored identities yet</p>";
        }

        list.innerHTML = html;
    },
    /**
     * @brief the icon list: pushes each icon into g_icons, appends its settings entry, then repaints the channel icons that were rendered empty before the icons arrived
     *
     * @param object msg -> the server message, msg.message.icons holds the icons
     *
     * @return void
     */
    process_icon_list_from_server: function(msg)
    {
        UI.wire_settings_delete_delegation();

        // same as the tag list: the previous session's rows would otherwise stack up on every reconnect
        document.getElementById("server-settings-tab-icons-container").innerHTML = "";

        for (let i = 0; i < msg.message.icons.length; i++)
        {
            let icon = {
                id: msg.message.icons[i].icon_id,
                base64_icon: msg.message.icons[i].base64_icon
            }

            g_icons.push(icon);

            let html_to_append = "<div class='server-settings-icon-entry' data-icon-id="+icon.id+"><img class='img-uploaded-icon' src="+icon.base64_icon+"></img><button class='settings-entry-delete-button' title='delete icon'>✕</button></div>";

            document.getElementById("server-settings-tab-icons-container").insertAdjacentHTML("beforeend", html_to_append);
        }

        // g_channel_list arrives BEFORE this icon_list, so channel rows were rendered with empty icon
        // boxes; now that the icons exist, paint them
        UI.refresh_all_channel_icons();
    },
    /**
     * @brief the channel list, once per session: builds the whole channel tree html from the server snapshot, fills g_channel_list, wires the click and touch handlers, sets g_is_channel_list_retrieved
     *
     * @param object msg -> the server message, msg.message.channels holds the channels
     *
     * @return void
     */
    process_channel_list_from_server: function(msg)
    {
        if (g_is_channel_list_retrieved == true)
        {
            utils__custom_log("channel_list message received more than once. Server is doing something weird");
            return;
        }

        g_is_channel_list_retrieved = true;
        document.getElementById("channel-list-container").innerHTML = "";
        var root_channels = channel_tree__get_channels_by_channel_parent_id(msg.message.channels, g_ROOT_LEVEL_PARENT_SENTINEL);

        let processed_channels = [];
        let current_parent_channel_id_to_find = g_ROOT_LEVEL_PARENT_SENTINEL;
        let previous_parent_channel_id_to_find = g_ROOT_LEVEL_PARENT_SENTINEL;
        let channel_tree_build_stall_counter = 0;

        while (processed_channels.length != msg.message.channels.length)
        {
            let processed_count_before_iteration = processed_channels.length;

            if (current_parent_channel_id_to_find == previous_parent_channel_id_to_find)
            {
                current_parent_channel_id_to_find = g_ROOT_LEVEL_PARENT_SENTINEL;
            }
            previous_parent_channel_id_to_find = current_parent_channel_id_to_find;

            var child_channels = channel_tree__get_channels_by_channel_parent_id(msg.message.channels, current_parent_channel_id_to_find);

            for (let a = 0; a < child_channels.length; a++)
            {
                // first, check if current child channel is present in HTML. That is done by checking its presence in g_channel_list array.
                // if not, add current channel to g_channel_list and append channel to HTML

                let is_channel_added_to_html = channel_tree__get_channel_by_id(g_channel_list, child_channels[a].channel_id);

                if (is_channel_added_to_html == null)
                {
                    child_channels[a].is_channel_directly_collapsed = false;
                    g_channel_list.push(child_channels[a]);

                    let indentation_level = channel_tree__get_indentation_level(child_channels[a].channel_id, msg.message.channels);

                    let html_to_append = "";
                    let html_to_append_audio_disabled_class = (child_channels[a].is_audio_enabled == false) ? "single-channel-is-audio-disabled" : "";
                    let html_to_append_is_using_password_class = (child_channels[a].is_using_password == true) ? "single-channel-is-using-password" : "";
                    let html_to_append_is_temp_class = (child_channels[a].is_temp_channel == true) ? "single-channel-is-temp" : "";

                    if (child_channels[a].is_using_password == true)
                    {
                        console.log("channel " + child_channels[a].name + " is using password !!");

                        html_to_append = "<div class=\"single-channel\" data-channel-id=\"" + child_channels[a].channel_id + "\" data-channel-parent-id=\"" + child_channels[a].parent_channel_id + "\">\n\
                                            <div class=\"padding-div\" style=\"padding-left: " + indentation_level * 20 + "px;\"></div>\n\
                                            <div class=\"single-channel-collapse-button\">\n\
                                            </div>\n\
                                            <p class='single-channel-name-p "+html_to_append_is_using_password_class+" "+html_to_append_is_temp_class+"' data-channel-name-id=\"" + child_channels[a].channel_id + "\">" + chat__sanitize_string(child_channels[a].name) + "</p>\n\
                                            <div class=\"single-channel-icon\" data-channel-icon-for=\"" + child_channels[a].channel_id + "\"></div>\n\
                                            <p class=\"single-channel-unread-number unread-empty\" data-channel-unread-for=\"" + child_channels[a].channel_id + "\"></p>\n\
                                            <div class="+html_to_append_audio_disabled_class+"></div>\n\
                                        </div>";
                    }
                    else
                    {
                        console.log("channel " + child_channels[a].name + " is not using password !!");

                        html_to_append = "<div class=\"single-channel\" data-channel-id=\"" + child_channels[a].channel_id + "\" data-channel-parent-id=\"" + child_channels[a].parent_channel_id + "\">\n\
                                                <div class=\"padding-div\" style=\"padding-left: " + indentation_level * 20 + "px;\"></div>\n\
                                                <div class=\"single-channel-collapse-button\">\n\
                                                </div>\n\
                                                <p class='single-channel-name-p "+html_to_append_is_temp_class+"' data-channel-name-id=\"" + child_channels[a].channel_id + "\" >" + chat__sanitize_string(child_channels[a].name) + "</p>\n\
                                                <div class=\"single-channel-icon\" data-channel-icon-for=\"" + child_channels[a].channel_id + "\"></div>\n\
                                            <p class=\"single-channel-unread-number unread-empty\" data-channel-unread-for=\"" + child_channels[a].channel_id + "\"></p>\n\
                                                <div class="+html_to_append_audio_disabled_class+"></div>\n\
                                            </div>";
                    }

                    document.getElementById("channel-list-container").insertAdjacentHTML("beforeend", html_to_append);
                }

                // next, check if channel is already processed. If it is, check if for loop is at its end.
                // If for loop is at its end, and current channel is processed, that means all child channels of a parent channel were processed and that they have no more inner child channels in them

                let is_channel_already_processed = processed_channels.indexOf(child_channels[a].channel_id) != -1;

                if (is_channel_already_processed == true)
                {
                    if ((a + 1) == child_channels.length)
                    {
                        if (current_parent_channel_id_to_find == g_ROOT_LEVEL_PARENT_SENTINEL)
                        {
                            continue;
                        }

                        // all child channels in parent channel are processed.\
                        // add parent channel of child channels in this loop to processed_channels

                        processed_channels.push(current_parent_channel_id_to_find);
                    }
                }
                else
                {
                    // if channel is not processed, find out if the current channel in loop has any child channels. If it has, change current_parent_channel_id_to_find
                    // if not, push channel to processed_channel list

                    var children_of_child_channel = channel_tree__get_channels_by_channel_parent_id(msg.message.channels, child_channels[a].channel_id);
                    if (children_of_child_channel.length > 0)
                    {
                        current_parent_channel_id_to_find = child_channels[a].channel_id;
                        break;
                    }

                    processed_channels.push(child_channels[a].channel_id);
                }
            }

            // self-recovery guard: a healthy pass always processes at least one channel.
            // if an entire pass made no progress, the channel data is inconsistent
            // (missing root, orphaned child, or a cycle) - stop instead of freezing the UI.
            if (processed_channels.length == processed_count_before_iteration)
            {
                channel_tree_build_stall_counter++;
                if (channel_tree_build_stall_counter > msg.message.channels.length + 1)
                {
                    console.error("channel tree builder made no progress - received channel data is invalid. " + (msg.message.channels.length - processed_channels.length) + " channel(s) could not be placed; aborting render");
                    break;
                }
            }
            else
            {
                channel_tree_build_stall_counter = 0;
            }
        }

        let elements = document.getElementsByClassName('single-channel');

        // handle click events on channels differently on touch devices
        if (g_is_client_running_under_touch_device)
        {
            for (let i = 0; i < elements.length; i++)
            {
                let local_touch_press_timer = null;

                if (elements[i].classList.contains("idle-channel"))
                {
                    continue;
                }

                elements[i].addEventListener("touchstart", (event) => {

                    // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                    // by the time setTimeout runs, event object (or at least part of it) is lost,
                    // things from event object must be imediatelly stored in temp variable in this case, for later use

                    const currentTarget = event.currentTarget;
                    const clientY = event.touches[0].clientY;
                    const clientX = event.touches[0].clientX;

                    is_pressing = true;

                    local_touch_press_timer = window.setTimeout( () => {
                        if (is_pressing)
                        {
                            UI.single_channel_onmousedown(event, true, clientX, clientY, currentTarget);
                            is_pressing = false;
                        }
                    },
                    600, event);

                });

                elements[i].addEventListener("touchend", (event) => {
                    is_pressing = false;
                    clearTimeout(local_touch_press_timer);
                });

                elements[i].addEventListener("touchcancel", (event) => {
                    is_pressing = false;
                    clearTimeout(local_touch_press_timer);
                });

                elements[i].getElementsByClassName("single-channel-collapse-button")[0].addEventListener("mousedown", UI.collapse_expand_channel);
            }
        }
        else
        {
            for (let i = 0; i < elements.length; i++)
            {
                if (elements[i].classList.contains("idle-channel"))
                {
                    continue;
                }

                elements[i].addEventListener("mousedown", UI.single_channel_onmousedown);
                let selected_channel_id1 = parseInt(elements[i].getAttribute("data-channel-id"));
                console.log("selected_channel_id1 => " + selected_channel_id1);
                elements[i].querySelector('[data-channel-name-id="' + selected_channel_id1 + '"]').addEventListener("dblclick", UI.single_channel_doubleclick_join);
                elements[i].querySelector('[data-channel-name-id="' + selected_channel_id1 + '"]').addEventListener("click", UI.single_channel_onclick);
                elements[i].getElementsByClassName("single-channel-collapse-button")[0].addEventListener("mousedown", UI.collapse_expand_channel);
            }
        }

        // recompute chevron (leaf) visibility after the channel tree changed
        UI.refresh_all_channel_fullness();
    },
    /**
     * @brief a client stopped streaming a song: hides the marquee on their row; for the local client also sets g_stop_song_stream_message_received so the song stops being sent
     *
     * @param object msg -> the server message, msg.message.client_id names the client
     *
     * @return void
     */
    process_stop_song_stream_from_server: function(msg)
    {
        // start / stop song stream messages server pupose of handling marquee animation where text moves
        // microphone changes in gui are handles elsewhere

        // disable css marquee effect

        let element = document.querySelector('.marquee-music-playing-container[data-marquee-music-playing-container-id="' + msg.message.client_id + '"]');
        if (element != null)
        {
            element.style.display = "none";
            document.getElementById("marquee-song-name-client-id-" + msg.message.client_id).innerHTML = "";
        }
        else
        {
            console.log("could not find element");
        }

        // set g_stop_song_stream_message_received to true, so song is not sent anymore to server, this variable is then read elsewhere
        // this only applies to local client

        if (msg.message.client_id == g_local_client_id)
        {
            g_stop_song_stream_message_received = true;
        }
    },
    /**
     * @brief somebody connected: adds them to g_client_list (and the id map), renders their row with handlers, promotes their offline chat, re-keys the root channel if we maintain it, and requests their avatar
     *
     * @param object msg -> the server message, msg.message holds the client's fields
     *
     * @return void
     */
    process_client_connect_from_server: function(msg)
    {
        let username = chat__sanitize_string(msg.message.username);
        let client_id = msg.message.client_id;

        if (channel_tree__get_client_by_client_id(g_local_client_id).channel_id == msg.message.channel_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.user_entered_your_channel.play();
            }
        }
        let single_client = {
            client_id: null,
            username: null,
            alias: null,
            public_key: null,
            channel_id: null,
            audio_state: null,
            tag_ids: null,
            is_clients_channel_hidden: null,
            country_iso_code: null,
            is_idle: null,
            is_ignored_by_local_client: null,
            is_muted_by_local_client: null,
            unread_count: 0,
            is_music_bot: null
        };

        single_client.client_id = parseInt(client_id);
        single_client.username = username;
        single_client.alias = (msg.message.alias != null) ? msg.message.alias : "";
        single_client.public_key = msg.message.public_key;
        single_client.channel_id = msg.message.channel_id;
        single_client.tag_ids = (msg.message.tag_ids != null) ? msg.message.tag_ids : [];
        single_client.audio_state = msg.message.audio_state;
        single_client.is_clients_channel_hidden = false;
        single_client.country_iso_code = msg.message.country_iso_code;
        single_client.is_idle = false;
        single_client.is_ignored_by_local_client = false;
        single_client.is_muted_by_local_client = false;
        single_client.unread_count = 0;
        single_client.is_music_bot = msg.message.is_music_bot;

        g_client_list.push(single_client);
        g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);

        // an offline conversation with this person becomes their live private chat, history kept;
        // the merge is state and happens here, only the markup move is left to the ui
        let promotion = chat__promote_offline_chat_context_state(single_client);
        UI.promote_offline_chat_context_render(promotion, single_client);

        html_to_append = channel_tree__generate_html_for_single_client(single_client, false);

        channel_tree__get_channel_own_clients_last_element(msg.message.channel_id).insertAdjacentHTML("afterend", html_to_append);

        let elements = document.getElementsByClassName('connected-client');

        // local user uses diferent handler
        for (let i = 0; i < elements.length; i++)
        {

            if (g_is_client_running_under_touch_device)
            {

                let local_touch_press_timer = null; // for touch devices

                elements[i].addEventListener("touchstart", (event) => {

                    is_long_press = false;

                    // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                    // by the time setTimeout runs, event object (or at least part of it) is lost,
                    // things from event object must be imediatelly stored in temp variable in this case, for later use

                    const currentTarget = event.currentTarget;
                    const clientY = event.touches[0].clientY;
                    const clientX = event.touches[0].clientX;

                    local_touch_press_timer = window.setTimeout( () => {

                        is_long_press = true;
                        UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                    },
                    600, event);

                });

                elements[i].addEventListener("touchend", (event) => {

                    clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                    const currentTarget = event.currentTarget;
                    const clientY = event.changedTouches[0].pageY;
                    const clientX = event.changedTouches[0].pageX;

                    if (is_long_press == false)
                    {
                        UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget, true);
                    }
                });

            }
            else
            {
                elements[i].addEventListener("mousedown", UI.connected_user_onmousedown);
            }
            // elements[i].style.backgroundColor = "";
        }

        // html got re-written, assign event handler to html element again
        // used addEventListener("focusout",function), object.onfocusout = function() did not work in chrome

        { let rename_input = document.getElementById('connected-local-client-input'); if (rename_input != null) { rename_input.addEventListener("focusout", UI.connected_local_user_input_on_focusout); } }

        // if local_user is maintainer of root channel, he will sent new keys for root channel
        // if not, client will wait for channel keys
        // current_channel_keys must be set to null at client_connect if local client is at current_channnel_id

        if (g_current_channel_id == 0)
        {
            g_current_channel_keys = null;
            console.log("setting current keys to null");
        }

        let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);

        // if we are maintainers of root channel, send new keys

        if (g_channel_list[index].has_maintainer && g_channel_list[index].maintainer_id == g_local_client_id && g_current_channel_id == 0)
        {
            keys__create_and_send_new_channel_keys();
        }
        else
        {
            console.log("waiting for keys from maintainer");
        }

        // add tag ids

        console.log("process_tag_list_from_server | processing client: " +  single_client.username);
        for (let y = 0; y < single_client.tag_ids.length; y++)
        {
            console.log("process_tag_list_from_server | processing tag id : " + single_client.tag_ids[y]);

            let tag = channel_tree__get_tag_by_tag_id(single_client.tag_ids[y]);

            if (tag == null)
            {
                console.log("tag is null");
                continue;
            }

            let target_element = document.getElementById("client-tags-" + single_client.client_id);

            const node = document.createElement("div");
            node.className = "single-tag";
            node.setAttribute("tag-id", tag.tag_id);

            // has_icon is the only thing saying an icon was assigned, and id 0 resolves to a real
            // icon, so an unguarded lookup paints one on tags that have none
            let icon = (tag.has_icon == true) ? channel_tree__get_icon_by_icon_id(tag.tag_linked_icon_id) : null;

            if (icon != null)
            {
                node.style.backgroundImage = "url("+icon.base64_icon+")";
            }

            target_element.appendChild(node);
        }

        UI.refresh_all_channel_fullness();

        // a new client joined: pull their avatar (if any) so it shows on their row
        channel_tree__request_single_avatar(client_id);

        // a bot that just appeared streams within seconds; the player must already know it is one
        // (the headless node build has no player: audio.js is not part of its bundle)
        if (typeof audio__audio_player_announce_music_bots === "function")
        {
            audio__audio_player_announce_music_bots();
        }
    },
    /**
     * @brief somebody disconnected: removes them from the dom, g_client_list and the id map (swap-with-last), keeps an aliased client in g_offline_client_list, and drops their private chat context if open
     *
     * @param object msg -> the server message, msg.message.client_id names the client
     *
     * @return void
     */
    process_client_disconnect_from_server: function(msg)
    {
        chat__clear_typing_from_client(msg.message.client_id);  // gone, so nothing is being typed

        let disconnecting_client = channel_tree__get_client_by_client_id(msg.message.client_id);
        let local_client = channel_tree__get_client_by_client_id(g_local_client_id);

        // a disconnect can arrive for a client we never fully registered locally (e.g. one still
        // mid key-exchange). the map guard further down returns cleanly in that case, but this
        // early sound-effect check must not dereference an undefined client first
        if (disconnecting_client != null && local_client != null && disconnecting_client.channel_id == local_client.channel_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.user_disconnected_from_your_channel.play();
            }
        }

        // an aliased client is registered with the server, so he lingers as an offline contact;
        // the stored-clients snapshot only arrives on connect, so the local copy is kept fresh here
        if (disconnecting_client != null && typeof disconnecting_client.alias === "string" && disconnecting_client.alias.length > 0)
        {
            let already_stored = false;
            for (let i = 0; i < g_offline_client_list.length; i++)
            {
                if (g_offline_client_list[i].alias.toLowerCase() == disconnecting_client.alias.toLowerCase())
                {
                    already_stored = true;
                    break;
                }
            }

            if (already_stored == false)
            {
                g_offline_client_list.push({
                    alias: disconnecting_client.alias,
                    base64_avatar: (typeof disconnecting_client.base64_avatar === "string") ? disconnecting_client.base64_avatar : "",
                    tag_ids: (disconnecting_client.tag_ids != null) ? disconnecting_client.tag_ids : [],
                    last_seen: Math.floor(new Date().valueOf() / 1000) // we watched them leave: that IS their last-seen
                });
            }
        }

        if (document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]') != null)
        {
            document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]').remove();
        }

        if (document.getElementById('chat-context-pm-' + msg.message.client_id) != null)
        {
            document.getElementById('chat-context-pm-' + msg.message.client_id).remove();
        }

        let chat_context_index_to_remove = chat__get_chat_context_index_by_chat_context_id("chat-context-pm-" + msg.message.client_id);

        if (chat_context_index_to_remove != -1)
        {
            g_chat_context_array.splice(chat_context_index_to_remove, 1);
        }

        // if used chat context was deleted one, switch to current channel
        if (g_current_chat_context_id == "chat-context-pm-" + msg.message.client_id)
        {
            console.log("switching to main channel? ");
            g_current_chat_context_id = "chat-context-channel-" + g_current_channel_id;
        chat__clear_channel_unread_count(g_current_channel_id); // opened it, so it is read
            document.getElementById("chat-context-channel-" + g_current_channel_id).style.display = "block";
            g_chat_message_receiver_type = "channel";
            g_offline_chat_recipient_alias = ""; // back on a channel: no offline target
            UI.schedule_member_list_sync(); // active ring moves back to the channel circle
        }

        let index = g_map_client_id_to_array_index.get(parseInt(msg.message.client_id));
        if (index === undefined || index == -1)
        {
            // disconnect for a client that was never in our local list (e.g. one that dropped
            // mid key-exchange). we skip the array removal, but say so instead of silently returning
            console.warn("process_client_disconnect_from_server: no local entry for client_id " + msg.message.client_id + " (index " + index + "); skipping client_list removal");
            return;
        }

        let lastIndex = g_client_list.length - 1;
        let lastClient = g_client_list[lastIndex];

        // move last client into removed spot
        g_client_list[index] = lastClient;
        g_map_client_id_to_array_index.set(lastClient.client_id, index);

        // remove last
        g_client_list.pop();
        g_map_client_id_to_array_index.delete(parseInt(msg.message.client_id));

        UI.refresh_all_channel_fullness();
    },
    /**
     * @brief an incoming call, on its way to the native accept/decline screen: the Android object when a webview exists, the bridge listener when node runs headless
     *
     * @param object msg -> the server message, msg.message.caller_client_id names the caller
     *
     * @return void
     */
    process_call_from_server: function(msg)
    {
        let client = channel_tree__get_client_by_client_id(msg.message.caller_client_id);

        if (client == null)
        {
            return;
        }

        if (g_is_running_in_android_webview == true)
        {
            Android.JavaExportStartCall(client.username, client.channel_id);
            return;
        }

        if (g_node_incoming_call_listener != null)
        {
            g_node_incoming_call_listener(client.username, client.channel_id);
            return;
        }

        utils__custom_alert("incoming call from " + chat__sanitize_string(client.username));
    },
    /**
     * @brief a client left idle: updates their channel_id and is_idle, re-renders their row in the target channel with handlers, plays the join sound and may resume the always-on mic
     *        for the local client the channel globals are synced first, or the maintainer_id message
     *        (and with it the channel keys) would be silently discarded when returning straight into a call
     *
     * @param object msg -> the server message, msg.message holds client_id and channel_id
     *
     * @return void
     */
    process_client_coming_back_from_idle_mode_from_server: function(msg)
    {
        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].channel_id = msg.message.channel_id;
        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].is_idle = false;

        // the local client returned from idle: sync the channel globals - a stale
        // current_channel_id made the maintainer_id message (and with it the channel
        // keys) be silently discarded when returning straight into a call
        if (msg.message.client_id == g_local_client_id)
        {
            g_current_channel_id = msg.message.channel_id;
            g_current_channel_keys = null;

            // the server answered, so we are allowed to go idle again
            android_host__clear_come_from_idle_in_flight();
        }

        // someone else returned into OUR channel and we are its maintainer: rotate the
        // keys so the returner can communicate - same as on a normal channel join
        if (msg.message.client_id != g_local_client_id && msg.message.channel_id == g_current_channel_id)
        {
            let key_channel_index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);

            if (key_channel_index != -1 && g_channel_list[key_channel_index].has_maintainer && g_channel_list[key_channel_index].maintainer_id == g_local_client_id)
            {
                console.log("client " + msg.message.client_id + " returned from idle into current channel, rotating channel keys as maintainer");
                keys__create_and_send_new_channel_keys();
            }
        }

        if (document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]') != null)
        {
            document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]').remove();
        }

        let single_client = g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)];

        if (g_local_client_id != single_client.client_id)
        {
            html_to_append = channel_tree__generate_html_for_single_client(single_client, false);
        }
        else
        {
            html_to_append = channel_tree__generate_html_for_single_client(single_client, true);
        }

        channel_tree__get_channel_own_clients_last_element(msg.message.channel_id).insertAdjacentHTML("afterend", html_to_append);

        let element = document.querySelector('.connected-client[data-connected-client-id="'+msg.message.client_id+'"]');

        if (g_is_client_running_under_touch_device)
        {
            let local_touch_press_timer = null; // for touch devices

            element.addEventListener("touchstart", (event) => {

                is_long_press = false;

                // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                // by the time setTimeout runs, event object (or at least part of it) is lost,
                // things from event object must be imediatelly stored in temp variable in this case, for later use

                const currentTarget = event.currentTarget;
                const clientY = event.touches[0].clientY;
                const clientX = event.touches[0].clientX;

                local_touch_press_timer = window.setTimeout( () => {

                    is_long_press = true;
                    UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                },
                600, event);

            });

            element.addEventListener("touchend", (event) => {

                clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                const currentTarget = event.currentTarget;
                const clientY = event.changedTouches[0].pageY;
                const clientX = event.changedTouches[0].pageX;

                if (is_long_press == false)
                {
                    UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget, true);
                }
            });
        }
        else
        {
            element.addEventListener("mousedown", UI.connected_user_onmousedown);
        }

        // play sound in case the client that came back from idle mode joined channel of local client
        let local_client_channel_id = channel_tree__get_client_by_client_id(g_local_client_id).channel_id;

        if (local_client_channel_id == msg.message.channel_id && g_local_client_id != msg.message.client_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.user_joined_your_channel.play();
                g_sound_effects.call.pause();
                g_sound_effects.currentTime = 0;
            }
        }

        // continuous transmission no longer auto-starts on join - it is tap-toggled

    },
    /**
     * @brief the server's sdp offer for the datachannel: applied to g_peer_connection_with_server, then an answer is created, set as local description and sent back
     *        node sees this frame too (it owns the socket) but never built a peer connection, so it just forwards and ignores
     *
     * @param object msg -> the server message, msg.message.value is the offer as json
     *
     * @return void
     */
    process_sdp_offer_from_server: function(msg)
    {
        // voice lives in the webview only. node sees this frame too (it owns the socket)
        // but never built a peer connection, so it just forwards and ignores
        if (g_peer_connection_with_server == null)
        {
            return;
        }

        console.log(msg);

        let description = JSON.parse(msg.message.value); // value is OBJECT, contains sdp and type

        console.log("description -> ", description);

        g_peer_connection_with_server.setRemoteDescription(description)
            .then(function ()
            {
                console.log("sending answer...");
                g_peer_connection_with_server.createAnswer()
                    .then(function (answer)
                    {
                        g_peer_connection_with_server.setLocalDescription(answer);
                        voice__send_sdp_answer_to_server(answer);
                    })
                    .catch(function (error)
                    {
                        console.log(`Failed to create session description: ${error.toString()}`);
                    });
            })
            .catch(function (error)
            {
                console.log(`Failed to set session description: ${error.toString()}`);
            });
    },
    /**
     * @brief a channel was deleted: removes it from g_channel_list and the dom; plays the delete sound when the local client requested it
     *
     * @param object msg -> the server message, msg.message holds channel_id and channel_deletor_id
     *
     * @return void
     */
    process_channel_delete_from_server: function(msg)
    {
        console.log("channel deleted: channel_id=" + msg.message.channel_id);
        if (msg.message.channel_deletor_id == g_local_client_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.channel_deleted.play();
            }
        }

        let channel_index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, msg.message.channel_id);
        if (channel_index != -1)
        {
            g_channel_list.splice(channel_index, 1);
            document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').remove();
            UI.refresh_all_channel_fullness();
        }
    },
    /**
     * @brief an ice candidate from the server, fed into g_peer_connection_with_server; nothing without one (node, or a candidate arriving just after a teardown)
     *
     * @param object msg -> the server message, msg.message.value is the candidate
     *
     * @return void
     */
    process_ice_candidate_from_server: function(msg)
    {
        // same as the sdp offer: node has no peer connection, and in the browser a
        // candidate can still arrive just after a teardown nulled it
        if (g_peer_connection_with_server == null)
        {
            return;
        }

        console.log("got ice candidate from server");
        g_peer_connection_with_server.addIceCandidate(msg.message.value);
    },
    /**
     * @brief a server broadcast: appended to the root channel chat context, the trailing spacer rebuilt, the chat scrolled to the bottom
     *
     * @param object msg -> the server message, msg.message.value is the text
     *
     * @return void
     */
    process_server_info_broadcast_from_server: function(msg)
    {
        let index = chat__get_chat_context_index_by_chat_context_id("chat-context-channel-0");
        console.log("process_server_info_broadcast_from_server chat context index -> " + index);
        g_chat_context_array[index].last_known_message_sender_username = "";
        let html_to_append = "<div class=\"single-server-message\">" + chat__sanitize_string(msg.message.value) + " " + new Date().toLocaleTimeString() + "</div>";
        document.getElementById("chat-context-channel-0").insertAdjacentHTML("beforeend", html_to_append);
        let elements_count = document.getElementsByClassName("chat-spacer").length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName("chat-spacer")[0].remove();
        }

        let html_to_append1 = "<div class=\"chat-spacer\"></div>";
        document.getElementById("chat-context-channel-0").insertAdjacentHTML("beforeend", html_to_append1);
        chat__scroll_chat_to_end(false);
    },
    /**
     * @brief pairs our just-sent message or picture with its server id (stored as an attribute, which enables the right-click menu) and records our own public key as that message's author
     *
     * @param object msg -> the server message, msg.message holds local_message_id and server_chat_message_id
     *
     * @return void
     */
    process_server_chat_message_id_for_local_message_id_from_server: function(msg)
    {
        g_chat_message_author_public_keys[msg.message.server_chat_message_id] = g_rsa_public_key_string;

        let element = document.querySelector('.local-single-chat-message-content-p[data-single-chat-message-local-message-id="' + msg.message.local_message_id + '"]');

        if (element != null)
        {
            element.setAttribute("data-single-chat-message-server-message-id", msg.message.server_chat_message_id);
            element.addEventListener("mousedown", UI.single_chat_message_onrightclick);
        }
        else
        {
            let element = document.querySelector('.local-client-chat-picture-img[data-single-chat-message-local-message-id="' + msg.message.local_message_id + '"]');

            if (element != null)
            {
                element.setAttribute("data-single-chat-message-server-message-id", msg.message.server_chat_message_id);
                element.addEventListener("mousedown", UI.single_chat_message_onrightclick);
            }
            else
            {
                let file_card = document.querySelector('.local-client-chat-file[data-single-chat-message-local-message-id="' + msg.message.local_message_id + '"]');

                if (file_card != null)
                {
                    file_card.setAttribute("data-single-chat-message-server-message-id", msg.message.server_chat_message_id);
                    file_card.addEventListener("mousedown", chat_files__chat_file_card_onrightclick);
                }
            }
        }
    },
    /**
     * @brief a channel was edited: applies the fields to g_channel_list and its row (name text plus the password and audio-disabled classes); plays the edited sound if the local client edited it
     *
     * @param object msg -> the server message, msg.message holds the channel's fields and channel_editor_id
     *
     * @return void
     */
    process_channel_edit_from_server: function(msg)
    {
        console.log("channel edited: channel_id=" + msg.message.channel_id);
        if (msg.message.channel_editor_id == g_local_client_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.channel_edited.play();
            }
        }

        let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, msg.message.channel_id);
        document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').children[2].innerHTML = chat__sanitize_string(msg.message.channel_name);
        g_channel_list[index].description = msg.message.channel_description;
        g_channel_list[index].is_using_password = msg.message.is_using_password;
        g_channel_list[index].name = msg.message.channel_name;
        g_channel_list[index].is_audio_enabled = msg.message.is_audio_enabled;
        g_channel_list[index].is_client_limit_active = msg.message.is_client_limit_active;
        g_channel_list[index].max_client_count = msg.message.max_client_count;
        UI.refresh_all_channel_fullness();

        if (msg.message.is_using_password == true)
        {
            let element1 = document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').children[2];
            if (element1.classList.contains("single-channel-is-using-password") == false)
            {
                element1.classList.add("single-channel-is-using-password");
            }
        }
        else
        {
            let element1 = document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').children[2];
            if (element1.classList.contains("single-channel-is-using-password") == true)
            {
                element1.classList.remove("single-channel-is-using-password");
            }
        }

        if (msg.message.is_audio_enabled == false)
        {
            let element1 = document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').lastElementChild;

            console.log("audio is disabled, adding class single-channel-is-audio-disabled");

            if (element1.classList.contains("single-channel-is-audio-disabled") == false)
            {
                element1.classList.add("single-channel-is-audio-disabled");
                console.log("audio is disabled, class added");

            }
        }
        else
        {
            console.log(msg.message);
            let element1 = document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').lastElementChild;
            console.log("audio is enabled, removing class single-channel-is-audio-disabled");

            if (element1.classList.contains("single-channel-is-audio-disabled") == true)
            {
                element1.classList.remove("single-channel-is-audio-disabled");
                console.log("audio is enabled, class removed");
            }
        }
    },
    /**
     * @brief the client-info popup for admins: username, connected time, ip, country, last action, identity
     *
     * @param object msg -> the server message, msg.message holds the info fields
     *
     * @return void
     */
    process_client_info_from_server: function(msg)
    {
        // admin-only client info popup (the server only replies to admins)
        let client = channel_tree__get_client_by_client_id(msg.message.client_id);
        let username = (client != null && client.username != null) ? client.username : ("client " + msg.message.client_id);

        let total_seconds = parseInt(msg.message.connected_seconds);
        if (isNaN(total_seconds)) { total_seconds = 0; }
        let days = Math.floor(total_seconds / 86400);
        let hours = Math.floor((total_seconds % 86400) / 3600);
        let minutes = Math.floor((total_seconds % 3600) / 60);

        let country = (msg.message.country_iso_code != null && msg.message.country_iso_code.length > 0) ? msg.message.country_iso_code : "unknown";

        document.getElementById("client-info-username").innerText = username;
        document.getElementById("client-info-connected").innerText = days + "d " + hours + "h " + minutes + "m";
        document.getElementById("client-info-ip").innerText = msg.message.ip_address;
        document.getElementById("client-info-country").innerText = country;
        document.getElementById("client-info-last-action").innerText = parseInt(msg.message.last_action_seconds_ago) + "s ago";
        document.getElementById("client-info-identity").innerText = msg.message.identity;

        document.getElementById("client-info-container").style.display = "block";
    },

    /**
     * @brief a channel was created: pushes it into g_channel_list, inserts its row after the parent's subtree, rewires all channel handlers; plays the created sound if the local client created it
     *
     * @param object msg -> the server message, msg.message holds the channel's fields and channel_creator_id
     *
     * @return void
     */
    process_channel_create_from_server: function(msg)
    {

        console.log(msg);

        if (msg.message.channel_creator_id == g_local_client_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.channel_created.play();
            }
        }

        let new_channel = {
            channel_id: msg.message.channel_id,
            parent_channel_id: msg.message.parent_channel_id,
            name: msg.message.name,
            description: msg.message.description,
            maintainer_id: msg.message.maintainer_id,
            is_root_channel: msg.message.is_root_channel,
            has_maintainer: msg.message.has_maintainer,
            is_using_password: msg.message.is_using_password,
            is_audio_enabled: msg.message.is_audio_enabled,
            is_temp_channel: msg.message.is_temp_channel,
            is_client_limit_active: msg.message.is_client_limit_active,
            max_client_count: msg.message.max_client_count,
            has_channel_icon: msg.message.has_channel_icon,
            channel_icon_id: msg.message.channel_icon_id,
            is_channel_directly_collapsed: false,
        };

        g_channel_list.push(new_channel);
        let indentation_level = channel_tree__get_indentation_level(new_channel.channel_id, g_channel_list);

        let html_to_append = "";
        let html_to_append_audio_disabled_class = (new_channel.is_audio_enabled == false) ? "single-channel-is-audio-disabled" : "";
        let html_to_append_is_using_password_class = (new_channel.is_using_password == true) ? "single-channel-is-using-password" : "";
        let html_to_append_is_temp_class = (new_channel.is_temp_channel == true) ? "single-channel-is-temp" : "";

        if (msg.message.is_using_password == true)
        {
            html_to_append = "<div class=\"single-channel\" data-channel-id=\"" + new_channel.channel_id + "\" data-channel-parent-id=\"" + new_channel.parent_channel_id + "\">\n\
                                        <div class=\"padding-div\" style=\"padding-left: " + indentation_level * 20 + "px;\"></div>\n\
                                        <div class=\"single-channel-collapse-button\">\n\
                                        </div>\n\
                                        <p class='single-channel-name-p "+html_to_append_is_using_password_class+" "+html_to_append_is_temp_class+"' data-channel-name-id=\"" + new_channel.channel_id + "\">" + new_channel.name + "</p>\n\
                                        <div class=\"single-channel-icon\" data-channel-icon-for=\"" + new_channel.channel_id + "\"></div>\n\
                                        <p class=\"single-channel-unread-number unread-empty\" data-channel-unread-for=\"" + new_channel.channel_id + "\"></p>\n\
                                        <div class="+html_to_append_audio_disabled_class+"></div>\n\
                                    </div>";
        }
        else
        {
            html_to_append = "<div class=\"single-channel\" data-channel-id=\"" + new_channel.channel_id + "\" data-channel-parent-id=\"" + new_channel.parent_channel_id + "\">\n\
                                        <div class=\"padding-div\" style=\"padding-left: " + indentation_level * 20 + "px;\"></div>\n\
                                        <div class=\"single-channel-collapse-button\">\n\
                                        </div>\n\
                                        <p class='single-channel-name-p "+html_to_append_is_temp_class+"' data-channel-name-id=\"" + new_channel.channel_id + "\" >" + new_channel.name + "</p>\n\
                                        <div class=\"single-channel-icon\" data-channel-icon-for=\"" + new_channel.channel_id + "\"></div>\n\
                                        <p class=\"single-channel-unread-number unread-empty\" data-channel-unread-for=\"" + new_channel.channel_id + "\"></p>\n\
                                        <div class="+html_to_append_audio_disabled_class+"></div>\n\
                                    </div>";
        }

        // first find out if there are any clients in channel where sub channel is about to be added
        // goal is to append channel after last client connected to the parrent channel, if there is some client present
        // so gui is consistent

        // append the new subchannel at the end of the parent's whole subtree (after the parent's clients
        // and any already-existing subchannels), so siblings keep creation order instead of the new one
        // being prepended right after the parent header
        let subtree_anchor = channel_tree__get_channel_subtree_last_element(new_channel.parent_channel_id);
        subtree_anchor.insertAdjacentHTML("afterend", html_to_append);

        // paint the new row's icon (a fresh channel has none yet, but this also covers the case
        // where it was created carrying one, and keeps the icon box consistent)
        UI.refresh_channel_icon(new_channel);

        let elements = document.getElementsByClassName('single-channel');

        // handle UI differently on touch devices..
        if (g_is_client_running_under_touch_device)
        {
            for (let i = 0; i < elements.length; i++)
            {

                // skip the special idle channel

                if (elements[i].classList.contains("idle-channel"))
                {
                    continue;
                }

                let local_touch_press_timer = null;

                elements[i].addEventListener("touchstart", (event) => {

                    // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                    // by the time setTimeout runs, event object (or at least part of it) is lost,
                    // things from event object must be imediatelly stored in temp variable in this case, for later use

                    const currentTarget = event.currentTarget;
                    const clientY = event.touches[0].clientY;
                    const clientX = event.touches[0].clientX;

                    is_pressing = true;

                    local_touch_press_timer = window.setTimeout( () => {
                        if (is_pressing)
                        {
                            UI.single_channel_onmousedown(event, true, clientX, clientY, currentTarget);
                            is_pressing = false;
                        }
                    },
                    600, event);

                });

                elements[i].addEventListener("touchend", (event) => {
                    is_pressing = false;
                    clearTimeout(local_touch_press_timer);
                });

                elements[i].addEventListener("touchcancel", (event) => {
                    is_pressing = false;
                    clearTimeout(local_touch_press_timer);
                });

                elements[i].getElementsByClassName("single-channel-collapse-button")[0].addEventListener("mousedown", UI.collapse_expand_channel);
            }
        }
        else
        {
            for (let i = 0; i < elements.length; i++)
            {
                if (elements[i].classList.contains("idle-channel"))
                {
                    continue;
                }

                elements[i].addEventListener("mousedown", UI.single_channel_onmousedown);
                let selected_channel_id1 = parseInt(elements[i].getAttribute("data-channel-id"));
                console.log("selected_channel_id1 => " + selected_channel_id1);
                elements[i].querySelector('[data-channel-name-id="' + selected_channel_id1 + '"]').addEventListener("dblclick", UI.single_channel_doubleclick_join);
                elements[i].querySelector('[data-channel-name-id="' + selected_channel_id1 + '"]').addEventListener("click", UI.single_channel_onclick);
                elements[i].getElementsByClassName("single-channel-collapse-button")[0].addEventListener("mousedown", UI.collapse_expand_channel);
            }
        }

        // recompute chevron (leaf) visibility after the channel tree changed
        UI.refresh_all_channel_fullness();
    },
    /**
     * @brief a picture is coming to the channel: appends a placeholder (progress ring) message; the real image arrives later and is matched by picture_id
     *        an ignored sender is skipped
     *
     * @param object msg -> the server message, msg.message holds sender_id, picture_id and size
     *
     * @return void
     */
    process_channel_chat_picture_metadata_from_server: function(msg)
    {
        let client_index = channel_tree__get_client_index_in_array_by_client_id(msg.message.sender_id);

        if (client_index == -1)
        {
            return;
        }

        if (channel_tree__get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        // the ring counts the incoming chunks against the encrypted length the server announced
        chat_files__begin_chat_file_transfer(msg.message.picture_id, msg.message.size);

        let sender_username = g_client_list[client_index].username;
        let index = chat__get_chat_context_index_by_chat_context_id("chat-context-channel-" + g_current_channel_id);
        let receiving_chat_context_id = "chat-context-channel-" + msg.message.channel_id;

        g_chat_context_array[index].last_known_message_sender_username = sender_username;

        let elements_count = document.getElementsByClassName('chat-spacer').length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName('chat-spacer')[0].remove();
        }

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(msg.message.sender_id, chat__sanitize_string(sender_username)) + "\n\
                                        </div>\n\
                                    <div class=\"single-chat-message-sender-time\">\n\
                                        <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-content\">\n\
                                        " + chat_files__generate_chat_picture_progress_html(msg.message.picture_id) + "\n\
                                        <img class='chat-picture-img chat-picture-img-default' data-single-chat-message-server-message-id='"+ msg.message.picture_id + "' id=\"chat-picture-img-" + msg.message.picture_id + "\"></img>\n\
                                    </div>\n\
                                </div>";

        document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
        document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");

        if (g_current_chat_context_id == receiving_chat_context_id)
        {
            chat__scroll_chat_to_end(false);
        }
    },
    /**
     * @brief a picture is coming in private: creates the pm chat context if missing, plays the received sound, appends the placeholder and bumps the sender's unread badge
     *        g_chat_context_array is the source of truth for the context's existence, not the dom
     *
     * @param object msg -> the server message, msg.message holds sender_id, picture_id and size
     *
     * @return void
     */
    process_direct_chat_picture_metadata_from_server: function(msg)
    {
        // if direct message is of type direct_chat_message, do not display the message if client is ignored
        if (channel_tree__get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        // the ring counts the incoming chunks against the encrypted length the server announced
        chat_files__begin_chat_file_transfer(msg.message.picture_id, msg.message.size);

        let received_direct_message_chat_context_id = 'chat-context-pm-' + msg.message.sender_id;

        // g_chat_context_array is the source of truth, not the dom. asking the dom whether the
        // context exists ties state creation to rendering, which breaks with no dom present
        let is_chat_context_existing = false;

        for (let context_index = 0; context_index < g_chat_context_array.length; context_index++)
        {
            if (g_chat_context_array[context_index].chat_context_id == received_direct_message_chat_context_id)
            {
                is_chat_context_existing = true;
                break;
            }
        }

        if (g_are_sound_effects_enabled)
        {
            g_sound_effects.message_received.play();
        }

        if (is_chat_context_existing == false)
        {
            console.log("element not found");

            let html_to_append = "<div class=\"chat-context\" id=\"" + received_direct_message_chat_context_id + "\">\n\
                                            <div class=\"single-server-message\">now talking to user: " + chat__sanitize_string(channel_tree__get_display_name_by_client_id(msg.message.sender_id, msg.message.sender_username)) + "</div>\n\
                                                <div class=\"single-server-message\">your public key: " + g_rsa_public_key_string + "</div>\n\
                                                <div class=\"single-server-message\">his public key: " + chat__sanitize_string(channel_tree__get_public_key_by_client_id(msg.message.sender_id)) + "</div>\n\
                                            <div class=\"single-server-message\"></div>\n\
                                        </div>";

            document.getElementById("chat-context-container").insertAdjacentHTML("beforeend", html_to_append);
            document.getElementById(received_direct_message_chat_context_id).style.display = "none";

            let single_chat_context = {
                type: "user",
                chat_context_id: received_direct_message_chat_context_id,
                last_known_message_sender_username: ""
            };

            g_chat_context_array.push(single_chat_context);
        }

        let chat_context_index = chat__get_chat_context_index_by_chat_context_id(received_direct_message_chat_context_id);
        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(msg.message.sender_id, chat__sanitize_string(msg.message.sender_username)) + "\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-sender-time\"><p>" + new Date().toLocaleTimeString() + "</p>\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-content\">\n\
                                            " + chat_files__generate_chat_picture_progress_html(msg.message.picture_id) + "\n\
                                            <img class='chat-picture-img-default chat-picture-img' data-single-chat-message-server-message-id='"+ msg.message.picture_id + "' id=\"chat-picture-img-" + msg.message.picture_id + "\"></img>\n\
                                        </div>\n\
                                    </div>\n\
                                </div>";

        document.getElementById(received_direct_message_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
        let elements_count = document.getElementsByClassName("chat-spacer").length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName("chat-spacer")[0].remove();
        }

        let html_to_append1 = "<div class=\"chat-spacer\"></div>";
        document.getElementById(received_direct_message_chat_context_id).insertAdjacentHTML("beforeend", html_to_append1);

        if (g_current_chat_context_id == received_direct_message_chat_context_id)
        {
            chat__scroll_chat_to_end(false);
        }

        if (g_current_chat_context_id != received_direct_message_chat_context_id)
        {
            chat__increment_unread_count(msg.message.sender_id);
            chat__render_unread_badge(msg.message.sender_id, true);
        }
        g_chat_context_array[chat_context_index].last_known_message_sender_username = msg.message.sender_username;
    },
    /**
     * @brief a file is coming to the channel: the worker already opened its header, so the card goes up now with the name, size, icon and a progress ring; the body fills it in later by file_id
     *
     * @param object msg -> the server message, msg.message holds sender_id, channel_id, file_id, the header and size
     *
     * @return void
     */
    process_channel_chat_file_metadata_from_server: function(msg)
    {
        let client_index = channel_tree__get_client_index_in_array_by_client_id(msg.message.sender_id);

        if (client_index == -1)
        {
            return;
        }

        if (channel_tree__get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        let receiving_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
        let chat_context_index = chat__get_chat_context_index_by_chat_context_id(receiving_chat_context_id);

        if (chat_context_index == -1)
        {
            return;
        }

        let sender_username = g_client_list[client_index].username;
        let header = (msg.message.file_header_decrypted != null) ? msg.message.file_header_decrypted : { name: "(file, no key to open it)", size: 0 };

        g_chat_context_array[chat_context_index].last_known_message_sender_username = sender_username;
        g_chat_message_author_public_keys[msg.message.file_id] = channel_tree__get_public_key_by_client_id(msg.message.sender_id);
        chat_files__begin_chat_file_transfer(msg.message.file_id, msg.message.encrypted_size);

        let elements_count = document.getElementsByClassName("chat-spacer").length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName("chat-spacer")[0].remove();
        }

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(msg.message.sender_id, chat__sanitize_string(sender_username)) + "\n\
                                        </div>\n\
                                    <div class=\"single-chat-message-sender-time\">\n\
                                        <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-content\">\n\
                                        " + chat_files__generate_chat_file_card_html({ key: msg.message.file_id, name: header.name, size: header.size, is_receiving: true }) + "\n\
                                    </div>\n\
                                </div>";

        document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
        document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");

        let card = chat_files__get_chat_file_card_by_key(msg.message.file_id);

        if (card != null)
        {
            card.addEventListener("mousedown", chat_files__chat_file_card_onrightclick);
        }

        chat__scroll_chat_to_end(false);
    },
    /**
     * @brief a file is coming in private: creates the pm chat context if missing, plays the received sound, puts up the card with its ring and bumps the sender's unread badge
     *
     * @param object msg -> the server message, msg.message holds sender_id, file_id, the header and size
     *
     * @return void
     */
    process_direct_chat_file_metadata_from_server: function(msg)
    {
        if (channel_tree__get_client_by_client_id(msg.message.sender_id) == null || channel_tree__get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        let received_direct_message_chat_context_id = 'chat-context-pm-' + msg.message.sender_id;
        let is_chat_context_existing = false;

        for (let context_index = 0; context_index < g_chat_context_array.length; context_index++)
        {
            if (g_chat_context_array[context_index].chat_context_id == received_direct_message_chat_context_id)
            {
                is_chat_context_existing = true;
                break;
            }
        }

        if (g_are_sound_effects_enabled)
        {
            g_sound_effects.message_received.play();
        }

        if (is_chat_context_existing == false)
        {
            let html_to_append = "<div class=\"chat-context\" id=\"" + received_direct_message_chat_context_id + "\">\n\
                                            <div class=\"single-server-message\">now talking to user: " + chat__sanitize_string(channel_tree__get_display_name_by_client_id(msg.message.sender_id, msg.message.sender_username)) + "</div>\n\
                                                <div class=\"single-server-message\">your public key: " + g_rsa_public_key_string + "</div>\n\
                                                <div class=\"single-server-message\">his public key: " + chat__sanitize_string(channel_tree__get_public_key_by_client_id(msg.message.sender_id)) + "</div>\n\
                                            <div class=\"single-server-message\"></div>\n\
                                        </div>";

            document.getElementById("chat-context-container").insertAdjacentHTML("beforeend", html_to_append);
            document.getElementById(received_direct_message_chat_context_id).style.display = "none";

            let single_chat_context = {
                type: "user",
                chat_context_id: received_direct_message_chat_context_id,
                last_known_message_sender_username: ""
            };

            g_chat_context_array.push(single_chat_context);
        }

        let chat_context_index = chat__get_chat_context_index_by_chat_context_id(received_direct_message_chat_context_id);
        let header = (msg.message.file_header_decrypted != null) ? msg.message.file_header_decrypted : { name: "(file, no key to open it)", size: 0 };

        g_chat_message_author_public_keys[msg.message.file_id] = channel_tree__get_public_key_by_client_id(msg.message.sender_id);
        chat_files__begin_chat_file_transfer(msg.message.file_id, msg.message.encrypted_size);

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(msg.message.sender_id, chat__sanitize_string(msg.message.sender_username)) + "\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-sender-time\"><p>" + new Date().toLocaleTimeString() + "</p>\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-content\">\n\
                                            " + chat_files__generate_chat_file_card_html({ key: msg.message.file_id, name: header.name, size: header.size, is_receiving: true }) + "\n\
                                        </div>\n\
                                    </div>\n\
                                </div>";

        document.getElementById(received_direct_message_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);

        let elements_count = document.getElementsByClassName("chat-spacer").length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName("chat-spacer")[0].remove();
        }

        document.getElementById(received_direct_message_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");

        let card = chat_files__get_chat_file_card_by_key(msg.message.file_id);

        if (card != null)
        {
            card.addEventListener("mousedown", chat_files__chat_file_card_onrightclick);
        }

        if (g_current_chat_context_id != received_direct_message_chat_context_id)
        {
            chat__increment_unread_count(msg.message.sender_id);
            chat__render_unread_badge(msg.message.sender_id, true);
        }
        else
        {
            chat__scroll_chat_to_end(false);
        }

        g_chat_context_array[chat_context_index].last_known_message_sender_username = msg.message.sender_username;
    },
    /**
     * @brief the announced maintainer of the current channel, recorded after checking he exists in our channel; we then either send fresh keys (we are him) or arm the keys-wait timer
     *
     * @param object msg -> the server message, msg.message holds maintainer_id, channel_id and has_maintainer
     *
     * @return void
     */
    process_channel_maintainer_id_from_server: function(msg)
    {
        console.log(msg);

        // we received some maintainer id from server and we are expected to set it
        // but not so fast, dont let server to trick us...
        // first, check if maintainer really exists at our side and if he really is present in current channel at our end

        let client_index = channel_tree__get_client_index_in_array_by_client_id(msg.message.maintainer_id);

        if (client_index == -1)
        {
            console.log("%c process_channel_maintainer_id_from_server: client with id (" + msg.message.maintainer_id + ") does not exist", "color: red");
            return;
        }

        if (g_client_list[client_index].channel_id != g_current_channel_id)
        {
            console.log("%c process_channel_maintainer_id_from_server: client with id (" + msg.message.maintainer_id + ") not in current channel", "color: red");
            return;
        }

        let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);

        // unguarded -1 crashed the whole handler mid-dispatch on device
        if (index == -1)
        {
            console.warn("maintainer message for unknown channel " + g_current_channel_id + ", ignoring");
            return;
        }

        g_channel_list[index].maintainer_id = msg.message.maintainer_id;
        g_channel_list[index].has_maintainer = msg.message.has_maintainer;
        if (msg.message.has_maintainer && (g_local_client_id == msg.message.maintainer_id) && (g_current_channel_id == msg.message.channel_id))
        {
            // reuse the already-validated index; a fresh unguarded lookup crashed here
            console.log("local user is maintainer of channel '" + g_channel_list[index].name + "'");

            // local user is the key SENDER now, there is nothing to wait for
            keys__cancel_maintainer_keys_wait_timer();

            keys__create_and_send_new_channel_keys();
        }
        else if ((g_current_channel_id == msg.message.channel_id) && (g_local_client_id != msg.message.maintainer_id))
        {
            console.log("%c received channel_maintainer_id " + msg.message.maintainer_id + " for channel: " + msg.message.channel_id, "color: blue");

            // keys from this maintainer are expected shortly; if none arrive within the
            // timeout, the timer votes for a maintainer reset (and keeps re-voting)
            keys__arm_maintainer_keys_wait_timer();
        }
        else
        {
            console.log("%c process_channel_maintainer_id_from_server: unknown", "color: blue");
        }
    },
    /**
     * @brief a channel chat message: rendered with the font size clamped to 12-30 and grouped under the previous sender; our own echo and ignored senders are skipped, the received sound plays
     *
     * @param object msg -> the server message, msg.message holds sender_id, server_chat_message_id and the decrypted payload
     *
     * @return void
     */
    process_channel_chat_message_from_server: function(msg)
    {
        chat__clear_typing_from_client(msg.message.sender_id);  // the message arrived, he is done

        if (msg.message.sender_id == g_local_client_id)
        {
            return;
        }

        g_chat_message_author_public_keys[msg.message.server_chat_message_id] = channel_tree__get_public_key_by_client_id(msg.message.sender_id);

        if (channel_tree__get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        if (g_are_sound_effects_enabled == true)
        {
            g_sound_effects.message_received.play();
        }

        let chat_message_username = chat__sanitize_string(msg.message.sender_username);
        let receiving_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
        let chat_context_index = chat__get_chat_context_index_by_chat_context_id(receiving_chat_context_id);

        // badge the channel unless it is the one on screen
        chat__increment_channel_unread_count(msg.message.channel_id);

        let received_channel_chat_message_object = JSON.parse(msg.message.decrypted_value);
        received_channel_chat_message_object.value = chat__sanitize_string(received_channel_chat_message_object.value);
        let font_size1 = received_channel_chat_message_object.font_size;

        if (custom_typeof(font_size1) != "number")
        {
            console.log("custom_typeof(font_size1) != number");
            console.log("font_size1" + font_size1);
            font_size1 = 12;
        }

        if (font_size1 < 12)
        {
            font_size1 = 12;
        }

        if (font_size1 > 30)
        {
            font_size1 = 30;
        }

        font_size1 = font_size1 + "px";

        if (g_chat_context_array[chat_context_index].last_known_message_sender_username == chat_message_username)
        {
            let chat_message = "<p class='single-chat-message-content-p "+chat__sanitize_string(received_channel_chat_message_object.font)+"' data-single-chat-message-server-message-id='" + msg.message.server_chat_message_id + "' style='color: "+chat__sanitize_string(received_channel_chat_message_object.font_color)+"; font-size: "+font_size1+";'>" + received_channel_chat_message_object.value + "</p>";
            let last_child_index = document.getElementById(receiving_chat_context_id).getElementsByClassName("single-chat-message").length - 1;
            document.getElementById(receiving_chat_context_id).getElementsByClassName("single-chat-message")[last_child_index].getElementsByClassName("single-chat-message-content")[0].insertAdjacentHTML("beforeend", chat_message);
        }
        else
        {
            let html_to_append = "<div class=\"single-chat-message\">\n\
                                            <div class=\"single-message-content\">\n\
                                                <div class=\"single-chat-message-sender-username-container\">\n\
                                                    " + android_host__generate_message_sender_html(msg.message.sender_id, chat_message_username) + "\n\
                                                </div>\n\
                                                <div class=\"single-chat-message-sender-time\">\n\
                                                    <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                                </div>\n\
                                                <div class=\"single-chat-message-content\">\n\
                                                    <p class='single-chat-message-content-p "+chat__sanitize_string(received_channel_chat_message_object.font)+"' data-single-chat-message-server-message-id='"+ msg.message.server_chat_message_id + "' style='color: "+chat__sanitize_string(received_channel_chat_message_object.font_color)+"; font-size: "+font_size1+";'>" + received_channel_chat_message_object.value + "</p>\n\
                                                </div>\n\
                                            </div>\n\
                                        </div>";

            let elements_count = document.getElementById(receiving_chat_context_id).getElementsByClassName("chat-spacer").length;

            for (let i = 0; i < elements_count; i++)
            {
                document.getElementById(receiving_chat_context_id).getElementsByClassName("chat-spacer")[0].remove();
            }

            document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
            document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");
            chat__scroll_chat_to_end(false);
        }

        chat__scroll_chat_to_end(false);

        if (g_local_username == chat_message_username)
        {
            document.getElementById("chat-input-container-text-input").value = "";
        }

        g_chat_context_array[chat_context_index].last_known_message_sender_username = chat_message_username;
    },
    /**
     * @brief a message somebody left for us while we were offline, handed over on connect
     *        the sender may or may not be connected now, so it is addressed by their ALIAS: the chat
     *        context is keyed by alias too, and merges with the live one if they are here
     *
     * @param object msg -> the server message, msg.message holds sender_alias, some_json and queued_unix_seconds
     *
     * @return void
     */
    process_offline_chat_message_from_server: function(msg)
    {
        let sender_alias = (typeof msg.message.sender_alias === "string") ? msg.message.sender_alias : "";
        let received_object = null;

        try { received_object = JSON.parse(msg.message.some_json.value); }
        catch (e) { console.warn("offline message payload was not readable:", e.message); return; }

        let message_text = chat__sanitize_string(received_object.value);
        let when_text = (typeof msg.message.queued_unix_seconds === "number" && msg.message.queued_unix_seconds > 0)
            ? UI.format_time_ago(msg.message.queued_unix_seconds)
            : "while you were away";

        // if that person is connected right now, their live private chat is the natural
        // home for this; otherwise it lands in an alias-keyed context of its own
        let sender_client = null;
        for (let i = 0; i < g_client_list.length; i++)
        {
            if (g_client_list[i] != null && g_client_list[i].alias == sender_alias && sender_alias.length > 0)
            {
                sender_client = g_client_list[i];
                break;
            }
        }

        // raw alias in the id so it matches what open_offline_chat_context builds; the
        // alias only ever reaches the DOM as text through chat__sanitize_string below
        let context_id = (sender_client != null) ? ("chat-context-pm-" + sender_client.client_id) : ("chat-context-offline-" + sender_alias);

        // g_chat_context_array is the source of truth, not the dom. asking the dom whether the
        // context exists ties state creation to rendering, which breaks with no dom present
        let is_chat_context_existing = false;

        for (let context_index = 0; context_index < g_chat_context_array.length; context_index++)
        {
            if (g_chat_context_array[context_index].chat_context_id == context_id)
            {
                is_chat_context_existing = true;
                break;
            }
        }

        if (is_chat_context_existing == false)
        {
            let html_to_append = "<div class=\"chat-context\" id=\"" + context_id + "\" style=\"display: none;\">\n\
                                    <div class=\"single-server-message\">now talking to user: " + chat__sanitize_string(sender_alias) + "</div>\n\
                                </div>";
            document.getElementById('chat-context-container').insertAdjacentHTML("beforeend", html_to_append);

            g_chat_context_array.push({
                type: "user",
                chat_context_id: context_id,
                last_known_message_sender_username: ""
            });

            // a pill so the message is reachable (strip themes surface it through the people strip);
            // the raw alias goes into the id, matching the context id the promotion looks up
            let selector_id = (sender_client != null) ? ("user-" + sender_client.client_id) : ("offline-" + sender_alias);
            if (document.querySelector('[data-chat-context-selector-id="' + selector_id + '"]') == null)
            {
                let selector_html = "<div class=\"chat-context-selector\" data-chat-context-selector-type=\"user\" data-chat-context-selector-id=\"" + selector_id + "\">\n\
                                        <div class=\"p-container\"><p>" + chat__sanitize_string(sender_alias) + "</p></div>\n\
                                        <div class=\"remove-chat-context-selector\" data-chat-context-remove-selector-type=\"user\" data-chat-context-remove-selector-id=\"" + selector_id + "\"></div>\n\
                                    </div>";
                document.getElementById("chat-context-selectors-container").insertAdjacentHTML("beforeend", selector_html);

                // freshly inserted markup carries no handlers - rebind them all, the same
                // way every other place that adds a pill does
                for (let x = 0; x < document.getElementsByClassName("chat-context-selector").length; x++)
                {
                    document.getElementsByClassName("chat-context-selector")[x].onclick = UI.chat_context_selector_onclick;
                }
                for (let x = 0; x < document.getElementsByClassName("remove-chat-context-selector").length; x++)
                {
                    document.getElementsByClassName("remove-chat-context-selector")[x].onclick = UI.chat_context_remove_onclick;
                }
            }
        }

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                <div class=\"single-message-content\">\n\
                                    <div class=\"single-chat-message-sender-username-container\">\n\
                                        " + android_host__generate_message_sender_html((sender_client != null) ? sender_client.client_id : null, chat__sanitize_string(sender_alias)) + "\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-sender-time\">\n\
                                        <p>" + when_text + "</p>\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-content\">\n\
                                        <p class='single-chat-message-content-p'>" + message_text + "</p>\n\
                                    </div>\n\
                                </div>\n\
                            </div>";

        document.getElementById(context_id).insertAdjacentHTML("beforeend", html_to_append);

        if (g_current_chat_context_id == context_id)
        {
            chat__scroll_chat_to_end(false);
        }

        // unread marker on that person, exactly like a live private message
        if (sender_client != null)
        {
            chat__increment_unread_count(sender_client.client_id);
            chat__render_unread_badge(sender_client.client_id, true);
        }

        UI.schedule_member_list_sync();
    },

    /**
     * @brief a direct message, two payload types: renders a private chat message (creating the pm context and the unread badge), or installs the channel keys if they come from the announced maintainer
     *
     * @param object msg -> the server message, msg.message.some_json.type picks the payload
     *
     * @return void
     */
    process_direct_chat_message_from_server: function(msg)
    {
        chat__clear_typing_from_client(msg.message.sender_id);  // the message arrived, he is done

        g_chat_message_author_public_keys[msg.message.server_chat_message_id] = channel_tree__get_public_key_by_client_id(msg.message.sender_id);

        if (msg.message.some_json.type == "direct_chat_message")
        {
            // if direct message is of type direct_chat_message, do not display the message if client is ignored
            if (channel_tree__get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
            {
                return;
            }

            let decrypted_text = msg.message.some_json.value;

            let received_direct_message_object = JSON.parse(decrypted_text);
            received_direct_message_object.value = chat__sanitize_string(received_direct_message_object.value);

            let font_size1 = received_direct_message_object.font_size;

            if (custom_typeof(font_size1) != "number")
            {
                font_size1 = 12;
            }

            if (font_size1 < 12)
            {
                font_size1 = 12;
            }

            if (font_size1 > 30)
            {
                font_size1 = 30;
            }

            font_size1 = font_size1 + "px";

            let received_direct_message_chat_context_id = "chat-context-pm-" + msg.message.sender_id;

            // g_chat_context_array is the source of truth, not the dom. asking the dom whether the
            // context exists ties state creation to rendering, which breaks with no dom present
            let is_chat_context_existing = false;

            for (let context_index = 0; context_index < g_chat_context_array.length; context_index++)
            {
                if (g_chat_context_array[context_index].chat_context_id == received_direct_message_chat_context_id)
                {
                    is_chat_context_existing = true;
                    break;
                }
            }

            if (is_chat_context_existing == false)
            {
                if (g_are_sound_effects_enabled)
                {
                    g_sound_effects.message_received.play();
                }

                let html_to_append = "<div class=\"chat-context\" id=\"" + received_direct_message_chat_context_id + "\">\n\
                                        <div class=\"single-server-message\">now talking to user:  " + chat__sanitize_string(channel_tree__get_display_name_by_client_id(msg.message.sender_id, msg.message.sender_username)) + "</div>\n\
                                        <div class=\"single-server-message\">your public key: " + g_rsa_public_key_string + "</div>\n\
                                        <div class=\"single-server-message\">his public key: " + chat__sanitize_string(channel_tree__get_public_key_by_client_id(msg.message.sender_id)) + "</div>\n\
                                        <div class=\"single-server-message\"></div>\n\
                                    </div>"

                document.getElementById('chat-context-container').insertAdjacentHTML("beforeend", html_to_append);
                document.getElementById(received_direct_message_chat_context_id).style.display = "none";

                let single_chat_context = {
                    type: "user",
                    chat_context_id: received_direct_message_chat_context_id,
                    last_known_message_sender_username: ""
                };

                g_chat_context_array.push(single_chat_context);
            }

            let chat_context_index = chat__get_chat_context_index_by_chat_context_id(received_direct_message_chat_context_id);

            if (g_chat_context_array[chat_context_index].last_known_message_sender_username == msg.message.sender_username)
            {
                let chat_message = "<p class='single-chat-message-content-p "+chat__sanitize_string(received_direct_message_object.font)+"' data-single-chat-message-server-message-id='" + msg.message.server_chat_message_id + "' style='color: "+chat__sanitize_string(received_direct_message_object.font_color)+"; font-size: "+font_size1+";'>" + received_direct_message_object.value + "</p>";
                let last_child_index = document.getElementById(received_direct_message_chat_context_id).getElementsByClassName("single-chat-message").length - 1;
                let exists = document.getElementById(received_direct_message_chat_context_id).getElementsByClassName("single-chat-message") != "undefined";
                document.getElementById(received_direct_message_chat_context_id).getElementsByClassName("single-chat-message")[last_child_index].getElementsByClassName("single-chat-message-content")[0].insertAdjacentHTML("beforeend", chat_message);
                // hides the badge without clearing the count, which is what this did before
                chat__render_unread_badge(msg.message.sender_id, false);
            }
            else
            {
                let html_to_append = "<div class=\"single-chat-message\">\n\
                                        <div class=\"single-message-content\">\n\
                                            <div class=\"single-chat-message-sender-username-container\">\n\
                                                " + android_host__generate_message_sender_html(msg.message.sender_id, chat__sanitize_string(msg.message.sender_username)) + "\n\
                                            </div>\n\
                                            <div class=\"single-chat-message-sender-time\">\n\
                                                <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                            </div>\n\
                                            <div class=\"single-chat-message-content\">\n\
                                                <p class='single-chat-message-content-p "+chat__sanitize_string(received_direct_message_object.font)+"' data-single-chat-message-server-message-id='"+ msg.message.server_chat_message_id + "' style='color: "+chat__sanitize_string(received_direct_message_object.font_color)+"; font-size: "+font_size1+";'>" + received_direct_message_object.value + "</p>\n\
                                            </div>\n\
                                        </div>\n\
                                    </div>";

                document.getElementById(received_direct_message_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
                let elements_count = document.getElementsByClassName("chat-spacer").length;

                for (let i = 0; i < elements_count; i++)
                {
                    document.getElementsByClassName("chat-spacer")[0].remove();
                }

                let html_to_append1 = "<div class=\"chat-spacer\"></div>";
                document.getElementById(received_direct_message_chat_context_id).insertAdjacentHTML("beforeend", html_to_append1);

                if (g_current_chat_context_id == received_direct_message_chat_context_id)
                {
                    chat__scroll_chat_to_end(false);
                }
            }
            if (g_current_chat_context_id != received_direct_message_chat_context_id)
            {
                chat__increment_unread_count(msg.message.sender_id);
                chat__remember_message_awaiting_receipt(msg.message.sender_id, msg.message.server_chat_message_id);
                chat__render_unread_badge(msg.message.sender_id, true);
            }
            else
            {
                // the conversation is open. send only if the screen is actually on, otherwise owe it
                if (chat__is_the_user_actually_looking() == true)
                {
                    chat__send_seen_receipt_for_message(msg.message.sender_id, msg.message.server_chat_message_id);
                }
                else
                {
                    chat__remember_message_awaiting_receipt(msg.message.sender_id, msg.message.server_chat_message_id);
                }
            }
            g_chat_context_array[chat_context_index].last_known_message_sender_username = msg.message.sender_username;
        }
        // the other side read a private message of ours. the server routed this like any
        // other direct message and never saw what it says
        else if (msg.message.some_json.type == "message_seen")
        {
            // the eye is off, so the receipt is dropped without touching the message
            if (g_show_seen_indicator == false)
            {
                return;
            }

            chat__mark_message_as_seen(msg.message.sender_id);
        }
        else if (msg.message.some_json.type == "channel_keys_from_maintainer")
        {
            let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);

            // unguarded -1 crashed exactly here on device and the keys were never set
            if (index == -1)
            {
                console.warn("channel keys arrived for unknown channel " + g_current_channel_id + ", ignoring");
                return;
            }

            if (g_channel_list[index].has_maintainer && g_channel_list[index].maintainer_id == msg.message.sender_id)
            {
                console.log("received keys for channel '" + g_channel_list[index].name + "' from user : '" + msg.message.sender_username + "'. Setting new value of current_channel_keys");
                g_current_channel_keys = JSON.parse(msg.message.some_json.value);
                console.log("current_channel_keys -> ", g_current_channel_keys);

                // the workers run their own copy of every global, so the new channel keys are posted
                // to the data-processing and opus decoder workers explicitly

                g_data_processing_worker.postMessage({
                    type: "mainthread__channel_keys_for_data_processing_worker",
                    value: g_current_channel_keys
                });

                g_opus_decoder_worker.postMessage({
                    type: "mainthread__channel_keys_for_opus_decoder",
                    value: g_current_channel_keys
                });

                // valid keys arrived from the announced maintainer - stop the reset countdown
                keys__cancel_maintainer_keys_wait_timer();

            }
            else
            {
                console.log("received keys for channel '" + g_channel_list[index].name + "' from user : '" + msg.message.sender_username + "' The user is not maintainer of current channel. Refusing to set keys");
                console.log("channel_list[index].maintainer_id = " + g_channel_list[index].maintainer_id);
                console.log("msg.message.sender_id = " + msg.message.sender_id);
            }
        }
    },
    /**
     * @brief a private chat picture arrived: fills its placeholder with the decrypted image and scrolls down
     *
     * @param object msg -> the server message, msg.message holds picture_id and decrypted_base64_picture
     *
     * @return void
     */
    process_direct_chat_picture_from_server: function(msg)
    {
        chat_files__finish_chat_picture_progress(msg.message.picture_id);

        if (document.getElementById("chat-picture-img-" + msg.message.picture_id) != null)
        {
            document.getElementById("chat-picture-img-" + msg.message.picture_id).src = msg.message.decrypted_base64_picture;
            chat__scroll_chat_to_end(false);
            document.getElementById("chat-picture-img-" + msg.message.picture_id).classList.remove("chat-picture-img-default");
        }
    },
    /**
     * @brief a channel chat picture arrived: fills its placeholder with the decrypted image and scrolls down
     *
     * @param object msg -> the server message, msg.message holds picture_id and decrypted_base64_picture
     *
     * @return void
     */
    process_channel_chat_picture_from_server: function(msg)
    {
        chat_files__finish_chat_picture_progress(msg.message.picture_id);

        if (document.getElementById("chat-picture-img-" + msg.message.picture_id) != null)
        {
            document.getElementById("chat-picture-img-" + msg.message.picture_id).src = msg.message.decrypted_base64_picture;
            document.getElementById("chat-picture-img-" + msg.message.picture_id).classList.remove("chat-picture-img-default");
            chat__scroll_chat_to_end(false);
        }
    },
    /**
     * @brief the decrypted body of a private chat file arrived: the card gets its download button
     *
     * @param object msg -> the server message, msg.message holds file_id and the file
     *
     * @return void
     */
    process_direct_chat_file_from_server: function(msg)
    {
        chat_files__finish_chat_file_card(msg.message.file_id, msg.message.file);
    },
    /**
     * @brief the decrypted body of a channel file arrived: the card gets its download button
     *
     * @param object msg -> the server message, msg.message holds file_id and the file
     *
     * @return void
     */
    process_channel_chat_file_from_server: function(msg)
    {
        chat_files__finish_chat_file_card(msg.message.file_id, msg.message.file);
    },
    /**
     * @brief a file body arrived but no key opened it: says so on the card instead of spinning forever
     *
     * @param object msg -> the server message, msg.message.file_id names the transfer
     *
     * @return void
     */
    process_chat_file_decrypt_failed_from_server: function(msg)
    {
        delete g_chat_file_transfers[msg.message.file_id];
        chat_files__mark_chat_file_card_failed(chat_files__get_chat_file_card_by_key(msg.message.file_id), "could not decrypt this file");
    },
    /**
     * @brief the server refused our file and says why: drops the upload lock, tells the user, marks the card
     *
     * @param object msg -> the server message, msg.message holds reason, file_upload_max_size and local_message_id
     *
     * @return void
     */
    process_file_send_error_from_server: function(msg)
    {
        chat__release_file_upload_lock();
        g_file_send_intent = "";
        g_file_send_intent_extra_data = {};

        let text = chat_files__explain_file_send_error(msg.message.reason, msg.message.file_upload_max_size);

        utils__custom_alert(text);
        chat_files__mark_local_chat_file_card_failed(msg.message.local_message_id, text);
    },
    /**
     * @brief our picture upload was relayed: strips the "imgnotsentyet" marker class from every element carrying it
     *
     * @param object msg -> the server message
     *
     * @return void
     */
    process_image_sent_status_from_server: function(msg)
    {
        chat__hide_picture_delivery_status();

        var lights = document.getElementsByClassName("imgnotsentyet");
        while (lights.length)
        {
            lights[0].classList.remove("imgnotsentyet");
        }
    },
    /**
     * @brief a client went idle: moves them into the idle list (channel_id -2, is_idle true), re-renders their row there with handlers, plays the leave sound and stops our own audio sending
     *
     * @param object msg -> the server message, msg.message.client_id names the client
     *
     * @return void
     */
    process_client_client_going_to_idle_mode_from_server: function(msg)
    {
        let client_old_channel_id = channel_tree__get_client_by_client_id(msg.message.client_id).channel_id;
        let local_client_channel_id = channel_tree__get_client_by_client_id(g_local_client_id).channel_id;

        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].channel_id = -2;
        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].is_idle = true;

        let single_client = g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)];

        if (document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]') != null)
        {
            document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]').remove();
        }

        if (g_local_client_id != single_client.client_id)
        {
            html_to_append = channel_tree__generate_html_for_single_client(single_client, false);
        }
        else
        {
            html_to_append = channel_tree__generate_html_for_single_client(single_client, true);
        }

        channel_tree__get_channel_own_clients_last_element("idle").insertAdjacentHTML("afterend", html_to_append);

        let element = document.querySelector('.connected-client[data-connected-client-id="'+msg.message.client_id+'"]');

        if (g_is_client_running_under_touch_device)
        {
            let local_touch_press_timer = null; // for touch devices

            element.addEventListener("touchstart", (event) => {

                is_long_press = false;

                // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                // by the time setTimeout runs, event object (or at least part of it) is lost,
                // things from event object must be imediatelly stored in temp variable in this case, for later use

                const currentTarget = event.currentTarget;
                const clientY = event.touches[0].clientY;
                const clientX = event.touches[0].clientX;

                local_touch_press_timer = window.setTimeout( () => {

                    is_long_press = true;
                    UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                },
                600, event);

            });

            element.addEventListener("touchend", (event) => {

                clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                const currentTarget = event.currentTarget;
                const clientY = event.changedTouches[0].pageY;
                const clientX = event.changedTouches[0].pageX;

                if (is_long_press == false)
                {
                    UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget, true);
                }
            });
        }
        else
        {
            element.addEventListener("mousedown", UI.connected_user_onmousedown);
        }

        if (g_local_client_id != msg.message.client_id && local_client_channel_id == client_old_channel_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.user_left_your_channel.play();
            }
        }

        // stop sending audio if it was being sent
        voice__process_stop_sending_audio();

    },
    /**
     * @brief a client was renamed: updates the username in g_client_list, on their row (or the local input for ourselves) and on their chat context pill if one exists
     *
     * @param object msg -> the server message, msg.message holds client_id and new_username
     *
     * @return void
     */
    process_client_rename_from_server: function(msg)
    {
        let client_index = channel_tree__get_client_index_in_array_by_client_id(parseInt(msg.message.client_id));
        if (client_index == -1)
        {
            return;
        }
        utils__custom_log(g_client_list[client_index].username + ' renamed to -> ' + msg.message.new_username);
        g_client_list[client_index].username = msg.message.new_username;

        if (msg.message.client_id != g_local_client_id)
        {
            // null-guarded: a client in a hidden channel has no painted row
            let renamed_row = document.querySelector('[data-connected-client-id="' + msg.message.client_id + '"]');

            if (renamed_row != null && renamed_row.getElementsByClassName("connected-client-p")[0] != null)
            {
                renamed_row.getElementsByClassName("connected-client-p")[0].textContent = msg.message.new_username;
            }
        }
        else
        {
            // our own chat messages render from g_local_username, so keep it in sync
            g_local_username = chat__sanitize_string(msg.message.new_username);
            { let rename_input = document.getElementById("connected-local-client-input"); if (rename_input != null) { rename_input.value = msg.message.new_username; } }
        }

        let is_found = document.querySelector('[data-chat-context-selector-id="user-' + msg.message.client_id + '"]');
        if (is_found != null)
        {
            // the pill is labelled with the alias when there is one, so a rename must not
            // paint the username over it
            is_found.getElementsByClassName("p-container")[0].textContent = channel_tree__get_display_name_by_client_id(msg.message.client_id, msg.message.new_username);
        }

        // messages already on screen keep the old name without this
        android_host__retag_rendered_messages_of_sender(msg.message.client_id, channel_tree__get_display_name_by_client_id(msg.message.client_id, msg.message.new_username));
    },
    /**
     * @brief a client's avatar arrived: applied to every ui spot showing that client
     *
     * @param object msg -> the server message, msg.message holds client_id and base64_avatar
     *
     * @return void
     */
    process_client_avatar_from_server: function(msg)
    {
        channel_tree__apply_avatar_to_ui(msg.message.client_id, msg.message.base64_avatar);
    },
    /**
     * @brief a client's avatar changed (no image payload): the cached copy is forgotten and re-fetched
     *
     * @param object msg -> the server message, msg.message.client_id names the client
     *
     * @return void
     */
    process_avatar_changed_from_server: function(msg)
    {
        if (g_server_policy.allow_avatars == false) { return; }
        let client_object = channel_tree__get_client_by_client_id(msg.message.client_id);
        if (client_object != null) { client_object.base64_avatar = null; }
        channel_tree__request_single_avatar(msg.message.client_id);
    },
    /**
     * @brief the client list, once per session: fills g_client_list (and the id map), identifies the local client, renders every visible row with handlers, then kicks off the avatar lazy-loading
     *
     * @param object msg -> the server message, msg.message.clients holds the clients, local_username names us
     *
     * @return void
     */
    process_client_list_from_server: function(msg)
    {
        if (G_HAS_DOM == false)
        {
            return;
        }

        if (g_is_client_list_retrieved == true)
        {
            utils__custom_log("client_list received more than once. Server is doing something weird");
            return;
        }

        console.log(msg);

        g_is_client_list_retrieved = true;

        for (let i = 0; i < msg.message.clients.length; i++)
        {
            let username = chat__sanitize_string(msg.message.clients[i].username);
            let client_id = msg.message.clients[i].client_id;

            // clients channel is hidden (-1)
            if (msg.message.clients[i].channel_id == -1)
            {
                let single_client = {
                    client_id: null,
                    username: null,
                    alias: null,
                    public_key: null,
                    channel_id: null,
                    audio_state: null,
                    tag_ids: null,
                    is_clients_channel_hidden: null,
                    country_iso_code: null,
                    is_idle: null,
                    is_ignored_by_local_client: null,
                    is_muted_by_local_client: null,
                    unread_count: 0,
                    is_music_bot: null
                };

                single_client.client_id = parseInt(client_id);
                single_client.username = username;
                single_client.alias = (msg.message.clients[i].alias != null) ? msg.message.clients[i].alias : "";
                single_client.public_key = msg.message.clients[i].public_key;
                single_client.channel_id = msg.message.clients[i].channel_id;
                single_client.audio_state = msg.message.clients[i].audio_state;
                single_client.tag_ids = msg.message.clients[i].tag_ids;
                single_client.is_clients_channel_hidden = msg.message.clients[i].is_clients_channel_hidden;
                single_client.country_iso_code = msg.message.clients[i].country_iso_code;
                single_client.is_idle = msg.message.clients[i].is_idle;
                single_client.is_ignored_by_local_client = false;
                single_client.is_muted_by_local_client = false;
                single_client.unread_count = 0;
                single_client.is_music_bot = msg.message.clients[i].is_music_bot;

                g_client_list.push(single_client);
                g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);
            }
            else if (msg.message.clients[i].is_idle == true)
            {

                let single_client = {
                    client_id: null,
                    username: null,
                    alias: null,
                    public_key: null,
                    channel_id: null,
                    audio_state: null,
                    tag_ids: null,
                    is_clients_channel_hidden: null,
                    country_iso_code: null,
                    is_idle: null,
                    is_ignored_by_local_client: null,
                    is_muted_by_local_client: null,
                    unread_count: 0,
                    is_music_bot: null
                };

                single_client.client_id = parseInt(client_id);
                single_client.username = username;
                single_client.alias = (msg.message.clients[i].alias != null) ? msg.message.clients[i].alias : "";
                single_client.public_key = msg.message.clients[i].public_key;
                single_client.channel_id = msg.message.clients[i].channel_id;
                single_client.audio_state = msg.message.clients[i].audio_state;
                single_client.tag_ids = msg.message.clients[i].tag_ids;
                single_client.is_clients_channel_hidden = msg.message.clients[i].is_clients_channel_hidden;
                single_client.country_iso_code = msg.message.clients[i].country_iso_code;
                single_client.is_idle = msg.message.clients[i].is_idle;
                single_client.is_ignored_by_local_client = false;
                single_client.is_muted_by_local_client = false;
                single_client.unread_count = 0;
                single_client.is_music_bot = msg.message.clients[i].is_music_bot;

                g_client_list.push(single_client);
                g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);

                let indentation_level = 1;

                if (username != chat__sanitize_string(msg.message.local_username))
                {
                    html_to_append = channel_tree__generate_html_for_single_client(msg.message.clients[i], false);
                }
                else
                {
                    // this else statement will never run
                    g_local_username = chat__sanitize_string(msg.message.local_username);
                    g_local_client_id = msg.message.clients[i].client_id;
                    html_to_append = channel_tree__generate_html_for_single_client(msg.message.clients[i], true);
                }

                document.querySelector('[data-channel-id="idle"]').insertAdjacentHTML("afterend", html_to_append);
            }
            else
            {
                let html_to_append = "";

                if (username != chat__sanitize_string(msg.message.local_username))
                {
                    html_to_append = channel_tree__generate_html_for_single_client(msg.message.clients[i], false);
                }
                else
                {
                    g_local_username = chat__sanitize_string(msg.message.local_username);
                    g_local_client_id = msg.message.clients[i].client_id;
                    html_to_append = channel_tree__generate_html_for_single_client(msg.message.clients[i], true);
                }

                document.querySelector('[data-channel-id="' + msg.message.clients[i].channel_id + '"]').insertAdjacentHTML("afterend", html_to_append);

                let single_client = {
                    client_id: null,
                    username: null,
                    alias: null,
                    public_key: null,
                    channel_id: null,
                    audio_state: null,
                    tag_ids: null,
                    is_clients_channel_hidden: null,
                    country_iso_code: null,
                    is_idle: null,
                    is_ignored_by_local_client: null,
                    is_muted_by_local_client: null,
                    unread_count: 0,
                    is_music_bot: null
                };

                // they killed the subcultures they cancelled the future and for what, for you to not be offended

                single_client.audio_state = msg.message.clients[i].audio_state;
                single_client.tag_ids = msg.message.clients[i].tag_ids;
                single_client.is_clients_channel_hidden = false;
                single_client.client_id = parseInt(client_id);
                single_client.username = username;
                single_client.alias = (msg.message.clients[i].alias != null) ? msg.message.clients[i].alias : "";
                single_client.public_key = msg.message.clients[i].public_key;
                single_client.channel_id = msg.message.clients[i].channel_id;
                single_client.country_iso_code = msg.message.clients[i].country_iso_code;
                single_client.is_idle = msg.message.clients[i].is_idle;
                single_client.is_ignored_by_local_client = false;
                single_client.is_muted_by_local_client = false;
                single_client.unread_count = 0;
                single_client.is_music_bot = msg.message.clients[i].is_music_bot;

                g_client_list.push(single_client);
                g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);
            }
        }

        for (let x = 0; x < g_chat_context_array.length; x++)
        {
            if (g_chat_context_array[x].type == "user")
            {
                for (let y = 0; y < msg.message.clients.length; y++)
                {
                    // .client_id, not .id: the payload has no id field, and a comparison that never
                    // matched would delete every open pm context on every client_list
                    if (g_chat_context_array[x].chat_context_id == "chat-context-pm-" + msg.message.clients[y].client_id)
                    {
                        break;
                    }
                    else
                    {
                        if (y + 1 == msg.message.clients.length)
                        {
                            utils__custom_log("chat context with id " + g_chat_context_array[x].chat_context_id.toString() + " NOT FOUND");
                            document.getElementById(g_chat_context_array[x].chat_context_id).remove();
                        }
                    }
                }
            }
        }

        let elements = document.getElementsByClassName('connected-client');
        for (let i = 0; i < elements.length; i++)
        {
            if (g_is_client_running_under_touch_device)
            {
                let local_touch_press_timer = null; // for touch devices

                elements[i].addEventListener("touchstart", (event) => {

                    is_long_press = false;

                    // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                    // by the time setTimeout runs, event object (or at least part of it) is lost,
                    // things from event object must be imediatelly stored in temp variable in this case, for later use

                    const currentTarget = event.currentTarget;
                    const clientY = event.touches[0].clientY;
                    const clientX = event.touches[0].clientX;

                    local_touch_press_timer = window.setTimeout( () => {

                        is_long_press = true;
                        UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                    },
                    600, event);

                });

                elements[i].addEventListener("touchend", (event) => {

                    clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                    const currentTarget = event.currentTarget;
                    const clientY = event.changedTouches[0].pageY;
                    const clientX = event.changedTouches[0].pageX;

                    if (is_long_press == false)
                    {
                        UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget, true);
                    }
                });

            }
            else
            {
                elements[i].addEventListener("mousedown", UI.connected_user_onmousedown);
            }

            elements[i].style.backgroundColor = "";
        }
        { let rename_input = document.getElementById('connected-local-client-input'); if (rename_input != null) { rename_input.addEventListener("focusout", UI.connected_local_user_input_on_focusout); } }

        if (g_is_running_in_android_webview == true)
        {
            Android.JavaExportOnConnected();
        }

        UI.refresh_all_channel_fullness();

        // the full client list is in: lazy-load everyone's avatars in growing chunks
        channel_tree__enqueue_all_avatars_for_loading();

        // themes that do not show the avatar grid get the paced one-at-a-time prefetch instead
        channel_tree__start_avatar_prefetch();

        // the player needs to know which senders are bots before their first audio arrives
        // (the headless node build has no player: audio.js is not part of its bundle)
        if (typeof audio__audio_player_announce_music_bots === "function")
        {
            audio__audio_player_announce_music_bots();
        }

        // a stream already running when the list arrives: its marquee shows when the streamer is in
        // this channel, or wherever it is when the server shows marquees to everyone
        for (let i = 0; i < msg.message.clients.length; i++)
        {
            let listed = msg.message.clients[i];

            if (listed.is_streaming_song != true) { continue; }
            if (g_server_policy.show_music_bot_marquee_to_everyone == false && listed.channel_id != g_current_channel_id) { continue; }

            server_msg.process_start_song_stream_from_server({ message: { client_id: listed.client_id, song_name: listed.song_name } });
        }

        // a saved strip theme is applied before the server policy allowed avatars, so the grid never
        // armed; re-evaluate now that the flag and the clients exist (the refresh bulk-enqueues itself)
        UI.refresh_member_list_state();
    },
    /**
     * @brief the server denied a request: shows its reason when it sent one, the generic line otherwise
     *
     * @param object msg -> the server message, msg.message.reason is the optional text
     *
     * @return void
     */
    process_access_denied_from_server: function(msg)
    {
        utils__custom_alert((typeof msg.message.reason === "string" && msg.message.reason.length > 0)
            ? chat__sanitize_string(msg.message.reason) : "insufficient permissions");
        g_sound_effects.insufficient_permissions.play();
    },
    /**
     * @brief the admin log: fills the log tab's textarea, one dated line per row, newest at the bottom
     *
     * @param object msg -> the server message, msg.message.lines holds the lines
     *
     * @return void
     */
    process_admin_log_from_server: function(msg)
    {
        let textarea = document.getElementById("server-settings-log-textarea");
        let lines = Array.isArray(msg.message.lines) ? msg.message.lines : [];

        textarea.value = lines.join("\n");
        textarea.scrollTop = textarea.scrollHeight;
    },
    /**
     * @brief somebody switched channel: moves their row, replays their tag chips, plays the join, leave or switch sound; for the local client also swaps the chat context and nulls the channel keys
     *
     * @param object msg -> the server message, msg.message holds client_id and channel_id (-1 for a hidden channel)
     *
     * @return void
     */
    process_channel_join_from_server: function(msg)
    {
        let client_that_joined = g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)];
        let client_old_channel_id = client_that_joined.channel_id;
        client_that_joined.channel_id = msg.message.channel_id;

        // check if client went into unknown channel (-1)
        // or known channel

        if (msg.message.channel_id == -1)
        {
            // this is case where client left to other unknown channel
            client_that_joined.is_clients_channel_hidden = true;
            document.querySelector('[data-connected-client-id="' + msg.message.client_id + '"]').remove();
        }
        else
        {
            let local_client_channel_id = channel_tree__get_client_by_client_id(g_local_client_id).channel_id;

            if (local_client_channel_id == msg.message.channel_id && g_local_client_id != msg.message.client_id)
            {
                if (g_are_sound_effects_enabled)
                {
                    g_sound_effects.user_joined_your_channel.play();
                }
            }
            else if (g_local_client_id == msg.message.client_id)
            {
                if (g_are_sound_effects_enabled)
                {
                    g_sound_effects.channel_switched.play();
                }
            }
            else if (g_local_client_id != msg.message.client_id && local_client_channel_id == client_old_channel_id)
            {
                if (g_are_sound_effects_enabled)
                {
                    g_sound_effects.user_left_your_channel.play();
                }
            }

            if (msg.message.client_id == g_local_client_id)
            {
                audio__audio_player_clear();

                // leaving a channel drops its marquees, unless the server shows them to everyone
                if (g_server_policy.show_music_bot_marquee_to_everyone == false)
                {
                    let marquee_containers = document.getElementsByClassName("marquee-music-playing-container");
                    for (let i = 0; i < marquee_containers.length; i++)
                    {
                        marquee_containers[i].style.display = "none";
                    }
                }

                document.getElementById("channel-password-enter-container").style.display = "none";
                document.getElementById("background-container").style.display = "none";
                document.getElementsByClassName("connected-local-client")[0].remove();

                html_to_append = channel_tree__generate_html_for_single_client(client_that_joined, true);

                channel_tree__get_channel_own_clients_last_element(msg.message.channel_id).insertAdjacentHTML("afterend", html_to_append);
                { let rename_input = document.getElementById('connected-local-client-input'); if (rename_input != null) { rename_input.addEventListener("focusout", UI.connected_local_user_input_on_focusout); } }

                g_current_channel_id = msg.message.channel_id;
                g_current_channel_keys = null;

                console.log("local_user joined new channel, nulling out current_channel_keys");

                let is_found = document.querySelector('[data-chat-context-selector-id="channel-' + msg.message.channel_id + '"]');

                if (is_found == null)
                {
                    let channel_name = document.querySelector('[data-channel-name-id="' + msg.message.channel_id + '"]').innerHTML;
                    let to_append = "<div class=\"chat-context-selector\" data-chat-context-selector-type=\"channel\" data-chat-context-selector-id=\"channel-" + msg.message.channel_id + "\">\n\
                                        <div class=\"p-container\">\n\
                                            <p>" + channel_name + "</p>\n\
                                        </div>\n\
                                        <div class=\"remove-chat-context-selector\" data-chat-context-remove-selector-type=\"channel\" data-chat-context-remove-selector-id=\"channel-" + msg.message.channel_id + "\">\n\
                                        </div>\n\
                                    </div>";

                    document.getElementById("chat-context-selectors-container").insertAdjacentHTML("beforeend", to_append);

                    for (let x = 0; x < document.getElementsByClassName("remove-chat-context-selector").length; x++)
                    {
                        document.getElementsByClassName("remove-chat-context-selector")[x].onclick = UI.chat_context_remove_onclick;
                    }

                    for (let x = 0; x < document.getElementsByClassName("chat-context-selector").length; x++)
                    {
                        document.getElementsByClassName("chat-context-selector")[x].onclick = UI.chat_context_selector_onclick;
                    }
                }

                for (let i = 0; i < document.getElementsByClassName("chat-context-selector").length; i++)
                {
                    document.getElementsByClassName("chat-context-selector")[i].style.backgroundColor = "";
                }

                let elements1 = document.getElementsByClassName('connected-client');
                for (let i = 0; i < elements1.length; i++)
                {
                    elements1[i].style.backgroundColor = "";
                }

                document.querySelector('[data-chat-context-selector-id="channel-' + msg.message.channel_id + '"]').style.backgroundColor = "#36393f";

                // g_chat_context_array is the source of truth, not the dom. asking the dom whether the
                // context exists ties state creation to rendering, which breaks with no dom present
                let joined_channel_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
                let is_chat_context_existing = false;

                for (let context_index = 0; context_index < g_chat_context_array.length; context_index++)
                {
                    if (g_chat_context_array[context_index].chat_context_id == joined_channel_chat_context_id)
                    {
                        is_chat_context_existing = true;
                        break;
                    }
                }

                if (is_chat_context_existing == false)
                {
                    console.log("adding channel");

                    // the channel name is state. taking it out of the row's markup made the
                    // context label depend on the row having been painted first. chat__sanitize_string
                    // here yields exactly what that markup already held, so the text is unchanged
                    let joined_channel = channel_tree__get_channel_by_id(g_channel_list, msg.message.channel_id);
                    let channel_name = (joined_channel != null) ? chat__sanitize_string(joined_channel.name) : "";

                    let html_to_append3 = '<div class="chat-context" id="chat-context-channel-' + msg.message.channel_id + '" style="display: none;">\n\
                                                <div class="single-server-message">now talking channel: ' + channel_name + '</div>\n\
                                                    </div>\n\
                                                </div>\n\
                                            </div>';

                    // the sibling insert is kept rather than switched to a container append, so
                    // the node lands in the same place in the browser. only the blind [count - 1]
                    // is guarded, which is what made this throw with no contexts painted
                    let count = document.getElementsByClassName("chat-context").length;

                    if (count > 0)
                    {
                        document.getElementsByClassName("chat-context")[count - 1].insertAdjacentHTML("afterend", html_to_append3);
                    }

                    let single_chat_context = {
                        type: "channel",
                        chat_context_id: joined_channel_chat_context_id,
                        last_known_message_sender_username: ""
                    };

                    g_chat_context_array.push(single_chat_context);
                }

                let count1 = document.getElementsByClassName("chat-context").length;

                for (let i = 0; i < count1; i++)
                {
                    document.getElementsByClassName("chat-context")[i].style.display = "none";
                }

                document.getElementById("chat-context-channel-" + msg.message.channel_id).style.display = "block";

                g_current_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
        chat__clear_channel_unread_count(msg.message.channel_id); // opened it, so it is read
                g_chat_message_receiver_type = "channel";
            g_offline_chat_recipient_alias = ""; // back on a channel: no offline target
                console.log("joined new channel, nulling out current_channel_keys");
                g_current_channel_keys = null;

                // joined new channel, now loop through clients and set microphone state to 2 for all clients that have it set to 1
                // only clients in current channel should have microphone state set to 1
                // information about active mic state of clients in new channel is later received through websocket and processed

                for (var i = 0; i < g_client_list.length; i++)
                {
                    if (g_client_list[i].audio_state == G_AUDIO_STATE.PUSH_TO_TALK_ACTIVE)
                    {
                        g_client_list[i].audio_state = G_AUDIO_STATE.PUSH_TO_TALK_ENABLED; // not active but enabled

                        let client = {
                            client_id: g_client_list[i].client_id,
                            audio_state: g_client_list[i].audio_state
                        };

                        voice__process_audio_state_of_single_client(client);
                    }
                }

                UI.enable_inputs();
            }
            else
            {
                // this is branch that gets run if client that joined new channel isnt local client
                // this code tries to remove client in tree visually and create him later

                // only remove clients entry if client was not in hidden channel before joining new channel
                if (client_that_joined.is_clients_channel_hidden == false)
                {
                    document.querySelector('[data-connected-client-id="' + msg.message.client_id + '"]').remove();
                }
                else
                {
                    // client was in hidden channel before
                    client_that_joined.is_clients_channel_hidden = false;
                }

                html_to_append = channel_tree__generate_html_for_single_client(client_that_joined, false);

                channel_tree__get_channel_own_clients_last_element(msg.message.channel_id).insertAdjacentHTML("afterend", html_to_append);

                if (msg.message.channel_id == g_current_channel_id)
                {
                    console.log("user '" + client_that_joined.username + "' joined current_channel where local_user is. setting current_channel_keys to null");
                    g_current_channel_keys = null;
                    let channel_index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);
                    if (g_channel_list[channel_index].has_maintainer && g_channel_list[channel_index].maintainer_id == g_local_client_id)
                    {
                        console.log("local_user is the maintainer of current_channel. keys__create_and_send_new_channel_keys()");
                        keys__create_and_send_new_channel_keys();
                    }
                    else
                    {
                        console.log("local user is not maintainer of current channel. Waiting for new keys");

                        // somebody joined -> the maintainer must re-key the whole channel;
                        // if the fresh keys never arrive, vote for a maintainer reset
                        keys__arm_maintainer_keys_wait_timer();
                    }
                }
            }

            // html element that represents client is now appended to newly joined channel at this point
            // now append client tags to client again

            for (let y = 0; y < client_that_joined.tag_ids.length; y++)
            {
                let tag = channel_tree__get_tag_by_tag_id(client_that_joined.tag_ids[y]);
                if (tag == null)
                {
                    console.log("tag is null");
                    continue;
                }
                let target_element = document.getElementById("client-tags-" + client_that_joined.client_id);
                const node = document.createElement("div");
                node.className = "single-tag";
                node.setAttribute("tag-id", tag.tag_id);

                // same guard as everywhere else: this is the path that runs when somebody switches
                // channel, which is why an unassigned icon used to appear only after a channel change
                let icon = (tag.has_icon == true) ? channel_tree__get_icon_by_icon_id(tag.tag_linked_icon_id) : null;
                if (icon != null)
                {
                    node.style.backgroundImage = "url("+icon.base64_icon+")";
                }
                target_element.appendChild(node);
            }

            let elements = document.getElementsByClassName('connected-client');

            for (let i = 0; i < elements.length; i++)
            {
                if (g_is_client_running_under_touch_device)
                {
                    let local_touch_press_timer = null; // for touch devices

                    elements[i].addEventListener("touchstart", (event) => {

                        is_long_press = false;

                        // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                        // by the time setTimeout runs, event object (or at least part of it) is lost,
                        // things from event object must be imediatelly stored in temp variable in this case, for later use

                        const currentTarget = event.currentTarget;
                        const clientY = event.touches[0].clientY;
                        const clientX = event.touches[0].clientX;

                        local_touch_press_timer = window.setTimeout( () => {

                            is_long_press = true;
                            UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                        },
                        600, event);

                    });

                    elements[i].addEventListener("touchend", (event) => {

                        clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                        const currentTarget = event.currentTarget;
                        const clientY = event.changedTouches[0].pageY;
                        const clientX = event.changedTouches[0].pageX;

                        if (is_long_press == false)
                        {
                            UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget, true);
                        }
                    });

                }
                else
                {
                    elements[i].addEventListener("mousedown", UI.connected_user_onmousedown);
                }
            }

            if (msg.message.is_streaming_song)
            {
                let element = document.querySelector('.marquee-music-playing-container[data-marquee-music-playing-container-id="' + msg.message.client_id + '"]');
                if (element != null)
                {
                    element.style.display = "inline-block";
                    document.getElementById("marquee-song-name-client-id-" + msg.message.client_id).innerHTML = chat__sanitize_string(msg.message.song_name);
                }
                else
                {
                    console.log("could not find element");
                }
            }
        }

        UI.refresh_all_channel_fullness();
    },
    /**
     * @brief a chat message was deleted: blanked to "deleted" (a picture reset to the placeholder) after checking the requester is the recorded author or an admin
     *
     * @param object msg -> the server message, msg.message holds chat_message_id, requester_public_key and requester_is_admin
     *
     * @return void
     */
    process_chat_message_delete_from_server: function(msg)
    {
        // the server tells us WHO asked for the delete; only honour it if that requester is the message's
        // author (recorded at render) or an admin. otherwise this client keeps the message - its prerogative.
        let recorded_author_public_key = g_chat_message_author_public_keys[msg.message.chat_message_id];
        if (recorded_author_public_key !== undefined && recorded_author_public_key !== msg.message.requester_public_key && msg.message.requester_is_admin !== true)
        {
            return;
        }

        // first try to find chat message under chat_message id, if not found try to find picture

        let element = document.querySelector('.single-chat-message-content-p[data-single-chat-message-server-message-id="' + msg.message.chat_message_id + '"]');
        if (element != null)
        {
            element.removeEventListener("mousedown", UI.single_chat_message_onrightclick);
            element.innerHTML = "deleted";
            element.style.fontStyle = "italic";
            element.style.fontSize = "10px";

            if (element.classList.contains("local-single-chat-message-content-p"))
            {
                element.classList.remove("local-single-chat-message-content-p");
            }
        }
        else
        {
            let element = document.querySelector('.chat-picture-img[data-single-chat-message-server-message-id="' + msg.message.chat_message_id + '"]');
            if (element != null)
            {
                element.removeEventListener("mousedown", UI.single_chat_message_onrightclick);
                element.src = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABgAAAAYCAYAAADgdz34AAADF0lEQVRIx71VsU5cRxQ9d98s1UoUISYFJUIgpUDySiloUIqUTmeUwo27lPmCOHWadEhUFBRQOK7sD9hNEcWhoDG4iGQs2ayNFBuZArN7z0lx580+bC/QxCPtvpl5896599xz7gP+52HXObS1tbU2Pz//A0mQhLtrfX39x+3t7cFVz6brAHQ6na+Xl5e/H7nDR47z4ZAkfwEwuFYGm5ubt6anpxcllRv1nCTm5ua+W1xc/NadcLr5yPXw0cPfOp3OwPJZCRAESKCEfq93f2Nj458EAEtLS3e73e4tAKYYUJ5DgiSTJMYcorB2e+0nibFHGSVJhChQsr8fP34NIABUPxiL8fqDXxMg1uWFUHNOQpmdBAC7u7t/npycfLWwsPBNRAsAJXIogrAxOPK+TBTUOPv27cnp88PDv07fnb4oADs7O7+urKz0V1dX/wDqRGTjUsiCYqGmsXENwEDH3t7e8wcPfr99fHz8BgBaANDr9TylNEwpIaWEKiVUVYXhcIiUKqSUkKqE0WgEM4szVYK1WhgOR6hShVRVqKqEqqp0fn7+vt/vswA0R6bCXr16jSdP9s3dCx37+wf28uio0DE4GtjBwYGJrPesIcSPfZD5VkiNBkAkYWYmIERFZqXBSCoX31BUqssBMrehiHhZDWzh5LqgAEXziD48VXvhUwAKjyAHConmdBXVQPJoFcZANFGiO0QZLGc4OQMVJ0oAKdDZ8IhA99B7jqbuTbFGLdcrKJIsdE+Q3qBIRnr9Qqvr5O4Xqb0MIBtJAkDK3KMVtESTIHeClCk4MpIivQhCQs3zJSqqG1dkcGGvkUFuGYR7tAjYuOlNKHL0EdQKIUF3EwW2BAhGJyhGxHEmaIsalFZyVQ2Ui2zurJVhkuT0D3wgRQ1o4+Z7GQBrJQFTU220qhbMrOynlNBuT5V1u91GSiloEUNpkwBUnBxtYGbmS3wxM2NN6m52uyjOlTA7O2s3Zm+gkeXkDBp9P2CUrVNAiwMtT8cfJ8nGBp0AcPjs2dN7936+I6CdG/ZH31dd/PvkvbOzs39JvsfnGv8B0U6eGZr+co0AAAAASUVORK5CYII=";
                element.classList.add("chat-picture-img-default");
            }
            else
            {
                let file_card = document.querySelector('.chat-file-card[data-single-chat-message-server-message-id="' + msg.message.chat_message_id + '"]');

                if (file_card != null)
                {
                    chat_files__mark_chat_file_card_deleted(file_card);
                }
                else
                {
                    console.log("process_chat_message_delete_from_server unable to find element");
                }
            }
        }
    },
    /**
     * @brief a poke: shows its text in an alert with the poke sound, unless the poker is ignored
     *
     * @param object msg -> the server message, msg.message holds client_id and poke_message
     *
     * @return void
     */
    process_poke_from_server: function(msg)
    {
        // the sender may be missing from the client list (hidden password channel, or a poke that
        // lands before the list is built); a null entry must not kill the handler
        let poke_sender = channel_tree__get_client_by_client_id(msg.message.client_id);

        if (poke_sender != null && poke_sender.is_ignored_by_local_client == true)
        {
            return;
        }

        if (g_are_sound_effects_enabled)
        {
            g_sound_effects.poke.play();
        }
        let poke_sender_name = channel_tree__get_display_name_by_client_id(msg.message.client_id, "");
        let string1 = "" + chat__sanitize_string(poke_sender_name) + " says : " + chat__sanitize_string(msg.message.poke_message);
        utils__custom_alert(string1);

        // in the wrapper app the alert above is drawn inside a webview nobody is looking
        // at while the app is in the background - which is when a poke actually matters.
        // hand it to android too, it decides whether a notification is needed
        if (g_is_running_in_android_webview == true && typeof Android !== "undefined")
        {
            try { Android.JavaExportShowPokeNotification(poke_sender_name, msg.message.poke_message); }
            catch (e) { console.warn("poke notification bridge failed: " + e.message); }
        }
    },
    /**
     * @brief a chat message was edited: the text is replaced in place (shown pink) if the requester is the recorded author or an admin
     *
     * @param object msg -> the server message, msg.message holds chat_message_id, new_message_value, requester_public_key and requester_is_admin
     *
     * @return void
     */
    process_chat_message_edit_from_server: function(msg)
    {
        let recorded_author_public_key = g_chat_message_author_public_keys[msg.message.chat_message_id];
        if (recorded_author_public_key !== undefined && recorded_author_public_key !== msg.message.requester_public_key && msg.message.requester_is_admin !== true)
        {
            return;
        }

        let element = document.querySelector('.single-chat-message-content-p[data-single-chat-message-server-message-id="' + msg.message.chat_message_id + '"]');
        if (element != null)
        {
            element.innerHTML = chat__sanitize_string(msg.message.new_message_value);
            element.style.color = "pink";
        }
    },
    /**
     * @brief a new icon: pushed into g_icons with its settings entry; if this reply is our in-flight upload, the next queued icon upload goes out
     *
     * @param object msg -> the server message, msg.message holds icon_id and base64_icon
     *
     * @return void
     */
    process_icon_add_from_server: function(msg)
    {
        let icon = {
            id: msg.message.icon_id,
            base64_icon: msg.message.base64_icon
        }

        g_icons.push(icon);

        let html_to_append = "<div class='server-settings-icon-entry' data-icon-id="+icon.id+"><img class='img-uploaded-icon' src="+icon.base64_icon+"></img><button class='settings-entry-delete-button' title='delete icon'>✕</button></div>";

        document.getElementById("server-settings-tab-icons-container").insertAdjacentHTML("beforeend", html_to_append);

        // batch upload: if this reply is the icon we just sent, send the next queued one
        if (g_icon_upload_in_flight_base64 != null && msg.message.base64_icon == g_icon_upload_in_flight_base64)
        {
            g_icon_upload_in_flight_base64 = null;
            server_settings_tab__send_next_queued_icon_upload();
        }
    },
    /**
     * @brief a tag was taken off a client: drops the id from their tag_ids and removes its chip from their row
     *
     * @param object msg -> the server message, msg.message holds client_id and tag_id
     *
     * @return void
     */
    process_remove_tag_from_client_from_server: function(msg)
    {
        console.log(msg);
        let client = channel_tree__get_client_by_client_id(msg.message.client_id);

        if (client == null)
        {
            return;
        }

        // do not add duplicate tag ids to client object

        if (client.tag_ids.includes(msg.message.tag_id))
        {
            client.tag_ids.splice(client.tag_ids.indexOf(msg.message.tag_id), 1);  // deleting
        }

        // double selector..first find client tags element
        let client_tags = document.getElementById("client-tags-"+msg.message.client_id);
        let target_element = client_tags.querySelector('.single-tag[tag-id="'+msg.message.tag_id+'"]');
        if (target_element != null)
        {
            target_element.remove();
        }
        else
        {
            console.log("process_remove_tag_from_client_from_server failed to find tag-id:" + msg.message.tag_id);
        }
    },
    /**
     * @brief the identities the server stores, once: [{ alias, base64_avatar, tag_ids }]
     *        it carries no ids or keys; the alias is the handle, and the server keeps aliases unique,
     *        so an entry whose alias matches a connected client IS that client and is not listed again
     *
     * @param object msg -> the server message, msg.message.stored_clients holds the entries
     *
     * @return void
     */
    process_stored_clients_list_from_server: function(msg)
    {
        g_offline_client_list = [];

        if (msg.message.stored_clients == null)
        {
            return;
        }

        for (let i = 0; i < msg.message.stored_clients.length; i++)
        {
            let entry = msg.message.stored_clients[i];

            if (entry == null || typeof entry.alias !== "string" || entry.alias.length == 0)
            {
                continue; // nothing to name or pair it by
            }

            g_offline_client_list.push({
                alias: entry.alias,
                base64_avatar: (typeof entry.base64_avatar === "string") ? entry.base64_avatar : "",
                tag_ids: (entry.tag_ids != null) ? entry.tag_ids : [],
                // unix seconds; only sent when the server has last-seen enabled
                last_seen: (typeof entry.last_seen === "number") ? entry.last_seen : 0,
                // only sent when the server has offline messages enabled. having it IS the
                // permission to write to this person while they are away - without their
                // public key there is no way to encrypt anything for them
                public_key: (typeof entry.public_key === "string") ? entry.public_key : ""
            });
        }

        utils__custom_log("stored clients list: " + g_offline_client_list.length + " people kept of " + msg.message.stored_clients.length + " sent");
        UI.schedule_member_list_sync();
    },

    /**
     * @brief a client's alias changed: updated in g_client_list, on their rows (text and has-alias class), on old messages and an open chat pill, then the member list is synced
     *
     * @param object msg -> the server message, msg.message holds client_id and alias
     *
     * @return void
     */
    process_client_alias_changed_from_server: function(msg)
    {
        let client_object = channel_tree__get_client_by_client_id(msg.message.client_id);
        if (client_object == null)
        {
            return;
        }

        client_object.alias = (msg.message.alias != null) ? msg.message.alias : "";

        // an alias change changes the display name on old messages too
        android_host__retag_rendered_messages_of_sender(msg.message.client_id, channel_tree__get_display_name_by_client_id(msg.message.client_id, client_object.username));

        // update the live rows in place; the member-list strip mirror re-clones them right after
        let rows = document.querySelectorAll('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]');
        for (let i = 0; i < rows.length; i++)
        {
            let alias_p = rows[i].querySelector(".client-alias");
            if (alias_p == null)
            {
                continue;
            }

            alias_p.textContent = client_object.alias;

            if (client_object.alias.length > 0)
            {
                rows[i].classList.add("has-alias");
            }
            else
            {
                rows[i].classList.remove("has-alias");
            }
        }

        // an open chat with this person is labelled by name too - relabel it now instead
        // of leaving the old username sitting there until the chat is reopened
        let open_pill = document.querySelector('[data-chat-context-selector-id="user-' + msg.message.client_id + '"]');
        if (open_pill != null)
        {
            open_pill.getElementsByClassName("p-container")[0].textContent = channel_tree__get_display_name_by_client_id(msg.message.client_id, client_object.username);
        }

        UI.schedule_member_list_sync();
    },

    /**
     * @brief the country code a client's row shows from now on; the server withholds an admin's, so it arrives as "" when they become admin and comes back when they stop being one
     *
     * @param object msg -> the server message, msg.message holds client_id and country_iso_code
     *
     * @return void
     */
    process_client_country_code_changed_from_server: function(msg)
    {
        let client_object = channel_tree__get_client_by_client_id(msg.message.client_id);
        if (client_object == null)
        {
            return;
        }

        client_object.country_iso_code = (msg.message.country_iso_code != null) ? msg.message.country_iso_code : "";

        // repaint the flag on the live rows in place; the member-list strip mirror re-clones them right after
        let rows = document.querySelectorAll('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]');
        for (let i = 0; i < rows.length; i++)
        {
            let flag = rows[i].querySelector(".connected-client-country-flag");
            if (flag != null)
            {
                flag.className = "connected-client-country-flag country-flag-" + client_object.country_iso_code.toLowerCase();
            }
        }

        UI.schedule_member_list_sync();
    },

    /**
     * @brief a tag was given to a client: recorded on the client (no duplicates) and painted as a chip; also reveals the server-settings button when the local client just received the admin tag
     *
     * @param object msg -> the server message, msg.message holds client_id and tag_id
     *
     * @return void
     */
    process_add_tag_to_client_from_server: function(msg)
    {
        let client = channel_tree__get_client_by_client_id(msg.message.client_id);

        if (client == null)
        {
            return;
        }

        // do not add duplicate tag ids to client object

        let tag = channel_tree__get_tag_by_tag_id(msg.message.tag_id);

        // resolve the tag BEFORE recording it - an unknown id used to be pushed onto the
        // client and only then bailed on, leaving an id no later render could resolve
        if (tag == null)
        {
            console.log("tag is null");
            return;
        }

        if (!client.tag_ids.includes(msg.message.tag_id))
        {
            client.tag_ids.push(msg.message.tag_id);
        }

        let target_element = document.getElementById("client-tags-" + msg.message.client_id);

        // a client in a hidden channel has no row to paint on. this used to throw here and
        // skip everything below it, including revealing the local admin's settings button
        if (target_element != null)
        {
            const node = document.createElement("div");
            node.className = "single-tag";
            node.setAttribute("tag-id", tag.tag_id);

            let icon = tag.has_icon ? channel_tree__get_icon_by_icon_id(tag.tag_linked_icon_id) : null;

            if (icon != null)
            {
                node.style.backgroundImage = "url("+icon.base64_icon+")";
            }

            target_element.appendChild(node);
        }

        if (msg.message.client_id == g_local_client_id && msg.message.tag_id == 0) // admin tag id
        {
            document.getElementById("enter-server-settings").style.display = "block";
            document.getElementById("enter-server-settings").onclick = UI.enter_server_settings_onclick;

            // an admin may rename even when renames are off for users, so the input un-greys
            g_is_local_client_admin = true;
            server_settings_tab__apply_rename_policy_to_ui();
        }
    },
    /**
     * @brief a new tag: pushed into g_tags and appended to the settings tag table
     *
     * @param object msg -> the server message, msg.message holds the tag's fields
     *
     * @return void
     */
    process_tag_add_from_server: function(msg)
    {
        let tag = msg.message;

        g_tags.push(tag);

        let icon = tag.has_icon ? channel_tree__get_icon_by_icon_id(tag.tag_linked_icon_id) : null;

        let base64_icon = "";

        if (icon != null)
        {
            base64_icon = icon.base64_icon;
        }

        let tag_delete_button_html = (tag.tag_id != 0) ? "<button class=\"settings-entry-delete-button\" title=\"delete tag\">✕</button>" : "";
        let html_to_append = "<div class=\"server-settings-tag-entry\" data-tag-id=\""+tag.tag_id+"\">\n\
                                <p class=\"tag-settings-entry-p\">"+tag.tag_id+"</p>\n\
                                <p class=\"tag-settings-entry-p\">"+tag.tag_name+"</p>\n\
                                <p class=\"tag-settings-entry-p\">"+tag.tag_linked_icon_id+"</p>\n\
                                <div class=\"tag-settings-entry-img\" style=\"background-image: url("+base64_icon+");\"></div>\n\
                                "+tag_delete_button_html+"\n\
                            </div>";

        document.getElementById("server-settings-tab-tags-container").insertAdjacentHTML("beforeend", html_to_append);
    },
    /**
     * @brief a tag was deleted: removed from g_tags, its settings row, every displayed chip and every client's tag_ids
     *
     * @param object msg -> the server message, msg.message.tag_id names the tag
     *
     * @return void
     */
    process_tag_delete_from_server: function(msg)
    {
        let tag_id = msg.message.tag_id;

        for (let i = g_tags.length - 1; i >= 0; i--)
        {
            if (g_tags[i].tag_id == tag_id) { g_tags.splice(i, 1); }
        }

        let entry = document.querySelector('.server-settings-tag-entry[data-tag-id="' + tag_id + '"]');
        if (entry != null) { entry.remove(); }

        let displayed_tags = document.querySelectorAll('.single-tag[tag-id="' + tag_id + '"]');
        for (let i = 0; i < displayed_tags.length; i++) { displayed_tags[i].remove(); }

        for (let i = 0; i < g_client_list.length; i++)
        {
            let index_of_tag = g_client_list[i].tag_ids.indexOf(tag_id);
            if (index_of_tag != -1) { g_client_list[i].tag_ids.splice(index_of_tag, 1); }
        }
    },
    /**
     * @brief an icon was deleted: removed from g_icons and from the settings icon list
     *
     * @param object msg -> the server message, msg.message.icon_id names the icon
     *
     * @return void
     */
    process_icon_delete_from_server: function(msg)
    {
        let icon_id = msg.message.icon_id;

        for (let i = g_icons.length - 1; i >= 0; i--)
        {
            if (g_icons[i].id == icon_id) { g_icons.splice(i, 1); }
        }

        let entry = document.querySelector('.server-settings-icon-entry[data-icon-id="' + icon_id + '"]');
        if (entry != null) { entry.remove(); }
    },
    /**
     * @brief a channel's icon changed: the new icon fields are stored and its row repainted (and the icon box of the channel edit form if it is open for that channel)
     *
     * @param object msg -> the server message, msg.message holds channel_id, has_channel_icon and channel_icon_id
     *
     * @return void
     */
    process_channel_icon_changed_from_server: function(msg)
    {
        let channel = channel_tree__get_channel_by_id(g_channel_list, msg.message.channel_id);
        if (channel != null)
        {
            channel.has_channel_icon = msg.message.has_channel_icon;
            channel.channel_icon_id = msg.message.channel_icon_id;
            UI.refresh_channel_icon(channel);

            // if the edit form is open for this channel, update its icon box too
            if (g_channel_properties_edit_channel_id == msg.message.channel_id)
            {
                UI.refresh_channel_edit_icon_box(channel);
            }
        }
    },
    /**
     * @brief a tag's icon changed: the icon fields are updated in g_tags, its settings row and every displayed chip repainted
     *
     * @param object msg -> the server message, msg.message holds tag_id, has_icon and tag_linked_icon_id
     *
     * @return void
     */
    process_tag_icon_changed_from_server: function(msg)
    {
        let tag_id = msg.message.tag_id;
        let has_icon = msg.message.has_icon;
        let icon_id = msg.message.tag_linked_icon_id;

        for (let i = 0; i < g_tags.length; i++)
        {
            if (g_tags[i].tag_id == tag_id)
            {
                g_tags[i].has_icon = has_icon;
                g_tags[i].tag_linked_icon_id = icon_id;
                break;
            }
        }

        let icon = has_icon ? channel_tree__get_icon_by_icon_id(icon_id) : null;
        let background = (icon != null) ? ("url(" + icon.base64_icon + ")") : "";

        let entry = document.querySelector('.server-settings-tag-entry[data-tag-id="' + tag_id + '"]');
        if (entry != null)
        {
            let img_box = entry.querySelector('.tag-settings-entry-img');
            if (img_box != null) { img_box.style.backgroundImage = background; }
        }

        let displayed_tags = document.querySelectorAll('.single-tag[tag-id="' + tag_id + '"]');
        for (let i = 0; i < displayed_tags.length; i++)
        {
            displayed_tags[i].style.backgroundImage = background;
        }
    },
    /**
     * @brief a client started streaming a song: the marquee on their row shows the song name
     *
     * @param object msg -> the server message, msg.message holds client_id and song_name
     *
     * @return void
     */
    process_start_song_stream_from_server: function(msg)
    {
        let element = document.querySelector('.marquee-music-playing-container[data-marquee-music-playing-container-id="' + msg.message.client_id + '"]');
        if (element != null)
        {
            element.style.display = "inline-block";
            document.getElementById("marquee-song-name-client-id-" + msg.message.client_id).innerHTML = chat__sanitize_string(msg.message.song_name);
        }
        else
        {
            console.log("could not find element");
        }
    },

    /**
     * @brief a music bot's song list: opens the musicbot management dialog and rebuilds its song table; each row's X sends a remove-song request
     *
     * @param object msg -> the server message, msg.message.songs holds the songs
     *
     * @return void
     */
    process_music_bot_song_list_from_server: function(msg)
    {
        console.log(msg);

        document.getElementById("musicbot-management-background-container").style.display = "block";

        let html_to_append = "";

        for (single_song of msg.message.songs)
        {
            html_to_append += '\n\
            <tr style="border-bottom: 1px solid #eee;">\n\
                <td style="padding:10px;">'+ single_song.name +'</td>\n\
                <td style="padding:10px; text-align:right;">'+ single_song.duration_seconds +'</td>\n\
                <td style="padding:10px; text-align:right; cursor:pointer;"><input class="remove-song-from-musicbot-button" type="button" data-song-id="'+single_song.id+'" value="X"></td>\n\
            </tr>';
        }

        document.getElementById("musicbot-management-background-container-tbody").innerHTML = html_to_append;

        for (let x = 0; x < document.getElementsByClassName("remove-song-from-musicbot-button").length; x++)
        {
            document.getElementsByClassName("remove-song-from-musicbot-button")[x].onclick = function(event) {
                event.stopPropagation();

                let song_id = parseInt(event.currentTarget.getAttribute("data-song-id"));

                client_msg.send_remove_song_from_music_bot_request(song_id, parseInt(g_selected_client_id));
            };
        }
    },

    /**
     * @brief our upload was acknowledged: the lock is released and the server is told what the finished file is for, by sending the current file_send_intent back
     *        one upload = one completion; the intent is cleared so a stray ack cannot replay the last file
     *
     * @param object msg -> the server message
     *
     * @return void
     */
    process_file_send_success_from_server: function(msg)
    {
        let was_picture = (g_file_send_intent == "direct_chat_picture_file" || g_file_send_intent == "channel_chat_picture_file");

        // the ack means every part actually arrived, so this is where the upload is really
        // finished. released before the intent check so a stray ack cannot leave the lock stuck
        chat__release_file_upload_lock();

        if (g_file_send_intent == "musicbot_file" || g_file_send_intent == "direct_chat_picture_file" || g_file_send_intent == "channel_chat_picture_file"
            || g_file_send_intent == "direct_chat_file" || g_file_send_intent == "channel_chat_file")
        {
            client_msg.send_file_send_completed_request(g_file_send_intent);

            // one upload = one completion. clear the intent so a later stray file_send_success
            // cannot replay the last file (e.g. re-upload the song on an unrelated action)
            g_file_send_intent = "";
            g_file_send_intent_extra_data = {};
        }

        // the picture is on the server now; image_sent_status marks the end of the relay
        if (was_picture == true)
        {
            chat__show_picture_delivery_status();
        }
    },

    /**
     * @brief one base64 chunk of an incoming file, accumulated in received_files (keyed by server_chat_message_id) with a refreshed last-received timestamp
     *
     * @param object msg -> the server message, msg.message holds server_chat_message_id and the chunk
     *
     * @return void
     */
    process_file_receive_chunk_from_server_from_server: function(msg)
    {
        let is_found = false;
        for (i = 0; i < received_files.length; i++)
        {
            if(received_files[i].file_id == msg.message.server_chat_message_id)
            {
                is_found = true;
                break;
            }
        }

        if (is_found == false)
        {
            let single_file = {
                file_id: msg.message.server_chat_message_id,
                file_content_base64: "",
                timestamp_last_received: new Date().valueOf()
            };

            received_files.push(single_file);
        }

        for (i = 0; i < received_files.length; i++)
        {
            if(received_files[i].file_id == msg.message.server_chat_message_id)
            {
                received_files[i].timestamp_last_received = new Date().valueOf();
                received_files[i].file_content_base64 += msg.message.value;

                // a file card's ring; a no-op for pictures, which register no transfer
                chat_files__update_chat_file_progress(msg.message.server_chat_message_id, received_files[i].file_content_base64.length);
                break;
            }
        }
    },

    /**
     * @brief an incoming file is complete: the base64 goes to the data-processing worker for decryption (direct or channel chat picture) and leaves received_files
     *
     * @param object msg -> the server message, msg.message.server_chat_message_id names the file
     *
     * @return void
     */
    process_file_receive_completed_from_server_from_server: function(msg)
    {
        let message_raw = "";
        let index_to_delete = 0;
        let is_found = false;
        for (i = 0; i < received_files.length; i++)
        {
            if(received_files[i].file_id == msg.message.server_chat_message_id)
            {
                is_found = true;
                index_to_delete = i;
                message_raw = received_files[i].file_content_base64;
                break;
            }
        }

        if (is_found == false)
        {
            return;
        }

        // evict before the handoff: this is the only removal from received_files, and a stranded entry
        // would have the next picture with this message id appended on top of the leftovers
        received_files.splice(index_to_delete, 1);

        if (msg.message.receive_type == "direct_chat_picture")
        {
            // loopback: no private key here, node sends the decrypted picture instead
            if (android_host__is_ui_only_runtime())
            {
                return;
            }

            g_data_processing_worker.postMessage({
                type: "mainthread__process_encrypted_direct_chat_picture_data",
                message_raw: message_raw,
                picture_id: msg.message.server_chat_message_id,
                sender_id: msg.message.sender_id
            });

            console.log("total files in received_files " + received_files.length +"");
        }
        else if (msg.message.receive_type == "channel_chat_picture")
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__process_encrypted_channel_chat_picture_data",
                message_raw: message_raw,
                picture_id: msg.message.server_chat_message_id,
                sender_id: msg.message.sender_id
            });

            console.log("total files in received_files " + received_files.length +"");
        }
        else if (msg.message.receive_type == "direct_chat_file")
        {
            // loopback: no private key here, node sends the decrypted file instead
            if (android_host__is_ui_only_runtime())
            {
                return;
            }

            g_data_processing_worker.postMessage({
                type: "mainthread__process_encrypted_direct_chat_file_data",
                message_raw: message_raw,
                file_id: msg.message.server_chat_message_id,
                sender_id: msg.message.sender_id
            });
        }
        else if (msg.message.receive_type == "channel_chat_file")
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__process_encrypted_channel_chat_file_data",
                message_raw: message_raw,
                file_id: msg.message.server_chat_message_id,
                sender_id: msg.message.sender_id
            });
        }
    }
};

var client_msg = {
    /**
     * @brief asks the server to move the local client into idle mode
     *
     * @return void
     */
    send_go_to_idle_mode_request: function()
    {
        let message_object = {
            message:
            {
                type: "go_to_idle_mode_request",
            }
        };

        connection__send_message_object(message_object);
    },

    /**
     * @brief asks for the identities the server has stored (alias, avatar, tags), so people registered here can be shown while offline; the server ignores it unless it allows the list
     *
     * @return void
     */
    send_request_stored_clients: function()
    {
        let message_object = {
            message:
            {
                type: "request_stored_clients",
            }
        };

        connection__send_message_object(message_object);
    },

    /**
     * @brief asks the server to bring the local client back from idle into a channel
     *
     * @param number|null channelId -> the channel, null for root
     *
     * @return void
     */
    send_come_from_idle_mode_request: function(channelId = null)
    {
        let message_object = {
            message:
            {
                type: "come_back_from_idle_mode_request",
                channel_id: channelId,
            }
        };

        connection__send_message_object(message_object);
    },

    /**
     * @brief asks the server to rename a client
     *
     * @param string username_to_set -> the new name
     * @param number client_id -> the client
     *
     * @return void
     */
    send_change_client_username_request: function(username_to_set, client_id)
    {
        let message_object = {
            message: {
                type: "change_client_username",
                client_id: client_id,
                new_username: username_to_set
            }
        };

        connection__send_message_object(message_object);
    },

    /**
     * @brief requests the song list of the selected music bot (g_selected_client_id)
     *
     * @return void
     */
    send_musicbot_song_list_request: function()
    {
        let message_object = {
            message:
            {
                type: "musicbot_get_song_list",
                musicbot_id: parseInt(g_selected_client_id),
            }
        };

        connection__send_message_object(message_object);
    },

    /**
     * @brief uploads one base64 part of a file; index 0 flags the start of a new file
     *        the server merges the parts and, once done, asks what the file is for
     *
     * @param number total_bytes_length -> the whole file's length
     * @param string data_part_base64 -> this part
     * @param number index -> the part's index
     *
     * @return void
     */
    send_file_send_request: function(total_bytes_length, data_part_base64, index)
    {
        // uploads file by smaller base64 parts that server will merge together to produce file... for now this will be used only for mp3 files
        // after the file upload is done, server sends the information to the client that it is done, and by that it asks the client what to do with the file (is it mp3 file, image for channel, for client, or avatar?)

        is_new_file = false;
        if (index === 0)
        {
            is_new_file = true;
        }

        let message_object = {
            message: {
                type: "file_send",
                total_bytes_length: total_bytes_length,
                data_part_base64: data_part_base64,
                is_new_file: is_new_file
            }
        };

        connection__send_message_object(message_object);
    },
    /**
     * @brief tells the server what the fully uploaded file is for: the intent plus the global g_file_send_intent_extra_data
     *
     * @param string g_file_send_intent -> the intent, "musicbot_file", "channel_chat_file" and the like
     *
     * @return void
     */
    send_file_send_completed_request: function(g_file_send_intent)
    {
        // after file upload is done, server informs the client that it is done. Clients then tells the server what to do with the file (file_send_intent)

        let message_object = {
            message: {
                type: "file_send_completed",
                file_send_intent: g_file_send_intent,
                file_send_intent_extra_data: g_file_send_intent_extra_data
            }
        };

        connection__send_message_object(message_object);
    },

    /**
     * @brief asks a music bot to delete one of its songs
     *
     * @param number song_id -> the song
     * @param number musicbot_client_id -> the bot
     *
     * @return void
     */
    send_remove_song_from_music_bot_request: function(song_id, musicbot_client_id) {
        let message_object = {
                message: {
                    type: "remove_song_from_music_bot",
                    musicbot_id: musicbot_client_id,
                    song_id: song_id
                }
            };

        connection__send_message_object(message_object);
    }
};

// the chat file handlers in messages.js call into this (card markup, header/body crypto), and
// node decrypts direct chat files itself, so it is part of the headless bundle too
// chat-files.js is a file that is embedded in template.html along with other files
// it facilitates functionality for uploading and receiving all kinds of files from the local filesystem
// its functions are called from three other files in the lemon chat client, chat.js (sending and the upload lock), messages.js (receiving and rendering) and ui.js (attach functionality)
// the encryption mechanism for files in a channel mirrors the channel encryption mechanism of lemon chat; 
// files sent directly to a user mirror the direct chat message encryption mechanism

// above this limit the server settings tab shows a warning, because one transfer holds
// roughly twice the file size in ram
var FILE_UPLOAD_WARNING_MB = 20;

// used only in the worker. it remembers the key set that decrypted a file's header, so the
// body (up to 20 MB) is not tried against every historic key set
var chat_file_keys_by_file_id = {};

// the circumference of the progress ring (radius 15.5). stroke-dashoffset counts down from this value
var CHAT_FILE_RING_LENGTH = 97.4;

// maps file extensions to an icon kind. the kind picks the badge colour of the file icon in layout.style
var CHAT_FILE_KINDS = {
    archive: ["zip", "rar", "7z", "tar", "gz", "bz2", "xz", "tgz", "iso"],
    pdf: ["pdf"],
    document: ["doc", "docx", "odt", "rtf", "txt", "md", "epub"],
    sheet: ["xls", "xlsx", "ods", "csv"],
    slides: ["ppt", "pptx", "odp"],
    code: ["js", "ts", "py", "c", "h", "cpp", "hpp", "java", "cs", "go", "rs", "php", "rb", "sh", "bat", "html", "css", "json", "xml", "yml", "yaml", "sql"],
    audio: ["mp3", "wav", "ogg", "flac", "m4a", "aac", "opus", "wma"],
    video: ["mp4", "mkv", "avi", "mov", "webm", "wmv", "m4v"],
    image: ["png", "jpg", "jpeg", "gif", "ico", "bmp", "webp", "svg", "tiff", "heic"],
    executable: ["exe", "msi", "apk", "dmg", "deb", "rpm", "appimage"]
};

// the extensions the picture picker accepts. anything else dropped on the chat is sent as a file
var CHAT_PICTURE_EXTENSIONS = ["png", "jpg", "jpeg", "gif", "ico", "webp", "bmp", "svg", "avif"];

// ---------------------------------------------------------------------------
// base64 and crypto helpers. these run in the page, in the workers and in the node client
// ---------------------------------------------------------------------------

/**
 * @brief bytes -> base64 with the native btoa, about 100 times faster than utils__bytesToBase64String, which matters at 20 MB
 *
 * @param Uint8Array bytes -> the bytes
 *
 * @return string the base64 text
 */
function chat_files__fast_bytes_to_base64(bytes)
{
    if (typeof btoa !== "function")
    {
        return utils__bytesToBase64String(bytes);
    }

    let chunks = [];
    const step = 0x8000;

    for (let i = 0; i < bytes.length; i += step)
    {
        chunks.push(String.fromCharCode.apply(null, bytes.subarray(i, i + step)));
    }

    return btoa(chunks.join(""));
}

/**
 * @brief base64 -> bytes with the native atob; throws on malformed base64, because atob does, and the callers catch that
 *
 * @param string base64_string -> the base64 text
 *
 * @return Uint8Array the bytes
 */
function chat_files__fast_base64_to_bytes(base64_string)
{
    if (typeof atob !== "function")
    {
        return new Uint8Array(utils__base64StringToBytes(base64_string));
    }

    let binary = atob(base64_string);
    let bytes = new Uint8Array(binary.length);

    for (let i = 0; i < binary.length; i++)
    {
        bytes[i] = binary.charCodeAt(i);
    }

    return bytes;
}

/**
 * @brief encrypts a string the way pictures are encrypted: utf8 bytes, zero padding, aes-ctr with every key, then base64
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes
 * @param string string_to_encrypt -> the plaintext
 * @param number minimum_padded_bytes -> the padding floor; defaults to the picture path's 1024, file bodies pass CHAT_FILE_BODY_MIN_PADDED_BYTES to clear the server's minimum upload size
 *
 * @return string the base64 ciphertext
 */
function chat_files__encrypt_string_with_aes_keys_fast(keys, string_to_encrypt, minimum_padded_bytes)
{
    let padding_floor = (typeof minimum_padded_bytes === "number") ? minimum_padded_bytes : 1024;
    let bytes = aesjs.utils.utf8.toBytes(string_to_encrypt);

    if (bytes.length < padding_floor)
    {
        let padded = new Uint8Array(padding_floor);
        padded.set(bytes);
        bytes = padded;
    }

    let data = bytes;

    for (let i = 0; i < keys.length; i++)
    {
        let aes_ctr = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        data = aes_ctr.encrypt(data);
    }

    return chat_files__fast_bytes_to_base64(data);
}

/**
 * @brief decrypts what chat_files__encrypt_string_with_aes_keys_fast produced
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes
 * @param string base64_string -> the base64 ciphertext
 *
 * @return string|null the plaintext cut at the first null, null when the base64 is malformed
 */
function chat_files__decrypt_base64_with_aes_keys_fast(keys, base64_string)
{
    let data = null;

    try
    {
        data = chat_files__fast_base64_to_bytes(base64_string);
    }
    catch (decode_error)
    {
        return null;
    }

    for (let i = keys.length - 1; i >= 0; i--)
    {
        let aes_ctr = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        data = aes_ctr.decrypt(data);
    }

    return utils__substringByNullTerminator(new TextDecoder().decode(data));
}

/**
 * @brief four fresh random aes-ctr keys, the key set a direct message envelope uses
 *
 * @return array the keys as [{ key_bytes, iv_bytes }]
 */
function chat_files__create_random_message_keys()
{
    let keys = [];

    for (let i = 0; i < 4; i++)
    {
        keys.push({
            key_bytes: Array.from(crypto.getRandomValues(new Uint8Array(32))),
            iv_bytes: Array.from(crypto.getRandomValues(new Uint8Array(16)))
        });
    }

    return keys;
}

/**
 * @brief builds the direct chat envelope: message_keys carries the aes keys encrypted with the receiver's rsa key, message_value the aes-encrypted plaintext
 *
 * @param string receiver_public_key -> the receiver's public key string
 * @param string plaintext -> the text to wrap
 * @param number minimum_padded_bytes -> the padding floor, see chat_files__encrypt_string_with_aes_keys_fast
 *
 * @return string|null the envelope as json, null when the receiver's public key is unusable
 */
function chat_files__create_direct_chat_file_envelope(receiver_public_key, plaintext, minimum_padded_bytes)
{
    let keys = chat_files__create_random_message_keys();
    let encryption_result = lemon_crypto.encrypt(JSON.stringify(keys), receiver_public_key);

    if (encryption_result == null || encryption_result.status != "success")
    {
        return null;
    }

    return JSON.stringify({
        message_keys: encryption_result.cipher,
        message_value: chat_files__encrypt_string_with_aes_keys_fast(keys, plaintext, minimum_padded_bytes)
    });
}

/**
 * @brief opens a direct envelope with our own rsa key
 *
 * @param string envelope_json_string -> the envelope as json
 *
 * @return string|null the plaintext, null when anything about the envelope is wrong
 */
function chat_files__open_direct_chat_file_envelope(envelope_json_string)
{
    let envelope = null;

    try
    {
        envelope = JSON.parse(envelope_json_string);
    }
    catch (parse_error)
    {
        return null;
    }

    if (envelope == null || typeof envelope.message_keys !== "string" || typeof envelope.message_value !== "string")
    {
        return null;
    }

    let decryption_result = lemon_crypto.decrypt(envelope.message_keys, g_my_rsa_key_object);

    if (decryption_result.status == "failure")
    {
        return null;
    }

    let keys = null;

    try
    {
        keys = JSON.parse(decryption_result.plaintext);
    }
    catch (keys_error)
    {
        return null;
    }

    return chat_files__decrypt_base64_with_aes_keys_fast(keys, envelope.message_value);
}

/**
 * @brief the channel key sets a file could be encrypted with: the hinted set first, then the current channel keys, then the historic ones
 *
 * @param array|null hinted_keys -> the set that worked before, or null
 *
 * @return array the key sets to try, in that order
 */
function chat_files__get_channel_key_candidates(hinted_keys)
{
    let candidates = [];

    if (hinted_keys != null)
    {
        candidates.push(hinted_keys);
    }

    if (g_current_channel_keys != null && g_current_channel_keys !== hinted_keys)
    {
        candidates.push(g_current_channel_keys);
    }

    for (let i = 0; i < g_historic_keys_of_current_channel.length; i++)
    {
        if (g_historic_keys_of_current_channel[i] !== hinted_keys)
        {
            candidates.push(g_historic_keys_of_current_channel[i]);
        }
    }

    return candidates;
}

/**
 * @brief decrypts the header of a channel file, trying every candidate key set, and remembers the one that worked so the body can use it directly
 *        runs in the worker
 *
 * @param number file_id -> the transfer
 * @param string header_base64 -> the encrypted header
 *
 * @return object|null { name, size, mime }, null when no key set fits
 */
function chat_files__decrypt_channel_chat_file_header(file_id, header_base64)
{
    let candidates = chat_files__get_channel_key_candidates(null);

    for (let i = 0; i < candidates.length; i++)
    {
        let header = chat_files__parse_chat_file_header(chat_files__decrypt_base64_with_aes_keys_fast(candidates[i], header_base64));

        if (header != null)
        {
            chat_file_keys_by_file_id[file_id] = candidates[i];
            return header;
        }
    }

    return null;
}

/**
 * @brief decrypts the body of a channel file with the key set the header found, else every candidate
 *        runs in the worker
 *
 * @param number file_id -> the transfer
 * @param string body_base64 -> the encrypted body
 *
 * @return object|null { name, size, mime, base64 }, null when no key set fits
 */
function chat_files__decrypt_channel_chat_file_body(file_id, body_base64)
{
    let candidates = chat_files__get_channel_key_candidates(chat_file_keys_by_file_id[file_id]);

    delete chat_file_keys_by_file_id[file_id];

    for (let i = 0; i < candidates.length; i++)
    {
        let file = chat_files__parse_decrypted_chat_file_body(chat_files__decrypt_base64_with_aes_keys_fast(candidates[i], body_base64));

        if (file != null)
        {
            return file;
        }
    }

    return null;
}

// ---------------------------------------------------------------------------
// the shapes that travel over the wire
// ---------------------------------------------------------------------------

/**
 * @brief the header json that travels encrypted in the metadata; it carries enough to draw the file card before the body arrives
 *
 * @param object file -> { name, size, mime }
 *
 * @return string the json
 */
function chat_files__build_chat_file_header_json(file)
{
    return JSON.stringify({ name: file.name, size: file.size, mime: file.mime });
}

/**
 * @brief the body json that travels encrypted in the 400-part upload
 *
 * @param object file -> { name, size, mime, base64 }
 *
 * @return string the json
 */
function chat_files__build_chat_file_body_json(file)
{
    return JSON.stringify({ name: file.name, size: file.size, mime: file.mime, data: file.base64 });
}

/**
 * @brief parses a decrypted header or body, sanitising the name and the mime type
 *        decrypting with a wrong key produces noise instead of json, so anything that is not one is null
 *
 * @param string text -> the decrypted text
 *
 * @return object|null the parsed object, null when the text is not a file json
 */
function chat_files__parse_chat_file_json(text)
{
    if (typeof text !== "string" || text.charAt(0) !== "{")
    {
        return null;
    }

    let parsed = null;

    try
    {
        parsed = JSON.parse(text);
    }
    catch (parse_error)
    {
        return null;
    }

    if (parsed == null || typeof parsed.name !== "string")
    {
        return null;
    }

    return {
        name: chat_files__sanitize_file_name(parsed.name),
        size: (typeof parsed.size === "number" && parsed.size > 0) ? Math.floor(parsed.size) : 0,
        mime: chat_files__sanitize_mime_type(parsed.mime),
        data: parsed.data
    };
}

/**
 * @brief a decrypted header as { name, size, mime }
 *
 * @param string text -> the decrypted text
 *
 * @return object|null the header, null when the text is not one
 */
function chat_files__parse_chat_file_header(text)
{
    let parsed = chat_files__parse_chat_file_json(text);

    if (parsed == null)
    {
        return null;
    }

    return { name: parsed.name, size: parsed.size, mime: parsed.mime };
}

/**
 * @brief a decrypted body as { name, size, mime, base64 }, the data checked to be base64
 *
 * @param string text -> the decrypted text
 *
 * @return object|null the body, null when the text is not one
 */
function chat_files__parse_decrypted_chat_file_body(text)
{
    let parsed = chat_files__parse_chat_file_json(text);

    if (parsed == null || typeof parsed.data !== "string" || /^[A-Za-z0-9+/]*={0,2}$/.test(parsed.data) == false)
    {
        return null;
    }

    return { name: parsed.name, size: parsed.size, mime: parsed.mime, base64: parsed.data };
}

// ---------------------------------------------------------------------------
// names, sizes, icons
// ---------------------------------------------------------------------------

/**
 * @brief cleans a file name: no path separators or control characters, at most 255 characters, never empty
 *
 * @param string name -> the name as received
 *
 * @return string the cleaned name, "file" when nothing usable is left
 */
function chat_files__sanitize_file_name(name)
{
    let result = ("" + (name == null ? "" : name)).replace(/[\\\/\x00-\x1f\x7f]/g, "_").trim();

    if (result.length > 255)
    {
        result = result.slice(0, 255);
    }

    if (result.length == 0 || result == "." || result == "..")
    {
        result = "file";
    }

    return result;
}

/**
 * @brief cleans a mime type: lowercase, at most 100 characters, of the type/subtype shape
 *
 * @param string mime -> the type as received
 *
 * @return string the cleaned type, "application/octet-stream" when it is not a valid one
 */
function chat_files__sanitize_mime_type(mime)
{
    let text = "" + (mime == null ? "" : mime);

    if (/^[a-z0-9!#$&^_.+-]+\/[a-z0-9!#$&^_.+-]+$/i.test(text) == false || text.length > 100)
    {
        return "application/octet-stream";
    }

    return text.toLowerCase();
}

/**
 * @brief the icon kind of a file extension, from CHAT_FILE_KINDS
 *
 * @param string extension -> the extension without the dot
 *
 * @return string the kind, "generic" when unknown
 */
function chat_files__get_chat_file_kind(extension)
{
    for (let kind in CHAT_FILE_KINDS)
    {
        if (CHAT_FILE_KINDS[kind].indexOf(extension) != -1)
        {
            return kind;
        }
    }

    return "generic";
}

/**
 * @brief whether a file takes the picture path
 *        the browser's own type is trusted first, so a dropped image behaves like one picked from
 *        disk; the extension is the fallback for a drop that carried no type
 *
 * @param File file -> the file
 *
 * @return boolean true for a picture
 */
function chat_files__is_chat_picture_file(file)
{
    if (file == null)
    {
        return false;
    }

    // the browser's own type is trusted first, so a dropped image behaves like one picked from
    // disk. the extension is the fallback for a drop that carried no type
    if (typeof file.type === "string" && file.type.indexOf("image/") === 0)
    {
        return true;
    }

    return CHAT_PICTURE_EXTENSIONS.indexOf(chat__get_file_extension(file.name)) != -1;
}

/**
 * @brief a byte count as "12 KB" / "1.5 MB" text
 *
 * @param number bytes -> the size
 *
 * @return string the text, "" for anything that is not a non-negative number
 */
function chat_files__format_file_size(bytes)
{
    if (typeof bytes !== "number" || bytes < 0)
    {
        return "";
    }

    if (bytes < 1024)
    {
        return bytes + " B";
    }

    if (bytes < 1024 * 1024)
    {
        return (bytes / 1024).toFixed(bytes < 10 * 1024 ? 1 : 0) + " KB";
    }

    if (bytes < 1024 * 1024 * 1024)
    {
        return (bytes / (1024 * 1024)).toFixed(bytes < 10 * 1024 * 1024 ? 2 : 1) + " MB";
    }

    return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB";
}

/**
 * @brief the file icon: a page with a folded corner and, for a known extension, a coloured badge carrying it
 *
 * @param string file_name -> the name the extension is taken from
 * @param string css_class -> the class of the svg
 *
 * @return string the svg markup
 */
function chat_files__generate_chat_file_icon_svg(file_name, css_class)
{
    let extension = chat__get_file_extension(file_name);
    let label = (extension.length > 0 && extension.length <= 4) ? extension.toUpperCase() : "";
    let badge = "";

    if (label.length > 0)
    {
        badge = "<rect class=\"chat-file-icon-badge\" x=\"1\" y=\"27\" width=\"38\" height=\"16\" rx=\"3\"></rect>"
              + "<text class=\"chat-file-icon-text\" x=\"20\" y=\"38.5\" text-anchor=\"middle\">" + label + "</text>";
    }

    return "<svg class=\"" + css_class + " chat-file-kind-" + chat_files__get_chat_file_kind(extension) + "\" viewBox=\"0 0 40 48\" aria-hidden=\"true\">"
         + "<path class=\"chat-file-icon-page\" d=\"M7 1h18l11 11v32a3 3 0 0 1-3 3H7a3 3 0 0 1-3-3V4a3 3 0 0 1 3-3z\"></path>"
         + "<path class=\"chat-file-icon-fold\" d=\"M25 1v11h11\"></path>"
         + badge
         + "</svg>";
}

// ---------------------------------------------------------------------------
// the file card in the chat
// ---------------------------------------------------------------------------

/**
 * @brief the file card html
 *
 * @param object options -> { key, name, size, is_local, local_message_id, is_receiving }; key is the server message id for received files, or "local-N" for our own echo
 *
 * @return string the card markup
 */
function chat_files__generate_chat_file_card_html(options)
{
    let name_html = chat__sanitize_string(options.name);
    let size_text = (options.size > 0) ? chat_files__format_file_size(options.size) : "";
    let classes = "chat-file-card";
    let attributes = " data-chat-file-key=\"" + options.key + "\"";

    if (options.is_local == true)
    {
        classes += " local-client-chat-file imgnotsentyet";
        attributes += " data-single-chat-message-local-message-id=\"" + options.local_message_id + "\" data-single-chat-message-server-message-id=\"unspecified\"";
    }
    else
    {
        attributes += " id=\"chat-file-card-" + options.key + "\" data-single-chat-message-server-message-id=\"" + options.key + "\"";
    }

    if (options.is_receiving == true)
    {
        classes += " chat-file-receiving";
    }

    return "<div class=\"" + classes + "\"" + attributes + ">"
         + "<div class=\"chat-file-icon-wrap\">" + chat_files__generate_chat_file_icon_svg(options.name, "chat-file-icon") + "</div>"
         + "<div class=\"chat-file-info\">"
         + "<p class=\"chat-file-name\" title=\"" + name_html + "\">" + name_html + "</p>"
         + "<p class=\"chat-file-size\">" + size_text + "</p>"
         + "<p class=\"chat-file-status\"></p>"
         + "</div>"
         + "<div class=\"chat-file-progress\" title=\"receiving\">"
         + "<svg viewBox=\"0 0 36 36\">"
         + "<circle class=\"chat-file-ring-track\" cx=\"18\" cy=\"18\" r=\"15.5\"></circle>"
         + "<circle class=\"chat-file-ring-bar\" cx=\"18\" cy=\"18\" r=\"15.5\"></circle></svg>"
         + "<span class=\"chat-file-progress-text\">0%</span>"
         + "</div>"
         + "<button type=\"button\" class=\"chat-file-download\" title=\"download\"></button>"
         + "</div>";
}

/**
 * @brief finds a file card or an incoming picture's progress ring by its key; both carry the same data attribute and the same ring innards, so one lookup serves both
 *
 * @param string|number key -> the transfer key
 *
 * @return Element|null the element, null when there is none
 */
function chat_files__get_chat_file_card_by_key(key)
{
    return document.querySelector('[data-chat-file-key="' + key + '"]');
}

/**
 * @brief registers an incoming transfer with its total size, so its progress ring can be moved
 *
 * @param number file_id -> the transfer
 * @param number encrypted_size -> the encrypted size in characters, 0 when unknown
 *
 * @return void
 */
function chat_files__begin_chat_file_transfer(file_id, encrypted_size)
{
    g_chat_file_transfers[file_id] = {
        encrypted_size: (typeof encrypted_size === "number" && encrypted_size > 0) ? encrypted_size : 0
    };
}

/**
 * @brief the progress ring a file card shows, for an incoming picture, because a picture has no card of its own
 *
 * @param number picture_id -> the transfer
 *
 * @return string the ring markup
 */
function chat_files__generate_chat_picture_progress_html(picture_id)
{
    return "<div class=\"chat-picture-progress\" data-chat-file-key=\"" + picture_id + "\">"
         + "<div class=\"chat-picture-progress-ring\">"
         + "<svg viewBox=\"0 0 36 36\">"
         + "<circle class=\"chat-file-ring-track\" cx=\"18\" cy=\"18\" r=\"15.5\"></circle>"
         + "<circle class=\"chat-file-ring-bar\" cx=\"18\" cy=\"18\" r=\"15.5\"></circle></svg>"
         + "<span class=\"chat-file-progress-text\">0%</span>"
         + "</div>"
         + "<span class=\"chat-picture-progress-label\">receiving image ...</span>"
         + "</div>";
}

/**
 * @brief removes the progress ring and forgets the transfer, once the picture arrived or its transfer died
 *
 * @param number picture_id -> the transfer
 *
 * @return void
 */
function chat_files__finish_chat_picture_progress(picture_id)
{
    delete g_chat_file_transfers[picture_id];

    let ring = document.querySelector('.chat-picture-progress[data-chat-file-key="' + picture_id + '"]');

    if (ring != null)
    {
        ring.remove();
    }
}

/**
 * @brief moves the progress ring from how many characters of the transfer arrived so far
 *
 * @param number file_id -> the transfer
 * @param number received_chars -> characters received so far
 *
 * @return void
 */
function chat_files__update_chat_file_progress(file_id, received_chars)
{
    let transfer = g_chat_file_transfers[file_id];

    if (transfer == null)
    {
        return;
    }

    let ratio = (transfer.encrypted_size > 0) ? Math.min(1, received_chars / transfer.encrypted_size) : 0;
    chat_files__render_chat_file_progress(file_id, ratio);
}

/**
 * @brief paints a ratio onto a transfer's ring and its percentage text
 *
 * @param number file_id -> the transfer
 * @param number ratio -> 0 to 1
 *
 * @return void
 */
function chat_files__render_chat_file_progress(file_id, ratio)
{
    let card = chat_files__get_chat_file_card_by_key(file_id);

    if (card == null)
    {
        return;
    }

    let bar = card.querySelector(".chat-file-ring-bar");
    let text = card.querySelector(".chat-file-progress-text");

    if (bar != null)
    {
        bar.style.strokeDashoffset = (CHAT_FILE_RING_LENGTH * (1 - ratio)).toFixed(2);
    }

    if (text != null)
    {
        text.textContent = Math.round(ratio * 100) + "%";
    }
}

/**
 * @brief enables a card's download button and points it at the stored file
 *
 * @param Element card -> the file card
 * @param string|number key -> the transfer key
 *
 * @return void
 */
function chat_files__wire_chat_file_download_button(card, key)
{
    let button = card.querySelector(".chat-file-download");

    if (button == null)
    {
        return;
    }

    button.disabled = false;
    button.onclick = function(event)
    {
        event.stopPropagation();
        chat_files__download_chat_file(key);
    };
}

/**
 * @brief finishes a file card once the body arrived and decrypted: the file is kept for the download button and the progress ring goes away
 *        when there is no live card, no copy is kept; this catches node's dead elements (the phone
 *        would otherwise hold every received file in ram forever), a cleared chat and a deleted message
 *
 * @param string|number key -> the transfer key
 * @param object file -> { name, size, mime, base64 }
 *
 * @return void
 */
function chat_files__finish_chat_file_card(key, file)
{
    delete g_chat_file_transfers[key];

    let card = chat_files__get_chat_file_card_by_key(key);

    // when there is no live card, no copy is kept. this catches node's dead elements (the phone
    // would otherwise hold every received file in ram forever), a cleared chat and a deleted message
    if (card == null || card.isConnected !== true || card.classList.contains("chat-file-deleted"))
    {
        return;
    }

    g_chat_files_by_message_id[key] = file;
    card.classList.remove("chat-file-receiving");

    let name = card.querySelector(".chat-file-name");
    let size = card.querySelector(".chat-file-size");

    if (name != null)
    {
        name.textContent = file.name;
        name.title = file.name;
    }

    if (size != null)
    {
        size.textContent = chat_files__format_file_size(file.size > 0 ? file.size : Math.floor(file.base64.length * 3 / 4));
    }

    chat_files__wire_chat_file_download_button(card, key);
}

/**
 * @brief turns a card into its failed state with a status text
 *
 * @param Element|null card -> the file card, nothing happens for null
 * @param string text -> the status text
 *
 * @return void
 */
function chat_files__mark_chat_file_card_failed(card, text)
{
    if (card == null)
    {
        return;
    }

    card.classList.remove("imgnotsentyet");
    card.classList.remove("chat-file-receiving");
    card.classList.add("chat-file-failed");

    let status = card.querySelector(".chat-file-status");

    if (status != null)
    {
        status.textContent = text;
    }
}

/**
 * @brief marks a refused upload as failed
 *        the server names the local message id it refused; 0 means it refused the very first part,
 *        and then the newest unsent card is the one that failed
 *
 * @param number g_local_message_id -> the refused local message id, 0 for the newest unsent card
 * @param string text -> the status text
 *
 * @return void
 */
function chat_files__mark_local_chat_file_card_failed(g_local_message_id, text)
{
    let card = null;

    if (typeof g_local_message_id === "number" && g_local_message_id > 0)
    {
        card = document.querySelector('.local-client-chat-file[data-single-chat-message-local-message-id="' + g_local_message_id + '"]');
    }
    else
    {
        let unsent = document.querySelectorAll(".local-client-chat-file.imgnotsentyet");
        card = (unsent.length > 0) ? unsent[unsent.length - 1] : null;
    }

    chat_files__mark_chat_file_card_failed(card, text);
}

/**
 * @brief turns a card into its deleted state: forgets the file and the transfer, unwires the menu, leaves a "deleted" card
 *
 * @param Element|null card -> the file card, nothing happens for null
 *
 * @return void
 */
function chat_files__mark_chat_file_card_deleted(card)
{
    if (card == null)
    {
        return;
    }

    delete g_chat_files_by_message_id[card.getAttribute("data-chat-file-key")];
    delete g_chat_file_transfers[card.getAttribute("data-chat-file-key")];
    card.removeEventListener("mousedown", chat_files__chat_file_card_onrightclick);
    card.className = "chat-file-card chat-file-deleted";
    card.innerHTML = "<p class=\"chat-file-status\">deleted</p>";
}

/**
 * @brief forgets the stored bytes of cards that no longer exist, after "clear chat" removed them
 *
 * @return void
 */
function chat_files__prune_chat_files_without_cards()
{
    for (let key in g_chat_files_by_message_id)
    {
        if (chat_files__get_chat_file_card_by_key(key) == null)
        {
            delete g_chat_files_by_message_id[key];
        }
    }
}

/**
 * @brief the text for a refused file: uploads off, too big, receiver unavailable, private messages off, or the server's own reason
 *
 * @param string reason -> the reason code from the server
 * @param number max_bytes -> the server's limit, for the "too big" text
 *
 * @return string the text to show
 */
function chat_files__explain_file_send_error(reason, max_bytes)
{
    if (reason == "file_uploads_disabled")
    {
        return "file not sent: file uploads are not allowed on this server";
    }

    if (reason == "file_too_large")
    {
        return "file not sent: too big, this server allows up to " + chat_files__format_file_size(max_bytes);
    }

    if (reason == "receiver_unavailable")
    {
        return "file not sent: the receiver is not available (offline or idle)";
    }

    if (reason == "private_messages_disabled")
    {
        return "file not sent: private messages are disabled on this server";
    }

    return "file not sent: refused by the server (" + chat__sanitize_string("" + reason) + ")";
}

/**
 * @brief right click on a file card opens the delete menu; there is no edit item, because a file has nothing to edit
 *
 * @param object event -> the mouse event
 *
 * @return void
 */
function chat_files__chat_file_card_onrightclick(event)
{
    if (event.which != 3)
    {
        return;
    }

    let card = event.currentTarget;
    let server_message_id = card.getAttribute("data-single-chat-message-server-message-id");

    if (server_message_id == null || server_message_id == "unspecified")
    {
        return;
    }

    event.stopPropagation();

    let old_menus = document.getElementsByClassName("chat-message-context-menu");

    while (old_menus.length > 0)
    {
        old_menus[0].remove();
    }

    g_selected_server_chat_message_id = parseInt(server_message_id);

    let contextmenu_html = "<div class=\"chat-message-context-menu\" style=\"top: " + event.clientY + "px; left: " + event.clientX + "px;\">"
                         + "<div class='chat-message-context-menu-background'></div>"
                         + "<div class='chat-message-context-menu-items'>"
                         + "<p class='chat-message-context-menu-item' data-action='0'>delete</p>"
                         + "</div></div>";

    document.getElementById("contextmenus-container").insertAdjacentHTML("beforeend", contextmenu_html);

    let items = document.getElementsByClassName("chat-message-context-menu-item");

    for (let i = 0; i < items.length; i++)
    {
        items[i].addEventListener("mousedown", UI.chat_message_contextmenuitem_onclick);
    }
}

// ---------------------------------------------------------------------------
// download
// ---------------------------------------------------------------------------

/**
 * @brief saves a received file: the java side writes it into Downloads in the webview, a browser gets a blob download
 *
 * @param string|number key -> the transfer key
 *
 * @return void
 */
function chat_files__download_chat_file(key)
{
    let file = g_chat_files_by_message_id[key];

    if (file == null)
    {
        utils__custom_alert("this file is no longer available");
        return;
    }

    // the android webview can not save a blob url, so the java side writes the file into Downloads instead
    if (typeof Android !== "undefined" && Android != null && typeof Android.JavaExportSaveFile === "function")
    {
        Android.JavaExportSaveFile(file.name, file.mime, file.base64);
        return;
    }

    let bytes = null;

    try
    {
        bytes = chat_files__fast_base64_to_bytes(file.base64);
    }
    catch (decode_error)
    {
        utils__custom_alert("the file data is damaged, it can not be saved");
        return;
    }

    let url = URL.createObjectURL(new Blob([bytes], { type: file.mime }));
    let anchor = document.createElement("a");

    anchor.href = url;
    anchor.download = file.name;
    anchor.style.display = "none";
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();

    setTimeout(function() { URL.revokeObjectURL(url); }, 60000);
}

// ---------------------------------------------------------------------------
// attaching: paperclip, drag & drop, the pending chip
// ---------------------------------------------------------------------------

/**
 * @brief the drag and drop entry point; every refusal explains why
 *        a dropped image takes the picture path; an image over the server's inline picture limit
 *        goes out as a plain file instead
 *
 * @param File file -> the dropped file
 *
 * @return void
 */
function chat_files__attach_chat_file_or_picture(file)
{
    if (file == null)
    {
        return;
    }

    if (document.getElementById("chat-input-container-text-input").disabled == true)
    {
        utils__custom_alert("join a channel or open a private chat first");
        return;
    }

    if (chat_files__is_chat_picture_file(file))
    {
        if (g_server_policy.allow_chat_pictures == false)
        {
            if (g_server_policy.allow_file_uploads == true && file.size <= g_server_policy.file_upload_max_size)
            {
                utils__custom_alert("inline pictures are disabled on this server, so the image goes as a file");
                chat_files__attach_chat_file(file);
                return;
            }

            utils__custom_alert("inline pictures are disabled on this server");
            return;
        }

        if (file.size <= g_server_policy.chat_picture_max_size)
        {
            chat_files__attach_chat_picture(file);
            return;
        }

        if (g_server_policy.allow_file_uploads == true && file.size <= g_server_policy.file_upload_max_size)
        {
            utils__custom_alert("image is over the server's " + chat_files__format_file_size(g_server_policy.chat_picture_max_size) + " picture limit, so it goes as a file - the receiver downloads it instead of seeing it inline");
            chat_files__attach_chat_file(file);
            return;
        }

        utils__custom_alert("image is too large to send as a picture (this server allows up to " + chat_files__format_file_size(g_server_policy.chat_picture_max_size) + ")");
        return;
    }

    chat_files__attach_chat_file(file);
}

/**
 * @brief attaches a picture for the next send, the same preview path the picture button uses; drag and drop lands here too
 *
 * @param File file -> the picture
 *
 * @return void
 */
function chat_files__attach_chat_picture(file)
{
    if (g_server_policy.allow_chat_pictures == false)
    {
        utils__custom_alert("inline pictures are disabled on this server");
        return;
    }

    if (file.size > g_server_policy.chat_picture_max_size)
    {
        if (g_server_policy.allow_file_uploads == true && file.size <= g_server_policy.file_upload_max_size)
        {
            utils__custom_alert("image is too large to send as a picture (this server allows up to " + chat_files__format_file_size(g_server_policy.chat_picture_max_size) + "). use the paperclip to send it as a file");
        }
        else
        {
            utils__custom_alert("image is too large to send as a picture (this server allows up to " + chat_files__format_file_size(g_server_policy.chat_picture_max_size) + ")");
        }

        return;
    }

    let reader = new FileReader();

    reader.onload = function(event)
    {
        // one attachment per message, so a pending file gives way to the picture
        chat_files__clear_pending_chat_file();

        // the thumbnail overlays the text box, so the typed text is cleared out of its way
        document.getElementById("chat-input-container-text-input").value = "";
        document.getElementById("image-upload-preview").style.aspectRatio = "";
        document.getElementById("image-upload-preview").style.display = "inline-block";
        document.getElementById("image-upload-preview").style.backgroundImage = "url(" + event.target.result + ")";
        g_base64_picture_string_to_send = event.target.result;

        // the thumbnail takes the picture's own shape, because the css box alone would crop it
        let preview_image = new Image();
        preview_image.onload = function()
        {
            if (preview_image.naturalWidth > 0 && preview_image.naturalHeight > 0 && g_base64_picture_string_to_send === preview_image.src)
            {
                document.getElementById("image-upload-preview").style.aspectRatio = preview_image.naturalWidth + " / " + preview_image.naturalHeight;
            }
        };
        preview_image.src = event.target.result;
    };

    reader.onerror = function()
    {
        utils__custom_alert("could not read the image");
    };

    reader.readAsDataURL(file);
}

/**
 * @brief attaches a file for the next send after the policy checks (uploads allowed, within the size limit, not empty); it replaces a pending picture and shows the chip
 *
 * @param File file -> the file
 *
 * @return void
 */
function chat_files__attach_chat_file(file)
{
    if (g_server_policy.allow_file_uploads == false)
    {
        utils__custom_alert("file uploads are not allowed on this server");
        return;
    }

    if (file.size > g_server_policy.file_upload_max_size)
    {
        utils__custom_alert("file is too big (" + chat_files__format_file_size(file.size) + "). this server allows up to " + chat_files__format_file_size(g_server_policy.file_upload_max_size));
        return;
    }

    if (file.size == 0)
    {
        utils__custom_alert("the file is empty");
        return;
    }

    let reader = new FileReader();

    reader.onload = function(event)
    {
        g_pending_chat_file = {
            name: chat_files__sanitize_file_name(file.name),
            size: file.size,
            mime: chat_files__sanitize_mime_type(file.type),
            base64: chat__remove_data_url_prefix_from_base64_string(event.target.result)
        };

        // one attachment per message, so a pending picture gives way
        chat_files__clear_pending_chat_picture();

        // the chip overlays the text box, so the typed text is cleared out of its way
        document.getElementById("chat-input-container-text-input").value = "";

        chat_files__render_pending_chat_file_chip();
    };

    reader.onerror = function()
    {
        utils__custom_alert("could not read the file");
    };

    reader.readAsDataURL(file);
}

/**
 * @brief drops the pending file and hides its chip
 *
 * @return void
 */
function chat_files__clear_pending_chat_file()
{
    g_pending_chat_file = null;
    chat_files__render_pending_chat_file_chip();
}

/**
 * @brief removes a pending picture: the thumbnail hides and nothing is left to send
 *        wired to the thumbnail's cancel cross, and used by the file path because one attachment replaces the other
 *
 * @return void
 */
function chat_files__clear_pending_chat_picture()
{
    g_base64_picture_string_to_send = "";
    document.getElementById("image-upload-preview").style.backgroundImage = "";
    document.getElementById("image-upload-preview").style.display = "none";
    document.getElementById("image-upload-preview").style.aspectRatio = "";
}

/**
 * @brief renders the pending file chip over the text box's corner, or nothing when no file is pending
 *
 * @return void
 */
function chat_files__render_pending_chat_file_chip()
{
    let chip = document.getElementById("file-upload-preview");

    if (chip == null)
    {
        return;
    }

    if (g_pending_chat_file == null)
    {
        chip.style.display = "none";
        chip.innerHTML = "";
        return;
    }

    chip.innerHTML = chat_files__generate_chat_file_icon_svg(g_pending_chat_file.name, "file-upload-preview-icon")
                   + "<span class=\"file-upload-preview-text\">"
                   + "<span class=\"file-upload-preview-name\">" + chat__sanitize_string(g_pending_chat_file.name) + "</span>"
                   + "<span class=\"file-upload-preview-size\">" + chat_files__format_file_size(g_pending_chat_file.size) + "</span>"
                   + "</span>"
                   + "<span class=\"file-upload-preview-remove\" title=\"remove\">&#10005;</span>";

    chip.querySelector(".file-upload-preview-remove").onclick = function(event)
    {
        event.stopPropagation();
        chat_files__clear_pending_chat_file();
    };

    chip.style.display = "flex";
}

/**
 * @brief the paperclip's file input
 *        it always attaches as a plain file, even an image, because the user chose the file button
 *        on purpose; the picture path belongs to the image button and drag and drop
 *
 * @return void
 */
function chat_files__choose_chat_file_input_onchange()
{
    if (G_HAS_DOM == false)
    {
        return;
    }

    let files = document.getElementById("choose-file-input").files;

    if (files.length > 0)
    {
        chat_files__attach_chat_file(files[0]);
    }
}

/**
 * @brief sets up drag and drop: the text box and the chat itself take drops; the document refuses them, because a missed drop would otherwise make the browser open the file as a page
 *
 * @return void
 */
function chat_files__setup_chat_file_drag_and_drop()
{
    if (G_HAS_DOM == false)
    {
        return;
    }

    document.addEventListener("dragover", function(event) { event.preventDefault(); });
    document.addEventListener("drop", function(event) { event.preventDefault(); });

    let zone_ids = ["chat-input-file-input-container", "chat-context-container"];

    for (let i = 0; i < zone_ids.length; i++)
    {
        let zone = document.getElementById(zone_ids[i]);

        if (zone == null)
        {
            continue;
        }

        // dragenter and dragleave fire for every child element; the depth counter turns them into one highlight
        let depth = 0;

        zone.addEventListener("dragenter", function(event)
        {
            event.preventDefault();
            depth++;
            zone.classList.add("chat-drop-active");
        });

        zone.addEventListener("dragleave", function()
        {
            depth--;

            if (depth <= 0)
            {
                depth = 0;
                zone.classList.remove("chat-drop-active");
            }
        });

        zone.addEventListener("dragover", function(event)
        {
            event.preventDefault();
            event.dataTransfer.dropEffect = "copy";
        });

        zone.addEventListener("drop", function(event)
        {
            event.preventDefault();
            depth = 0;
            zone.classList.remove("chat-drop-active");

            let files = (event.dataTransfer != null) ? event.dataTransfer.files : null;

            if (files == null || files.length == 0)
            {
                return;
            }

            if (files.length > 1)
            {
                utils__custom_alert("one file at a time - taking the first one");
            }

            chat_files__attach_chat_file_or_picture(files[0]);
        });
    }
}

/**
 * @brief the cursor-lit border of the file cards: one delegated mousemove on the chat writes the cursor position into the hovered card's --mx and --my, which the default theme's ring gradient follows
 *
 * @return void
 */
function chat_files__setup_chat_file_card_glow()
{
    if (G_HAS_DOM == false)
    {
        return;
    }

    let chat = document.getElementById("chat-context-container");

    if (chat == null)
    {
        return;
    }

    chat.addEventListener("mousemove", function(event)
    {
        let card = (event.target != null && typeof event.target.closest === "function") ? event.target.closest(".chat-file-card") : null;

        if (card == null)
        {
            return;
        }

        let box = card.getBoundingClientRect();
        card.style.setProperty("--mx", (event.clientX - box.left) + "px");
        card.style.setProperty("--my", (event.clientY - box.top) + "px");
    });
}

/**
 * @brief applies the server's upload policy to the paperclip button; it stays visible when uploads are off, only dimmed, so a click can still explain why
 *
 * @return void
 */
function chat_files__apply_file_upload_policy_to_ui()
{
    if (typeof document === "undefined")
    {
        return;
    }

    let button = document.getElementById("custom-file-upload-button-file");

    if (button == null)
    {
        return;
    }

    if (g_server_policy.allow_file_uploads == true)
    {
        button.classList.remove("chat-file-uploads-off");
        button.title = "attach a file (up to " + chat_files__format_file_size(g_server_policy.file_upload_max_size) + ")";
    }
    else
    {
        button.classList.add("chat-file-uploads-off");
        button.title = "file uploads are not allowed on this server";
    }
}

/**
 * @brief the same treatment for the picture button: it stays visible when pictures are off, only dimmed
 *
 * @return void
 */
function chat_files__apply_chat_picture_policy_to_ui()
{
    if (typeof document === "undefined")
    {
        return;
    }

    let button = document.getElementById("custom-file-upload-button-image");

    if (button == null)
    {
        return;
    }

    if (g_server_policy.allow_chat_pictures == true)
    {
        button.classList.remove("chat-file-uploads-off");
        button.title = "attach a picture (up to " + chat_files__format_file_size(g_server_policy.chat_picture_max_size) + ")";
    }
    else
    {
        button.classList.add("chat-file-uploads-off");
        button.title = "inline pictures are disabled on this server";
    }
}

// ---------------------------------------------------------------------------
// server settings tab
// ---------------------------------------------------------------------------

/**
 * @brief shows the ram and upload-time warning while the typed limit is over FILE_UPLOAD_WARNING_MB
 *
 * @return void
 */
function chat_files__refresh_file_upload_size_warning()
{
    let input = document.getElementById("server-settings-general-file-upload-max-size-input");
    let warning = document.getElementById("server-settings-general-file-upload-size-warning");

    if (input == null || warning == null)
    {
        return;
    }

    let megabytes = parseInt(input.value);

    warning.style.display = (megabytes > FILE_UPLOAD_WARNING_MB) ? "block" : "none";
}

/**
 * @brief the pictures checkbox shows or hides the picture max-size row under it
 *
 * @return void
 */
function chat_files__refresh_picture_size_visibility()
{
    let checkbox = document.getElementById("server-settings-general-allow-pictures-checkbox");
    let size_row = document.getElementById("server-settings-picture-max-size-row");

    if (checkbox == null || size_row == null)
    {
        return;
    }

    size_row.style.display = (checkbox.checked == true) ? "" : "none";
}

/**
 * @brief the uploads checkbox shows or hides the max-size row below it; the stored limit stays server state either way
 *        the size warning only makes sense while the row is visible
 *
 * @return void
 */
function chat_files__refresh_file_upload_size_visibility()
{
    let checkbox = document.getElementById("server-settings-general-allow-file-uploads-checkbox");
    let size_row = document.getElementById("server-settings-file-upload-size-row");
    let warning = document.getElementById("server-settings-general-file-upload-size-warning");

    if (checkbox == null || size_row == null || warning == null)
    {
        return;
    }

    if (checkbox.checked == true)
    {
        size_row.style.display = "";
        chat_files__refresh_file_upload_size_warning();
    }
    else
    {
        size_row.style.display = "none";
        warning.style.display = "none";
    }
}

// android-host.js is embedded in template.html along with the other client files, and in the node bundle
// it is the client's two android hosts: the headless node runtime of the background service (the workers
// stood in for in-process, the api the host calls), the webview bridge (the implementations behind the
// JavascriptJavaBridge__* functions of android-bridge.js), and the deep idle mode both of them use
// a desktop or website client wires none of it; dispatch.js calls the idle side

// state private to this file
// pending AudioContext.suspend() from idle entry; exit chains its resume on it so a
// late-settling suspend can never land after the resume and silence the graph
var audio_suspend_promise = null;

// the "leaving idle" request in flight: it keeps a late idle request from putting the client back to sleep
var is_come_from_idle_in_flight = false;

var come_from_idle_in_flight_timer = null;

// call-accept presence grace: gentle idle waits this long after an accept before re-deciding,
// so the accept transition's blink of invisibility cannot idle the user out of the call
var CALL_ACCEPT_PRESENCE_GRACE_MS = 5000;

var presence_grace_until_timestamp = 0;

var presence_grace_recheck_timer = null;

// ---- headless node runtime ----

/**
 * @brief whether this runtime only renders: the android webview draws the ui while node owns the connection over the loopback
 *        a ui-only runtime must not answer the protocol (keys, votes, transport encryption) or the
 *        client gets kicked; the workers have no Android object, so they key on the loopback port
 *        the main thread hands them
 *
 * @return boolean true for the webview and for a worker of it
 */
function android_host__is_ui_only_runtime()
{
    return (typeof Android !== "undefined") || g_loopback_port > 0;
}

/**
 * @brief a stand-in for the workers that genuinely do not exist headless; everything posted to it is dropped
 *
 * @return object an object with postMessage and terminate that do nothing
 */
function android_host__make_discarding_worker()
{
    return {
        postMessage: function() {},
        terminate: function() {}
    };
}

/**
 * @brief whether a person is in front of this client
 *        a browser or the webview always has somebody in front of it; headless node only does
 *        while a ui is attached. typeof process is how this codebase tells node from a page
 *
 * @return boolean true when somebody is looking
 */
function android_host__is_someone_watching_the_ui()
{
    if (typeof process === "undefined")
    {
        return true;
    }

    return g_node_has_attached_ui;
}

/**
 * @brief node with no ui is an idle client: present on the server so it can be called and messaged, but not standing in a channel as if somebody were there
 *        a ui attaching means the user arrived, so the client comes back out of idle; a page
 *        manages its own idle through the activity lifecycle and leaves at once
 *
 * @return void
 */
function android_host__node_apply_idle_for_ui_state()
{
    if (typeof process === "undefined")
    {
        return; // a page manages its own idle through the activity lifecycle
    }

    // android_host__enter_deep_idle and android_host__exit_deep_idle own g_is_deep_idle and are both idempotent, so this
    // can run on every attach and detach without tracking state here
    try
    {
        if (g_node_has_attached_ui == true)
        {
            // rejoin the channel that was open, or root, because the server drops a null channel id
            let channel_to_rejoin = (typeof g_current_channel_id === "number") ? g_current_channel_id : 0;

            console.log("connect-path: ui attached, leaving idle into channel " + channel_to_rejoin);
            android_host__exit_deep_idle();
            client_msg.send_come_from_idle_mode_request(channel_to_rejoin);
        }
        else
        {
            console.log("connect-path: no ui attached, going idle");
            android_host__enter_deep_idle();
        }
    }
    catch (idle_switch_failed)
    {
        console.error("idle switch failed: " + idle_switch_failed);
    }
}

g_show_message_avatars = (utils__storage_get("lemon_show_message_avatars") === "1");

let stored_rsa_key_bits = parseInt(utils__storage_get("lemon_rsa_key_bits", ""));
if (G_ALLOWED_RSA_KEY_BITS.indexOf(stored_rsa_key_bits) >= 0) { g_rsa_key_bits = stored_rsa_key_bits; }

g_show_seen_indicator = (utils__storage_get("lemon_seen_indicator") !== "0");

g_send_seen_receipts = (utils__storage_get("lemon_send_seen") !== "0");

g_auto_scroll_chat_to_end = (utils__storage_get("lemon_auto_scroll") !== "0");

g_hide_microphone_button = (utils__storage_get("lemon_hide_mic") === "1");

g_selected_microphone_device_id = utils__storage_get("lemon_mic_device_id", "");

g_is_continuous_mic_mode = (utils__storage_get("lemon_continuous_mic") === "1");

let stored_key_code = parseInt(utils__storage_get("lemon_ptt_key_code"));
let stored_key_label = utils__storage_get("lemon_ptt_key_label");
if (stored_key_code > 0) { g_push_to_talk_key_code = stored_key_code; }
if (stored_key_label) { g_push_to_talk_key_label = stored_key_label; }

/**
 * @brief the sender header of one message: an optional avatar and the name, tagged with the sender id so a rename can find and retag old messages
 *
 * @param number|null sender_client_id -> the sender, null for an untagged header
 * @param string display_name_html -> the name, already sanitized
 *
 * @return string the header html
 */
function android_host__generate_message_sender_html(sender_client_id, display_name_html)
{
    let avatar_html = "";

    if (g_show_message_avatars == true && sender_client_id != null)
    {
        let sender = channel_tree__get_client_by_client_id(sender_client_id);

        if (sender != null && typeof sender.base64_avatar === "string" && sender.base64_avatar.length > 0)
        {
            avatar_html = "<img class=\"chat-message-avatar\" src=\"" + sender.base64_avatar + "\">";
        }
    }

    let id_attribute = (sender_client_id != null) ? (" data-sender-id=\"" + sender_client_id + "\"") : "";

    return avatar_html + "<p" + id_attribute + ">" + display_name_html + "</p>";
}

/**
 * @brief renames every already-rendered message header of one sender, after a rename or an alias change
 *
 * @param number sender_client_id -> the sender
 * @param string display_name -> the new name
 *
 * @return void
 */
function android_host__retag_rendered_messages_of_sender(sender_client_id, display_name)
{
    let name_nodes = document.querySelectorAll('p[data-sender-id="' + sender_client_id + '"]');

    for (let i = 0; i < name_nodes.length; i++)
    {
        name_nodes[i].textContent = display_name;
    }
}

/**
 * @brief the host's park flag: with false the socket closes and the driver idles instead of redialing, with true the driver may dial again right away
 *        re-dialing after a re-arm rides the settings push; this only owns the flag and the park
 *
 * @param boolean is_wanted -> whether node should hold a connection
 *
 * @return void
 */
function android_host__node_set_connection_wanted(is_wanted)
{
    g_node_connection_wanted = (is_wanted == true);

    if (g_node_connection_wanted == false && g_websocket_worker != null)
    {
        g_websocket_worker.postMessage({ type: "close" });
    }

    // re-dialing after a re-arm rides the settings push, connection__request_connect("settings")
    // this function only owns the wanted flag and the park
}

/**
 * @brief runs a handler on one message and contains any throw, because the loopback workers run in-process and node can not be restarted once dead; one malformed message must not kill the runtime
 *        the message listeners are told afterwards, with whether the handler failed
 *
 * @param function handler -> the worker entry point to call
 * @param object message -> the message, handed over as { data: message }
 * @param string source_name -> which worker it stands for, for the log
 *
 * @return void
 */
function android_host__dispatch_safely(handler, message, source_name)
{
    let caught_error = null;

    try
    {
        handler({ data: message });
    }
    catch (dispatch_error)
    {
        caught_error = dispatch_error;
        console.error("handler failed for '" + ((message != null && message.type) ? message.type : "?")
            + "' via " + source_name + ": "
            + (dispatch_error != null && dispatch_error.stack ? dispatch_error.stack : dispatch_error));
    }

    // this fires per internal hop, so several times per server message. it only means state
    // may have changed; the host side debounces
    for (let i = 0; i < g_node_message_listeners.length; i++)
    {
        try
        {
            g_node_message_listeners[i](((message != null && message.type) ? message.type : ""), caught_error != null);
        }
        catch (callback_error)
        {
            console.error("message listener failed: " + callback_error);
        }
    }
}

/**
 * @brief a stand-in for a real webworker: the entry points are plain functions in this scope, so node runs them in-process
 *        the reply is async on purpose, like a real worker's message queue
 *
 * @param function handler -> the worker entry point
 * @param string worker_name -> the name, for the log
 *
 * @return object an object with postMessage and terminate
 */
function android_host__make_loopback_worker(handler, worker_name)
{
    return {
        postMessage: function(message)
        {
            setTimeout(function() { android_host__dispatch_safely(handler, message, worker_name); }, 0);
        },
        terminate: function() {}
    };
}

/**
 * @brief wires the headless runtime the way main__window_onload does for the browser, minus everything that needs a dom
 *        it lives inside the factory because a bootstrap outside cannot assign the factory's locals
 *
 * @param string|null identity_passphrase_string -> the passphrase the identity is derived from, null for a random one
 *
 * @return void
 */
function android_host__init_node_runtime(identity_passphrase_string)
{
    // every outgoing message goes through utils__custom_log, which falls back to global.postMessage
    // when this is null. a dead element's .value is "", so the append and the 50kb truncation
    // both work harmlessly and main.js needs no change
    g_textarea_log = document.getElementById("textarea-log");

    // the entry points reply with global.postMessage; here global is the shim window, so the reply
    // is routed to the same dispatcher a browser uses, contained so a throwing handler cannot kill node
    global.postMessage = function(message)
    {
        // raw frames go to the frame listener only, not into the dispatcher
        if (message != null && message.type === "decrypted_frame")
        {
            // the cheap indexOf check runs first and the real parse only on a hit, so a chat
            // message containing the literal can not overwrite the cached frame
            if (message.value.indexOf("\"authentication_status\"") !== -1)
            {
                try
                {
                    if (JSON.parse(message.value).message.type === "authentication_status")
                    {
                        g_node_cached_auth_frame = message.value;
                    }
                }
                catch (e) { }
            }

            if (g_node_frame_listener != null)
            {
                try { g_node_frame_listener(message.value); }
                catch (listener_error) { console.error("frame listener failed: " + listener_error); }
            }
            return;
        }

        setTimeout(function() { android_host__dispatch_safely(dispatch__mainthread_onmessage, message, "mainthread"); }, 0);
    };

    // workers__websocket_worker_onmessage does `new WebSocket(...)`, which node has natively, so the
    // transport needs no replacement, only somewhere to run
    g_websocket_worker = android_host__make_loopback_worker(workers__websocket_worker_onmessage, "websocket_worker");
    g_data_processing_worker = android_host__make_loopback_worker(workers__data_processing_worker_onmessage, "data_processing_worker");

    // every decrypted frame also reaches the frame listener, for the ui replay
    g_data_processing_worker.postMessage({ type: "mainthread__set_frame_forwarding", value: true });

    // the last line of defence. the heartbeat loss loop is an async while and one escaped throw
    // can kill it silently; a setInterval survives its own throws, so this watchdog always runs
    setInterval(function()
    {
        try
        {
            let heartbeat_age = new Date().valueOf() - g_connection_check.last_response_timestamp;

            if (g_is_authenticated == true
                && g_connection_check.last_response_timestamp > 0
                && heartbeat_age > (g_connection_check.lost_threshold_ms + 15000))
            {
                console.error("watchdog: heartbeat response " + heartbeat_age + "ms old, forcing reset + reconnect");
                g_is_authenticated = false;
                connection__reset_chat_app_keep_identity();
            }
        }
        catch (watchdog_error)
        {
            console.error("watchdog failed: " + watchdog_error);
        }
    }, 15000);

    // logs one greppable line per minute, so a dead night can be reconstructed from logcat
    setInterval(function()
    {
        try
        {
            let heartbeat_age = (g_connection_check.last_response_timestamp > 0)
                ? (new Date().valueOf() - g_connection_check.last_response_timestamp) : -1;
            console.log("conn: auth=" + g_is_authenticated + " ws=" + g_is_websocket_connected
                + " hb_age_ms=" + heartbeat_age + " checker=" + g_should_connection_check_be_running);
        }
        catch (log_error) { }
    }, 60000);

    // opus and minimp3 are not in this bundle, but shared code posts to them unconditionally; a
    // discarding stand-in keeps those call sites working, messages to it go nowhere on purpose
    g_opus_decoder_worker = android_host__make_discarding_worker();
    g_opus_encoder_worker = android_host__make_discarding_worker();
    g_minimp3_worker = android_host__make_discarding_worker();

    // a persisted identity can start generating right away; otherwise the settings push
    // supplies the seed. the driver waits on the identity slot either way
    if (typeof identity_passphrase_string === "string" && identity_passphrase_string.length >= 199)
    {
        connection__request_identity(identity_passphrase_string);
    }

    connection__connection_driver();
}

// ---- webview bridge ----

var android_js_bridge = {
    /**
     * @brief java's "go idle": enters deep idle, which sends the request to the server itself and shuts down audio, the opus tick and the datachannel
     *        a gentle call (home button) keeps an in-channel session alive; a forced one (swipe-away) idles from any channel
     *
     * @param boolean is_forced -> true for a swipe-away
     *
     * @return void
     */
    send_go_to_idle_mode_request_android: function(is_forced)
    {
        // android_host__enter_deep_idle sends the go-to-idle request to the server itself and additionally
        // shuts down audio, the opus tick and the webrtc datachannel. a gentle call (home
        // button) keeps an in-channel session alive; a forced one (swipe-away) idles from any channel
        android_host__enter_deep_idle(is_forced === true);
    },

    /**
     * @brief java's "leave idle": asks the server to bring the client back into a channel
     *        sometimes runs twice (a call accept, then onResume) and the server refuses the second;
     *        a channel id means a call accept, so presence is held through the accept transition
     *
     * @param number|null channelId -> the channel to rejoin, null for root
     *
     * @return void
     */
    send_come_from_idle_mode_request_android: function(channelId = null)
    {
        // this function sometimes gets ran twice,
        // when client accepts the call and a bit later when app runs onResume (because he accepted it)
        // but server is smart and doesnt let user come back from idle mode twice

        // a channel id means a call accept, so presence is held, because the accept
        // transition's blink of invisibility could otherwise idle the user right back
        // out of the call
        if (channelId != null)
        {
            android_host__mark_call_accept_presence_grace();
        }

        if (channelId == null)
        {
            channelId = 0; // unspecified, so root channel.
        }

        // restore audio, the opus tick, webrtc and the fast heartbeat before rejoining
        android_host__exit_deep_idle();

        client_msg.send_come_from_idle_mode_request(channelId);
    },

    /**
     * @brief java calls this on ACTION_ACCEPT_CALL, so the webview's own idle logic knows a call was just accepted; node learns it through its come-from-idle instead
     *
     * @return void
     */
    mark_call_accept_presence_android: function()
    {
        android_host__mark_call_accept_presence_grace();
    },

    /**
     * @brief java hands over the username: it is sent as a rename now and remembered as the chosen name, so the next connect applies it at login (a rename the admin may have switched off for users)
     *
     * @param string username -> the name, "" is ignored
     *
     * @return void
     */
    set_username_on_connect_android: function(username)
    {
        if (username.length == 0)
        {
            return;
        }

        // remembered as the chosen username too, because on the next connect the
        // server can then apply the name at login instead of through a rename -
        // a rename the admin may have switched off for users
        g_chosen_username = username;

        client_msg.send_change_client_username_request(username, g_local_client_id);

        let index = channel_tree__get_client_index_in_array_by_client_id(g_local_client_id);

        if (index != -1)
        {
            g_client_list[index].username = username;
        }

        g_local_username = username;
        { let rename_input = document.getElementById('connected-local-client-input'); if (rename_input != null) { rename_input.setAttribute('value', g_local_username); } }
    },

    /**
     * @brief node's connection phase relayed by java
     *        it feeds the page's own status machinery, which the ticker keeps repainting, and keeps
     *        the spinner up while node works; nothing once authenticated
     *
     * @param string state -> the phase name
     * @param string reason -> the text to show with it
     *
     * @return void
     */
    show_connection_phase_android: function(state, reason)
    {
        if (g_is_authenticated == true)
        {
            return;
        }

        g_connection_status.state = state;
        g_connection_status.reason = (reason != null) ? reason : "";
        connection__render_connection_status();

        if (state === "connecting" || state === "connected")
        {
            connection__extend_connect_page_holdback();
        }
        else
        {
            connection__reveal_connect_page();
        }
    },

    /**
     * @brief java says node is connected or the app came to front
     *        the dial decision belongs to connection__request_connect; this only self-heals a lost
     *        settings push first (no loopback details yet means one got lost, so it asks again)
     *
     * @return void
     */
    nudge_loopback_reattach_android: function()
    {
        if (g_is_authenticated == true)
        {
            return;
        }

        // no loopback details yet means a settings push got lost, so ask again
        if (g_loopback_port <= 0)
        {
            if (typeof Android !== "undefined")
            {
                Android.JavaExportRequestCurrentSettingsFromAndroid();
            }
            return;
        }

        connection__request_connect("resume");
    },

    /**
     * @brief takes the settings json java pushes: app mode and theme first, then the loopback details, the identity, the microphone mode and the autoconnect, and finally hands the driver its target
     *        the theme is applied before anything below can throw: the settings json grows
     *        incrementally on the java side, and a missing field must not skip the theme
     *
     * @param string json_current_settings -> the settings as json
     *
     * @return void
     */
    accept_current_settings_from_android: function(json_current_settings)
    {

        let settings_from_android = JSON.parse(json_current_settings);

        // the theme is applied first, before anything below can throw: the settings json
        // grows incrementally on the java side, and a missing field must not skip the theme
        g_android_app_mode = (typeof settings_from_android.app_mode === "string") ? settings_from_android.app_mode : "";
        console.log("android settings received, app_mode = " + g_android_app_mode);

        main__apply_theme_for_app_mode();

        g_are_server_details_predefined = true;
        is_autoconnect_enabled = true;

        // there is difference between how web browser app and how android app handles auto connects

        // in a browser: predefined details show only the connect button, no details show the
        // form as well, and autoconnect without user action shows neither and keeps dialing

        // in android
        // autoconnect is on -> connect button doesnt show, app goes in loop and tries to join the server until it succeeds
        // autoconnect is off -> connect button shows (but server details need specified in android settings)

        // these differences exists because web browser has limitation, sometimes it requries user interaction to play soudns and android app doesnt
        // different enviroments sometimes need different way of doing things

        // yes, this is confusing, and needs improvements, should be documentated better

        // a first-run json may lack these fields; an absent field means on, because a
        // fresh install must connect (undefined == false is false in js, so the checks need it)
        g_is_autoconnect_without_user_action_active = (settings_from_android.is_autoconnect_enabled != false);

        // android decides if the log file is written. the page only copies the answer
        g_is_file_logging_enabled = (settings_from_android.is_file_logging_enabled != false);

        // a choice made in the local settings panel outranks the android settings json
        let has_local_sound_choice = false;
        has_local_sound_choice = (utils__storage_get("lemon_sound_effects") != null);

        if (has_local_sound_choice == false)
        {
            g_are_sound_effects_enabled = (settings_from_android.is_audio_effect_enabled == true);
        }

        // the mic has to start or stop with the flag; the connect path applies it on the first
        // push, so only a switch moved while running is acted on here. the flag is set directly,
        // not through the click handler, so a runtime with no ui sets it and touches no audio
        let is_microphone_always_on_wanted = (settings_from_android.is_microphone_always_on == true);

        if (g_have_received_android_settings == false)
        {
            voice__set_microphone_always_on(is_microphone_always_on_wanted);
        }
        else if (is_microphone_always_on_wanted != g_is_microphone_always_on)
        {
            voice__set_microphone_always_on(is_microphone_always_on_wanted);
            UI.activate_continous_audio_broadcast_apply();
        }

        g_have_received_android_settings = true;

        // the app log is opt-in from the java settings. the open textarea is hidden too
        // when the switch turns off, because it would otherwise linger with no button to close it
        if (settings_from_android.is_app_log_enabled == true)
        {
            document.getElementById("show-hide-log-button").style.display = "";
        }
        else
        {
            document.getElementById("show-hide-log-button").style.display = "none";
            document.getElementById("textarea-log").style.display = "none";
        }

        // on android the details come from the java settings window, so the in-page form
        // is hidden (display none, no dead gap); autoconnect off means the user needs a connect button
        document.getElementById("connect-form-sub-container-1").style.display = "none";
        document.getElementById("connect-form-sub-container-2").style.display = "none";
        document.getElementById("add-key-button").style.display = "none";

        // bookmarks fill those hidden fields, so they have nothing to act on here
        document.getElementById("server-bookmarks-container").style.display = "none";

        if (g_is_autoconnect_without_user_action_active == false)
        {
            document.getElementById("another-buttons-sub-container").style.display = "";
            document.getElementById("another-buttons-sub-loading-container").style.display = "none";
            // explicit visible is needed because a stylesheet default hides it; the spinner
            // page has an opaque background now, so the button can not float through anymore
            document.getElementById("connect-button").style.visibility = "visible";
            document.getElementById("import-identity-button").style.display = "none";

            // nothing dials on its own here: no spinner, the connect page is the page
            connection__reveal_connect_page();
        }

        if (g_are_sound_effects_enabled == true)
        {
            document.getElementById("sound-effects-button").style.backgroundImage = "url(data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiIHN0YW5kYWxvbmU9Im5vIj8+PCFET0NUWVBFIHN2ZyBQVUJMSUMgIi0vL1czQy8vRFREIFNWRyAxLjEvL0VOIiAiaHR0cDovL3d3dy53My5vcmcvR3JhcGhpY3MvU1ZHLzEuMS9EVEQvc3ZnMTEuZHRkIj48c3ZnIHdpZHRoPSIxODJweCIgaGVpZ2h0PSIxNjBweCIgdmVyc2lvbj0iMS4xIiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHhtbG5zOnhsaW5rPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5L3hsaW5rIiB4bWw6c3BhY2U9InByZXNlcnZlIiB4bWxuczpzZXJpZj0iaHR0cDovL3d3dy5zZXJpZi5jb20vIiBzdHlsZT0iZmlsbC1ydWxlOmV2ZW5vZGQ7Y2xpcC1ydWxlOmV2ZW5vZGQ7c3Ryb2tlLWxpbmVqb2luOnJvdW5kO3N0cm9rZS1taXRlcmxpbWl0OjI7Ij48ZyB0cmFuc2Zvcm09Im1hdHJpeCgwLjI0LDAsMCwwLjI0LC01MTguMDg4MjEyLC01NDIuNDU0NDM4KSI+PGc+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMTIwLjkxNDAwNCwyMTUwLjU1MzYwNSkiPjxwYXRoIGQ9Ik00NDIsMTM0QzQ0MywxMzUgNDQzLDEzNSA0NDksMTQwQzQ1NCwxNDYgNDUzLDE0NiA0NTksMTUyQzQ3MywxNjYgNDcyLDE3MyA0NzUsMTgyQzQ3NiwxODggNDg1LDIyMyA0NjgsMjU4QzQ2NSwyNjMgNDQ2LDMwMiA0MTcsMzE3QzQxMSwzMjAgMzcwLDMxOCAzNDAsMzMyQzI5NywzNTQgMzAxLDM2MCAyNjIsMzg5QzI1NiwzOTMgMjU1LDM5MyAyNDcsMzg2QzI0MSwzNzkgMjEzLDM2OCAyMDcsMzY4QzIwMSwzNjcgMjAyLDM2OCAxOTUsMzY5QzE0MCwzNzUgMTU2LDQ4NiAyMDEsNDkyQzIyNSw0OTUgMjIyLDUwNiAyMjEsNTQ1QzIyMSw1NDggMjIwLDU0NyAyMjAsNTUwQzIxOSw1NzUgMjE3LDU3NCAyMTYsNTk5QzIxNiw2MDEgMjE1LDYwMiAyMTUsNjA1QzIxMiw2NzkgMjI3LDcxNCAyNTAsNzQ5QzI1Nyw3NTkgMjM5LDc1OCAyMzYsNzU3QzIyNyw3NTMgMTkzLDc1OCAxNDEsNzQwQzEzMiw3MzcgMTMyLDczNiAxMjQsNzMzQzg1LDcyMCA2MSw2ODUgNTcsNjc3QzQyLDY0NiA0Niw2MjcgNDUsNjIzQzQxLDYxMSA0NCw2MDcgNDMsNjAyQzM5LDU5MSA0MSw1MzcgMzgsNTI1QzM4LDUyNSAzOCw0NzkgMzgsNDc5QzQzLDQ1NiAzNyw0MjkgNDAsNDI0QzQ1LDQxNCAzMCwyNTYgMTQ5LDE2MkMxNTMsMTU5IDE5MSwxMzYgMTk5LDEzNEMyMDMsMTMyIDI0NiwxMTggMjQ5LDExOEMyNjgsMTE3IDI2NywxMTMgMjg5LDExM0MyOTEsMTEzIDI5MCwxMTEgMjkyLDExMUMzMTEsMTA5IDM3MSwxMDkgMzc1LDExMUMzODEsMTE0IDM4NSwxMTIgMzg3LDExM0MzOTQsMTE3IDM5NSwxMTUgMzk1LDExNUM0MDIsMTE4IDQwMiwxMTYgNDAyLDExN0M0MTEsMTIxIDQzOCwxMjQgNDQyLDEzNFoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIxMjAuOTE0MDA0LDIxNTAuNTUzNjA1KSI+PHBhdGggZD0iTTQ0NSwxMzJDNDQ3LDEzMCA0NDcsMTI5IDQ0OSwxMzFDNDU0LDEzMyA0NTUsMTMxIDQ1OCwxMzJDNDY4LDEzNyA1ODIsMTU0IDU4MiwyNjNDNTgyLDI2NSA1ODAsMjY1IDU4MCwyNjdDNTc5LDMyMSA1NTQsMzIxIDU0NywzMDVDNTQyLDI5NiA1NTgsMjYxIDU0NywyMjhDNTM2LDE5NiA0OTksMTc1IDUwOCwxODRDNTE2LDE5MyA1MTUsMTk0IDUyMiwyMDRDNTM2LDIyNSA1NDIsMjYxIDU0MiwyNjNDNTQyLDI4OSA1NDEsMjg4IDU0MCwzMDZDNTQwLDMwOSA1MzksMzA4IDUzOSwzMTFDNTM4LDMxNiA1MzgsMzE1IDUzNywzMjFDNTM2LDMyOCA1MzUsMzI3IDUzMywzMzRDNTMzLDMzOSA1MjksMzUwIDUyOCwzNTNDNTI2LDM2NCA1MjEsMzY1IDUyNywzNzVDNTM3LDM4OSA2MDUsNDMyIDU0OCw0NjJDNTQxLDQ2NSA1NDIsNDY2IDU0NSw0NzNDNTU0LDQ5MSA1MzUsNDk1IDUzNyw0OTlDNTUyLDUxOCA1MzAsNTI0IDUzMiw1MzJDNTM1LDU0MyA1MzMsNTYyIDUzMSw1NjRDNTIzLDU4MCA1MjEsNTg1IDUwNCw1OTNDNDk1LDU5OCA0NjMsNTkxIDQ2Myw1OTFDNDQyLDU4MSA0MjcsNTkwIDQyNCw1OTJDNDEyLDU5OSA0MTUsNjAyIDM5MSw2NzVDMzgyLDcwNSAzODMsNzA2IDM4Miw3MDhDMzc5LDcxNSAzODIsNzE3IDM4Myw3MTlDMzgzLDczMiA0MDEsNzQ3IDQwNSw3NTJDNDE3LDc2NCA0MDAsNzczIDM5OSw3NzRDMzkzLDc3NyAyOTcsNzYzIDI5Myw3NjJDMjkwLDc2MiAyNTUsNzA3IDI1NSw3MDVDMjU0LDcwNSAyNTQsNzAyIDI1MSw2OTRDMjI5LDY0MSAyNDcsNTY4IDI0Nyw1MjBDMjQ3LDUwNCAyNDYsNTA1IDI0Miw0ODlDMjQxLDQ4MiAyMjcsNDcyIDIyNSw0NzJDMjEzLDQ2OSAyMTQsNDY4IDIxMiw0NjhDMTg3LDQ2NiAxODIsNDI1IDE4Nyw0MTBDMTk5LDM3MiAyMzQsNDEwIDIzOCw0MTJDMjU4LDQyMyAyNjYsNDE2IDI3Myw0MTNDMjkxLDQwNCAzMzgsMzQ1IDM5MywzNDVDNDMxLDM0NSA0NDEsMzMyIDQ1NiwzMjBDNDU5LDMxNyA0NTksMzE3IDQ2MiwzMTRDNDY0LDMxMiA0NzYsMjk5IDQ4MywyODZDNTA0LDI0MyA0OTgsMjM5IDUwMSwyMzRDNTA0LDIyNiA1MDIsMjIzIDUwMiwyMjFDNTA2LDIxMiA1MDEsMTk0IDUwNSwxODNDNTA3LDE3OCA0NDcsMTM3IDQ0NSwxMzJaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMDY4LjI4ODM4MywyMTE1LjcxMDQ3OSkiPjxwYXRoIGQ9Ik04NDMsNDcwQzg0NCw0ODMgODQ1LDQ4MiA4NDUsNDk0Qzg0NSw1MjcgODQ1LDUyNyA4NDIsNTQ2QzgzMyw1OTAgODMxLDU4OSA4MTIsNjI5QzgxMiw2MzAgNzk5LDY0OSA3OTgsNjUwQzc5OCw2NTEgNzc4LDY3NyA3NzAsNjgyQzc2MSw2ODkgNzM0LDY3NyA3NTYsNjU1QzgwNSw2MDYgODQ0LDUwMiA3OTAsNDAxQzc2MSwzNDYgNzMzLDM0NiA3NTUsMzI3Qzc2NywzMTYgNzgxLDMzOCA3OTIsMzUwQzgwMSwzNTkgODM0LDQxNSA4MzcsNDM4QzgzNyw0NDIgODM5LDQ0MiA4NDAsNDUzQzg0MCw0NTQgODQzLDQ2MiA4NDMsNDcwWiIgc3R5bGU9ImZpbGw6d2hpdGU7Ii8+PC9nPjxnIHRyYW5zZm9ybT0ibWF0cml4KDEsMCwwLDEsMjA2OC4yODgzODMsMjExNS43MTA0NzkpIj48cGF0aCBkPSJNNzI2LDQ3MUM3MjUsNDY4IDcyMCw0NTIgNzE4LDQ1MEM2OTcsNDA3IDY3NCw0MDYgNjk0LDM4OEM3MDIsMzgxIDcxNSwzODkgNzE4LDM5NkM3MjEsNDAyIDczMiw0MDkgNzQ0LDQzNkM3NjMsNDc0IDc2MCw1MTMgNzYwLDUxM0M3NTgsNTE5IDc1Nyw1MzUgNzU3LDUzOEM3NTIsNTYyIDczMiw1OTcgNzI1LDYwNEM3MTUsNjE1IDcxNyw2MTggNzA0LDYyNEM2OTksNjI2IDY4OCw2MTcgNjg4LDYxM0M2ODQsNTkzIDcwNiw1OTQgNzIxLDU1M0M3MjUsNTQwIDcyNSw1NDAgNzI4LDUyN0M3MzMsNDk4IDcyNiw0NzggNzI2LDQ3MVoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIwNjguMjg4MzgzLDIxMTUuNzEwNDc5KSI+PHBhdGggZD0iTTY1NSw1MTZDNjU1LDUxNCA2NTcsNTA2IDY1NCw0OTFDNjUwLDQ2NyA2MTgsNDUyIDY0Niw0MzdDNjU1LDQzMyA2NzEsNDU0IDY3NSw0NjJDNzA4LDUyNSA2NTcsNTgzIDY0Myw1NjlDNjIwLDU0OCA2NTEsNTQ3IDY1NSw1MTZaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PC9nPjxnIHRyYW5zZm9ybT0ibWF0cml4KDEsMCwwLDEsMjA2OC4yODgzODMsMjExNS43MTA0NzkpIj48cGF0aCBkPSJNODQzLDQ3MEM4NDQsNDgzIDg0NSw0ODIgODQ1LDQ5NEM4NDUsNTI3IDg0NSw1MjcgODQyLDU0NkM4MzMsNTkwIDgzMSw1ODkgODEyLDYyOUM4MTIsNjMwIDc5OSw2NDkgNzk4LDY1MEM3OTgsNjUxIDc3OCw2NzcgNzcwLDY4MkM3NjEsNjg5IDczNCw2NzcgNzU2LDY1NUM4MDUsNjA2IDg0NCw1MDIgNzkwLDQwMUM3NjEsMzQ2IDczMywzNDYgNzU1LDMyN0M3NjcsMzE2IDc4MSwzMzggNzkyLDM1MEM4MDEsMzU5IDgzNCw0MTUgODM3LDQzOEM4MzcsNDQyIDgzOSw0NDIgODQwLDQ1M0M4NDAsNDU0IDg0Myw0NjIgODQzLDQ3MFoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIwNjguMjg4MzgzLDIxMTUuNzEwNDc5KSI+PHBhdGggZD0iTTcyNiw0NzFDNzI1LDQ2OCA3MjAsNDUyIDcxOCw0NTBDNjk3LDQwNyA2NzQsNDA2IDY5NCwzODhDNzAyLDM4MSA3MTUsMzg5IDcxOCwzOTZDNzIxLDQwMiA3MzIsNDA5IDc0NCw0MzZDNzYzLDQ3NCA3NjAsNTEzIDc2MCw1MTNDNzU4LDUxOSA3NTcsNTM1IDc1Nyw1MzhDNzUyLDU2MiA3MzIsNTk3IDcyNSw2MDRDNzE1LDYxNSA3MTcsNjE4IDcwNCw2MjRDNjk5LDYyNiA2ODgsNjE3IDY4OCw2MTNDNjg0LDU5MyA3MDYsNTk0IDcyMSw1NTNDNzI1LDU0MCA3MjUsNTQwIDcyOCw1MjdDNzMzLDQ5OCA3MjYsNDc4IDcyNiw0NzFaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMDY4LjI4ODM4MywyMTE1LjcxMDQ3OSkiPjxwYXRoIGQ9Ik02NTUsNTE2QzY1NSw1MTQgNjU3LDUwNiA2NTQsNDkxQzY1MCw0NjcgNjE4LDQ1MiA2NDYsNDM3QzY1NSw0MzMgNjcxLDQ1NCA2NzUsNDYyQzcwOCw1MjUgNjU3LDU4MyA2NDMsNTY5QzYyMCw1NDggNjUxLDU0NyA2NTUsNTE2WiIgc3R5bGU9ImZpbGw6d2hpdGU7Ii8+PC9nPjwvZz48L3N2Zz4=)";
        }
        else
        {
            document.getElementById("sound-effects-button").style.backgroundImage = "url(data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiIHN0YW5kYWxvbmU9Im5vIj8+PCFET0NUWVBFIHN2ZyBQVUJMSUMgIi0vL1czQy8vRFREIFNWRyAxLjEvL0VOIiAiaHR0cDovL3d3dy53My5vcmcvR3JhcGhpY3MvU1ZHLzEuMS9EVEQvc3ZnMTEuZHRkIj48c3ZnIHdpZHRoPSIxODJweCIgaGVpZ2h0PSIxNjBweCIgdmVyc2lvbj0iMS4xIiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHhtbG5zOnhsaW5rPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5L3hsaW5rIiB4bWw6c3BhY2U9InByZXNlcnZlIiB4bWxuczpzZXJpZj0iaHR0cDovL3d3dy5zZXJpZi5jb20vIiBzdHlsZT0iZmlsbC1ydWxlOmV2ZW5vZGQ7Y2xpcC1ydWxlOmV2ZW5vZGQ7c3Ryb2tlLWxpbmVqb2luOnJvdW5kO3N0cm9rZS1taXRlcmxpbWl0OjI7Ij48ZyB0cmFuc2Zvcm09Im1hdHJpeCgwLjI0LDAsMCwwLjI0LC01MTguMDg4MjEyLC01NDIuNDU0NDM4KSI+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMTIwLjkxNDAwNCwyMTUwLjU1MzYwNSkiPjxwYXRoIGQ9Ik0zMzEsMzM3QzI5NywzNTUgMjk4LDM2MyAyNjIsMzg5QzI1NiwzOTMgMjU1LDM5MyAyNDcsMzg2QzI0MSwzNzkgMjEzLDM2OCAyMDcsMzY4QzIwMSwzNjcgMjAyLDM2OCAxOTUsMzY5QzE0MCwzNzUgMTU2LDQ4NiAyMDEsNDkyQzIyNSw0OTUgMjIyLDUwNiAyMjEsNTQ1QzIyMSw1NDggMjIwLDU0NyAyMjAsNTUwQzIxOSw1NzUgMjE3LDU3NCAyMTYsNTk5QzIxNiw2MDEgMjE1LDYwMiAyMTUsNjA1QzIxMiw2NzkgMjI3LDcxNCAyNTAsNzQ5QzI1Nyw3NTkgMjM5LDc1OCAyMzYsNzU3QzIyNyw3NTMgMTkzLDc1OCAxNDEsNzQwQzEzMiw3MzcgMTMyLDczNiAxMjQsNzMzQzg1LDcyMCA2MSw2ODUgNTcsNjc3QzQyLDY0NiA0Niw2MjcgNDUsNjIzQzQxLDYxMSA0NCw2MDcgNDMsNjAyQzM5LDU5MSA0MSw1MzcgMzgsNTI1QzM4LDUyNSAzOCw0NzkgMzgsNDc5QzQzLDQ1NiAzNyw0MjkgNDAsNDI0QzQ1LDQxNCAzMSwyNjYgMTM5LDE3MUwzMzEsMzM3Wk0xNzksMTQ0QzE4OCwxMzkgMTk1LDEzNSAxOTksMTM0QzIwMywxMzIgMjQ2LDExOCAyNDksMTE4QzI2OCwxMTcgMjY3LDExMyAyODksMTEzQzI5MSwxMTMgMjkwLDExMSAyOTIsMTExQzMxMSwxMDkgMzcxLDEwOSAzNzUsMTExQzM4MSwxMTQgMzg1LDExMiAzODcsMTEzQzM5NCwxMTcgMzk1LDExNSAzOTUsMTE1QzQwMiwxMTggNDAyLDExNiA0MDIsMTE3QzQxMSwxMjEgNDM4LDEyNCA0NDIsMTM0QzQ0MywxMzUgNDQzLDEzNSA0NDksMTQwQzQ1NCwxNDYgNDUzLDE0NiA0NTksMTUyQzQ3MywxNjYgNDcyLDE3MyA0NzUsMTgyQzQ3NiwxODggNDg1LDIyMyA0NjgsMjU4QzQ2NSwyNjMgNDQ2LDMwMiA0MTcsMzE3QzQxNCwzMTggNDAwLDMxOCAzODMsMzIxTDE3OSwxNDRaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMTIwLjkxNDAwNCwyMTUwLjU1MzYwNSkiPjxwYXRoIGQ9Ik00MTAsMzQ0QzQzNCwzNDEgNDQzLDMzMCA0NTYsMzIwQzQ1OSwzMTcgNDU5LDMxNyA0NjIsMzE0QzQ2NCwzMTIgNDc2LDI5OSA0ODMsMjg2QzUwNCwyNDMgNDk4LDIzOSA1MDEsMjM0QzUwNCwyMjYgNTAyLDIyMyA1MDIsMjIxQzUwNiwyMTIgNTAxLDE5NCA1MDUsMTgzQzUwNywxNzggNDQ3LDEzNyA0NDUsMTMyQzQ0NywxMzAgNDQ3LDEyOSA0NDksMTMxQzQ1NCwxMzMgNDU1LDEzMSA0NTgsMTMyQzQ2OCwxMzcgNTgyLDE1NCA1ODIsMjYzQzU4MiwyNjUgNTgwLDI2NSA1ODAsMjY3QzU3OSwzMjEgNTU0LDMyMSA1NDcsMzA1QzU0MiwyOTYgNTU4LDI2MSA1NDcsMjI4QzUzNiwxOTYgNDk5LDE3NSA1MDgsMTg0QzUxNiwxOTMgNTE1LDE5NCA1MjIsMjA0QzUzNiwyMjUgNTQyLDI2MSA1NDIsMjYzQzU0MiwyODkgNTQxLDI4OCA1NDAsMzA2QzU0MCwzMDkgNTM5LDMwOCA1MzksMzExQzUzOCwzMTYgNTM4LDMxNSA1MzcsMzIxQzUzNiwzMjggNTM1LDMyNyA1MzMsMzM0QzUzMywzMzkgNTI5LDM1MCA1MjgsMzUzQzUyNiwzNjQgNTIxLDM2NSA1MjcsMzc1QzUzNywzODkgNjA1LDQzMiA1NDgsNDYyQzU0Nyw0NjIgNTQ3LDQ2MiA1NDYsNDYyTDQxMCwzNDRaTTU0MCw1MTlDNTM2LDUyNCA1MzEsNTI3IDUzMiw1MzJDNTM1LDU0MyA1MzMsNTYyIDUzMSw1NjRDNTIzLDU4MCA1MjEsNTg1IDUwNCw1OTNDNDk1LDU5OCA0NjMsNTkxIDQ2Myw1OTFDNDQyLDU4MSA0MjcsNTkwIDQyNCw1OTJDNDEyLDU5OSA0MTUsNjAyIDM5MSw2NzVDMzgyLDcwNSAzODMsNzA2IDM4Miw3MDhDMzc5LDcxNSAzODIsNzE3IDM4Myw3MTlDMzgzLDczMiA0MDEsNzQ3IDQwNSw3NTJDNDE3LDc2NCA0MDAsNzczIDM5OSw3NzRDMzkzLDc3NyAyOTcsNzYzIDI5Myw3NjJDMjkwLDc2MiAyNTUsNzA3IDI1NSw3MDVDMjU0LDcwNSAyNTQsNzAyIDI1MSw2OTRDMjI5LDY0MSAyNDcsNTY4IDI0Nyw1MjBDMjQ3LDUwNCAyNDYsNTA1IDI0Miw0ODlDMjQxLDQ4MiAyMjcsNDcyIDIyNSw0NzJDMjEzLDQ2OSAyMTQsNDY4IDIxMiw0NjhDMTg3LDQ2NiAxODIsNDI1IDE4Nyw0MTBDMTk5LDM3MiAyMzQsNDEwIDIzOCw0MTJDMjU4LDQyMyAyNjYsNDE2IDI3Myw0MTNDMjg2LDQwNyAzMTUsMzczIDM1MiwzNTZMNTQwLDUxOVoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIwNjguMjg4MzgzLDIxMTUuNzEwNDc5KSI+PHBhdGggZD0iTTg0Myw0NzBDODQ0LDQ4MyA4NDUsNDgyIDg0NSw0OTRDODQ1LDUyNyA4NDUsNTI3IDg0Miw1NDZDODMzLDU5MCA4MzEsNTg5IDgxMiw2MjlDODEyLDYzMCA3OTksNjQ5IDc5OCw2NTBDNzk4LDY1MSA3NzgsNjc3IDc3MCw2ODJDNzYxLDY4OSA3MzQsNjc3IDc1Niw2NTVDODA1LDYwNiA4NDQsNTAyIDc5MCw0MDFDNzYxLDM0NiA3MzMsMzQ2IDc1NSwzMjdDNzY3LDMxNiA3ODEsMzM4IDc5MiwzNTBDODAxLDM1OSA4MzQsNDE1IDgzNyw0MzhDODM3LDQ0MiA4MzksNDQyIDg0MCw0NTNDODQwLDQ1NCA4NDMsNDYyIDg0Myw0NzBaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMDY4LjI4ODM4MywyMTE1LjcxMDQ3OSkiPjxwYXRoIGQ9Ik03MjYsNDcxQzcyNSw0NjggNzIwLDQ1MiA3MTgsNDUwQzY5Nyw0MDcgNjc0LDQwNiA2OTQsMzg4QzcwMiwzODEgNzE1LDM4OSA3MTgsMzk2QzcyMSw0MDIgNzMyLDQwOSA3NDQsNDM2Qzc2Myw0NzQgNzYwLDUxMyA3NjAsNTEzQzc1OCw1MTkgNzU3LDUzNSA3NTcsNTM4Qzc1Miw1NjIgNzMyLDU5NyA3MjUsNjA0QzcxNSw2MTUgNzE3LDYxOCA3MDQsNjI0QzY5OSw2MjYgNjg4LDYxNyA2ODgsNjEzQzY4NCw1OTMgNzA2LDU5NCA3MjEsNTUzQzcyNSw1NDAgNzI1LDU0MCA3MjgsNTI3QzczMyw0OTggNzI2LDQ3OCA3MjYsNDcxWiIgc3R5bGU9ImZpbGw6d2hpdGU7Ii8+PC9nPjxnIHRyYW5zZm9ybT0ibWF0cml4KDEsMCwwLDEsMjA2OC4yODgzODMsMjExNS43MTA0NzkpIj48cGF0aCBkPSJNNjU1LDUxNkM2NTUsNTE0IDY1Nyw1MDYgNjU0LDQ5MUM2NTAsNDY3IDYxOCw0NTIgNjQ2LDQzN0M2NTUsNDMzIDY3MSw0NTQgNjc1LDQ2MkM3MDgsNTI1IDY1Nyw1ODMgNjQzLDU2OUM2MjAsNTQ4IDY1MSw1NDcgNjU1LDUxNloiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgwLjY1ODc0OCwwLjU3MjE0LC0wLjY1NTczMiwwLjc1NDk5NCwyODczLjU3MjI1NywtMTEyMy4zODAwNykiPjxyZWN0IHg9IjE5NTIiIHk9IjMwMDMiIHdpZHRoPSIxMDk4IiBoZWlnaHQ9IjQ3IiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PC9nPjwvc3ZnPg==)";
        }

        // kept so a save that did not touch the server details can not tear down a live
        // connection. captured before the keys array below is cleared and refilled
        let previous_autoconnect_host = g_autoconnect_details.host;
        let previous_autoconnect_port = g_autoconnect_details.port;
        let previous_autoconnect_keys = (g_autoconnect_details.keys || []).join("\n");
        // gaining or losing the loopback must count as a change, because a webview that
        // connected directly before node announced itself would otherwise never re-route through it
        let previous_loopback = g_loopback_port + ":" + g_loopback_token;

        g_autoconnect_details.host = settings_from_android.host;
        g_autoconnect_details.port = settings_from_android.port;

        // null it out in case it wasnt
        g_autoconnect_details.keys.length = 0;
        g_autoconnect_details.keys = [];

        // metadata_keys is optional, because the json grows incrementally (identity
        // first, app_mode on the first-run answer) and only a native-settings save
        // writes the full form - and a server without keys never gets any
        if (settings_from_android.metadata_keys != null)
        {
            for (i = 0; i < settings_from_android.metadata_keys.length; i++)
            {
                g_autoconnect_details.keys.push(settings_from_android.metadata_keys[i]);
            }
        }

        // the loopback fields only ever appear in the webview's settings json
        g_loopback_port = (typeof settings_from_android.loopback_port === "number") ? settings_from_android.loopback_port : 0;

        g_loopback_token = (typeof settings_from_android.loopback_token === "string") ? settings_from_android.loopback_token : "";

        // no unconditional spinner hold here, because connection__request_connect raises the spinner
        // only when it actually dials; a save with autoconnect off changes nothing on screen

        // the data worker has its own copy of the flag and encrypts outgoing direct
        // messages itself, so it must skip that in loopback mode too
        if (g_data_processing_worker != null)
        {
            g_data_processing_worker.postMessage({ type: "mainthread__set_loopback_mode", value: g_loopback_port });
        }

        console.log("accept_current_settings_from_android autoconnect_details" + JSON.stringify(g_autoconnect_details));

        // a save that changed the server means the live connection is stale
        let are_server_details_changed = (g_autoconnect_details.host != previous_autoconnect_host)
            || (g_autoconnect_details.port != previous_autoconnect_port)
            || ((g_autoconnect_details.keys || []).join("\n") != previous_autoconnect_keys)
            || ((g_loopback_port + ":" + g_loopback_token) != previous_loopback);

        if (are_server_details_changed && g_is_authenticated == true)
        {
            console.log("connect-path: server details changed, dropping the live connection");
            g_websocket_worker.postMessage({ type: "close" });
        }

        // the android default username becomes the chosen username, because the settings
        // arrive before the dial and the server then applies the name at login - the old
        // way was a rename after connect, which an admin can switch off for users
        if (typeof settings_from_android.default_username === "string" && settings_from_android.default_username.length > 0)
        {
            g_chosen_username = settings_from_android.default_username;
        }

        // only node derives its identity from the settings, because the webview does not
        // own the connection. whether to actually dial is decided inside connection__request_connect,
        // and that holds for both runtimes
        if (android_host__is_ui_only_runtime() == false)
        {
            connection__request_identity(settings_from_android.identity_string);
        }

        connection__request_connect("settings");
    }
}

// ---- idle mode ----

/**
 * @brief remembers that a come-from-idle request is waiting for the server's answer, so idle entry is deferred meanwhile
 *        if the answer never comes, the flag clears itself after 20 s so idle is not blocked forever
 *
 * @return void
 */
function android_host__mark_come_from_idle_in_flight()
{
    is_come_from_idle_in_flight = true;

    if (come_from_idle_in_flight_timer != null)
    {
        clearTimeout(come_from_idle_in_flight_timer);
    }

    // if the answer never comes, give up after a while so idle is not blocked forever
    come_from_idle_in_flight_timer = setTimeout(function()
    {
        come_from_idle_in_flight_timer = null;
        is_come_from_idle_in_flight = false;
        console.log("connect-path: come-from-idle was never confirmed, idle allowed again");
        android_host__apply_pending_deep_idle_if_any();
    }, 20000);
}

/**
 * @brief the server answered the come-from-idle: idle is allowed again, and a deferred idle entry runs now
 *
 * @return void
 */
function android_host__clear_come_from_idle_in_flight()
{
    if (come_from_idle_in_flight_timer != null)
    {
        clearTimeout(come_from_idle_in_flight_timer);
        come_from_idle_in_flight_timer = null;
    }

    is_come_from_idle_in_flight = false;

    android_host__apply_pending_deep_idle_if_any();
}

/**
 * @brief runs an idle entry that android_host__enter_deep_idle deferred while a come-from-idle was unconfirmed
 *        android_host__exit_deep_idle cancels the pending flag, so a foregrounded app never re-enters idle here
 *
 * @return void
 */
function android_host__apply_pending_deep_idle_if_any()
{
    if (g_is_deep_idle_pending == true && g_is_deep_idle == false)
    {
        g_is_deep_idle_pending = false;
        console.log("connect-path: running deferred idle entry");
        android_host__enter_deep_idle();
    }
}

/**
 * @brief holds presence for a moment after a call accept
 *        accepting a call blinks the screen off or hands the call screen over to the activity, and
 *        both read as "nobody is looking" exactly while the user IS there, which idled people right
 *        back out of the call they just answered
 *
 * @return void
 */
function android_host__mark_call_accept_presence_grace()
{
    presence_grace_until_timestamp = new Date().valueOf() + CALL_ACCEPT_PRESENCE_GRACE_MS;
    console.log("connect-path: call accepted, holding presence for " + CALL_ACCEPT_PRESENCE_GRACE_MS + "ms");
}

/**
 * @brief one shot at the end of the presence grace: node re-reads whether a ui is attached, the webview retries the gentle idle entry
 *        when the user actually came back, android_host__exit_deep_idle cancelled this and nothing runs
 *
 * @return void
 */
function android_host__schedule_presence_grace_recheck()
{
    if (presence_grace_recheck_timer != null)
    {
        return;
    }

    let wait_ms = Math.max(250, (presence_grace_until_timestamp - new Date().valueOf()) + 250);

    presence_grace_recheck_timer = setTimeout(function()
    {
        presence_grace_recheck_timer = null;

        if (typeof process !== "undefined")
        {
            android_host__node_apply_idle_for_ui_state();
        }
        else
        {
            android_host__enter_deep_idle(false);
        }
    }, wait_ms);
}

/**
 * @brief drops the queued presence-grace re-check
 *
 * @return void
 */
function android_host__cancel_presence_grace_recheck()
{
    if (presence_grace_recheck_timer != null)
    {
        clearTimeout(presence_grace_recheck_timer);
        presence_grace_recheck_timer = null;
    }
}

/**
 * @brief deep idle, entered when the android app goes to background: keeps the websocket alive on a slow heartbeat and shuts down everything else (audio graph, opus decoder tick, webrtc datachannel)
 *        every refusal is logged, because a silent one is indistinguishable from the request never
 *        arriving; an unconfirmed come-from-idle defers the entry instead of dropping it
 *
 * @param boolean is_forced -> true idles from any channel, false keeps an in-channel session alive
 *
 * @return void
 */
function android_host__enter_deep_idle(is_forced)
{
    // every refusal below says so. a silent one is indistinguishable from the request
    // never arriving, which is what made this so hard to pin down
    if (g_is_deep_idle == true)
    {
        console.log("connect-path: idle requested, already idle");
        return;
    }

    // an unconfirmed come-from-idle blocks idle entry, but the request must not be thrown
    // away: dropping it left the page awake while the server considered the client idle,
    // and every datachannel handshake attempted from that split state was doomed
    if (is_come_from_idle_in_flight == true)
    {
        console.log("connect-path: idle deferred, a come-from-idle is still unconfirmed");
        g_is_deep_idle_pending = true;
        return;
    }

    // app backgrounded before authentication finished - remember it, the auth success handler
    // re-enters (fixes the "went to background while still connecting" race)
    if (g_is_authenticated == false)
    {
        console.log("connect-path: idle requested while not authenticated, deferred to login");
        g_is_deep_idle_pending = true;
        return;
    }

    // inside the call-accept presence grace: hold, then re-decide from the real state.
    // a swipe-away (is_forced) stays deliberate and is honoured immediately
    if (is_forced != true && new Date().valueOf() < presence_grace_until_timestamp)
    {
        console.log("connect-path: idle deferred, inside the call-accept presence grace");
        android_host__schedule_presence_grace_recheck();
        return;
    }

    // backgrounding (home) keeps an in-channel session alive - music and calls continue.
    // only a swipe-away forces idle from any channel (is_forced, from onTaskRemoved)
    if (is_forced != true)
    {
        let local_client = channel_tree__get_client_by_client_id(g_local_client_id);

        if (local_client != null && local_client.channel_id != 0)
        {
            console.log("connect-path: idle refused, in channel " + local_client.channel_id + " not root");
            return;
        }
    }

    console.log("connect-path: going idle" + (is_forced == true ? " (forced)" : ""));

    g_is_deep_idle = true;

    client_msg.send_go_to_idle_mode_request();

    // heartbeat 10s -> 120s, loss detector 120s -> 360s (three missed checks)
    g_connection_check.interval_ms = 120 * 1000;
    g_connection_check.lost_threshold_ms = 360 * 1000;

    if (g_audio_context != null && g_audio_context.state === "running")
    {
        audio_suspend_promise = g_audio_context.suspend();
    }

    // a push-to-talk held down at background time must not keep capturing all idle long;
    // capture stays off until the next press. guarded: node has no audio at all, and
    // an unguarded call threw out of here, so headless node never actually went idle
    if (typeof audio__set_microphone_capture_active === "function")
    {
        audio__set_microphone_capture_active(false);
    }

    if (typeof voice__set_mic_transmitting_visual === "function")
    {
        voice__set_mic_transmitting_visual(false);
    }

    if (g_opus_decoder_worker != null)
    {
        g_opus_decoder_worker.postMessage({ type: "deep_idle_stop" });
    }

    // the datachannel's ICE consent checks ping the server every few seconds even when nothing
    // streams - close it, android_host__exit_deep_idle re-establishes it
    if (g_peer_connection_with_server != null)
    {
        try
        {
            g_peer_connection_with_server.close();
        }
        catch (e)
        {
            console.log("deep idle: peer connection close failed ", e.message);
        }
        g_peer_connection_with_server = null;
        g_datachannel = null;
        g_is_webrtc_datachannel_connected = false;
    }
}

/**
 * @brief leaves deep idle: restores the heartbeat cadence, the audio graph, the opus tick and the webrtc datachannel
 *
 * @return void
 */
function android_host__exit_deep_idle()
{
    // the user is demonstrably here: a queued presence-grace re-check must not fire behind them.
    // before the was-not-idle return, because the webview arms that timer without ever being idle
    android_host__cancel_presence_grace_recheck();

    g_is_deep_idle_pending = false;

    if (g_is_deep_idle == false)
    {
        console.log("connect-path: leave-idle requested, was not idle");
        return;
    }

    // we really were idle, so an answer from the server is coming. armed here rather
    // than where the request is sent, because a request sent while not idle gets none
    android_host__mark_come_from_idle_in_flight();

    console.log("connect-path: leaving idle");

    g_is_deep_idle = false;

    g_connection_check.interval_ms = 10 * 1000;
    g_connection_check.lost_threshold_ms = 35 * 1000; // keep in sync with the startup value
    g_connection_check.last_response_timestamp = new Date().valueOf();

    // wake the heartbeat loop out of its long idle sleep so a fresh check goes out now
    if (g_connection_check.sleep_resolve != null)
    {
        g_connection_check.sleep_resolve();
        g_connection_check.sleep_resolve = null;
    }

    if (g_audio_context != null)
    {
        // wait for idle entry's suspend to settle before acting, so the two can never race
        let suspend_settled = (audio_suspend_promise != null) ? audio_suspend_promise : Promise.resolve();
        audio_suspend_promise = null;

        suspend_settled.catch(function() {}).then(function()
        {
            // android resets the audio HAL under a backgrounded webview; resume() then
            // "succeeds" into a broken graph (gurgling both ways), so rebuild instead
            if (g_android_app_mode != "")
            {
                audio__rebuild_audio_graph_after_idle();
                return;
            }

            // browsers: resume, but verify it took - rebuild when the context stays stuck
            g_audio_context.resume().catch(function(e) { console.log("audio resume rejected: " + e.message); });
            setTimeout(function()
            {
                if (g_audio_context.state !== "running")
                {
                    console.log("audio context stuck in '" + g_audio_context.state + "' after resume, rebuilding");
                    audio__rebuild_audio_graph_after_idle();
                }
            }, 1000);
        });
    }

    if (g_opus_decoder_worker != null)
    {
        g_opus_decoder_worker.postMessage({ type: "deep_idle_resume" });
    }

    // re-establish the webrtc datachannel that idle entry closed
    if (g_is_voice_chat_allowed_by_server == true && g_is_webrtc_datachannel_connected == false && g_is_webrtc_datachannel_check_running == false)
    {
        g_is_webrtc_datachannel_check_running = true;
        voice__webrtc_datachannel_connection_check(true);
    }
}

// connection.js is embedded in template.html along with the other client files, and in the node bundle
// it is the connecting side: the driver that decides when to dial (who am i, which identity, where to,
// one attempt in flight), the dial itself, the heartbeat loop, fast reconnect, the connect-page hold
// and the saved server bookmarks
// ui.js and android-host.js nudge the driver through connection__request_connect; dispatch.js reports what the socket did

// state private to this file
var port = 0;

var FAST_RECONNECT_DEADLINE_MS = 12000;

var VERIFICATION_MESSAGE = "welcome";

// where to connect, as { kind: "loopback", port, token } or { kind: "server", host, port, wss_port }
var target_slot = connection__make_slot();

// the identity slot is declared in globals.js (dispatch.js fills it); the slot itself is built here, where its type lives
g_identity_slot = connection__make_slot();

// one keygen per passphrase, because a repeat request for the same identity must be a no-op.
// the size is part of the key: the same passphrase at a new size is a different identity,
// so it has to be part of what makes a request a repeat
var identity_requested_for = null;
var identity_requested_bits = 0;

// resolved by the websocket close and error handlers. the driver awaits this while a
// socket exists, whether it is mid-handshake or long connected
var connection_closed_resolvers = [];

// skips the retry countdown, used when the connect button is pressed or new details arrive.
// a nudge that lands while the driver is still watching the old socket is kept, not lost:
// an identity switch closes the socket and nudges in the same breath, before the close is seen
var driver_nudge_resolver = null;
var is_driver_nudge_pending = false;

var is_connection_driver_running = false;

// webview only: true when connect was pressed before node announced its loopback port
var ui_connect_requested = false;

// ---- connection driver ----

/**
 * @brief a slot holds one async value: set() stores it and wakes everyone who called wait()
 *
 * @return object the slot, with set(new_value) and wait(), which returns a promise of the value
 */
function connection__make_slot()
{
    let slot = {
        value: null,
        is_set: false,
        waiters: []
    };

    slot.set = function(new_value)
    {
        slot.value = new_value;
        slot.is_set = true;

        let woken = slot.waiters;
        slot.waiters = [];

        for (let i = 0; i < woken.length; i++)
        {
            woken[i](new_value);
        }
    };

    slot.wait = function()
    {
        if (slot.is_set)
        {
            return Promise.resolve(slot.value);
        }

        return new Promise(function(resolve) { slot.waiters.push(resolve); });
    };

    return slot;
}

/**
 * @brief asks the data-processing worker for the identity keypair: derived from the passphrase when one is given (199 characters or more), random otherwise
 *        the same request twice is ignored; without a worker yet the request is dropped, and the
 *        next settings push retries it
 *
 * @param string|null passphrase_or_null -> the passphrase, null or anything short for a random key
 *
 * @return void
 */
function connection__request_identity(passphrase_or_null)
{
    let requested_key = (typeof passphrase_or_null === "string" && passphrase_or_null.length >= 199)
        ? passphrase_or_null : "(random)";

    if (identity_requested_for === requested_key && identity_requested_bits === g_rsa_key_bits)
    {
        return;
    }

    // settings can arrive before the workers exist; the next settings push retries this
    if (g_data_processing_worker == null)
    {
        console.log("connect-path: no worker yet, keypair request dropped");
        return;
    }

    identity_requested_for = requested_key;
    identity_requested_bits = g_rsa_key_bits;
    g_identity_slot.is_set = false;
    g_is_rsa_key_generated = false;

    console.log("connect-path: keypair requested (" + (requested_key === "(random)" ? "random" : "from passphrase") + ", " + g_rsa_key_bits + " bits)");

    g_data_processing_worker.postMessage({
        type: "mainthread__generate_rsa_keypair",
        from_identity_string: requested_key !== "(random)",
        identity_passphrase_string: requested_key === "(random)" ? null : requested_key,
        rsa_key_bits: g_rsa_key_bits
    });

    // the derive can take minutes on a phone, so the status says what the wait actually is
    connection__report_connection_status("connecting", "generating the identity key (takes a while on first start)");
}

/**
 * @brief wakes everyone awaiting connection__connection_closed(): the socket is gone
 *
 * @return void
 */
function connection__signal_connection_closed()
{
    let woken = connection_closed_resolvers;
    connection_closed_resolvers = [];

    for (let i = 0; i < woken.length; i++)
    {
        woken[i]();
    }
}

/**
 * @brief a promise for the next connection close; the driver awaits it between attempts
 *
 * @return promise resolves when the connection closes
 */
function connection__connection_closed()
{
    return new Promise(function(resolve) { connection_closed_resolvers.push(resolve); });
}

/**
 * @brief wakes the driver out of its retry wait, or remembers the nudge for the wait that has not started yet
 *
 * @return void
 */
function connection__nudge_connection_driver()
{
    if (driver_nudge_resolver != null)
    {
        let resolver = driver_nudge_resolver;
        driver_nudge_resolver = null;
        resolver();
        return;
    }

    is_driver_nudge_pending = true;
}

/**
 * @brief the countdown between retries: shows the failure text every second and leaves early on a nudge
 *
 * @param number seconds -> how long to wait
 *
 * @return promise resolves when the countdown ended or a nudge cut it short
 */
async function connection__driver_retry_wait(seconds)
{
    if (is_driver_nudge_pending == true)
    {
        is_driver_nudge_pending = false;
        console.log("connect-path: retry wait skipped by a pending nudge");
        return;
    }

    let nudged = new Promise(function(resolve) { driver_nudge_resolver = resolve; });

    for (let remaining = seconds; remaining > 0; remaining--)
    {
        if (g_last_connect_attempt_failed == true)
        {
            let status_element = document.getElementById("another-buttons-loading-container-p");

            if (status_element != null)
            {
                status_element.innerHTML = "connection failed<br>retrying in " + remaining + "s";
            }
        }

        let done = await Promise.race([utils__sleep(1000).then(function() { return false; }), nudged.then(function() { return true; })]);

        if (done == true)
        {
            console.log("connect-path: retry wait skipped by nudge");
            break;
        }
    }

    driver_nudge_resolver = null;
}

/**
 * @brief the connection driver, the one loop that dials
 *        waits for a target and an identity, attempts once, awaits the close, then waits out a
 *        countdown and tries again; runs once per page and never returns
 *
 * @return promise never resolves
 */
async function connection__connection_driver()
{
    if (is_connection_driver_running == true)
    {
        return;
    }

    is_connection_driver_running = true;
    console.log("connect-path: driver started");

    while (true)
    {
        let target = await target_slot.wait();

        // the node host parks the connection during the idle handover
        if (g_node_connection_wanted == false)
        {
            await utils__sleep(1000);
            continue;
        }

        let identity = null;

        if (target.kind === "server")
        {
            console.log("connect-path: target " + target.host + ":" + target.port + ", waiting for keypair");
            identity = await g_identity_slot.wait();
        }
        else
        {
            console.log("connect-path: target loopback :" + target.port);
        }

        let closed = connection__connection_closed();

        g_last_disconnect_reason = "";
        connection__report_connection_status("connecting");

        // a dial consumes any nudge; one kept from before must not skip a later countdown
        is_driver_nudge_pending = false;
        await connection__attempt_connection(target, identity);

        // wait for the socket to die. a dial that could not even create a socket never
        // fires close, so an unauthenticated wait also has a deadline
        while (true)
        {
            let outcome = await Promise.race([
                closed.then(function() { return "closed"; }),
                utils__sleep(45000).then(function() { return "deadline"; })
            ]);

            if (outcome === "closed")
            {
                break;
            }

            if (g_is_authenticated == false)
            {
                // the loopback socket is fine, node just has not logged in yet
                // redialing now would kill a healthy attach, so keep waiting
                if (target.kind === "loopback")
                {
                    console.log("connect-path: loopback attached, still waiting for node's login");
                    continue;
                }

                console.log("connect-path: no login within 45s, treating the attempt as failed");
                break;
            }
        }

        // a fast reconnect re-dials at once with the same identity: no countdown, no button
        if (g_is_authenticated == false && g_fast_reconnect.in_progress == true)
        {
            continue;
        }

        if (g_is_authenticated == false && g_is_autoconnect_without_user_action_active == false)
        {
            // manual mode means one attempt per button press: state the failure and
            // hold until the connect button nudges, with no countdown
            if (is_driver_nudge_pending == true)
            {
                is_driver_nudge_pending = false;
                console.log("connect-path: dialing on the pending nudge");
                continue;
            }

            connection__set_connect_button_pending(false);
            connection__report_connection_status("idle",
                (g_last_disconnect_reason !== "") ? g_last_disconnect_reason : "the connection attempt did not complete");
            await new Promise(function(resolve) { driver_nudge_resolver = resolve; });
            continue;
        }

        // dialing node on this device is free, so the loopback redials fast; the 30s
        // pace is for a remote server that may be down
        let retry_seconds = (target.kind === "loopback") ? 2 : 30;

        if (g_is_authenticated == false)
        {
            g_connection_status.next_retry_at = new Date().valueOf() + retry_seconds * 1000;
            connection__report_connection_status("waiting_retry",
                (g_last_disconnect_reason !== "") ? g_last_disconnect_reason : "the connection attempt did not complete");
        }

        await connection__driver_retry_wait(retry_seconds);
    }
}

/**
 * @brief the one place that may start a connection, and the whole dial policy
 *        "button" and "attach" always dial, "settings", "resume" and "retry" only under
 *        autoconnect, and "resume" also reattaches when node already holds a session
 *
 * @param string trigger -> "button", "attach", "settings", "resume" or "retry"
 *
 * @return void
 */
function connection__request_connect(trigger)
{
    let is_wanted =
        (trigger === "button")
        || (trigger === "attach")
        || (g_is_autoconnect_without_user_action_active == true)
        || (ui_connect_requested == true)
        || (trigger === "resume" && g_connection_status.state === "connected");

    console.log("connect-path: connection__request_connect(" + trigger + ") -> " + (is_wanted ? "dial" : "ignored"));

    if (is_wanted == false)
    {
        return;
    }

    // derive the target from where we run and what the page knows
    if (android_host__is_ui_only_runtime())
    {
        if (g_loopback_port <= 0)
        {
            // node has not announced its port yet; the settings push that follows finishes this
            if (trigger === "button")
            {
                ui_connect_requested = true;
                document.getElementById("another-buttons-loading-container-p").innerHTML = "waiting for app runtime...";
            }
            return;
        }

        ui_connect_requested = false;
        target_slot.set({ kind: "loopback", port: g_loopback_port, token: g_loopback_token });

        // a button press keeps the page exactly as it is, only the button itself
        // fades; every other trigger connects behind the spinner page
        if (trigger !== "button")
        {
            connection__hold_back_connect_page();
        }
    }
    else if (g_are_server_details_predefined == true)
    {
        // a partial first-run json has no host yet, and that must never become a target
        if (typeof g_autoconnect_details.host !== "string" || g_autoconnect_details.host.length == 0
            || (parseInt(g_autoconnect_details.port) > 0) == false)
        {
            console.log("connect-path: no server details yet, waiting");
            return;
        }

        target_slot.set({
            kind: "server",
            host: g_autoconnect_details.host,
            port: g_autoconnect_details.port,
            wss_port: g_autoconnect_details.wss_port
        });
    }
    else
    {
        target_slot.set({
            kind: "server",
            host: document.getElementById("input-ip-address").value,
            port: document.getElementById("input-port-number").value
        });
    }

    connection__nudge_connection_driver();
}

/**
 * @brief the connect button; it keeps its own name for the onclick wiring
 *
 * @return void
 */
function connection__submit_connection_target_from_ui()
{
    connection__request_connect("button");
}

/**
 * @brief turns a node socket error code into something a person can act on
 *        the codes are the difference between your wifi being off and the server rejecting you;
 *        without them an unreachable network used to be reported as a login rejection
 *
 * @param string error_code -> the node error code, "ECONNREFUSED" and the like
 *
 * @return string the text for the connect page
 */
function connection__describe_socket_error(error_code)
{
    if (g_device_has_network === false)
    {
        return "no network connection (wifi and mobile data are off)";
    }

    if (error_code === "ENETUNREACH" || error_code === "EHOSTUNREACH"
        || error_code === "EAI_AGAIN" || error_code === "ENOTFOUND")
    {
        return "no route to the server (network down, or the address is unreachable)";
    }

    if (error_code === "ECONNREFUSED")
    {
        return "the server refused the connection (is it running on that port?)";
    }

    if (error_code === "ETIMEDOUT")
    {
        return "the server did not answer in time (wrong address, or a firewall)";
    }

    if (typeof navigator !== "undefined" && navigator.onLine === false)
    {
        return "no network connection (wifi off?)";
    }

    return "could not reach the server (wrong address, server down, or no network)";
}

/**
 * @brief records a connection status change in g_connection_status, tells the listeners and repaints the connect page
 *
 * @param string state -> the new state
 * @param string reason -> the text that explains it, "" for none
 *
 * @return void
 */
function connection__report_connection_status(state, reason)
{
    g_connection_status.state = state;
    g_connection_status.reason = (reason != null) ? reason : "";

    for (let i = 0; i < g_connection_status_listeners.length; i++)
    {
        try { g_connection_status_listeners[i](g_connection_status); } catch (e) { }
    }

    connection__render_connection_status();
}

/**
 * @brief paints the login page extras from g_connection_status
 *        the ticker recalls it every second so the countdown and the "ago" times stay live; safe
 *        headless, because the shim absorbs the dom
 *
 * @return void
 */
function connection__render_connection_status()
{
    let reason_element = document.getElementById("connection-status-reason");
    let countdown_element = document.getElementById("connection-status-countdown");
    let lastseen_element = document.getElementById("connection-status-lastseen");

    if (reason_element == null || countdown_element == null || lastseen_element == null)
    {
        return;
    }

    let reason_text = g_connection_status.reason;

    // the page knows the device's connectivity; node cannot see it
    if (typeof navigator !== "undefined" && navigator.onLine === false)
    {
        reason_text = "no network connection (wifi off?)";
    }

    reason_element.textContent = (g_connection_status.state === "connected") ? "" : reason_text;

    let countdown_text = "";

    // only the retry countdown belongs here, because the loading line above already
    // says "connecting to <host>" and printing it twice read as two separate states
    if (g_connection_status.state === "waiting_retry" && g_connection_status.next_retry_at > 0)
    {
        let seconds_left = Math.max(0, Math.ceil((g_connection_status.next_retry_at - new Date().valueOf()) / 1000));
        countdown_text = "next attempt in " + seconds_left + "s";
    }

    countdown_element.textContent = countdown_text;

    let lastseen_text = "";

    if (g_connection_status.state !== "connected" && g_connection_status.last_connected_at > 0)
    {
        let minutes_ago = Math.round((new Date().valueOf() - g_connection_status.last_connected_at) / 60000);
        lastseen_text = (minutes_ago < 1) ? "last connected under a minute ago"
            : "last connected " + minutes_ago + " min ago";
    }

    lastseen_element.textContent = lastseen_text;
}

/**
 * @brief one repaint per second keeps the countdown and the "ago" times moving
 *
 * @return void
 */
function connection__start_connection_status_ticker()
{
    window.setInterval(connection__render_connection_status, 1000);
}

// ---- dial and heartbeat ----

/**
 * @brief the heartbeat loop's sleep; android_host__exit_deep_idle resolves it early so the first check after idle goes out at once
 *
 * @param number ms -> the sleep in milliseconds
 *
 * @return promise resolves after the sleep or on the early wake
 */
function connection__connection_check_sleep(ms)
{
    return new Promise(function(resolve)
    {
        let timer_id = setTimeout(function()
        {
            g_connection_check.sleep_resolve = null;
            resolve();
        }, ms);

        // early-wake hook used by android_host__exit_deep_idle: cancel this sleep's timer so a stale
        // (long) timeout can never fire later and clobber a newer sleep's resolver
        g_connection_check.sleep_resolve = function()
        {
            clearTimeout(timer_id);
            g_connection_check.sleep_resolve = null;
            resolve();
        };
    });
}

/**
 * @brief the heartbeat loop: sends client_connection_check on an interval, and when no reply arrives within the lost threshold, alerts the user and resets the app (identity kept)
 *        one throw in here must not kill the loop, since it carries both the heartbeat and the loss detector
 *
 * @return promise resolves when the loop is told to stop
 */
async function connection__websocket_connection_check()
{
    while (g_should_connection_check_be_running == true)
    {
        // one throw in here must not kill the loop: it carries BOTH the heartbeat and
        // the loss detector, and a dead loop is an unnoticed dead connection
        try
        {
            let timestamp_now = new Date().valueOf();

            if (g_connection_check.last_response_timestamp == 0)
            {
                g_connection_check.last_response_timestamp = timestamp_now;
            }

            if ((g_connection_check.last_response_timestamp + g_connection_check.lost_threshold_ms) < timestamp_now)
            {
                let silence_ms = timestamp_now - g_connection_check.last_response_timestamp;
                console.error("loss detector: no heartbeat response for " + silence_ms + "ms");

                // the resume path ends this loop; the login that follows starts a fresh one
                if (connection__try_start_fast_reconnect("no heartbeat reply for " + silence_ms + " ms") == true)
                {
                    break;
                }

                utils__custom_alert("connection with server lost");
                console.error("loss detector: resetting");
                connection__reset_chat_app_keep_identity();
            }

            g_session.ping_sent_at = timestamp_now; // the response handler turns this into the round-trip time

            let message_object = {
                message: {
                    type: "client_connection_check",
                }
            };

            connection__send_message_object(message_object);
        }
        catch (checker_error)
        {
            console.error("connection check iteration failed: " + (checker_error != null && checker_error.stack ? checker_error.stack : checker_error));
        }

        await connection__connection_check_sleep(g_connection_check.interval_ms);
    }
}

/**
 * @brief fast reconnect: the socket is gone but the page keeps everything (lists, chat, channel, keys) and the driver re-dials at once with the same identity, asking the server to adopt the session
 *
 * @param string reason -> why the socket went, for the log
 *
 * @return boolean false when a resume is not possible, so the caller runs the classic wipe instead
 */
function connection__try_start_fast_reconnect(reason)
{
    // node owns the real connection in the webview, and only a logged-in session can be resumed
    if (g_server_policy.is_fast_reconnect_allowed == false || g_is_authenticated == false
        || g_is_identity_switch_in_progress == true || android_host__is_ui_only_runtime() == true)
    {
        return false;
    }

    // one attempt per loss: when the resume socket itself dies the caller takes the classic path
    if (g_fast_reconnect.in_progress == true)
    {
        return false;
    }

    g_fast_reconnect.in_progress = true;
    g_fast_reconnect.resumed = false;
    g_fast_reconnect.pending_lists = null;

    // the driver keys off this flag; the page itself is left exactly as it is
    g_is_authenticated = false;
    g_should_connection_check_be_running = false;
    g_connection_check.last_response_timestamp = 0;
    channel_tree__stop_avatar_prefetch();

    // the worker goes back to handshake mode; it keeps the identity and the channel keys
    g_data_processing_worker.postMessage({ type: "mainthread_reset_data" });

    console.log("%cfast reconnect: " + reason + " - re-dialing with the same identity", "color: #ffa500; font-weight: bold; font-size: 14px;");
    utils__custom_log("fast reconnect attempt (" + reason + ")");

    g_fast_reconnect.deadline_timer = setTimeout(function()
    {
        connection__fast_reconnect_failed("no adopted session within " + (FAST_RECONNECT_DEADLINE_MS / 1000) + " s");
    }, FAST_RECONNECT_DEADLINE_MS);

    // wakes the driver out of its wait on the old socket; it re-dials instead of parking
    connection__signal_connection_closed();
    return true;
}

/**
 * @brief the one failure of a fast reconnect: the classic "connection lost" toast and the full wipe
 *
 * @param string reason -> why it failed, for the log
 *
 * @return void
 */
function connection__fast_reconnect_failed(reason)
{
    if (g_fast_reconnect.in_progress == false && g_fast_reconnect.resumed == false)
    {
        return;
    }

    g_fast_reconnect.in_progress = false;
    g_fast_reconnect.resumed = false;
    g_fast_reconnect.pending_lists = null;
    clearTimeout(g_fast_reconnect.deadline_timer);
    g_fast_reconnect.deadline_timer = null;

    console.log("%cfast reconnect failed: " + reason + " - back to the connect screen", "color: #ff4040; font-weight: bold; font-size: 14px;");
    utils__custom_log("fast reconnect failed (" + reason + ")");

    if (g_is_running_in_android_webview == false)
    {
        utils__custom_alert("connection with server lost");
    }

    g_is_authenticated = false;
    connection__reset_chat_app_keep_identity();
}

/**
 * @brief the server adopted the session: everything on this page is still valid
 *        the complete lists follow as on a login and are held back until the last one, see
 *        connection__fast_reconnect_buffer_list
 *
 * @return void
 */
function connection__fast_reconnect_succeeded()
{
    g_fast_reconnect.resumed = true;
    g_fast_reconnect.pending_lists = {};
    console.log("fast reconnect: the server adopted the session, waiting for the refreshed lists");
}

/**
 * @brief holds back one of the four lists (channel, client, icon, tag) a resume refreshes, so they are applied together in one repaint and nothing on screen flickers
 *
 * @param string kind -> "channel", "client", "icon" or "tag"
 * @param object value -> the list message
 *
 * @return boolean true when the list was buffered, false when no resume is in progress
 */
function connection__fast_reconnect_buffer_list(kind, value)
{
    if (g_fast_reconnect.resumed == false)
    {
        return false;
    }

    if (g_fast_reconnect.pending_lists == null)
    {
        g_fast_reconnect.pending_lists = {};
    }

    g_fast_reconnect.pending_lists[kind] = value;

    if (g_fast_reconnect.pending_lists.channel != null && g_fast_reconnect.pending_lists.client != null
        && g_fast_reconnect.pending_lists.icon != null && g_fast_reconnect.pending_lists.tag != null)
    {
        connection__fast_reconnect_apply_lists();
    }

    return true;
}

/**
 * @brief applies the four refreshed lists in one task: the old lists go in the same task the new ones are built, one repaint, already complete
 *
 * @return void
 */
function connection__fast_reconnect_apply_lists()
{
    let lists = g_fast_reconnect.pending_lists;

    g_fast_reconnect.pending_lists = null;
    g_fast_reconnect.resumed = false;
    clearTimeout(g_fast_reconnect.deadline_timer);
    g_fast_reconnect.deadline_timer = null;

    // the old lists go in the same task the new ones are built: one repaint, already complete
    g_map_client_id_to_array_index.clear();
    g_client_list.length = 0;
    g_channel_list.length = 0;
    g_tags.length = 0;
    g_icons.length = 0;
    g_is_client_list_retrieved = false;
    g_is_channel_list_retrieved = false;

    let elements_count = document.getElementsByClassName("connected-client").length;
    for (let i = 0; i < elements_count; i++)
    {
        document.getElementsByClassName("connected-client")[0].remove();
    }

    server_msg.process_channel_list_from_server(lists.channel);
    server_msg.process_client_list_from_server(lists.client);
    server_msg.process_icon_list_from_server(lists.icon);
    server_msg.process_tag_list_from_server(lists.tag);

    console.log("%cFAST RECONNECT HAPPENED - session resumed, lists refreshed, nothing was lost", "color: #00e000; font-weight: bold; font-size: 20px;");
    utils__custom_log("fast reconnect happened");
}

/**
 * @brief tears the session down to the connect screen while keeping the rsa identity
 *        closes the socket, clears the client and channel DOM and resets nearly all
 *        connection, chat and voice state
 *
 * @return void
 */
function connection__reset_chat_app_keep_identity()
{
    // no connection, nobody is typing - drop the notices and the ticker with them
    g_typing.state = {};
    chat__render_typing_indicator();

    // whatever triggered the reset, the socket must really be gone: a reconnect stalls for as long
    // as an old authenticated socket lives on. closing a closed socket is a no-op, and the onclose
    // it fires re-enters this reset harmlessly (g_is_authenticated is already false)
    g_websocket_worker.postMessage({ type: "close" });

    UI.clear_chat_button_onclick();
    UI.clear_chat_button_onclick();

    // the list goes with the map. they used to be emptied 55 lines apart, and a frame
    // arriving in between found an empty map next to a full list
    g_map_client_id_to_array_index.clear();
    g_client_list.length = 0;

    // the session is over, so the stored login frame is a lie. the loopback replays it
    // to any ui that attaches, which showed "connected" and played the sound on every
    // reconnect attempt against a server that was not there
    g_node_cached_auth_frame = null;

    document.getElementById("another-buttons-sub-container").style.display = "";
    document.getElementById("another-buttons-sub-loading-container").style.display = "none";
    document.getElementById("another-buttons-loading-container-p").innerHTML = "loading...";
    connection__set_connect_button_pending(false);
    g_is_microphone_available = false;

    document.getElementById('verification-system').style.display = "block";
    document.getElementById('communication-system-container').style.display = "none";

    // after the page switch, otherwise the mic is repainted while the old screen is still up
    voice__update_microphone_button();

    // back on the connect screen: the in-flow "server settings" button owns this
    // state, a second gear in the bar would only confuse - it returns on connect
    if (g_is_running_in_android_webview)
    {
        document.getElementById("android-settings-button").style.display = "none";
    }

    let elements_count = document.getElementsByClassName("connected-client").length;
    for (let i = 0; i < elements_count; i++)
    {
        document.getElementsByClassName("connected-client")[0].remove();
    }

    g_data_processing_worker.postMessage({
        type: "mainthread_reset_data"
    });

    g_base64_picture_string_to_send = "" ;
    g_is_websocket_connected = false;
    g_local_username = "";
    g_local_client_id = 0;
    g_alert_push_to_talk_key_shown_once = false;
    g_alert_streaming_music_shown_once = false;
    g_stop_song_stream_message_received = false;
    g_selected_server_chat_message_id = null;
    g_is_authenticated = false;
    g_should_connection_check_be_running = false;

    // the avatar prefetch timer must not outlive the session - its client ids are meaningless
    // after a reconnect (they are slot indices), and it would keep asking a dead socket
    channel_tree__stop_avatar_prefetch();

    // if the connection died while backgrounded-idle, re-enter idle automatically after reconnect
    g_is_deep_idle_pending = (g_is_deep_idle == true) || g_is_deep_idle_pending;
    g_is_deep_idle = false;
    g_connection_check.interval_ms = 10 * 1000;
    g_connection_check.lost_threshold_ms = 35 * 1000; // keep in sync with the startup value
    g_current_channel_id = 0;
    g_current_chat_context_id = "chat-context-channel-0";
    g_chat_message_receiver_type = "channel";
    g_chat_message_receiver_id = "main";
    g_is_client_list_retrieved = false;
    g_is_channel_list_retrieved = false;
    g_is_webrtc_datachannel_connected = false;

    // emptied in place, not reassigned: a fresh array would leave anything still
    // holding the old one reading stale clients. the client list is already cleared above
    g_icons.length = 0;
    g_tags.length = 0;
    g_channel_list.length = 0;

    g_offline_client_list = [];

    g_is_chat_hidden = false;
    layout__layout_apply(); // re-sync the grid (panel visibility) with the reset flag; no-op on touch
    g_local_message_id = 0;
    g_selected_font = "custom-font-usage-default";
    g_selected_font_color = "#ffffff";
    g_selected_font_size = 12;
    g_is_microphone_enabled_on_touch_device = false;  // for touch devices

    g_connection_check.last_response_timestamp = 0;

    g_selected_channel_id = null;
    g_selected_client_id = null;
    g_current_channel_keys = null;
    g_chat_context_array = [
        {
            type: "channel",
            chat_context_id: "chat-context-channel-0",
            last_known_message_sender_username: ""
        }
    ];

    // for voice chat

    g_peer_connection_with_server = null;

    g_iceconfig = null;

    g_is_voice_chat_allowed_by_server = false;
    g_is_client_microphone_allowed_by_server = false;
    g_is_microphone_enabled = false;
    g_is_microphone_active = false;
    g_last_sent_value_microphone_usage = false;

    if (g_local_audio_stream != null)
    {
        if (g_local_audio_stream.getTracks() != null)
        {
            g_local_audio_stream.getTracks()[0].enabled = false;
            // g_local_audio_stream = null; careful
        }
    }
}

/**
 * @brief the spinner that covers the held-back connect page, so the user never stares at a blank screen
 *        opaque, in the theme's color when one paints body/html; a dark fallback keeps page one solid otherwise
 *
 * @param boolean is_visible -> true to show it
 *
 * @return void
 */
function connection__set_connect_holdback_loader_visible(is_visible)
{
    let loader = document.getElementById("connect-holdback-loader");

    if (loader != null)
    {
        // opaque, in the theme's color when one paints body/html; some themes (bluebell)
        // paint only inner containers, then a dark fallback keeps page one solid
        if (is_visible == true && typeof getComputedStyle === "function")
        {
            let page_background = getComputedStyle(document.body).backgroundColor;

            if (page_background === "rgba(0, 0, 0, 0)" || page_background === "transparent")
            {
                page_background = getComputedStyle(document.documentElement).backgroundColor;
            }

            if (page_background === "rgba(0, 0, 0, 0)" || page_background === "transparent")
            {
                page_background = "#12151c";
            }

            loader.style.backgroundColor = page_background;
        }

        loader.style.display = (is_visible == true) ? "flex" : "none";
    }
}

/**
 * @brief hides the connect page behind the spinner while node is still connecting for the webview
 *        a deadline reveals the page anyway, because nothing arrives at all if node is wedged
 *
 * @return void
 */
function connection__hold_back_connect_page()
{
    if (g_loopback_port <= 0 || g_is_authenticated == true)
    {
        return;
    }

    if (g_is_holding_back_connect_page == false)
    {
        g_is_holding_back_connect_page = true;
        g_connect_holdback_started = new Date().valueOf();
        document.getElementById("verification-system").style.visibility = "hidden";
        connection__set_connect_holdback_loader_visible(true);

        setTimeout(connection__connect_holdback_check, 250);
    }

    // nothing arrives at all if node is wedged, so the page cannot stay blank forever
    g_connect_holdback_deadline = new Date().valueOf() + 2500;
}

/**
 * @brief node reporting "still working on it" keeps the spinner up a little longer, capped so a looping connect attempt can never hide the page forever
 *
 * @return void
 */
function connection__extend_connect_page_holdback()
{
    if (g_is_holding_back_connect_page == false)
    {
        return;
    }

    let now = new Date().valueOf();

    if (now - g_connect_holdback_started > 12000)
    {
        return;
    }

    g_connect_holdback_deadline = now + 4000;
}

/**
 * @brief the holdback timer: reveals the connect page once the deadline passed, else looks again in 250 ms
 *
 * @return void
 */
function connection__connect_holdback_check()
{
    if (g_is_holding_back_connect_page == false)
    {
        return;
    }

    if (new Date().valueOf() >= g_connect_holdback_deadline)
    {
        connection__reveal_connect_page();
        return;
    }

    setTimeout(connection__connect_holdback_check, 250);
}

/**
 * @brief shows the held-back connect page and drops the spinner
 *
 * @return void
 */
function connection__reveal_connect_page()
{
    if (g_is_holding_back_connect_page == false)
    {
        return;
    }

    g_is_holding_back_connect_page = false;
    document.getElementById("verification-system").style.visibility = "";
    connection__set_connect_holdback_loader_visible(false);
}

/**
 * @brief fades the connect button while an attempt runs; hiding rows caused a layout flicker
 *
 * @param boolean is_pending -> true while an attempt is in flight
 *
 * @return void
 */
function connection__set_connect_button_pending(is_pending)
{
    let button = document.getElementById("connect-button");

    if (button != null && button.classList != null)
    {
        button.classList.toggle("connect-attempt-pending", is_pending == true);
    }
}

/**
 * @brief one dial, driven only by connection__connection_driver(): builds the ws/wss connection string and has the websocket worker open the socket with the right opening frame
 *
 * @param object target -> the host, port and keys to dial
 * @param object identity -> the identity to present
 *
 * @return promise resolves when the attempt has ended, either way
 */
async function connection__attempt_connection(target, identity)
{
    if (android_host__is_ui_only_runtime())
    {
        // the page stays exactly as it is; only the button signals the running attempt
        connection__set_connect_button_pending(true);
    }
    else
    {
        document.getElementById("another-buttons-sub-container").style.display = "none";
        document.getElementById("another-buttons-sub-loading-container").style.display = "";
        document.getElementById("another-buttons-loading-container-p").innerHTML = "connecting to server...";
    }

    // a new attempt is in flight - the failure text only shows after this one resolves
    g_last_connect_attempt_failed = false;

    // loopback: connect to node on this device, token as the opening frame, no dh.
    // stun/turn still point at the real server, so keep the real host for webrtc
    if (target.kind === "loopback")
    {
        g_host = g_autoconnect_details.host;
        port = g_autoconnect_details.port;

        let loopback_connection_string = "ws://127.0.0.1:" + target.port + "/";
        console.log("connection_string -> " + loopback_connection_string + " (loopback)");

        // name the SERVER being joined, not the local hop to node - and never print an
        // unknown one ("connecting to :0" when the settings have not landed yet)
        document.getElementById("another-buttons-loading-container-p").innerHTML =
            (typeof g_host === "string" && g_host.length > 0)
                ? "connecting to " + chat__sanitize_string(g_host) + ":" + chat__sanitize_string("" + port)
                : "connecting...";

        g_websocket_worker.postMessage({
            type: "create_websocket_object",
            value: {
                connection_string: loopback_connection_string,
                onopen_data: target.token
            }
        });

        return;
    }

    g_host = target.host;
    port = target.port;

    keys__init_keys_object();

    while (g_keys_init_status == false)
    {
        await utils__sleep(100);
    }

    let protocol_part_of_connection_string = "ws://";
    let connection_port = port;

    if (document.location.protocol == "https:")
    {
        protocol_part_of_connection_string = "wss://";
        // an https page must use wss, on the separate stunnel wss port (not the plain ws port)
        if (target.wss_port)
        {
            connection_port = target.wss_port;
        }
    }

    let connection_string = protocol_part_of_connection_string + '' + g_host + ':' + connection_port + '/';
    console.log("connection_string -> " + connection_string);

    // say WHERE we are connecting: only now are the real host/port known
    document.getElementById("another-buttons-loading-container-p").innerHTML =
        "connecting to " + chat__sanitize_string(g_host) + ":" + chat__sanitize_string("" + connection_port)
        + "<br>using " + ((protocol_part_of_connection_string == "wss://") ? "secure websocket (wss)" : "websocket (ws)");

    let dh_public_mix = keys__get_public_mix().toString();

    let message_object = {
        message: {
            type: "public_key_info",
            value: identity.public_key_string,
            verification_string: VERIFICATION_MESSAGE,
            dh_public_mix: dh_public_mix
        }
    };

    let message_json_string = connection__process_message_before_sending(message_object);
    let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

    g_websocket_worker.postMessage({
        type: "create_websocket_object",
        value: {
            connection_string: connection_string,
            onopen_data: data
        }
    });
}

/**
 * @brief hands encrypted data to the websocket worker for sending; also counts the bytes into g_session.bytes_sent for the session-info card
 *
 * @param string data_to_send -> the frame to send
 *
 * @return void
 */
function connection__websocket_worker_send(data_to_send)
{
    if (data_to_send != null && data_to_send.length > 0) { g_session.bytes_sent += data_to_send.length; }

    g_websocket_worker.postMessage({
        type: "send",
        value: data_to_send
    });
}

/**
 * @brief builds, encrypts and ships one client -> server message, the process/encrypt/send triple that used to be pasted at every call site
 *
 * @param object message_object -> { message: { type, ... } }
 *
 * @return void
 */
function connection__send_message_object(message_object)
{
    let message_json_string = connection__process_message_before_sending(message_object);
    let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);
    connection__websocket_worker_send(data);
}

/**
 * @brief logs the outgoing message type and serialises the message
 *
 * @param object message_object -> { message: { type, ... } }
 *
 * @return string the message as json
 */
function connection__process_message_before_sending(message_object)
{
    let outgoing_type = message_object.message.type;

    if (outgoing_type != "ice_candidate" && outgoing_type != "sdp_answer"
        && outgoing_type != "client_connection_check" && outgoing_type != "create_new_webrtc_datachannel_connection")
    {
        utils__custom_log('[S] msg.message.type : ' + outgoing_type);
    }
    return JSON.stringify(message_object);
}

/**
 * @brief the saved server configurations, kept on this device only
 *
 * @return array the bookmarks as [{ name, host, port }], [] when none or unreadable
 */
function connection__read_server_bookmarks()
{
    try
    {
        let stored = JSON.parse(utils__storage_get("lemon_server_bookmarks"));
        return Array.isArray(stored) ? stored : [];
    }
    catch (e) { return []; }
}

/**
 * @brief persists the server bookmarks to localStorage
 *
 * @param array bookmarks -> the bookmarks as [{ name, host, port }]
 *
 * @return void
 */
function connection__write_server_bookmarks(bookmarks)
{
    utils__storage_set("lemon_server_bookmarks", JSON.stringify(bookmarks));
}

/**
 * @brief repaints the bookmark dropdown; the option value is the index into the stored array
 *
 * @return void
 */
function connection__render_server_bookmarks()
{
    let select_element = document.getElementById("server-bookmark-select");

    if (select_element == null) { return; }

    let bookmarks = connection__read_server_bookmarks();
    select_element.innerHTML = "";

    let placeholder = document.createElement("option");
    placeholder.value = "";
    placeholder.textContent = (bookmarks.length > 0) ? "saved servers" : "no saved servers";
    select_element.appendChild(placeholder);

    for (let x = 0; x < bookmarks.length; x++)
    {
        let option = document.createElement("option");
        option.value = "" + x;
        option.textContent = bookmarks[x].name + " (" + bookmarks[x].host + ":" + bookmarks[x].port + ")";
        select_element.appendChild(option);
    }
}

/**
 * @brief wires the bookmark dropdown and its buttons on the connect page: picking one loads it into the address and port fields, ready to connect
 *
 * @return void
 */
function connection__wire_server_bookmarks()
{
    let select_element = document.getElementById("server-bookmark-select");

    if (select_element == null) { return; }

    connection__render_server_bookmarks();

    // picking one loads it into the address and port fields, ready to connect
    select_element.onchange = function()
    {
        if (this.value === "") { return; }

        let bookmark = connection__read_server_bookmarks()[parseInt(this.value)];

        if (bookmark == null) { return; }

        document.getElementById("input-ip-address").value = bookmark.host;
        document.getElementById("input-port-number").value = bookmark.port;
        document.getElementById("server-bookmark-name").value = bookmark.name;
    };

    // saving under a name that already exists overwrites it, so this doubles as "update"
    document.getElementById("server-bookmark-save").onclick = function()
    {
        let name_element = document.getElementById("server-bookmark-name");
        let name = name_element.value.trim();
        let typed_host = document.getElementById("input-ip-address").value.trim();
        let typed_port = document.getElementById("input-port-number").value.trim();

        if (name.length === 0) { utils__custom_alert("give this server a name first"); return; }
        if (typed_host.length === 0 || typed_port.length === 0) { utils__custom_alert("fill in the address and port first"); return; }

        let bookmarks = connection__read_server_bookmarks();
        let existing = -1;

        for (let x = 0; x < bookmarks.length; x++)
        {
            if (bookmarks[x].name.toLowerCase() === name.toLowerCase()) { existing = x; }
        }

        if (existing === -1) { bookmarks.push({ name: name, host: typed_host, port: typed_port }); }
        else { bookmarks[existing] = { name: name, host: typed_host, port: typed_port }; }

        connection__write_server_bookmarks(bookmarks);
        connection__render_server_bookmarks();
    };

    document.getElementById("server-bookmark-delete").onclick = function()
    {
        let selected = document.getElementById("server-bookmark-select").value;

        if (selected === "") { utils__custom_alert("pick a saved server to delete"); return; }

        let bookmarks = connection__read_server_bookmarks();
        bookmarks.splice(parseInt(selected), 1);

        connection__write_server_bookmarks(bookmarks);
        connection__render_server_bookmarks();
        document.getElementById("server-bookmark-name").value = "";
    };
}

// workers.js is embedded in template.html along with the other client files, and in the node bundle
// it holds the two worker handlers, each running inside its worker only: the data-processing worker
// (the rsa keypair, decrypting what the socket delivers, encrypting what the page sends) and the
// websocket worker (the socket to the server, frames relayed both ways)
// worker-entry.js picks the handler; connection.js posts to them, dispatch.js receives what they post

// state private to this file
// the server refuses uploads of 4096 base64 characters or less, so file bodies are padded to this
// many plaintext bytes (5462 characters encoded); without it a small file would be refused silently
var CHAT_FILE_BODY_MIN_PADDED_BYTES = 4096;

// when on, every decrypted server frame is also posted back raw (node uses it for the ui replay)
var dpw_forward_frames = false;

var websocket_instance = null;

// counts sockets ever created. events from an old (replaced) socket are ignored
var websocket_generation = 0;

// ---- data-processing worker ----

/**
 * @brief the data-processing worker's message handler
 *        generates the rsa keypair, decrypts what the socket delivers, encrypts what the page sends
 *        (chat messages, pictures, channel keys) and posts every result back to the main thread
 *
 * @param object e -> the worker message event; e.data.type picks the branch
 *
 * @return void
 */
function workers__data_processing_worker_onmessage(e)
{
    if (DBG_WORKER_BOOT_LOG) { console.log("[dpw] got: " + (e.data && e.data.type)); }
    if (e.data.type == "mainthread__set_frame_forwarding")
    {
        dpw_forward_frames = (e.data.value == true);
    }
    else if (e.data.type == "mainthread__set_loopback_mode")
    {
        // the worker builds and transport-encrypts direct messages itself; without this
        // its encrypt_all still wrapped them, node wrapped again, and the server kicked
        // the connection on the resulting garbage
        g_loopback_port = e.data.value;
    }
    else if (e.data.type == "mainthread__metadata_keys")
    {
        g_metadata_keys = [];
        g_metadata_keys.length = 0;
        g_metadata_keys = e.data.value;
        global.postMessage({
            type: "data_processing_worker__metadata_keys_accepted",
            value: null
        });
    }
    else if (e.data.type == "mainthread__generate_rsa_keypair")
    {
        utils__custom_log("generating random public/private key pair");
        if (DBG_WORKER_BOOT_LOG) { console.log("[keygen] starting generateRSAKey"); }

        const randomstring = (length = 8) =>
        {
            // Declare all characters
            let chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';

            // Pick characers randomly
            let str = '';
            for (let i = 0; i < length; i++)
            {
                str += chars.charAt(Math.floor(Math.random() * chars.length));
            }

            return str;

        };

        let identity_passphrase_string = "";

        if (e.data.from_identity_string == true && e.data.identity_passphrase_string != null && e.data.identity_passphrase_string.length > 0)
        {
            identity_passphrase_string = e.data.identity_passphrase_string; // also known as passphrase
        }
        else
        {
            identity_passphrase_string = randomstring(200); // also known as passphrase
        }

        // the size travels with the request: this worker has its own copy of every global,
        // so the main thread's preference does not reach it any other way
        let requested_rsa_key_bits = 2048;
        if (G_ALLOWED_RSA_KEY_BITS.indexOf(e.data.rsa_key_bits) >= 0) { requested_rsa_key_bits = e.data.rsa_key_bits; }

        try
        {
            g_my_rsa_key_object = lemon_crypto.generateRSAKey(identity_passphrase_string, requested_rsa_key_bits);
        }
        catch (err)
        {
            // the wasm module is the only rsa implementation; without it there is no identity
            utils__custom_log("FATAL: rsa keygen wasm failed (" + err + "), cannot create an identity");
            throw err;
        }
        g_rsa_public_key_string = lemon_crypto.publicKeyString(g_my_rsa_key_object);

        utils__custom_log("identity_passphrase_string now in use -> " + identity_passphrase_string);

        global.postMessage({
            type: "data_processing_worker__generate_rsa_keypair_result",
            value: g_rsa_public_key_string,
            identity_string: identity_passphrase_string
        });

        utils__custom_log("public key now in use: " + g_rsa_public_key_string);

    }
    else if (e.data.type == "mainthread__process_received_websocket_message")
    {
        workers__mainthread__process_received_websocket_message_continue(e);
    }
    else if (e.data.type == "mainthread_reset_data")
    {
        g_is_authenticated = false;
    }
    else if (e.data.type == "mainthread__channel_keys_for_data_processing_worker")
    {
        if (g_current_channel_keys != null)
        {
            g_historic_keys_of_current_channel.push(g_current_channel_keys);

            if (g_historic_keys_of_current_channel.length > 7)
            {
                g_historic_keys_of_current_channel.shift();
            }
        }
        g_current_channel_keys = e.data.value;
    }
    // a read receipt, encrypted for the original sender the same way channel keys are.
    // the server never learns what it says: to it this is an ordinary direct message
    else if (e.data.type == "mainthread__create_seen_receipt_message")
    {
        let receipt_keys = [];

        for (let x = 0; x < 4; x++)
        {
            let single_key = { key_bytes: [], iv_bytes: [] };

            for (let ii = 0; ii < 32; ii++)
            {
                single_key.key_bytes.push(crypto.getRandomValues(new Uint8Array(1))[0]);
            }

            for (let ii = 0; ii < 16; ii++)
            {
                single_key.iv_bytes.push(crypto.getRandomValues(new Uint8Array(1))[0]);
            }

            receipt_keys.push(single_key);
        }

        let receipt_encryption = lemon_crypto.encrypt(JSON.stringify(receipt_keys), e.data.public_key);

        let receipt_value_object = {
            type: "message_seen",
            value: "" + e.data.server_chat_message_id
        };

        let receipt_data = {
            message_keys: receipt_encryption.cipher,
            message_value: keys__encrypt_with_aes_keys_and_convert_to_base64(receipt_keys, JSON.stringify(receipt_value_object))
        };

        let receipt_message_object = {
            message: {
                type: "direct_chat_message",
                value: JSON.stringify(receipt_data),
                receiver_type: null,
                receiver_id: parseInt(e.data.receiver_id),
                local_message_id: 1
            }
        };

        global.postMessage({
            type: "data_processing_worker__seen_receipt_message_result",
            seen_receipt_message_content: keys__encrypt_all_message_data_and_convert_to_base64(JSON.stringify(receipt_message_object))
        });
    }
    else if (e.data.type == "mainthread__create_websocket_channel_keys_message")
    {
        let new_channel_keys = [];

        for (let i = 0; i < 4; i++)
        {
            let single_key = {
                key_bytes: [],
                iv_bytes: []
            };

            for (let ii = 0; ii < 32; ii++)
            {
                let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                single_key.key_bytes.push(single_byte);
            }

            for (let ii = 0; ii < 16; ii++)
            {
                let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                single_key.iv_bytes.push(single_byte);
            }

            new_channel_keys.push(single_key);
        }

        g_current_channel_keys = new_channel_keys;

        global.postMessage({
            type: "data_processing_worker__new_channel_keys_from_data_processing_worker",
            value: new_channel_keys
        });

        for (let x = 0; x < e.data.clients.length; x++)
        {
            if (e.data.clients[x].channel_id != e.data.current_channel_id)
            {
                continue;
            }
            // under node the maintainer also sends ITSELF a keys message: the server
            // echoes it back and the loopback forwards it, which is how the webview
            // learns the keys. in a browser the local copy suffices, skip as before
            if (e.data.clients[x].client_id == e.data.local_client_id && typeof process === "undefined")
            {
                continue;
            }

            let current_message_keys = [];

            for (let i = 0; i < 4; i++)
            {
                let single_key = {
                    key_bytes: [],
                    iv_bytes: []
                };

                for (let ii = 0; ii < 32; ii++)
                {
                    let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                    single_key.key_bytes.push(single_byte);
                }

                for (let ii = 0; ii < 16; ii++)
                {
                    let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                    single_key.iv_bytes.push(single_byte);
                }

                current_message_keys.push(single_key);
            }

            let single_message_aes_keys_string = JSON.stringify(current_message_keys);

            let encryption_result = lemon_crypto.encrypt(single_message_aes_keys_string, e.data.clients[x].public_key);

            let message_data = {
                message_keys: encryption_result.cipher,
                message_value: ""
            };

            let message_value_object = {
                type: "channel_keys_from_maintainer",
                value: JSON.stringify(g_current_channel_keys)
            };

            message_data.message_value = keys__encrypt_with_aes_keys_and_convert_to_base64(current_message_keys, JSON.stringify(message_value_object));

            let message_object = {
                message: {
                    type: null,
                    value: null,
                    receiver_type: null,
                    receiver_id: null,
                    local_message_id: 1 // local_message_id is not used but still needed because channel keys messages are subform of direct messages, and this is processed the same as direct message, this object property is also needed even if not used
                    // because server side processes these type of messages same as direct messages, to prevent server from discarding this as wrong, this field is specified even if not needed
                }
            };

            message_object.message.type = "direct_chat_message";
            message_object.message.value = JSON.stringify(message_data);
            message_object.message.receiver_id = parseInt(e.data.clients[x].client_id);

            let message_json_string = JSON.stringify(message_object);
            let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

            // channel keys message created, its contents encrypted
            // send message data, to be sent over websocket, to main thread

            if (e.data.clients[x].client_id == e.data.local_client_id)
            {
                // our own copy: the server never echoes a direct message back to its
                // sender, so hand the ui the decrypted shape it now expects
                global.postMessage({
                    type: "data_processing_worker__local_channel_keys_for_ui",
                    value: JSON.stringify({
                        message: {
                            type: "direct_chat_message",
                            sender_id: e.data.local_client_id,
                            sender_username: e.data.clients[x].username,
                            value: null,
                            some_json: message_value_object
                        }
                    })
                });
            }
            else
            {
                global.postMessage({
                    type: "data_processing_worker__create_websocket_channel_keys_message_result",
                    channel_keys_message_content: data,
                    username: e.data.clients[x].username
                });
            }
        }
    }
    else if (e.data.type == "mainthread__create_direct_chat_message")
    {
        let current_message_keys = [];

        for (let i = 0; i < 4; i++)
        {
            let single_key = {
                key_bytes: [],
                iv_bytes: []
            };

            for (let ii = 0; ii < 32; ii++)
            {
                let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                single_key.key_bytes.push(single_byte);
            }

            for (let ii = 0; ii < 16; ii++)
            {
                let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                single_key.iv_bytes.push(single_byte);
            }

            current_message_keys.push(single_key);
        }

        let single_message_aes_keys_string = JSON.stringify(current_message_keys);
        let encryption_result = lemon_crypto.encrypt(single_message_aes_keys_string, e.data.receiver_public_key);

        let message_data = {
            message_keys: encryption_result.cipher,
            message_value: ""
        };

        let message_value_object = {
            type: "direct_chat_message",
            value: e.data.chat_message_value
        };

        message_data.message_value = keys__encrypt_with_aes_keys_and_convert_to_base64(current_message_keys, JSON.stringify(message_value_object));

        let message_object = {
            message: {
                type: null,
                value: null,
                receiver_id: null,
                local_message_id: e.data.local_message_id
            }
        };

        message_object.message.type = "direct_chat_message";
        message_object.message.value = JSON.stringify(message_data);
        message_object.message.receiver_id = parseInt(e.data.chat_message_receiver_id);

        let message_json_string = connection__process_message_before_sending(message_object);

        let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

        global.postMessage({
            type: "data_processing_worker__tell_websocket_worker_to_send_data",
            data_to_be_sent_over_websocket: data
        });
    }
    else if (e.data.type == "mainthread__create_offline_chat_message")
    {
        // identical crypto to a direct message - the recipient is simply not connected,
        // so it is addressed by their registered alias and the server parks the result
        // until they come back. the server sees the same opaque blob either way.
        let current_message_keys = [];

        for (let i = 0; i < 4; i++)
        {
            let single_key = {
                key_bytes: [],
                iv_bytes: []
            };

            for (let ii = 0; ii < 32; ii++)
            {
                let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                single_key.key_bytes.push(single_byte);
            }

            for (let ii = 0; ii < 16; ii++)
            {
                let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                single_key.iv_bytes.push(single_byte);
            }

            current_message_keys.push(single_key);
        }

        let single_message_aes_keys_string = JSON.stringify(current_message_keys);
        let encryption_result = lemon_crypto.encrypt(single_message_aes_keys_string, e.data.receiver_public_key);

        let message_data = {
            message_keys: encryption_result.cipher,
            message_value: ""
        };

        let message_value_object = {
            type: "direct_chat_message",
            value: e.data.chat_message_value
        };

        message_data.message_value = keys__encrypt_with_aes_keys_and_convert_to_base64(current_message_keys, JSON.stringify(message_value_object));

        let message_object = {
            message: {
                type: "offline_chat_message",
                value: JSON.stringify(message_data),
                recipient_alias: e.data.recipient_alias
            }
        };

        let message_json_string = connection__process_message_before_sending(message_object);

        let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

        global.postMessage({
            type: "data_processing_worker__tell_websocket_worker_to_send_data",
            data_to_be_sent_over_websocket: data
        });
    }
    else if (e.data.type == "mainthread__create_direct_chat_picture")
    {
        let current_message_keys = [];

        for (let i = 0; i < 4; i++)
        {
            let single_key = {
                key_bytes: [],
                iv_bytes: []
            };

            for (let ii = 0; ii < 32; ii++)
            {
                let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                single_key.key_bytes.push(single_byte);
            }

            for (let ii = 0; ii < 16; ii++)
            {
                let single_byte = crypto.getRandomValues(new Uint8Array(1))[0];
                single_key.iv_bytes.push(single_byte);
            }

            current_message_keys.push(single_key);
        }

        let single_message_aes_keys_string = JSON.stringify(current_message_keys);

        let encryption_result = lemon_crypto.encrypt(single_message_aes_keys_string, e.data.receiver_public_key);

        // data_for_upload_process are the data server should not be possible to decrypt, server only forwards them to client
        // message_object contains metadata server can decrypt

        let message_data = {
            message_keys: encryption_result.cipher,
            message_value: ""
        };

        message_data.message_value = keys__encrypt_with_aes_keys_and_convert_to_base64(current_message_keys, e.data.base64_picture_string_to_send);

        let data_for_upload_process = JSON.stringify(message_data);

        let extra_data = {
            receiver_id: parseInt(e.data.chat_message_receiver_id),
            local_message_id: e.data.local_message_id
        };

        global.postMessage({
            type: "data_processing_worker__direct_chat_picture_to_be_uploaded_by_parts",
            data_for_upload_process: data_for_upload_process,
            extra_data: extra_data
        });
    }
    else if (e.data.type == "mainthread__create_channel_chat_picture")
    {
        let extra_data = {
            receiver_id: parseInt(e.data.current_channel_id),
            local_message_id: e.data.local_message_id
        };

        let data_for_upload_process = keys__encrypt_with_aes_keys_and_convert_to_base64(e.data.current_channel_keys, e.data.base64_picture_string_to_send);

        global.postMessage({
            type: "data_processing_worker__channel_chat_picture_to_be_uploaded_by_parts",
            data_for_upload_process: data_for_upload_process,
            extra_data: extra_data
        });
    }
    else if (e.data.type == "mainthread__create_channel_chat_message")
    {
        let message_object = {
            message: {
                type: "channel_chat_message",
                value: "",
                receiver_id: parseInt(e.data.current_channel_id),
                local_message_id: e.data.local_message_id // server needs this to inform client that message has been received.This is different from message ID stored by server
            }
        };

        message_object.message.value = keys__encrypt_with_aes_keys_and_convert_to_base64(e.data.current_channel_keys, e.data.chat_message_to_send_value);

        let message_json_string = connection__process_message_before_sending(message_object);
        let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

        global.postMessage({
            type: "data_processing_worker__tell_websocket_worker_to_send_data",
            data_to_be_sent_over_websocket: data
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_direct_chat_picture_data")
    {
        let message_data = JSON.parse(e.data.message_raw);
        let decryption_result = lemon_crypto.decrypt(message_data.message_keys, g_my_rsa_key_object);

        if (decryption_result.status == 'failure')
        {
            log("failed to decrypt direct chat picture");
            return;
        }

        let current_message_keys = JSON.parse(decryption_result.plaintext);
        let decrypted_base64_picture = keys__decrypt_base64_contents_with_aes_keys(current_message_keys, message_data.message_value);
        let decrypted_base64_picture_sanitized = chat__sanitize_image_data_url(decrypted_base64_picture);

        let msg = {
            message : {
                picture_id: e.data.picture_id,
                client_id: e.data.sender_id,
                value: null,
                decrypted_base64_picture: ""
            }
        };

        msg.message.decrypted_base64_picture = decrypted_base64_picture_sanitized;

        // loopback: the ui has no key for this, so send it the finished picture
        if (dpw_forward_frames == true)
        {
            global.postMessage({ type: "decrypted_frame", value: JSON.stringify({
                message: {
                    type: "direct_chat_picture_decrypted",
                    picture_id: e.data.picture_id,
                    client_id: e.data.sender_id,
                    decrypted_base64_picture: decrypted_base64_picture_sanitized
                }
            }) });
        }

        global.postMessage({
            type: "data_processing_worker__direct_chat_picture",
            value: msg
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_channel_chat_picture_data")
    {
        if (g_current_channel_keys != null)
        {
            let msg = {
                message : {
                    picture_id: e.data.picture_id,
                    client_id: e.data.sender_id,
                    value: null,
                    decrypted_base64_picture: ""
                }
            }

            let is_decrypted = false;
            let decrypted_base64_picture = keys__decrypt_base64_contents_with_aes_keys(g_current_channel_keys, e.data.message_raw);

            if (decrypted_base64_picture.startsWith("data:image"))
            {
                is_decrypted = true;
            }
            else
            {
                for (i = 0; i < g_historic_keys_of_current_channel.length; i++)
                {
                    decrypted_base64_picture = keys__decrypt_base64_contents_with_aes_keys(g_historic_keys_of_current_channel[i], e.data.message_raw);
                    if (decrypted_base64_picture.startsWith("data:image"))
                    {
                        console.log("managed to decrypt channel chat picture with channel key at" + i);
                        is_decrypted = true;
                        break;
                    }
                }
            }

            if (is_decrypted)
            {
                let decrypted_base64_picture_sanitized = chat__sanitize_image_data_url(decrypted_base64_picture);
                msg.message.decrypted_base64_picture = decrypted_base64_picture_sanitized;
                msg.message.value = null;
                global.postMessage({
                    type: "data_processing_worker__channel_chat_picture",
                    value: msg
                });
            }
        }
        else
        {
            utils__custom_log("channel_chat_picture decrypt failed, data_processing_worker thread missing current channel keys");
        }
    }
    else if (e.data.type == "mainthread__create_direct_chat_file")
    {
        // header and body get their own key sets: two plaintexts must never share a ctr keystream.
        // the body pads up so even a 1 kb file clears the server's minimum upload size
        let file_header = chat_files__create_direct_chat_file_envelope(e.data.receiver_public_key, chat_files__build_chat_file_header_json(e.data.file));
        let data_for_upload_process = chat_files__create_direct_chat_file_envelope(e.data.receiver_public_key, chat_files__build_chat_file_body_json(e.data.file), CHAT_FILE_BODY_MIN_PADDED_BYTES);

        if (file_header == null || data_for_upload_process == null)
        {
            global.postMessage({
                type: "data_processing_worker__chat_file_send_failed",
                local_message_id: e.data.local_message_id,
                reason: "could not encrypt the file for this receiver"
            });
            return;
        }

        global.postMessage({
            type: "data_processing_worker__direct_chat_file_to_be_uploaded_by_parts",
            data_for_upload_process: data_for_upload_process,
            extra_data: {
                receiver_id: parseInt(e.data.chat_message_receiver_id),
                local_message_id: e.data.local_message_id,
                file_header: file_header
            }
        });
    }
    else if (e.data.type == "mainthread__create_channel_chat_file")
    {
        global.postMessage({
            type: "data_processing_worker__channel_chat_file_to_be_uploaded_by_parts",
            data_for_upload_process: chat_files__encrypt_string_with_aes_keys_fast(e.data.current_channel_keys, chat_files__build_chat_file_body_json(e.data.file), CHAT_FILE_BODY_MIN_PADDED_BYTES),
            extra_data: {
                receiver_id: parseInt(e.data.current_channel_id),
                local_message_id: e.data.local_message_id,
                file_header: chat_files__encrypt_string_with_aes_keys_fast(e.data.current_channel_keys, chat_files__build_chat_file_header_json(e.data.file))
            }
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_direct_chat_file_data")
    {
        let file = chat_files__parse_decrypted_chat_file_body(chat_files__open_direct_chat_file_envelope(e.data.message_raw));

        if (file == null)
        {
            utils__custom_log("failed to decrypt direct chat file");
            global.postMessage({
                type: "data_processing_worker__chat_file_decrypt_failed",
                value: { message: { file_id: e.data.file_id } }
            });
            return;
        }

        let msg = {
            message: {
                file_id: e.data.file_id,
                client_id: e.data.sender_id,
                file: file
            }
        };

        // loopback: the ui has no key for this, so send it the finished file
        if (dpw_forward_frames == true)
        {
            global.postMessage({ type: "decrypted_frame", value: JSON.stringify({
                message: {
                    type: "direct_chat_file_decrypted",
                    file_id: e.data.file_id,
                    client_id: e.data.sender_id,
                    file: file
                }
            }) });
        }

        global.postMessage({
            type: "data_processing_worker__direct_chat_file",
            value: msg
        });
    }
    else if (e.data.type == "mainthread__process_encrypted_channel_chat_file_data")
    {
        let file = chat_files__decrypt_channel_chat_file_body(e.data.file_id, e.data.message_raw);

        if (file == null)
        {
            utils__custom_log("channel_chat_file decrypt failed, no channel key opened it");
            global.postMessage({
                type: "data_processing_worker__chat_file_decrypt_failed",
                value: { message: { file_id: e.data.file_id } }
            });
            return;
        }

        global.postMessage({
            type: "data_processing_worker__channel_chat_file",
            value: {
                message: {
                    file_id: e.data.file_id,
                    client_id: e.data.sender_id,
                    file: file
                }
            }
        });
    }
}

/**
 * @brief the announced minimum key size when a raw frame is the plaintext "your key is too weak" notice
 *        that notice is the only type accepted in the clear, because the server sends it before the
 *        diffie-hellman exchange has produced a shared secret
 *
 * @param string raw_value -> the frame as it arrived on the socket
 *
 * @return number|null the minimum rsa key bits, null for any other frame
 */
function workers__try_read_plaintext_rsa_key_notice(raw_value)
{
    if (typeof raw_value !== "string" || raw_value.indexOf("rsa_key_too_weak") < 0)
    {
        return null;
    }

    let parsed = null;

    try { parsed = JSON.parse(raw_value); }
    catch (parse_error) { return null; }

    if (parsed == null || parsed.message == null || parsed.message.type !== "rsa_key_too_weak")
    {
        return null;
    }
    return parsed.message.minimum_rsa_key_bits;
}

/**
 * @brief processes one frame that arrived on the socket
 *        recognises the plaintext key-size notice, decrypts everything else and posts each server
 *        message to the main thread under its data_processing_worker__*_from_server type
 *
 * @param object e -> the worker message event; e.data.value is the frame, e.data.is_plaintext marks a loopback frame
 *
 * @return void
 */
function workers__mainthread__process_received_websocket_message_continue(e)
{
    if (e.data.is_plaintext != true)
    {
        let announced_minimum_bits = workers__try_read_plaintext_rsa_key_notice(e.data.value);

        if (announced_minimum_bits != null)
        {
            global.postMessage({
                type: "data_processing_worker__rsa_key_too_weak",
                minimum_rsa_key_bits: announced_minimum_bits
            });
            return;
        }
    }

    // loopback frames are plaintext; everything downstream is identical
    let decrypted_metadata = (e.data.is_plaintext == true) ? e.data.value : keys__decrypt_message_metadata(e.data.value);

    let msg = null;

    try
    {
        msg = JSON.parse(decrypted_metadata);
    }
    catch (Exception)
    {
        console.log(Exception+ " decrypted string length: " + decrypted_metadata.length + " "+ decrypted_metadata);
    }

    // the webrtc handshake retries every 10s and its frames used to flood the 50kb log,
    // pushing the interesting events out before they could be read
    if (msg.message.type != "ice_candidate" && msg.message.type != "sdp_offer"
        && msg.message.type != "connection_check_response")
    {
        utils__custom_log('[R] msg.message.type : ' + msg.message.type);
    }

    // rsa-encrypted kinds are forwarded from their own branches instead, once node has
    // decrypted them - the ui never holds a private key
    if (dpw_forward_frames == true
        && msg.message.type != "direct_chat_message"
        && msg.message.type != "offline_chat_message"
        && msg.message.type != "direct_chat_file_metadata")
    {
        global.postMessage({ type: "decrypted_frame", value: decrypted_metadata });
    }

    // node's connection status for the login page - meta, not protocol, and it must
    // flow while unauthenticated, so it is handled before the auth gate
    if (msg.message.type == "loopback_status")
    {
        global.postMessage({
            type: "data_processing_worker__loopback_status",
            value: msg.message.value
        });
        return;
    }

    // handled before the auth gate, so a login replay is never dropped
    // (a stale "already authenticated" worker used to swallow it)
    if (msg.message.type == "authentication_status")
    {
        if (msg.message.value == "success")
        {
            g_is_authenticated = true;

            // the whole message rides along as the policy: server_settings_tab__apply_server_policy_fields takes the
            // fields it knows, so a new server field never needs a second list here
            global.postMessage({
                type: "data_processing_worker__authentication_status",
                is_idle_mode_allowed: msg.message.is_idle_mode_allowed,
                policy: msg.message,
                value: "success"
            });

            // the audio datachannel is needed when either client voice or music bots are on (the
            // server pushes bot audio through it); whether this client may transmit is a separate flag
            if (msg.message.is_voice_chat_active == true || msg.message.is_music_bot_audio_active == true)
            {
                global.postMessage({
                    type: "data_processing_worker__audio_enabled",
                    value: true,
                    client_voice_allowed: msg.message.is_voice_chat_active == true,
                    stun_port: msg.message.stun_port
                });
            }
        }
        return;
    }

    // a fast reconnect's answer comes right before authentication_status, i.e. while this worker is
    // still in handshake mode, so it is handled ahead of the authenticated / unauthenticated split
    if (msg.message.type == "fast_reconnect_ok")
    {
        global.postMessage({
            type: "data_processing_worker__fast_reconnect_ok",
            value: msg
        });
        return;
    }

    if (msg.message.type == "datachannel_cooldown")
    {
        global.postMessage({
            type: "data_processing_worker__datachannel_cooldown",
            value: msg
        });
        return;
    }

    if (g_is_authenticated == false)
    {
        if (msg.message.type == "public_key_challenge")
        {
            decryption_result = lemon_crypto.challenge_decrypt(msg.message.value, g_my_rsa_key_object); // has to be processed here, the main thread never holds the private key

            msg.message.decryption_result = decryption_result;

            global.postMessage({
                type: "data_processing_worker__public_key_challenge_from_server",
                value: msg
            });

        }
        else
        {
            global.postMessage({
                type: "data_processing_worker__authentication_status",
                value: "fail"
            });
        }
    }
    else
    {
        if (msg.message.type == "connection_check_response")
        {
            global.postMessage({
                type: "data_processing_worker__connection_check_response",
                value: msg
            });
        }
        else if (msg.message.type == "client_list")
        {
            global.postMessage({
                type: "data_processing_worker__client_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_avatar")
        {
            global.postMessage({
                type: "data_processing_worker__client_avatar_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "avatar_changed")
        {
            global.postMessage({
                type: "data_processing_worker__avatar_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "poke")
        {
            global.postMessage({
                type: "data_processing_worker__poke_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "access_denied")
        {
            global.postMessage({
                type: "data_processing_worker__access_denied_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "force_admin_password_change")
        {
            global.postMessage({
                type: "data_processing_worker__force_admin_password_change",
                value: msg
            });
        }
        else if (msg.message.type == "server_settings_values")
        {
            global.postMessage({
                type: "data_processing_worker__server_settings_values_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "admin_log")
        {
            global.postMessage({
                type: "data_processing_worker__admin_log_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "server_policy")
        {
            global.postMessage({
                type: "data_processing_worker__server_policy_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_info")
        {
            global.postMessage({
                type: "data_processing_worker__client_info_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_full")
        {
            global.postMessage({
                type: "data_processing_worker__channel_full_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_list")
        {
            global.postMessage({
                type: "data_processing_worker__channel_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "icon_list")
        {
            global.postMessage({
                type: "data_processing_worker__icon_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "tag_list")
        {
            global.postMessage({
                type: "data_processing_worker__tag_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "identity_list")
        {
            global.postMessage({
                type: "data_processing_worker__identity_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "icon_add")
        {
            global.postMessage({
                type: "data_processing_worker__icon_add_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "tag_add_to_client")
        {
            global.postMessage({
                type: "data_processing_worker__tag_add_to_client_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "remove_tag_from_client")
        {
            global.postMessage({
                type: "data_processing_worker__remove_tag_from_client_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_alias_changed")
        {
            global.postMessage({
                type: "data_processing_worker__client_alias_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_country_code_changed")
        {
            global.postMessage({
                type: "data_processing_worker__client_country_code_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "typing_indicator")
        {
            global.postMessage({
                type: "data_processing_worker__typing_indicator_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "stored_clients_list")
        {
            global.postMessage({
                type: "data_processing_worker__stored_clients_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "tag_add")
        {
            global.postMessage({
                type: "data_processing_worker__tag_add_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "tag_delete")
        {
            global.postMessage({
                type: "data_processing_worker__tag_delete_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "icon_delete")
        {
            global.postMessage({
                type: "data_processing_worker__icon_delete_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "tag_icon_changed")
        {
            global.postMessage({
                type: "data_processing_worker__tag_icon_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_icon_changed")
        {
            global.postMessage({
                type: "data_processing_worker__channel_icon_changed_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "start_song_stream")
        {
            global.postMessage({
                type: "data_processing_worker__start_song_stream_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "stop_song_stream")
        {
            global.postMessage({
                type: "data_processing_worker__stop_song_stream_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "chat_message_delete")
        {
            global.postMessage({
                type: "data_processing_worker__chat_message_delete_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "chat_message_edit")
        {
            global.postMessage({
                type: "data_processing_worker__chat_message_edit_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "server_chat_message_id_for_local_message_id")
        {
            global.postMessage({
                type: "data_processing_worker__server_chat_message_id_for_local_message_id_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_join")
        {
            global.postMessage({
                type: "data_processing_worker__channel_join_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_role_add")
        {
            global.postMessage({
                type: "data_processing_worker__client_role_add_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_connect")
        {
            global.postMessage({
                type: "data_processing_worker__client_connect_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_disconnect")
        {
            global.postMessage({
                type: "data_processing_worker__client_disconnect_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_rename")
        {
            global.postMessage({
                type: "data_processing_worker__client_rename_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "audio_state_of_single_client")
        {
            global.postMessage({
                type: "data_processing_worker__audio_state_of_single_client_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "current_channel_active_microphone_usage")
        {
            global.postMessage({
                type: "data_processing_worker__current_channel_active_microphone_usage_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "sdp_offer")
        {
            global.postMessage({
                type: "data_processing_worker__sdp_offer_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "ice_candidate")
        {
            global.postMessage({
                type: "data_processing_worker__ice_candidate_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "server_info_broadcast")
        {
            global.postMessage({
                type: "data_processing_worker__server_info_broadcast_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_edit")
        {
            global.postMessage({
                type: "data_processing_worker__channel_edit_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "client_going_to_idle_mode")
        {
            global.postMessage({
                type: "data_processing_worker__client_going_to_idle_mode",
                value: msg
            });
        }
        else if (msg.message.type == "client_coming_back_from_idle_mode")
        {
            global.postMessage({
                type: "data_processing_worker__client_coming_back_from_idle_mode",
                value: msg
            });
        }
        else if (msg.message.type == "channel_delete")
        {
            global.postMessage({
                type: "data_processing_worker__channel_delete_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_create")
        {
            global.postMessage({
                type: "data_processing_worker__channel_create_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_chat_picture_metadata")
        {
            global.postMessage({
                type: "data_processing_worker__channel_chat_picture_metadata",
                value: msg
            });
        }
        else if (msg.message.type == "direct_chat_picture_metadata")
        {
            global.postMessage({
                type: "data_processing_worker__direct_chat_picture_metadata",
                value: msg
            });
        }
        else if (msg.message.type == "channel_chat_file_metadata")
        {
            // the header is opened here (channel keys live in this worker); null leaves a nameless card
            msg.message.file_header_decrypted = (typeof msg.message.file_header === "string")
                ? chat_files__decrypt_channel_chat_file_header(msg.message.file_id, msg.message.file_header) : null;
            msg.message.file_header = null;

            global.postMessage({
                type: "data_processing_worker__channel_chat_file_metadata",
                value: msg
            });
        }
        else if (msg.message.type == "direct_chat_file_metadata")
        {
            // loopback: node already opened the header, so just pass it on
            if (msg.message.file_header_decrypted == null)
            {
                msg.message.file_header_decrypted = (typeof msg.message.file_header === "string")
                    ? chat_files__parse_chat_file_header(chat_files__open_direct_chat_file_envelope(msg.message.file_header)) : null;
                msg.message.file_header = null;

                // opened shape, so the ui reads it without a key of its own
                if (dpw_forward_frames == true)
                {
                    global.postMessage({ type: "decrypted_frame", value: JSON.stringify(msg) });
                }
            }

            global.postMessage({
                type: "data_processing_worker__direct_chat_file_metadata",
                value: msg
            });
        }
        else if (msg.message.type == "direct_chat_file_decrypted")
        {
            // loopback only: node decrypted it for us
            global.postMessage({
                type: "data_processing_worker__direct_chat_file",
                value: { message: {
                    file_id: msg.message.file_id,
                    client_id: msg.message.client_id,
                    file: msg.message.file
                } }
            });
        }
        else if (msg.message.type == "file_send_error")
        {
            global.postMessage({
                type: "data_processing_worker__file_send_error_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "channel_maintainer_id")
        {
            global.postMessage({
                type: "data_processing_worker__channel_maintainer_id",
                value: msg
            });
        }
        else if (msg.message.type == "channel_chat_message")
        {
            if (g_current_channel_keys != null)
            {
                let decrypted_value = keys__decrypt_base64_contents_with_aes_keys(g_current_channel_keys, msg.message.value);

                msg.message.decrypted_value = decrypted_value;
                msg.message.value = null;

                global.postMessage({
                    type: "data_processing_worker__channel_chat_message",
                    value: msg
                });
            }
            else
            {
                utils__custom_log("channel_chat_message decrypt failed");
            }
        }
        else if (msg.message.type == "direct_chat_message")
        {
            // loopback: node already decrypted this, so just pass it on
            if (msg.message.some_json != null)
            {
                global.postMessage({
                    type: "data_processing_worker__direct_chat_message",
                    value: msg
                });
                return;
            }

            let message_data = JSON.parse(msg.message.value);

            let decryption_result = lemon_crypto.decrypt(message_data.message_keys, g_my_rsa_key_object);

            if (decryption_result.status == 'failure')
            {
                utils__custom_log("! failed to decrypt direct chat message !");
                return;
            }

            let currect_direct_message_keys = JSON.parse(decryption_result.plaintext);
            let decrypted_text = keys__decrypt_base64_contents_with_aes_keys(currect_direct_message_keys, message_data.message_value);

            let some_json = JSON.parse(decrypted_text);

            if (some_json.type == "direct_chat_message")
            {
                some_json.value = some_json.value; // sanitize here ?
            }

            msg.message.some_json = some_json;
            msg.message.value = null;

            // decrypted shape, so the ui reads it without a key of its own
            if (dpw_forward_frames == true)
            {
                global.postMessage({ type: "decrypted_frame", value: JSON.stringify(msg) });
            }

            global.postMessage({
                type: "data_processing_worker__direct_chat_message",
                value: msg
            });
        }
        else if (msg.message.type == "offline_chat_message")
        {
            // loopback: node already decrypted this, so just pass it on
            if (msg.message.some_json != null)
            {
                global.postMessage({
                    type: "data_processing_worker__offline_chat_message",
                    value: msg
                });
                return;
            }

            // something that was said to us while we were away. same envelope as a direct
            // message, so the same decrypt; it just arrives on connect instead of live
            let message_data = JSON.parse(msg.message.value);

            let decryption_result = lemon_crypto.decrypt(message_data.message_keys, g_my_rsa_key_object);

            if (decryption_result.status == 'failure')
            {
                // queued for a different keypair than the one we hold now
                utils__custom_log("! failed to decrypt offline chat message !");
                return;
            }

            let current_offline_message_keys = JSON.parse(decryption_result.plaintext);
            let decrypted_text = keys__decrypt_base64_contents_with_aes_keys(current_offline_message_keys, message_data.message_value);

            msg.message.some_json = JSON.parse(decrypted_text);
            msg.message.value = null;

            // decrypted shape, so the ui reads it without a key of its own
            if (dpw_forward_frames == true)
            {
                global.postMessage({ type: "decrypted_frame", value: JSON.stringify(msg) });
            }

            global.postMessage({
                type: "data_processing_worker__offline_chat_message",
                value: msg
            });
        }
        else if (msg.message.type == "direct_chat_picture_decrypted")
        {
            // loopback only: node decrypted it for us
            global.postMessage({
                type: "data_processing_worker__direct_chat_picture",
                value: { message: {
                    picture_id: msg.message.picture_id,
                    client_id: msg.message.client_id,
                    value: null,
                    decrypted_base64_picture: msg.message.decrypted_base64_picture
                } }
            });
        }
        else if (msg.message.type == "image_sent_status")
        {
            global.postMessage({
                type: "data_processing_worker__image_sent_status_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "call")
        {
            global.postMessage({
                type: "data_processing_worker__call_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "music_bot_song_list")
        {
            global.postMessage({
                type: "data_processing_worker__music_bot_song_list_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "file_send_success")
        {
            global.postMessage({
                type: "data_processing_worker__file_send_success_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "file_receive_chunk")
        {
            global.postMessage({
                type: "data_processing_worker__file_receive_chunk_from_server",
                value: msg
            });
        }
        else if (msg.message.type == "file_receive_completed")
        {
            global.postMessage({
                type: "data_processing_worker__file_receive_completed_from_server",
                value: msg
            });
        }
    }
}

// ---- websocket worker ----

/**
 * @brief the websocket worker's message handler
 *        creates the socket (relaying its events to the main thread), sends data when the socket
 *        is open, and closes it on request
 *
 * @param object e -> the worker message event; e.data.type picks the branch
 *
 * @return void
 */
function workers__websocket_worker_onmessage(e)
{

    if (e.data.type == "create_websocket_object")
    {
        let generation = ++websocket_generation;

        // a replaced socket is closed, and its events below are ignored as stale
        if (websocket_instance != null)
        {
            try { websocket_instance.close(); } catch (close_error) { }
        }

        websocket_instance = new WebSocket(e.data.value.connection_string);

        websocket_instance.addEventListener('message', function (event)
        {
            if (generation != websocket_generation) { return; }

            global.postMessage({
                type: "websocket_worker_onmessage",
                value: event.data
            });
        });

        websocket_instance.onclose = function ()
        {
            if (generation != websocket_generation) { return; }

            console.log("websocket closed");
            global.postMessage({
                type: "websocket_worker_onclose"
            });
        }

        websocket_instance.onerror = function (error)
        {
            if (generation != websocket_generation) { return; }

            // node hands us the real socket error; its code says WHY (no network vs
            // refused vs timeout), which is the difference between a useful message
            // and a guess. a browser gives an opaque event and no code - that is fine
            let error_code = (error != null && typeof error.code === "string") ? error.code : "";

            console.error("websocket error event" + (error_code !== "" ? " (" + error_code + ")" : ""));
            global.postMessage({
                type: "websocket_worker_onerror",
                error_code: error_code
            });
        }

        websocket_instance.onopen = function ()
        {
            if (generation != websocket_generation) { return; }

            websocket_instance.send(e.data.value.onopen_data);
        }
    }

    else if (e.data.type == "send")
    {
        // null before the first connect: the loopback ui can forward a request while
        // node still has no socket, and dereferencing it threw out of the handler
        if (websocket_instance == null)
        {
            console.warn("websocket send skipped: no socket yet");
        }
        else if (websocket_instance.readyState === 1)
        {
            websocket_instance.send(e.data.value);
        }
        else
        {
            console.warn("websocket send skipped: socket not open (readyState " + websocket_instance.readyState + ")");
        }
    }

    else if (e.data.type == "close")
    {
        // deliberate disconnect (identity switch): the onclose event that follows
        // drives the normal reset path on the main thread
        if (websocket_instance != null)
        {
            websocket_instance.close();
        }
    }
}

// console-log.js is embedded in template.html along with the other client files, and in the node bundle
// it wraps console.* in every context (the page and all five workers) with a coloured [context][fn:line][time] tag
// tune it live from devtools: global.__LOG.disable() / .enable() / .setTime(false) / .set({ level: "warn" })
// reach every context; global.__LOG.level = "warn" or .mute.push("opus_decoder_worker") only the one you type in
// THREAD_NAME names the context; do not add another THREAD_NAME assignment before this file
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
    var cfg = global.__LOG || (global.__LOG = { enabled: true, level: "debug", mute: [], time: true, raw: false });

    // cross-context control: cfg.set / .disable / .enable / .setTime broadcast to every context over
    // a BroadcastChannel; the receive handler only copies fields and never re-broadcasts (no echo loop)
    var log_channel = null;
    try { log_channel = new BroadcastChannel("lemon_log_config"); } catch (e) { console.warn("BroadcastChannel unavailable; cross-context log control disabled:", e.message); log_channel = null; }
    if (log_channel !== null)
    {
        log_channel.onmessage = function (ev)
        {
            if (ev && ev.data) { for (var k in ev.data) { cfg[k] = ev.data[k]; } }
        };
    }
    cfg.set = function (patch)
    {
        for (var k in patch) { cfg[k] = patch[k]; }
        if (log_channel !== null) { try { log_channel.postMessage(patch); } catch (ee) { console.warn("failed to broadcast log config:", ee.message); } }
    };
    cfg.disable = function () { cfg.set({ enabled: false }); };
    cfg.enable  = function () { cfg.set({ enabled: true }); };
    cfg.setTime = function (on) { cfg.set({ time: on !== false }); };

    function pad(n, width)
    {
        var s = "" + n;
        while (s.length < width) { s = "0" + s; }
        return s;
    }

    function stamp()
    {
        var d = new Date();
        return pad(d.getHours(), 2) + ":" + pad(d.getMinutes(), 2) + ":" + pad(d.getSeconds(), 2) + "." + pad(d.getMilliseconds(), 3);
    }

    // pull the real call site (function + line) out of a fresh stack. the browser's own
    // file:line link would otherwise point here at the wrapper, so we reconstruct it in text.
    function origin()
    {
        var stack = "";
        try { throw new Error(); } catch (e) { stack = e.stack || ""; }

        var frames = [];
        var raw = stack.split("\n");
        for (var i = 0; i < raw.length; i++)
        {
            // drop chrome's leading "Error" header line; keep the actual frames
            if (raw[i].indexOf("Error") === 0 && raw[i].indexOf("@") === -1) { continue; }
            if (raw[i].length > 0) { frames.push(raw[i]); }
        }

        // frames[0] = origin(), frames[1] = the console wrapper, frames[2] = the real caller
        var caller = frames[2] || frames[frames.length - 1] || "";

        var fn = "anon";
        var m = caller.match(/at (?:async )?([^\s(]+)\s*\(/); // chrome: "    at fn (url:line:col)"
        if (m === null) { m = caller.match(/([^@\s]+)@/); }    // firefox: "fn@url:line:col"
        if (m !== null && m[1]) { fn = m[1]; }

        var line = "?";
        var lm = caller.match(/:(\d+):\d+\)?\s*$/);
        if (lm !== null) { line = lm[1]; }

        return fn + ":" + line;
    }

    function passes(level)
    {
        if (LEVELS[level] < LEVELS[cfg.level || "debug"]) { return false; }
        for (var i = 0; i < cfg.mute.length; i++)
        {
            if (cfg.mute[i] == context_name) { return false; }
        }
        return true;
    }

    function make(level, native_fn)
    {
        return function ()
        {
            if (cfg.enabled === false) { return; }                                 // global master switch, wins over everything
            if (cfg.raw) { return native_fn.apply(console, arguments); }
            if (passes(level) === false) { return; }

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
        var native_log   = console.log   ? console.log.bind(console)   : function () {};
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

// keys.js is embedded in template.html along with the other client files, and in the node bundle
// it is the key handling: the rsa keypair size rules, the diffie-hellman session keys, the aes message
// bodies and the metadata envelope of every frame, and the channel keys a maintainer hands out (with
// the wait timer that votes a silent maintainer out)
// workers.js does the same wire work inside the worker; messages.js calls the channel key side

// state private to this file
// the maintainer reset: whenever this client waits for keys, a timer is armed; keys that do not arrive
// in time send a reset_channel_maintainer vote (re-sent every timeout), and once more than half of the
// channel has voted the server deposes the maintainer and announces a new one
var MAINTAINER_KEYS_WAIT_TIMEOUT_MS = 5000;

var maintainer_keys_wait_timer = null;

var maintainer_keys_wait_channel_id = -1;

// ---- identity and wire crypto ----

/**
 * @brief the smallest key size we can create that satisfies the server
 *        a modulus may come out one bit short, so a server asking for exactly N is satisfied by our
 *        N; anything between the offered sizes rounds up to the next one we can actually generate
 *
 * @param number minimum_bits -> the size the server demands
 *
 * @return number the size to generate, 0 when no offered size is big enough
 */
function keys__pick_rsa_key_bits_for_minimum(minimum_bits)
{
    for (let i = 0; i < G_ALLOWED_RSA_KEY_BITS.length; i++)
    {
        if (G_ALLOWED_RSA_KEY_BITS[i] >= minimum_bits) { return G_ALLOWED_RSA_KEY_BITS[i]; }
    }
    return 0;
}

/**
 * @brief the server rejected our identity key for being too small and told us what it wants
 *        offers to switch this device to a big enough key and reconnect. the announcement is
 *        optional on the server: without it we are simply dropped and this never runs
 *
 * @param number|string announced_minimum_bits -> the size from the notice, treated as untrusted input
 *
 * @return void
 */
function keys__handle_rsa_key_too_weak_notice(announced_minimum_bits)
{
    let minimum_bits = parseInt(announced_minimum_bits);

    // the notice arrives before the handshake, so it is unauthenticated: treat the number as
    // untrusted input. nothing outside the range the server itself enforces is believable
    if (isNaN(minimum_bits) || minimum_bits < 2048 || minimum_bits > 8192)
    {
        console.warn("ignoring an implausible key size requirement from the server: " + announced_minimum_bits);
        return;
    }

    // we already generate a key this big, so the rejection was not really about size - say
    // nothing rather than offer a switch that would change nothing
    if (g_rsa_key_bits >= minimum_bits)
    {
        return;
    }

    let target_bits = keys__pick_rsa_key_bits_for_minimum(minimum_bits);

    if (target_bits == 0)
    {
        utils__custom_alert("this server requires an RSA key of " + minimum_bits + " bits, which this client cannot create");
        return;
    }

    // the driver redials on its own and the server rejects every attempt, so without this the
    // dialog would reopen in a loop
    if (g_rsa_key_too_weak_prompted_for_bits == minimum_bits)
    {
        return;
    }
    g_rsa_key_too_weak_prompted_for_bits = minimum_bits;

    document.getElementById("rsa-key-too-weak-text").textContent =
        "This server requires an identity key of at least " + minimum_bits + " bits. Your key is "
        + g_rsa_key_bits + " bits, so the server refused the connection. Switch this device to "
        + target_bits + "-bit keys and reconnect? Creating the key may take a while, and it gives "
        + "you a NEW identity here - the same passphrase produces a different key at a different size.";

    document.getElementById("rsa-key-too-weak-yes-button").setAttribute("data-target-bits", target_bits);
    document.getElementById("rsa-key-too-weak-container").style.display = "block";
    document.getElementById("background-container").style.display = "block";
}

/**
 * @brief hides the "stronger key required" dialog
 *
 * @return void
 */
function keys__hide_rsa_key_too_weak_dialog()
{
    document.getElementById("rsa-key-too-weak-container").style.display = "none";
    document.getElementById("background-container").style.display = "none";
}

/**
 * @brief rebuilds g_metadata_keys by sha256-hashing the predefined (or form-entered) key strings, then hands the key list to the data-processing worker
 *
 * @return void
 */
function keys__init_keys_object()
{
    g_metadata_keys.length = 0; // clear it in case client is reconnecting

    if (g_are_server_details_predefined == true)
    {
        for (let i = 0; i < g_autoconnect_details.keys.length; i++)
        {
            let key_string_value = g_autoconnect_details.keys[i];
            // removed: never log key material
            let hash = _sha256.create();

            hash.update(key_string_value);
            hash.array();
            key_bytes = hash.array();

            let single_key = {
                info: "aes-ctr",
                key_string: key_string_value,
                key_bytes: key_bytes
            };

            g_metadata_keys[i] = single_key;
        }
    }
    else
    {
        // iterate the key inputs that actually exist in the dom (in order), not a 0..count
        // range - keys can be removed, which leaves gaps in the input-key-N ids. this also
        // lets the user drop an accidental (even empty) key back to zero without a restart.
        let key_input_elements = document.querySelectorAll('#connect-form-sub-container-2 input[id^="input-key-"]');
        for (let i = 0; i < key_input_elements.length; i++)
        {
            let key_string_value = key_input_elements[i].value;
            // removed: never log key material

            let hash = _sha256.create();

            hash.update(key_string_value);
            hash.array();
            key_bytes = hash.array();

            let single_key = {
                info: "aes-ctr",
                key_string: key_string_value,
                key_bytes: key_bytes
            };

            g_metadata_keys[i] = single_key;
        }
    }

    g_data_processing_worker.postMessage({
        type: "mainthread__metadata_keys",
        value: g_metadata_keys
    });

    // removed: never log key material (g_metadata_keys holds the session key)
}

/**
 * @brief diffie-hellman first step: draws a fresh secret exponent into g_dh_secret_exponent
 *        the exponent comes from the cryptographically secure RNG; 256 bits for a 2048-bit modulus, else 512
 *
 * @return bigint the public mix g^x mod p
 */
function keys__get_public_mix()
{
    // the secret exponent: 256 bits for a 2048-bit modulus, else 512. an N-bit exponent gives N/2 bits
    // of security, ample against the modulus's own strength (112 to 192 bits); bigger only slows modPow
    let dh_exponent_bits = (dh_modulus_bits == 2048) ? 256 : 512;

    // draw the exponent from a cryptographically secure RNG. Math.random is a non-cryptographic PRNG
    // whose internal state is recoverable from a handful of outputs, so it must never make key material.
    let random_exponent_bytes = new Uint8Array(dh_exponent_bits / 8);
    crypto.getRandomValues(random_exponent_bytes);

    let random_exponent_binary_string = "0b";

    // dh_exponent_bits bits, first bit always 1 (full length), the rest from the secure RNG above
    for (let a = 0; a < dh_exponent_bits; a++)
    {
        if (a == 0)
        {
            random_exponent_binary_string += "1";
        }
        else
        {
            let single_random_bit_stringpart = ((random_exponent_bytes[a >> 3] >> (7 - (a & 7))) & 1) ? "1" : "0";
            random_exponent_binary_string += single_random_bit_stringpart;
        }
    }

    g_dh_secret_exponent = BigInt(random_exponent_binary_string);

    let A = lemon_crypto.modpow(g_dh_generator, g_dh_secret_exponent, g_dh_modulus);

    return A;
}

/**
 * @brief AES-CTR encrypts a byte buffer with every key layer in order
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes
 * @param array|Uint8Array data -> the bytes to encrypt
 *
 * @return Uint8Array the encrypted bytes
 */
function keys__encrypt_data_with_aes_keys(keys, data)
{
    let encrypted_data_in_current_iteration = new Uint8Array(data);

    for (let i = 0; i < keys.length; i++)
    {
        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        encrypted_data_in_current_iteration = aesCtrCustom.encrypt(encrypted_data_in_current_iteration);
    }
    return encrypted_data_in_current_iteration;
}

/**
 * @brief the reverse of keys__encrypt_data_with_aes_keys: applies the key layers in reverse order
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes
 * @param array|Uint8Array data -> the bytes to decrypt
 *
 * @return Uint8Array the decrypted bytes
 */
function keys__decrypt_data_with_aes_keys(keys, data)
{
    let current_data = new Uint8Array(data);

    for (let i = (keys.length - 1); i >= 0; i--)
    {
        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        current_data = aesCtrCustom.decrypt(current_data);
    }

    return current_data;
}

/**
 * @brief utf8 string -> zero-padded buffer (1024 bytes minimum) -> AES-CTR key layers -> base64
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes
 * @param string string_to_encrypt -> the plaintext
 *
 * @return string the base64 ciphertext
 */
function keys__encrypt_with_aes_keys_and_convert_to_base64(keys, string_to_encrypt)
{
    let byteArrayToSend = new Uint8Array(1024);
    if (string_to_encrypt.length > 1024)
    {
        byteArrayToSend = new Uint8Array(string_to_encrypt.length);
    }

    let textBytes = aesjs.utils.utf8.toBytes(string_to_encrypt);

    for (i = 0; i < textBytes.length; i++)
    {
        byteArrayToSend[i] = textBytes[i];
    }

    let base64EncryptedData = "";
    let encrypted_data_in_current_iteration = byteArrayToSend;

    for (let i = 0; i < keys.length; i++)
    {
        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        encrypted_data_in_current_iteration = aesCtrCustom.encrypt(encrypted_data_in_current_iteration);

        if (i + 1 == keys.length) // if end has been reached, convert encrypted bytes to base64string
        {
            base64EncryptedData = utils__bytesToBase64String(encrypted_data_in_current_iteration);
        }
    }
    return base64EncryptedData;
}

/**
 * @brief base64 -> AES-CTR key layers in reverse -> utf8 string cut at the first null terminator
 *        the encrypt side pads short messages up to 1024 bytes
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes
 * @param string string_to_decrypt -> the base64 ciphertext
 *
 * @return string the plaintext
 */
function keys__decrypt_base64_contents_with_aes_keys(keys, string_to_decrypt)
{
    let encrypted_data_in_current_iteration = utils__base64StringToBytes(string_to_decrypt);

    // decrypt by applying keys in reverse order
    for (let i = (keys.length - 1); i >= 0; i--)
    {
        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        encrypted_data_in_current_iteration = aesCtrCustom.decrypt(encrypted_data_in_current_iteration);
    }

    decryptedBytes = encrypted_data_in_current_iteration;

    // coerce to Uint8Array: when no cipher layer is applied (zero metadata keys) the value is still the
    // plain array from utils__base64StringToBytes, and TextDecoder.decode rejects a plain array
    let str1 = new TextDecoder().decode(new Uint8Array(decryptedBytes));
    let decrypted_metadata = utils__substringByNullTerminator(str1);
    return decrypted_metadata;
}

/**
 * @brief sha256 of a byte array, the primitive under the HKDF and HMAC below
 *        the DH metadata layer MUST match the server (mbedtls hkdf/md); js-sha256's built-in
 *        .hmac is broken in this build, so HMAC is hand-rolled on the plain hash
 *
 * @param array bytes -> the bytes to hash, plain or typed
 *
 * @return array the 32 hash bytes
 */
function keys___sha256_bytes(bytes)
{
    let h = _sha256.create();
    h.update(bytes);
    return h.array();
}

/**
 * @brief standard HMAC-SHA256 built on the plain hash
 *
 * @param array key_bytes -> the mac key, plain or typed
 * @param array msg_bytes -> the message, plain or typed
 *
 * @return array the 32 mac bytes
 */
function keys___hmac_sha256(key_bytes, msg_bytes)
{
    // normalize to plain arrays: aesjs.utils.utf8.toBytes returns a Uint8Array, which has no .push,
    // and Array.concat(typedArray) appends it as ONE element instead of spreading its bytes.
    let k = Array.from(key_bytes);
    let msg = Array.from(msg_bytes);
    if (k.length > 64) { k = keys___sha256_bytes(k); }
    while (k.length < 64) { k.push(0); }
    let ipad = k.map(b => b ^ 0x36);
    let opad = k.map(b => b ^ 0x5c);
    let inner = keys___sha256_bytes(ipad.concat(msg));
    return keys___sha256_bytes(opad.concat(inner));
}

/**
 * @brief HKDF-SHA256 extract + expand
 *
 * @param array ikm_bytes -> the input key material
 * @param array salt_bytes -> the salt
 * @param array info_bytes -> the context info
 * @param number length -> how many derived bytes to produce
 *
 * @return array the derived bytes as a plain array
 */
function keys___hkdf_sha256(ikm_bytes, salt_bytes, info_bytes, length)
{
    let info = Array.from(info_bytes); // plain array so t.concat(info) spreads the bytes
    let prk = keys___hmac_sha256(salt_bytes, ikm_bytes);
    let okm = [];
    let t = [];
    let counter = 1;
    while (okm.length < length)
    {
        t = keys___hmac_sha256(prk, t.concat(info).concat([counter & 0xff]));
        okm = okm.concat(t);
        counter++;
    }
    return okm.slice(0, length);
}

/**
 * @brief derives the DH layer's AES key and HMAC key from the shared-secret decimal string
 *        salt and info MUST match _base_internal__derive_dh_keys in base.c on the server
 *
 * @param string shared_secret_string -> the shared secret as a decimal string
 *
 * @return object { enc_key, mac_key }, 32 bytes each
 */
function keys__dh_derive_keys(shared_secret_string)
{
    let ikm = aesjs.utils.utf8.toBytes(shared_secret_string);
    let salt = aesjs.utils.utf8.toBytes("lemonchat-hkdf-salt-v1");
    let info = aesjs.utils.utf8.toBytes("lemonchat-dh-keys-v1");
    let okm = keys___hkdf_sha256(ikm, salt, info, 64);
    return { enc_key: okm.slice(0, 32), mac_key: okm.slice(32, 64) };
}

/**
 * @brief wraps an outgoing message for the wire
 *        pads, AES-CTR encrypts with every metadata key under a fresh random IV, base64s, and
 *        builds the { iv, data [, tag] } envelope; on the loopback (a ui-only runtime) the text
 *        stays plain, node wraps it toward the real server
 *
 * @param string string_to_encrypt -> the message json
 *
 * @return string the envelope json, or the plaintext itself on the loopback
 */
function keys__encrypt_all_message_data_and_convert_to_base64(string_to_encrypt)
{
    // one log line saying who encrypted what: a message must be wrapped exactly once (the webview
    // leaves it plain, node wraps it), and this shows a double-wrapped or bare one
    if (typeof utils__custom_log === "function" && string_to_encrypt.indexOf("direct_chat_message") !== -1)
    {
        utils__custom_log("[crypto] direct message, ui_only=" + android_host__is_ui_only_runtime()
            + " loopback_port=" + g_loopback_port + " -> " + (android_host__is_ui_only_runtime() ? "left plain" : "wrapped"));
    }

    // loopback stays plaintext on-device; node re-encrypts toward the real server
    if (android_host__is_ui_only_runtime())
    {
        return string_to_encrypt;
    }

    let byteArrayToSend = new Uint8Array(1024);
    if (string_to_encrypt.length > 1024)
    {
        byteArrayToSend = new Uint8Array(string_to_encrypt.length);
    }

    let textBytes = aesjs.utils.utf8.toBytes(string_to_encrypt);

    for (i = 0; i < textBytes.length; i++)
    {
        byteArrayToSend[i] = textBytes[i];
    }

    // fresh random IV per message (CSPRNG). the same IV seeds every metadata layer's AES-CTR counter;
    // it is not secret, so it travels as a separate "iv" field in the JSON envelope next to the ciphertext.
    let message_iv = new Uint8Array(16);
    crypto.getRandomValues(message_iv);

    let encrypted_data_in_current_iteration = byteArrayToSend;

    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        let key_bytes = g_metadata_keys[i].key_bytes;

        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(key_bytes, new aesjs.Counter(Array.from(message_iv)));
        encrypted_data_in_current_iteration = aesCtrCustom.encrypt(encrypted_data_in_current_iteration);
    }

    // base64 the result after all layers. with 0 metadata keys this is just the plaintext bytes,
    // matching the server which base64-decodes and applies no cipher when keys_count is 0
    let base64EncryptedData = utils__bytesToBase64String(encrypted_data_in_current_iteration);

    let iv_base64 = utils__bytesToBase64String(message_iv);
    let envelope = { iv: iv_base64, data: base64EncryptedData };

    // encrypt-then-MAC: if a metadata key carries an HMAC key (the DH layer), authenticate iv || data
    let dh_mac_key = null;
    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        if (g_metadata_keys[i].mac_bytes) { dh_mac_key = g_metadata_keys[i].mac_bytes; break; }
    }
    if (dh_mac_key)
    {
        let tag_bytes = keys___hmac_sha256(dh_mac_key, aesjs.utils.utf8.toBytes(iv_base64 + base64EncryptedData));
        envelope.tag = utils__bytesToBase64String(tag_bytes);
    }

    return JSON.stringify(envelope);
}

/**
 * @brief the reverse of keys__encrypt_all_message_data_and_convert_to_base64
 *        verifies the HMAC tag when the DH layer is active, then peels the metadata key layers
 *
 * @param string envelope_json -> the { iv, data [, tag] } envelope from the wire
 *
 * @return string the plaintext, "" on any failure
 */
function keys__decrypt_message_metadata(envelope_json)
{
    // the wire payload is a JSON envelope { "iv": <base64>, "data": <base64 ciphertext> }.
    // the per-message IV is public and seeds every metadata layer's AES-CTR counter.
    let envelope = null;
    try
    {
        envelope = JSON.parse(envelope_json);
    }
    catch (exception)
    {
        return "";
    }

    if (envelope == null || typeof envelope.iv != "string" || typeof envelope.data != "string")
    {
        return "";
    }

    // encrypt-then-MAC: if a metadata key carries an HMAC key (the DH layer), the message MUST carry a
    // valid tag. verify it over iv || data BEFORE decrypting; reject (return "") on a missing/bad tag.
    let dh_mac_key = null;
    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        if (g_metadata_keys[i].mac_bytes) { dh_mac_key = g_metadata_keys[i].mac_bytes; break; }
    }
    if (dh_mac_key)
    {
        if (typeof envelope.tag != "string") { return ""; }
        let expected_tag = utils__bytesToBase64String(keys___hmac_sha256(dh_mac_key, aesjs.utils.utf8.toBytes(envelope.iv + envelope.data)));
        if (expected_tag !== envelope.tag) { return ""; }
    }

    let message_iv = utils__base64StringToBytes(envelope.iv);
    let encrypted_data_in_current_iteration = utils__base64StringToBytes(envelope.data);

    for (let i = (g_metadata_keys.length - 1); i >= 0; i--)
    {
        let key_bytes = g_metadata_keys[i].key_bytes;

        let aesCtrCustom = new aesjs.ModeOfOperation.ctr(key_bytes, new aesjs.Counter(Array.from(message_iv)));
        encrypted_data_in_current_iteration = aesCtrCustom.decrypt(encrypted_data_in_current_iteration);
    }

    decryptedBytes = encrypted_data_in_current_iteration;

    // coerce to Uint8Array: when no cipher layer is applied (zero metadata keys) the value is still the
    // plain array from utils__base64StringToBytes, and TextDecoder.decode rejects a plain array
    let str1 = new TextDecoder().decode(new Uint8Array(decryptedBytes));
    let decrypted_metadata = utils__substringByNullTerminator(str1);
    return decrypted_metadata;
}

// ---- channel keys ----

/**
 * @brief arms (re-arms) the keys-wait timer for the current channel
 *        no-op when there is nothing to wait for: no maintainer announced, or the local user IS
 *        the maintainer; node owns this decision, the webview must not vote
 *
 * @return void
 */
function keys__arm_maintainer_keys_wait_timer()
{
    // node owns every reactive protocol decision, this one included. the webview voting
    // too churned the maintainer in a loop and invalidated the keys it had just applied
    if (android_host__is_ui_only_runtime())
    {
        return;
    }

    let channel_index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);

    keys__cancel_maintainer_keys_wait_timer();

    if (channel_index == -1)
    {
        return;
    }

    if (!g_channel_list[channel_index].has_maintainer)
    {
        return;
    }

    if (g_channel_list[channel_index].maintainer_id == g_local_client_id)
    {
        return;
    }

    maintainer_keys_wait_channel_id = g_current_channel_id;
    maintainer_keys_wait_timer = setTimeout(keys__maintainer_keys_wait_timer_fired, MAINTAINER_KEYS_WAIT_TIMEOUT_MS);
}

/**
 * @brief stops the pending keys-wait timer, if any, and clears maintainer_keys_wait_timer
 *
 * @return void
 */
function keys__cancel_maintainer_keys_wait_timer()
{
    if (maintainer_keys_wait_timer != null)
    {
        clearTimeout(maintainer_keys_wait_timer);
        maintainer_keys_wait_timer = null;
    }
}

/**
 * @brief the maintainer stayed silent for the whole timeout: sends the reset vote and re-arms
 *        every condition is re-checked, because a channel switch, arriving keys or a new
 *        maintainer may have happened meanwhile
 *
 * @return void
 */
function keys__maintainer_keys_wait_timer_fired()
{
    maintainer_keys_wait_timer = null;

    if (maintainer_keys_wait_channel_id != g_current_channel_id)
    {
        return;
    }

    if (g_current_channel_keys != null)
    {
        return;
    }

    let channel_index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);

    if (channel_index == -1)
    {
        return;
    }

    if (!g_channel_list[channel_index].has_maintainer)
    {
        return;
    }

    if (g_channel_list[channel_index].maintainer_id == g_local_client_id)
    {
        return;
    }

    console.log("%c no channel keys from maintainer (id " + g_channel_list[channel_index].maintainer_id + ") after " + MAINTAINER_KEYS_WAIT_TIMEOUT_MS + " ms, sending reset_channel_maintainer vote", "color: red");

    // payload-free: the client just asks for its channel's maintainer to be reset,
    // the server does all vote bookkeeping (one vote per client, dies on maintainer change)
    let message_object = {
        message: {
            type: "reset_channel_maintainer"
        }
    };

    connection__send_message_object(message_object);

    // keep voting while still keyless: the server counts at most one vote per client,
    // so repeats are harmless and cover a lost request on the way
    maintainer_keys_wait_timer = setTimeout(keys__maintainer_keys_wait_timer_fired, MAINTAINER_KEYS_WAIT_TIMEOUT_MS);
}

/**
 * @brief asks the data-processing worker to create a fresh channel key set and send it to every member, encrypted for each one's public key
 *        reactive protocol runs in ONE runtime, node: the webview reacting too sent a second key
 *        set on the same connection and the server kicked it
 *
 * @return void
 */
function keys__create_and_send_new_channel_keys()
{
    // reactive protocol runs in ONE runtime: node. the webview reacting too sent a
    // second key set on the same connection and the server kicked it
    if (android_host__is_ui_only_runtime())
    {
        return;
    }

    g_data_processing_worker.postMessage({
        type: "mainthread__create_websocket_channel_keys_message",
        clients: g_client_list,
        local_client_id: g_local_client_id,
        current_channel_id: g_current_channel_id
    });

    // rsa stays in the data-processing worker (the private key lives only there), so each public key
    // is posted to it and the encrypted keys come back for the main thread to route
}

// channel-tree.js is embedded in template.html along with the other client files, and in the node bundle
// it holds the lookups over g_channel_list, g_client_list, g_icons and g_tags, the html of a client row,
// and the avatars on those rows (the cache, the lazy loading queue and the prefetch after joining)
// messages.js and ui.js call it while rendering the tree

// state private to this file
var PREFETCH_AVATARS_AUTOMATICALLY = true;

var AVATAR_PREFETCH_INTERVAL_MS = 300;

// ---- lookups and the client row ----

/**
 * @brief icon object from g_icons by id
 *
 * @param number icon_id -> the icon id
 *
 * @return object|null the icon, null when not found
 */
function channel_tree__get_icon_by_icon_id(icon_id)
{
    for (var i = 0; i < g_icons.length; i++)
    {
        if (g_icons[i].id == icon_id)
        {
            return g_icons[i];
        }
    }
    return null;
}

/**
 * @brief tag object from g_tags by id
 *
 * @param number tag_id -> the tag id
 *
 * @return object|null the tag, null when not found
 */
function channel_tree__get_tag_by_tag_id(tag_id)
{
    for (var i = 0; i < g_tags.length; i++)
    {
        if (g_tags[i].tag_id == tag_id)
        {
            return g_tags[i];
        }
    }
    return null;
}

/**
 * @brief the direct children of a parent channel
 *        pass g_ROOT_LEVEL_PARENT_SENTINEL to get the root channels (those are matched by their
 *        is_root_channel flag, not by parent id)
 *
 * @param array channels -> the channel list to search
 * @param number parent_channel_id -> the parent, or the sentinel for the roots
 *
 * @return array the child channel objects
 */
function channel_tree__get_channels_by_channel_parent_id(channels, parent_channel_id)
{
    let result = [];
    for (var i = 0; i < channels.length; i++)
    {
        if (parent_channel_id == g_ROOT_LEVEL_PARENT_SENTINEL)
        {
            // root channels no longer carry a usable parent id (uint64 has no -1),
            // so match them by the explicit is_root_channel flag sent by the server
            if (channels[i].is_root_channel == true)
            {
                result.push(channels[i]);
            }
        }
        else if (channels[i].parent_channel_id == parent_channel_id)
        {
            result.push(channels[i]);
        }
    }
    return result;
}

/**
 * @brief channel object from the given channel list by id
 *
 * @param array channel_list_a -> the channel list to search
 * @param number channel_id -> the channel id
 *
 * @return object|null the channel, null when not found
 */
function channel_tree__get_channel_by_id(channel_list_a, channel_id)
{
    let result = null;

    for (let x = 0; x < channel_list_a.length; x++)
    {
        if (channel_list_a[x].channel_id == channel_id)
        {
            result = channel_list_a[x];
            break;
        }
    }

    return result;
}

/**
 * @brief index of a channel in the given channel list by id
 *
 * @param array channel_list_a -> the channel list to search
 * @param number channel_id -> the channel id
 *
 * @return number the index, -1 when not found
 */
function channel_tree__get_channel_index_in_array_by_channel_id(channel_list_a, channel_id)
{
    let result = -1;

    for (let x = 0; x < channel_list_a.length; x++)
    {
        if (channel_list_a[x].channel_id == channel_id)
        {
            result = x;
            break;
        }
    }

    return result;
}

/**
 * @brief depth of a channel in the tree, walked up the parent ids
 *
 * @param number channel_id -> the channel to measure
 * @param array channels -> the channel list to walk
 *
 * @return number 0 for a root; the depth reached so far when the parent chain is broken or cyclic (logged as an error)
 */
function channel_tree__get_indentation_level(channel_id, channels)
{
    let result = 0;
    let is_root_channel_found = false;
    let channel_id_currently_looking_for = channel_id;

    let safety_counter = 0;

    while (is_root_channel_found == false)
    {
        // a valid parent chain reaches a root in at most channels.length steps. if it does not,
        // the data is malformed (missing root or a cycle) - bail out instead of looping forever
        if (safety_counter > channels.length)
        {
            console.error("channel_tree__get_indentation_level: could not reach a root channel for channel_id " + channel_id + " - channel data looks invalid");
            return result;
        }
        safety_counter++;

        let was_channel_found = false;
        for (let i = 0; i < channels.length; i++)
        {
            if (channels[i].channel_id == channel_id_currently_looking_for)
            {
                was_channel_found = true;
                if (channels[i].is_root_channel == true)
                {
                    is_root_channel_found = true;
                }
                else
                {
                    result++;
                    channel_id_currently_looking_for = channels[i].parent_channel_id;
                }
                break;
            }
        }

        // the channel we are looking for is not in the list - the chain is broken, stop
        if (was_channel_found == false)
        {
            console.error("channel_tree__get_indentation_level: channel_id " + channel_id_currently_looking_for + " not found while walking parents - channel data looks invalid");
            return result;
        }
    }
    return result;
}

/**
 * @brief whether the local client carries the admin tag (tag id 0)
 *
 * @return boolean true for an admin
 */
function channel_tree__is_local_client_admin()
{
    let result = false;
    let client = channel_tree__get_client_by_client_id(g_local_client_id);
    if (client.tag_ids.includes(0))
    {
        result = true;
    }
    return result;
}

/**
 * @brief client object from g_client_list via the id -> index map
 *
 * @param number|string client_id -> the client id, a string is parsed to a number
 *
 * @return object|undefined the client, undefined when the id is not there
 */
function channel_tree__get_client_by_client_id(client_id)
{
    let actual_client_id = null;
    if (typeof client_id === "string")
    {
        actual_client_id = parseInt(client_id);
    }
    else
    {
        actual_client_id = client_id;
    }

    let result = null;

    let client_index = g_map_client_id_to_array_index.get(actual_client_id);
    result = g_client_list[client_index];

    return result;
}

/**
 * @brief index of a client in g_client_list via the id -> index map
 *        a map hands back undefined for a miss, which no -1 check catches, so the miss is
 *        turned into -1 here (and logged, since the map and the list then disagree)
 *
 * @param number client_id -> the client id, must be a number
 *
 * @return number the index, -1 when the id is not there
 */
function channel_tree__get_client_index_in_array_by_client_id(client_id)
{
    let index = g_map_client_id_to_array_index.get(client_id);

    // callers all check for -1, and a map hands back undefined instead. undefined == -1
    // is false, so the check passed and the array lookup threw one line later
    if (index === undefined)
    {
        // the map and the client list disagree. the list length says which way:
        // a non-empty list here means the two were cleared at different moments
        console.warn("client lookup miss: no entry for client_id " + client_id
            + ", client list holds " + g_client_list.length);

        return -1;
    }

    return index;
}

/**
 * @brief username for a client id
 *
 * @param number client_id -> the client id
 *
 * @return string|undefined the username, undefined when the client is not in g_client_list
 */
function channel_tree__get_username_by_client_id(client_id)
{
    let result = "";
    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i].client_id == client_id)
        {
            result = g_client_list[i].username;
            return result;
        }
    }
}

/**
 * @brief the name a person is labelled with: the admin-granted alias when there is one, else the username
 *        for anything the user reads, while channel_tree__get_username_by_client_id serves the protocol
 *
 * @param number client_id -> the client id
 * @param string fallback_username -> used when the client is not in the list
 *
 * @return string the alias, the username, the fallback, else ""
 */
function channel_tree__get_display_name_by_client_id(client_id, fallback_username)
{
    let client_object = channel_tree__get_client_by_client_id(client_id);

    if (client_object != null && typeof client_object.alias === "string" && client_object.alias.length > 0)
    {
        return client_object.alias;
    }

    if (client_object != null && typeof client_object.username === "string" && client_object.username.length > 0)
    {
        return client_object.username;
    }

    return (typeof fallback_username === "string") ? fallback_username : "";
}

/**
 * @brief public key string for a client id
 *
 * @param number client_id -> the client id
 *
 * @return string|undefined the public key, undefined when the client is not in g_client_list
 */
function channel_tree__get_public_key_by_client_id(client_id)
{
    let result = "";
    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i].client_id == client_id)
        {
            result = g_client_list[i].public_key;
            return result;
        }
    }
}

/**
 * @brief a CONNECTED client carrying this alias
 *        aliases are unique per identity, so this is how an offline conversation finds its owner
 *        once they come back
 *
 * @param string alias -> the alias, compared case-insensitively
 *
 * @return object|null the client, null when nobody connected carries it
 */
function channel_tree__get_client_by_alias(alias)
{
    if (typeof alias !== "string" || alias.length == 0) { return null; }

    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i] != null && typeof g_client_list[i].alias === "string" && g_client_list[i].alias.toLowerCase() == alias.toLowerCase())
        {
            return g_client_list[i];
        }
    }

    return null;
}

/**
 * @brief offline contact from g_offline_client_list by alias
 *
 * @param string alias -> the alias, compared case-insensitively
 *
 * @return object|null the stored client, null when not found
 */
function channel_tree__get_stored_client_by_alias(alias)
{
    if (typeof alias !== "string" || alias.length == 0) { return null; }

    for (let i = 0; i < g_offline_client_list.length; i++)
    {
        if (g_offline_client_list[i].alias.toLowerCase() == alias.toLowerCase())
        {
            return g_offline_client_list[i];
        }
    }

    return null;
}

// tree ordering helpers: the channel list is a flat pre-order list (a channel header, its client rows,
// then its subchannels' subtrees), so new items are inserted relative to these anchors

/**
 * @brief whether a channel is another channel itself or sits anywhere below it
 *
 * @param number child_channel_id -> the channel to test
 * @param number ancestor_channel_id -> the candidate ancestor
 *
 * @return boolean true when ancestor_channel_id is child_channel_id or one of its ancestors
 */
function channel_tree__is_channel_in_subtree_of(child_channel_id, ancestor_channel_id)
{
    let current = channel_tree__get_channel_by_id(g_channel_list, child_channel_id);
    let guard = 0;

    while (current != null && guard < 10000)
    {
        if (current.channel_id == ancestor_channel_id)
        {
            return true;
        }
        current = channel_tree__get_channel_by_id(g_channel_list, current.parent_channel_id);
        guard++;
    }

    return false;
}

/**
 * @brief the last DOM element belonging to a channel's whole subtree (its clients, its subchannels and their content)
 *        insert a new subchannel after this
 *
 * @param number channel_id -> the channel
 *
 * @return Element|null the last element, the channel header itself when the subtree is empty, null when the channel is not rendered
 */
function channel_tree__get_channel_subtree_last_element(channel_id)
{
    let header = document.querySelector('[data-channel-id="' + channel_id + '"]');
    let anchor = null;
    let node = null;

    if (header == null)
    {
        return null;
    }

    anchor = header;
    node = header.nextElementSibling;

    while (node != null)
    {
        let belongs = false;

        if (node.classList.contains("connected-client"))
        {
            let client = channel_tree__get_client_by_client_id(parseInt(node.getAttribute("data-connected-client-id")));
            if (client != null && channel_tree__is_channel_in_subtree_of(client.channel_id, channel_id))
            {
                belongs = true;
            }
        }
        else if (node.classList.contains("single-channel"))
        {
            if (channel_tree__is_channel_in_subtree_of(parseInt(node.getAttribute("data-channel-id")), channel_id))
            {
                belongs = true;
            }
        }

        if (belongs == false)
        {
            break;
        }

        anchor = node;
        node = node.nextElementSibling;
    }

    return anchor;
}

/**
 * @brief the last of a channel's own .connected-client rows (the contiguous run right after its header, before any subchannel)
 *        insert a joining client after this
 *
 * @param number channel_id -> the channel
 *
 * @return Element|null the last row, the header itself when the channel has no clients, null when the channel is not rendered
 */
function channel_tree__get_channel_own_clients_last_element(channel_id)
{
    let header = document.querySelector('[data-channel-id="' + channel_id + '"]');
    let anchor = null;
    let node = null;

    if (header == null)
    {
        return null;
    }

    anchor = header;
    node = header.nextElementSibling;

    while (node != null && node.classList.contains("connected-client"))
    {
        anchor = node;
        node = node.nextElementSibling;
    }

    return anchor;
}

/**
 * @brief the channel-tree row of one client: mic-state circle, name (an editable input for the local client), alias, flag, song marquee and tags, indented by channel depth
 *
 * @param object client -> the client from g_client_list
 * @param boolean is_local_client -> true for the local user, whose name is editable
 *
 * @return string the row html
 */
function channel_tree__generate_html_for_single_client(client, is_local_client)
{
    let result = "";
    let client_id = client.client_id;
    let channel_id_of_client = client.channel_id;
    let username = chat__sanitize_string(client.username);
    let alias_text = chat__sanitize_string((client.alias != null) ? client.alias : "");
    let has_alias_html_class = (alias_text.length > 0) ? " has-alias" : "";
    let audio_state = client.audio_state;
    let country_iso_code = client.country_iso_code;
    let is_music_bot = client.is_music_bot;
    // every theme hides this badge by default, so it needs the inline display to show at all.
    // painting it from state means a re-rendered row (channel join, idle) keeps its count
    // instead of silently resetting to 0, which is what the hardcoded markup used to do
    let unread_count = (typeof client.unread_count === "number") ? client.unread_count : 0;

    let client_audio_state_html_class = "client-audio-state-completely-disabled";
    switch (audio_state)
    {
        case 1:
            client_audio_state_html_class = "client-audio-state-microphone-active";
            break;

        case 2:
            client_audio_state_html_class = "client-audio-state-microphone-enabled";
            break;

        case 3:
            client_audio_state_html_class = "client-audio-state-microphone-disabled";
            break;

        case 4:
            client_audio_state_html_class = "client-audio-state-completely-disabled";
            break;
    }

    let additional_client_type_html_class = "";
    if (is_music_bot == true)
    {
        additional_client_type_html_class = "connected-client-p-music-bot";
    }

    let indentation_level = 1;

    if (channel_id_of_client != -2) // if channel is idle channel, (ID -2), do not run this function, program will enter infinite loop
    {
        indentation_level = channel_tree__get_indentation_level(channel_id_of_client, g_channel_list) + 1;
    }

    let is_channel_collapsed = "";
    if (channel_tree__is_channel_or_his_parent_collapsed(channel_id_of_client) == true)
    {
        is_channel_collapsed = "collapsed";
    }

    // the avatar is offered to the row as a css variable plus a marker, not painted: in most themes
    // that circle is the mic-state icon; a theme that wants faces (termix) reads var(--client-avatar)
    let avatar_inline_style = "";
    if (g_server_policy.allow_avatars == true && typeof client.base64_avatar === "string" && client.base64_avatar.length > 0)
    {
        avatar_inline_style = " data-has-avatar=\"1\" style=\"--client-avatar: url(" + client.base64_avatar + ");\"";
    }

    if (is_local_client)
    {
        result = "<div class=\"connected-local-client connected-client " + is_channel_collapsed + has_alias_html_class + "\" data-connected-client-id=\"" + client_id + "\">\n\
                    <div style=\"width: " + indentation_level * 20 + "px; height: 1px; vertical-align:top; display: inline-block;\">\n\
                    </div>\n\
                    <div class='client-audio-state "+ client_audio_state_html_class + "' id=\"client-audio-state-" + client_id + "\"" + avatar_inline_style + "><div class=\"client-audio-availability\"></div>\n\
                    </div>\n\
                    <input maxlength=\"40\" id=\"connected-local-client-input\" value=\"" + username + "\">\n\
                    <p class=\"client-alias\">" + alias_text + "</p>\n\
                    <div class=\"connected-client-country-flag country-flag-"+(country_iso_code ? (""+country_iso_code).toLowerCase() : "")+"\"></div>\n\
                    <div class=\"marquee-music-playing-container\" data-marquee-music-playing-container-id='"+ client_id + "'\>\n\
                        <p class='p-marquee-song-icon'>[</p>\n\
                        <div class=\"marquee\">\n\
                            <div id='marquee-song-name-client-id-"+ client_id + "'></div>\n\
                        </div>\n\
                        <p>]</p>\n\
                    </div>\n\
                    <div class='client-tags' id =\"client-tags-" + client_id + "\">\n\
                    </div>\n\
                </div>";
    }
    else
    {

        let client_mute_state_display = client.is_muted_by_local_client == true ? "block" : "none";
        let client_ignore_state_display = client.is_ignored_by_local_client == true ? "block" : "none";
        let is_idle_client_html = client.is_idle == true ? " idle-client" : "";
        let music_bot_client_html = client.is_music_bot == true ? " music-bot-client" : "";

        result = "<div class=\"connected-client " + is_channel_collapsed + " " + music_bot_client_html + " " + is_idle_client_html + has_alias_html_class + "\" data-connected-client-id=\"" + client_id + "\">\n\
                    <div style=\"width: " + indentation_level * 20 + "px; height: 1px; vertical-align:top; display: inline-block;\">\n\
                        <div class=\"client-mute-state\" style=\"display: "+client_mute_state_display+";\"></div>\n\
                    </div>\n\
                    <div class='client-audio-state "+ client_audio_state_html_class + "' id=\"client-audio-state-" + client_id + "\"" + avatar_inline_style + "><div class=\"client-audio-availability\"></div>\n\
                    </div>\n\
                    <p class=\"connected-client-p "+additional_client_type_html_class+"\">" + username + "</p>\n\
                    <p class=\"client-alias\">" + alias_text + "</p>\n\
                    <div class=\"connected-client-country-flag country-flag-"+(country_iso_code ? (""+country_iso_code).toLowerCase() : "")+"\"></div>\n\
                    <p class=\"connected-client-p-received-messages-number\" style=\"display: "+((unread_count > 0) ? "inline-block" : "none")+";\">"+unread_count+"</p>\n\
                    <div class=\"marquee-music-playing-container\" data-marquee-music-playing-container-id='"+ client_id + "'\>\n\
                        <p class='p-marquee-song-icon'>[</p>\n\
                        <div class=\"marquee\">\n\
                            <div id='marquee-song-name-client-id-"+ client_id + "'></div>\n\
                        </div>\n\
                        <p>]</p>\n\
                    </div>\n\
                    <div class='client-tags' id=\"client-tags-" + client_id + "\">\n\
                    </div>\n\
                    <svg class=\"client-ignore-state\" style=\"margin-left: "+indentation_level * 20+"px; display: "+client_ignore_state_display+";\"  viewBox=\"0 0 100 20\">\n\
                        <line x1=\"0\" y1=\"0\" x2=\"100\" y2=\"20\"></line>\n\
                        <line x1=\"0\" y1=\"20\" x2=\"100\" y2=\"0\"></line>\n\
                    </svg>\n\
                </div>";
    }

    return result;
}

/**
 * @brief whether the channel itself or any of its ancestors is collapsed (recursive walk up)
 *
 * @param number channel_id -> the channel
 *
 * @return boolean true when the channel is hidden by a collapse
 */
function channel_tree__is_channel_or_his_parent_collapsed(channel_id)
{
    let result = false;
    let channel_index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, channel_id);

    if (channel_index == -1)
    {
        return false;
    }

    if (g_channel_list[channel_index].is_channel_directly_collapsed == false)
    {
        result = channel_tree__is_channel_or_his_parent_collapsed(g_channel_list[channel_index].parent_channel_id);
    }
    else
    {
        result = true;
    }
    return result;
}

/**
 * @brief ids of every channel below the given one (fully recursive), used when collapsing
 *
 * @param number channel_to_find -> the channel whose subtree is collapsed
 *
 * @return array the channel ids below it
 */
function channel_tree__find_subchannels_of_channel_for_collapse(channel_to_find)
{
    let result = [];

    for (var i = 0; i < g_channel_list.length; i++)
    {
        if (g_channel_list[i].parent_channel_id == channel_to_find)
        {
            result.push(g_channel_list[i].channel_id);
            let result1 = channel_tree__find_subchannels_of_channel_for_collapse(g_channel_list[i].channel_id);
            result = result.concat(result1);
        }
    }

    return result;
}

/**
 * @brief ids of the channels below the given one, without descending into subchannels that are themselves directly collapsed (those stay folded when expanding)
 *
 * @param number channel_to_find -> the channel being expanded
 *
 * @return array the channel ids to show
 */
function channel_tree__find_subchannels_of_channel_to_expand(channel_to_find)
{
    let result = [];

    for (var i = 0; i < g_channel_list.length; i++)
    {
        if (g_channel_list[i].parent_channel_id == channel_to_find)
        {
            result.push(g_channel_list[i].channel_id);

            if (g_channel_list[i].is_channel_directly_collapsed == true)
            {
                console.log("skipping channel" + g_channel_list[i].name);
                continue;
            }
            else
            {
                console.log("trying to find subchannels of channel " + g_channel_list[i].name);
                let result1 = channel_tree__find_subchannels_of_channel_to_expand(g_channel_list[i].channel_id);
                result = result.concat(result1);
            }
        }
    }

    return result;
}

/**
 * @brief client ids of everybody sitting in any of the given channels
 *
 * @param array channel_ids -> the channel ids
 *
 * @return array the client ids
 */
function channel_tree__find_clients_in_channel(channel_ids)
{
    let result = [];

    for (var i = 0; i < g_client_list.length; i++)
    {
        if (channel_ids.includes(g_client_list[i].channel_id))
        {
            result.push(g_client_list[i].client_id);
        }
    }
    return result;
}

// ---- avatars ----

/**
 * @brief puts a client's avatar on everything that shows it: the client object, the tree row, the member list and the profile pane
 *        a variable plus a marker on the row, never a painted background, so a new or cleared
 *        avatar shows without waiting for the next full render
 *
 * @param number client_id -> the client
 * @param string|null base64 -> the avatar as a data url, empty or null to clear it
 *
 * @return void
 */
function channel_tree__apply_avatar_to_ui(client_id, base64)
{
    if (client_id === undefined || client_id === null) { return; }

    let has_avatar = (typeof base64 === "string" && base64.length > 0);

    // keep the avatar ON the client object in g_client_list, so it travels with the client and is
    // re-applied by avatar_inline_style_for_client on every tree render (channel switch etc.)
    let client_object = channel_tree__get_client_by_client_id(client_id);
    if (client_object != null) { client_object.base64_avatar = has_avatar ? base64 : null; }

    // offer it to the tree row the way the renderer does (a variable plus a marker, never a painted
    // background), so a new or cleared avatar shows without waiting for the next full render
    let tree_circle = document.getElementById("client-audio-state-" + client_id);
    if (tree_circle != null)
    {
        if (has_avatar == true && g_server_policy.allow_avatars == true)
        {
            tree_circle.style.setProperty("--client-avatar", "url(" + base64 + ")");
            tree_circle.setAttribute("data-has-avatar", "1");
        }
        else
        {
            tree_circle.style.removeProperty("--client-avatar");
            tree_circle.removeAttribute("data-has-avatar");
        }
    }

    // the member list is a clone of the tree, so repaint it too
    if (g_avatar.grid_visible == true) { UI.schedule_member_list_sync(); }

    // the big avatar in the right-pane profile, only if this is the client shown there
    if (parseInt(g_profile_avatar_client_id) === parseInt(client_id))
    {
        let big = document.getElementById("current-client-avatar");
        if (big != null)
        {
            if (has_avatar)
            {
                big.style.backgroundImage = "url(" + base64 + ")";
                big.style.backgroundSize = "100% 100%";

                // a picture replaces the letter that was standing in for it
                big.textContent = "";
                big.classList.remove("avatar-empty");
            }
            else
            {
                big.style.backgroundImage = "";
            }
        }
    }
}

/**
 * @brief the first letter, shown in the profile avatar circle while there is no picture in it
 *
 * @param string username -> the name the letter is taken from
 *
 * @return void
 */
function channel_tree__set_profile_avatar_monogram(username)
{
    let big = document.getElementById("current-client-avatar");

    if (big == null) { return; }

    big.textContent = (typeof username === "string" && username.length > 0) ? username.charAt(0) : "?";
    big.classList.add("avatar-empty");
}

/**
 * @brief the profile line under the name, built only from things the client list actually knows: you, music bot, channel, idle, alias, country, muted, ignored
 *
 * @param object client -> the client from g_client_list
 *
 * @return string the parts joined with " · "
 */
function channel_tree__describe_client_for_profile(client)
{
    let parts = [];

    if (client.client_id === g_local_client_id) { parts.push("you"); }
    if (client.is_music_bot == true) { parts.push("music bot"); }

    let channel = channel_tree__get_channel_by_id(g_channel_list, client.channel_id);

    if (channel != null) { parts.push("in " + channel.name); }

    if (client.is_idle == true) { parts.push("idle"); }
    if (client.alias != null && client.alias.length > 0) { parts.push("known as " + client.alias); }
    if (client.country_iso_code != null && client.country_iso_code.length > 0) { parts.push(client.country_iso_code.toUpperCase()); }
    if (client.is_muted_by_local_client == true) { parts.push("muted by you"); }
    if (client.is_ignored_by_local_client == true) { parts.push("ignored by you"); }

    return parts.join(" · ");
}

/**
 * @brief asks the server for one client's avatar; no-op when avatars are disabled
 *
 * @param number client_id -> the client
 *
 * @return void
 */
function channel_tree__request_single_avatar(client_id)
{
    if (g_server_policy.allow_avatars == false || client_id === undefined || client_id === null) { return; }
    let message_object = { message: { type: "request_avatar_for_client", client_id: parseInt(client_id) } };
    connection__send_message_object(message_object);
}

/**
 * @brief chunked lazy load for the avatar grid: enqueues every connected client id and pulls avatars in growing chunks (50, then 100, then 150)
 *        only the grid member list shows everybody at once; other themes fetch the clicked client's avatar alone
 *
 * @return void
 */
function channel_tree__enqueue_all_avatars_for_loading()
{
    // only bulk-load everyone's avatar for the avatar grid member list (shown all at once).
    // other themes only ever fetch the clicked client's avatar for the big right-pane.
    if (g_server_policy.allow_avatars == false || g_avatar.grid_visible == false) { return; }
    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i] != null) { g_avatar.load_queue.push(g_client_list[i].client_id); }
    }
    if (g_avatar.load_scheduled == false) { channel_tree__pump_avatar_load_queue(50); }
}

/**
 * @brief queues every connected person whose avatar we do not have yet and drains the queue one request per tick
 *        safe to call repeatedly: already-queued and already-known ids are skipped, the timer is
 *        started once; the strip themes bulk-load through channel_tree__enqueue_all_avatars_for_loading instead
 *
 * @return void
 */
function channel_tree__start_avatar_prefetch()
{
    if (PREFETCH_AVATARS_AUTOMATICALLY == false || g_server_policy.allow_avatars == false) { return; }

    // the strip themes (simpledark/bluebell) show everybody at once and already bulk-load
    // through channel_tree__enqueue_all_avatars_for_loading; prefetching on top would double the requests
    if (g_avatar.grid_visible == true) { return; }

    for (let i = 0; i < g_client_list.length; i++)
    {
        let client_object = g_client_list[i];

        if (client_object == null) { continue; }
        if (client_object.client_id == g_local_client_id) { continue; }
        if (client_object.is_music_bot == true) { continue; }
        if (typeof client_object.base64_avatar === "string" && client_object.base64_avatar.length > 0) { continue; }
        if (g_avatar.prefetch_queue.indexOf(client_object.client_id) != -1) { continue; }

        g_avatar.prefetch_queue.push(client_object.client_id);
    }

    if (g_avatar.prefetch_timer != null || g_avatar.prefetch_queue.length == 0) { return; }

    // one request per tick, so a room full of people never arrives as a burst
    g_avatar.prefetch_timer = setInterval(function()
    {
        if (g_server_policy.allow_avatars == false || g_avatar.prefetch_queue.length == 0)
        {
            channel_tree__stop_avatar_prefetch();
            return;
        }

        let client_id = g_avatar.prefetch_queue.shift();
        let client_object = channel_tree__get_client_by_client_id(client_id);

        // they left, or their avatar arrived some other way while waiting in the queue
        if (client_object == null) { return; }
        if (typeof client_object.base64_avatar === "string" && client_object.base64_avatar.length > 0) { return; }

        channel_tree__request_single_avatar(client_id);
    }, AVATAR_PREFETCH_INTERVAL_MS);
}

/**
 * @brief stops the prefetch drain timer and empties g_avatar.prefetch_queue
 *
 * @return void
 */
function channel_tree__stop_avatar_prefetch()
{
    if (g_avatar.prefetch_timer != null)
    {
        clearInterval(g_avatar.prefetch_timer);
        g_avatar.prefetch_timer = null;
    }

    g_avatar.prefetch_queue.length = 0;
}

/**
 * @brief drains g_avatar.load_queue: requests one chunk of avatars now, then re-schedules itself every 400 ms with a growing chunk size (max 150) until the queue is empty
 *
 * @param number chunk_size -> how many avatars this round asks for
 *
 * @return void
 */
function channel_tree__pump_avatar_load_queue(chunk_size)
{
    if (g_server_policy.allow_avatars == false || g_avatar.load_queue.length === 0)
    {
        g_avatar.load_scheduled = false;
        return;
    }

    let ids = g_avatar.load_queue.splice(0, chunk_size);
    let message_object = { message: { type: "request_avatars", client_ids: ids } };
    connection__send_message_object(message_object);

    if (g_avatar.load_queue.length > 0)
    {
        g_avatar.load_scheduled = true;
        let next_chunk = Math.min(chunk_size + 50, 150);
        setTimeout(function() { channel_tree__pump_avatar_load_queue(next_chunk); }, 400);
    }
    else
    {
        g_avatar.load_scheduled = false;
    }
}

// chat.js is embedded in template.html along with the other client files, and in the node bundle
// it is the chat side: composing and sending (a text, a picture or a file goes from the input to the
// worker for encryption and lands in a chat context), the typing indicator, and the read state of
// conversations (unread counters, badges, seen receipts)
// ui.js calls chat__send_chat_message and the typing side; messages.js and dispatch.js feed the incoming state

// state private to this file
var picture_delivery_pending = false; // between the server's upload ack and image_sent_status (relay to the receivers finished)

var picture_delivery_hide_timer = null;

var upload_ack_timer = null;

var UPLOAD_ACK_TIMEOUT_MS = 300000;   // a slow 10mb upload legitimately takes minutes

var TYPING_SEND_INTERVAL_MS = 3000;   // at most one message per 3s of continuous typing

var TYPING_EXPIRY_MS = 6000;          // twice the send interval, so one lost message does not flicker

// the private-message ids this client already receipted; each is sent once, only to its author
var seen_receipts_already_sent = {};

// which conversations have had our latest message read. one eye in the corner of the chat,
// not one on every bubble
var seen_state_by_context = {};

// private messages that arrived while their conversation was closed. a receipt is owed
// for each, and is sent when the user actually opens it
var unreceipted_messages_by_sender = {};

// ---- composing and sending ----

/**
 * @brief index of a chat context in g_chat_context_array
 *
 * @param string id -> the context id, "chat-context-channel-N", "chat-context-pm-N" or "chat-context-offline-<alias>"
 *
 * @return number the index, -1 when not found
 */
function chat__get_chat_context_index_by_chat_context_id(id)
{
    for (var i = 0; i < g_chat_context_array.length; i++)
    {
        if (g_chat_context_array[i].chat_context_id == id)
        {
            return i;
        }
    }
    return -1;
}

/**
 * @brief the state half of promoting an offline conversation into the live one, when its owner connects
 *        every decision (anything to promote, a live thread already there, were we reading it) is
 *        asked of g_chat_context_array, never of the dom; UI.promote_offline_chat_context_render
 *        then only moves the markup
 *
 * @param object client -> the connected client, whose alias names the offline context
 *
 * @return object { did_promote, had_live_context, was_open, offline_context_id, live_context_id }
 */
function chat__promote_offline_chat_context_state(client)
{
    let promotion = {
        did_promote: false,
        had_live_context: false,
        was_open: false,
        offline_context_id: "",
        live_context_id: ""
    };

    if (client == null || typeof client.alias !== "string" || client.alias.length == 0)
    {
        return promotion;
    }

    let offline_context_id = "chat-context-offline-" + client.alias;
    let live_context_id = "chat-context-pm-" + client.client_id;

    let offline_index = -1;
    let has_live_context = false;

    for (let i = 0; i < g_chat_context_array.length; i++)
    {
        if (g_chat_context_array[i].chat_context_id == offline_context_id)
        {
            offline_index = i;
        }

        if (g_chat_context_array[i].chat_context_id == live_context_id)
        {
            has_live_context = true;
        }
    }

    if (offline_index == -1)
    {
        return promotion;
    }

    promotion.did_promote = true;
    promotion.had_live_context = has_live_context;
    promotion.was_open = (g_current_chat_context_id == offline_context_id);
    promotion.offline_context_id = offline_context_id;
    promotion.live_context_id = live_context_id;

    if (has_live_context == false)
    {
        // no live thread yet: the offline one simply becomes it, history and all
        g_chat_context_array[offline_index].chat_context_id = live_context_id;
    }
    else
    {
        // both exist: the offline entry is folded into the live one and stops existing
        g_chat_context_array.splice(offline_index, 1);
    }

    // if we were reading or typing in it, keep sending live from now on
    if (promotion.was_open == true)
    {
        g_current_chat_context_id = live_context_id;
        g_chat_message_receiver_type = "user";
        g_chat_message_receiver_id = client.client_id;
        g_offline_chat_recipient_alias = "";
    }

    return promotion;
}

/**
 * @brief clears the upload lock and the progress overlay
 *        the parts loop must not do this itself: it only queues, so releasing there let a second
 *        upload start and wipe the server buffer of the first
 *
 * @return void
 */
function chat__release_file_upload_lock()
{
    if (upload_ack_timer != null)
    {
        clearTimeout(upload_ack_timer);
        upload_ack_timer = null;
    }

    document.getElementById("upload-progress-overlay").style.display = "none";
    document.getElementById("upload-progress-bar").style.width = '0%';
    document.getElementById("upload-progress-text").innerHTML = '0%';
    g_is_file_being_uploaded = false;
}

/**
 * @brief the upload ack arrived but the relay to the receivers is still running: says so instead of leaving the grayed thumbnail unexplained
 *        image_sent_status ends it, a two-minute timer is the fallback
 *
 * @return void
 */
function chat__show_picture_delivery_status()
{
    picture_delivery_pending = true;

    document.getElementById("upload-progress-overlay").style.display = "block";
    document.getElementById("upload-progress-bar").style.width = "100%";
    document.getElementById("upload-progress-text").innerHTML = "image received by server, being received by users ...";

    if (picture_delivery_hide_timer != null)
    {
        clearTimeout(picture_delivery_hide_timer);
    }

    picture_delivery_hide_timer = setTimeout(chat__hide_picture_delivery_status, 120000);
}

/**
 * @brief ends the picture delivery status; the overlay stays when a new upload already owns it
 *
 * @return void
 */
function chat__hide_picture_delivery_status()
{
    if (picture_delivery_hide_timer != null)
    {
        clearTimeout(picture_delivery_hide_timer);
        picture_delivery_hide_timer = null;
    }

    if (picture_delivery_pending == false)
    {
        return;
    }

    picture_delivery_pending = false;

    // a new upload may already own the overlay; leave it to that one then
    if (g_is_file_being_uploaded == false)
    {
        document.getElementById("upload-progress-overlay").style.display = "none";
        document.getElementById("upload-progress-bar").style.width = "0%";
        document.getElementById("upload-progress-text").innerHTML = "0%";
    }
}

/**
 * @brief whether an upload may start now, one at a time
 *        the intent globals are written before chat__send_file_by_parts runs its guard, so a
 *        refused upload relabelled the one already in flight; call sites ask here first and touch
 *        nothing on refusal
 *
 * @return boolean true when no upload is in flight
 */
function chat__can_start_file_upload()
{
    if (g_is_file_being_uploaded == true)
    {
        utils__custom_alert("cant upload more than 1 file at a time");
        return false;
    }

    return true;
}

/**
 * @brief uploads a file in parts, one every delay_ms, with the progress overlay; the lock stays until the server's ack
 *        a file_send_error mid-upload releases the lock, and the loop stops feeding a file the server dropped
 *
 * @param array parts -> the base64 parts
 * @param number total_bytes_length -> the whole file's length, sent with every part
 * @param number delay_ms -> the pause between parts
 *
 * @return void
 */
function chat__send_file_by_parts(parts, total_bytes_length, delay_ms) {

    if (g_is_file_being_uploaded == true)
    {
        utils__custom_alert("cant upload more than 1 file at a time");
        return;
    }

    let i = 0;
    document.getElementById("upload-progress-overlay").style.display = "block";

    function next_part_upload ()
    {
        // a file_send_error released the lock mid-upload: the server dropped this file, stop feeding it
        if (i > 0 && g_is_file_being_uploaded == false)
        {
            return;
        }

        g_is_file_being_uploaded = true;

        if (i >= parts.length)
        {
            // queued, not delivered. hold the lock until the server acks, with a timer so a lost
            // ack cannot block uploads for the rest of the session
            document.getElementById("upload-progress-text").innerHTML = "waiting for server ...";
            upload_ack_timer = setTimeout(chat__release_file_upload_lock, UPLOAD_ACK_TIMEOUT_MS);
            return;
        }

        const part = parts[i];

        // send

        client_msg.send_file_send_request(total_bytes_length, part, i);

        i++;

        let percent = i / (parts.length / 100);
        document.getElementById("upload-progress-bar").style.width = percent + '%';
        document.getElementById("upload-progress-text").innerHTML = Math.round(percent) + '%';

        setTimeout(next_part_upload, delay_ms);
    }

    next_part_upload();
}

/**
 * @brief sends the attached file as a message of its own; typed text stays in the box for the next send
 *        every way this can fail tells the user and keeps the file attached
 *
 * @param string receiver_type -> "channel" or "user"
 *
 * @return boolean true when the file was handed off
 */
function chat__send_pending_chat_file(receiver_type)
{
    if (g_pending_chat_file == null)
    {
        return false;
    }

    if (g_is_file_being_uploaded == true)
    {
        utils__custom_alert("cant upload more than 1 file at a time");
        return false;
    }

    let chat_context_index = chat__get_chat_context_index_by_chat_context_id(g_current_chat_context_id);

    if (chat_context_index == -1)
    {
        utils__custom_log("[send-file] STOP: no chat context for " + g_current_chat_context_id);
        return false;
    }

    let receiver_public_key = "";

    if (receiver_type == "user")
    {
        let receiver = channel_tree__get_client_by_client_id(g_chat_message_receiver_id);

        if (receiver == null)
        {
            utils__custom_alert("files can only be sent to people who are online");
            return false;
        }

        if (receiver.is_idle == true)
        {
            utils__custom_alert(chat__sanitize_string(channel_tree__get_display_name_by_client_id(g_chat_message_receiver_id, receiver.username)) + " is idle, files can not be delivered to idle clients");
            return false;
        }

        receiver_public_key = channel_tree__get_public_key_by_client_id(g_chat_message_receiver_id);

        if (receiver_public_key == "" || receiver_public_key == null)
        {
            utils__custom_alert("no public key for this user yet, try again in a moment");
            return false;
        }
    }
    else if (g_current_channel_keys == null)
    {
        utils__custom_alert("no channel keys from channel maintainer, cant send the file");
        return false;
    }

    let file = g_pending_chat_file;
    let key = "local-" + g_local_message_id;

    chat_files__clear_pending_chat_file();

    let element_count = document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer').length;

    for (let i = 0; i < element_count; i++)
    {
        document.getElementById(g_current_chat_context_id).getElementsByClassName('chat-spacer')[0].remove();
    }

    let html_to_append = "<div class=\"single-chat-message\">\n\
                            <div class=\"single-message-content\">\n\
                                <div class=\"single-chat-message-sender-local-username-container\">\n\
                                    " + android_host__generate_message_sender_html(g_local_client_id, g_local_username) + "\n\
                                </div>\n\
                                <div class=\"single-chat-message-sender-time\">\n\
                                    <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                </div>\n\
                                <div class=\"single-chat-message-content\">\n\
                                    " + chat_files__generate_chat_file_card_html({ key: key, name: file.name, size: file.size, is_local: true, local_message_id: g_local_message_id }) + "\n\
                                </div>\n\
                            </div>\n\
                        </div>";

    document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
    document.getElementById(g_current_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");
    chat__scroll_chat_to_end(true);

    g_chat_context_array[chat_context_index].last_known_message_sender_username = g_local_username;

    // our own copy is downloadable right away
    g_chat_files_by_message_id[key] = file;

    let card = chat_files__get_chat_file_card_by_key(key);

    if (card != null)
    {
        chat_files__wire_chat_file_download_button(card, key);
    }

    // lock now, not when the worker answers: encrypting 20 MB takes a moment and a second
    // send in that window must be refused instead of relabelling this one
    g_is_file_being_uploaded = true;
    document.getElementById("upload-progress-overlay").style.display = "block";
    document.getElementById("upload-progress-text").innerHTML = "encrypting ...";

    if (receiver_type == "user")
    {
        chat__clear_seen_indicator_for_client(g_chat_message_receiver_id);

        g_data_processing_worker.postMessage({
            type: "mainthread__create_direct_chat_file",
            file: file,
            receiver_public_key: receiver_public_key,
            chat_message_receiver_id: g_chat_message_receiver_id,
            local_message_id: g_local_message_id
        });
    }
    else
    {
        g_data_processing_worker.postMessage({
            type: "mainthread__create_channel_chat_file",
            file: file,
            current_channel_keys: g_current_channel_keys,
            current_channel_id: g_current_channel_id,
            local_message_id: g_local_message_id
        });
    }

    g_local_message_id++;
    return true;
}

/**
 * @brief the one send path for what sits in the input, for both targets
 *        "channel" is the current channel (keys already known), "user" the selected person
 *        (encrypted to their public key); a pending file goes first, on its own.
 *        chat__send_chat_message decides the target and the offline cases
 *
 * @param string target -> "channel" or "user"
 *
 * @return void
 */
function chat__send_composed_chat_message(target)
{
    if (g_pending_chat_file != null)
    {
        chat__send_pending_chat_file(target);
        return;
    }

    let text_input = document.getElementById("chat-input-container-text-input");
    let chat_message_to_send_value = text_input.value;
    text_input.value = "";

    if (g_base64_picture_string_to_send.length == 0 && chat_message_to_send_value.trim().length == 0)
    {
        return;
    }

    let chat_context_index = chat__get_chat_context_index_by_chat_context_id(g_current_chat_context_id);
    if (chat_context_index == -1)
    {
        utils__custom_log("[send] no chat context for " + g_current_chat_context_id);
        return;
    }

    let receiver_public_key = "";
    if (target == "user")
    {
        receiver_public_key = channel_tree__get_public_key_by_client_id(g_chat_message_receiver_id);
        if (receiver_public_key == "" || receiver_public_key == null)
        {
            utils__custom_log("[send] no public key for receiver " + g_chat_message_receiver_id);
            return;
        }

        // a new message of ours is unread by definition, so the eye goes until they read it
        chat__clear_seen_indicator_for_client(g_chat_message_receiver_id);
    }

    let chat_context = document.getElementById(g_current_chat_context_id);
    let sender_html = android_host__generate_message_sender_html(g_local_client_id, g_local_username);
    let time_html = new Date().toLocaleTimeString();

    if (g_base64_picture_string_to_send.length > 0)
    {
        if (chat__can_start_file_upload() == false)
        {
            return;
        }

        let picture_html = "<img class='imgnotsentyet local-client-chat-picture-img chat-picture-img' data-single-chat-message-local-message-id='" + g_local_message_id + "' data-single-chat-message-server-message-id='unspecified' src=" + g_base64_picture_string_to_send + "></img>";
        chat__append_local_chat_message_block(chat_context, sender_html, time_html, picture_html);

        document.getElementById("image-upload-preview").style.backgroundImage = "";
        document.getElementById("image-upload-preview").style.display = "none";

        // lock now, like the file path: encrypting a big picture takes a while and a second
        // send in that window must be refused, not relabel this one
        g_is_file_being_uploaded = true;
        document.getElementById("upload-progress-overlay").style.display = "block";
        document.getElementById("upload-progress-text").innerHTML = "processing image for send ...";

        if (target == "user")
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__create_direct_chat_picture",
                base64_picture_string_to_send: g_base64_picture_string_to_send,
                receiver_public_key: receiver_public_key,
                chat_message_receiver_id: g_chat_message_receiver_id,
                local_message_id: g_local_message_id
            });
        }
        else
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__create_channel_chat_picture",
                base64_picture_string_to_send: g_base64_picture_string_to_send,
                current_channel_keys: g_current_channel_keys,
                current_channel_id: g_current_channel_id,
                local_message_id: g_local_message_id
            });
        }

        g_base64_picture_string_to_send = "";
    }
    else
    {
        // the server never sees the font or color, they travel inside the encrypted body
        let actual_chat_message_to_send_value = JSON.stringify({
            font: g_selected_font,
            font_size: g_selected_font_size,
            font_color: g_selected_font_color,
            value: chat_message_to_send_value
        });

        let text_html = "<p class='single-chat-message-content-p local-single-chat-message-content-p " + g_selected_font + "' data-single-chat-message-local-message-id='" + g_local_message_id + "' data-single-chat-message-server-message-id='unspecified' style='color: " + g_selected_font_color + "; font-size: " + g_selected_font_size + "px;'>" + chat__sanitize_string(chat_message_to_send_value) + "</p>";

        if (g_chat_context_array[chat_context_index].last_known_message_sender_username == g_local_username)
        {
            // a follow-up line by the same sender joins their last block
            let blocks = chat_context.getElementsByClassName("single-chat-message");
            blocks[blocks.length - 1].getElementsByClassName("single-chat-message-content")[0].insertAdjacentHTML("beforeend", text_html);
        }
        else
        {
            chat__append_local_chat_message_block(chat_context, sender_html, time_html, text_html);
        }

        if (target == "user")
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__create_direct_chat_message",
                chat_message_value: actual_chat_message_to_send_value,
                receiver_public_key: receiver_public_key,
                chat_message_receiver_id: g_chat_message_receiver_id,
                local_message_id: g_local_message_id
            });
        }
        else
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__create_channel_chat_message",
                chat_message_to_send_value: actual_chat_message_to_send_value,
                current_channel_keys: g_current_channel_keys,
                current_channel_id: g_current_channel_id,
                local_message_id: g_local_message_id
            });
        }
    }

    chat__scroll_chat_to_end(true);
    g_chat_context_array[chat_context_index].last_known_message_sender_username = g_local_username;
    g_local_message_id++;
}

/**
 * @brief scrolls the chat to its newest message
 *        a received message only does so while the local "auto-scroll to the end" setting is on, so a
 *        person reading older messages is not yanked down; the user's own send or tab switch always does
 *
 * @param boolean is_forced -> true ignores the setting
 *
 * @return void
 */
function chat__scroll_chat_to_end(is_forced)
{
    if (is_forced != true && g_auto_scroll_chat_to_end == false)
    {
        return;
    }

    let container = document.getElementById("chat-context-container");

    if (container == null)
    {
        return;
    }

    container.scrollTop = container.scrollHeight;
}

/**
 * @brief a fresh message block of our own at the end of a chat context
 *        the spacer that keeps the bottom padding is dropped first and re-added last, so it stays the final element
 *
 * @param Element chat_context -> the context element
 * @param string sender_html -> the sender header
 * @param string time_html -> the time text
 * @param string content_html -> the message body
 *
 * @return void
 */
function chat__append_local_chat_message_block(chat_context, sender_html, time_html, content_html)
{
    let spacers = chat_context.getElementsByClassName("chat-spacer");
    while (spacers.length > 0)
    {
        spacers[0].remove();
    }

    chat_context.insertAdjacentHTML("beforeend",
        "<div class=\"single-chat-message\">\n\
            <div class=\"single-message-content\">\n\
                <div class=\"single-chat-message-sender-local-username-container\">\n\
                    " + sender_html + "\n\
                </div>\n\
                <div class=\"single-chat-message-sender-time\">\n\
                    <p>" + time_html + "</p>\n\
                </div>\n\
                <div class=\"single-chat-message-content\">\n\
                    " + content_html + "\n\
                </div>\n\
            </div>\n\
        </div>");
    chat_context.insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");
}

/**
 * @brief sends the input to the current channel
 *
 * @return void
 */
function chat__send_channel_chat_message()
{
    chat__send_composed_chat_message("channel");
}

/**
 * @brief sends the input (or the pending picture) to the selected client: the local echo is rendered, then the worker RSA+AES encrypts it for the receiver's public key
 *
 * @return void
 */
function chat__send_direct_chat_message()
{
    chat__send_composed_chat_message("user");
}

/**
 * @brief text to somebody who is not connected: the same encryption as a direct message (their public key came with the offline roster), addressed by alias
 *        the server parks it and hands it over when they return
 *
 * @param object offline_contact -> the entry of g_offline_client_list
 *
 * @return void
 */
function chat__send_offline_chat_message(offline_contact)
{
    let chat_message_to_send_value = document.getElementById('chat-input-container-text-input').value;
    document.getElementById('chat-input-container-text-input').value = "";

    if (chat_message_to_send_value.trim().length == 0)
    {
        return;
    }

    let context_id = "chat-context-offline-" + offline_contact.alias;

    let actual_data_to_send = {
        font: g_selected_font,
        font_size: g_selected_font_size,
        font_color: g_selected_font_color,
        value: chat_message_to_send_value
    };

    if (document.getElementById(context_id) != null)
    {
        let html_to_append = "<div class=\"single-chat-message\">\n\
                                <div class=\"single-message-content\">\n\
                                    <div class=\"single-chat-message-sender-local-username-container\">\n\
                                        " + android_host__generate_message_sender_html(g_local_client_id, g_local_username) + "\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-sender-time\">\n\
                                        <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-content\">\n\
                                        <p class='single-chat-message-content-p " + g_selected_font + "' style='color: " + g_selected_font_color + "; font-size: " + g_selected_font_size + "px;'>" + chat__sanitize_string(chat_message_to_send_value) + "</p>\n\
                                    </div>\n\
                                </div>\n\
                            </div>";

        document.getElementById(context_id).insertAdjacentHTML("beforeend", html_to_append);
        chat__scroll_chat_to_end(true);
    }

    g_data_processing_worker.postMessage({
        type: "mainthread__create_offline_chat_message",
        chat_message_value: JSON.stringify(actual_data_to_send),
        receiver_public_key: offline_contact.public_key,
        recipient_alias: offline_contact.alias
    });
}

/**
 * @brief the send-button entry point: routes to channel, direct or offline sending by the current receiver type, with fallbacks when the direct receiver went offline or came back
 *        every exit is logged, because a message that quietly goes nowhere is the hardest kind of bug to chase
 *
 * @return void
 */
function chat__send_chat_message()
{
    // EVERY exit from this function is logged. a message that quietly goes nowhere is
    // the hardest kind of bug to chase, and this path has several silent returns
    utils__custom_log("[send] type=" + g_chat_message_receiver_type
        + " receiver_id=" + g_chat_message_receiver_id + " (" + (typeof g_chat_message_receiver_id) + ")"
        + " context=" + g_current_chat_context_id);

    if (g_chat_message_receiver_type == "channel")
    {
        if (g_current_channel_keys == null)
        {
            utils__custom_log("[send] STOP: no channel keys");
            utils__custom_alert("no channel keys from channel maintainer, cant send the message");
            return;
        }
        chat__send_channel_chat_message();
    }
    else if (g_chat_message_receiver_type == "user")
    {
        if (channel_tree__get_client_by_client_id(g_chat_message_receiver_id) == null)
        {
            // nobody is there to decrypt a file, and the server keeps nothing this big
            if (g_pending_chat_file != null)
            {
                utils__custom_log("[send] STOP: file attached but the receiver is offline");
                utils__custom_alert("files can only be sent to people who are online");
                return;
            }

            utils__custom_log("[send] receiver not in client list, trying the offline paths"
                + " (alias='" + g_offline_chat_recipient_alias + "')");
            // they may have come online since this context was opened: then send it live instead
            // of queueing (the server refuses to queue for a connected client)
            let live_client = channel_tree__get_client_by_alias(g_offline_chat_recipient_alias);

            if (live_client != null)
            {
                // state first, markup second. routing this through UI.* alone would lose the
                // whole promotion in a runtime with no ui to paint
                let promotion = chat__promote_offline_chat_context_state(live_client);
                UI.promote_offline_chat_context_render(promotion, live_client);

                utils__custom_log("[send] promoted offline context to live, sending direct");
                chat__send_direct_chat_message();
                return;
            }

            let offline_contact = channel_tree__get_stored_client_by_alias(g_offline_chat_recipient_alias);

            if (offline_contact != null && typeof offline_contact.public_key === "string" && offline_contact.public_key.length > 0)
            {
                utils__custom_log("[send] sending as OFFLINE message");
                chat__send_offline_chat_message(offline_contact);
                return;
            }

            if (offline_contact != null)
            {
                // we know who they are but were never given their public key, so nothing
                // can be encrypted for them - the server does not hold messages here
                utils__custom_log("[send] STOP: known contact but no public key");
                utils__custom_alert("this server does not keep messages for people who are offline");
                return;
            }

            utils__custom_log("[send] STOP: receiver unknown and no offline contact");
            utils__custom_alert("this user is offline, the message can not be delivered");
            return;
        }

        utils__custom_log("[send] receiver found, going direct");
        chat__send_direct_chat_message();
    }
}

/**
 * @brief finishing an inline chat-message edit: locks the element again and sends the edit_chat_message_request with the new content to the server
 *
 * @param object event -> the blur event of the edited element
 *
 * @return void
 */
function chat__local_chat_message_onblur(event)
{
    event.target.setAttribute("contenteditable", "false");
    event.target.removeEventListener('blur', chat__local_chat_message_onblur);

    let message_object = {
        message: {
            type: "edit_chat_message_request",
            message_id: g_selected_server_chat_message_id,
            new_message_value: event.target.innerHTML,
            receiver_type: g_chat_message_receiver_type,
            receiver_id: parseInt(g_chat_message_receiver_id)
        }
    };

    connection__send_message_object(message_object);
}

/**
 * @brief html-entity escape for element text and quoted attribute values: < > & and both quotes
 *        not enough for unquoted attributes, javascript: urls, event handlers or script/style
 *        bodies, and not idempotent, so escape exactly once on the way into the dom; an <img> src
 *        takes chat__sanitize_image_data_url
 *
 * @param string str -> the text, anything else is coerced
 *
 * @return string the escaped text, "" for null or undefined
 */
function chat__sanitize_string(str)
{
    if (str === null || str === undefined)
    {
        return "";
    }
    str = "" + str; // coerce so a non-string (malformed/attacker field) cannot throw on .replace
    var map = {
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        '"': '&quot;',
        "'": '&#039;'
    };
    var new_string = str.replace(/[&<>"']/g, function (m)
    {
        return map[m];
    });
    return new_string;
}

/**
 * @brief the allowlist for a decrypted picture used as an <img> src: only a base64 image data url passes
 *        anything else (an external url that would beacon the viewer, a javascript: or html
 *        scheme, junk) becomes a blank placeholder; .src does not decode entities, so
 *        chat__sanitize_string is the wrong tool
 *
 * @param string str -> the candidate src
 *
 * @return string the src itself, else a 1x1 transparent png data url, "" for null or undefined
 */
function chat__sanitize_image_data_url(str)
{
    if (str === null || str === undefined)
    {
        return "";
    }
    str = "" + str;

    // data:image/<subtype>;base64,<base64 payload>
    if (/^data:image\/[a-zA-Z0-9.+-]+;base64,[A-Za-z0-9+/=\s]+$/.test(str) === true)
    {
        return str;
    }

    // 1x1 transparent png, shown in place of anything that is not a well-formed image data URL
    return "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+M8AAAMBAQDJ/pLvAAAAAElFTkSuQmCC";
}

/**
 * @brief strips the "data:...;base64," prefix
 *
 * @param string base64string -> the data url
 *
 * @return string the bare base64, the input unchanged when there is no comma
 */
function chat__remove_data_url_prefix_from_base64_string(base64string)
{
    const commaindex = base64string.indexOf(',');
    if (commaindex === -1)
    {
        return base64string;
    }
    else
    {
        return base64string.slice(commaindex + 1);
    }
}

/**
 * @brief splits a string into up to required_parts_count roughly equal slices
 *
 * @param string string_to_split -> the text
 * @param number required_parts_count -> how many slices
 *
 * @return array the slices
 */
function chat__split_string_into_smaller_parts(string_to_split, required_parts_count)
{
    const result = [];
    const partsize = Math.ceil(string_to_split.length / required_parts_count);

    let start = 0;

    while (start < string_to_split.length)
    {
        result.push(string_to_split.slice(start, start + partsize));
        start += partsize;
    }

    return result;
}

/**
 * @brief the file extension without the dot
 *
 * @param string filename -> the file name
 *
 * @return string the extension, "" when the name has none
 */
function chat__get_file_extension(filename)
{
    var ext = /^.+\.([^.]+)$/.exec(filename);
    return ext == null ? "" : ext[1];
}

// ---- typing indicator ----

/**
 * @brief tells whoever is on the other side of the open conversation that we are writing
 *        called on every keystroke but sends at most once every few seconds, so a long message
 *        is a handful of tiny messages, not one per character; carries no text, only where it belongs
 *
 * @return void
 */
function chat__send_typing_indicator()
{
    if (g_server_policy.allow_typing_indicator == false || g_is_authenticated == false)
    {
        return;
    }

    // an offline conversation has nobody to tell
    if (g_offline_chat_recipient_alias != null && g_offline_chat_recipient_alias.length > 0)
    {
        return;
    }

    let now = new Date().valueOf();
    if (now - g_typing.last_sent_at < TYPING_SEND_INTERVAL_MS)
    {
        return;
    }
    g_typing.last_sent_at = now;

    let receiver_id = (g_chat_message_receiver_type == "channel") ? g_current_channel_id : parseInt(g_chat_message_receiver_id);

    if (isNaN(receiver_id))
    {
        return;
    }

    connection__send_message_object({
        message: {
            type: "typing_indicator_request",
            receiver_type: g_chat_message_receiver_type,
            receiver_id: receiver_id
        }
    });
}

/**
 * @brief the chat context id a typing notice belongs to, matching the ids the contexts are built with, so the notice shows only while that conversation is on screen
 *
 * @param string receiver_type -> "channel" or "user"
 * @param number receiver_id -> the channel, or us
 * @param number sender_client_id -> who is typing
 *
 * @return string the context id
 */
function chat__get_chat_context_id_for_typing(receiver_type, receiver_id, sender_client_id)
{
    if (receiver_type == "channel")
    {
        return "chat-context-channel-" + receiver_id;
    }

    // he is writing to US, so the conversation is the one with HIM
    return "chat-context-pm-" + sender_client_id;
}

/**
 * @brief remembers that somebody is typing in a conversation and (re)starts the ticker that repaints and expires the notice
 *
 * @param number sender_client_id -> who is typing
 * @param string receiver_type -> "channel" or "user"
 * @param number receiver_id -> the channel, or us
 *
 * @return void
 */
function chat__note_typing_from_client(sender_client_id, receiver_type, receiver_id)
{
    let context_id = chat__get_chat_context_id_for_typing(receiver_type, receiver_id, sender_client_id);

    if (g_typing.state[context_id] == null)
    {
        g_typing.state[context_id] = {};
    }

    g_typing.state[context_id][sender_client_id] = new Date().valueOf() + TYPING_EXPIRY_MS;

    chat__render_typing_indicator();

    if (g_typing.render_timer == null)
    {
        g_typing.render_timer = window.setInterval(chat__render_typing_indicator, 1000);
    }
}

/**
 * @brief somebody's message arrived (or he left): whatever he was typing is over
 *
 * @param number sender_client_id -> the client
 *
 * @return void
 */
function chat__clear_typing_from_client(sender_client_id)
{
    for (let context_id in g_typing.state)
    {
        if (g_typing.state[context_id] != null)
        {
            delete g_typing.state[context_id][sender_client_id];
        }
    }

    chat__render_typing_indicator();
}

/**
 * @brief one typing line: the text, then three dots that animate on their own
 *        the dots are separate spans so css can fade them in one after another; rewriting the
 *        text on a timer would repaint the whole line several times a second
 *
 * @param string text_without_dots -> the "x is typing" text, set as textContent since a username is user-supplied
 *
 * @return Element the line
 */
function chat__build_typing_line(text_without_dots)
{
    let line = document.createElement("div");
    line.className = "typing-indicator-line";

    let label = document.createElement("span");
    // textContent, never innerHTML: a username is user-supplied
    label.textContent = text_without_dots;
    line.appendChild(label);

    let dots = document.createElement("span");
    dots.className = "typing-indicator-dots";

    for (let i = 0; i < 3; i++)
    {
        let dot = document.createElement("span");
        dot.textContent = ".";
        dots.appendChild(dot);
    }

    line.appendChild(dots);
    return line;
}

/**
 * @brief paints the "x is typing ..." line for the conversation on screen, drops entries that were not refreshed, and stops its own ticker once nothing is left to show
 *
 * @return void
 */
function chat__render_typing_indicator()
{
    let element = document.getElementById("typing-indicator-container");
    if (element == null)
    {
        return;
    }

    let now = new Date().valueOf();
    let names = [];
    let anything_left = false;

    for (let context_id in g_typing.state)
    {
        let typers = g_typing.state[context_id];
        if (typers == null) { continue; }

        for (let client_id in typers)
        {
            if (typers[client_id] <= now)
            {
                delete typers[client_id];
                continue;
            }

            anything_left = true;

            if (context_id == g_current_chat_context_id)
            {
                names.push(channel_tree__get_display_name_by_client_id(parseInt(client_id), ""));
            }
        }
    }

    // one line per person, stacked. capped at three so a busy channel cannot push the chat
    // upwards without end - the rest is counted on a final line
    element.textContent = "";

    if (names.length == 0)
    {
        element.style.display = "none";
    }
    else
    {
        let lines_to_show = (names.length > 3) ? 3 : names.length;

        for (let i = 0; i < lines_to_show; i++)
        {
            element.appendChild(chat__build_typing_line(names[i] + " is typing"));
        }

        if (names.length > lines_to_show)
        {
            element.appendChild(chat__build_typing_line("and " + (names.length - lines_to_show) + " others are typing"));
        }

        // "block", never "": clearing the inline style falls back to the stylesheet,
        // where this element is display:none - so it would never actually appear
        element.style.display = "block";
    }

    if (anything_left == false && g_typing.render_timer != null)
    {
        window.clearInterval(g_typing.render_timer);
        g_typing.render_timer = null;
    }
}

// ---- unread and seen ----

/**
 * @brief shows or hides the seen eye of the conversation on screen, from seen_state_by_context and the local setting
 *
 * @return void
 */
function chat__render_seen_indicator()
{
    let indicator = document.getElementById("chat-seen-indicator");

    if (indicator == null)
    {
        return;
    }

    let is_seen = (g_current_chat_context_id != null)
        && (seen_state_by_context[g_current_chat_context_id] == true)
        && (g_show_seen_indicator == true);

    indicator.classList.toggle("chat-seen-visible", is_seen);
}

/**
 * @brief we just wrote to this person, so our newest message is unread again
 *
 * @param number client_id -> the person
 *
 * @return void
 */
function chat__clear_seen_indicator_for_client(client_id)
{
    seen_state_by_context["chat-context-pm-" + client_id] = false;
    chat__render_seen_indicator();
}

/**
 * @brief the other side read our latest private message: the eye goes on for that conversation
 *
 * @param number sender_client_id -> who read it
 *
 * @return void
 */
function chat__mark_message_as_seen(sender_client_id)
{
    seen_state_by_context["chat-context-pm-" + sender_client_id] = true;
    chat__render_seen_indicator();
}

/**
 * @brief queues a private message that arrived while its conversation was closed; a receipt is owed for it and sent when the user opens the conversation
 *
 * @param number sender_client_id -> the author
 * @param number server_chat_message_id -> the message
 *
 * @return void
 */
function chat__remember_message_awaiting_receipt(sender_client_id, server_chat_message_id)
{
    if (server_chat_message_id == null || sender_client_id == g_local_client_id)
    {
        return;
    }

    if (unreceipted_messages_by_sender[sender_client_id] == null)
    {
        unreceipted_messages_by_sender[sender_client_id] = [];
    }

    unreceipted_messages_by_sender[sender_client_id].push(server_chat_message_id);
}

/**
 * @brief sends the receipts owed to one sender, once, when their conversation is opened
 *
 * @param number sender_client_id -> the author
 *
 * @return void
 */
function chat__send_pending_seen_receipts(sender_client_id)
{
    let pending = unreceipted_messages_by_sender[sender_client_id];

    if (pending == null)
    {
        return;
    }

    unreceipted_messages_by_sender[sender_client_id] = [];

    for (let x = 0; x < pending.length; x++)
    {
        chat__send_seen_receipt_for_message(sender_client_id, pending[x]);
    }
}

/**
 * @brief tells a sender we read their private message, at most once per message, only while somebody is looking and the local setting allows it
 *
 * @param number sender_client_id -> the author
 * @param number server_chat_message_id -> the message
 *
 * @return void
 */
function chat__send_seen_receipt_for_message(sender_client_id, server_chat_message_id)
{
    if (chat__is_the_user_actually_looking() == false)
    {
        return;
    }

    if (g_send_seen_receipts == false)
    {
        return;
    }

    if (server_chat_message_id == null || sender_client_id == g_local_client_id)
    {
        return;
    }

    if (seen_receipts_already_sent[server_chat_message_id] == true)
    {
        return;
    }

    let public_key = channel_tree__get_public_key_by_client_id(sender_client_id);

    if (public_key == null || public_key == "")
    {
        return;
    }

    seen_receipts_already_sent[server_chat_message_id] = true;

    g_data_processing_worker.postMessage({
        type: "mainthread__create_seen_receipt_message",
        receiver_id: sender_client_id,
        public_key: public_key,
        server_chat_message_id: server_chat_message_id
    });
}

// coming back to the app pays whatever was owed for the conversation on screen. without
// this a message read on return only receipted if the chat was clicked a second time
if (typeof document !== "undefined" && typeof document.addEventListener === "function")
{
    document.addEventListener("visibilitychange", function()
    {
        if (document.hidden === true || g_current_chat_context_id == null)
        {
            return;
        }

        if (g_current_chat_context_id.indexOf("chat-context-pm-") != 0)
        {
            return;
        }

        chat__send_pending_seen_receipts(parseInt(g_current_chat_context_id.split("chat-context-pm-")[1]));
    });
}

/**
 * @brief whether the screen is on and this page is in front of it
 *        headless node always answers no: nobody is looking at it, and if it answered yes every
 *        read would be receipted twice
 *
 * @return boolean true when the user can see the page
 */
function chat__is_the_user_actually_looking()
{
    if (typeof process !== "undefined")
    {
        return false;
    }

    if (typeof document !== "undefined" && document.hidden === true)
    {
        return false;
    }

    return true;
}

/**
 * @brief the unread count of a channel
 *
 * @param number channel_id -> the channel
 *
 * @return number the count, 0 when nothing is stored
 */
function chat__get_channel_unread_count(channel_id)
{
    let stored = g_channel_unread_counts[channel_id];
    return (typeof stored === "number") ? stored : 0;
}

/**
 * @brief counts one more unread message in a channel and repaints its badge
 *        the channel being read never accumulates a badge, but "being read" needs somebody
 *        looking; under node with no ui attached nobody is, so everything counts
 *
 * @param number channel_id -> the channel the message arrived in
 *
 * @return void
 */
function chat__increment_channel_unread_count(channel_id)
{
    // the channel being read never accumulates a badge, but "being read" needs somebody looking;
    // under node with no ui attached nobody is, so everything counts
    if (android_host__is_someone_watching_the_ui() == true
        && g_current_chat_context_id === ("chat-context-channel-" + channel_id))
    {
        return;
    }

    g_channel_unread_counts[channel_id] = chat__get_channel_unread_count(channel_id) + 1;
    chat__render_channel_unread_badge(channel_id);
    chat__update_app_unread_badge();
}

/**
 * @brief zeroes a channel's unread count and repaints its badge
 *
 * @param number channel_id -> the channel
 *
 * @return void
 */
function chat__clear_channel_unread_count(channel_id)
{
    g_channel_unread_counts[channel_id] = 0;
    chat__render_channel_unread_badge(channel_id);
    chat__update_app_unread_badge();
}

/**
 * @brief paints a channel's unread badge and mirrors it into the member list
 *        a class, not style.display: whether the badge shows at all is the theme's call (mobile
 *        themes only), and an inline display would override that decision
 *
 * @param number channel_id -> the channel
 *
 * @return void
 */
function chat__render_channel_unread_badge(channel_id)
{
    let badge = document.querySelector('[data-channel-unread-for="' + channel_id + '"]');

    if (badge == null)
    {
        return;
    }

    let count = chat__get_channel_unread_count(channel_id);
    badge.innerHTML = count;

    // a class, not style.display: whether the badge shows at all is the theme's call
    // (mobile themes only), and an inline display would override that decision
    if (count > 0)
    {
        badge.classList.remove("unread-empty");
    }
    else
    {
        badge.classList.add("unread-empty");
    }

    // the strip themes hide the channel tree and show a circle instead; the count
    // lives on that circle, which is rebuilt by the member-list mirror
    if (typeof UI !== "undefined" && typeof UI.schedule_member_list_sync === "function")
    {
        UI.schedule_member_list_sync();
    }
}

/**
 * @brief the launcher icon badge: every unread channel plus every unread private conversation
 *        android shows it through the notification count via the java bridge; node hands the
 *        number to its host, which keeps the badge working while the app is idle or closed
 *
 * @return void
 */
function chat__update_app_unread_badge()
{
    let total = 0;

    for (let channel_id in g_channel_unread_counts)
    {
        total += chat__get_channel_unread_count(channel_id);
    }

    for (let i = 0; i < g_client_list.length; i++)
    {
        if (typeof g_client_list[i].unread_count === "number")
        {
            total += g_client_list[i].unread_count;
        }
    }

    // the webview reports through the java bridge; node has no Android object and hands the number
    // to its host, which is what keeps the badge working while the app is idle or closed
    if (typeof Android !== "undefined" && typeof Android.JavaExportSetUnreadBadge === "function")
    {
        Android.JavaExportSetUnreadBadge(total);
    }
    else if (g_node_unread_listener != null)
    {
        g_node_unread_listener(total);
    }
}

/**
 * @brief the unread count of a private conversation
 *
 * @param number client_id -> the other person
 *
 * @return number the count, 0 when the client is unknown
 */
function chat__get_unread_count(client_id)
{
    let client_object = channel_tree__get_client_by_client_id(client_id);

    if (client_object == null || typeof client_object.unread_count !== "number")
    {
        return 0;
    }

    return client_object.unread_count;
}

/**
 * @brief counts one more unread private message from a client
 *
 * @param number client_id -> the author
 *
 * @return void
 */
function chat__increment_unread_count(client_id)
{
    let client_object = channel_tree__get_client_by_client_id(client_id);

    if (client_object != null)
    {
        client_object.unread_count = chat__get_unread_count(client_id) + 1;
    chat__update_app_unread_badge();
    }
}

/**
 * @brief opening a private conversation is reading it: the count goes to zero and the receipts owed are sent
 *
 * @param number client_id -> the other person
 *
 * @return void
 */
function chat__clear_unread_count(client_id)
{
    // opening it is reading it, so anything that arrived while it was closed is
    // receipted now
    chat__send_pending_seen_receipts(client_id);
    chat__render_seen_indicator();

    let client_object = channel_tree__get_client_by_client_id(client_id);

    if (client_object != null)
    {
        client_object.unread_count = 0;
    chat__update_app_unread_badge();
    }
}

/**
 * @brief paints a client row's unread badge from the state; visibility is a parameter, because one caller hides the badge without clearing it (a message grouped under the same sender)
 *
 * @param number client_id -> the client
 * @param boolean is_visible -> whether the badge shows
 *
 * @return void
 */
function chat__render_unread_badge(client_id, is_visible)
{
    let row = document.querySelector('[data-connected-client-id="' + client_id + '"]');

    if (row == null)
    {
        return;
    }

    let badge = row.getElementsByClassName("connected-client-p-received-messages-number")[0];

    if (badge == null)
    {
        return;
    }

    badge.style.display = (is_visible == true) ? "inline-block" : "none";
    badge.innerHTML = chat__get_unread_count(client_id);
}

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

// voice.js is embedded in template.html along with the other client files
// it is the voice side of the page: the microphone (push-to-talk, always-on), the webrtc datachannel
// to the server that carries audio both ways, and the music bot mp3 stream
// audio.js owns the sound graph and the workers; dispatch.js and ui.js call in here

// state private to this file
// set once the udp-capability probe has run (after the first failed datachannel attempt window)
var webrtc_udp_probe_done = false;

// push-to-talk release hangover: capture keeps running this long after the key is let go, so word
// tails ("hello" not "hell") ship before the mic-off message stops the relay
var PTT_RELEASE_HANGOVER_MS = 250;

var ptt_pending_stop_timer = null;

/**
 * @brief the one writer of g_is_microphone_always_on
 *        the side effects (mic, android bridge, repaint) stay in ui.js, so a headless runtime can
 *        own the flag without touching the microphone
 *
 * @param boolean is_active -> the new value
 *
 * @return void
 */
function voice__set_microphone_always_on(is_active)
{
    g_is_microphone_always_on = (is_active == true);

    // the android settings radio maps onto the new tap-to-toggle mode; a choice made in
    // the local settings panel (localStorage) outranks it
    let has_local_choice = false;
    has_local_choice = (utils__storage_get("lemon_continuous_mic") != null);

    if (has_local_choice == false)
    {
        g_is_continuous_mic_mode = g_is_microphone_always_on;
    }
}

/**
 * @brief the datachannel loop's sleep, resolvable early through g_datachannel_retry_sleep_resolve
 *
 * @param number ms -> the sleep in milliseconds
 *
 * @return promise resolves after the sleep or on the early wake
 */
function voice__datachannel_retry_sleep(ms)
{
    return new Promise(function(resolve)
    {
        let timer = setTimeout(function()
        {
            g_datachannel_retry_sleep_resolve = null;
            resolve();
        }, ms);

        g_datachannel_retry_sleep_resolve = function()
        {
            clearTimeout(timer);
            g_datachannel_retry_sleep_resolve = null;
            resolve();
        };
    });
}

/**
 * @brief push-to-talk press: enables the mic track, connects the recorder chain and reports microphone_usage=1 to the server
 *        no-op when the mic is off, forbidden or already sending; a re-press inside the release
 *        hangover cancels the pending stop
 *
 * @return void
 */
function voice__process_start_sending_audio()
{
    // a re-press inside the release hangover cancels the pending stop: the mic never
    // went down, so the early-outs below just keep the running capture untouched
    if (ptt_pending_stop_timer != null)
    {
        clearTimeout(ptt_pending_stop_timer);
        ptt_pending_stop_timer = null;
    }

    if (g_is_microphone_enabled == false)
    {
        return;
    }
    if (g_is_client_microphone_allowed_by_server == false)
    {
        return;
    }

    if (g_last_sent_value_microphone_usage == true)
    {
        return;
    }
    if (g_local_audio_stream == null)
    {
        return;
    }

    if (g_local_audio_stream.getTracks() == null)
    {
        return;
    }

    // spurt-start marker: receivers scrub this sender's decoder on the jump, pairing with
    // the encoder reset that the capture-start clear below performs
    g_voice_send_sequence_number = (g_voice_send_sequence_number + G_OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP) & 0xffff;

    audio__set_microphone_capture_active(true);
    voice__set_mic_transmitting_visual(true);

    g_local_audio_stream.getTracks()[0].enabled = true;

    try {
        g_audio_recorder_gain_node.connect(g_microphone_recorder);
    } catch (e) {
        console.log("audio_recorder_gain_node.connect" + e.message);
    }
    // g_microphone_recorder.connect(audio_context.destination);

    g_is_microphone_active = true;
    let message_object = {
        message: {
            type: "microphone_usage",
            value: 1,
        }
    };

    connection__send_message_object(message_object);
    g_last_sent_value_microphone_usage = 1;
}

/**
 * @brief the glow on the mic controls while actually transmitting, so the user knows the mic is hot
 *
 * @param boolean is_transmitting -> true while audio is going out
 *
 * @return void
 */
function voice__set_mic_transmitting_visual(is_transmitting)
{
    let button_ids = ["microphone-push-to-talk-button-touch-device", "microphone-always-broadcasting-audio-button", "toggle-microphone-label"];

    for (let i = 0; i < button_ids.length; i++)
    {
        let button = document.getElementById(button_ids[i]);

        if (button != null)
        {
            if (is_transmitting == true) { button.classList.add("mic-transmitting"); }
            else { button.classList.remove("mic-transmitting"); }
        }
    }
}

/**
 * @brief push-to-talk release: keeps capturing for a short hangover so the tail of the last word ships (and the mic-off message cannot outrun the final audio frames), then stops
 *
 * @return void
 */
function voice__process_stop_sending_audio()
{
    if (ptt_pending_stop_timer != null)
    {
        clearTimeout(ptt_pending_stop_timer);
    }

    ptt_pending_stop_timer = setTimeout(function()
    {
        ptt_pending_stop_timer = null;
        voice__process_stop_sending_audio_now();
    }, PTT_RELEASE_HANGOVER_MS);
}

/**
 * @brief the actual stop: disables the mic track (the capture graph stays wired) and reports microphone_usage=2 to the server
 *
 * @return void
 */
function voice__process_stop_sending_audio_now()
{
    // audio capture only exists in the webview; node crashed here on a missing symbol
    if (typeof audio__set_microphone_capture_active !== "function")
    {
        return;
    }

    audio__set_microphone_capture_active(false);
    voice__set_mic_transmitting_visual(false);

    if (g_is_microphone_enabled == false)
    {
        return;
    }
    if (g_is_client_microphone_allowed_by_server == false)
    {
        return;
    }

    if (g_local_audio_stream == null)
    {
        return;
    }

    if (g_local_audio_stream.getTracks() == null)
    {
        return;
    }

    // activate recorder by assigning function to onaudioprocess

    g_local_audio_stream.getTracks()[0].enabled = false;

    // no edge of the capture graph may ever be disconnected: an unpulled mic source freezes stale
    // speech in chromium's stream fifo, and the next press replays it. the worklet gate mutes alone

    g_is_microphone_active = false;
    let message_object = {
        message: {
            type: "microphone_usage",
            value: 2,
        }
    };

    connection__send_message_object(message_object);
    g_last_sent_value_microphone_usage = 2;

    // drop the encoder's half-filled frame, otherwise its stale samples lead the NEXT
    // transmission (a piece of the previous sentence replayed on the new press)
    g_opus_encoder_worker.postMessage({ type: "clear_opus_encoder_buffer" });
}

/**
 * @brief the udp probe, run once after a datachannel attempt failed
 *        a browser under the "disable non-proxied udp" policy yields no udp ice candidates at all,
 *        which the page cannot read but can infer, and then says so
 *
 * @return void
 */
function voice__probe_webrtc_udp_and_warn()
{
    if (webrtc_udp_probe_done == true)
    {
        return;
    }
    webrtc_udp_probe_done = true;

    try
    {
        let probe_pc = new RTCPeerConnection();
        let got_udp_candidate = false;

        probe_pc.onicecandidate = function(event)
        {
            if (event.candidate != null && event.candidate.candidate.toLowerCase().indexOf(" udp ") != -1)
            {
                got_udp_candidate = true;
            }
        };

        probe_pc.createDataChannel("udp-probe");
        probe_pc.createOffer().then(function(offer) { return probe_pc.setLocalDescription(offer); });

        window.setTimeout(function()
        {
            probe_pc.close();

            if (got_udp_candidate == false)
            {
                let reason = "audio data blocked: the browser refuses direct UDP - possibly a chrome://flags setting like 'disable non-proxied UDP'. this is just a UDP websocket to your server, never a connection to other users";
                console.warn(reason);
                utils__custom_log(reason);
                utils__custom_alert(reason);
            }
        }, 3000);
    }
    catch (probe_error)
    {
        console.warn("webrtc udp probe could not run: " + probe_error.message);
    }
}

/**
 * @brief the datachannel loop: whenever voice is allowed, the channel is down and the client is not idle, it creates a fresh peer connection and asks the server for a new datachannel, then sleeps and looks again
 *        an unordered, unreliable channel between this client and the server only, never between
 *        clients; node has no webrtc and leaves at once
 *
 * @param boolean is_this_reconnect -> true when a previous channel existed
 *
 * @return promise resolves when the loop ends
 */
async function voice__webrtc_datachannel_connection_check(is_this_reconnect = false)
{
    // voice lives in the webview only; node has no webrtc and must never enter this.
    // the caller set the running flag, so it must be dropped here too
    if (typeof RTCPeerConnection === "undefined")
    {
        g_is_webrtc_datachannel_check_running = false;
        return;
    }

    // an unordered, unreliable datachannel (maxRetransmits 0): a udp-like pipe between this client
    // and the server only, never between clients, so nothing here is peer to peer

    while (true)
    {
        console.log("voice__webrtc_datachannel_connection_check ran");

        if (g_is_voice_chat_allowed_by_server == true && g_is_webrtc_datachannel_connected == false && g_is_deep_idle == false)
        {
            // one throw here must not kill the loop: a dead loop leaves the running flag
            // stranded true, which blocks every future re-create until the app restarts
            try
            {
                voice__create_new_peer_connection_object_for_use(is_this_reconnect);

                let message_object = {
                    message: {
                        type: "create_new_webrtc_datachannel_connection",
                    }
                };

                connection__send_message_object(message_object);
            }
            catch (datachannel_attempt_error)
            {
                console.error("webrtc datachannel attempt failed: "
                    + (datachannel_attempt_error != null && datachannel_attempt_error.stack ? datachannel_attempt_error.stack : datachannel_attempt_error));
            }

            await voice__datachannel_retry_sleep(10000);

            // a full attempt window passed and the channel is still down: find out
            // whether the browser is even capable of direct udp before retrying forever
            if (g_is_webrtc_datachannel_connected == false && g_is_deep_idle == false)
            {
                voice__probe_webrtc_udp_and_warn();
            }

            // the server refused for a while (10 attempts never connected): wait it out here instead
            // of asking every 10 s; a new login resolves the sleep and the server counts afresh
            if (g_is_webrtc_datachannel_connected == false && g_webrtc_datachannel_cooldown_until_ms > new Date().valueOf())
            {
                let remaining_ms = g_webrtc_datachannel_cooldown_until_ms - new Date().valueOf();
                console.log("datachannel: server cooldown, next attempt in " + Math.ceil(remaining_ms / 1000) + " s");
                await voice__datachannel_retry_sleep(remaining_ms);
            }
        }
        else
        {
            console.log("voice__webrtc_datachannel_connection_check break");
            g_is_webrtc_datachannel_check_running = false;
            break;
        }
    }
}

/**
 * @brief ships the local webrtc sdp answer to the server
 *
 * @param string value -> the sdp answer
 *
 * @return void
 */
function voice__send_sdp_answer_to_server(value)
{
    let message_object = {
        message: {
            type: "sdp_answer",
            value: value
        }
    };

    console.log(message_object);

    connection__send_message_object(message_object);
}

/**
 * @brief replaces g_peer_connection_with_server with a fresh RTCPeerConnection wired to the webrtc handlers
 *        the replaced one is closed, since an abandoned-but-open pc kept its ice agent alive and
 *        hijacked state with zombie events; g_iceconfig carries the TURN server exactly once
 *
 * @param boolean is_this_reconnect -> true when a previous channel existed
 *
 * @return void
 */
function voice__create_new_peer_connection_object_for_use(is_this_reconnect = false)
{
    // close the replaced pc: an abandoned-but-open pc kept its ice agent and its
    // 'datachannel' listener alive, hijacking state with zombie events and slowly
    // eating into the browser's peer connection cap
    if (g_peer_connection_with_server != null)
    {
        try
        {
            g_peer_connection_with_server.removeEventListener('datachannel', webrtc.peerconnection_on_datachannel_receive);
            g_peer_connection_with_server.close();
        }
        catch (peer_connection_close_error)
        {
            console.log("closing replaced peer connection failed: " + peer_connection_close_error.message);
        }
    }

    g_datachannel = null;
    g_peer_connection_with_server = null;

    // added when missing rather than keyed on is_this_reconnect: the audio_enabled handler
    // rebuilds g_iceconfig without it, and retries used to push a duplicate every 10s
    let is_turn_server_present = false;

    for (let i = 0; i < g_iceconfig.iceServers.length; i++)
    {
        let ice_server_urls = g_iceconfig.iceServers[i].urls;

        if ((typeof ice_server_urls === "string" && ice_server_urls.indexOf("turn:") == 0)
            || (Array.isArray(ice_server_urls) && ice_server_urls.length > 0 && String(ice_server_urls[0]).indexOf("turn:") == 0))
        {
            is_turn_server_present = true;
            break;
        }
    }

    if (is_turn_server_present == false)
    {
        let turn_server = {
            urls: [
                "turn:"+g_host+":3478?transport=udp",
                "turn:"+g_host+":3478?transport=tcp"
            ],
            username: "usweger123",
            credential: "pw1wegweg23Q"
        };

        g_iceconfig.iceServers.push(turn_server);
    }

    g_peer_connection_with_server = new RTCPeerConnection(g_iceconfig);
    g_peer_connection_with_server.onicecandidate = webrtc.peerconnection_handle_ice_candidate_event;
    g_peer_connection_with_server.onsignalingstatechange = webrtc.peerconnection_handle_signaling_state_change_event;
    g_peer_connection_with_server.onicegatheringstatechange = webrtc.peerconnection_handle_ice_gathering_state_change_event;
    g_peer_connection_with_server.oniceconnectionstatechange = webrtc.peerconnection_handle_ice_connection_state_change_event;
    g_peer_connection_with_server.onnegotiationneeded = webrtc.peerconnection_handle_negotiation_needed_event;
    g_peer_connection_with_server.onconnectionstatechange = webrtc.peer_connection_handle_onconnection_state_change_event;
    g_peer_connection_with_server.addEventListener('datachannel', webrtc.peerconnection_on_datachannel_receive);
}

/**
 * @brief shows or hides the floating push-to-talk button: touch devices only, never over the connect page, and not when hidden in the local settings
 *
 * @return void
 */
function voice__update_microphone_button()
{
    let button = document.getElementById("microphone-push-to-talk-button-touch-device");

    if (button == null)
    {
        return;
    }

    if (g_is_client_running_under_touch_device == false || g_hide_microphone_button == true)
    {
        button.style.display = "none";
        return;
    }

    // the connect screen has nobody to talk to, so the mic has no business floating over it
    let connect_page = document.getElementById("verification-system");

    if (connect_page != null && connect_page.style.display != "none")
    {
        button.style.display = "none";
        return;
    }

    button.style.display = "block";
    button.classList.toggle("mic-unavailable", g_is_microphone_available == false);
}

/**
 * @brief requests getUserMedia (with a plain-http explainer when unavailable), builds the mic capture chain once, and reports microphone_usage=2 (enabled, not sending) to the server
 *        a chosen input device rides along as "ideal", so an unplugged device falls back to the default instead of failing
 *
 * @return void
 */
function voice__activate_microphone()
{
    // getUserMedia exists only in a secure context (https, localhost or a file:// page); over plain
    // http from a remote host navigator.mediaDevices is undefined, so say so instead of throwing
    if (navigator.mediaDevices == null || typeof navigator.mediaDevices.getUserMedia != "function")
    {
        utils__custom_alert("the microphone needs a secure connection: open the client over HTTPS, from localhost, or as a local file. you are most likely loading it over plain HTTP.");
        return;
    }

    let microphone_constraints = { audio: true };

    // a chosen input device rides along as "ideal", because "exact" would fail the whole mic
    // activation when that device is unplugged - the browser then falls back to its default
    if (g_selected_microphone_device_id != "")
    {
        microphone_constraints = { audio: { deviceId: { ideal: g_selected_microphone_device_id } } };
    }

    navigator.mediaDevices.getUserMedia(microphone_constraints)
    .then(
        function (stream)
        {
            if (g_alert_push_to_talk_key_shown_once == false)
            {
                // only display the alert if touch device is not used
                if (!g_is_client_running_under_touch_device)
                {
                    utils__custom_alert("press Q to talk");
                }
                g_alert_push_to_talk_key_shown_once = true;
            }

            document.getElementById("play-pause-song-container").style.visibility = "visible";
            document.getElementById("stop-song").style.display = "none";
            document.getElementById("custom-file-upload-button-song").style.visibility = "visible";
            document.getElementById("custom-file-upload-button-song").style.display = "block";

            // only do this once
            if (g_microphone_recorder == null)
            {
                g_opus_encoder_worker.postMessage({
                    type: "init",
                    sampleRate: g_audio_context.sampleRate
                });

                stream.getTracks()[0].enabled = false;

                g_local_audio_stream = stream;
                g_audio_input = g_audio_context.createMediaStreamSource(stream);
                g_audio_recorder_gain_node = g_audio_context.createGain();

                // AudioWorkletNode capture when the worklet module is available, ScriptProcessorNode
                // otherwise; also wires mic -> gain -> capture node -> destination
                audio__create_microphone_capture_node();

                const audioTracks = stream.getAudioTracks();

                utils__custom_log('Using audio device: ' + audioTracks[0].label);
                stream.oninactive = function ()
                {
                    utils__custom_log('Stream ended');
                };
            }

            g_is_microphone_enabled = true;

            document.getElementById("play-pause-song-container").style.visibility = "visible";
            document.getElementById("toggle-microphone-label").style.backgroundImage = "url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAE30lEQVR4Xu2ae+jfUxjHX2MyImxJ5hKSrLX5T/xpNZeUkcsfiCSstTFrbRTij03mtpFEolzKJffLpqTUUlptjBqR27BFsdxyp3c7nzy/0/f3+5znfM75RPuc/z7f33Oe5znv53p+55lEv+twYB5wOnAEcGgQ/wXwKfAS8DywtS+1JvUk6BDgBuBSYPcWmX8BTwPLAihVVewDgDOBR4B9nCf5AbgQeMG5z0VeG4CrgDuA3Vxa/Ussb7gauCtzf+u2mgDI8nLl3MM3yguEs2p5Qi0AlNy2ZLj9eBZTOBwLfNVqUidBLQAeBC5x6tJG/gBwWRuR9+81AFCp+zgh23t1/TOUTpXMYqsGAFcCa4ppOJbRQuCekrxrALAWOLWkkobXK6GJKsa+BgAfAkcX03Asow9CMizGvgYAytjepif1QD8C+6YSp9DVAODvFMEdaIrqXJRZONQAQAfrpmwtarSizAYP2InAEAIpftyBpqjXFmU2hMAQAkMOGJLgUAV2gTIY1/m4kvTdB7TpM2HFzSmDbQIHADo0OSlb2zzOZVQX8TiNTptCKYfy0LTJc53JRTwAMLrTa7OIx7optG3yXEZ1EQ8e8N/zgMnA75HbuIzqIg6CfgP2MEKnAL+a75r/FP0e2M/IOjh6LpNue6bEUUOTA8B3wP5GyEHA1+Zb/7o+xqOEg/Z9YIahnw28Y76/BaY5+JEDwHvATCPkBOAt893nw4heoJ81st8FBEryygFAAiW4WVcA95tvPV/dnayBj3ABcK/ZshrQDEKzngHO9rDMAeBaYKUR8hRwnvk+DPikwuPoH8CRgH0cfRs4zsheDqyqDcCJwJtGiBLTdOAn85uesjUPVHLdB8w3DBWGcnlrxOOBDR6hOR6giQ9NcenQ44WBhqI0IFHqGUsga0Bim5EpQC433/IMPc27LmM5AEimJr5uMsI1D6DsrDLUrNOAFwuEgkZklHPEq1kqfx8Be5vfrgNWeKwv2lwApgKfRY+g1wC3RApoVuDODnNCOvziEUn1SeBcI0u9h6y/oy8AJOd2YIkRqGZIMbg5UuIM4NGMcJDbXxCGJy1LDVo+F8m4NcwVes+f7QESpGZIh1XWb5amPZUkt0eaHAjIRVXG1L5OtGT1xwBldBvz2jMLeAM4wDD4MvyuBs29ckOgETQXeDUKJYFyyggQtEfTY82orEqaHZVVHnk5jMONmgM6ClgPKP6bpYR3MvCa++RhQ1cAxEYxrji1S4c5B9iUq1i0bw7wOCBPskuybRi6xZUAQLO/DwPnR9J/AW4MANnq4FFyL2BpqDpx6ChMLgY0PZa9SgAg4VJOCtmOsFFK5eq2MC/8c6KmKm8XAddH/Uaz/YmQIDsdXsxKAdCAoDosi40aj9V8jy5KrwMbQzP1Tbi+aqZIOUEXGY3SK7fI+vFSglSrK2DUGndeJQFolFG8ajrcdoqdFQ1Xbrn8uhLMGh41ABBv3cmVnBZl1P/4fOoHdLvU1Lnu+0VXLQAaJdUx6rqquWHbL6Qc4nPgoTAqX/zgtT0gPqCAVpd4EnBzy+l13Vae0K3OdbFJQXWUYjn7uuxpO1Rtrxyje6/CguQBgBb36dUovQobPGAnAkMIDCEwMQK9hmWvwnaVHNAW4116CO0tarSizBItPADQEYGiRivK7P/oAf8Ae0zZQdfLrKYAAAAASUVORK5CYII=)";

            let message_object = {
                message: {
                    type: "microphone_usage",
                    value: 2,
                }
            };

            let message_json_string = connection__process_message_before_sending(message_object);
            let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

            connection__websocket_worker_send(data);

            g_last_sent_value_microphone_usage = 2;

            if (g_is_client_running_under_touch_device)
            {
                g_is_microphone_enabled_on_touch_device = true;
            }

            // legacy always-on used to hide the button and open the mic right here;
            // continuous mode is a tap-toggle now, so the button stays and stays quiet

            // desktop continuous armed the flag before getUserMedia resolved, so the
            // start call found g_is_microphone_enabled false and returned silently
            if (g_is_continuous_transmission_active == true)
            {
                voice__process_start_sending_audio();
            }

        },
        function (fail) {
            console.log("voice__activate_microphone is acting weird");
            console.log(fail);
        }
    )
    .catch(function (error)
    {
        const errorMessage = 'navigator.MediaDevices.getUserMedia error: ' + error.message + ' ' + error.name;
        console.log(errorMessage);
        document.getElementById("toggle-microphone").checked = false;
    });
}

/**
 * @brief applies a client's new audio state: swaps the mic-state css class on their tree row, toggles the local push-to-talk controls, and shows the song marquee when streaming
 *
 * @param object client -> the client_id and audio_state from the server
 *
 * @return void
 */
function voice__process_audio_state_of_single_client(client)
{

    let target_element = document.getElementById("client-audio-state-" + client.client_id);

    // an audio state can arrive for a client this runtime has not listed yet; the lookup
    // returns -1 and g_client_list[-1] is undefined, which threw out of the handler
    let client_index = channel_tree__get_client_index_in_array_by_client_id(client.client_id);

    if (client_index == -1 || target_element == null)
    {
        return;
    }

    g_client_list[client_index].audio_state = client.audio_state;

    if (client.audio_state == G_AUDIO_STATE.PUSH_TO_TALK_ACTIVE)
    {

        if (target_element.classList.contains("client-audio-state-microphone-enabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-enabled");
        }

        if (target_element.classList.contains("client-audio-state-microphone-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-disabled");
        }

        if (target_element.classList.contains("client-audio-state-completely-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-completely-disabled");
        }

        document.getElementById("client-audio-state-" + client.client_id).classList.add("client-audio-state-microphone-active");

    }
    else if (client.audio_state == G_AUDIO_STATE.PUSH_TO_TALK_ENABLED)
    {

        if (target_element.classList.contains("client-audio-state-microphone-active"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-active");
        }

        if (target_element.classList.contains("client-audio-state-microphone-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-disabled");
        }

        if (target_element.classList.contains("client-audio-state-completely-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-completely-disabled");
        }

        document.getElementById("client-audio-state-" + client.client_id).classList.add("client-audio-state-microphone-enabled");

        if (client.client_id == g_local_client_id)
        {
            if (g_is_client_running_under_touch_device)
            {
                g_is_microphone_available = true;
                voice__update_microphone_button();
            }
            else
            {
                document.getElementById("toggle-microphone-label").style.display = "block";
            }
        }
    }
    else if (client.audio_state == G_AUDIO_STATE.PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS)
    {
        if (target_element.classList.contains("client-audio-state-microphone-active"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-active");
        }

        if (target_element.classList.contains("client-audio-state-microphone-enabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-enabled");
        }

        if (target_element.classList.contains("client-audio-state-completely-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-completely-disabled");
        }

        document.getElementById("client-audio-state-" + client.client_id).classList.add("client-audio-state-microphone-disabled");

        if (client.client_id == g_local_client_id)
        {
            if (g_is_client_running_under_touch_device)
            {
                g_is_microphone_available = true;
                voice__update_microphone_button();
            }
            else
            {
                document.getElementById("toggle-microphone-label").style.display = "block";
            }
        }

    }

    else if (client.audio_state == G_AUDIO_STATE.AUDIO_COMPLETELY_DISABLED)
    {
        if (target_element.classList.contains("client-audio-state-microphone-active"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-active");
        }

        if (target_element.classList.contains("client-audio-state-microphone-enabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-enabled");
        }

        if (target_element.classList.contains("client-audio-state-microphone-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-disabled");
        }

        document.getElementById("client-audio-state-" + client.client_id).classList.add("client-audio-state-completely-disabled");

        if (client.client_id == g_local_client_id)
        {
            g_is_microphone_available = false;
            voice__update_microphone_button();
            document.getElementById("toggle-microphone-label").style.display = "none";

            // audio is enabled by the server and server just disconnected our audio, attempt datachannel reconnect
            if (g_is_voice_chat_allowed_by_server == true)
            {
                g_is_webrtc_datachannel_connected = false;
                if (g_is_webrtc_datachannel_check_running == false)
                {
                    // claim the flag like every other starter - without this the loop ran
                    // unregistered and later triggers stacked concurrent loops on top of it
                    g_is_webrtc_datachannel_check_running = true;
                    voice__webrtc_datachannel_connection_check(true);
                }
            }
        }
    }

    if (client.is_streaming_song)
    {
        let element = document.querySelector('.marquee-music-playing-container[data-marquee-music-playing-container-id="' + client.client_id + '"]');
        if (element != null)
        {
            element.style.display = "inline-block";
            document.getElementById("marquee-song-name-client-id-" + client.client_id).innerHTML = chat__sanitize_string(client.song_name);
        }
        else
        {
            console.log("could not find element");
        }
    }
}

/**
 * @brief paces the song's mp3 chunks into the opus encoder worker
 *        bails out early when the server sends stop_song_stream, otherwise announces the song end after the last chunk
 *
 * @param array data_chunks -> the mp3 chunks
 * @param number mp3_sample_rate -> the song's sample rate
 *
 * @return promise resolves when the song ended or was stopped
 */
async function voice__stream_local_mp3_file_to_other_clients(data_chunks, mp3_sample_rate)
{
    // a song stream is a fresh spurt like a press: reset the encoder and mark the
    // boundary, so receivers scrub this sender's decoder before the first music frame
    g_opus_encoder_worker.postMessage({ type: "clear_opus_encoder_buffer" });
    g_voice_send_sequence_number = (g_voice_send_sequence_number + G_OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP) & 0xffff;

    for (var i = 0; i < data_chunks.length; i++)
    {
        let message = {
            type: "encode_mp3_chunk",
            value: data_chunks[i],
            mp3_sample_rate: mp3_sample_rate
        };

        g_opus_encoder_worker.postMessage(message);

        if (g_stop_song_stream_message_received)
        {
            g_stop_song_stream_message_received = false;

            // a song stream ends three ways: the pause button, the microphone being switched off, or
            // all bytes sent; each is handled a little differently

            // the first two cases arrive as the server's stop_song_stream message, which sets
            // g_stop_song_stream_message_received while this loop awaits its sleep

            // if condition is met only if user clicked "pause song" button located near file image select button
            // if user clicks de-activate microphone button, audio_state is set to 3 (deactivated), not checking audio_state
            // would cause the state go from 3 to 2 again..

            if (g_client_list[channel_tree__get_client_index_in_array_by_client_id(g_local_client_id)].audio_state == G_AUDIO_STATE.PUSH_TO_TALK_ACTIVE)
            {
                g_is_microphone_active = true;
                is_playing_music = false;
                g_last_sent_value_microphone_usage = 2;

                let message_object2 = {
                    message: {
                        type: "microphone_usage",
                        value: 2,
                    }
                };

                connection__send_message_object(message_object2);
            }

            return;
        }

        // the loop finishes sending before the others finish playing, so the mic-off request goes
        // out while the song is still audible on their side

        // also works with 60
        // 100 - starts lagging
        await utils__sleep(80);
    }

    // this is case 3 when song ends because it finished playing
    // update marquee animation and microphone state for other users

    let message_object1 = {
        message:
        {
            type: "stop_song_stream"
        }
    };

    connection__send_message_object(message_object1);

    document.getElementById("custom-file-upload-button-song").style.display = "inline-block";
    document.getElementById("stop-song").style.display = "none";

    g_is_microphone_active = true;
    is_playing_music = false;
    g_last_sent_value_microphone_usage = 2;

    let message_object3 = {
        message: {
            type: "microphone_usage",
            value: 2,
        }
    };

    connection__send_message_object(message_object3);
}

/**
 * @brief wav ArrayBuffer -> chunks of float samples: skips the 44-byte header and converts the 16-bit signed samples to floats
 *
 * @param ArrayBuffer arrayBuffer -> the wav file
 * @param number chunkLength -> samples per chunk
 *
 * @return array the Float32Array chunks
 */
function voice__chunk_buffers(arrayBuffer, chunkLength)
{
    var chunkedBuffers = [];

    var totalFile = new Int16Array(arrayBuffer);
    // Skip wave header; 44 bytes
    for (i = 22; i < totalFile.length; i += chunkLength)
    {

        // Convert 16 bit signed int to 32bit float
        var bufferChunk = new Float32Array(chunkLength);
        for (j = 0; j < chunkLength; j++)
        {
            bufferChunk[j] = (totalFile[i + j] + 0.5) / 32767.5;
        }

        chunkedBuffers.push(bufferChunk);
    };

    return chunkedBuffers;
}

// server-settings-tab.js is embedded in template.html along with the other client files
// it is the admin's server settings tab: the field table that loads and saves the general and log
// settings, the policy the server announces, the icon upload queue and the country block list
// dispatch.js feeds it the server's values, ui.js has the click handlers

// the server settings tab, one row per input: "key" is the wire name, "kind" says checkbox or
// number field, "tab" which save button sends it. the load and both saves walk this table
var SERVER_SETTINGS_FIELDS = [
    { key: "display_country_flags", id: "server-settings-general-display-flags-checkbox", kind: "bool", tab: "general" },
    { key: "hide_admin_country_flag", id: "server-settings-general-hide-admin-flag-checkbox", kind: "bool", tab: "general" },
    { key: "enable_audio", id: "server-settings-general-enable-audio", kind: "bool", tab: "general" },
    { key: "enable_music_bot_audio", id: "server-settings-general-enable-music-bot-audio-checkbox", kind: "bool", tab: "general" },
    { key: "hide_clients_in_password_channels", id: "server-settings-general-hide-clients-in-password-protected-channels", kind: "bool", tab: "general" },
    { key: "allow_temp_channels", id: "server-settings-general-allow-temp-channels-checkbox", kind: "bool", tab: "general" },
    { key: "allow_typing_indicator", id: "server-settings-general-allow-typing-indicator-checkbox", kind: "bool", tab: "general" },
    { key: "allow_client_renames", id: "server-settings-general-allow-renames-checkbox", kind: "bool", tab: "general" },
    { key: "is_sending_text_to_idle_clients_allowed", id: "server-settings-general-allow-text-to-idle-clients-checkbox", kind: "bool", tab: "general" },
    { key: "allow_private_messages", id: "server-settings-general-allow-private-messages-checkbox", kind: "bool", tab: "general" },
    { key: "allow_file_uploads", id: "server-settings-general-allow-file-uploads-checkbox", kind: "bool", tab: "general" },
    { key: "file_upload_max_size_mb", id: "server-settings-general-file-upload-max-size-input", kind: "number", fallback: 10, tab: "general" },
    { key: "allow_chat_pictures", id: "server-settings-general-allow-pictures-checkbox", kind: "bool", tab: "general" },
    { key: "chat_picture_max_size_mb", id: "server-settings-general-picture-max-size-input", kind: "number", fallback: 4, tab: "general" },
    { key: "is_same_ip_address_allowed", id: "server-settings-general-allow-same-ip-checkbox", kind: "bool", tab: "general" },
    { key: "is_fast_reconnect_allowed", id: "server-settings-general-fast-reconnect-checkbox", kind: "bool", tab: "general" },
    { key: "is_identity_takeover_allowed", id: "server-settings-general-identity-takeover-checkbox", kind: "bool", tab: "general" },
    { key: "is_websocket_ping_active", id: "server-settings-general-websocket-ping-checkbox", kind: "bool", tab: "general" },
    { key: "webrtc_datachannel_cooldown_seconds", id: "server-settings-general-datachannel-cooldown-input", kind: "number", fallback: 600, tab: "general" },
    { key: "icon_max_size_bytes", id: "server-settings-general-icon-max-size-input", kind: "number", fallback: 5000, tab: "general" },
    { key: "show_music_bot_marquee_to_everyone", id: "server-settings-general-marquee-everyone-checkbox", kind: "bool", tab: "general" },
    { key: "minimum_rsa_key_bits", id: "server-settings-general-minimum-rsa-bits-input", kind: "number", fallback: 2048, tab: "general" },
    { key: "announce_minimum_rsa_key_bits", id: "server-settings-general-announce-rsa-bits-checkbox", kind: "bool", tab: "general" },
    { key: "is_country_blocking_active", id: "server-settings-general-country-blocking-checkbox", kind: "bool", tab: "general" },
    { key: "log_client_joins", id: "server-settings-log-joins-checkbox", kind: "bool", tab: "log" },
    { key: "log_username_changes", id: "server-settings-log-renames-checkbox", kind: "bool", tab: "log" },
    { key: "log_tag_changes", id: "server-settings-log-tags-checkbox", kind: "bool", tab: "log" },
    { key: "log_server_settings_updates", id: "server-settings-log-settings-checkbox", kind: "bool", tab: "log" },
    { key: "log_kicks_and_bans", id: "server-settings-log-kicks-bans-checkbox", kind: "bool", tab: "log" },
    { key: "log_client_disconnects", id: "server-settings-log-disconnects-checkbox", kind: "bool", tab: "log" },
    { key: "log_failed_attempts", id: "server-settings-log-failed-checkbox", kind: "bool", tab: "log" },
    { key: "admin_log_max_size_mb", id: "server-settings-log-max-size-input", kind: "number", fallback: 10, tab: "log" },
    { key: "admin_log_retention_days", id: "server-settings-log-retention-select", kind: "number", fallback: 7, tab: "log" }
];

// stylesheet harvest and the Intl helper, both resolved once on first use
var country_code_cache = null;

var country_display_names = null;

var is_country_select_populated = false;

/**
 * @brief fills the tab's inputs from a server_settings_values message; a number the server left out shows its fallback
 *
 * @param object values -> the message body
 *
 * @return void
 */
function server_settings_tab__apply_server_settings_values_to_tab(values)
{
    for (let i = 0; i < SERVER_SETTINGS_FIELDS.length; i++)
    {
        let field = SERVER_SETTINGS_FIELDS[i];
        let element = document.getElementById(field.id);

        if (field.kind == "bool")
        {
            element.checked = (values[field.key] == true);
        }
        else
        {
            element.value = (typeof values[field.key] === "number") ? values[field.key] : field.fallback;
        }
    }
}

/**
 * @brief reads one tab's inputs back into a save_server_settings message body
 *        an empty number field sends its fallback; 0 stays 0, because some fields mean "off" by it
 *
 * @param string tab -> "general" or "log", which fields to read
 *
 * @return object the message body, type included
 */
function server_settings_tab__collect_server_settings_from_tab(tab)
{
    let settings = { type: "save_server_settings" };

    for (let i = 0; i < SERVER_SETTINGS_FIELDS.length; i++)
    {
        let field = SERVER_SETTINGS_FIELDS[i];

        if (field.tab != tab)
        {
            continue;
        }

        let element = document.getElementById(field.id);

        if (field.kind == "bool")
        {
            settings[field.key] = element.checked;
        }
        else
        {
            let number = parseInt(element.value);
            settings[field.key] = isNaN(number) ? field.fallback : number;
        }
    }

    return settings;
}

/**
 * @brief applies the server's client-facing policy to g_server_policy, from authentication_status on join and from the "server_policy" broadcast an admin's settings save triggers
 *        a field an older server does not send keeps its current value; a number is taken only when positive
 *
 * @param object data -> the message body
 *
 * @return void
 */
function server_settings_tab__apply_server_policy_fields(data)
{
    for (let policy_field in g_server_policy)
    {
        if (data[policy_field] === undefined)
        {
            continue;
        }

        if (typeof g_server_policy[policy_field] === "number")
        {
            if (typeof data[policy_field] === "number" && data[policy_field] > 0)
            {
                g_server_policy[policy_field] = data[policy_field];
            }
        }
        else
        {
            g_server_policy[policy_field] = (data[policy_field] == true);
        }
    }

    server_settings_tab__apply_rename_policy_to_ui();
    chat_files__apply_file_upload_policy_to_ui();
    chat_files__apply_chat_picture_policy_to_ui();
}

/**
 * @brief greys the local rename input when the server ignores user renames, because a rename that silently does nothing reads as a bug; an admin keeps the input editable
 *
 * @return void
 */
function server_settings_tab__apply_rename_policy_to_ui()
{
    let rename_input = document.getElementById("connected-local-client-input");

    if (rename_input == null)
    {
        return;
    }

    let may_rename = (g_server_policy.allow_client_renames == true) || (g_is_local_client_admin == true);

    rename_input.readOnly = (may_rename == false);
    rename_input.title = (may_rename == false) ? "renames are disabled on this server" : "";
}

/**
 * @brief the icon upload pump: sends the next queued icon unless one is in flight
 *        the reply handler clears g_icon_upload_in_flight_base64 and calls this again for the rest of the queue
 *
 * @return void
 */
function server_settings_tab__send_next_queued_icon_upload()
{
    if (g_icon_upload_in_flight_base64 != null) // still waiting for the previous upload's reply
    {
        return;
    }
    if (g_icon_upload_queue.length == 0)
    {
        return;
    }

    let base64_icon = g_icon_upload_queue.shift();
    g_icon_upload_in_flight_base64 = base64_icon;

    let message_object = {
        message: {
            type: "server_settings_icon_upload",
            base64_icon_value: base64_icon
        }
    };

    connection__send_message_object(message_object);
}

/**
 * @brief english name for an iso country code
 *
 * @param string code -> the two-letter code
 *
 * @return string the name, or the code itself when the browser cannot name it
 */
function server_settings_tab__get_country_display_name(code)
{
    if (country_display_names === null)
    {
        try
        {
            country_display_names = new Intl.DisplayNames(["en"], { type: "region" });
        }
        catch (intl_error)
        {
            country_display_names = false;
        }
    }

    if (country_display_names !== false)
    {
        try
        {
            let name = country_display_names.of(code);

            if (typeof name === "string" && name.length > 0)
            {
                return name;
            }
        }
        catch (lookup_error) { }
    }

    return code;
}

/**
 * @brief every code the flag stylesheet can draw, sorted by display name
 *
 * @return array the codes, [] headless
 */
function server_settings_tab__get_all_country_codes()
{
    if (country_code_cache != null)
    {
        return country_code_cache;
    }

    let codes = {};

    if (typeof document !== "undefined" && document.styleSheets != null)
    {
        for (let sheet_index = 0; sheet_index < document.styleSheets.length; sheet_index++)
        {
            let rules = null;

            try
            {
                rules = document.styleSheets[sheet_index].cssRules;
            }
            catch (rules_error)
            {
                continue;
            }

            if (rules == null)
            {
                continue;
            }

            for (let rule_index = 0; rule_index < rules.length; rule_index++)
            {
                let selector = rules[rule_index].selectorText;

                if (typeof selector !== "string")
                {
                    continue;
                }

                let matches = selector.match(/\.country-flag-([a-z]{2})\b/g);

                if (matches == null)
                {
                    continue;
                }

                for (let match_index = 0; match_index < matches.length; match_index++)
                {
                    codes[matches[match_index].slice(-2).toUpperCase()] = true;
                }
            }
        }
    }

    country_code_cache = Object.keys(codes).sort(function(a, b)
    {
        return server_settings_tab__get_country_display_name(a).localeCompare(server_settings_tab__get_country_display_name(b));
    });

    return country_code_cache;
}

/**
 * @brief fills the country picker once, the first time the section becomes visible
 *
 * @return void
 */
function server_settings_tab__populate_country_block_select()
{
    if (is_country_select_populated == true)
    {
        return;
    }

    let select = document.getElementById("server-settings-country-block-select");

    if (select == null)
    {
        return;
    }

    let codes = server_settings_tab__get_all_country_codes();

    if (codes.length == 0)
    {
        return;
    }

    let html = "<option value=\"\">add a country to the block list ...</option>";

    for (let i = 0; i < codes.length; i++)
    {
        html += "<option value=\"" + codes[i] + "\">" + chat__sanitize_string(server_settings_tab__get_country_display_name(codes[i])) + " (" + codes[i] + ")</option>";
    }

    select.innerHTML = html;
    is_country_select_populated = true;
}

/**
 * @brief repaints the blocked-countries list from g_blocked_countries_draft, "no blocked countries" when empty
 *
 * @return void
 */
function server_settings_tab__render_blocked_countries_list()
{
    let container = document.getElementById("server-settings-blocked-countries-list");

    if (container == null)
    {
        return;
    }

    container.innerHTML = "";

    if (g_blocked_countries_draft.length == 0)
    {
        let empty = document.createElement("p");
        empty.className = "blocked-country-empty";
        empty.innerText = "no blocked countries";
        container.appendChild(empty);
        return;
    }

    for (let i = 0; i < g_blocked_countries_draft.length; i++)
    {
        let code = g_blocked_countries_draft[i];

        let row = document.createElement("div");
        row.className = "blocked-country-entry";

        let flag = document.createElement("div");
        flag.className = "blocked-country-flag country-flag-" + code.toLowerCase();
        row.appendChild(flag);

        let name = document.createElement("span");
        name.className = "blocked-country-name";
        name.innerText = server_settings_tab__get_country_display_name(code) + " (" + code + ")";
        row.appendChild(name);

        let remove = document.createElement("span");
        remove.className = "blocked-country-remove";
        remove.title = "remove";
        remove.innerText = "✕";
        remove.setAttribute("data-country-code", code);
        remove.onclick = function(event)
        {
            event.stopPropagation();

            let removed_code = event.currentTarget.getAttribute("data-country-code");

            g_blocked_countries_draft = g_blocked_countries_draft.filter(function(entry) { return entry != removed_code; });
            server_settings_tab__render_blocked_countries_list();
        };
        row.appendChild(remove);

        container.appendChild(row);
    }
}

/**
 * @brief picking an option adds it to the draft (at most 100, sorted by name); the picker snaps back to its placeholder
 *
 * @return void
 */
function server_settings_tab__country_block_select_onchange()
{
    let select = document.getElementById("server-settings-country-block-select");
    let code = select.value;

    select.value = "";

    if (typeof code !== "string" || /^[A-Z]{2}$/.test(code) == false)
    {
        return;
    }

    if (g_blocked_countries_draft.indexOf(code) != -1)
    {
        return;
    }

    if (g_blocked_countries_draft.length >= 100)
    {
        utils__custom_alert("the block list is full (100 countries)");
        return;
    }

    g_blocked_countries_draft.push(code);
    g_blocked_countries_draft.sort(function(a, b)
    {
        return server_settings_tab__get_country_display_name(a).localeCompare(server_settings_tab__get_country_display_name(b));
    });
    server_settings_tab__render_blocked_countries_list();
}

/**
 * @brief the "hide admin's flag" row only makes sense while flags are displayed at all, so it follows the flags checkbox
 *
 * @return void
 */
function server_settings_tab__refresh_hide_admin_flag_row_visibility()
{
    let flags_checkbox = document.getElementById("server-settings-general-display-flags-checkbox");
    let row = document.getElementById("server-settings-hide-admin-flag-row");

    if (flags_checkbox == null || row == null)
    {
        return;
    }

    row.style.display = (flags_checkbox.checked == true) ? "" : "none";
}

/**
 * @brief the country-blocking checkbox shows or hides the picker below it
 *        the block list itself is server state either way; unchecking just disables enforcement, it does not clear the list
 *
 * @return void
 */
function server_settings_tab__refresh_country_blocking_visibility()
{
    let checkbox = document.getElementById("server-settings-general-country-blocking-checkbox");
    let container = document.getElementById("server-settings-country-blocking-container");

    if (checkbox == null || container == null)
    {
        return;
    }

    if (checkbox.checked == true)
    {
        server_settings_tab__populate_country_block_select();
        container.style.display = "block";
    }
    else
    {
        container.style.display = "none";
    }
}

/**
 * @brief takes the blocked countries the server currently has, straight from server_settings_values, into the draft
 *
 * @param array list -> the two-letter codes
 *
 * @return void
 */
function server_settings_tab__set_blocked_countries_from_server(list)
{
    g_blocked_countries_draft = [];

    if (Array.isArray(list))
    {
        for (let i = 0; i < list.length; i++)
        {
            if (typeof list[i] === "string" && /^[A-Za-z]{2}$/.test(list[i]) == true
                && g_blocked_countries_draft.indexOf(list[i].toUpperCase()) == -1)
            {
                g_blocked_countries_draft.push(list[i].toUpperCase());
            }
        }
    }

    server_settings_tab__render_blocked_countries_list();
}

// dispatch.js is embedded in template.html along with the other client files, and in the node bundle
// it is the main thread's message dispatcher: everything the workers post back (decrypted server
// messages, encryption results, socket events) arrives in dispatch__mainthread_onmessage and is routed to the
// handlers in messages.js and the feature files

/**
 * @brief the main thread's message handler for everything the workers post back
 *        decrypted server messages go to their server_msg handler, encoder output to the
 *        datachannel, log lines to the log, and so on, one branch per message type
 *
 * @param object e -> the worker message event; e.data.type picks the branch
 *
 * @return void
 */
function dispatch__mainthread_onmessage(e)
{
    if (DBG_WORKER_BOOT_LOG) { console.log("[m<-w] " + (e.data && e.data.type)); }

    if (e.data.type == "log")
    {
        utils__custom_log(e.data.value);
    }
    else if (e.data.type == "opus_encoder_worker__encode_result")
    {
        // chunks already inside the encoder when push-to-talk ended arrive here after
        // mic-off; dropping them keeps the stale tail out of receivers' jitter lanes
        if (g_is_microphone_active == false && (typeof is_playing_music == "undefined" || is_playing_music != true))
        {
            return;
        }

        if (g_current_channel_keys != null)
        {
            let opus_data_chunks = e.data.value;

            for (var i = 0; i < opus_data_chunks.length; i++)
            {
                // prepend a 2-byte little-endian sequence number INSIDE the encrypted payload;
                // receivers use it to reorder frames the unordered datachannel scrambled and to
                // detect losses. the server relay never sees it (it sits under the encryption)
                let opus_chunk_bytes = new Uint8Array(opus_data_chunks[i]);
                let sequenced_chunk = new Uint8Array(2 + opus_chunk_bytes.length);

                sequenced_chunk[0] = g_voice_send_sequence_number & 0xff;
                sequenced_chunk[1] = (g_voice_send_sequence_number >> 8) & 0xff;
                sequenced_chunk.set(opus_chunk_bytes, 2);
                g_voice_send_sequence_number = (g_voice_send_sequence_number + 1) & 0xffff;

                let data = keys__encrypt_data_with_aes_keys(g_current_channel_keys, sequenced_chunk);
                if (data != null && data.byteLength > 0) { g_session.bytes_sent += data.byteLength; }
                g_datachannel.send(data);
            }
        }
        else
        {
            utils__custom_log("dont have channel keys, cant encrypt audio for sending");
        }
    }
    else if (e.data.type == "opus_decoder_worker__decode_result")
    {
        audio__audio_player_write_chunk(e.data.value);
    }
    else if (e.data.type == "minimp3_worker__decode_result")
    {
        let data_chunks = voice__chunk_buffers(e.data.value, 4096);

        let message_object1 = {
            message:
            {
                type: "start_song_stream",
                song_name: g_selected_song_name
            }
        };

        connection__send_message_object(message_object1);

        voice__stream_local_mp3_file_to_other_clients(data_chunks, e.data.mp3_sample_rate);

        if (g_alert_streaming_music_shown_once == false)
        {
            utils__custom_alert("when streaming music from file, detach browser tab where chat is to separate window (if using multiple tabs) or stay focused on tab where chat is otherwise music is not sent reliably and it lags");
            g_alert_streaming_music_shown_once = true;
        }
    }
    else if (e.data.type == "websocket_worker_onmessage")
    {
        if (e.data.value != null && e.data.value.length > 0) { g_session.bytes_received += e.data.value.length; }

        // data from g_websocket_worker are passed to main thread and then to g_data_processing_worker
        g_data_processing_worker.postMessage({
            type: "mainthread__process_received_websocket_message",
            value: e.data.value,
            // loopback frames arrive as plain json, the worker skips decryption
            is_plaintext: android_host__is_ui_only_runtime()
        });
    }
    else if (e.data.type == "data_processing_worker__rsa_key_too_weak")
    {
        keys__handle_rsa_key_too_weak_notice(e.data.minimum_rsa_key_bits);
    }
    else if (e.data.type == "websocket_worker_onclose")
    {
        // the driver awaits this signal; it decides whether and when to redial
        g_last_connect_attempt_failed = true;

        // the server never says why - these are inferences from WHERE the socket died.
        // in the webview the socket is the loopback to node, so its close says nothing
        // about the real connection: node owns that story and reports it separately
        if (android_host__is_ui_only_runtime() == false)
        {
            if (g_is_authenticated == true)
            {
                g_connection_status.last_connected_at = new Date().valueOf();

                if (g_last_disconnect_reason === "")
                {
                    g_last_disconnect_reason = "the server closed the connection (it probably kicked this client, or shut down)";
                }
            }
            else if (g_last_disconnect_reason === "")
            {
                g_last_disconnect_reason = (g_device_has_network === false)
                    ? "no network connection (wifi and mobile data are off)"
                    : "the server probably rejected the keys (it closed the connection during login)";
            }
        }

        connection__signal_connection_closed();
        console.warn("server connection lost (identity switch in progress: " + g_is_identity_switch_in_progress + ")");

        // the resume socket itself died: that is the one allowed failure, back to the connect screen
        if (g_fast_reconnect.in_progress == true)
        {
            connection__fast_reconnect_failed("the resume socket closed");
        }
        else if (connection__try_start_fast_reconnect("socket closed") == false)
        {
            // a deliberate close (identity switch) is not a lost connection - no scary alert
            if (g_is_authenticated && g_is_identity_switch_in_progress == false)
            {
                if (g_is_running_in_android_webview == false)
                {
                    utils__custom_alert("connection with server was lost");
                }
            }
            g_is_authenticated = false;

            connection__reset_chat_app_keep_identity();
        }
    }
    else if (e.data.type == "websocket_worker_onerror")
    {
        g_last_connect_attempt_failed = true;

        if (g_last_disconnect_reason === "")
        {
            g_last_disconnect_reason = connection__describe_socket_error(e.data.error_code);
        }

        if (g_is_authenticated == true)
        {
            g_connection_status.last_connected_at = new Date().valueOf();
        }

        connection__signal_connection_closed();

        if (g_fast_reconnect.in_progress == true)
        {
            connection__fast_reconnect_failed("the resume socket failed");
        }
        else if (connection__try_start_fast_reconnect("socket error") == false)
        {
            if (g_is_running_in_android_webview == false)
            {
                utils__custom_alert("connecting to server failed");
            }
            connection__reset_chat_app_keep_identity();
            g_is_authenticated = false;
        }
    }
    else if (e.data.type == "data_processing_worker__generate_rsa_keypair_result")
    {
        g_rsa_public_key_string = e.data.value;
        console.log("public key string -> " + g_rsa_public_key_string);
        g_is_rsa_key_generated = true;

        // the keygen phase is over: stop claiming it. when nothing will dial on its own the
        // status goes idle (empty), otherwise the driver's "connecting" replaces it right away
        if (g_is_authenticated == false && g_is_autoconnect_without_user_action_active == false)
        {
            connection__report_connection_status("idle", "");
        }
        document.getElementById("another-buttons-sub-loading-container").style.display = "none";
        document.getElementById("another-buttons-sub-container").style.display = "";
        g_identity_string = e.data.identity_string;

        // wakes the driver if it is waiting to dial with this identity
        g_identity_slot.set({ public_key_string: e.data.value, identity_string: e.data.identity_string });
        g_is_identity_switch_in_progress = false;

        // persist the identity (the passphrase) so the next launch reconstructs this same keypair
        // instead of a fresh random one. covers first launch (random) and identity switches alike:
        // the last identity used is the one remembered.
        if (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.persist_identity === true)
        {
            utils__storage_set("lemon_identity_string", g_identity_string);
        }
    }
    else if (e.data.type == "data_processing_worker__loopback_status")
    {
        // node still working keeps the spinner; a failed or idle state shows the page
        if (e.data.value.state == "connecting" || e.data.value.state == "connected")
        {
            connection__extend_connect_page_holdback();
        }
        else
        {
            connection__reveal_connect_page();
        }

        // node owns the real connection; its report replaces the local guesswork
        g_connection_status.state = e.data.value.state;
        g_connection_status.reason = e.data.value.reason;
        g_connection_status.next_retry_at = e.data.value.next_retry_at;
        g_connection_status.last_connected_at = e.data.value.last_connected_at;
        connection__render_connection_status();
    }
    else if (e.data.type == "data_processing_worker__authentication_status")
    {
        if (e.data.value == "success")
        {
            // a resume the server answered with a plain login: it had no session left to adopt, so
            // the page state is stale. one clean failure instead of a half-refreshed page
            if (g_fast_reconnect.in_progress == true && g_fast_reconnect.resumed == false)
            {
                connection__fast_reconnect_failed("the server had no session to resume");
                return;
            }

            g_connection_status.last_connected_at = new Date().valueOf();
            g_connection_status.next_retry_at = 0;
            connection__report_connection_status("connected");

            // node that logs in with nobody watching belongs in idle straight away,
            // otherwise it stands in the root channel looking present
            if (typeof android_host__node_apply_idle_for_ui_state === "function")
            {
                android_host__node_apply_idle_for_ui_state();
            }

            utils__hide_custom_alert(); // if there was any alert

            // the same policy set arrives again as "server_policy" whenever an admin saves,
            // so both paths share one application function
            server_settings_tab__apply_server_policy_fields(e.data.policy);

            // fresh session: the info card counts from this moment. a resumed one keeps counting
            if (g_fast_reconnect.resumed == false)
            {
                g_session.connected_at = new Date().valueOf();
                g_session.bytes_sent = 0;
                g_session.bytes_received = 0;
                g_session.last_ping_ms = -1;
            }

            // ask for the offline-people roster on every connect, from every device and theme.
            // the server decides: it answers only if it offers the list AND this user is
            // registered on it. a refusal is simply silence, so nothing here needs to know.
            client_msg.send_request_stored_clients();

            // the idle panel is wired once; a resume still has it (a second listener would cancel the first)
            if (e.data.is_idle_mode_allowed && g_fast_reconnect.resumed == false)
            {
                document.getElementById('channel-list-container').style.height = "calc(70% - 30px)";
                document.getElementById("idle-channel-collapse-button").addEventListener("mousedown", UI.collapse_expand_channel);
                document.getElementById('idle-clients-container').style.display = "block";
            }

            g_is_authenticated = true;
            console.log("client authenticated");

            // the attempt is over once the adopted session accepted the login; the refreshed
            // lists are still on their way and are applied together when the last one arrives
            if (g_fast_reconnect.resumed == true)
            {
                g_fast_reconnect.in_progress = false;
                console.log("fast reconnect: login accepted on the adopted session");
            }

            g_client_list = g_client_list;
            g_channel_list = g_channel_list;

            if (g_is_running_in_android_webview)
            {
                // the bar gear is the connected state's settings button; the connect screen has its
                // own, and tapping this one while connected asks first (it edits the live connection)
                document.getElementById("android-settings-button").style.display = "block";
            }

            // the connected ui is up, so the hold has served its purpose
            g_is_holding_back_connect_page = false;
            document.getElementById('verification-system').style.visibility = "";
            connection__set_connect_holdback_loader_visible(false);
            connection__set_connect_button_pending(false);

            document.getElementById('verification-system').style.display = "none";
            document.getElementById('communication-system-container').style.display = (g_layout.grid_active == true) ? "grid" : "block";

            // the connect screen is gone, so the mic may come back out
            voice__update_microphone_button();

            // people on touch device dont need to see chat by default, current UI isnt suitable for that
            // most likely they will just want to call with voice. a resume leaves the chat as the user had it
            if (g_is_client_running_under_touch_device && g_fast_reconnect.resumed == false)
            {
                UI.hide_chat_container();
            }

            if (!g_should_connection_check_be_running)
            {
                g_should_connection_check_be_running = true;
                connection__websocket_connection_check();
            }

            // a fresh session starts the server's datachannel attempt count from zero, so a retry loop
            // sitting out a cooldown may try again now (a resumed session keeps the server's cooldown)
            if (g_fast_reconnect.resumed == false)
            {
                g_webrtc_datachannel_cooldown_until_ms = 0;
            }
            if (g_datachannel_retry_sleep_resolve != null)
            {
                g_datachannel_retry_sleep_resolve();
            }

            // a resumed session was never gone from the user's point of view: no connected sound
            if (g_fast_reconnect.resumed == true)
            {
                console.log("fast reconnect: connected sound skipped");
            }
            else
            {
                // the connected sound waits a second: on android the client list that follows makes
                // java push the user settings (with the sound preference) right after this point
                setTimeout( () => {
                    if (g_are_sound_effects_enabled)
                    {
                        g_sound_effects.connected.play();
                    }
                }, 1000);
            }
        }
    }
    else if (e.data.type == "data_processing_worker__audio_enabled")
    {
        // default stun port is 3478 for this chat. Stun port is given to client with websocket connection, stun port can be changed if needed, there are two places that need to be edited in code of server to set different stun port

        g_iceconfig = {
            iceServers: [{
                urls: "stun:" + g_host + ":" + e.data.stun_port,
            }],
        };

        // the datachannel/playback below is set up whenever audio is on for clients or music bots,
        // so this flag (which keeps the datachannel established/reconnected) is always true here.
        // whether this client may transmit its own microphone is a separate flag, gated on client voice
        g_is_voice_chat_allowed_by_server = true;
        g_is_client_microphone_allowed_by_server = (e.data.client_voice_allowed == true);
        // from here on it is playback and webrtc; the headless service has no webaudio, no opus
        // decoder worker and no RTCPeerConnection, so it leaves before the constructor throws
        if (typeof window.AudioContext !== "function" && typeof window.webkitAudioContext !== "function")
        {
            console.log("no webaudio in this runtime, skipping playback and datachannel setup");
            return;
        }

        // decoded audio is 48 kHz pcm pushed in untouched, so the context is asked for 48 kHz and the
        // browser converts to the device rate; a 44.1 kHz context played it 9% slow and lagged more each second
        try
        {
            g_audio_context = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 48000 });
        }
        catch (sample_rate_hint_not_supported)
        {
            g_audio_context = new (window.AudioContext || window.webkitAudioContext)();
        }
        console.log("audio_context.sampleRate" + g_audio_context.sampleRate);

        // without a user gesture the browser creates the context suspended and drops every frame;
        // resume now if a gesture already happened, else the first-gesture handler in main.js does
        if (g_audio_context.state === "suspended")
        {
            console.log("audio_context created suspended (autoplay policy, no user gesture yet); audio stays silent until a gesture resumes it");
            g_audio_context.resume();
        }

        let opus_decoding_sampler_channels = 1;
        let opus_decoding_sampler_input_rate = 48000;
        let opus_decoding_sampler_output_rate = g_audio_context.sampleRate;

        g_opus_decoding_sampler = new SpeexResampler(opus_decoding_sampler_channels,
            opus_decoding_sampler_input_rate,
            opus_decoding_sampler_output_rate);

        g_opus_decoder_worker.postMessage({
            type: "init",
            sampleRate: opus_decoding_sampler_output_rate
        });

        g_silence = new Float32Array(g_audio_config.codec.bufferSize);
        g_audio_player_gain_node = g_audio_context.createGain();
        g_audio_player_gain_node.connect(g_audio_context.destination);

        // AudioWorklet playback when the context supports it, ScriptProcessorNode otherwise. guarded,
        // because an exception here used to abort the handler before the webrtc datachannel check below
        try
        {
            audio__create_audio_player_output();
        }
        catch (audio_output_setup_error)
        {
            console.log("audio player output setup failed (" + audio_output_setup_error + "); continuing so the datachannel still gets created");
        }

        try
        {
            console.log("voice__create_new_peer_connection_object_for_use");

            // a re-login burst can arrive while a previous check loop is still sleeping;
            // that loop will pick the fresh config up itself, a second one just fights it
            if (g_is_webrtc_datachannel_check_running == false)
            {
                g_is_webrtc_datachannel_check_running = true;
                voice__webrtc_datachannel_connection_check(false);
            }
        }
        catch (Exception)
        {
            utils__custom_log(Exception.toString());
            utils__custom_alert('audio connection failed');
            return;
        }

        // android app went to background while the connect was still in progress - enter idle now
        if (g_is_deep_idle_pending == true)
        {
            g_is_deep_idle_pending = false;
            android_host__enter_deep_idle();
        }
    }
    else if (e.data.type == "data_processing_worker__metadata_keys_accepted")
    {
        g_keys_init_status = true;
    }
    else if (e.data.type == "data_processing_worker__client_list_from_server")
    {
        if (connection__fast_reconnect_buffer_list("client", e.data.value) == false)
        {
            server_msg.process_client_list_from_server(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__client_avatar_from_server")
    {
        server_msg.process_client_avatar_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__avatar_changed_from_server")
    {
        server_msg.process_avatar_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__connection_check_response")
    {
        g_connection_check.last_response_timestamp = new Date().valueOf();

        if (g_session.ping_sent_at > 0)
        {
            g_session.last_ping_ms = g_connection_check.last_response_timestamp - g_session.ping_sent_at;
        }
    }
    else if (e.data.type == "data_processing_worker__fast_reconnect_ok")
    {
        connection__fast_reconnect_succeeded();
    }
    else if (e.data.type == "data_processing_worker__datachannel_cooldown")
    {
        let seconds_left = parseInt(e.data.value.message.seconds_left);

        if (isNaN(seconds_left) == false && seconds_left > 0)
        {
            g_webrtc_datachannel_cooldown_until_ms = new Date().valueOf() + seconds_left * 1000;
            console.warn("datachannel: the server refuses new attempts for " + seconds_left + " s (10 attempts never connected)");
        }
    }
    else if (e.data.type == "data_processing_worker__channel_list_from_server")
    {
        if (connection__fast_reconnect_buffer_list("channel", e.data.value) == false)
        {
            server_msg.process_channel_list_from_server(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__tag_list_from_server")
    {
        if (connection__fast_reconnect_buffer_list("tag", e.data.value) == false)
        {
            server_msg.process_tag_list_from_server(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__identity_list_from_server")
    {
        server_msg.process_identity_list_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__icon_list_from_server")
    {
        if (connection__fast_reconnect_buffer_list("icon", e.data.value) == false)
        {
            server_msg.process_icon_list_from_server(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__icon_add_from_server")
    {
        server_msg.process_icon_add_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_add_to_client_from_server")
    {
        server_msg.process_add_tag_to_client_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_alias_changed_from_server")
    {
        server_msg.process_client_alias_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_country_code_changed_from_server")
    {
        server_msg.process_client_country_code_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__typing_indicator_from_server")
    {
        chat__note_typing_from_client(e.data.value.message.client_id, e.data.value.message.receiver_type, e.data.value.message.receiver_id);
    }
    else if (e.data.type == "data_processing_worker__stored_clients_list_from_server")
    {
        server_msg.process_stored_clients_list_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__remove_tag_from_client_from_server")
    {
        server_msg.process_remove_tag_from_client_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_add_from_server")
    {
        server_msg.process_tag_add_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_delete_from_server")
    {
        server_msg.process_tag_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__icon_delete_from_server")
    {
        server_msg.process_icon_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_icon_changed_from_server")
    {
        server_msg.process_tag_icon_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_icon_changed_from_server")
    {
        server_msg.process_channel_icon_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__start_song_stream_from_server")
    {
        server_msg.process_start_song_stream_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__stop_song_stream_from_server")
    {
        server_msg.process_stop_song_stream_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__chat_message_delete_from_server")
    {
        server_msg.process_chat_message_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__public_key_challenge_from_server")
    {
        let msg = e.data.value;
        let dh_public_mix_string = msg.message.dh_public_mix;

        let dh_public_mix = BigInt(dh_public_mix_string);

        // reject a degenerate server public mix (B <= 1 or B >= p-1) before using it; such values
        // force a known/tiny shared secret. for a safe prime, requiring 2 <= B <= p-2 is sufficient.
        if (dh_public_mix <= 1n || dh_public_mix >= g_dh_modulus - 1n)
        {
            console.error("rejected degenerate DH public mix from server; aborting handshake");
            return;
        }

        let shared_secret = lemon_crypto.modpow(dh_public_mix, g_dh_secret_exponent, g_dh_modulus);

        // add shared secret to

        // because there is also "secret key" used ,used to further secure connection between server and client, and is different for every connected client, (obtained with steps that wants to look like diffie-hellman key exchange)
        // this secret key needs to be added to metadata keys
        // and then copy of g_metadata_keys object sent to data processing webworker aswell (there are two copies of same object)

        let shared_secret_string = shared_secret.toString();

        // derive the AES enc key + HMAC mac key from the shared secret via HKDF (matches the server)
        let dh_keys = keys__dh_derive_keys(shared_secret_string);
        key_bytes = dh_keys.enc_key;

        let single_key = {
            info: "aes-ctr",
            key_string: shared_secret_string,
            key_bytes: key_bytes,
            mac_bytes: dh_keys.mac_key
        };

        g_metadata_keys.unshift(single_key);

        // removed: never log key material (g_metadata_keys holds the session key)

        // here the g_metadata_keys object within data_processing web worker is updated aswell

        console.log("public_key_challenge_response result -> ", msg.message.decryption_result);

        g_data_processing_worker.postMessage({
            type: "mainthread__metadata_keys",
            value: g_metadata_keys
        });

        let message_object = {
            message: {
                type: "public_key_challenge_response",
                value: msg.message.decryption_result,
                // asks the server to adopt the still-open session of this identity (fast reconnect)
                fast_reconnect: (g_fast_reconnect.in_progress == true)
            }
        };

        // the chosen username rides along on the last login message, because the server
        // assigns the final name right after accepting this response
        if (typeof g_chosen_username === "string" && g_chosen_username.length > 0)
        {
            message_object.message.chosen_username = g_chosen_username;
        }

        connection__send_message_object(message_object);
    }
    else if (e.data.type == "data_processing_worker__chat_message_edit_from_server")
    {
        server_msg.process_chat_message_edit_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__poke_from_server")
    {
        server_msg.process_poke_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__access_denied_from_server")
    {
        server_msg.process_access_denied_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__force_admin_password_change")
    {
        // the admin password set during server setup was typed in cleartext; prompt once for a new one
        let new_admin_password = prompt("The admin password set at server setup was typed in cleartext in the console. Please set a new admin password now.");
        if (new_admin_password != null && new_admin_password != "")
        {
            let message_object = { message: { type: "change_admin_password", value: new_admin_password } };
            connection__send_message_object(message_object);
        }
    }
    else if (e.data.type == "data_processing_worker__client_info_from_server")
    {
        server_msg.process_client_info_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_full_from_server")
    {
        let full_channel = channel_tree__get_channel_by_id(g_channel_list, e.data.value.message.channel_id);
        let full_channel_name = (full_channel != null && full_channel.name != null) ? full_channel.name : "channel";
        utils__custom_alert("'" + full_channel_name + "' is full");
    }
    else if (e.data.type == "data_processing_worker__server_settings_values_from_server")
    {
        let msg = e.data.value;
        server_settings_tab__apply_server_settings_values_to_tab(msg.message);
        server_settings_tab__refresh_hide_admin_flag_row_visibility();
        chat_files__refresh_file_upload_size_visibility();
        chat_files__refresh_picture_size_visibility();
        server_settings_tab__set_blocked_countries_from_server(msg.message.blocked_countries);
        server_settings_tab__refresh_country_blocking_visibility();
        UI.render_bans_list(msg.message.bans);
    }
    else if (e.data.type == "data_processing_worker__admin_log_from_server")
    {
        server_msg.process_admin_log_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__server_policy_from_server")
    {
        server_settings_tab__apply_server_policy_fields(e.data.value.message);
    }
    else if (e.data.type == "data_processing_worker__channel_join_from_server")
    {
        server_msg.process_channel_join_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__server_chat_message_id_for_local_message_id_from_server")
    {
        server_msg.process_server_chat_message_id_for_local_message_id_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_connect_from_server")
    {
        server_msg.process_client_connect_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_rename_from_server")
    {
        server_msg.process_client_rename_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_disconnect_from_server")
    {
        server_msg.process_client_disconnect_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__audio_state_of_single_client_from_server")
    {

        let client = {
            client_id: e.data.value.message.client_id,
            audio_state: e.data.value.message.value,
        }
        voice__process_audio_state_of_single_client(client);
    }
    else if (e.data.type == "data_processing_worker__current_channel_active_microphone_usage_from_server")
    {
        console.log("data_processing_worker__current_channel_active_microphone_usage_from_server");

        console.log(e.data.value);
        for (var i = 0; i < e.data.value.message.clients.length; i++)
        {
            voice__process_audio_state_of_single_client(e.data.value.message.clients[i]);
        }
    }
    else if (e.data.type == "data_processing_worker__sdp_offer_from_server")
    {
        server_msg.process_sdp_offer_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__ice_candidate_from_server")
    {
        server_msg.process_ice_candidate_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__server_info_broadcast_from_server")
    {
        server_msg.process_server_info_broadcast_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_edit_from_server")
    {
        server_msg.process_channel_edit_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_going_to_idle_mode")
    {
        server_msg.process_client_client_going_to_idle_mode_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_coming_back_from_idle_mode")
    {
        server_msg.process_client_coming_back_from_idle_mode_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_delete_from_server")
    {
        server_msg.process_channel_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_create_from_server")
    {
        server_msg.process_channel_create_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_picture_metadata")
    {
        server_msg.process_channel_chat_picture_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_picture_metadata")
    {
        server_msg.process_direct_chat_picture_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_file_metadata")
    {
        server_msg.process_channel_chat_file_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_file_metadata")
    {
        server_msg.process_direct_chat_file_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_file")
    {
        server_msg.process_direct_chat_file_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_file")
    {
        server_msg.process_channel_chat_file_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__chat_file_decrypt_failed")
    {
        server_msg.process_chat_file_decrypt_failed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__file_send_error_from_server")
    {
        server_msg.process_file_send_error_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__seen_receipt_message_result")
    {
        connection__websocket_worker_send(e.data.seen_receipt_message_content);
    }
    else if (e.data.type == "data_processing_worker__channel_maintainer_id")
    {
        server_msg.process_channel_maintainer_id_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_message")
    {
        server_msg.process_channel_chat_message_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_message")
    {
        server_msg.process_direct_chat_message_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__offline_chat_message")
    {
        server_msg.process_offline_chat_message_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_picture")
    {
        server_msg.process_direct_chat_picture_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_picture")
    {
        server_msg.process_channel_chat_picture_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__image_sent_status_from_server")
    {
        server_msg.process_image_sent_status_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__call_from_server")
    {
        server_msg.process_call_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__create_websocket_channel_keys_message_result")
    {
        console.log("sending channel keys to user -> " + e.data.username);
        connection__websocket_worker_send(e.data.channel_keys_message_content);
    }
    else if (e.data.type == "data_processing_worker__local_channel_keys_for_ui")
    {
        // node is the maintainer: it generated these keys itself, so no inbound frame
        // exists to forward. deliver our own copy to the attached ui directly
        if (g_node_frame_listener != null)
        {
            console.log("delivering locally generated channel keys to the ui");
            g_node_frame_listener(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__new_channel_keys_from_data_processing_worker")
    {
        g_current_channel_keys = e.data.value;

        // data processing worker created new channel keys and sent them to UI thread, now send them from UI thread to opus_decoder worker
        g_opus_decoder_worker.postMessage({
            type: "mainthread__channel_keys_for_opus_decoder",
            value: g_current_channel_keys
        });
    }
    else if (e.data.type == "data_processing_worker__tell_websocket_worker_to_send_data")
    {
        // the last stop before the socket: if a message reaches here and still does not
        // arrive, the fault is past the webview (node or the server), not in it
        utils__custom_log("[send-out] worker produced "
            + (e.data.data_to_be_sent_over_websocket || "").length + " chars for the socket");
        connection__websocket_worker_send(e.data.data_to_be_sent_over_websocket);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_picture_to_be_uploaded_by_parts")
    {
        let total_bytes_length = e.data.data_for_upload_process.length;
        let parts = chat__split_string_into_smaller_parts(e.data.data_for_upload_process, 400);

        // the send path took the upload lock before handing the picture to the worker
        g_file_send_intent = "direct_chat_picture_file";
        g_file_send_intent_extra_data = e.data.extra_data;
        g_is_file_being_uploaded = false;
        chat__send_file_by_parts(parts, total_bytes_length, 5);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_picture_to_be_uploaded_by_parts")
    {
        let total_bytes_length = e.data.data_for_upload_process.length;
        let parts = chat__split_string_into_smaller_parts(e.data.data_for_upload_process, 400);

        // the send path took the upload lock before handing the picture to the worker
        g_file_send_intent = "channel_chat_picture_file";
        g_file_send_intent_extra_data = e.data.extra_data;
        g_is_file_being_uploaded = false;
        chat__send_file_by_parts(parts, total_bytes_length, 5);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_file_to_be_uploaded_by_parts"
        || e.data.type == "data_processing_worker__channel_chat_file_to_be_uploaded_by_parts")
    {
        // the send path took the upload lock before handing the file to the worker, so this
        // cannot collide with another upload the way a refused picture could
        let total_bytes_length = e.data.data_for_upload_process.length;
        let parts = chat__split_string_into_smaller_parts(e.data.data_for_upload_process, 400);

        g_file_send_intent = (e.data.type == "data_processing_worker__direct_chat_file_to_be_uploaded_by_parts") ? "direct_chat_file" : "channel_chat_file";
        g_file_send_intent_extra_data = e.data.extra_data;
        g_is_file_being_uploaded = false;
        chat__send_file_by_parts(parts, total_bytes_length, 5);
    }
    else if (e.data.type == "data_processing_worker__chat_file_send_failed")
    {
        // the worker could not encrypt it (bad receiver key); the card and the lock are ours to clean up
        chat__release_file_upload_lock();
        utils__custom_alert("file not sent: " + e.data.reason);
        chat_files__mark_local_chat_file_card_failed(e.data.local_message_id, "not sent: " + e.data.reason);
    }
    else if (e.data.type == "data_processing_worker__music_bot_song_list_from_server")
    {
        server_msg.process_music_bot_song_list_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__file_send_success_from_server")
    {
        server_msg.process_file_send_success_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__file_receive_chunk_from_server")
    {
        server_msg.process_file_receive_chunk_from_server_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__file_receive_completed_from_server")
    {
        server_msg.process_file_receive_completed_from_server_from_server(e.data.value);
    }
}

// main.js is embedded in template.html along with the other client files, and in the node bundle
// it is the startup: main__window_onload wires the page (buttons, keys, panels, local settings), and the
// export seam at the end hands the node runtime what it needs
// it also closes the closure that aes-js.js opens, so it stays the last include

// state private to this file
// true once java has answered whether anyone is looking. java watches the activity while the
// loopback socket only knows whether anyone is connected, so java outranks it
var node_ui_visibility_from_host = false;

/**
 * @brief global mousedown: closes the chat-message context menu, and every other context menu
 *        unless the click landed on a menu item (so the item's own click handler still runs)
 *
 * @param object event -> the mouse event
 *
 * @return void
 */
function main__document_onmousedown(event)
{
    UI.delete_chat_message_contextmenu();

    let is_channel_list_contextmenu_delete_needed = !event.target.classList.contains("context-menu-item");

    if (is_channel_list_contextmenu_delete_needed)
    {
        UI.delete_contextmenus(true);
    }
}

/**
 * @brief the push-to-talk key (Q unless changed in local settings): held down = speaking
 *        in continuous mode the mic is toggled by the button instead, so the key does nothing
 *
 * @param object event -> the keyboard event
 *
 * @return void
 */
function main__document_onkeydown(event)
{
    if (g_is_microphone_always_on == true || g_is_continuous_mic_mode == true)
    {
        return;
    }
    if (event.which == g_push_to_talk_key_code)
    {
        voice__process_start_sending_audio();
    }
}

/**
 * @brief releasing the push-to-talk key stops sending audio
 *
 * @param object event -> the keyboard event
 *
 * @return void
 */
function main__document_onkeyup(event)
{
    if (g_is_microphone_always_on == true || g_is_continuous_mic_mode == true)
    {
        return;
    }
    if (event.which == g_push_to_talk_key_code)
    {
        // the encoder clear moved into voice__process_stop_sending_audio_now: clearing here
        // would wipe the release-hangover tail that is still being captured
        voice__process_stop_sending_audio();
    }
}

/**
 * @brief the android wrapper's simple/advanced mode drives the theme, both ways
 *        simple locks the messenger look; advanced restores the user's saved theme (or the device
 *        default). called at startup
 *
 * @return void
 */
function main__apply_theme_for_app_mode()
{
    // simple = the bluebell messenger look, locked
    if (g_android_app_mode == "simple")
    {
        UI.apply_theme("bluebell", false);
        return;
    }

    // advanced = the full mobile interface. honor a theme the user picked manually (so
    // their choice persists), otherwise fall back to default-mobile
    if (g_android_app_mode == "advanced")
    {
        let saved_theme = null;
        saved_theme = utils__storage_get("lemon_theme");

        if (saved_theme != null && saved_theme != "bluebell" && UI.is_theme_allowed_on_this_device(saved_theme))
        {
            UI.apply_theme(saved_theme, false);
        }
        else
        {
            UI.apply_theme("default-mobile", false);
        }
    }
}

/**
 * @brief stops event bubbling; wired where a click must not reach parent handlers
 *
 * @param object event -> the event to stop
 *
 * @return void
 */
function main__stop_propagation(event)
{
    event.stopPropagation();
}

/**
 * @brief mirrors the three toggle states onto css classes of their toolbar buttons
 *        .sfx-on / .mic-on / .chat-hidden, which themes with state-specific icons (oldschool)
 *        style with !important
 *
 * @return void
 */
function main__sync_toolbar_state_classes()
{
    let sfx = document.getElementById("sound-effects-button");
    let mic = document.getElementById("microphone-always-broadcasting-audio-button");
    let hide = document.getElementById("hide-chat-button");
    if (sfx != null) { sfx.classList.toggle("sfx-on", g_are_sound_effects_enabled == true); }
    if (mic != null) { mic.classList.toggle("mic-on", g_is_microphone_always_on == true); }
    if (hide != null) { hide.classList.toggle("chat-hidden", g_is_chat_hidden == true); }
}

/**
 * @brief the page entry point
 *        detects the android webview and touch devices, applies the predefined server
 *        autoconnect config, sets up ui visibility, gestures and every element's event handlers
 *
 * @return promise ignored by window.onload; resolves once the page is wired
 */
async function main__window_onload()
{
    if (typeof Android !== 'undefined')
    {
        g_is_running_in_android_webview = true;
        g_is_client_running_under_touch_device = true;
        g_is_autoconnect_without_user_action_active = false;
        g_are_server_details_predefined = false;
    }

    // touch detection lives in platform-detection.js; the android webview force above stays
    if (utils__detect_touch_device() == true)
    {
        g_is_client_running_under_touch_device = true;
    }

    if (g_is_running_in_android_webview == true)
    {
        // in android, connection settings are stored in its own container..
        // these ui elements are not needed
        document.getElementById("add-key-button").style.visibility = "hidden";
        document.getElementById("connect-form-sub-container-1").style.visibility = "hidden";
        document.getElementById("connect-form-sub-container-2").style.visibility = "hidden";
    }

    // web: if the server baked connection details into the page, enable autoconnect before the form-visibility logic below runs; hide only the connect form (ip/port), leaving the loading/connecting status visible so the page isn't blank while it connects
    if (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.port && g_is_running_in_android_webview == false)
    {
        g_are_server_details_predefined = true;
        g_is_autoconnect_without_user_action_active = true;
        g_autoconnect_details = {
            host: window.location.hostname,
            port: window.__SERVER_CONFIG__.port,
            keys: window.__SERVER_CONFIG__.keys,
            wss_port: window.__SERVER_CONFIG__.wss_port
        };
        document.getElementById("connect-form-sub-container-1").style.display = "none";
        document.getElementById("connect-form-sub-container-2").style.display = "none";
    }

    if (g_is_running_in_android_webview == false)
    {
        // the address is baked in (injected config, or set by hand in the source), so the fields the
        // bookmarks fill are never consulted: hidden in every theme, and it stays hidden on the
        // connect screen after a disconnect
        if (g_are_server_details_predefined == true)
        {
            document.getElementById("server-bookmarks-container").style.display = "none";
        }

        if (g_are_server_details_predefined == false)
        {
            document.getElementById("connect-form-sub-container-1").style.visibility = "visible";
            document.getElementById("connect-form-sub-container-2").style.visibility = "visible";
            document.getElementById("add-key-button").style.visibility = "visible";
            document.getElementById("connect-button").style.visibility = "visible";
        }

        if (g_are_server_details_predefined == true && g_is_autoconnect_without_user_action_active == false)
        {
            document.getElementById("connect-button").style.visibility = "visible";
        }

        if (g_is_autoconnect_without_user_action_active == true)
        {
            document.getElementById("import-identity-button").style.display = "none";
        }
    }

    asm = create_opus_webassembly_instance(); // do not delete this line

    g_textarea_log = document.getElementById("textarea-log");

    // member-strip gestures: strip themes tap the clones in the right-pane member list, which are
    // rebuilt on every sync, so delegated listeners forward tap (open chat / show channel chat) and
    // hold (row menu / channel switch list) to the real tree rows; touch and mouse both register

    function member_strip_show_channel_switch_menu(click_x, click_y)
    {
        UI.delete_contextmenus();

        let menu_items_html = "";
        for (let i = 0; i < g_channel_list.length; i++)
        {
            let channel = g_channel_list[i];
            let lock_html = (channel.is_using_password == true) ? "🔒 " : "";
            let current_html_class = (parseInt(channel.channel_id) == parseInt(g_current_channel_id)) ? " context-menu-item-disabled" : "";
            // context-menu-item keeps main__document_onmousedown from closing the menu
            // before the item's own click handler can run
            menu_items_html += "<p class='context-menu-item channel-switch-menu-item" + current_html_class + "' data-switch-channel-id='" + channel.channel_id + "'>" + lock_html + chat__sanitize_string(channel.name) + "</p>\n";
        }

        let menu_html = "<div class=\"channel-list-context-menu\" style=\"top: " + click_y + "px; left: " + click_x + "px;\">\n\
                            <div class='channel-list-context-menu-background'>\n\
                            </div>\n\
                            <div class='channel-list-context-menu-items'>\n\
                                " + menu_items_html + "\n\
                            </div>\n\
                        </div>";

        document.getElementById("contextmenus-container").insertAdjacentHTML("beforeend", menu_html);

        let switch_items = document.getElementsByClassName("channel-switch-menu-item");
        for (let i = 0; i < switch_items.length; i++)
        {
            switch_items[i].onclick = function()
            {
                let switch_channel_id = this.getAttribute("data-switch-channel-id");
                UI.delete_contextmenus(true);

                // same flow as double clicking the channel in the tree: handles the
                // current-channel guard and the password dialog for locked channels
                let name_element = document.querySelector('#channel-list-container .single-channel[data-channel-id="' + switch_channel_id + '"] [data-channel-name-id]');
                if (name_element != null)
                {
                    UI.single_channel_doubleclick_join({ stopPropagation: function() {}, target: name_element });
                }
            };
        }
    }

    // true when the gesture landed on a circle and was handled, so the caller stops the event and
    // main__document_onmousedown does not close the menu the gesture just opened
    function member_strip_forward(target_element, click_x, click_y, is_short_click)
    {
        let channel_circle = target_element.closest(".member-list-channel");
        if (channel_circle != null)
        {
            if (is_short_click == true)
            {
                // bring the channel chat back on screen - same as tapping its pill. a pill
                // fresh from the html has no handler yet - give it the standard one
                let channel_pill = document.querySelector('.chat-context-selector[data-chat-context-selector-id="channel-' + channel_circle.getAttribute("data-member-list-channel-id") + '"]');
                if (channel_pill != null)
                {
                    if (channel_pill.onclick == null)
                    {
                        channel_pill.onclick = UI.chat_context_selector_onclick;
                    }

                    channel_pill.click();
                }
            }
            else
            {
                member_strip_show_channel_switch_menu(click_x, click_y);
            }
            return true;
        }

        let clone = target_element.closest(".member-list-client");
        if (clone == null) { return false; }

        // offline contacts have no live row to forward to. a HOLD shows what we know about
        // them; a TAP opens their conversation when the server keeps messages for people
        // who are away (we can tell: we were given their public key), else the info popup
        if (clone.classList.contains("offline-contact") == true)
        {
            let offline_alias_element = clone.querySelector(".client-alias");
            let offline_alias = (offline_alias_element != null) ? offline_alias_element.textContent : "";
            let offline_contact = channel_tree__get_stored_client_by_alias(offline_alias);

            // a TAP opens the conversation like it does for anybody else - the chat itself
            // says whether it can actually be delivered. a HOLD shows their details.
            if (is_short_click == true && offline_contact != null)
            {
                UI.open_offline_chat_context(offline_contact);
            }
            else
            {
                UI.show_offline_contact_info_popup(offline_alias);
            }
            return true;
        }

        let strip_client_id = clone.getAttribute("data-connected-client-id");
        if (strip_client_id == null) { return false; }

        let tree_row = document.querySelector('#channel-list-container .connected-client[data-connected-client-id="' + strip_client_id + '"], #idle-clients-container .connected-client[data-connected-client-id="' + strip_client_id + '"]');
        if (tree_row == null) { return false; }

        // the row handler accepts an explicit target + coordinates, so the strip can
        // hand it the REAL tree row no matter which clone was tapped
        let forwarded_event = { stopPropagation: function() {}, currentTarget: null, which: null };
        UI.connected_user_onmousedown(forwarded_event, true, click_x, click_y, tree_row, is_short_click);
        return true;
    }

    let member_strip_body = document.getElementById("member-list-body");
    if (member_strip_body != null)
    {
        let member_strip_press_timer = null;
        let member_strip_long_press = false;
        let member_strip_moved = false;

        member_strip_body.addEventListener("touchstart", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }

            member_strip_long_press = false;
            member_strip_moved = false;

            const target_element = event.target;
            const click_x = event.touches[0].clientX;
            const click_y = event.touches[0].clientY;

            member_strip_press_timer = window.setTimeout(function() {
                member_strip_long_press = true;
                member_strip_forward(target_element, click_x, click_y, false);
            }, 600);
        }, { passive: true });

        member_strip_body.addEventListener("touchmove", function() {
            // finger is scrolling the strip, not pressing a circle
            member_strip_moved = true;
            clearTimeout(member_strip_press_timer);
        }, { passive: true });

        member_strip_body.addEventListener("touchend", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }

            clearTimeout(member_strip_press_timer);

            if (member_strip_long_press == false && member_strip_moved == false)
            {
                member_strip_forward(event.target, event.changedTouches[0].clientX, event.changedTouches[0].clientY, true);
            }
        });

        member_strip_body.addEventListener("mousedown", function(event) {
            if (g_is_client_running_under_touch_device == true) { return; }

            // left opens, right = actions / channel list - same split the tree rows use
            if (event.which == 1 || event.which == 3)
            {
                if (member_strip_forward(event.target, event.clientX, event.clientY, event.which == 1) == true)
                {
                    event.stopPropagation();
                }
            }
        });
    }

    // holding a channel pill offers the channel switch list too, but only while a
    // theme hides the channel tree (the strip themes) - elsewhere the tree does that job
    let context_pill_container = document.getElementById("chat-context-selectors-container");
    if (context_pill_container != null)
    {
        function is_channel_tree_hidden()
        {
            let tree_pane = document.getElementById("space-devider1");
            return (tree_pane != null && getComputedStyle(tree_pane).display == "none");
        }

        function pill_press_target(event_target)
        {
            let pill = event_target.closest('.chat-context-selector[data-chat-context-selector-type="channel"]');
            return pill;
        }

        let pill_press_timer = null;
        let pill_long_press = false;

        context_pill_container.addEventListener("touchstart", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }
            if (pill_press_target(event.target) == null || is_channel_tree_hidden() == false) { return; }

            pill_long_press = false;
            const click_x = event.touches[0].clientX;
            const click_y = event.touches[0].clientY;

            pill_press_timer = window.setTimeout(function() {
                pill_long_press = true;
                member_strip_show_channel_switch_menu(click_x, click_y);
            }, 600);
        }, { passive: true });

        context_pill_container.addEventListener("touchmove", function() {
            clearTimeout(pill_press_timer);
        }, { passive: true });

        context_pill_container.addEventListener("touchend", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }

            clearTimeout(pill_press_timer);

            // a long press must not ALSO fire the pill's own click
            if (pill_long_press == true)
            {
                event.preventDefault();
            }
        });

        context_pill_container.addEventListener("mousedown", function(event) {
            if (g_is_client_running_under_touch_device == true) { return; }

            if (event.which == 3 && pill_press_target(event.target) != null && is_channel_tree_hidden() == true)
            {
                event.stopPropagation(); // or main__document_onmousedown closes it in the same event
                member_strip_show_channel_switch_menu(event.clientX, event.clientY);
            }
        });
    }

    // long press on an own message opens the delete/edit menu: phones never send the right-click
    // mousedown desktop uses, so a 600 ms hold synthesizes the same event on the same element
    let chat_context_container_element = document.getElementById("chat-context-container");
    if (chat_context_container_element != null)
    {
        let message_press_timer = null;
        let message_press_moved = false;

        chat_context_container_element.addEventListener("touchstart", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }

            let message_p = event.target.closest(".local-single-chat-message-content-p, .local-client-chat-picture-img");
            if (message_p == null) { return; }

            message_press_moved = false;
            const click_x = event.touches[0].clientX;
            const click_y = event.touches[0].clientY;

            message_press_timer = window.setTimeout(function() {
                if (message_press_moved == true) { return; }
                // bubbles:false is load-bearing: a bubbling mousedown would reach main__document_onmousedown,
                // whose menu cleanup would delete the menu created by this very dispatch
                message_p.dispatchEvent(new MouseEvent("mousedown", {
                    bubbles: false,
                    cancelable: true,
                    button: 2,
                    clientX: click_x,
                    clientY: click_y
                }));
            }, 600);
        }, { passive: true });

        chat_context_container_element.addEventListener("touchmove", function() {
            // finger is scrolling the chat, not holding a message
            message_press_moved = true;
            clearTimeout(message_press_timer);
        }, { passive: true });

        chat_context_container_element.addEventListener("touchend", function() {
            clearTimeout(message_press_timer);
        });
    }

    // member-strip preferences, persisted like the theme, as body classes and css variables:
    // orientation (top row or side rail), neighbors only, the own-actions menu, the circle size
    let saved_strip_vertical = null;
    let saved_strip_scale = null;
    let saved_strip_neighbors = null;
    saved_strip_vertical = utils__storage_get("lemon_strip_vertical");
    saved_strip_scale = utils__storage_get("lemon_strip_scale");
    saved_strip_neighbors = utils__storage_get("lemon_strip_neighbors");

    if (saved_strip_vertical == "1") { document.body.classList.add("msgr-vertical"); }
    if (saved_strip_neighbors == "1") { document.body.classList.add("msgr-neighbors-only"); }

    // on unless it was turned off. the badge only appears for people with audio
    // off, so leaving it on costs nothing until there is something worth saying
    let saved_audio_availability = null;
    saved_audio_availability = utils__storage_get("lemon_audio_availability");

    if (saved_audio_availability != "0") { document.body.classList.add("msgr-audio-availability"); }

    function apply_strip_scale(circle_px)
    {
        document.body.style.setProperty("--msgr-circle", circle_px + "px");
        document.body.style.setProperty("--msgr-item", (circle_px + 16) + "px");
        document.body.style.setProperty("--msgr-strip", (circle_px + 52) + "px");
        document.body.style.setProperty("--msgr-name", (circle_px <= 46 ? 10 : (circle_px <= 56 ? 11 : 12)) + "px");
    }

    // legacy values were the steps "1"/"2"/"3"; anything bigger is pixels
    let restored_circle_px = 46;
    if (saved_strip_scale != null)
    {
        if (saved_strip_scale == "2") { restored_circle_px = 54; }
        else if (saved_strip_scale == "3") { restored_circle_px = 62; }
        else if (parseInt(saved_strip_scale) >= 38) { restored_circle_px = parseInt(saved_strip_scale); }
    }
    apply_strip_scale(restored_circle_px);

    document.getElementById("communication-system-head-menu").insertAdjacentHTML("afterbegin",
        "<input type='button' id='msgr-orientation-button' title='people strip: switch between top row and side rail'>" +
        "<input type='button' id='msgr-neighbors-button' title='show only people in your channel'>" +
        "<input type='button' id='msgr-me-button' title='you: avatar and account actions'>" +
        "<input type='range' id='msgr-scale-slider' min='38' max='64' step='2' value='" + restored_circle_px + "' title='people size'>");

    document.getElementById("msgr-orientation-button").onclick = function()
    {
        let vertical_now = document.body.classList.toggle("msgr-vertical");
        utils__storage_set("lemon_strip_vertical", vertical_now ? "1" : "0");
    };

    document.getElementById("msgr-neighbors-button").onclick = function()
    {
        let neighbors_now = document.body.classList.toggle("msgr-neighbors-only");
        utils__storage_set("lemon_strip_neighbors", neighbors_now ? "1" : "0");
    };

    // own actions (set avatar / delete avatar): the same menu a long press on
    // the own row would open - the row itself is hidden in the strip theme
    document.getElementById("msgr-me-button").onclick = function()
    {
        let local_row = document.querySelector("#channel-list-container .connected-local-client");
        if (local_row == null) { return; }

        let forwarded_event = { stopPropagation: function() {}, currentTarget: null, which: null };
        UI.connected_user_onmousedown(forwarded_event, true, window.innerWidth - 190, 48, local_row, false);
    };

    document.getElementById("msgr-scale-slider").oninput = function()
    {
        let circle_px = parseInt(this.value);
        apply_strip_scale(circle_px);
        utils__storage_set("lemon_strip_scale", "" + circle_px);
    };

    // session info: its own icon in the bar, a small card with connection status,
    // ping (heartbeat round trip), traffic counters and session length. values
    // refresh once a second, but only while the card is open.
    document.getElementById("communication-system-head-menu").insertAdjacentHTML("afterbegin",
        "<input type='button' id='msgr-session-button' title='connection and session info'>");

    document.body.insertAdjacentHTML("beforeend",
        "<div id='msgr-session-card'>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>status</span><span id='msgr-session-status'></span></p>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>ping</span><span id='msgr-session-ping'></span></p>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>sent</span><span id='msgr-session-sent'></span></p>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>received</span><span id='msgr-session-received'></span></p>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>session</span><span id='msgr-session-uptime'></span></p>" +
        "</div>");

    function format_byte_count(byte_count)
    {
        if (byte_count >= 1048576) { return (byte_count / 1048576).toFixed(1) + " MB"; }
        if (byte_count >= 1024) { return (byte_count / 1024).toFixed(1) + " KB"; }
        return byte_count + " B";
    }

    function refresh_session_card()
    {
        let now = new Date().valueOf();
        let is_connection_alive = (g_is_authenticated == true && (g_connection_check.last_response_timestamp + g_connection_check.lost_threshold_ms) >= now);

        document.getElementById("msgr-session-status").textContent = is_connection_alive ? "connected" : "not connected";
        document.getElementById("msgr-session-status").style.color = is_connection_alive ? "#3ddc84" : "#e05a4e";
        document.getElementById("msgr-session-ping").textContent = (is_connection_alive && g_session.last_ping_ms >= 0) ? (g_session.last_ping_ms + " ms") : "-";
        document.getElementById("msgr-session-sent").textContent = format_byte_count(g_session.bytes_sent);
        document.getElementById("msgr-session-received").textContent = format_byte_count(g_session.bytes_received);

        let uptime_text = "-";
        if (is_connection_alive == true && g_session.connected_at > 0)
        {
            let total_seconds = Math.floor((now - g_session.connected_at) / 1000);
            uptime_text = Math.floor(total_seconds / 3600) + "h " + Math.floor((total_seconds % 3600) / 60) + "m " + (total_seconds % 60) + "s";
        }
        document.getElementById("msgr-session-uptime").textContent = uptime_text;
    }

    let session_card_refresh_timer = null;

    document.getElementById("msgr-session-button").onclick = function()
    {
        let card = document.getElementById("msgr-session-card");
        let opening = (card.classList.contains("msgr-session-card-open") == false);
        card.classList.toggle("msgr-session-card-open", opening);

        clearInterval(session_card_refresh_timer);
        if (opening == true)
        {
            refresh_session_card();
            session_card_refresh_timer = setInterval(refresh_session_card, 1000);
        }
    };

    // tapping anywhere else closes the card - it is a glance, not a screen
    document.addEventListener("mousedown", function(event) {
        let session_card = document.getElementById("msgr-session-card");
        if (session_card == null || session_card.classList.contains("msgr-session-card-open") == false) { return; }
        if (event.target.closest("#msgr-session-card") != null || event.target.closest("#msgr-session-button") != null) { return; }

        session_card.classList.remove("msgr-session-card-open");
        clearInterval(session_card_refresh_timer);
    });

    // grid layout engine: desktop only. touch keeps the legacy inline-block layout,
    // so there g_layout.grid_active stays false and every layout_* entry point no-ops
    g_layout.panels = {
        channels: document.getElementById("space-devider1"),
        chat: document.getElementById("space-devider2"),
        info: document.getElementById("space-devider-42"),
        input: document.getElementById("space-devider4")
    };
    if (g_is_client_running_under_touch_device == false)
    {
        layout__layout_load_saved_state();
        g_layout.grid_active = true;
        layout__layout_apply();

        document.getElementById("layout-edit-button").onclick = layout__layout_edit_toggle;
        document.getElementById("drag-resize-info").addEventListener("mousedown", function(e) {
            layout__layout_column_drag_start(e, "info");
        }, false);
        document.getElementById("drag-resize-channels").addEventListener("mousedown", function(e) {
            layout__layout_column_drag_start(e, "channels");
        }, false);

        // edit mode: capture-phase so a panel drag wins over every inner click handler
        document.getElementById("communication-system-container").addEventListener("mousedown", layout__layout_edit_mousedown, true);
        document.addEventListener("mousemove", layout__layout_edit_mousemove, false);
        document.addEventListener("mouseup", layout__layout_edit_mouseup, false);
    }
    else
    {
        document.getElementById("layout-edit-button").style.display = "none";
        document.getElementById("drag-resize-info").style.display = "none";
        document.getElementById("drag-resize-channels").style.display = "none";
    }

    document.addEventListener("keydown", main__document_onkeydown);
    document.addEventListener("mousedown", main__document_onmousedown);
    document.addEventListener("keyup", main__document_onkeyup);

    document.getElementById("channel-properties-edit").addEventListener("mousedown", main__stop_propagation);
    document.getElementById("background-container").addEventListener("mousedown", main__stop_propagation);

    document.getElementById("chat-input-container-send-chat-input").onclick = UI.send_chat_input_on_click;

    // the pills shipped in the html (the root channel's) have no handlers until a rebind loop
    // happens to run; bind them now so they work on a fresh session too - opening an offline
    // chat used to leave the channel pill (and the strip's channel circle) dead
    for (let x = 0; x < document.getElementsByClassName("chat-context-selector").length; x++)
    {
        document.getElementsByClassName("chat-context-selector")[x].onclick = UI.chat_context_selector_onclick;
    }
    for (let x = 0; x < document.getElementsByClassName("remove-chat-context-selector").length; x++)
    {
        document.getElementsByClassName("remove-chat-context-selector")[x].onclick = UI.chat_context_remove_onclick;
    }
    document.getElementById("connect-button").onclick = UI.connect_button_onclick;
    connection__wire_server_bookmarks();
    document.getElementById("chat-input-container-text-input").onkeyup = UI.chat_input_on_keyup;
    document.getElementById("choose_image_input").onchange = UI.choose_image_input;
    document.getElementById("choose-file-input").onchange = chat_files__choose_chat_file_input_onchange;
    document.getElementById("image-upload-preview-remove").onclick = chat_files__clear_pending_chat_picture;
    document.getElementById("server-settings-general-file-upload-max-size-input").oninput = chat_files__refresh_file_upload_size_warning;
    document.getElementById("server-settings-general-allow-file-uploads-checkbox").onchange = chat_files__refresh_file_upload_size_visibility;
    document.getElementById("server-settings-general-allow-pictures-checkbox").onchange = chat_files__refresh_picture_size_visibility;
    document.getElementById("server-settings-general-country-blocking-checkbox").onchange = server_settings_tab__refresh_country_blocking_visibility;
    document.getElementById("server-settings-general-display-flags-checkbox").onchange = server_settings_tab__refresh_hide_admin_flag_row_visibility;
    document.getElementById("server-settings-country-block-select").onchange = server_settings_tab__country_block_select_onchange;
    chat_files__setup_chat_file_drag_and_drop();
    chat_files__setup_chat_file_card_glow();
    chat_files__apply_file_upload_policy_to_ui();
    document.getElementById("add-key-button").onclick = UI.add_key_button_on_click;
    document.getElementById("show-hide-log-button").onclick = UI.show_hide_log_on_click;
    document.querySelector("[data-chat-context-remove-selector-id='channel-0']").onclick = UI.chat_context_remove_onclick;
    document.getElementById("close-button").onclick = UI.channel_properties_closebutton_onclick;
    document.getElementById("create-channel-button").onclick = UI.create_channel_button_onclick;
    document.getElementById("edit-channel-button").onclick = UI.edit_channel_button_onclick;
    document.getElementById("channel-properties-limit-clients-checkbox").onchange = UI.refresh_channel_limit_input_visibility;
    document.getElementById("close-button-enter-password").onclick = UI.enter_channel_password_closebutton_onclick;
    document.getElementById("close-button-admin-password").onclick = UI.close_admin_password_context_button;
    document.getElementById("close-button-secret-identity-string").onclick = UI.close_button_secret_identity_string_onclick;
    document.getElementById("close-button-identity-string-use").onclick = UI.close_button_identity_string_use;
    document.getElementById("join-passworded-channel-button").onclick = UI.channel_join_button_onclick;
    document.getElementById("admin-password-send-button").onclick = UI.admin_password_send_button_onclick;
    document.getElementById("clear-chat-button").onclick = UI.clear_chat_button_onclick;
    document.getElementById("hide-chat-button").onclick = UI.hide_chat_container;
    document.getElementById("show-admin-password-entry").onclick = UI.enter_admin_password_button_onclick;
    document.getElementById("choose-song-file-input").onchange = UI.choose_song_file_input_onchange;
    document.getElementById("choose_icon_input").onchange = UI.choose_icon_input_onchange;
    document.getElementById("drag-resize-chat").addEventListener('mousedown', UI.drag_resize_chat_onclick, false);
    document.getElementById("stop-song").onclick = UI.stop_song_onclick;
    document.getElementById("add-new-tag-to-server-button").onclick = UI.add_new_tag_to_server_button_onlick;
    document.getElementById("save-server-settings-button").onclick = UI.save_server_settings_button_onclick;
    document.getElementById("server-settings-log-save-button").onclick = UI.server_settings_log_save_button_onclick;
    document.getElementById("server-settings-log-refresh-button").onclick = UI.server_settings_log_refresh_button_onclick;
    document.getElementById("server-settings-log-clear-button").onclick = UI.server_settings_log_clear_button_onclick;
    document.getElementById("enter-server-settings").onclick = UI.enter_server_settings_onclick;
    document.getElementById("channel-flatten-toggle-button").onclick = UI.channel_flatten_toggle_onclick;
    document.getElementById("show-secret-identity-string").onclick = UI.show_secret_identity_string_onclick;
    document.getElementById("export-identity-file-button").onclick = UI.export_identity_file_button_onclick;
    document.getElementById("choose-identity-file-button").onclick = UI.choose_identity_file_button_onclick;
    document.getElementById("identity-file-input").onchange = UI.identity_file_input_onchange;
    document.getElementById("identity-file-confirm-button").onclick = UI.identity_file_confirm_button_onclick;
    document.getElementById("identity-mode-toggle-link").onclick = UI.identity_mode_toggle_onclick;
    // dropping a .lmn file on the identity dialog behaves like picking it; stopPropagation keeps
    // the drop away from the chat-file handlers
    {
        let identity_dialog = document.getElementById("identity-string-use-container-enter");
        identity_dialog.addEventListener("dragover", function(drag_event) { drag_event.preventDefault(); drag_event.stopPropagation(); });
        identity_dialog.addEventListener("drop", function(drop_event)
        {
            drop_event.preventDefault();
            drop_event.stopPropagation();

            if (document.getElementById("identity-file-mode").style.display == "none")
            {
                UI.identity_mode_toggle_onclick(drop_event);
            }

            if (drop_event.dataTransfer != null && drop_event.dataTransfer.files != null && drop_event.dataTransfer.files.length > 0)
            {
                UI.identity_read_identity_file(drop_event.dataTransfer.files[0]);
            }
        });
    }
    document.getElementById("import-identity-button").onclick = UI.import_identity_button_onclick;
    // top bar: same passphrase dialog; entered while connected it disconnects, regenerates
    // the keypair from the passphrase and reconnects as that identity (see the confirm handler)
    document.getElementById("switch-identity-button").onclick = UI.import_identity_button_onclick;
    document.getElementById("identity-string-use-confirm-button").onclick = UI.identity_string_use_confirm_button;
    document.getElementById("close-button-poke-client").onclick = UI.close_button_poke_client_onclick;
    document.getElementById("close-button-client-info").onclick = UI.close_button_client_info_onclick;
    document.getElementById("close-button-set-new-username").onclick = UI.close_button_set_new_username_onclick;
    document.getElementById("close-button-server-settings").onclick = UI.close_button_server_settings_onclick;
    document.getElementById("poke-client-send-button").onclick = UI.poke_client_send_button_onclick;
    document.getElementById("set-alias-send-button").onclick = UI.set_alias_send_button_onclick;
    document.getElementById("close-button-set-alias").onclick = UI.close_button_set_alias_onclick;

    // channel edit form: clicking the icon box opens the shared icon picker for that channel.
    // no admin gate here: the picker always opens, the server rejects the request if not allowed
    document.getElementById("channel-properties-icon-box").onclick = function()
    {
        if (g_channel_properties_edit_channel_id != null)
        {
            UI.open_channel_icon_picker(g_channel_properties_edit_channel_id);
        }
    };

    // "remove icon" next to the box: same set_channel_icon request with no icon_id, which the
    // server treats as clearing it; its broadcast repaints the tree row and the edit form box
    document.getElementById("channel-properties-icon-remove-button").onclick = function()
    {
        if (g_channel_properties_edit_channel_id != null)
        {
            UI.send_set_channel_icon_request(g_channel_properties_edit_channel_id, "none");
        }
    };

    // adjust-volume dialog: close button + live slider (updates the worklet lane gain as it moves)
    document.getElementById("close-button-adjust-volume").onclick = function()
    {
        document.getElementById("adjust-volume-container").style.display = "none";
    };
    document.getElementById("adjust-volume-slider").oninput = function()
    {
        let target_client_id = this.getAttribute("data-target-client-id");
        let gain = parseInt(this.value) / 100;

        document.getElementById("adjust-volume-value-label").innerHTML = this.value + "%";

        if (target_client_id != null)
        {
            g_client_volume_by_id[target_client_id] = gain;
            audio__set_client_playback_volume(parseInt(target_client_id), gain);
        }
    };
    document.getElementById("set-new-username-send-button").onclick = UI.set_new_username_send_button_onclick;
    document.getElementById("chat-input-file-input-container").onmousedown = UI.chat_input_container_on_mousedown;
    document.getElementById("color-picker-input").addEventListener("input", UI.color_picker_oninput);
    document.getElementById("chat-input-font-size-range").addEventListener("input", UI.chat_input_font_size_range_oninput);
    document.getElementById("hide-show-flags-button").onclick = UI.hide_show_flags_button_onclick;
    document.getElementById("microphone-always-broadcasting-audio-button").onclick = UI.activate_continous_audio_broadcast_onclick;
    document.getElementById("sound-effects-button").onclick = UI.sound_effects_button_onclick;

    // local settings: device-only preferences, no admin involved. new messages only -
    // already-rendered ones keep whatever they had
    document.getElementById("local-settings-button").onclick = function()
    {
        let panel = document.getElementById("local-settings-container");
        document.getElementById("local-settings-show-avatars").checked = g_show_message_avatars;
        document.getElementById("local-settings-rsa-key-bits").value = String(g_rsa_key_bits);
        document.getElementById("local-settings-seen-indicator").checked = g_show_seen_indicator;
        document.getElementById("local-settings-send-seen").checked = g_send_seen_receipts;
        document.getElementById("local-settings-auto-scroll").checked = g_auto_scroll_chat_to_end;
        document.getElementById("local-settings-hide-mic").checked = g_hide_microphone_button;
        document.getElementById("local-settings-sound-effects").checked = g_are_sound_effects_enabled;
        document.getElementById("local-settings-country-flags").checked = (g_show_hide_toggle == false);
        render_mic_mode_controls();
        document.getElementById("local-settings-file-logging").checked = g_is_file_logging_enabled;
        document.getElementById("local-settings-show-log").checked = (document.getElementById("show-hide-log-button").style.display !== "none");
        document.getElementById("local-settings-strip-vertical").checked = document.body.classList.contains("msgr-vertical");
        document.getElementById("local-settings-strip-neighbors").checked = document.body.classList.contains("msgr-neighbors-only");
        document.getElementById("local-settings-audio-availability").checked = document.body.classList.contains("msgr-audio-availability");
        populate_microphone_device_list();

        if (panel.style.display === "none")
        {
            panel.classList.remove("closing");

            // clear it rather than force "block": an inline display beats the stylesheet,
            // and the phone themes need the panel to stay the flex column that lets the
            // row list scroll under a fixed title bar and save button
            panel.style.display = "";
        }
        else
        {
            close_local_settings();
        }
    };

    // fade OUT as well as in - display:none mid-animation just made it vanish.
    // a declaration, not a let: it is referenced above this line too, and hoisting
    // keeps that working if this block is ever reordered
    function close_local_settings()
    {
        let panel = document.getElementById("local-settings-container");
        panel.classList.add("closing");

        window.setTimeout(function()
        {
            panel.style.display = "none";
            panel.classList.remove("closing");
        }, 200);
    }

    // the select and the touch segments are two faces of g_is_continuous_mic_mode; a theme
    // shows one of them, and this paints whichever is on screen from that one variable
    function render_mic_mode_controls()
    {
        document.getElementById("local-settings-mic-mode").value = g_is_continuous_mic_mode ? "continuous" : "push-to-talk";
        document.getElementById("local-settings-ptt-key").value = g_push_to_talk_key_label;

        // the key only means anything in push-to-talk mode
        document.getElementById("local-settings-ptt-key-row").style.display = g_is_continuous_mic_mode ? "none" : "";

        let segments = document.querySelectorAll("#local-settings-mic-mode-segmented .local-settings-segment");

        for (let x = 0; x < segments.length; x++)
        {
            let is_selected = (segments[x].getAttribute("data-mic-mode") === "continuous") === g_is_continuous_mic_mode;
            segments[x].classList.toggle("segment-selected", is_selected);
        }
    }

    // fills the microphone picker with the audio inputs the browser reports. the row always
    // shows; before the microphone has been used once the browser hides the real device list
    // behind the permission, so a disabled hint entry explains why there is nothing to pick yet
    function populate_microphone_device_list()
    {
        let row = document.getElementById("local-settings-mic-device-row");

        if (row == null || navigator.mediaDevices == null || typeof navigator.mediaDevices.enumerateDevices != "function")
        {
            return;
        }

        navigator.mediaDevices.enumerateDevices().then(function(devices)
        {
            // a device with an empty id is the pre-permission placeholder, not a real choice
            let inputs = devices.filter(function(device) { return device.kind === "audioinput" && device.deviceId !== ""; });

            let select = document.getElementById("local-settings-mic-device");
            let html = "<option value=\"\">system default</option>";

            if (inputs.length == 0)
            {
                html += "<option value=\"\" disabled>(use the microphone once to list devices)</option>";
            }

            for (let i = 0; i < inputs.length; i++)
            {
                let label = (typeof inputs[i].label === "string" && inputs[i].label.length > 0) ? inputs[i].label : ("microphone " + (i + 1));
                html += "<option value=\"" + chat__sanitize_string(inputs[i].deviceId) + "\">" + chat__sanitize_string(label) + "</option>";
            }

            select.innerHTML = html;
            select.value = g_selected_microphone_device_id;

            // a remembered device that no longer exists falls back to the default entry
            if (select.value !== g_selected_microphone_device_id)
            {
                select.value = "";
            }

            row.style.display = "";
        }).catch(function() { row.style.display = "none"; });
    }

    // switches the live capture to the chosen device without touching the capture graph: a new
    // source joins the same gain node and the old track just stops. nothing is ever disconnected,
    // because a capture-edge disconnect once stalled chromium's whole mic delivery
    function apply_selected_microphone_device()
    {
        if (g_local_audio_stream == null || navigator.mediaDevices == null)
        {
            return; // the mic is not built yet; the choice applies when it activates
        }

        let switch_constraints = (g_selected_microphone_device_id != "")
            ? { audio: { deviceId: { ideal: g_selected_microphone_device_id } } }
            : { audio: true };

        navigator.mediaDevices.getUserMedia(switch_constraints).then(function(new_stream)
        {
            let old_track = g_local_audio_stream.getAudioTracks()[0];
            let new_track = new_stream.getAudioTracks()[0];

            // the new track inherits the transmit state, so a switch mid-talk keeps talking
            new_track.enabled = (old_track != null) ? old_track.enabled : false;

            let new_source = g_audio_context.createMediaStreamSource(new_stream);
            new_source.connect(g_audio_recorder_gain_node);

            if (old_track != null)
            {
                old_track.stop();
            }

            g_local_audio_stream = new_stream;
            g_audio_input = new_source;

            utils__custom_log("microphone switched to: " + (new_track.label || "default device"));
        }).catch(function(switch_error)
        {
            utils__custom_alert("could not switch the microphone: " + switch_error);
        });
    }

    function apply_mic_mode(is_continuous)
    {
        g_is_continuous_mic_mode = is_continuous;
        utils__storage_set("lemon_continuous_mic", g_is_continuous_mic_mode ? "1" : "0");

        // switching modes mid-transmission: stop cleanly, the button state must not lie
        if (g_is_continuous_mic_mode == false && g_is_continuous_transmission_active == true)
        {
            g_is_continuous_transmission_active = false;
            document.getElementById("microphone-push-to-talk-button-touch-device").classList.remove("mic-continuous-active");
            voice__process_stop_sending_audio();
        }

        render_mic_mode_controls();
    }

    // every setting applies live, so this is a "save" only in the sense of "done"
    document.getElementById("local-settings-close-button").onclick = close_local_settings;
    document.getElementById("local-settings-x-button").onclick = close_local_settings;

    // the people-strip preferences moved here from the toolbar; the toolbar buttons
    // still exist for the strip themes, so both drive the same body classes
    document.getElementById("local-settings-strip-vertical").onchange = function()
    {
        document.body.classList.toggle("msgr-vertical", this.checked);
        utils__storage_set("lemon_strip_vertical", this.checked ? "1" : "0");
    };

    document.getElementById("local-settings-strip-neighbors").onchange = function()
    {
        document.body.classList.toggle("msgr-neighbors-only", this.checked);
        utils__storage_set("lemon_strip_neighbors", this.checked ? "1" : "0");
    };

    document.getElementById("local-settings-audio-availability").onchange = function()
    {
        document.body.classList.toggle("msgr-audio-availability", this.checked);
        utils__storage_set("lemon_audio_availability", this.checked ? "1" : "0");
    };

    // avatar / account actions: opens the same menu the msgr "me" button opens
    document.getElementById("local-settings-avatar-button").onclick = function()
    {
        let local_row = document.querySelector("#channel-list-container .connected-local-client");

        if (local_row == null)
        {
            utils__custom_alert("connect first to change your avatar");
            return;
        }

        close_local_settings();

        let forwarded_event = { stopPropagation: function() {}, currentTarget: null, which: null };
        UI.connected_user_onmousedown(forwarded_event, true, window.innerWidth - 190, 48, local_row, false);
    };

    document.getElementById("local-settings-show-log").onchange = function()
    {
        let log_button = document.getElementById("show-hide-log-button");
        log_button.style.display = this.checked ? "" : "none";

        if (this.checked == false)
        {
            document.getElementById("textarea-log").style.display = "none";
        }

        utils__storage_set("lemon_show_log_button", this.checked ? "1" : "0");
    };

    document.getElementById("local-settings-show-avatars").onchange = function()
    {
        g_show_message_avatars = this.checked;
        utils__storage_set("lemon_show_message_avatars", this.checked ? "1" : "0");
    };

    // changing the key size changes the identity itself, so it only takes effect on the next
    // keypair: the current connection keeps the key it already has
    document.getElementById("local-settings-rsa-key-bits").onchange = function()
    {
        let chosen_bits = parseInt(this.value);

        if (G_ALLOWED_RSA_KEY_BITS.indexOf(chosen_bits) < 0) { return; }

        g_rsa_key_bits = chosen_bits;
        utils__storage_set("lemon_rsa_key_bits", String(chosen_bits));

        // a size the user picked by hand replaces whatever a server last asked for
        g_rsa_key_too_weak_prompted_for_bits = 0;

        utils__custom_alert("identity key size set to " + chosen_bits + " bits. it applies to the next identity you create - use the identity button to switch now");
    };

    document.getElementById("rsa-key-too-weak-no-button").onclick = keys__hide_rsa_key_too_weak_dialog;
    document.getElementById("close-button-rsa-key-too-weak").onclick = keys__hide_rsa_key_too_weak_dialog;

    document.getElementById("rsa-key-too-weak-yes-button").onclick = function()
    {
        let target_bits = parseInt(this.getAttribute("data-target-bits"));

        if (G_ALLOWED_RSA_KEY_BITS.indexOf(target_bits) < 0) { return; }

        g_rsa_key_bits = target_bits;
        utils__storage_set("lemon_rsa_key_bits", String(target_bits));

        keys__hide_rsa_key_too_weak_dialog();
        utils__custom_alert("creating a " + target_bits + "-bit identity key, this can take a while ...");

        // keep the same passphrase where there is one: at the new size it derives a different
        // keypair, but it stays reproducible from what the user already saved
        connection__request_identity((typeof g_identity_string === "string" && g_identity_string.length >= 199) ? g_identity_string : null);
        connection__request_connect("button");
    };
    document.getElementById("local-settings-seen-indicator").onchange = function()
    {
        g_show_seen_indicator = this.checked;
        utils__storage_set("lemon_seen_indicator", this.checked ? "1" : "0");
        chat__render_seen_indicator();
    };

    document.getElementById("local-settings-auto-scroll").onchange = function()
    {
        g_auto_scroll_chat_to_end = this.checked;
        utils__storage_set("lemon_auto_scroll", this.checked ? "1" : "0");
    };

    document.getElementById("local-settings-send-seen").onchange = function()
    {
        g_send_seen_receipts = this.checked;
        utils__storage_set("lemon_send_seen", this.checked ? "1" : "0");
    };
    document.getElementById("local-settings-hide-mic").onchange = function()
    {
        g_hide_microphone_button = this.checked;
        utils__storage_set("lemon_hide_mic", this.checked ? "1" : "0");
        voice__update_microphone_button();
    };

    document.getElementById("local-settings-file-logging").onchange = function()
    {
        g_is_file_logging_enabled = this.checked;

        // java saves it and turns the logging on or off on both sides
        if (typeof Android !== "undefined" && typeof Android.JavaExportSetFileLogging === "function")
        {
            Android.JavaExportSetFileLogging(this.checked);
        }
    };

    document.getElementById("local-settings-sound-effects").onchange = function()
    {
        // route through the one toggle that also syncs the mute gate and the android side
        if (this.checked != g_are_sound_effects_enabled)
        {
            UI.sound_effects_button_onclick();
        }
        utils__storage_set("lemon_sound_effects", this.checked ? "1" : "0");
    };

    document.getElementById("local-settings-country-flags").onchange = function()
    {
        UI.hide_show_flags_button_onclick();
        utils__storage_set("lemon_show_flags", this.checked ? "1" : "0");
    };

    document.getElementById("local-settings-mic-mode").onchange = function()
    {
        apply_mic_mode(this.value === "continuous");
    };

    document.getElementById("local-settings-mic-device").onchange = function()
    {
        g_selected_microphone_device_id = this.value;
        utils__storage_set("lemon_mic_device_id", g_selected_microphone_device_id);
        apply_selected_microphone_device();
    };

    {
        let segments = document.querySelectorAll("#local-settings-mic-mode-segmented .local-settings-segment");

        for (let x = 0; x < segments.length; x++)
        {
            segments[x].onclick = function()
            {
                apply_mic_mode(this.getAttribute("data-mic-mode") === "continuous");
            };
        }
    }

    // press the button, then press the key you want. escape cancels
    document.getElementById("local-settings-ptt-key").onclick = function()
    {
        let key_button = this;
        key_button.value = "press a key…";

        let capture_key = function(event)
        {
            event.preventDefault();
            event.stopPropagation();
            window.removeEventListener("keydown", capture_key, true);

            if (event.key !== "Escape")
            {
                g_push_to_talk_key_code = event.which || event.keyCode;
                g_push_to_talk_key_label = (event.key.length === 1) ? event.key.toUpperCase() : event.key;

                try
                {
                    localStorage.setItem("lemon_ptt_key_code", g_push_to_talk_key_code);
                    localStorage.setItem("lemon_ptt_key_label", g_push_to_talk_key_label);
                }
                catch (e) { }
            }

            key_button.value = g_push_to_talk_key_label;
        };

        window.addEventListener("keydown", capture_key, true);
    };

    // these toggles live in local settings now; hiding the toolbar copies frees icon space
    document.getElementById("sound-effects-button").style.display = "none";
    document.getElementById("hide-show-flags-button").style.display = "none";
    document.getElementById("microphone-always-broadcasting-audio-button").style.display = "none";

    // restore the persisted sound preference; runs after the var initializers, so it wins
    let stored_sound = utils__storage_get("lemon_sound_effects");
    if (stored_sound != null)
    {
        g_are_sound_effects_enabled = (stored_sound === "1");
        sounds__apply_sound_effects_muted();
    }
    main__sync_toolbar_state_classes(); // seed the state classes from the initial bools so oldschool shows the right icons on load
    document.getElementById("musicbot-management-close-button").onclick = UI.musicbot_management_close_button_onclick;
    document.getElementById("musicbot-management-background-container-upload-button-confirm").onclick = UI.musicbot_management_confirm_upload_button_onclick;

    document.querySelector("a-toast").onclick = UI.a_toast_onclick;
    document.addEventListener("contextmenu", e =>
    {
        e.preventDefault();
    });

    if (g_is_client_running_under_touch_device)
    {
        document.getElementById("microphone-push-to-talk-button-touch-device").addEventListener("touchstart", UI.toggle_microphone_onclick);
        document.getElementById("microphone-push-to-talk-button-touch-device").addEventListener("touchend", UI.toggle_microphone_onclick);
    }
    else
    {
        document.getElementById("toggle-microphone").addEventListener("change", UI.toggle_microphone_onclick);
    }

    for (let i = 0; i < document.getElementsByClassName("server-settings-tab-li").length; i++)
    {
        document.getElementsByClassName("server-settings-tab-li")[i].onclick = UI.server_settings_tab_li_onclick;
    }

    // identity list: delete buttons are rendered dynamically, so delegate their clicks. a
    // delete sends the identity hash; the server clears it and strips the holder's tags
    // send an add/remove of a single tag on a stored identity (works offline)
    let send_modify_identity_tag = function(identity_hash, tag_id, add)
    {
        if (identity_hash == null || identity_hash.length == 0) { return; }
        let message_object = { message: { type: "modify_identity_tag", public_key_hash: identity_hash, tag_id: parseInt(tag_id), add: add } };
        connection__send_message_object(message_object);
    };

    // registers (or with an empty alias, unregisters) a STORED identity by hash, so it works
    // whether or not its owner is connected. an empty result is normal: the server refuses
    // silently when the alias is already taken by somebody else
    let send_set_identity_alias = function(identity_hash, alias)
    {
        if (identity_hash == null || identity_hash.length == 0) { return; }

        let message_object = { message: { type: "set_identity_alias_request", public_key_hash: identity_hash, alias: ("" + alias).trim() } };
        connection__send_message_object(message_object);

        // the list is a server-rendered snapshot; ask for a fresh one so the alias and the
        // registered column reflect what the server actually accepted
        setTimeout(function()
        {
            let refresh_object = { message: { type: "request_identity_list" } };
            let refresh_json = connection__process_message_before_sending(refresh_object);
            connection__websocket_worker_send(keys__encrypt_all_message_data_and_convert_to_base64(refresh_json));
        }, 250);
    };

    document.getElementById("server-settings-identities-list").addEventListener("click", function(event)
    {
        // remove a single tag from an identity (the chip's ✕)
        let tag_remove = event.target.closest(".identity-tag-remove");
        if (tag_remove != null)
        {
            send_modify_identity_tag(tag_remove.getAttribute("data-identity-hash"), tag_remove.getAttribute("data-tag-id"), false);
            return;
        }

        // save the alias typed into this row (registers the identity)
        let alias_set_button = event.target.closest(".identity-alias-set-button");
        if (alias_set_button != null)
        {
            let alias_input = document.querySelector('.identity-alias-input[data-identity-hash="' + alias_set_button.getAttribute("data-identity-hash") + '"]');
            send_set_identity_alias(alias_set_button.getAttribute("data-identity-hash"), (alias_input != null) ? alias_input.value : "");
            return;
        }

        // clear the alias (unregisters the identity)
        let alias_clear_button = event.target.closest(".identity-alias-clear-button");
        if (alias_clear_button != null)
        {
            send_set_identity_alias(alias_clear_button.getAttribute("data-identity-hash"), "");
            return;
        }

        // delete the whole identity (the row's ✕)
        let button = event.target.closest(".identity-delete-button");
        if (button == null) { return; }

        let identity_hash = button.getAttribute("data-identity-hash");
        if (identity_hash == null || identity_hash.length == 0) { return; }

        let message_object = { message: { type: "delete_identity", public_key_hash: identity_hash } };
        connection__send_message_object(message_object);
    });

    // enter in the alias field saves it, same as pressing the ✓
    document.getElementById("server-settings-identities-list").addEventListener("keyup", function(event)
    {
        if (event.key != "Enter") { return; }

        let alias_input = event.target.closest(".identity-alias-input");
        if (alias_input == null) { return; }

        send_set_identity_alias(alias_input.getAttribute("data-identity-hash"), alias_input.value);
    });

    // give an identity a tag (the "+ tag" dropdown)
    document.getElementById("server-settings-identities-list").addEventListener("change", function(event)
    {
        let select = event.target.closest(".identity-tag-add");
        if (select == null || select.value === "") { return; }
        send_modify_identity_tag(select.getAttribute("data-identity-hash"), select.value, true);
        select.value = "";
    });

    // make the server settings window draggable by its left nav column (tab clicks still work,
    // since a click without movement does not reposition the window)
    (function() {
        let settings_window = document.getElementById("server-settings-tab");
        let drag_handle = document.getElementById("server-settings-tab-subcontainer");
        let is_dragging = false;
        let drag_offset_x = 0;
        let drag_offset_y = 0;

        if (settings_window != null && drag_handle != null)
        {
            drag_handle.addEventListener("mousedown", function(e) {
                let rect = settings_window.getBoundingClientRect();
                is_dragging = true;
                drag_offset_x = e.clientX - rect.left;
                drag_offset_y = e.clientY - rect.top;
                e.preventDefault();
            });

            document.addEventListener("mousemove", function(e) {
                if (is_dragging == false)
                {
                    return;
                }
                settings_window.style.left = (e.clientX - drag_offset_x) + "px";
                settings_window.style.top = (e.clientY - drag_offset_y) + "px";
            });

            document.addEventListener("mouseup", function() {
                is_dragging = false;
            });
        }
    })();

    // every popup drags by its title bar; the drag pins it with an inline fixed position, and the pin
    // is cleared on every re-open so popups start centered (a pin taken while hidden was 0,0 forever)
    (function() {
        let dragging = null, ox = 0, oy = 0;
        let bars = document.querySelectorAll('[id="menu-bar-container"]');

        let clear_drag_pin = function(dialog) {
            dialog.style.position = "";
            dialog.style.margin = "";
            dialog.style.left = "";
            dialog.style.top = "";
        };

        // recenter on every open: watch each dialog's container for its display flipping on
        let recenter_observer = new MutationObserver(function(mutations) {
            for (let i = 0; i < mutations.length; i++) {
                let container = mutations[i].target;
                if (container.style.display == "none" || container.style.display == "") { continue; }
                let dialog = container.querySelector('[data-recenter-on-open="true"]');
                if (dialog != null)
                {
                    // center by js instead of trusting theme css: some themes (oldschool)
                    // give the overlay container zero size, which parked dialogs top-left
                    clear_drag_pin(dialog);
                    dialog.style.position = "fixed";
                    dialog.style.margin = "0";
                    // measure with offsetWidth/Height (layout size) not getBoundingClientRect:
                    // a theme's pop-in scale animation (e.g. termix) makes the client rect
                    // smaller than the real box and pushed the centering off. center on both axes.
                    let dw = dialog.offsetWidth;
                    let dh = dialog.offsetHeight;
                    dialog.style.left = Math.max(0, Math.round((window.innerWidth - dw) / 2)) + "px";
                    dialog.style.top = Math.max(0, Math.round((window.innerHeight - dh) / 2)) + "px";
                }
            }
        });

        for (let i = 0; i < bars.length; i++)
        {
            let dialog = bars[i].parentElement;
            dialog.setAttribute("data-recenter-on-open", "true");
            if (dialog.parentElement != null)
            {
                recenter_observer.observe(dialog.parentElement, { attributes: true, attributeFilter: ["style"] });
            }

            bars[i].addEventListener("mousedown", function(e) {
                if (e.target.closest(".close-button") != null) { return; }
                let dialog = this.parentElement;
                let rect = dialog.getBoundingClientRect();
                if (rect.width == 0 && rect.height == 0) { return; } // no layout (hidden): a pin here would stick the dialog to 0,0
                dragging = dialog;
                ox = e.clientX - rect.left;
                oy = e.clientY - rect.top;
                dialog.style.position = "fixed";
                dialog.style.margin = "0";
                dialog.style.left = rect.left + "px";
                dialog.style.top = rect.top + "px";
                e.preventDefault();
            });
        }
        document.addEventListener("mousemove", function(e) {
            if (dragging == null) { return; }
            dragging.style.left = (e.clientX - ox) + "px";
            dragging.style.top = (e.clientY - oy) + "px";
        });
        document.addEventListener("mouseup", function() { dragging = null; });
    })();

    for (let i = 0; i < document.getElementsByClassName("choose-theme-item").length; i++)
    {
        document.getElementsByClassName("choose-theme-item")[i].onclick = UI.choose_theme_item_onclick;
    }

    // on a touch device, hide every theme not tagged mobile-capable, so a phone can only pick
    // themes that are actually laid out for it (currently just the "mobile" theme)
    if (g_is_client_running_under_touch_device)
    {
        let theme_items = document.getElementsByClassName("choose-theme-item");
        for (let i = 0; i < theme_items.length; i++)
        {
            if (theme_items[i].getAttribute("data-mobile") !== "true")
            {
                theme_items[i].style.display = "none";
            }
        }
    }

    // browsers create an AudioContext suspended unless it is made during a user gesture; resume it on the first click/tap so audio works after an autoconnect, with no dedicated connect click.
    // the context that matters is audio_context (created at authentication time, all voice/music playback runs through it).
    // the listeners must stay attached until audio_context exists and is running: a gesture can fire before authentication creates it
    let unlock_audio_on_first_user_gesture = function()
    {
        if (typeof g_audio_context !== "undefined" && g_audio_context != null && g_audio_context.state === "suspended")
        {
            g_audio_context.resume();
        }

        if (typeof g_audio_context !== "undefined" && g_audio_context != null && g_audio_context.state === "running")
        {
            document.removeEventListener("click", unlock_audio_on_first_user_gesture, true);
            document.removeEventListener("touchend", unlock_audio_on_first_user_gesture, true);
            document.removeEventListener("keydown", unlock_audio_on_first_user_gesture, true);
        }
    };
    document.addEventListener("click", unlock_audio_on_first_user_gesture, true);
    document.addEventListener("touchend", unlock_audio_on_first_user_gesture, true);
    document.addEventListener("keydown", unlock_audio_on_first_user_gesture, true);

    g_data_processing_worker = audio_opus_glue__create_new_webworker_in_same_file("data_processing_worker");
    g_websocket_worker = audio_opus_glue__create_new_webworker_in_same_file("websocket_worker");
    g_opus_encoder_worker = audio_opus_glue__create_new_webworker_in_same_file("opus_encoder_worker");
    g_opus_decoder_worker = audio_opus_glue__create_new_webworker_in_same_file("opus_decoder_worker");
    g_minimp3_worker = audio_opus_glue__create_new_webworker_in_same_file("minimp3_worker");

    // restore a persisted identity: the 200-char passphrase deterministically recreates the keypair,
    // so the same identity survives relaunches. it is private-key-equivalent in localStorage, hence
    // server opt-in only (persist_identity in the served config); off keeps every window a fresh identity
    let persist_identity_enabled = (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.persist_identity === true);

    // avatars: server opt-in (default off) + the accepted max upload size (default 50 KB)
    g_server_policy.allow_avatars = (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.allow_avatars === true);
    if (g_server_policy.allow_avatars == true && typeof window.__SERVER_CONFIG__.avatar_max_size === "number" && window.__SERVER_CONFIG__.avatar_max_size > 0)
    {
        g_server_policy.avatar_max_size = window.__SERVER_CONFIG__.avatar_max_size;
    }

    let persisted_identity = null;
    if (persist_identity_enabled == true)
    {
        persisted_identity = utils__storage_get("lemon_identity_string");
    }

    // only trust a stored value of the expected length; a short/corrupt one would silently seed
    // a DIFFERENT keypair, so fall back to a fresh random identity in that case
    let use_persisted_identity = (persisted_identity != null && persisted_identity.length >= 199);

    // the android webview talks to node, which owns the identity - no keypair needed there
    if (typeof Android === "undefined")
    {
        connection__request_identity(use_persisted_identity ? persisted_identity : null);
    }

    g_minimp3_worker.postMessage({
        type: "init"
    });

    // set css settings, set default theme
    // chat will support few themes at most, its up to people to modify their client.html to use own theme

    for (let i = 0; i < document.getElementsByClassName("style-theme-html-element").length; i++)
    {
        document.getElementsByClassName("style-theme-html-element")[i].setAttribute("media","max-width: 1px");
    }

    if (g_is_client_running_under_touch_device)
    {
        document.getElementById("style-theme-default-mobile").removeAttribute("media"); // set default theme
    }
    else
    {
        document.getElementById("style-theme-default").removeAttribute("media"); // set default theme
    }

    // server-baked default theme (from the injected config); applied unless the user has their own saved choice. persist=false so it never sticks.
    // on a phone a desktop theme is ignored here so the mobile default set above stays in effect
    let startup_theme_applied = false;

    if (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.theme && UI.is_theme_allowed_on_this_device(window.__SERVER_CONFIG__.theme))
    {
        UI.apply_theme(window.__SERVER_CONFIG__.theme, false);
        startup_theme_applied = true;
    }

    // restore a theme the user previously picked (from the in-app menu); wins over the server default.
    // a saved desktop theme is ignored on a phone (keeps the mobile default)
    let saved_theme = utils__storage_get("lemon_theme");
    if (saved_theme != null && UI.is_theme_allowed_on_this_device(saved_theme))
    {
        UI.apply_theme(saved_theme, false);
        startup_theme_applied = true;
    }

    // no server-baked theme and nothing saved: activate the default sheet explicitly.
    // without this ALL four theme <style> elements stay active at once and the cascade
    // mixes them (the last sheet mostly wins), which is never an intended look
    if (startup_theme_applied == false)
    {
        UI.apply_theme(g_is_client_running_under_touch_device ? "default-mobile" : "default", false);
    }

    // the wrapper app's mode drives the theme (both directions). runs after the restore
    // above so it wins, and is re-run whenever android pushes a mode change
    main__apply_theme_for_app_mode();

    // restore the flat-channel-list preference (it only has a visible effect under a theme that
    // styles the .channels-flattened class, i.e. termix, but the class is applied regardless so
    // switching back to termix keeps the choice)
    g_is_channel_list_flattened = (utils__storage_get("lemon_channels_flat") == "1");
    UI.apply_channel_flatten_state();

    if (g_is_running_in_android_webview)
    {
        g_send_go_to_idle_mode_request = android_js_bridge.send_go_to_idle_mode_request_android;
        g_send_come_from_idle_mode_request = android_js_bridge.send_come_from_idle_mode_request_android;
        g_mark_call_accept_presence = android_js_bridge.mark_call_accept_presence_android;
        g_set_username_on_connect = android_js_bridge.set_username_on_connect_android;
        g_accept_current_settings_from_android = android_js_bridge.accept_current_settings_from_android;
        g_nudge_loopback_reattach = android_js_bridge.nudge_loopback_reattach_android;
        g_show_connection_phase = android_js_bridge.show_connection_phase_android;

        // a push that raced ahead of this wiring was held by the page shim - use it now
        if (typeof g_pending_android_settings === "string" && g_pending_android_settings != null)
        {
            console.log("connect-path: applying settings that arrived before onload");
            g_accept_current_settings_from_android(g_pending_android_settings);
            g_pending_android_settings = null;
        }

        // lets a theme style itself for the wrapper app (the strip themes give the settings gear a
        // real slot in its top bar; css alone cannot tell app from browser)
        document.body.classList.add("android-app");

        // the app loads client.html from its assets, so the served config (and allow_avatars) never
        // arrives; assume avatars here, a server that has them off just ignores the upload
        g_server_policy.allow_avatars = true;

        document.getElementById("import-identity-button").style.display = "none";

        // the bar gear stays hidden on the connect screen (the in-flow button below
        // owns that state) and appears once connected. tapped while connected it warns
        // first: the user is about to edit the details of the connection he is on
        document.getElementById("android-settings-button").onclick = function() {
            if (g_is_authenticated)
            {
                document.getElementById("android-settings-warning-container").style.display = "block";
                return;
            }
            Android.JavaExportGoToSettings();
        }

        document.getElementById("android-settings-warning-continue-button").onclick = function() {
            document.getElementById("android-settings-warning-container").style.display = "none";
            Android.JavaExportGoToSettings();
        }

        let close_settings_warning = function() {
            document.getElementById("android-settings-warning-container").style.display = "none";
        }
        document.getElementById("android-settings-warning-cancel-button").onclick = close_settings_warning;
        document.getElementById("close-button-android-settings-warning").onclick = close_settings_warning;

        // in-flow twin of the top-bar gear: on the not-connected screen the user
        // should not have to hunt a corner icon to find where the address lives
        document.getElementById("android-server-settings-container").style.display = "";
        document.getElementById("android-server-settings-button").onclick = function() {
            Android.JavaExportGoToSettings();
        }

        Android.JavaExportRequestCurrentSettingsFromAndroid();

        // the settings callback sets the connection target; the driver takes it from there
    }
    else if (g_are_server_details_predefined == true)
    {
        // website with baked-in config: connection__request_connect dials it under autoconnect
        connection__request_connect("settings");
    }

    // the page loads with the spinner up (see the template); arm its reveal deadline so a
    // client that connects to nothing still shows the connect page after a moment.
    // the setter call paints the theme's background onto it (opaque page one)
    connection__set_connect_holdback_loader_visible(true);
    g_connect_holdback_started = new Date().valueOf();
    g_connect_holdback_deadline = g_connect_holdback_started + 2500;
    setTimeout(connection__connect_holdback_check, 250);

    // a browser client in manual mode knows at load that nothing will dial: no spinner
    if (g_is_running_in_android_webview == false && g_is_autoconnect_without_user_action_active == false)
    {
        connection__reveal_connect_page();
    }

    connection__start_connection_status_ticker();
    connection__connection_driver();
}

// needs to be checked so webworkers that this code is shared with do not try to use window object

if (typeof window !== 'undefined')
{
    window.onload = main__window_onload;
}

// the export seam for the headless node build: every g_* and both message tables are locals of
// moduleFactory, and this object is what a node host gets from module.exports = factory();
// a page takes root.what = factory() and simply discards it
return {
    server_msg: server_msg,
    client_msg: client_msg,
    // the java -> js entry points (messages.js:3183). the service runtime needs these:
    // accept_current_settings_from_android is how the android settings screen reaches the client
    android_js_bridge: android_js_bridge,

    // call once before connecting: stands up the transport and the log sink
    init_node_runtime: android_host__init_node_runtime,

    // the webview handover: false closes the socket and parks reconnect, true re-arms
    node_set_connection_wanted: android_host__node_set_connection_wanted,

    /**
     * @brief sets the username this client asks for while connecting, because a headless host has no page variable to edit
     *
     * @param string username -> the name, "" goes back to the assigned one
     *
     * @return void
     */
    node_set_chosen_username: function(username)
    {
        g_chosen_username = (typeof username === "string") ? username : "";
    },

    /**
     * @brief a plaintext request from the loopback ui: encrypts it and sends it to the real server
     *
     * @param string json_string -> the request as json text
     *
     * @return void
     */
    node_forward_raw_request: function(json_string)
    {
        connection__websocket_worker_send(keys__encrypt_all_message_data_and_convert_to_base64(json_string));
    },

    /**
     * @brief registers the callback that gets every decrypted server frame raw; feeds the ui replay
     *
     * @param function callback -> callback(json_string), null clears it
     *
     * @return void
     */
    set_frame_listener: function(callback)
    {
        g_node_frame_listener = (typeof callback === "function") ? callback : null;
    },

    /**
     * @brief registers a callback for every connection status change; additive, every caller gets called
     *
     * @param function callback -> callback(status)
     *
     * @return void
     */
    set_connection_status_listener: function(callback)
    {
        if (typeof callback === "function") { g_connection_status_listeners.push(callback); }
    },

    /**
     * @brief registers the callback that gets the unread total whenever it changes; drives the app icon badge
     *
     * @param function callback -> callback(total), null clears it
     *
     * @return void
     */
    set_unread_listener: function(callback)
    {
        g_node_unread_listener = (typeof callback === "function") ? callback : null;
    },

    /**
     * @brief registers the callback for an incoming call: headless node's only route to the native accept/decline screen
     *
     * @param function callback -> callback(caller_username, channel_id), null clears it
     *
     * @return void
     */
    set_incoming_call_listener: function(callback)
    {
        g_node_incoming_call_listener = (typeof callback === "function") ? callback : null;
    },

    /**
     * @brief a ui attached to or left the loopback
     *        while one is attached the user is looking, so node stops counting unread messages and
     *        clears what it accumulated; the ui owns the badge then
     *
     * @param boolean is_attached -> true when a ui is attached
     * @param boolean is_from_host -> true when java said so; the webview redialling in the background is not the user coming back
     *
     * @return void
     */
    node_set_ui_attached: function(is_attached, is_from_host)
    {
        if (is_from_host === true)
        {
            node_ui_visibility_from_host = true;
        }
        else if (node_ui_visibility_from_host == true)
        {
            // the webview redialling the loopback in the background is not the user
            // coming back, and java has already said so
            return;
        }

        let was_attached = g_node_has_attached_ui;
        g_node_has_attached_ui = (is_attached === true);

        if (g_node_has_attached_ui == true)
        {
            g_channel_unread_counts = {};

            for (let i = 0; i < g_client_list.length; i++)
            {
                g_client_list[i].unread_count = 0;
            }

            chat__update_app_unread_badge();
        }

        // headless node is a client nobody is sitting at, so it belongs in idle -
        // that is what lets other people CALL this phone. without this it just stood
        // in the root channel looking present
        if (g_is_authenticated == true && was_attached != g_node_has_attached_ui)
        {
            android_host__node_apply_idle_for_ui_state();
        }
    },

    /**
     * @brief java's connectivity watch, the only trustworthy "is there a network" on android
     *
     * @param boolean is_available -> true while wifi or mobile data is on
     *
     * @return void
     */
    node_set_device_network: function(is_available)
    {
        g_device_has_network = (is_available === true);

        // say it immediately - the retry text would otherwise keep the stale guess
        if (g_device_has_network === false && g_is_authenticated == false)
        {
            g_last_disconnect_reason = "no network connection (wifi and mobile data are off)";
            connection__report_connection_status(g_connection_status.state, g_last_disconnect_reason);
        }
    },

    /**
     * @brief the current connection status
     *
     * @return object g_connection_status, the state and its reason
     */
    get_connection_status: function()
    {
        return g_connection_status;
    },

    /**
     * @brief a ui attached to the loopback wants to be online; an attach is always the downstream of an authorized decision, so the driver dials for it
     *
     * @return void
     */
    node_connect_intent: function()
    {
        if (g_have_received_android_settings == true)
        {
            connection__request_connect("attach");
        }
    },

    /**
     * @brief the cached authentication frame, whenever it arrived; a replay leads with it regardless of start order
     *
     * @return string|null the frame, null before authentication
     */
    get_auth_frame: function()
    {
        return g_node_cached_auth_frame;
    },

    /**
     * @brief registers a callback fired after every dispatched message, the host's "state may have changed" signal; additive, every caller gets called
     *
     * @param function callback -> callback(message_type, had_error)
     *
     * @return void
     */
    set_on_message_processed: function(callback)
    {
        if (typeof callback === "function") { g_node_message_listeners.push(callback); }
    },

    /**
     * @brief a snapshot of the shared state for the host, built fresh per call since several entries are reassigned rather than mutated
     *        read-only: the arrays come back live, and writing through them bypasses the handlers' invariants
     *
     * @return object the client and channel lists, chat contexts, tags, icons, the current channel and its keys, the local ids and the connection flags
     */
    read_state: function()
    {
        return {
            g_client_list: g_client_list,
            g_channel_list: g_channel_list,
            g_chat_context_array: g_chat_context_array,
            g_map_client_id_to_array_index: g_map_client_id_to_array_index,
            g_offline_client_list: g_offline_client_list,
            g_chat_message_author_public_keys: g_chat_message_author_public_keys,
            g_tags: g_tags,
            g_icons: g_icons,
            current_channel_id: g_current_channel_id,
            current_channel_keys: g_current_channel_keys,
            local_client_id: g_local_client_id,
            g_local_username: g_local_username,
            g_current_chat_context_id: g_current_chat_context_id,
            g_chat_message_receiver_type: g_chat_message_receiver_type,
            g_is_client_list_retrieved: g_is_client_list_retrieved,
            g_is_channel_list_retrieved: g_is_channel_list_retrieved,
            // the live connection signal (g_is_websocket_connected is a dead flag, see ui.js)
            g_is_authenticated: g_is_authenticated,
            g_is_microphone_always_on: g_is_microphone_always_on,
            g_have_received_android_settings: g_have_received_android_settings
        };
    }
};
}));


