#include "ui/ui_layout.h"
#include "ui/ui_audio.h"
#include "ui/ui_headers.h"
#include "widgets/widget_select.h"
#include "platform/hotkey.h"

static desktop_session_t *g_audio_session;
static desktop_settings_t *g_audio_settings;
static boole g_audio_microphone;
static boole g_audio_open;
static boole g_audio_native_devices;
static desktop_devices_view_t g_audio_devices;
static char g_audio_input[DESKTOP_DEVICE_KEY];
static char g_audio_output[DESKTOP_DEVICE_KEY];

// declarations
static void _ui_audio_internal__text(const char *id, const char *text);
static void _ui_audio_internal__panel(const char *id, boole visible);
static void _ui_audio_internal__audio_values(gui_event_t *event);
static void _ui_audio_internal__show_audio(gui_event_t *event);
static void _ui_audio_internal__close_audio(gui_event_t *event);
static void _ui_audio_internal__save_audio(gui_event_t *event);
static void _ui_audio_internal__microphone(gui_event_t *event);
static void _ui_audio_internal__enable_audio(gui_event_t *event);
static void _ui_audio_internal__device_list(const char *id, boole capture, const char *selected);
static void _ui_audio_internal__choose_device(gui_event_t *event);
static void _ui_audio_internal__refresh_devices(gui_event_t *event);
static boole _ui_audio_internal__key_list(gui_node_t *parent);

/**
 * @brief fills the key picker from the same table used by all platform adapters.
 * @param gui_node_t* parent -> GUI-owned select; existing options are retained
 * @return boole -> TRUE when every supported key has an option
 */
static boole _ui_audio_internal__key_list(gui_node_t *parent)
{
    gui_node_t *node = NULL_POINTER;
    const char *name = NULL_POINTER;
    int64 index = 0;

    // add the key options
    for (index = parent->child_count; index < desktop_hotkey__count(); index++)
    {
        node = scene__node_new(GUI_NODE_TYPE__OPTION);
        if (!node)
        {
            return FALSE;
        }
        name = desktop_hotkey__name(desktop_hotkey__key(index));
        if (!scene__set_text(node, name, clib__strlen(name)))
        {
            scene__node_free(node);
            return FALSE;
        }
        scene__add_child(parent, node);
    }
    return TRUE;
}

/**
 * @brief rebuilds one bounded picker while preserving its unsaved device key.
 * @param const char* id -> picker control ID
 * @param boole capture -> TRUE for input, FALSE for output
 * @param const char* selected -> unsaved key; empty means system default
 * @return void
 */
static void _ui_audio_internal__device_list(const char *id, boole capture, const char *selected)
{
    gui_node_t *parent = scene__find_by_id((char *)id);
    gui_node_t *node = NULL_POINTER;
    gui_node_t *next = NULL_POINTER;
    int64 index = 0;
    int64 option = 0;
    int64 ordinal = 0;
    int64 length = 0;
    char caption[256] = { 0 };
    boole found = selected[0] == 0;

    // check the device list
    if (!parent)
    {
        return;
    }

    // close the open list
    scene__select_open(parent, FALSE);

    // remove the old options
    node = parent->first_child;
    parent->first_child = NULL;
    parent->last_child = NULL;
    parent->child_count = 0;
    parent->value = 0;

    // free the old options
    while (node)
    {
        next = node->next_sibling;
        node->parent = NULL;
        node->next_sibling = NULL;
        scene__node_free(node);
        node = next;
    }

    // add the default option
    node = scene__node_new(GUI_NODE_TYPE__OPTION);
    if (!node)
    {
        return;
    }
    scene__set_text(node, "System default", clib__strlen("System default"));
    scene__add_child(parent, node);

    // add the device options
    for (index = 0; index < g_audio_devices.count; index++)
    {
        // skip the other direction
        if (g_audio_devices.items[index].capture != capture)
        {
            continue;
        }

        // create an option
        node = scene__node_new(GUI_NODE_TYPE__OPTION);
        if (!node)
        {
            break;
        }
        option++;
        ordinal++;

        // shorten a long name
        length = clib__strlen(g_audio_devices.items[index].name);
        if (length > 220)
        {
            length = 220;
            while (length && ((ubyte)g_audio_devices.items[index].name[length] & 0xc0) == 0x80)
            {
                length--;
            }
        }

        // set the option text
        clib__null_memory(caption, sizeof(caption));
        clib__string_format(caption, sizeof(caption), "%lld. %.*s%s", ordinal, (int)length, g_audio_devices.items[index].name, g_audio_devices.items[index].default_device ? " (default)" : "");
        scene__set_text(node, caption, clib__strlen(caption));
        clib__null_memory(caption, sizeof(caption));
        node->user_data = (void *)(nint)(index + 1);
        scene__add_child(parent, node);

        // select the saved device
        if (clib__strcmp(selected, g_audio_devices.items[index].key) == 0)
        {
            parent->value = (float)option;
            found = TRUE;
        }
    }

    // add the saved device
    if (!found)
    {
        node = scene__node_new(GUI_NODE_TYPE__OPTION);
        if (node)
        {
            scene__set_text(node, g_audio_devices.scanning ? "Saved device (checking...)" : "Saved device (unavailable)", clib__strlen(g_audio_devices.scanning ? "Saved device (checking...)" : "Saved device (unavailable)"));
            node->user_data = (void *)(nint)-1;
            scene__add_child(parent, node);
            parent->value = (float)(option + 1);
        }
    }
}

