
        //some globals for dev console
        var g_client_list = null;
        var g_channel_list = null
    
        //Android javascript java bridge functions, used in WebView
        var g_send_go_to_idle_mode_request = null;
        var g_send_come_from_idle_mode_request = null;
        var g_activate_continous_audio_broadcast = null;
        var g_set_audio_effects_status = null;
        var g_accept_current_settings_from_android = null;

        function JavascriptJavaBridge__send_go_to_idle_mode_request()
        {
            g_send_go_to_idle_mode_request();
        }

        function JavascriptJavaBridge__send_come_from_idle_mode_request(channelId)
        {
            g_send_come_from_idle_mode_request(channelId);
        }
        
        //set to true if microphone should send audio by default
        function JavascriptJavaBridge__activate_continous_audio_broadcast()
        {
            g_activate_continous_audio_broadcast();
        }

        function JavascriptJavaBridge__set_username_on_connect(username)
        {
            g_set_username_on_connect(username);
        }

        function JavascriptJavaBridge__set_audio_effects_status()
        {
            g_set_audio_effects_status();
        }

        function JavascriptJavaBridge__accept_current_settings_from_android(settings_json_string)
        {
            g_accept_current_settings_from_android(settings_json_string);
        }

