            // ANDROID WEBVIEW BRIDGE - implementations behind the JavascriptJavaBridge__* page
            // globals in android-bridge.js, which java calls into. android only; a desktop or
            // website client wires none of these. included in both bundles because it must live
            // inside moduleFactory's scope, and node reaches it through the export seam.

            var android_js_bridge = {
                send_go_to_idle_mode_request_android: function(is_forced)
                {
                    // enter_deep_idle sends the go-to-idle request to the server itself and additionally
                    // shuts down audio, the opus tick and the webrtc datachannel. gentle (home) keeps an
                    // in-channel session alive; forced (swipe-away) idles from any channel
                    enter_deep_idle(is_forced === true);
                },

                send_come_from_idle_mode_request_android: function(channelId = null)
                {
                    // this function sometimes gets ran twice,
                    // when client accepts the call and a bit later when app runs onResume (because he accepted it)
                    // but server is smart and doesnt let user come back from idle mode twice

                    if (channelId == null)
                    {
                        channelId = 0; // unspecified, so root channel.
                    }

                    // restore audio, opus tick, webrtc and the fast heartbeat before rejoining
                    exit_deep_idle();

                    client_msg.send_come_from_idle_mode_request(channelId);
                },

                set_username_on_connect_android: function(username)
                {
                    if (username.length == 0)
                    {
                        return;
                    }

                    client_msg.send_change_client_username_request(username, local_client_id);

                    let index = get_client_index_in_array_by_client_id(local_client_id);

                    if (index != -1)
                    {
                        g_client_list[index].username = username;
                    }

                    g_local_username = username;
                    { let rename_input = document.getElementById('connected-local-client-input'); if (rename_input != null) { rename_input.setAttribute('value', g_local_username); } }
                },

                // node's phase relayed by java: feed the page's own status machinery (the
                // ticker keeps repainting it) and keep the spinner up while node works
                show_connection_phase_android: function(state, reason)
                {
                    if (g_is_authenticated == true)
                    {
                        return;
                    }

                    g_connection_status.state = state;
                    g_connection_status.reason = (reason != null) ? reason : "";
                    render_connection_status();

                    if (state === "connecting" || state === "connected")
                    {
                        extend_connect_page_holdback();
                    }
                    else
                    {
                        reveal_connect_page();
                    }
                },

                // java says "node is connected (or the app came to front)". the dial decision
                // belongs to request_connect; this only self-heals a lost settings push first
                nudge_loopback_reattach_android: function()
                {
                    if (g_is_authenticated == true)
                    {
                        return;
                    }

                    // no loopback details yet means a settings push got lost - ask again
                    if (g_loopback_port <= 0)
                    {
                        if (typeof Android !== "undefined")
                        {
                            Android.JavaExportRequestCurrentSettingsFromAndroid();
                        }
                        return;
                    }

                    request_connect("resume");
                },

                accept_current_settings_from_android: function(json_current_settings)
                {

                    let settings_from_android = JSON.parse(json_current_settings);

                    // theme FIRST, before anything below can throw: the settings json is built up
                    // incrementally on the java side, so fields like metadata_keys may simply not
                    // exist yet - one missing field used to kill this whole function halfway and
                    // the mode/theme code at the old spot (near the end) never ran
                    g_android_app_mode = (typeof settings_from_android.app_mode === "string") ? settings_from_android.app_mode : "";
                    console.log("android settings received, app_mode = " + g_android_app_mode);

                    apply_theme_for_app_mode();

                    g_are_server_details_predefined = true;
                    is_autoconnect_enabled = true;

                    // there is difference between how web browser app and how android app handles auto connects

                    // in web browser
                    // autoconnect is on -> connect button shows but server details are specified statically in html (useful in some cases)
                    // autoconnect is off -> connect button shows along with the server details promt,
                    // autoconnect is on and g_is_autoconnect_without_user_action_active is set to true -> connect button doesnt show, app goes in loop and tries to join the server until it succeeds

                    // in android
                    // autoconnect is on -> connect button doesnt show, app goes in loop and tries to join the server until it succeeds
                    // autoconnect is off -> connect button shows (but server details need specified in android settings)

                    // these differences exists because web browser has limitation, sometimes it requries user interaction to play soudns and android app doesnt
                    // different enviroments sometimes need different way of doing things

                    // yes, this is confusing, and needs improvements, should be documentated better

                    // == true coercion: these fields may be absent from an incremental first-run
                    // json, and undefined must count as false (undefined == false is FALSE in js,
                    // so the "== false" checks below would silently skip both branches otherwise)
                    // absent means ON: a fresh install's json lacks the field, and reading that as
                    // "off" left node never connecting, with no way to trigger it from the ui
                    g_is_autoconnect_without_user_action_active = (settings_from_android.is_autoconnect_enabled != false);

                    // android decides if the log file is written. the page only copies the answer
                    g_is_file_logging_enabled = (settings_from_android.is_file_logging_enabled != false);

                    // a choice made in the local settings panel outranks the android settings json
                    let has_local_sound_choice = false;
                    try { has_local_sound_choice = (localStorage.getItem("lemon_sound_effects") != null); } catch (e) { }

                    if (has_local_sound_choice == false)
                    {
                        g_are_sound_effects_enabled = (settings_from_android.is_audio_effect_enabled == true);
                    }

                    // assigning the flag is not enough, the mic has to start or stop with it. on the first push
                    // the connect path already applies it, so only a switch moved while running is acted on here.
                    //
                    // the flag is set HERE rather than by calling the click handler, which used to own the flip.
                    // routing state through a click handler meant it could not be applied without a ui, and the
                    // handler's `= !flag` would have inverted it the moment a second writer existed. the mic and
                    // the repaint stay behind UI.*, so a runtime with no ui sets the flag and touches no audio
                    let is_microphone_always_on_wanted = (settings_from_android.is_microphone_always_on == true);

                    if (g_have_received_android_settings == false)
                    {
                        set_microphone_always_on(is_microphone_always_on_wanted);
                    }
                    else if (is_microphone_always_on_wanted != g_is_microphone_always_on)
                    {
                        set_microphone_always_on(is_microphone_always_on_wanted);
                        UI.activate_continous_audio_broadcast_apply();
                    }

                    g_have_received_android_settings = true;

                    // the app log is opt-in from the java settings; hide the open textarea too
                    // when the switch turns off so it does not linger with no button to close it
                    if (settings_from_android.is_app_log_enabled == true)
                    {
                        document.getElementById("show-hide-log-button").style.display = "";
                    }
                    else
                    {
                        document.getElementById("show-hide-log-button").style.display = "none";
                        document.getElementById("textarea-log").style.display = "none";
                    }

                    // android: connection details come from the java settings window, so the in-page
                    // ip/port form is pointless - hide it fully (display:none, not just invisible, so
                    // it leaves no dead gap). the connect-button visibility block in window_onload is
                    // web-only, so drive it here: autoconnect off => the user needs a connect button
                    document.getElementById("connect-form-sub-container-1").style.display = "none";
                    document.getElementById("connect-form-sub-container-2").style.display = "none";
                    document.getElementById("add-key-button").style.display = "none";

                    // bookmarks fill those hidden fields, so they have nothing to act on here
                    document.getElementById("server-bookmarks-container").style.display = "none";

                    if (g_is_autoconnect_without_user_action_active == false)
                    {
                        document.getElementById("another-buttons-sub-container").style.display = "";
                        document.getElementById("another-buttons-sub-loading-container").style.display = "none";
                        // explicit visible is needed (a stylesheet default hides it); the spinner
                        // page has an opaque background now, so it cannot float through anymore
                        document.getElementById("connect-button").style.visibility = "visible";
                        document.getElementById("import-identity-button").style.display = "none";

                        // nothing dials on its own: no spinner, the connect page IS the page
                        reveal_connect_page();
                    }

                    if (g_are_sound_effects_enabled == true)
                    {
                        document.getElementById("sound-effects-button").style.backgroundImage = "url(data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiIHN0YW5kYWxvbmU9Im5vIj8+PCFET0NUWVBFIHN2ZyBQVUJMSUMgIi0vL1czQy8vRFREIFNWRyAxLjEvL0VOIiAiaHR0cDovL3d3dy53My5vcmcvR3JhcGhpY3MvU1ZHLzEuMS9EVEQvc3ZnMTEuZHRkIj48c3ZnIHdpZHRoPSIxODJweCIgaGVpZ2h0PSIxNjBweCIgdmVyc2lvbj0iMS4xIiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHhtbG5zOnhsaW5rPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5L3hsaW5rIiB4bWw6c3BhY2U9InByZXNlcnZlIiB4bWxuczpzZXJpZj0iaHR0cDovL3d3dy5zZXJpZi5jb20vIiBzdHlsZT0iZmlsbC1ydWxlOmV2ZW5vZGQ7Y2xpcC1ydWxlOmV2ZW5vZGQ7c3Ryb2tlLWxpbmVqb2luOnJvdW5kO3N0cm9rZS1taXRlcmxpbWl0OjI7Ij48ZyB0cmFuc2Zvcm09Im1hdHJpeCgwLjI0LDAsMCwwLjI0LC01MTguMDg4MjEyLC01NDIuNDU0NDM4KSI+PGc+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMTIwLjkxNDAwNCwyMTUwLjU1MzYwNSkiPjxwYXRoIGQ9Ik00NDIsMTM0QzQ0MywxMzUgNDQzLDEzNSA0NDksMTQwQzQ1NCwxNDYgNDUzLDE0NiA0NTksMTUyQzQ3MywxNjYgNDcyLDE3MyA0NzUsMTgyQzQ3NiwxODggNDg1LDIyMyA0NjgsMjU4QzQ2NSwyNjMgNDQ2LDMwMiA0MTcsMzE3QzQxMSwzMjAgMzcwLDMxOCAzNDAsMzMyQzI5NywzNTQgMzAxLDM2MCAyNjIsMzg5QzI1NiwzOTMgMjU1LDM5MyAyNDcsMzg2QzI0MSwzNzkgMjEzLDM2OCAyMDcsMzY4QzIwMSwzNjcgMjAyLDM2OCAxOTUsMzY5QzE0MCwzNzUgMTU2LDQ4NiAyMDEsNDkyQzIyNSw0OTUgMjIyLDUwNiAyMjEsNTQ1QzIyMSw1NDggMjIwLDU0NyAyMjAsNTUwQzIxOSw1NzUgMjE3LDU3NCAyMTYsNTk5QzIxNiw2MDEgMjE1LDYwMiAyMTUsNjA1QzIxMiw2NzkgMjI3LDcxNCAyNTAsNzQ5QzI1Nyw3NTkgMjM5LDc1OCAyMzYsNzU3QzIyNyw3NTMgMTkzLDc1OCAxNDEsNzQwQzEzMiw3MzcgMTMyLDczNiAxMjQsNzMzQzg1LDcyMCA2MSw2ODUgNTcsNjc3QzQyLDY0NiA0Niw2MjcgNDUsNjIzQzQxLDYxMSA0NCw2MDcgNDMsNjAyQzM5LDU5MSA0MSw1MzcgMzgsNTI1QzM4LDUyNSAzOCw0NzkgMzgsNDc5QzQzLDQ1NiAzNyw0MjkgNDAsNDI0QzQ1LDQxNCAzMCwyNTYgMTQ5LDE2MkMxNTMsMTU5IDE5MSwxMzYgMTk5LDEzNEMyMDMsMTMyIDI0NiwxMTggMjQ5LDExOEMyNjgsMTE3IDI2NywxMTMgMjg5LDExM0MyOTEsMTEzIDI5MCwxMTEgMjkyLDExMUMzMTEsMTA5IDM3MSwxMDkgMzc1LDExMUMzODEsMTE0IDM4NSwxMTIgMzg3LDExM0MzOTQsMTE3IDM5NSwxMTUgMzk1LDExNUM0MDIsMTE4IDQwMiwxMTYgNDAyLDExN0M0MTEsMTIxIDQzOCwxMjQgNDQyLDEzNFoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIxMjAuOTE0MDA0LDIxNTAuNTUzNjA1KSI+PHBhdGggZD0iTTQ0NSwxMzJDNDQ3LDEzMCA0NDcsMTI5IDQ0OSwxMzFDNDU0LDEzMyA0NTUsMTMxIDQ1OCwxMzJDNDY4LDEzNyA1ODIsMTU0IDU4MiwyNjNDNTgyLDI2NSA1ODAsMjY1IDU4MCwyNjdDNTc5LDMyMSA1NTQsMzIxIDU0NywzMDVDNTQyLDI5NiA1NTgsMjYxIDU0NywyMjhDNTM2LDE5NiA0OTksMTc1IDUwOCwxODRDNTE2LDE5MyA1MTUsMTk0IDUyMiwyMDRDNTM2LDIyNSA1NDIsMjYxIDU0MiwyNjNDNTQyLDI4OSA1NDEsMjg4IDU0MCwzMDZDNTQwLDMwOSA1MzksMzA4IDUzOSwzMTFDNTM4LDMxNiA1MzgsMzE1IDUzNywzMjFDNTM2LDMyOCA1MzUsMzI3IDUzMywzMzRDNTMzLDMzOSA1MjksMzUwIDUyOCwzNTNDNTI2LDM2NCA1MjEsMzY1IDUyNywzNzVDNTM3LDM4OSA2MDUsNDMyIDU0OCw0NjJDNTQxLDQ2NSA1NDIsNDY2IDU0NSw0NzNDNTU0LDQ5MSA1MzUsNDk1IDUzNyw0OTlDNTUyLDUxOCA1MzAsNTI0IDUzMiw1MzJDNTM1LDU0MyA1MzMsNTYyIDUzMSw1NjRDNTIzLDU4MCA1MjEsNTg1IDUwNCw1OTNDNDk1LDU5OCA0NjMsNTkxIDQ2Myw1OTFDNDQyLDU4MSA0MjcsNTkwIDQyNCw1OTJDNDEyLDU5OSA0MTUsNjAyIDM5MSw2NzVDMzgyLDcwNSAzODMsNzA2IDM4Miw3MDhDMzc5LDcxNSAzODIsNzE3IDM4Myw3MTlDMzgzLDczMiA0MDEsNzQ3IDQwNSw3NTJDNDE3LDc2NCA0MDAsNzczIDM5OSw3NzRDMzkzLDc3NyAyOTcsNzYzIDI5Myw3NjJDMjkwLDc2MiAyNTUsNzA3IDI1NSw3MDVDMjU0LDcwNSAyNTQsNzAyIDI1MSw2OTRDMjI5LDY0MSAyNDcsNTY4IDI0Nyw1MjBDMjQ3LDUwNCAyNDYsNTA1IDI0Miw0ODlDMjQxLDQ4MiAyMjcsNDcyIDIyNSw0NzJDMjEzLDQ2OSAyMTQsNDY4IDIxMiw0NjhDMTg3LDQ2NiAxODIsNDI1IDE4Nyw0MTBDMTk5LDM3MiAyMzQsNDEwIDIzOCw0MTJDMjU4LDQyMyAyNjYsNDE2IDI3Myw0MTNDMjkxLDQwNCAzMzgsMzQ1IDM5MywzNDVDNDMxLDM0NSA0NDEsMzMyIDQ1NiwzMjBDNDU5LDMxNyA0NTksMzE3IDQ2MiwzMTRDNDY0LDMxMiA0NzYsMjk5IDQ4MywyODZDNTA0LDI0MyA0OTgsMjM5IDUwMSwyMzRDNTA0LDIyNiA1MDIsMjIzIDUwMiwyMjFDNTA2LDIxMiA1MDEsMTk0IDUwNSwxODNDNTA3LDE3OCA0NDcsMTM3IDQ0NSwxMzJaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMDY4LjI4ODM4MywyMTE1LjcxMDQ3OSkiPjxwYXRoIGQ9Ik04NDMsNDcwQzg0NCw0ODMgODQ1LDQ4MiA4NDUsNDk0Qzg0NSw1MjcgODQ1LDUyNyA4NDIsNTQ2QzgzMyw1OTAgODMxLDU4OSA4MTIsNjI5QzgxMiw2MzAgNzk5LDY0OSA3OTgsNjUwQzc5OCw2NTEgNzc4LDY3NyA3NzAsNjgyQzc2MSw2ODkgNzM0LDY3NyA3NTYsNjU1QzgwNSw2MDYgODQ0LDUwMiA3OTAsNDAxQzc2MSwzNDYgNzMzLDM0NiA3NTUsMzI3Qzc2NywzMTYgNzgxLDMzOCA3OTIsMzUwQzgwMSwzNTkgODM0LDQxNSA4MzcsNDM4QzgzNyw0NDIgODM5LDQ0MiA4NDAsNDUzQzg0MCw0NTQgODQzLDQ2MiA4NDMsNDcwWiIgc3R5bGU9ImZpbGw6d2hpdGU7Ii8+PC9nPjxnIHRyYW5zZm9ybT0ibWF0cml4KDEsMCwwLDEsMjA2OC4yODgzODMsMjExNS43MTA0NzkpIj48cGF0aCBkPSJNNzI2LDQ3MUM3MjUsNDY4IDcyMCw0NTIgNzE4LDQ1MEM2OTcsNDA3IDY3NCw0MDYgNjk0LDM4OEM3MDIsMzgxIDcxNSwzODkgNzE4LDM5NkM3MjEsNDAyIDczMiw0MDkgNzQ0LDQzNkM3NjMsNDc0IDc2MCw1MTMgNzYwLDUxM0M3NTgsNTE5IDc1Nyw1MzUgNzU3LDUzOEM3NTIsNTYyIDczMiw1OTcgNzI1LDYwNEM3MTUsNjE1IDcxNyw2MTggNzA0LDYyNEM2OTksNjI2IDY4OCw2MTcgNjg4LDYxM0M2ODQsNTkzIDcwNiw1OTQgNzIxLDU1M0M3MjUsNTQwIDcyNSw1NDAgNzI4LDUyN0M3MzMsNDk4IDcyNiw0NzggNzI2LDQ3MVoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIwNjguMjg4MzgzLDIxMTUuNzEwNDc5KSI+PHBhdGggZD0iTTY1NSw1MTZDNjU1LDUxNCA2NTcsNTA2IDY1NCw0OTFDNjUwLDQ2NyA2MTgsNDUyIDY0Niw0MzdDNjU1LDQzMyA2NzEsNDU0IDY3NSw0NjJDNzA4LDUyNSA2NTcsNTgzIDY0Myw1NjlDNjIwLDU0OCA2NTEsNTQ3IDY1NSw1MTZaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PC9nPjxnIHRyYW5zZm9ybT0ibWF0cml4KDEsMCwwLDEsMjA2OC4yODgzODMsMjExNS43MTA0NzkpIj48cGF0aCBkPSJNODQzLDQ3MEM4NDQsNDgzIDg0NSw0ODIgODQ1LDQ5NEM4NDUsNTI3IDg0NSw1MjcgODQyLDU0NkM4MzMsNTkwIDgzMSw1ODkgODEyLDYyOUM4MTIsNjMwIDc5OSw2NDkgNzk4LDY1MEM3OTgsNjUxIDc3OCw2NzcgNzcwLDY4MkM3NjEsNjg5IDczNCw2NzcgNzU2LDY1NUM4MDUsNjA2IDg0NCw1MDIgNzkwLDQwMUM3NjEsMzQ2IDczMywzNDYgNzU1LDMyN0M3NjcsMzE2IDc4MSwzMzggNzkyLDM1MEM4MDEsMzU5IDgzNCw0MTUgODM3LDQzOEM4MzcsNDQyIDgzOSw0NDIgODQwLDQ1M0M4NDAsNDU0IDg0Myw0NjIgODQzLDQ3MFoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIwNjguMjg4MzgzLDIxMTUuNzEwNDc5KSI+PHBhdGggZD0iTTcyNiw0NzFDNzI1LDQ2OCA3MjAsNDUyIDcxOCw0NTBDNjk3LDQwNyA2NzQsNDA2IDY5NCwzODhDNzAyLDM4MSA3MTUsMzg5IDcxOCwzOTZDNzIxLDQwMiA3MzIsNDA5IDc0NCw0MzZDNzYzLDQ3NCA3NjAsNTEzIDc2MCw1MTNDNzU4LDUxOSA3NTcsNTM1IDc1Nyw1MzhDNzUyLDU2MiA3MzIsNTk3IDcyNSw2MDRDNzE1LDYxNSA3MTcsNjE4IDcwNCw2MjRDNjk5LDYyNiA2ODgsNjE3IDY4OCw2MTNDNjg0LDU5MyA3MDYsNTk0IDcyMSw1NTNDNzI1LDU0MCA3MjUsNTQwIDcyOCw1MjdDNzMzLDQ5OCA3MjYsNDc4IDcyNiw0NzFaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMDY4LjI4ODM4MywyMTE1LjcxMDQ3OSkiPjxwYXRoIGQ9Ik02NTUsNTE2QzY1NSw1MTQgNjU3LDUwNiA2NTQsNDkxQzY1MCw0NjcgNjE4LDQ1MiA2NDYsNDM3QzY1NSw0MzMgNjcxLDQ1NCA2NzUsNDYyQzcwOCw1MjUgNjU3LDU4MyA2NDMsNTY5QzYyMCw1NDggNjUxLDU0NyA2NTUsNTE2WiIgc3R5bGU9ImZpbGw6d2hpdGU7Ii8+PC9nPjwvZz48L3N2Zz4=)";
                    }
                    else
                    {
                        document.getElementById("sound-effects-button").style.backgroundImage = "url(data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiIHN0YW5kYWxvbmU9Im5vIj8+PCFET0NUWVBFIHN2ZyBQVUJMSUMgIi0vL1czQy8vRFREIFNWRyAxLjEvL0VOIiAiaHR0cDovL3d3dy53My5vcmcvR3JhcGhpY3MvU1ZHLzEuMS9EVEQvc3ZnMTEuZHRkIj48c3ZnIHdpZHRoPSIxODJweCIgaGVpZ2h0PSIxNjBweCIgdmVyc2lvbj0iMS4xIiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHhtbG5zOnhsaW5rPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5L3hsaW5rIiB4bWw6c3BhY2U9InByZXNlcnZlIiB4bWxuczpzZXJpZj0iaHR0cDovL3d3dy5zZXJpZi5jb20vIiBzdHlsZT0iZmlsbC1ydWxlOmV2ZW5vZGQ7Y2xpcC1ydWxlOmV2ZW5vZGQ7c3Ryb2tlLWxpbmVqb2luOnJvdW5kO3N0cm9rZS1taXRlcmxpbWl0OjI7Ij48ZyB0cmFuc2Zvcm09Im1hdHJpeCgwLjI0LDAsMCwwLjI0LC01MTguMDg4MjEyLC01NDIuNDU0NDM4KSI+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMTIwLjkxNDAwNCwyMTUwLjU1MzYwNSkiPjxwYXRoIGQ9Ik0zMzEsMzM3QzI5NywzNTUgMjk4LDM2MyAyNjIsMzg5QzI1NiwzOTMgMjU1LDM5MyAyNDcsMzg2QzI0MSwzNzkgMjEzLDM2OCAyMDcsMzY4QzIwMSwzNjcgMjAyLDM2OCAxOTUsMzY5QzE0MCwzNzUgMTU2LDQ4NiAyMDEsNDkyQzIyNSw0OTUgMjIyLDUwNiAyMjEsNTQ1QzIyMSw1NDggMjIwLDU0NyAyMjAsNTUwQzIxOSw1NzUgMjE3LDU3NCAyMTYsNTk5QzIxNiw2MDEgMjE1LDYwMiAyMTUsNjA1QzIxMiw2NzkgMjI3LDcxNCAyNTAsNzQ5QzI1Nyw3NTkgMjM5LDc1OCAyMzYsNzU3QzIyNyw3NTMgMTkzLDc1OCAxNDEsNzQwQzEzMiw3MzcgMTMyLDczNiAxMjQsNzMzQzg1LDcyMCA2MSw2ODUgNTcsNjc3QzQyLDY0NiA0Niw2MjcgNDUsNjIzQzQxLDYxMSA0NCw2MDcgNDMsNjAyQzM5LDU5MSA0MSw1MzcgMzgsNTI1QzM4LDUyNSAzOCw0NzkgMzgsNDc5QzQzLDQ1NiAzNyw0MjkgNDAsNDI0QzQ1LDQxNCAzMSwyNjYgMTM5LDE3MUwzMzEsMzM3Wk0xNzksMTQ0QzE4OCwxMzkgMTk1LDEzNSAxOTksMTM0QzIwMywxMzIgMjQ2LDExOCAyNDksMTE4QzI2OCwxMTcgMjY3LDExMyAyODksMTEzQzI5MSwxMTMgMjkwLDExMSAyOTIsMTExQzMxMSwxMDkgMzcxLDEwOSAzNzUsMTExQzM4MSwxMTQgMzg1LDExMiAzODcsMTEzQzM5NCwxMTcgMzk1LDExNSAzOTUsMTE1QzQwMiwxMTggNDAyLDExNiA0MDIsMTE3QzQxMSwxMjEgNDM4LDEyNCA0NDIsMTM0QzQ0MywxMzUgNDQzLDEzNSA0NDksMTQwQzQ1NCwxNDYgNDUzLDE0NiA0NTksMTUyQzQ3MywxNjYgNDcyLDE3MyA0NzUsMTgyQzQ3NiwxODggNDg1LDIyMyA0NjgsMjU4QzQ2NSwyNjMgNDQ2LDMwMiA0MTcsMzE3QzQxNCwzMTggNDAwLDMxOCAzODMsMzIxTDE3OSwxNDRaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMTIwLjkxNDAwNCwyMTUwLjU1MzYwNSkiPjxwYXRoIGQ9Ik00MTAsMzQ0QzQzNCwzNDEgNDQzLDMzMCA0NTYsMzIwQzQ1OSwzMTcgNDU5LDMxNyA0NjIsMzE0QzQ2NCwzMTIgNDc2LDI5OSA0ODMsMjg2QzUwNCwyNDMgNDk4LDIzOSA1MDEsMjM0QzUwNCwyMjYgNTAyLDIyMyA1MDIsMjIxQzUwNiwyMTIgNTAxLDE5NCA1MDUsMTgzQzUwNywxNzggNDQ3LDEzNyA0NDUsMTMyQzQ0NywxMzAgNDQ3LDEyOSA0NDksMTMxQzQ1NCwxMzMgNDU1LDEzMSA0NTgsMTMyQzQ2OCwxMzcgNTgyLDE1NCA1ODIsMjYzQzU4MiwyNjUgNTgwLDI2NSA1ODAsMjY3QzU3OSwzMjEgNTU0LDMyMSA1NDcsMzA1QzU0MiwyOTYgNTU4LDI2MSA1NDcsMjI4QzUzNiwxOTYgNDk5LDE3NSA1MDgsMTg0QzUxNiwxOTMgNTE1LDE5NCA1MjIsMjA0QzUzNiwyMjUgNTQyLDI2MSA1NDIsMjYzQzU0MiwyODkgNTQxLDI4OCA1NDAsMzA2QzU0MCwzMDkgNTM5LDMwOCA1MzksMzExQzUzOCwzMTYgNTM4LDMxNSA1MzcsMzIxQzUzNiwzMjggNTM1LDMyNyA1MzMsMzM0QzUzMywzMzkgNTI5LDM1MCA1MjgsMzUzQzUyNiwzNjQgNTIxLDM2NSA1MjcsMzc1QzUzNywzODkgNjA1LDQzMiA1NDgsNDYyQzU0Nyw0NjIgNTQ3LDQ2MiA1NDYsNDYyTDQxMCwzNDRaTTU0MCw1MTlDNTM2LDUyNCA1MzEsNTI3IDUzMiw1MzJDNTM1LDU0MyA1MzMsNTYyIDUzMSw1NjRDNTIzLDU4MCA1MjEsNTg1IDUwNCw1OTNDNDk1LDU5OCA0NjMsNTkxIDQ2Myw1OTFDNDQyLDU4MSA0MjcsNTkwIDQyNCw1OTJDNDEyLDU5OSA0MTUsNjAyIDM5MSw2NzVDMzgyLDcwNSAzODMsNzA2IDM4Miw3MDhDMzc5LDcxNSAzODIsNzE3IDM4Myw3MTlDMzgzLDczMiA0MDEsNzQ3IDQwNSw3NTJDNDE3LDc2NCA0MDAsNzczIDM5OSw3NzRDMzkzLDc3NyAyOTcsNzYzIDI5Myw3NjJDMjkwLDc2MiAyNTUsNzA3IDI1NSw3MDVDMjU0LDcwNSAyNTQsNzAyIDI1MSw2OTRDMjI5LDY0MSAyNDcsNTY4IDI0Nyw1MjBDMjQ3LDUwNCAyNDYsNTA1IDI0Miw0ODlDMjQxLDQ4MiAyMjcsNDcyIDIyNSw0NzJDMjEzLDQ2OSAyMTQsNDY4IDIxMiw0NjhDMTg3LDQ2NiAxODIsNDI1IDE4Nyw0MTBDMTk5LDM3MiAyMzQsNDEwIDIzOCw0MTJDMjU4LDQyMyAyNjYsNDE2IDI3Myw0MTNDMjg2LDQwNyAzMTUsMzczIDM1MiwzNTZMNTQwLDUxOVoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIwNjguMjg4MzgzLDIxMTUuNzEwNDc5KSI+PHBhdGggZD0iTTg0Myw0NzBDODQ0LDQ4MyA4NDUsNDgyIDg0NSw0OTRDODQ1LDUyNyA4NDUsNTI3IDg0Miw1NDZDODMzLDU5MCA4MzEsNTg5IDgxMiw2MjlDODEyLDYzMCA3OTksNjQ5IDc5OCw2NTBDNzk4LDY1MSA3NzgsNjc3IDc3MCw2ODJDNzYxLDY4OSA3MzQsNjc3IDc1Niw2NTVDODA1LDYwNiA4NDQsNTAyIDc5MCw0MDFDNzYxLDM0NiA3MzMsMzQ2IDc1NSwzMjdDNzY3LDMxNiA3ODEsMzM4IDc5MiwzNTBDODAxLDM1OSA4MzQsNDE1IDgzNyw0MzhDODM3LDQ0MiA4MzksNDQyIDg0MCw0NTNDODQwLDQ1NCA4NDMsNDYyIDg0Myw0NzBaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMDY4LjI4ODM4MywyMTE1LjcxMDQ3OSkiPjxwYXRoIGQ9Ik03MjYsNDcxQzcyNSw0NjggNzIwLDQ1MiA3MTgsNDUwQzY5Nyw0MDcgNjc0LDQwNiA2OTQsMzg4QzcwMiwzODEgNzE1LDM4OSA3MTgsMzk2QzcyMSw0MDIgNzMyLDQwOSA3NDQsNDM2Qzc2Myw0NzQgNzYwLDUxMyA3NjAsNTEzQzc1OCw1MTkgNzU3LDUzNSA3NTcsNTM4Qzc1Miw1NjIgNzMyLDU5NyA3MjUsNjA0QzcxNSw2MTUgNzE3LDYxOCA3MDQsNjI0QzY5OSw2MjYgNjg4LDYxNyA2ODgsNjEzQzY4NCw1OTMgNzA2LDU5NCA3MjEsNTUzQzcyNSw1NDAgNzI1LDU0MCA3MjgsNTI3QzczMyw0OTggNzI2LDQ3OCA3MjYsNDcxWiIgc3R5bGU9ImZpbGw6d2hpdGU7Ii8+PC9nPjxnIHRyYW5zZm9ybT0ibWF0cml4KDEsMCwwLDEsMjA2OC4yODgzODMsMjExNS43MTA0NzkpIj48cGF0aCBkPSJNNjU1LDUxNkM2NTUsNTE0IDY1Nyw1MDYgNjU0LDQ5MUM2NTAsNDY3IDYxOCw0NTIgNjQ2LDQzN0M2NTUsNDMzIDY3MSw0NTQgNjc1LDQ2MkM3MDgsNTI1IDY1Nyw1ODMgNjQzLDU2OUM2MjAsNTQ4IDY1MSw1NDcgNjU1LDUxNloiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgwLjY1ODc0OCwwLjU3MjE0LC0wLjY1NTczMiwwLjc1NDk5NCwyODczLjU3MjI1NywtMTEyMy4zODAwNykiPjxyZWN0IHg9IjE5NTIiIHk9IjMwMDMiIHdpZHRoPSIxMDk4IiBoZWlnaHQ9IjQ3IiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PC9nPjwvc3ZnPg==)";
                    }

                    // kept so a save that did not touch the server details cannot tear down a live connection.
                    // captured before the keys array below is cleared and refilled
                    let previous_autoconnect_host = g_autoconnect_details.host;
                    let previous_autoconnect_port = g_autoconnect_details.port;
                    let previous_autoconnect_keys = (g_autoconnect_details.keys || []).join("\n");
                    // gaining or losing the loopback must count as a change, or a webview that
                    // connected directly before node announced itself never re-routes through it
                    let previous_loopback = g_loopback_port + ":" + g_loopback_token;

                    g_autoconnect_details.host = settings_from_android.host;
                    g_autoconnect_details.port = settings_from_android.port;

                    // null it out in case it wasnt
                    g_autoconnect_details.keys.length = 0;
                    g_autoconnect_details.keys = [];

                    // metadata_keys is optional: the json grows incrementally (identity first,
                    // app_mode on first-run answer) and only a native-settings save writes the
                    // full form - and a server without keys never gets any
                    if (settings_from_android.metadata_keys != null)
                    {
                        for (i = 0; i < settings_from_android.metadata_keys.length; i++)
                        {
                            g_autoconnect_details.keys.push(settings_from_android.metadata_keys[i]);
                        }
                    }

                    // loopback fields only ever appear in the webview's settings json
                    g_loopback_port = (typeof settings_from_android.loopback_port === "number") ? settings_from_android.loopback_port : 0;

                    g_loopback_token = (typeof settings_from_android.loopback_token === "string") ? settings_from_android.loopback_token : "";

                    // no unconditional hold here: request_connect raises the spinner only when
                    // it actually dials - a save with autoconnect off changes nothing on screen

                    // the data worker has its own copy of the flag and encrypts outgoing direct
                    // messages itself - it must skip that in loopback mode too
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

                    // node also derives its identity from the settings; the dial decision itself
                    // is request_connect's, in both runtimes
                    if (is_ui_only_runtime() == false)
                    {
                        request_identity(settings_from_android.identity_string);
                    }

                    request_connect("settings");
                }
            }
