// Lemon Chat desktop app. Electron opens the built client.html in a window. The page itself is not changed.
//
// Push-to-talk while another program is in front. A browser page receives keys only while its window has the
// keyboard focus. A small helper program (ptt-helper, one per operating system, see ../ptt-helper) reports the
// presses and releases of the page's talk key from anywhere on the desktop. This file passes them to the page as
// keyboard events on the document, which is where the page listens for its talk key. The page keeps its talk key in
// localStorage under lemon_ptt_key_code. The local settings write it there, and the page reads it at every start.
// Without a stored key the page uses Q (81). This file reads the key each time the window loses the focus, so a key
// changed in the settings is used.
//
// This file writes "watch <keyCode>" lines to the standard input of the helper, where 0 means no key. The helper
// writes "method <name>", "down", "up" and "error <text>" lines to its standard output.
//
// Chromium writes localStorage to disk with a delay. This file has it written at once when the window loses the
// focus and before the app quits, so that a crash or a forced end keeps the settings.
//
// The page keeps bookmarks (lemon_server_bookmarks) but no last used server. After each successful login this file
// stores the address and port of the connect window in localStorage (lemon_electron_last_server). At the next start
// it fills them in, as long as the fields still hold the values of the page template, so that a client.html
// configured with server details keeps its own.
'use strict';

const { app, BrowserWindow, Menu, desktopCapturer, dialog, session, shell } = require('electron');
const child_process = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');
const url = require('node:url');

// permissions the page may use, which are microphone and camera, screen sharing, notifications, clipboard and full screen
const ALLOWED_PERMISSIONS = new Set(['media', 'display-capture', 'notifications', 'clipboard-read',
    'clipboard-sanitized-write', 'fullscreen', 'pointerLock']);
const PAGE_FILE = path.join(__dirname, 'client.html');
// the screen sharing chooser lists at most this many screens and windows
const DISPLAY_SOURCE_LIMIT = 12;
// runs in the page after it has loaded, fills in the last used server and stores the server of each login
const LAST_SERVER_SCRIPT = '(function() {'
    + ' var address = document.getElementById("input-ip-address");'
    + ' var port = document.getElementById("input-port-number");'
    + ' var is_saved = false;'
    + ' if (address == null || port == null) { return; }'
    + ' try {'
    + ' var last = JSON.parse(localStorage.getItem("lemon_electron_last_server") || "null");'
    + ' if (last != null && typeof last.host === "string" && last.host.length > 0'
    + ' && address.value === address.defaultValue && port.value === port.defaultValue) {'
    + ' address.value = last.host; port.value = String(last.port); }'
    + ' } catch (error) {}'
    + ' new MutationObserver(function() {'
    + ' var is_logged_in = document.querySelector(".connected-local-client") != null;'
    + ' if (is_logged_in && !is_saved) {'
    + ' is_saved = true;'
    + ' try { localStorage.setItem("lemon_electron_last_server", JSON.stringify({host: address.value.trim(), port: port.value.trim()})); }'
    + ' catch (error) {} }'
    + ' if (!is_logged_in) { is_saved = false; }'
    + ' }).observe(document.body, {childList: true, subtree: true});'
    + ' })();';

let main_window = null;
let ptt_helper = null;
// "hook", "evdev", "portal", "x11", "eventtap", "poll" or "none", as reported by the helper
let ptt_helper_method = '';
let ptt_watched_key_code = 0;

/**
 * @brief opens an address in the system browser. Only http and https addresses are opened.
 *
 * @param string address -> the address the page tried to open
 *
 * @return void
 */