/**
 * @brief records a draft selection; this handler never opens or switches an audio device.
 * @param gui_event_t* event -> select change event
 * @return void
 */
static void _ui_audio_internal__choose_device(gui_event_t *event)
{
    gui_node_t *parent = event ? event->sender : NULL;
    gui_node_t *option = parent ? parent->first_child : NULL;
    int64 index = 0;
    int64 slot = -1;
    char *selected = NULL_POINTER;

    // check the event sender
    if (!parent)
    {
        return;
    }

    // choose input or output
    selected = clib__strcmp(parent->id, "audio-input-device") == 0 ? g_audio_input : g_audio_output;

    // find the selected option
    while (option && index < (int64)parent->value)
    {
        option = option->next_sibling;
        index++;
    }
    if (!option)
    {
        return;
    }

    // store the device key
    slot = (nint)option->user_data;
    if (slot == 0)
    {
        clib__null_memory(selected, DESKTOP_DEVICE_KEY);
    }
    else if (slot > 0 && slot <= g_audio_devices.count)
    {
        clib__string_copy(selected, DESKTOP_DEVICE_KEY, g_audio_devices.items[slot - 1].key);
    }

    // show the result text
    _ui_audio_internal__text("audio-result", "Changes apply when saved.");
}

/**
 * @brief refreshes choices without changing the active input or output.
 * @param gui_event_t* event -> unused button event
 * @return void
 */
static void _ui_audio_internal__refresh_devices(gui_event_t *event)
{
    (void)event;

    // request the device list
    if (g_audio_native_devices)
    {
        desktop_session__devices(g_audio_session, NULL, TRUE);
        desktop_ui_audio__tick();
    }
}

/**
 * @brief polls enumeration revisions while the floating audio panel is open.
 * @return void
 */
void desktop_ui_audio__tick(void)
{
    gui_node_t *refresh = scene__find_by_id("audio-refresh-devices");

    // check panel is open
    if (!g_audio_open)
    {
        return;
    }

    // update the device lists
    if (g_audio_native_devices && desktop_session__devices(g_audio_session, &g_audio_devices, FALSE))
    {
        _ui_audio_internal__device_list("audio-input-device", TRUE, g_audio_input);
        _ui_audio_internal__device_list("audio-output-device", FALSE, g_audio_output);
        _ui_audio_internal__text("audio-devices-result", g_audio_devices.detail);
    }

    // update the refresh button
    if (refresh && (g_audio_devices.scanning || !g_audio_native_devices))
    {
        refresh->state = GUI_NODE_STATE__DISABLED;
    }
    else if (refresh && refresh->state == GUI_NODE_STATE__DISABLED)
    {
        refresh->state = GUI_NODE_STATE__DEFAULT;
    }
}

/**
 * @brief copies control text and redraws only when its visible value changes.
 * @param const char* id -> control ID
 * @param const char* text -> copied plain text
 * @return void
 */
static void _ui_audio_internal__text(const char *id, const char *text)
{
    gui_node_t *node = scene__find_by_id((char *)id);

    // set a changed text
    if (node && clib__strcmp(node->text, text) != 0)
    {
        scene__set_text(node, text, clib__strlen(text));
#ifdef _WIN32
        platform_win32__request_render();
#endif
    }
}

/**
 * @brief shows the audio panel without covering the surrounding chat.
 * @param const char* id -> panel control ID
 * @param boole visible -> TRUE to show, FALSE to hide
 * @return void
 */
