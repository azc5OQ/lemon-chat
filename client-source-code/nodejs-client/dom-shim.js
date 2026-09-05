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
