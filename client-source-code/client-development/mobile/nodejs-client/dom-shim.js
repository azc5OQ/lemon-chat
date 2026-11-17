// provides browser placeholders for the headless shared client

// defining document makes some dependencies select their browser implementation

if (typeof document === "undefined")
{
    // keep collections empty so render loops terminate
    // return placeholder elements for direct index access
    /**
     * @brief creates an empty collection that tolerates direct indexed lookups
     *
     * @return object -> collection proxy with placeholder elements
     */
    function make_dead_collection()
    {
        return new Proxy([], {
            get: function(target, key)
            {
                if (key === "length")
                {
                    return 0;
                }
                if (typeof key === "string" && /^-?\d+$/.test(key))
                {
                    return make_dead_element();
                }
                return target[key];
            }
        });
    }

    // provide the element methods used by shared code

    /**
     * @brief creates a nonrendering element with the methods shared client code expects
     *
     * @return object -> placeholder element
     */
    function make_dead_element()
    {
        let element = {
            style: {
                // provide the style methods used by avatar rendering
                setProperty: function()
                {
                },
                removeProperty: function()
                {
                },
                getPropertyValue: function()
                {
                    return "";
                }
            },
            dataset: {},
            classList: {
                add: function()
                {
                },
                remove: function()
                {
                },
                toggle: function()
                {
                },
                // no visual class is active in Node
                contains: function()
                {
                    return false;
                }
            },
            children: make_dead_collection(),
            childNodes: make_dead_collection(),
            innerHTML: "",
            textContent: "",
            innerText: "",
            value: "",
            checked: false,
            open: false,
            id: "",
            className: "",
            appendChild: function(child)
            {
                return child;
            },
            removeChild: function(child)
            {
                return child;
            },
            replaceChildren: function()
            {
            },
            close: function()
            {
            },
            showModal: function()
            {
            },
            insertAdjacentHTML: function()
            {
            },
            insertBefore: function(child)
            {
                return child;
            },
            setAttribute: function()
            {
            },
            getAttribute: function()
            {
                return null;
            },
            removeAttribute: function()
            {
            },
            addEventListener: function()
            {
            },
            removeEventListener: function()
            {
            },
            remove: function()
            {
            },
            focus: function()
            {
            },
            blur: function()
            {
            },
            click: function()
            {
            },
            scrollIntoView: function()
            {
            },
            getBoundingClientRect: function()
            {
                return { top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 };
            },
            querySelector: function()
            {
                return make_dead_element();
            },
            closest: function()
            {
                return make_dead_element();
            },
            querySelectorAll: function()
            {
                return make_dead_collection();
            },
            getElementsByClassName: function()
            {
                return make_dead_collection();
            },
            getElementsByTagName: function()
            {
                return make_dead_collection();
            },

            // create child placeholders lazily to avoid recursive construction

            get firstElementChild()
            {
                return make_dead_element();
            },
            get lastElementChild()
            {
                return make_dead_element();
            },

            // return null so child removal loops terminate

            get firstChild()
            {
                return null;
            },
            get lastChild()
            {
                return null;
            }
        };

        return element;
    }

    // share the location placeholder between document and window
    let dead_location = {
        href: "",
        protocol: "http:",
        hostname: "",
        host: "",
        port: "",
        reload: function()
        {
        }
    };

    global.document = {
        lemonchat_headless: true,

        getElementById: function()
        {
            return make_dead_element();
        },
        querySelector: function()
        {
            return make_dead_element();
        },
        querySelectorAll: function()
        {
            return make_dead_collection();
        },
        getElementsByClassName: function()
        {
            return make_dead_collection();
        },
        getElementsByTagName: function()
        {
            return make_dead_collection();
        },
        createElement: function()
        {
            return make_dead_element();
        },
        createTextNode: function()
        {
            return make_dead_element();
        },
        createDocumentFragment: function()
        {
            return make_dead_element();
        },
        addEventListener: function()
        {
        },
        removeEventListener: function()
        {
        },
        body: make_dead_element(),
        documentElement: make_dead_element(),
        location: dead_location
    };

    global.window = global;

    // keep Node's existing timers and globals

    let window_members = {
        addEventListener: function()
        {
        },
        removeEventListener: function()
        {
        },
        getComputedStyle: function()
        {
            return {};
        },
        innerWidth: 0,
        innerHeight: 0,
        location: dead_location,
        scrollTo: function()
        {
        },
        requestAnimationFrame: function(callback)
        {
            return setTimeout(callback, 16);
        },
        cancelAnimationFrame: function(id)
        {
            return clearTimeout(id);
        }
    };

    for (let member_name in window_members)
    {
        if (typeof global[member_name] === "undefined")
        {
            global[member_name] = window_members[member_name];
        }
    }

    global.navigator = { userAgent: "nodejs-mobile" };

    // discard rendering calls in Node
    global.UI = new Proxy({}, {
        get: function()
        {
            return function()
            {
            };
        }
    });

    // discard sound playback in Node
    global.g_sound_effects = new Proxy({}, {
        get: function() {
            return { play: function()
            {
            }, pause: function()
            {
            }, currentTime: 0, volume: 0 };
        }
    });

    // expose the shim window to the AES library
    if (typeof globalThis.self === "undefined")
    {
        globalThis.self = globalThis.window;
    }

    // set worker flags for the headless bundle
    globalThis.THREAD_NAME = "node-service";
    globalThis.DBG_WORKER_BOOT_LOG = false;

    // keep the SHA256 library on its browser path
    globalThis.JS_SHA256_NO_NODE_JS = true;
}

// keep ui.js and sounds.js outside the headless bundle
// their declarations would replace these placeholders
// keep sibling getters absent so sibling traversal terminates