static void _ui_audio_internal__panel(const char *id, boole visible)
{
    gui_node_t *panel = scene__find_by_id((char *)id);

    // change the visibility
    if (panel)
    {
        clib__string_copy(panel->klass, sizeof(panel->klass), visible ? "column panel" : "column panel hidden");
        if (visible)
        {
            desktop_ui_layout__raise(panel);
        }
#ifdef _WIN32
        platform_win32__request_render();
#endif
    }
}

/**
 * @brief updates audio status from a copied session snapshot, never from device callbacks.
 * @param const desktop_session_view_t* view -> borrowed GUI-owned session snapshot
 * @return void
 */
void desktop_ui_audio__update(const desktop_session_view_t *view)
{
    gui_node_t *meter = scene__find_by_id("input-meter-fill");
    char text[128] = { 0 };
    const char *level = text;
    boole ready = g_audio_microphone && view->microphone_ready;

    // show the audio status
    _ui_audio_internal__text("audio-status", view->connected ? view->audio_detail : "Not connected");

    // format the microphone level
    clib__string_format(text, sizeof(text), "Microphone: %lld dB%s", view->input_level, view->voice_detected ? " - speech detected" : "");

    // choose a hint instead
    if (!view->connected)
    {
        level = "Connect to a server to check your microphone.";
    }
    else if (!g_audio_microphone)
    {
        level = "Enable your microphone to check its level.";
    }
    else if (!g_audio_settings->audio_enabled)
    {
        level = "Select Enable audio at the bottom of the window.";
    }
    else if (!ready)
    {
        level = "Waiting for the microphone to be ready.";
    }
    else if (!g_audio_settings->voice_activation && !view->talking)
    {
        clib__string_format(text, sizeof(text), "Hold %s to check your microphone level.", desktop_hotkey__name(g_audio_settings->push_to_talk_key));
    }

    // show the microphone level
    _ui_audio_internal__text("input-level", level);
    if (meter)
    {
        meter->value = ready ? (float)(view->input_level + 60) / 60.0f : 0;
    }
}

/**
 * @brief registers audio controls before parsing the layout; the microphone uses the saved transmission mode.
 * @param desktop_session_t* session -> GUI-owned session
 * @param desktop_settings_t* settings -> shared GUI-owned saved settings
 * @param boole native_devices -> FALSE for a hidden preview that must not enumerate hardware
 * @return void
 */
void desktop_ui_audio__initialize(desktop_session_t *session, desktop_settings_t *settings, boole native_devices)
{
    // store the arguments
    g_audio_session = session;
    g_audio_settings = settings;
    g_audio_microphone = TRUE;
    g_audio_native_devices = native_devices;

    // register the handlers
    scene__register_handler("on_audio_values", _ui_audio_internal__audio_values);
    scene__register_handler("on_show_audio", _ui_audio_internal__show_audio);
    scene__register_handler("on_close_audio", _ui_audio_internal__close_audio);
    scene__register_handler("on_save_audio", _ui_audio_internal__save_audio);
    scene__register_handler("on_microphone", _ui_audio_internal__microphone);
    scene__register_handler("on_enable_audio", _ui_audio_internal__enable_audio);
    scene__register_handler("on_audio_device", _ui_audio_internal__choose_device);
    scene__register_handler("on_audio_refresh_devices", _ui_audio_internal__refresh_devices);
}

/**
 * @brief updates audio preferences without changing the microphone control when speech starts.
 * @return void
 */
void desktop_ui_audio__apply(void)
{
    desktop_audio_options_t options = { 0 };
    gui_node_t *warning = scene__find_by_id("audio-disabled-warning");

    // copy the audio options
    options.enabled = g_audio_settings->audio_enabled;
    options.microphone = g_audio_microphone;
    options.volume = g_audio_settings->audio_volume;
    options.gain = g_audio_settings->microphone_gain;
    options.voice_activation = g_audio_settings->voice_activation;
    options.activation_threshold = g_audio_settings->activation_threshold;
    options.voice_bitrate = g_audio_settings->voice_bitrate;
    clib__string_copy(options.input_device, sizeof(options.input_device), g_audio_settings->input_device);
    clib__string_copy(options.output_device, sizeof(options.output_device), g_audio_settings->output_device);

    // apply the audio options
    desktop_session__audio(g_audio_session, &options, g_audio_settings->push_to_talk_key);
    desktop_session__notifications(g_audio_session, g_audio_settings->sound_effects && g_audio_settings->audio_enabled, g_audio_settings->audio_volume, g_audio_settings->output_device);

    // update the microphone button
    _ui_audio_internal__text("microphone-button", g_audio_microphone ? "Mic enabled" : "Mic off");

    // show or hide warning
    if (warning)
    {
        clib__string_copy(warning->klass, sizeof(warning->klass), g_audio_settings->audio_enabled ? "row hidden" : "row");
#ifdef _WIN32
        platform_win32__request_render();
#endif
    }
}

