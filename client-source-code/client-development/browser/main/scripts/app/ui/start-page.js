// shows Connect and Import identity before connection startup

// true from page load until the user pressed the connect button
var start_page_is_active = false;

// the value g_is_autoconnect_without_user_action_active had before the start page set it to false
var start_page_is_autoconnect_wanted = false;

/**
 * @brief checks whether page options require the start page
 *
 * @return boolean true when the page options ask for the connect button and the page does not run in the android app
 */
function start_page__is_wanted()
{
    let config = (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null) ? window.__SERVER_CONFIG__ : {};

    return config.is_connect_button_shown === true && g_is_running_in_android_webview == false;
}

/**
 * @brief shows the start page and pauses automatic connection attempts
 *
 * @return void
 */
function start_page__init()
{
    let config = (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null) ? window.__SERVER_CONFIG__ : {};
    let page = document.getElementById("start-page");
    let connect_button = document.getElementById("start-page-connect-button");
    let import_button = document.getElementById("start-page-import-identity-button");
    let import_dialog = document.getElementById("identity-string-use-container");

    // start page not wanted
    if (page == null || connect_button == null || import_button == null || start_page__is_wanted() == false)
    {
        return;
    }

    // nothing connects while the start page is shown: the connect loop then waits for a connect request
    start_page_is_active = true;
    start_page_is_autoconnect_wanted = g_is_autoconnect_without_user_action_active;
    g_is_autoconnect_without_user_action_active = false;

    // connect the buttons
    connect_button.onclick = start_page__connect_button_onclick;
    import_button.style.display = (config.is_import_identity_button_shown === true) ? "" : "none";
    import_button.onclick = start_page__import_identity_button_onclick;

    // the start page is hidden while the import dialog is open and shown again when it closes
    if (import_dialog != null && typeof MutationObserver === "function")
    {
        new MutationObserver(function()
        {
            if (start_page_is_active == true)
            {
                page.style.display = (import_dialog.style.display == "block") ? "none" : "flex";
            }
        }).observe(import_dialog, { attributes: true, attributeFilter: ["style"] });
    }

    // show start page
    document.body.classList.add("is-start-page-shown");
    page.style.display = "flex";
    connect_button.focus();
}

/**
 * @brief enables sound, then connects or opens the connection form
 *
 * @return void
 */
function start_page__connect_button_onclick()
{
    let page = document.getElementById("start-page");

    // already closed
    if (start_page_is_active == false)
    {
        return;
    }

    // allow sound
    start_page_is_active = false;
    start_page__unlock_sound();

    // hide start page
    document.body.classList.remove("is-start-page-shown");
    page.style.display = "none";

    // connect if wanted
    if (start_page_is_autoconnect_wanted == true)
    {
        // restore the configured autoconnect preference
        g_is_autoconnect_without_user_action_active = true;
        connection__set_connect_holdback_loader_visible(true);
        connection__request_connect("button");
    }
}

/**
 * @brief opens identity import from the start page
 *
 * @return void
 */
function start_page__import_identity_button_onclick()
{
    UI.import_identity_button_onclick();
}

/**
 * @brief resumes existing audio and starts a silent sound during the user's click
 *
 * @return void
 */
function start_page__unlock_sound()
{
    let silent_sound = null;

    // resume audio context
    if (typeof g_audio_context !== "undefined" && g_audio_context != null && g_audio_context.state === "suspended")
    {
        g_audio_context.resume().catch(function(error)
        {
            utils__custom_log("the browser did not allow sound: " + error.message);
        });
    }

    // start silent sound
    if (typeof g_sound_effects !== "undefined" && g_sound_effects != null && g_sound_effects.message_sent)
    {
        silent_sound = g_sound_effects.message_sent.cloneNode();
        silent_sound.volume = 0;
        silent_sound.play().catch(function(error)
        {
            utils__custom_log("the browser did not allow sound effects: " + error.message);
        });
    }
}