function electron__open_external(address)
{
    // web addresses only
    if (/^https?:\/\//i.test(address))
    {
        shell.openExternal(address);
    }
}

/**
 * @brief checks whether an address is the app's own client.html, with or without a query or fragment
 *
 * @param string address -> the address of a navigation
 *
 * @return boolean -> true for client.html of this app
 */
function electron__is_page_address(address)
{
    // file address of the page
    try
    {
        let target = new URL(address);
        return target.protocol === 'file:' && path.resolve(url.fileURLToPath(target)) === path.resolve(PAGE_FILE);
    }
    catch (error)
    {
        return false;
    }
}

/**
 * @brief passes a press or release of the talk key to the page as a keyboard event on the document. Before a
 *        press, a text field that still has the keyboard focus inside the page loses it, because the page ignores
 *        the talk key while the user types, and nobody types in a window that is in the background. After the
 *        release the field gets the focus back, so the text cursor is where the user left it.
 *
 * @param string type -> "keydown" or "keyup"
 *
 * @return void
 */
function electron__send_key_to_page(type)
{
    // window and key known
    if (main_window == null || main_window.isDestroyed() || ptt_watched_key_code === 0)
    {
        return;
    }

    // event in the page
    let script = '(function() {'
        + ' var code = ' + ptt_watched_key_code + ';'
        + (type === 'keydown' ? ' var field = document.activeElement;'
            + ' if (field && field !== document.body && typeof field.blur === "function") {'
            + ' window.lemon_electron_talk_field = field; field.blur(); }' : '')
        + ' var event = new KeyboardEvent("' + type + '", {bubbles: true, cancelable: true});'
        + ' Object.defineProperty(event, "which", {get: function() { return code; }});'
        + ' Object.defineProperty(event, "keyCode", {get: function() { return code; }});'
        + ' document.dispatchEvent(event);'
        + (type === 'keyup' ? ' var previous = window.lemon_electron_talk_field; window.lemon_electron_talk_field = null;'
            + ' if (previous && document.contains(previous) && typeof previous.focus === "function") {'
            + ' previous.focus({preventScroll: true}); }' : '')
        + ' })();';
    main_window.webContents.executeJavaScript(script, false).catch(function() {});
}

/**
 * @brief tells the helper which key to report
 *
 * @param number key_code -> the talk key as a JavaScript keyCode, 0 for none
 *
 * @return void
 */
function electron__watch_key(key_code)
{
    // changed key only
    if (key_code === ptt_watched_key_code)
    {
        return;
    }
    ptt_watched_key_code = key_code;

    // command to the helper
    if (ptt_helper != null && ptt_helper.stdin.writable)
    {
        ptt_helper.stdin.write('watch ' + key_code + '\n');
    }
}

/**
 * @brief reads the talk key from the stored settings of the page and passes it to the helper. It runs when the
 *        window loses the keyboard focus. The variables of the page cannot be reached from here, because the page
 *        script is one function. So the stored value lemon_ptt_key_code is read, and without it the default Q (81).
 *
 * @return void
 */
async function electron__update_watched_key()
{
    // key from the page
    let key_code = 0;
    try
    {
        key_code = await main_window.webContents.executeJavaScript('(function() {'
            + ' try { var code = parseInt(localStorage.getItem("lemon_ptt_key_code"), 10); return code > 0 ? code : 81; }'
            + ' catch (error) { return 81; }'
            + ' })()', false);
    }
    catch (error)
    {
        key_code = 0;
    }

    // tell the helper
    electron__watch_key(Number.isInteger(key_code) ? key_code : 0);
}

/**
 * @brief handles one line from the helper. Presses and releases are passed to the page only while the window is
 *        not focused, because a focused page receives the real key itself. With the Wayland shortcut portal the
 *        desktop may keep the key away from every window, so these events are always passed on.
 *
 * @param string line -> one line of the helper's output
 *
 * @return void
 */
function electron__on_ptt_helper_line(line)
{
    // key events
    if (line === 'down' || line === 'up')
    {
        let is_window_focused = main_window != null && !main_window.isDestroyed() && main_window.isFocused();
        if (!is_window_focused || ptt_helper_method === 'portal')
        {
            electron__send_key_to_page(line === 'down' ? 'keydown' : 'keyup');
        }
        return;
    }

    // method and problems
    if (line.startsWith('method '))
    {
        ptt_helper_method = line.slice(7).trim();
    }
    console.log('push-to-talk helper reports ' + line);
}

/**
 * @brief starts the push-to-talk helper that the packaging placed next to the app resources
 *
 * @return void
 */
function electron__start_ptt_helper()
{
    // helper file
    let file = path.join(process.resourcesPath, process.platform === 'win32' ? 'ptt-helper.exe' : 'ptt-helper');
    if (!fs.existsSync(file))
    {
        console.log('push-to-talk outside the window is not available because ' + file + ' is missing');
        return;
    }

    // start and read lines
    ptt_helper = child_process.spawn(file, [], { stdio: ['pipe', 'pipe', 'inherit'], windowsHide: true });
    ptt_helper.on('error', function(error)
    {
        console.log('push-to-talk helper could not start (' + error.message + ')');
        ptt_helper = null;
    });
    ptt_helper.on('exit', function()
    {
        ptt_helper = null;
    });
    ptt_helper.stdin.on('error', function() {});
    readline.createInterface({ input: ptt_helper.stdout }).on('line', electron__on_ptt_helper_line);

    // known key
    if (ptt_watched_key_code !== 0)
    {
        ptt_helper.stdin.write('watch ' + ptt_watched_key_code + '\n');
    }
}

/**
 * @brief lets the user choose a screen or window when the page starts screen sharing. On Windows the sound of the
 *        computer is shared as well when the page asks for it.
 *
 * @param object request -> the request from the page
 * @param function callback -> receives the chosen source, or an empty object to cancel
 *
 * @return void
 */
async function electron__choose_display_source(request, callback)
{
    // screens and windows
    let sources = [];
    try
    {
        sources = await desktopCapturer.getSources({ types: ['screen', 'window'], thumbnailSize: { width: 0, height: 0 } });
    }
    catch (error)
    {
        sources = [];
    }
    sources = sources.slice(0, DISPLAY_SOURCE_LIMIT);
    if (sources.length === 0)
    {
        callback({});
        return;
    }

    // ask which one
    let buttons = sources.map(source => source.name).concat(['Cancel']);
    let result = await dialog.showMessageBox(main_window,
    {
        type: 'question',
        title: 'Share',
        message: 'What do you want to share?',
        buttons: buttons,
        defaultId: 0,
        cancelId: buttons.length - 1,
        noLink: true
    });
    if (result.response >= sources.length)
    {
        callback({});
        return;
    }

    // chosen source
    let answer = { video: sources[result.response] };
    if (request.audioRequested && process.platform === 'win32')
    {
        answer.audio = 'loopback';
    }
    callback(answer);
}

/**
 * @brief shows the right-click menu of a text field that a browser shows: cut, copy, paste and select all. Electron
 *        shows no right-click menu by itself. Paste also pastes a picture from the clipboard, for example a
 *        screenshot, in the same way as Ctrl+V. The page lets this event through only in text fields
 *
 * @param object params -> the parameters of the context-menu event of Electron
 *
 * @return void
 */
function electron__show_context_menu(params)
{
    // text field only
    if (!params.isEditable || main_window == null || main_window.isDestroyed())
    {
        return;
    }

    // menu at the pointer
    Menu.buildFromTemplate([
        { role: 'cut', enabled: params.editFlags.canCut },
        { role: 'copy', enabled: params.editFlags.canCopy },
        { role: 'paste' },
        { type: 'separator' },
        { role: 'selectAll' }
    ]).popup({ window: main_window });
}

/**
 * @brief opens the client window with client.html. Timers and audio keep running while the window is hidden or
 *        in the background, which a voice chat needs.
 *
 * @return void
 */
function electron__create_window()
{
    // window and page settings
    main_window = new BrowserWindow(
    {
        width: 1280,
        height: 860,
        minWidth: 360,
        minHeight: 480,
        title: 'Lemon Chat',
        backgroundColor: '#101418',
        icon: path.join(__dirname, 'icon.png'),
        autoHideMenuBar: true,
        webPreferences:
        {
            contextIsolation: true,
            nodeIntegration: false,
            sandbox: true,
            spellcheck: false,
            backgroundThrottling: false,
            autoplayPolicy: 'no-user-gesture-required'
        }
    });

    // links in the browser
    main_window.webContents.setWindowOpenHandler(function(details)
    {
        electron__open_external(details.url);
        return { action: 'deny' };
    });
    main_window.webContents.on('will-navigate', function(event, address)
    {
        if (!electron__is_page_address(address))
        {
            event.preventDefault();
            electron__open_external(address);
        }
    });

    // right-click menu
    main_window.webContents.on('context-menu', function(event, params)
    {
        electron__show_context_menu(params);
    });

    // last used server
    main_window.webContents.on('did-finish-load', function()
    {
        main_window.webContents.executeJavaScript(LAST_SERVER_SCRIPT, false).catch(function() {});
    });

    // talk key outside
    main_window.on('blur', electron__update_watched_key);

    // settings to disk
    main_window.on('blur', function()
    {
        session.defaultSession.flushStorageData();
    });
    main_window.on('closed', function()
    {
        main_window = null;
    });
    main_window.loadFile(PAGE_FILE);
}

// one running copy
if (!app.requestSingleInstanceLock())
{
    app.quit();
}

app.on('second-instance', function()
{
    if (main_window != null)
    {
        if (main_window.isMinimized())
        {
            main_window.restore();
        }
        main_window.focus();
    }
});

app.whenReady().then(function()
{
    // menu shown with Alt
    Menu.setApplicationMenu(Menu.buildFromTemplate([
        { label: 'File', submenu: [{ role: 'quit' }] },
        { label: 'Edit', submenu: [{ role: 'undo' }, { role: 'redo' }, { type: 'separator' }, { role: 'cut' }, { role: 'copy' }, { role: 'paste' }, { role: 'selectAll' }] },
        {
            label: 'View',
            submenu: [{ role: 'reload' }, { role: 'forceReload' }, { role: 'toggleDevTools' }, { type: 'separator' },
                { role: 'resetZoom' }, { role: 'zoomIn' }, { role: 'zoomOut' }, { type: 'separator' }, { role: 'togglefullscreen' }]
        }
    ]));

    // page permissions
    session.defaultSession.setPermissionRequestHandler(function(contents, permission, callback)
    {
        callback(ALLOWED_PERMISSIONS.has(permission));
    });
    session.defaultSession.setPermissionCheckHandler(function(contents, permission)
    {
        return ALLOWED_PERMISSIONS.has(permission);
    });
    session.defaultSession.setDisplayMediaRequestHandler(electron__choose_display_source, { useSystemPicker: true });

    // window and helper
    electron__create_window();
    electron__start_ptt_helper();
    app.on('activate', function()
    {
        if (BrowserWindow.getAllWindows().length === 0)
        {
            electron__create_window();
        }
    });
});

app.on('window-all-closed', function()
{
    if (process.platform !== 'darwin')
    {
        app.quit();
    }
});

app.on('before-quit', function()
{
    session.defaultSession.flushStorageData();
});

app.on('will-quit', function()
{
    if (ptt_helper != null)
    {
        ptt_helper.kill();
    }
});