/**
 * @brief updates labels for unsaved audio slider values.
 * @param gui_event_t* event -> unused GUI change event
 * @return void
 */
static void _ui_audio_internal__audio_values(gui_event_t *event)
{
    gui_node_t *volume = scene__find_by_id("audio-volume");
    gui_node_t *gain = scene__find_by_id("microphone-gain");
    gui_node_t *key = scene__find_by_id("ptt-key");
    gui_node_t *activation = scene__find_by_id("voice-activation");
    gui_node_t *threshold = scene__find_by_id("activation-threshold");
    char text[64] = { 0 };
    gui_node_t *bitrate = scene__find_by_id("voice-bitrate");
    (void)event;

    // check the controls
    if (!volume || !gain || !key || !activation)
    {
        return;
    }

    // show volume and gain
    clib__string_format(text, sizeof(text), "Listening volume: %d%%", (int)volume->value);
    _ui_audio_internal__text("volume-label", text);
    clib__string_format(text, sizeof(text), "Microphone gain: %d%%", (int)gain->value);
    _ui_audio_internal__text("gain-label", text);

    // set the key state
    key->state = activation->value != 0 || key->child_count != desktop_hotkey__count() ? GUI_NODE_STATE__DISABLED : GUI_NODE_STATE__DEFAULT;

    // show the voice threshold
    if (threshold)
    {
        clib__string_format(text, sizeof(text), "Minimum voice level: %lld dB", (int64)threshold->value);
        _ui_audio_internal__text("activation-label", text);
    }

    // show the voice bitrate
    if (bitrate)
    {
        clib__string_format(text, sizeof(text), "Voice bitrate: %lld kbit/s", (int64)bitrate->value);
        _ui_audio_internal__text("bitrate-label", text);
    }
}

/**
 * @brief opens audio settings with the currently applied values.
 * @param gui_event_t* event -> unused GUI click event
 * @return void
 */
static void _ui_audio_internal__show_audio(gui_event_t *event)
{
    gui_node_t *volume = scene__find_by_id("audio-volume");
    gui_node_t *gain = scene__find_by_id("microphone-gain");
    gui_node_t *key = scene__find_by_id("ptt-key");
    gui_node_t *activation = scene__find_by_id("voice-activation");
    gui_node_t *threshold = scene__find_by_id("activation-threshold");
    gui_node_t *bitrate = scene__find_by_id("voice-bitrate");
    gui_node_t *sounds = scene__find_by_id("sound-effects");
    gui_node_t *deafened = scene__find_by_id("audio-deafened");
    (void)event;

    // set volume and key
    if (volume && gain && key)
    {
        volume->value = (float)g_audio_settings->audio_volume;
        gain->value = (float)g_audio_settings->microphone_gain;
        _ui_audio_internal__key_list(key);
        key->value = (float)desktop_hotkey__index(g_audio_settings->push_to_talk_key);
        if (key->value < 0)
        {
            key->value = (float)desktop_hotkey__index(DESKTOP_HOTKEY_DEFAULT);
        }
    }

    // set the voice activation
    if (activation && threshold)
    {
        activation->value = g_audio_settings->voice_activation;
        threshold->value = (float)g_audio_settings->activation_threshold;
    }

    // set the other controls
    if (sounds)
    {
        sounds->value = g_audio_settings->sound_effects;
    }
    if (bitrate)
    {
        bitrate->value = (float)g_audio_settings->voice_bitrate;
    }
    if (deafened)
    {
        deafened->value = !g_audio_settings->audio_enabled;
    }

    // update the labels
    _ui_audio_internal__audio_values(NULL);

    // copy the saved devices
    clib__string_copy(g_audio_input, sizeof(g_audio_input), g_audio_settings->input_device);
    clib__string_copy(g_audio_output, sizeof(g_audio_output), g_audio_settings->output_device);

    // build the device lists
    g_audio_open = TRUE;
    _ui_audio_internal__device_list("audio-input-device", TRUE, g_audio_input);
    _ui_audio_internal__device_list("audio-output-device", FALSE, g_audio_output);
    _ui_audio_internal__refresh_devices(NULL);

    // report disabled device listing
    if (!g_audio_native_devices)
    {
        _ui_audio_internal__text("audio-devices-result", "Device enumeration is disabled in this preview.");
    }

    // show the audio panel
    _ui_audio_internal__text("audio-result", "Changes apply when saved.");
    _ui_audio_internal__panel("audio-panel", 1);
}

/**
 * @brief closes audio settings without applying slider changes.
 * @param gui_event_t* event -> unused GUI click event
 * @return void
 */
static void _ui_audio_internal__close_audio(gui_event_t *event)
{
    (void)event;
    g_audio_open = FALSE;
    _ui_audio_internal__panel("audio-panel", 0);
}

/**
 * @brief saves slider values before changing live audio settings.
 * @param gui_event_t* event -> unused GUI click event
 * @return void
 */
static void _ui_audio_internal__save_audio(gui_event_t *event)
{
    gui_node_t *volume = scene__find_by_id("audio-volume");
    gui_node_t *gain = scene__find_by_id("microphone-gain");
    gui_node_t *key = scene__find_by_id("ptt-key");
    gui_node_t *activation = scene__find_by_id("voice-activation");
    gui_node_t *threshold = scene__find_by_id("activation-threshold");
    gui_node_t *bitrate = scene__find_by_id("voice-bitrate");
    gui_node_t *sounds = scene__find_by_id("sound-effects");
    gui_node_t *deafened = scene__find_by_id("audio-deafened");
    desktop_settings_t settings = *g_audio_settings;
    (void)event;

    // check the controls
    if (!volume || !gain || !key || !activation || !threshold || !bitrate)
    {
        clib__null_memory(&settings, sizeof(settings));
        return;
    }

    // check the key list
    if (key->child_count != desktop_hotkey__count() || key->value < 0 || key->value >= desktop_hotkey__count())
    {
        _ui_audio_internal__text("audio-result", "Could not load the key choices. Reopen audio settings and try again.");
        clib__null_memory(&settings, sizeof(settings));
        return;
    }

    // read the control values
    settings.audio_enabled = deafened ? deafened->value == 0 : g_audio_settings->audio_enabled;
    settings.sound_effects = sounds ? sounds->value != 0 : g_audio_settings->sound_effects;
    settings.audio_volume = (int64)volume->value;
    settings.microphone_gain = (int64)gain->value;
    settings.push_to_talk_key = desktop_hotkey__key((int64)key->value);
    settings.voice_activation = activation->value != 0;
    settings.activation_threshold = (int64)threshold->value;
    settings.voice_bitrate = (int64)bitrate->value;
    clib__string_copy(settings.input_device, sizeof(settings.input_device), g_audio_input);
    clib__string_copy(settings.output_device, sizeof(settings.output_device), g_audio_output);

    // save the settings
    if (!desktop_settings__save(&settings))
    {
        _ui_audio_internal__text("audio-result", "Could not save audio settings.");
        clib__null_memory(&settings, sizeof(settings));
        return;
    }

    // apply the new settings
    *g_audio_settings = settings;
    clib__null_memory(&settings, sizeof(settings));
    desktop_ui_audio__apply();
    _ui_audio_internal__text("audio-result", "Settings saved");
}

/**
 * @brief explicitly opens or closes microphone capture using the saved activation mode.
 * @param gui_event_t* event -> unused GUI click event
 * @return void
 */
static void _ui_audio_internal__microphone(gui_event_t *event)
{
    (void)event;

    // switch the microphone
    g_audio_microphone = !g_audio_microphone;
    if (!g_audio_microphone)
    {
        desktop_session__sound(g_audio_session, DESKTOP_SOUND_MICROPHONE_MUTED);
    }

    // apply the audio options
    desktop_ui_audio__apply();
}

/**
 * @brief restores audio from the bottom warning without changing the microphone or transmission mode.
 * @param gui_event_t* event -> unused GUI click event
 * @return void
 */
static void _ui_audio_internal__enable_audio(gui_event_t *event)
{
    gui_node_t *deafened = scene__find_by_id("audio-deafened");

    (void)event;

    // enable the audio
    g_audio_settings->audio_enabled = TRUE;
    if (deafened)
    {
        deafened->value = 0;
    }

    // apply the audio options
    desktop_ui_audio__apply();

    // save the settings
    if (!desktop_settings__save(g_audio_settings))
    {
        _ui_audio_internal__text("audio-result", "Audio enabled, but settings could not be saved.");
    }
}
